// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(_WIN32)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "cpn/process.hpp"
#include "cpn/text.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cpn {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    int get() const noexcept { return value_; }
    void reset(int value = -1) noexcept {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }
private:
    int value_;
};

struct Pipe { Fd read; Fd write; };

Pipe make_pipe() {
    int fds[2];
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category());
#else
    if (::pipe(fds) != 0) throw std::system_error(errno, std::generic_category());
#endif
    Pipe pipe{Fd(fds[0]), Fd(fds[1])};
    for (Fd* fd : {&pipe.read, &pipe.write}) {
        if (fd->get() < 3) {
            const int copy = ::fcntl(fd->get(), F_DUPFD_CLOEXEC, 3);
            if (copy < 0) throw std::system_error(errno, std::generic_category());
            fd->reset(copy);
        }
        if (::fcntl(fd->get(), F_SETFD, FD_CLOEXEC) < 0)
            throw std::system_error(errno, std::generic_category());
    }
    return pipe;
}

void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category());
}

class SigpipeGuard {
public:
    SigpipeGuard() {
        sigemptyset(&set_); sigaddset(&set_, SIGPIPE);
        const int error = ::pthread_sigmask(SIG_BLOCK, &set_, &old_);
        if (error) throw std::system_error(error, std::generic_category());
        sigset_t pending;
        ::sigpending(&pending);
        was_pending_ = sigismember(&pending, SIGPIPE) == 1;
    }
    ~SigpipeGuard() {
        if (!was_pending_ && sigismember(&old_, SIGPIPE) != 1) {
            sigset_t pending;
            ::sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) {
                int signal = 0;
                ::sigwait(&set_, &signal);
            }
        }
        ::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }
private:
    sigset_t set_{};
    sigset_t old_{};
    bool was_pending_{false};
};

class SpawnActions {
public:
    SpawnActions() { check(::posix_spawn_file_actions_init(&value)); }
    ~SpawnActions() { ::posix_spawn_file_actions_destroy(&value); }
    static void check(int error) { if (error) throw std::system_error(error, std::generic_category()); }
    posix_spawn_file_actions_t value{};
};

class SpawnAttributes {
public:
    SpawnAttributes() { SpawnActions::check(::posix_spawnattr_init(&value)); }
    ~SpawnAttributes() { ::posix_spawnattr_destroy(&value); }
    posix_spawnattr_t value{};
};

