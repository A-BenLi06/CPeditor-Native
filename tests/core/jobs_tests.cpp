// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/jobs.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace cpn;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void success(const JobStatus& status, const char* message) { expect(static_cast<bool>(status), message); }

class MemoryFiles final : public FileStore {
public:
    std::string read(const std::filesystem::path&, std::size_t) const override {
        throw std::logic_error("Read is not used by the injected batch executor");
    }
    std::optional<std::string> read_optional(const std::filesystem::path&,std::size_t) const override {
        throw std::logic_error("Optional read is not used by the injected batch executor");
    }
    std::unique_ptr<FileLock> acquire_lock(const std::filesystem::path&,
        std::chrono::milliseconds,std::stop_token) const override {
        throw std::logic_error("Lock is not used by the injected batch executor");
    }
    void write_atomic(const std::filesystem::path& path, std::string_view text) const override {
        std::lock_guard lock(mutex_);
        writes_[path] = std::string{text};
    }
    std::filesystem::path create_workspace() const override {
        std::lock_guard lock(mutex_);
        auto path = std::filesystem::temp_directory_path() / ("cpn-job-memory-" + std::to_string(++id_));
        active_.insert(path);
        return path;
    }
    void remove_workspace(const std::filesystem::path& path) const noexcept override {
        std::lock_guard lock(mutex_);
        active_.erase(path);
    }
    std::size_t active() const {
        std::lock_guard lock(mutex_);
        return active_.size();
    }
    bool wrote(std::string_view text) const {
        std::lock_guard lock(mutex_);
        return std::any_of(writes_.begin(), writes_.end(), [&](const auto& item) { return item.second == text; });
    }
private:
    mutable std::mutex mutex_;
    mutable std::uint64_t id_{0};
    mutable std::map<std::filesystem::path,std::string> writes_;
    mutable std::set<std::filesystem::path> active_;
};

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released{false};
    bool cancelled{false};
    std::size_t entered{0};
    void wait_entered() {
        std::unique_lock lock(mutex);
        expect(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered != 0; }),
               "Executor did not reach the synchronous gate");
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }
    std::size_t entries() {
        std::lock_guard lock(mutex);
        return entered;
    }
};
class Gates {
public:
    std::shared_ptr<Gate> get(const std::string& input) {
        std::lock_guard lock(mutex_);
        auto& gate = gates_[input];
        if (!gate) gate = std::make_shared<Gate>();
        return gate;
    }
    ProcessResult execute(const ProcessSpec& spec, std::stop_token token,
                          const std::function<void()>& cancellation_audit = {}) {
        auto gate = get(spec.input);
        std::stop_callback stopped(token, [gate, &cancellation_audit] {
            // Calling back into poll here proves request_stop is invoked outside the registry mutex.
            if (cancellation_audit) cancellation_audit();
            { std::lock_guard lock(gate->mutex); gate->cancelled = true; }
            gate->changed.notify_all();
        });
        {
            std::unique_lock lock(gate->mutex);
            ++gate->entered;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->released || gate->cancelled; });
        }
        ProcessResult result;
        result.termination = token.stop_requested() ? ProcessTermination::cancelled : ProcessTermination::exited;
        if (!token.stop_requested()) { result.exit_code = 0; result.standard_output = spec.input; }
        return result;
    }
private:
    std::mutex mutex_;
    std::map<std::string,std::shared_ptr<Gate>> gates_;
};

