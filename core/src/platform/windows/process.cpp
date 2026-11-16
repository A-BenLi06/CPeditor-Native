// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "cpn/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cwchar>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>

namespace cpn {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) noexcept {
        if (valid()) ::CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

[[noreturn]] void fail(DWORD error, const char* operation) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
void require(BOOL result, const char* operation) { if (!result) fail(::GetLastError(), operation); }

std::wstring wide(const std::string& text) {
    if (text.find('\0') != std::string::npos) fail(ERROR_INVALID_PARAMETER, "Embedded NUL in UTF-8 argument");
    if (text.empty()) return {};
    if (text.size() > INT_MAX) fail(ERROR_INVALID_PARAMETER, "UTF-8 value is too long");
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) fail(::GetLastError(), "Invalid UTF-8 argument");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    require(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size), "UTF-8 conversion");
    return result;
}

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
void validate_native_path(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos || path.size() > INT_MAX)
        fail(ERROR_INVALID_PARAMETER, "Invalid native path");
    if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
                              nullptr, 0, nullptr, nullptr)) fail(::GetLastError(), "Invalid Unicode path");
}

struct EnvironmentLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};
using Environment = std::map<std::wstring, std::wstring, EnvironmentLess>;
Environment environment_for(const ProcessSpec& spec) {
    Environment values;
    if (spec.inherit_environment) {
        wchar_t* block = ::GetEnvironmentStringsW();
        if (!block) fail(::GetLastError(), "GetEnvironmentStringsW");
        struct Release { wchar_t* value; ~Release() { ::FreeEnvironmentStringsW(value); } } release{block};
        for (const wchar_t* entry = block; *entry; entry += ::wcslen(entry) + 1) {
            std::wstring item(entry);
            const auto equal = item.find(L'=', item.front() == L'=' ? 1 : 0);
            if (equal != std::wstring::npos) values[item.substr(0, equal)] = item.substr(equal + 1);
        }
    }
    for (const auto& [key, value] : spec.environment) {
        if (key.empty() || key.find('=') != std::string::npos) fail(ERROR_INVALID_PARAMETER, "Invalid environment key");
        values[wide(key)] = wide(value);
    }
    return values;
}
std::vector<wchar_t> environment_block(const Environment& values) {
    std::vector<wchar_t> block;
    for (const auto& [key, value] : values) {
        block.insert(block.end(), key.begin(), key.end()); block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end()); block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::wstring quote_argument(const std::wstring& argument) {
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\'); result.push_back(character);
        } else {
            result.append(slashes, L'\\'); result.push_back(character);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'"');
    return result;
}

std::filesystem::path resolve_executable(const ProcessSpec& spec, const Environment& environment,
                                        const std::filesystem::path& cwd) {
    validate_native_path(spec.executable.native());
    auto with_extension = [](std::filesystem::path path) {
        if (!path.has_extension()) path += L".exe";
        return path;
    };
    if (spec.executable.is_absolute() || spec.executable.has_parent_path())
        return with_extension(spec.executable.is_absolute() ? spec.executable : cwd / spec.executable);
    auto name = with_extension(spec.executable);
    std::wstring path;
    if (auto found = environment.find(L"PATH"); found != environment.end()) path = found->second;
    else {
        std::array<wchar_t, 32768> system;
        const auto length = ::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!length || length >= system.size()) fail(::GetLastError(), "GetSystemDirectoryW");
        path.assign(system.data(), length);
    }
    for (std::size_t begin = 0;;) {
        const auto end = path.find(L';', begin);
        auto directory = std::filesystem::path(path.substr(begin, end - begin));
        if (directory.empty()) directory = cwd;
        else if (directory.is_relative()) directory = cwd / directory;
        auto candidate = directory / name;
        const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    fail(ERROR_FILE_NOT_FOUND, "Executable not found in configured PATH");
}

