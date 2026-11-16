// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/external_checker.hpp"
#include "cpn/text.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace cpn {
namespace {
constexpr std::array<std::string_view, 5> variables{"input", "actual", "expected", "job_dir", "source_dir"};
// Private checker-file protocol: these names never target the caller's source directory.
constexpr std::array<std::string_view, 3> filenames{"input.bin", "actual.bin", "expected.bin"};
std::string bounded_detail(std::string_view text) {
    auto end = std::min<std::size_t>(4096, text.size());
    if (end < text.size()) while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end));
}
void consume(std::size_t& total, std::size_t count, std::size_t maximum) {
    if (count > maximum - total) throw std::invalid_argument("External checker byte limit exceeded");
    total += count;
}
void utf8(std::string_view value, bool nonempty = false) {
    if ((nonempty && value.empty()) || value.find('\0') != std::string_view::npos || decode_utf8(value).error_offset)
        throw std::invalid_argument("External checker text must be valid NUL-free UTF-8");
}
void bounds_valid(const ExternalCheckerBounds& bounds) {
    if (!bounds.max_command_bytes || !bounds.max_path_bytes) throw std::invalid_argument("Invalid external checker bounds");
}
void template_valid(std::string_view text) {
    utf8(text);
    for (auto begin = text.find("${"); begin != std::string_view::npos;) {
        const auto end = text.find('}', begin + 2);
        if (end == std::string_view::npos) throw std::invalid_argument("Unclosed external checker placeholder");
        const auto key = text.substr(begin + 2, end - begin - 2);
        if (std::find(variables.begin(), variables.end(), key) == variables.end())
            throw std::invalid_argument("Unknown external checker placeholder");
        begin = text.find("${", end + 1);
    }
}
void limits_valid(const ProcessLimits& limits) {
    using Clock = std::chrono::steady_clock;
    auto latest_start = Clock::time_point::max();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    for (const auto duration : {limits.spawn_timeout, limits.wall_timeout, limits.terminate_grace, limits.drain_timeout}) {
        if (duration.count() < 0 || duration > maximum) throw std::invalid_argument("Invalid external checker duration");
        const auto delta = std::chrono::duration_cast<Clock::duration>(duration);
        if (latest_start.time_since_epoch() < delta) throw std::invalid_argument("External checker durations overflow");
        latest_start -= delta;
    }
    if (limits.spawn_timeout.count() <= 0 || limits.wall_timeout.count() <= 0 || limits.drain_timeout.count() <= 0 ||
        Clock::now() > latest_start || (limits.memory_bytes && !*limits.memory_bytes))
        throw std::invalid_argument("Invalid external checker limits");
}
std::string fold_ascii(std::string value) {
    for (auto& byte : value) if (byte >= 'a' && byte <= 'z') byte = static_cast<char>(byte - 'a' + 'A');
    return value;
}
std::string expand(std::string_view text, const std::map<std::string_view, std::string>& values,
                   std::size_t maximum, std::size_t& total) {
    std::string result;
    for (std::size_t position = 0; position < text.size();) {
        if (text[position] == '$' && position + 1 < text.size() && text[position + 1] == '{') {
            const auto end = text.find('}', position + 2);
            if (end == std::string_view::npos) throw std::invalid_argument("Unclosed external checker placeholder");
            const auto found = values.find(text.substr(position + 2, end - position - 2));
            if (found == values.end()) throw std::invalid_argument("Unknown external checker placeholder");
            consume(total, found->second.size(), maximum); result += found->second; position = end + 1;
        } else { consume(total, 1, maximum); result += text[position++]; }
    }
    return result;
}
class Workspace {
public:
    explicit Workspace(std::shared_ptr<FileStore> files) : files_(std::move(files)), path(files_->create_workspace()) {}
    ~Workspace() { files_->remove_workspace(path); }
private:
    std::shared_ptr<FileStore> files_;
public:
    std::filesystem::path path;
};
std::uint64_t add(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
}
bool retain_outputs(ProcessResult& result, const ProcessLimits& limits) {
    result.stdout_total_bytes = std::max<std::uint64_t>(result.stdout_total_bytes, result.standard_output.size());
    result.stderr_total_bytes = std::max<std::uint64_t>(result.stderr_total_bytes, result.standard_error.size());
    const auto out = std::min<std::uint64_t>(result.standard_output.size(), std::min(limits.stdout_bytes, limits.combined_bytes));
    const auto err = std::min<std::uint64_t>(result.standard_error.size(), std::min(limits.stderr_bytes, limits.combined_bytes - out));
    result.stdout_truncated = result.stdout_truncated || out < result.standard_output.size();
    result.stderr_truncated = result.stderr_truncated || err < result.standard_error.size();
    result.standard_output.resize(static_cast<std::size_t>(out)); result.standard_error.resize(static_cast<std::size_t>(err));
    return result.stdout_truncated || result.stderr_truncated || result.stdout_total_bytes > limits.stdout_bytes ||
        result.stderr_total_bytes > limits.stderr_bytes || add(result.stdout_total_bytes, result.stderr_total_bytes) > limits.combined_bytes;
}
}
void validate_external_checker(const ExternalCheckerSpec& spec, ExternalCheckerBounds bounds) {
    bounds_valid(bounds); utf8(spec.command.executable, true); template_valid(spec.command.executable); limits_valid(spec.limits);
    if (spec.command.arguments.size() > bounds.max_arguments) throw std::invalid_argument("Too many external checker arguments");
    std::size_t bytes = 0; consume(bytes, spec.command.executable.size(), bounds.max_command_bytes);
    for (const auto& argument : spec.command.arguments) {
        template_valid(argument); consume(bytes, 1, bounds.max_command_bytes); consume(bytes, argument.size(), bounds.max_command_bytes);
    }
    if (spec.accepted_exit_codes.empty()) throw std::invalid_argument("External checker requires an accepted exit code");
    std::set<int> codes;
    for (const auto* mapping : {&spec.accepted_exit_codes, &spec.wrong_answer_exit_codes})
        for (const auto code : *mapping)
            if (code < 0 || code > 255 || !codes.insert(code).second)
                throw std::invalid_argument("External checker exit codes must be distinct portable values in 0..255");
    std::set<std::string> names;
    for (const auto& [name, value] : spec.environment) {
        utf8(name, true); utf8(value);
        if (name.find('=') != std::string::npos || !names.insert(fold_ascii(name)).second)
            throw std::invalid_argument("Invalid or duplicate external checker environment key");
        consume(bytes, name.size(), bounds.max_command_bytes); consume(bytes, value.size(), bounds.max_command_bytes);
    }
}
ExternalCheckerService::ExternalCheckerService(std::shared_ptr<FileStore> files, ProcessExecutor process, ExternalCheckerBounds bounds)
    : files_(std::move(files)), process_(std::move(process)), bounds_(bounds) {
    if (!files_ || !process_) throw std::invalid_argument("Missing external checker service port");
    bounds_valid(bounds_);
}
ExternalCheckerResult ExternalCheckerService::run(const ExternalCheckerSpec& spec, const CheckerInput& input, std::stop_token stop) const {
    ExternalCheckerResult result;
    const auto cancelled = [&] {
        result.verdict = ExternalCheckerVerdict::cancelled; result.process.termination = ProcessTermination::cancelled;
        result.detail = "External checker cancelled before execution";
    };
    if (stop.stop_requested()) { cancelled(); return result; }
    try {
        validate_external_checker(spec, bounds_);
        if (input.input.size() > bounds_.max_input_bytes || input.actual.size() > bounds_.max_actual_bytes || input.expected.size() > bounds_.max_expected_bytes)
            throw std::invalid_argument("External checker input stream exceeds byte limit");
        std::size_t bytes = 0;
        for (const auto* value : {&input.input, &input.actual, &input.expected}) consume(bytes, value->size(), bounds_.max_total_input_bytes);
        std::optional<std::string> source_dir;
        if (input.source_dir) {
            const auto original = path_to_utf8(*input.source_dir);
            if (original.empty() || original.size() > bounds_.max_path_bytes) throw std::invalid_argument("Invalid external checker source directory");
            source_dir = path_to_utf8(std::filesystem::absolute(*input.source_dir));
            if (source_dir->size() > bounds_.max_path_bytes) throw std::invalid_argument("Invalid external checker source directory");
        }
        if (stop.stop_requested()) { cancelled(); return result; }
        Workspace workspace(files_);
        const auto original_directory = path_to_utf8(workspace.path);
        if (original_directory.empty() || original_directory.size() > bounds_.max_path_bytes) throw std::invalid_argument("Invalid checker workspace path");
        const auto working_directory = std::filesystem::absolute(workspace.path);
        const auto directory = path_to_utf8(working_directory);
        if (directory.size() > bounds_.max_path_bytes) throw std::invalid_argument("Invalid checker workspace path");
        std::array<std::filesystem::path, 3> paths;
        const std::array<const std::string*, 3> values{&input.input, &input.actual, &input.expected};
        for (std::size_t index = 0; index < paths.size(); ++index) {
            if (stop.stop_requested()) { cancelled(); return result; }
            paths[index] = working_directory / path_from_utf8(filenames[index]);
            if (path_to_utf8(paths[index]).size() > bounds_.max_path_bytes) throw std::invalid_argument("Checker file path exceeds limit");
            files_->write_atomic(paths[index], *values[index]);
        }
        if (stop.stop_requested()) { cancelled(); return result; }
        std::map<std::string_view, std::string> replacements{{variables[0], path_to_utf8(paths[0])}, {variables[1], path_to_utf8(paths[1])},
            {variables[2], path_to_utf8(paths[2])}, {variables[3], directory}, {variables[4], source_dir.value_or(directory)}};
        ProcessSpec process; std::size_t expanded = 0;
        process.executable = path_from_utf8(expand(spec.command.executable, replacements, bounds_.max_command_bytes, expanded));
        for (const auto& argument : spec.command.arguments) {
            consume(expanded, 1, bounds_.max_command_bytes);
            process.arguments.push_back(expand(argument, replacements, bounds_.max_command_bytes, expanded));
        }
        process.working_directory = working_directory; process.limits = spec.limits;
        process.inherit_environment = spec.inherit_environment; process.environment = spec.environment;
        if (stop.stop_requested()) { cancelled(); return result; }
        result.process_attempted = true; result.process = process_(process, stop);
        result.process.detail = bounded_detail(result.process.detail); result.detail = result.process.detail;
        const auto over_limit = retain_outputs(result.process, spec.limits);
        if (result.process.termination == ProcessTermination::cancelled) result.verdict = ExternalCheckerVerdict::cancelled;
        else if (over_limit) {
            if (result.process.termination == ProcessTermination::exited) result.process.termination = ProcessTermination::output_limit;
            if (result.detail.empty()) result.detail = "External checker output byte limit exceeded";
        } else if (result.process.termination != ProcessTermination::exited || result.process.signal || !result.process.exit_code) {
            if (result.detail.empty()) result.detail = "External checker did not exit normally";
        } else if (std::find(spec.accepted_exit_codes.begin(), spec.accepted_exit_codes.end(), *result.process.exit_code) != spec.accepted_exit_codes.end())
            result.verdict = ExternalCheckerVerdict::accepted;
        else if (std::find(spec.wrong_answer_exit_codes.begin(), spec.wrong_answer_exit_codes.end(), *result.process.exit_code) != spec.wrong_answer_exit_codes.end())
            result.verdict = ExternalCheckerVerdict::wrong_answer;
        else if (result.detail.empty()) result.detail = "External checker returned an unmapped exit code: " + std::to_string(*result.process.exit_code);
    } catch (const std::exception& error) {
        result.detail = bounded_detail(error.what());
        if (result.process_attempted) { result.process.termination = ProcessTermination::io_failed; result.process.detail = result.detail; }
    }
    return result;
}
const char* external_checker_verdict_name(ExternalCheckerVerdict verdict) noexcept {
    constexpr const char* names[]{"Accepted", "WrongAnswer", "Cancelled", "CheckerError"};
    const auto index = static_cast<unsigned>(verdict); return index < std::size(names) ? names[index] : "Unknown";
}
} // namespace cpn