BatchRequest request(const std::string& input) {
    BatchRequest result;
    result.source = {"document-" + input, 42, "source-" + input, {}};
    result.toolchain.id = "injected-test";
    result.toolchain.language_id = "test-language";
    result.toolchain.source_filename = "snapshot.txt";
    result.toolchain.artifact_name = "artifact";
    result.toolchain.run = {"injected-executor", {}};
    result.tests = {{"test-" + input, input, input, true}};
    return result;
}
std::uint64_t submit(JobRegistry& registry, const BatchRequest& value) {
    const auto submission = registry.submit(value);
    expect(static_cast<bool>(submission) && submission.job_id != 0, "Submission rejected");
    return submission.job_id;
}
JobEvent terminal(JobRegistry& registry, std::uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        for (const auto& event : registry.poll(512)) if (event.job_id == id && event.result) return event;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Expected terminal event was not published");
}
JobEvent latest_progress(JobRegistry& registry, std::uint64_t id) {
    for (const auto& event : registry.poll(512)) if (event.job_id == id && !event.result) return event;
    throw std::runtime_error("Expected latest progress event");
}
std::vector<std::pair<std::uint64_t,std::uint64_t>> acknowledgements(const std::vector<JobEvent>& events,
                                                                   std::uint64_t only = 0) {
    std::vector<std::pair<std::uint64_t,std::uint64_t>> result;
    for (const auto& event : events) if (!only || event.job_id == only) result.emplace_back(event.job_id, event.sequence);
    return result;
}
void consume_and_release(JobRegistry& registry, std::uint64_t id) {
    (void)terminal(registry, id);
    success(registry.acknowledge(acknowledgements(registry.poll(512), id)), "Acknowledge completed job");
    success(registry.release(id), "Release completed job");
}
void options_and_unknown_ids() {
    for (auto options : {JobRegistry::Options{0, 16}, JobRegistry::Options{1, 0},
                         JobRegistry::Options{JobRegistry::worker_limit + 1, 16},
                         JobRegistry::Options{1, JobRegistry::job_limit + 1}}) {
        bool rejected = false;
        try { JobRegistry registry(options); } catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Invalid options accepted");
    }
    JobRegistry registry;
    expect(registry.cancel(0).code == JobError::unknown_job, "Unknown cancellation accepted");
    expect(registry.release(99).code == JobError::unknown_job, "Unknown release accepted");
    expect(registry.acknowledge({{99, 1}}).code == JobError::unknown_job, "Unknown acknowledgement accepted");
    expect(registry.poll(0).empty(), "Zero-size poll");
    registry.close();
    expect(registry.submit(request("closed")).status.code == JobError::closed, "Closed registry accepted work");
}

void capacity_cancel_and_immutable_request() {
    auto files = std::make_shared<MemoryFiles>();
    Gates gates;
    JobRegistry registry({1, 2}, BatchService(files, [&](const auto& spec, auto token) {
        return gates.execute(spec, token);
    }));
    const auto first = submit(registry, request("first-hold"));
    gates.get("first-hold")->wait_entered();
    const auto queued = submit(registry, request("queued-cancel"));
    expect(registry.submit(request("overflow")).status.code == JobError::capacity_exceeded,
           "Capacity does not include queued work");
    expect(registry.release(first).code == JobError::not_completed, "Running job released");
    success(registry.cancel(queued), "Queued cancellation");
    success(registry.cancel(queued), "Completed cancellation is not idempotent");
    expect(gates.get("queued-cancel")->entries() == 0, "Cancelled queued job executed");
    expect(registry.release(queued).code == JobError::terminal_unacknowledged, "Unacknowledged job released");
    expect(registry.acknowledge({{queued, 2}}).code == JobError::invalid_acknowledgement,
           "Terminal acknowledged before being polled");
    std::set<std::pair<std::uint64_t,std::uint64_t>> observed;
    for (int index = 0; index < 4; ++index) {
        const auto small = registry.poll(1);
        expect(small.size() == 1, "Small poll limit exceeded");
        observed.emplace(small[0].job_id, small[0].sequence);
    }
    expect(observed.size() == 3 && observed.contains({queued, 2}),
           "Unacknowledged progress starved another terminal in small polls");
    const auto stopped = terminal(registry, queued);
    expect(stopped.state == JobState::cancelled && stopped.result->tests[0].verdict == Verdict::cancelled,
           "Queued cancellation result");
    const auto repeated = terminal(registry, queued);
    expect(repeated.sequence == stopped.sequence && repeated.result == stopped.result,
           "Poll destroyed or duplicated the terminal");
    expect(registry.acknowledge({{queued, stopped.sequence}, {9999, 1}}).code == JobError::unknown_job,
           "Unknown acknowledgement list entry accepted");
    expect(terminal(registry, queued).sequence == stopped.sequence, "Failed acknowledgement partially applied");
    expect(registry.submit(request("still-full")).status.code == JobError::capacity_exceeded,
           "Completed un-released result did not consume capacity");
    success(registry.acknowledge({{queued, stopped.sequence}}), "Terminal acknowledgement");
    expect(registry.submit(request("still-full-acked")).status.code == JobError::capacity_exceeded,
           "Acknowledgement prematurely freed job capacity");
    success(registry.release(queued), "Release acknowledged terminal");
    expect(stopped.result->document_id == "document-queued-cancel", "Retained result lost after release");
    expect(registry.cancel(queued).code == JobError::unknown_job, "Released handle remained valid");

    auto immutable = request("original-input");
    const auto next = submit(registry, immutable);
    expect(next > queued && queued > first, "Job IDs reused or not monotonic");
    immutable.source = {"mutated-document", 999, "mutated-source", {}};
    immutable.tests[0].input = "mutated-input";
    immutable.tests[0].expected = "mutated-answer";
    gates.get("first-hold")->release();
    gates.get("original-input")->wait_entered();
    expect(gates.get("mutated-input")->entries() == 0 && files->wrote("source-original-input"),
           "Accepted request was not copied");
    consume_and_release(registry, first);
    gates.get("original-input")->release();
    const auto completed = terminal(registry, next);
    expect(completed.state == JobState::completed && completed.result->document_id == "document-original-input" &&
           completed.result->revision == 42 && completed.result->tests[0].verdict == Verdict::accepted,
           "Immutable source/expected output changed after submission");
    consume_and_release(registry, next);
    expect(files->active() == 0, "Completed batch workspace leaked");
    registry.close();
}