struct Pipe { Handle parent; Handle child; };
Pipe make_pipe(bool parent_writes) {
    static std::atomic<unsigned long long> serial{0};
    const std::wstring name = L"\\\\.\\pipe\\cpn-process-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
    Pipe pipe;
    const DWORD access = (parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
                         FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
    pipe.parent.reset(::CreateNamedPipeW(name.c_str(), access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                         PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    if (!pipe.parent.valid()) fail(::GetLastError(), "CreateNamedPipeW");
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    pipe.child.reset(::CreateFileW(name.c_str(), parent_writes ? GENERIC_READ : GENERIC_WRITE,
                                  0, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!pipe.child.valid()) fail(::GetLastError(), "CreateFileW pipe client");
    Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) fail(::GetLastError(), "CreateEventW pipe connect");
    OVERLAPPED connected{}; connected.hEvent = event.get();
    if (!::ConnectNamedPipe(pipe.parent.get(), &connected)) {
        const auto error = ::GetLastError();
        if (error == ERROR_IO_PENDING) {
            DWORD transferred = 0;
            require(::GetOverlappedResult(pipe.parent.get(), &connected, &transferred, TRUE), "ConnectNamedPipe");
        } else if (error != ERROR_PIPE_CONNECTED) fail(error, "ConnectNamedPipe");
    }
    return pipe;
}

class Operation {
public:
    explicit Operation(Handle file) : file(std::move(file)), event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!event.valid()) fail(::GetLastError(), "CreateEventW operation");
        reset_overlapped();
    }
    ~Operation() { close(); }
    void reset_overlapped() {
        ::ResetEvent(event.get()); overlapped = {}; overlapped.hEvent = event.get();
    }
    void close() noexcept {
        if (pending && file.valid()) {
            ::CancelIoEx(file.get(), &overlapped);
            DWORD ignored = 0;
            ::GetOverlappedResult(file.get(), &overlapped, &ignored, TRUE);
        }
        pending = false; closed = true; file.reset();
    }
    Handle file;
    Handle event;
    OVERLAPPED overlapped{};
    std::array<char, 32768> buffer{};
    bool pending{false};
    bool closed{false};
};

class AttributeList {
public:
    AttributeList(HANDLE* handles, std::size_t count) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        require(::InitializeProcThreadAttributeList(list, 1, 0, &size), "InitializeProcThreadAttributeList");
        initialized_ = true;
        if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                         count * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD error = ::GetLastError();
            ::DeleteProcThreadAttributeList(list); initialized_ = false;
            fail(error, "UpdateProcThreadAttribute");
        }
    }
    ~AttributeList() { if (initialized_) ::DeleteProcThreadAttributeList(list); }
    LPPROC_THREAD_ATTRIBUTE_LIST list{nullptr};
private:
    std::vector<std::byte> storage_;
    bool initialized_{false};
};
class Child {
public:
    explicit Child(PROCESS_INFORMATION information) : process(information.hProcess), thread(information.hThread) {}
    ~Child() {
        if (!finished && process.valid()) { ::TerminateProcess(process.get(), 1); ::WaitForSingleObject(process.get(), INFINITE); }
    }
    Handle process;
    Handle thread;
    bool finished{false};
};
std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max() : left + right;
}
bool collect(ProcessResult& result, const ProcessLimits& limits, bool standard_error, const char* bytes, DWORD count) {
    auto& target = standard_error ? result.standard_error : result.standard_output;
    auto& total = standard_error ? result.stderr_total_bytes : result.stdout_total_bytes;
    auto& truncated = standard_error ? result.stderr_truncated : result.stdout_truncated;
    const auto stream_limit = standard_error ? limits.stderr_bytes : limits.stdout_bytes;
    total = saturating_add(total, count);
    const auto retained = saturating_add(result.standard_output.size(), result.standard_error.size());
    const auto stream_room = stream_limit > target.size() ? stream_limit - target.size() : 0;
    const auto combined_room = limits.combined_bytes > retained ? limits.combined_bytes - retained : 0;
    const auto keep = static_cast<std::size_t>(std::min<std::uint64_t>(count, std::min(stream_room, combined_room)));
    target.append(bytes, keep); truncated = truncated || keep < count;
    return total > stream_limit || saturating_add(result.stdout_total_bytes, result.stderr_total_bytes) > limits.combined_bytes;
}
ProcessResult failure(DWORD error, std::string detail) {
    ProcessResult result; result.native_error = static_cast<int>(error); result.detail = std::move(detail); return result;
}
ProcessResult execution_failure(ProcessResult result, DWORD error, const char* detail) {
    if (result.termination == ProcessTermination::exited) {
        result.termination = ProcessTermination::io_failed;
        result.native_error = static_cast<int>(error);
        result.detail = detail;
    } else {
        if (!result.native_error) result.native_error = static_cast<int>(error);
        result.detail += "; process cleanup failed: ";
        result.detail += detail;
    }
    return result;
}
} // namespace