class Child {
public:
    explicit Child(pid_t pid) : pid_(pid) {}
    ~Child() {
        if (cleaned_) return;
        ::kill(-pid_, SIGKILL);
        if (!reaped_) {
            ::kill(pid_, SIGKILL);
            int status;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    void reaped() noexcept { reaped_ = true; }
    void cleaned() noexcept { cleaned_ = true; }
private:
    pid_t pid_;
    bool reaped_{false};
    bool cleaned_{false};
};

bool contains_nul(const std::string& value) { return value.find('\0') != std::string::npos; }
bool valid_utf8(const std::string& value) { return !decode_utf8(value).error_offset; }
bool valid_durations(const ProcessLimits& limits) {
    auto latest_start = Time::max();
    const auto representable = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    for (auto duration : {limits.spawn_timeout, limits.wall_timeout, limits.terminate_grace, limits.drain_timeout}) {
        if (duration.count() < 0 || duration > representable) return false;
        const auto delta = std::chrono::duration_cast<Clock::duration>(duration);
        if (latest_start.time_since_epoch() < delta) return false;
        latest_start -= delta;
    }
    return limits.spawn_timeout.count() > 0 && limits.wall_timeout.count() > 0 &&
           limits.drain_timeout.count() > 0 && Clock::now() <= latest_start;
}

std::map<std::string, std::string> environment_for(const ProcessSpec& spec) {
    std::map<std::string, std::string> values;
    if (spec.inherit_environment) {
        for (char** entry = environ; entry && *entry; ++entry) {
            std::string item(*entry);
            const auto equal = item.find('=');
            if (equal != std::string::npos) values[item.substr(0, equal)] = item.substr(equal + 1);
        }
    }
    for (const auto& [key, value] : spec.environment) {
        if (key.empty() || key.find('=') != std::string::npos || contains_nul(key) || contains_nul(value) || !valid_utf8(key) || !valid_utf8(value))
            throw std::system_error(EINVAL, std::generic_category(), "Invalid environment entry");
        values[key] = value;
    }
    return values;
}

std::filesystem::path resolve_executable(const ProcessSpec& spec,
                                        const std::map<std::string, std::string>& environment,
                                        const std::filesystem::path& cwd) {
    const auto program = spec.executable.native();
    if (program.empty() || contains_nul(program) || !valid_utf8(program))
        throw std::system_error(EINVAL, std::generic_category(), "Invalid executable");
    if (program.find('/') != std::string::npos)
        return spec.executable.is_absolute() ? spec.executable : cwd / spec.executable;
    std::string path;
    if (const auto found = environment.find("PATH"); found != environment.end()) path = found->second;
    else {
        const auto size = ::confstr(_CS_PATH, nullptr, 0);
        if (size > 0) {
            std::vector<char> buffer(size);
            ::confstr(_CS_PATH, buffer.data(), buffer.size());
            path = buffer.data();
        }
    }
    int error = ENOENT;
    for (std::size_t begin = 0;;) {
        const auto end = path.find(':', begin);
        auto directory = std::filesystem::path(path.substr(begin, end - begin));
        if (directory.empty()) directory = cwd;
        else if (directory.is_relative()) directory = cwd / directory;
        auto candidate = directory / spec.executable;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (errno == EACCES) error = EACCES;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    throw std::system_error(error, std::generic_category(), "Executable not found in configured PATH");
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

bool collect(ProcessResult& result, const ProcessLimits& limits, bool standard_error,
             const char* bytes, std::size_t count) {
    auto& target = standard_error ? result.standard_error : result.standard_output;
    auto& total = standard_error ? result.stderr_total_bytes : result.stdout_total_bytes;
    auto& truncated = standard_error ? result.stderr_truncated : result.stdout_truncated;
    const auto stream_limit = standard_error ? limits.stderr_bytes : limits.stdout_bytes;
    total = saturating_add(total, count);
    const auto retained = saturating_add(result.standard_output.size(), result.standard_error.size());
    const auto room_stream = stream_limit > target.size() ? stream_limit - target.size() : 0;
    const auto room_combined = limits.combined_bytes > retained ? limits.combined_bytes - retained : 0;
    const auto keep = static_cast<std::size_t>(std::min<std::uint64_t>(count, std::min(room_stream, room_combined)));
    target.append(bytes, keep);
    truncated = truncated || keep < count;
    return total > stream_limit || saturating_add(result.stdout_total_bytes, result.stderr_total_bytes) > limits.combined_bytes;
}

void decode_status(ProcessResult& result, int status) {
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.signal = WTERMSIG(status);
}

bool group_alive(pid_t pid) {
    return ::kill(-pid, 0) == 0 || errno == EPERM;
}

void terminate_group(pid_t pid, int signal) noexcept {
    ::kill(-pid, signal);
}

ProcessResult failure(int error, std::string detail) {
    ProcessResult result;
    result.native_error = error;
    result.detail = std::move(detail);
    return result;
}

ProcessResult execution_failure(ProcessResult result, int error, const char* detail) {
    if (result.termination == ProcessTermination::exited) {
        result.termination = ProcessTermination::io_failed;
        result.native_error = error;
        result.detail = detail;
    } else {
        if (!result.native_error) result.native_error = error;
        result.detail += "; process cleanup failed: ";
        result.detail += detail;
    }
    return result;
}
} // namespace

ProcessCapabilities process_capabilities() noexcept { return {true, false, false}; }

ProcessResult run_process(const ProcessSpec& spec, std::stop_token stop) {
    if (stop.stop_requested()) {
        ProcessResult result; result.termination = ProcessTermination::cancelled; return result;
    }
    if (spec.limits.memory_bytes) {
        auto result = failure(ENOTSUP, "This process backend does not enforce memory limits");
        result.termination = ProcessTermination::unsupported_limit; return result;
    }
    if (!valid_durations(spec.limits)) return failure(EINVAL, "Invalid or unrepresentable process duration limits");
    ProcessResult result;
    bool execution_started = false;
    try {
        for (const auto& argument : spec.arguments)
            if (contains_nul(argument) || !valid_utf8(argument)) return failure(EINVAL, "An argument is not valid NUL-free UTF-8");
        const auto cwd = spec.working_directory.empty() ? std::filesystem::current_path()
            : std::filesystem::absolute(spec.working_directory);
        if (contains_nul(cwd.native()) || !valid_utf8(cwd.native())) return failure(EINVAL, "Working directory is not valid NUL-free UTF-8");
        const auto environment = environment_for(spec);
        const auto executable = resolve_executable(spec, environment, cwd);
        std::vector<std::string> environment_storage;
        for (const auto& [key, value] : environment) environment_storage.push_back(key + "=" + value);
        std::vector<char*> environment_pointers;
        for (auto& entry : environment_storage) environment_pointers.push_back(entry.data());
        environment_pointers.push_back(nullptr);
        std::vector<std::string> argument_storage{executable.native()};
        argument_storage.insert(argument_storage.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argument_pointers;
        for (auto& argument : argument_storage) argument_pointers.push_back(argument.data());
        argument_pointers.push_back(nullptr);
        Pipe input = make_pipe(), output = make_pipe(), error = make_pipe();
        nonblocking(input.write.get()); nonblocking(output.read.get()); nonblocking(error.read.get());
        SpawnActions actions;
        SpawnActions::check(::posix_spawn_file_actions_adddup2(&actions.value, input.read.get(), STDIN_FILENO));
        SpawnActions::check(::posix_spawn_file_actions_adddup2(&actions.value, output.write.get(), STDOUT_FILENO));
        SpawnActions::check(::posix_spawn_file_actions_adddup2(&actions.value, error.write.get(), STDERR_FILENO));
        for (int fd : {input.read.get(), input.write.get(), output.read.get(), output.write.get(), error.read.get(), error.write.get()})
            SpawnActions::check(::posix_spawn_file_actions_addclose(&actions.value, fd));
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (__builtin_available(macOS 26.0, *)) {
            SpawnActions::check(::posix_spawn_file_actions_addchdir(&actions.value, cwd.c_str()));
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // Compatibility with the project's macOS 14 deployment floor.
            SpawnActions::check(::posix_spawn_file_actions_addchdir_np(&actions.value, cwd.c_str()));
#pragma clang diagnostic pop
        }
#elif defined(__APPLE__) || defined(__linux__)
        SpawnActions::check(::posix_spawn_file_actions_addchdir_np(&actions.value, cwd.c_str()));
#else
        if (cwd != std::filesystem::current_path()) return failure(ENOTSUP, "Working-directory spawn action is unavailable");
#endif
        SpawnAttributes attributes;
        short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
        SpawnActions::check(::posix_spawnattr_setflags(&attributes.value, flags));
        SpawnActions::check(::posix_spawnattr_setpgroup(&attributes.value, 0));
        sigset_t empty, defaults;
        sigemptyset(&empty); sigemptyset(&defaults); sigaddset(&defaults, SIGPIPE);
        SpawnActions::check(::posix_spawnattr_setsigmask(&attributes.value, &empty));
        SpawnActions::check(::posix_spawnattr_setsigdefault(&attributes.value, &defaults));
        SigpipeGuard sigpipe_guard;
        pid_t pid = 0;
        if (stop.stop_requested()) { ProcessResult result; result.termination = ProcessTermination::cancelled; return result; }
        const auto spawn_begin = Clock::now();
        const int spawn_error = ::posix_spawn(&pid, executable.c_str(), &actions.value, &attributes.value,
                                              argument_pointers.data(), environment_pointers.data());
        const auto execution_begin = Clock::now();
        if (spawn_error) return failure(spawn_error, std::strerror(spawn_error));
        Child child(pid);
        execution_started = true;
        input.read.reset(); output.write.reset(); error.write.reset();
        result.termination = ProcessTermination::exited;
        bool reaped = false, stopping = false, killed = false, forced = false;
        Time stop_begin{}, drain_deadline{};
        std::size_t written = 0;
        if (spec.input.empty()) input.write.reset();
        auto begin_cleanup = [&](Time now) {
            if (!stopping) {
                stopping = true; stop_begin = now; drain_deadline = now + spec.limits.terminate_grace + spec.limits.drain_timeout;
                input.write.reset(); terminate_group(pid, SIGTERM);
            }
        };
        auto force = [&](ProcessTermination reason, int native_error, const char* detail, Time now) {
            if (!forced) {
                forced = true; result.termination = reason; result.native_error = native_error; result.detail = detail;
            }
            begin_cleanup(now);
        };
        if (stop.stop_requested()) force(ProcessTermination::cancelled, 0, "Cancelled during process creation", execution_begin);
        else if (execution_begin - spawn_begin > spec.limits.spawn_timeout)
            force(ProcessTermination::spawn_failed, ETIMEDOUT, "Process creation exceeded the soft spawn timeout", execution_begin);

        std::array<char, 32768> buffer;
        auto read_stream = [&](Fd& fd, bool stderr_stream) {
            for (int chunks = 0; fd.get() >= 0 && chunks < 8; ++chunks) {
                const auto count = ::read(fd.get(), buffer.data(), buffer.size());
                if (count > 0) {
                    if (collect(result, spec.limits, stderr_stream, buffer.data(), static_cast<std::size_t>(count)))
                        force(ProcessTermination::output_limit, 0, "Output byte limit exceeded", Clock::now());
                } else if (count == 0) { fd.reset(); break; }
                else if (errno == EINTR) { --chunks; continue; }
                else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else {
                    const int read_error = errno; fd.reset();
                    force(ProcessTermination::io_failed, read_error, "Failed to read process output", Clock::now()); break;
                }
            }
        };

        for (;;) {
            auto now = Clock::now();
            if (!reaped) {
                int status = 0;
                const auto waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid) {
                    reaped = true; child.reaped(); decode_status(result, status);
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - execution_begin);
                    begin_cleanup(now);
                } else if (waited < 0 && errno != EINTR) {
                    const int wait_error = errno;
                    force(ProcessTermination::io_failed, wait_error, "Failed to reap child process", now);
                    if (wait_error == ECHILD) { reaped = true; child.reaped(); }
                }
            }
            if (!reaped && !forced) {
                if (stop.stop_requested()) force(ProcessTermination::cancelled, 0, "Cancelled", now);
                else if (now - execution_begin >= spec.limits.wall_timeout)
                    force(ProcessTermination::timed_out, 0, "Execution wall timeout exceeded", now);
            }
            if (stopping && !killed && now - stop_begin >= spec.limits.terminate_grace) {
                terminate_group(pid, SIGKILL); killed = true;
            }
            if (reaped && output.read.get() < 0 && error.read.get() < 0 && (!group_alive(pid) || killed)) break;
            if (stopping && now >= drain_deadline) {
                terminate_group(pid, SIGKILL); killed = true;
                if (!reaped) {
                    int status = 0;
                    pid_t waited;
                    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
                    if (waited == pid) { decode_status(result, status); reaped = true; child.reaped(); }
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - execution_begin);
                }
                read_stream(output.read, false); read_stream(error.read, true);
                if (!forced && (output.read.get() >= 0 || error.read.get() >= 0)) {
                    result.termination = ProcessTermination::drain_timeout;
                    result.detail = "Output pipes remained open beyond the cleanup deadline";
                }
                output.read.reset(); error.read.reset(); break;
            }
            std::array<pollfd, 3> descriptors{{
                {input.write.get(), POLLOUT, 0}, {output.read.get(), POLLIN, 0}, {error.read.get(), POLLIN, 0}}};
            const int polled = ::poll(descriptors.data(), descriptors.size(), 10);
            if (polled < 0) {
                if (errno != EINTR) force(ProcessTermination::io_failed, errno, "Process pipe poll failed", Clock::now());
                continue;
            }
            if (input.write.get() >= 0 && (descriptors[0].revents & POLLOUT)) {
                const auto count = ::write(input.write.get(), spec.input.data() + written,
                                            std::min<std::size_t>(32768, spec.input.size() - written));
                if (count > 0) written += static_cast<std::size_t>(count);
                else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno != EPIPE) force(ProcessTermination::io_failed, errno, "Failed to write process input", Clock::now());
                    input.write.reset();
                }
                if (written == spec.input.size()) input.write.reset();
            }
            if (input.write.get() >= 0 && (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))) input.write.reset();
            if (output.read.get() >= 0 && descriptors[1].revents) read_stream(output.read, false);
            if (error.read.get() >= 0 && descriptors[2].revents) read_stream(error.read, true);
        }
        if (reaped) child.cleaned();
        return result;
    } catch (const std::system_error& error) {
        if (execution_started) return execution_failure(std::move(result), error.code().value(), error.what());
        return failure(error.code().value(), error.what());
    } catch (const std::exception& error) {
        if (execution_started) return execution_failure(std::move(result), EIO, error.what());
        return failure(EINVAL, error.what());
    }
}
} // namespace cpn
#endif