void progress_coalescing_and_stale_ack() {
    auto files = std::make_shared<MemoryFiles>();
    Gates gates;
    JobRegistry registry({1, 2}, BatchService(files, [&](const auto& spec, auto token) {
        return gates.execute(spec, token);
    }));
    auto value = request("progress-first");
    value.tests.push_back({"second-test", "progress-second", std::string{"progress-second"}, true});
    const auto id = submit(registry, value);
    gates.get("progress-first")->wait_entered();
    const auto older = latest_progress(registry, id);
    expect(older.state == JobState::running && older.testcase_id == "test-progress-first", "Initial running phase");
    gates.get("progress-first")->release();
    gates.get("progress-second")->wait_entered();
    const auto latest = latest_progress(registry, id);
    expect(latest.sequence > older.sequence && latest.testcase_id == "second-test", "Latest progress not coalesced");
    success(registry.acknowledge({{id, older.sequence}}), "Stale progress acknowledgement rejected");
    expect(latest_progress(registry, id).sequence == latest.sequence, "Old ack deleted newer progress");
    expect(registry.acknowledge({{id, latest.sequence + 100}}).code == JobError::invalid_acknowledgement,
           "Future event acknowledgement accepted");
    expect(latest_progress(registry, id).sequence == latest.sequence, "Rejected ack changed progress");
    gates.get("progress-second")->release();
    const auto done = terminal(registry, id);
    const auto events = registry.poll(512);
    expect(events.size() <= 2 && std::count_if(events.begin(), events.end(), [](const auto& event) {
        return static_cast<bool>(event.result);
    }) == 1, "Unbounded/duplicate job event retention");
    expect(done.result->tests.size() == 2 && done.result->tests[1].verdict == Verdict::accepted, "Second test result");
    success(registry.cancel(id), "Completed cancellation changed terminal");
    expect(terminal(registry, id).sequence == done.sequence, "Completed cancellation emitted another terminal");
    success(registry.acknowledge(acknowledgements(events)), "Event acknowledgement");
    success(registry.acknowledge({{id, done.sequence}}), "Duplicate acknowledgement not idempotent");
    expect(registry.poll(512).empty(), "Acknowledged events retained");
    success(registry.release(id), "Release acknowledged completed job");
}

void worker_exception_is_terminal_and_worker_survives() {
    auto files = std::make_shared<MemoryFiles>();
    JobRegistry registry({1, 2}, BatchService(files, [](const auto& spec, auto) {
        if (spec.input == "throw-nonstd") throw 123;
        ProcessResult result; result.termination = ProcessTermination::exited;
        result.exit_code = 0; result.standard_output = spec.input; return result;
    }));
    const auto failed = submit(registry, request("throw-nonstd"));
    const auto event = terminal(registry, failed);
    expect(event.state == JobState::failed && !event.result->error.empty() &&
           event.result->tests[0].verdict == Verdict::infrastructure_error, "Worker exception lost its terminal");
    consume_and_release(registry, failed);
    const auto succeeding = submit(registry, request("after-exception"));
    expect(terminal(registry, succeeding).state == JobState::completed, "Worker died after exception");
    consume_and_release(registry, succeeding);
    expect(files->active() == 0, "Exception workspace leaked");
}