ProcessCapabilities process_capabilities() noexcept { return {true, false, false}; }

ProcessResult run_process(const ProcessSpec& spec, std::stop_token stop) {
    if (stop.stop_requested()) { ProcessResult result; result.termination = ProcessTermination::cancelled; return result; }
    if (spec.limits.memory_bytes) {
        auto result = failure(ERROR_NOT_SUPPORTED, "This process backend does not enforce memory limits");
        result.termination = ProcessTermination::unsupported_limit; return result;
    }
    if (!valid_durations(spec.limits)) return failure(ERROR_INVALID_PARAMETER, "Invalid or unrepresentable process duration limits");
    ProcessResult result;
    bool execution_started = false;
    try {
        const auto cwd = spec.working_directory.empty() ? std::filesystem::current_path()
            : std::filesystem::absolute(spec.working_directory);
        validate_native_path(cwd.native());
        const auto environment = environment_for(spec);
        const auto executable = resolve_executable(spec, environment, cwd);
        auto block = environment_block(environment);
        std::wstring command = quote_argument(executable.native());
        for (const auto& argument : spec.arguments) { command.push_back(L' '); command += quote_argument(wide(argument)); }
        if (command.size() >= 32767) fail(ERROR_INVALID_PARAMETER, "Process command line is too long");
        std::vector<wchar_t> command_line(command.begin(), command.end()); command_line.push_back(L'\0');
        Handle job(::CreateJobObjectW(nullptr, nullptr));
        if (!job.valid()) fail(::GetLastError(), "CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
        job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &job_limits, sizeof(job_limits)), "SetInformationJobObject");
        Pipe input = make_pipe(true), output = make_pipe(false), error = make_pipe(false);
        HANDLE inherited[]{input.child.get(), output.child.get(), error.child.get()};
        AttributeList attributes(inherited, std::size(inherited));
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.child.get(); startup.StartupInfo.hStdOutput = output.child.get();
        startup.StartupInfo.hStdError = error.child.get(); startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION information{};
        if (stop.stop_requested()) { result.termination = ProcessTermination::cancelled; return result; }
        const auto spawn_begin = Clock::now();
        require(::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                 CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                 block.data(), cwd.c_str(), &startup.StartupInfo, &information), "CreateProcessW");
        Child child(information);
        require(::AssignProcessToJobObject(job.get(), child.process.get()), "AssignProcessToJobObject");
        input.child.reset(); output.child.reset(); error.child.reset();
        Operation input_io(std::move(input.parent)), output_io(std::move(output.parent)), error_io(std::move(error.parent));
        Handle cancel_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!cancel_event.valid()) fail(::GetLastError(), "CreateEventW cancellation");
        std::stop_callback cancellation(stop, [&] { ::SetEvent(cancel_event.get()); });
        result.termination = ProcessTermination::exited;
        bool forced = false, stopping = false, killed = false;
        Time stop_begin{}, drain_deadline{};
        const auto execution_begin = Clock::now();
        auto begin_cleanup = [&](Time now) {
            if (!stopping) {
                stopping = true; stop_begin = now; drain_deadline = now + spec.limits.terminate_grace + spec.limits.drain_timeout;
                // EOF is the available cooperative shutdown request in a no-console batch host.
                input_io.close();
            }
        };
        auto force = [&](ProcessTermination reason, DWORD native_error, const char* detail, Time now) {
            if (!forced) { forced = true; result.termination = reason; result.native_error = static_cast<int>(native_error); result.detail = detail; }
            begin_cleanup(now);
        };
        if (stop.stop_requested()) force(ProcessTermination::cancelled, 0, "Cancelled during process creation", execution_begin);
        else if (execution_begin - spawn_begin > spec.limits.spawn_timeout)
            force(ProcessTermination::spawn_failed, WAIT_TIMEOUT, "Process creation exceeded the soft spawn timeout", execution_begin);
        else {
            if (::ResumeThread(child.thread.get()) == static_cast<DWORD>(-1)) fail(::GetLastError(), "ResumeThread");
        }
        execution_started = true;
        child.thread.reset();
        std::size_t written = 0;
        if (spec.input.empty()) input_io.close();
        auto read_stream = [&](Operation& operation, bool standard_error) {
            if (operation.closed) return;
            for (int chunks = 0; chunks < 8 && !operation.closed; ++chunks) {
                DWORD count = 0;
                if (operation.pending) {
                    if (!::GetOverlappedResult(operation.file.get(), &operation.overlapped, &count, FALSE)) {
                        const auto native_error = ::GetLastError();
                        if (native_error == ERROR_IO_INCOMPLETE) return;
                        operation.pending = false;
                        if (native_error == ERROR_BROKEN_PIPE || native_error == ERROR_OPERATION_ABORTED) { operation.close(); return; }
                        force(ProcessTermination::io_failed, native_error, "ReadFile completion failed", Clock::now()); operation.close(); return;
                    }
                    operation.pending = false;
                } else {
                    operation.reset_overlapped();
                    if (!::ReadFile(operation.file.get(), operation.buffer.data(), static_cast<DWORD>(operation.buffer.size()), &count, &operation.overlapped)) {
                        const auto native_error = ::GetLastError();
                        if (native_error == ERROR_IO_PENDING) { operation.pending = true; return; }
                        if (native_error == ERROR_BROKEN_PIPE) { operation.close(); return; }
                        force(ProcessTermination::io_failed, native_error, "ReadFile failed", Clock::now()); operation.close(); return;
                    }
                }
                if (!count) { operation.close(); return; }
                if (collect(result, spec.limits, standard_error, operation.buffer.data(), count))
                    force(ProcessTermination::output_limit, 0, "Output byte limit exceeded", Clock::now());
            }
        };
        auto write_input = [&] {
            if (input_io.closed) return;
            DWORD count = 0;
            if (input_io.pending) {
                if (!::GetOverlappedResult(input_io.file.get(), &input_io.overlapped, &count, FALSE)) {
                    const auto native_error = ::GetLastError();
                    if (native_error == ERROR_IO_INCOMPLETE) return;
                    input_io.pending = false;
                    if (native_error != ERROR_BROKEN_PIPE && native_error != ERROR_NO_DATA && native_error != ERROR_OPERATION_ABORTED)
                        force(ProcessTermination::io_failed, native_error, "WriteFile completion failed", Clock::now());
                    input_io.close(); return;
                }
                input_io.pending = false; written += count;
                if (!count && written < spec.input.size()) { force(ProcessTermination::io_failed, ERROR_WRITE_FAULT, "WriteFile made no progress", Clock::now()); return; }
            }
            if (written == spec.input.size()) { input_io.close(); return; }
            input_io.reset_overlapped();
            if (!::WriteFile(input_io.file.get(), spec.input.data() + written,
                             static_cast<DWORD>(std::min<std::size_t>(32768, spec.input.size() - written)), &count, &input_io.overlapped)) {
                const auto native_error = ::GetLastError();
                if (native_error == ERROR_IO_PENDING) { input_io.pending = true; return; }
                if (native_error != ERROR_BROKEN_PIPE && native_error != ERROR_NO_DATA)
                    force(ProcessTermination::io_failed, native_error, "WriteFile failed", Clock::now());
                input_io.close(); return;
            }
            written += count;
            if (written == spec.input.size()) input_io.close();
        };
        for (;;) {
            auto now = Clock::now();
            if (!child.finished) {
                const auto state = ::WaitForSingleObject(child.process.get(), 0);
                if (state == WAIT_OBJECT_0) {
                    child.finished = true;
                    DWORD code = 0; require(::GetExitCodeProcess(child.process.get(), &code), "GetExitCodeProcess");
                    result.exit_code = static_cast<int>(code);
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - execution_begin);
                    begin_cleanup(now);
                } else if (state == WAIT_FAILED) force(ProcessTermination::io_failed, ::GetLastError(), "Process wait failed", now);
            }
            if (!child.finished && !forced) {
                if (stop.stop_requested()) force(ProcessTermination::cancelled, 0, "Cancelled", now);
                else if (now - execution_begin >= spec.limits.wall_timeout)
                    force(ProcessTermination::timed_out, 0, "Execution wall timeout exceeded", now);
            }
            if (stopping && !killed && now - stop_begin >= spec.limits.terminate_grace) {
                require(::TerminateJobObject(job.get(), 1), "TerminateJobObject"); killed = true;
            }
            read_stream(output_io, false); read_stream(error_io, true); write_input();
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            require(::QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr), "QueryInformationJobObject");
            if (child.finished && output_io.closed && error_io.closed && accounting.ActiveProcesses == 0) break;
            if (stopping && Clock::now() >= drain_deadline) {
                require(::TerminateJobObject(job.get(), 1), "TerminateJobObject cleanup"); killed = true;
                if (!child.finished) {
                    require(::WaitForSingleObject(child.process.get(), INFINITE) == WAIT_OBJECT_0, "Process cleanup wait");
                    child.finished = true;
                    DWORD code = 0; require(::GetExitCodeProcess(child.process.get(), &code), "GetExitCodeProcess cleanup");
                    result.exit_code = static_cast<int>(code);
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - execution_begin);
                }
                read_stream(output_io, false); read_stream(error_io, true);
                if (!forced && (!output_io.closed || !error_io.closed)) {
                    result.termination = ProcessTermination::drain_timeout;
                    result.detail = "Output pipes remained open beyond the cleanup deadline";
                }
                input_io.close(); output_io.close(); error_io.close(); break;
            }
            std::array<HANDLE, 5> waits{};
            DWORD count = 0;
            if (!child.finished) waits[count++] = child.process.get();
            if (!forced) waits[count++] = cancel_event.get();
            for (Operation* operation : {&input_io, &output_io, &error_io})
                if (operation->pending && !operation->closed) waits[count++] = operation->event.get();
            if (count) {
                if (::WaitForMultipleObjects(count, waits.data(), FALSE, 10) == WAIT_FAILED)
                    force(ProcessTermination::io_failed, ::GetLastError(), "Process I/O wait failed", Clock::now());
            } else ::Sleep(10);
        }
        return result;
    } catch (const std::system_error& error) {
        if (execution_started) return execution_failure(std::move(result), static_cast<DWORD>(error.code().value()), error.what());
        return failure(static_cast<DWORD>(error.code().value()), error.what());
    } catch (const std::exception& error) {
        if (execution_started) return execution_failure(std::move(result), ERROR_GEN_FAILURE, error.what());
        return failure(ERROR_INVALID_PARAMETER, error.what());
    }
}
} // namespace cpn
#endif