void concurrent_close_and_cleanup() {
    auto files = std::make_shared<MemoryFiles>();
    Gates gates;
    JobRegistry* visible = nullptr;
    std::atomic<std::size_t> cancellation_callbacks{0};
    JobRegistry registry({2, 4}, BatchService(files, [&](const auto& spec, auto token) {
        return gates.execute(spec, token, [&] {
            (void)visible->poll(1);
            ++cancellation_callbacks;
        });
    }));
    visible = &registry;
    std::vector<std::uint64_t> ids;
    for (const auto& input : {"close-first", "close-second"}) {
        ids.push_back(submit(registry, request(input)));
        gates.get(input)->wait_entered();
    }
    ids.push_back(submit(registry, request("close-queued-first")));
    ids.push_back(submit(registry, request("close-queued-second")));
    expect(files->active() == 2, "Fixed worker concurrency exceeded or did not start");
    std::barrier start(3);
    std::exception_ptr first_error, second_error;
    std::jthread first([&] { start.arrive_and_wait(); try { registry.close(); } catch (...) { first_error = std::current_exception(); } });
    std::jthread second([&] { start.arrive_and_wait(); try { registry.close(); } catch (...) { second_error = std::current_exception(); } });
    start.arrive_and_wait();
    first.join(); second.join();
    if (first_error) std::rethrow_exception(first_error);
    if (second_error) std::rethrow_exception(second_error);
    expect(cancellation_callbacks == 2, "Running cancellation callbacks not completed during close");
    expect(files->active() == 0, "Close returned before workspace cleanup");
    expect(gates.get("close-queued-first")->entries() == 0 && gates.get("close-queued-second")->entries() == 0,
           "Shutdown started queued jobs");
    expect(registry.submit(request("closed")).status.code == JobError::closed, "Shutdown accepted work");
    registry.close();
    const auto events = registry.poll(512);
    expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return static_cast<bool>(event.result);
    }) == 4, "Close did not retain one terminal for every accepted job");
    for (auto id : ids) {
        const auto done = terminal(registry, id);
        expect(done.state == JobState::cancelled, "Close terminal state");
        expect(registry.release(id).code == JobError::terminal_unacknowledged, "Close implicitly released results");
    }
    success(registry.acknowledge(acknowledgements(events)), "Acknowledge shutdown events");
    for (auto id : ids) success(registry.release(id), "Release shutdown job");
    expect(registry.poll(512).empty(), "Released shutdown events remain");
}

void destructor_and_concurrent_submission() {
    auto files = std::make_shared<MemoryFiles>();
    Gates gates;
    {
        JobRegistry registry({1, 8}, BatchService(files, [&](const auto& spec, auto token) {
            return gates.execute(spec, token);
        }));
        const auto initial = submit(registry, request("destructor-running"));
        gates.get("destructor-running")->wait_entered();
        constexpr std::size_t submissions = 16;
        std::barrier start(static_cast<std::ptrdiff_t>(submissions + 1));
        std::vector<JobSubmission> results(submissions);
        std::vector<std::jthread> submitters;
        for (std::size_t index = 0; index < submissions; ++index) submitters.emplace_back([&, index] {
            start.arrive_and_wait();
            results[index] = registry.submit(request("queued-" + std::to_string(index)));
        });
        start.arrive_and_wait();
        for (auto& thread : submitters) thread.join();
        std::set<std::uint64_t> ids{initial};
        std::size_t rejected = 0;
        for (const auto& result : results) {
            if (result) expect(ids.insert(result.job_id).second, "Concurrent ID allocation reused a handle");
            else { expect(result.status.code == JobError::capacity_exceeded, "Unexpected submit rejection"); ++rejected; }
        }
        expect(ids.size() == 8 && rejected == 9, "Concurrent admission exceeded capacity");
        expect(files->active() == 1, "Queued submissions created extra workers/workspaces");
        // Scope exit cancels the gate and queued jobs; no sleep controls the worker's state.
    }
    expect(files->active() == 0, "Destructor did not wait for workspace cleanup");
    for (std::size_t index = 0; index < 16; ++index)
        expect(gates.get("queued-" + std::to_string(index))->entries() == 0, "Destructor executed queued work");
}
} // namespace

int main() {
    try {
        options_and_unknown_ids();
        capacity_cancel_and_immutable_request();
        progress_coalescing_and_stale_ack();
        worker_exception_is_terminal_and_worker_survives();
        concurrent_close_and_cleanup();
        destructor_and_concurrent_submission();
        std::cout << "job registry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
