// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/jobs.hpp"
#include "cpn/digest.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace cpn {
namespace {
thread_local const void* current_registry_worker = nullptr;
struct WorkerContext {
    explicit WorkerContext(const void* registry) : previous(current_registry_worker) {
        current_registry_worker = registry;
    }
    ~WorkerContext() { current_registry_worker = previous; }
    const void* previous;
};
JobStatus error(JobError code, const char* message) { return {code, message}; }
std::string bounded_failure_detail(const char* detail) {
    constexpr std::size_t maximum = 4096;
    std::size_t count = 0;
    while (count <= maximum && detail[count] != '\0') ++count;
    auto end = std::min(count, maximum);
    if (count > maximum) {
        while (end && (static_cast<unsigned char>(detail[end]) & 0xc0) == 0x80) --end;
    }
    return std::string(detail, end);
}
JobState terminal_state(const BatchResult& result) {
    if (result.build_outcome == BuildOutcome::cancelled ||
        std::any_of(result.tests.begin(), result.tests.end(), [](const auto& test) {
            return test.verdict == Verdict::cancelled;
        })) return JobState::cancelled;
    if (result.build_outcome == BuildOutcome::failed || !result.error.empty()) return JobState::failed;
    return JobState::completed;
}
JobState progress_state(BatchPhase phase) {
    switch (phase) {
        case BatchPhase::building: return JobState::building;
        case BatchPhase::running: return JobState::running;
        case BatchPhase::checking: return JobState::checking;
    }
    throw std::invalid_argument("Unknown batch progress phase");
}
}

struct JobRegistry::Impl {
    struct Job {
        explicit Job(const BatchRequest& source) : request(source), emergency(std::make_shared<BatchResult>()) {
            emergency->document_id = request.source.document_id;
            emergency->revision = request.source.revision;
            emergency->source_digest = sha256(request.source.text);
            emergency->profile_id = request.toolchain.id;
            emergency->build_outcome = request.mode == BatchMode::run_only ? BuildOutcome::not_required : BuildOutcome::failed;
            emergency->error = "Job worker failed before completing the batch";
            if (request.mode != BatchMode::compile_only) {
                for (const auto& test : request.tests) {
                    if (test.enabled) emergency->tests.push_back(
                        TestResult{test.id, Verdict::infrastructure_error, {}, {}});
                }
            }
        }
        const BatchRequest request;
        std::stop_source cancellation;
        std::shared_ptr<BatchResult> emergency;
        std::uint64_t id{0};
        std::uint64_t sequence{0};
        bool running{false};
        bool completed{false};
        bool terminal_delivered{false};
        bool terminal_acknowledged{false};
        std::optional<JobEvent> progress;
        std::optional<JobEvent> terminal;
    };

    explicit Impl(Options configuration, BatchService batch)
        : options(configuration), service(std::move(batch)) {
        if (!options.max_workers || options.max_workers > worker_limit ||
            !options.max_jobs || options.max_jobs > job_limit) {
            throw std::invalid_argument("JobRegistry worker/job limits are outside supported bounds");
        }
        queue.reserve(options.max_jobs);
        workers.reserve(options.max_workers);
        try {
            for (std::size_t index = 0; index < options.max_workers; ++index) {
                workers.emplace_back([this] { worker(); });
            }
        } catch (...) {
            { std::lock_guard lock(mutex); closing = true; }
            available.notify_all();
            for (auto& thread : workers) if (thread.joinable()) thread.join();
            throw;
        }
    }

    JobSubmission submit(const BatchRequest& request) {
        {
            std::lock_guard lock(mutex);
            if (closing) return {error(JobError::closed, "Job registry is closed"), 0};
            if (jobs.size() + constructing >= options.max_jobs)
                return {error(JobError::capacity_exceeded, "Job registry capacity is exhausted"), 0};
            if (ids_exhausted) return {error(JobError::id_exhausted, "Job ID space is exhausted"), 0};
            ++constructing;
        }
        // Large request copies, digest calculation, and terminal fallback allocation happen unlocked.
        // A reserved admission slot bounds concurrent copies even before a job is accepted.
        std::shared_ptr<Job> job;
        try { job = std::make_shared<Job>(request); }
        catch (...) {
            { std::lock_guard lock(mutex); --constructing; }
            admissions.notify_all();
            throw;
        }
        std::lock_guard lock(mutex);
        --constructing;
        admissions.notify_all();
        if (closing) return {error(JobError::closed, "Job registry is closed"), 0};
        if (ids_exhausted) return {error(JobError::id_exhausted, "Job ID space is exhausted"), 0};
        job->id = next_id;
        job->sequence = 1;
        job->progress = JobEvent{job->id, job->sequence, JobState::queued, {}, {}};
        // queue was reserved to max_jobs; a map allocation failure precedes acceptance.
        jobs.emplace(job->id, job);
        queue.push_back(job);
        if (next_id == std::numeric_limits<std::uint64_t>::max()) ids_exhausted = true;
        else ++next_id;
        available.notify_one();
        return {{}, job->id};
    }

    void finish(const std::shared_ptr<Job>& job, JobState state,
                std::shared_ptr<const BatchResult> result) {
        // Caller owns mutex; result is fully constructed before becoming externally visible.
        if (job->completed) return;
        job->terminal = JobEvent{job->id, ++job->sequence, state, {}, std::move(result)};
        job->running = false;
        job->completed = true;
    }

    void finish_queued_cancel(const std::shared_ptr<Job>& job) {
        job->emergency->build_outcome = BuildOutcome::cancelled;
        job->emergency->error.clear();
        for (auto& test : job->emergency->tests) test.verdict = Verdict::cancelled;
        finish(job, JobState::cancelled, job->emergency);
    }

    JobStatus cancel(std::uint64_t id) {
        std::optional<std::stop_source> cancellation;
        {
            std::lock_guard lock(mutex);
            auto found = jobs.find(id);
            if (found == jobs.end()) return error(JobError::unknown_job, "Unknown job ID");
            const auto& job = found->second;
            if (job->completed) return {};
            cancellation.emplace(job->cancellation);
            if (!job->running) {
                queue.erase(std::remove(queue.begin(), queue.end(), job), queue.end());
                finish_queued_cancel(job);
            }
        }
        // request_stop may synchronously invoke an injected executor's callbacks.
        cancellation->request_stop();
        return {};
    }

    std::vector<JobEvent> poll(std::size_t max_events) {
        std::vector<JobEvent> result;
        if (!max_events) return result;
        std::lock_guard lock(mutex);
        struct Entry { std::pair<std::uint64_t,bool> key; Job* job; };
        std::array<Entry, job_limit * 2> entries{};
        std::size_t count = 0;
        for (const auto& [id, job] : jobs) {
            if (job->progress) entries[count++] = {{id, false}, job.get()};
            if (job->terminal) entries[count++] = {{id, true}, job.get()};
        }
        if (!count) return result;
        const auto end = entries.begin() + static_cast<std::ptrdiff_t>(count);
        auto start = entries.begin();
        if (poll_cursor) start = std::upper_bound(entries.begin(), end, *poll_cursor,
            [](const auto& key, const Entry& entry) { return key < entry.key; });
        if (start == end) start = entries.begin();
        const auto offset = static_cast<std::size_t>(start - entries.begin());
        const auto selected = std::min(max_events, count);
        result.reserve(selected);
        for (std::size_t index = 0; index < selected; ++index) {
            const auto& entry = entries[(offset + index) % count];
            result.push_back(entry.key.second ? *entry.job->terminal : *entry.job->progress);
        }
        // Rotate by stable event slot, not a changing progress sequence: hot jobs cannot starve others.
        // Cursor mutation never deletes an event. Sorting preserves per-job sequence order at wraparound.
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return std::pair{left.job_id, left.sequence} < std::pair{right.job_id, right.sequence};
        });
        for (const auto& event : result) if (event.result) jobs.at(event.job_id)->terminal_delivered = true;
        poll_cursor = entries[(offset + selected - 1) % count].key;
        return result;
    }

    JobStatus acknowledge(const std::vector<std::pair<std::uint64_t,std::uint64_t>>& events) {
        std::lock_guard lock(mutex);
        // Validate the whole request before changing any event retention.
        for (const auto& [id, sequence] : events) {
            const auto found = jobs.find(id);
            if (found == jobs.end()) return error(JobError::unknown_job, "Unknown job ID");
            const auto& job = found->second;
            if (!sequence || sequence > job->sequence)
                return error(JobError::invalid_acknowledgement, "Unknown event sequence");
            if (job->terminal && job->terminal->sequence == sequence && !job->terminal_delivered)
                return error(JobError::invalid_acknowledgement, "Terminal event has not been polled");
        }
        for (const auto& [id, sequence] : events) {
            auto& job = jobs.at(id);
            if (job->progress && job->progress->sequence == sequence) job->progress.reset();
            if (job->terminal && job->terminal->sequence == sequence) {
                job->terminal.reset();
                job->terminal_acknowledged = true;
            }
        }
        return {};
    }

    JobStatus release(std::uint64_t id) {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end()) return error(JobError::unknown_job, "Unknown job ID");
        if (!found->second->completed) return error(JobError::not_completed, "Job is still active");
        if (!found->second->terminal_acknowledged)
            return error(JobError::terminal_unacknowledged, "Job terminal event must be acknowledged");
        jobs.erase(found);
        return {};
    }

    void progress(const std::shared_ptr<Job>& job, BatchPhase phase, const std::string& testcase) {
        JobEvent event{job->id, 0, progress_state(phase), testcase, {}};
        std::lock_guard lock(mutex);
        if (job->completed) return;
        // Reserve the final sequence value for the one terminal event.
        if (job->sequence >= std::numeric_limits<std::uint64_t>::max() - 1)
            throw std::overflow_error("Job event sequence space is exhausted");
        event.sequence = ++job->sequence;
        job->progress = std::move(event);
    }

    void worker() noexcept {
        WorkerContext context(this);
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [this] { return closing || !queue.empty(); });
                if (queue.empty()) return;
                job = std::move(queue.front());
                queue.erase(queue.begin());
                job->running = true;
            }
            std::shared_ptr<const BatchResult> result;
            JobState state = JobState::failed;
            try {
                auto batch = service.run(job->request, job->cancellation.get_token(),
                    [this, job](BatchPhase phase, const std::string& testcase) {
                        progress(job, phase, testcase);
                    });
                state = terminal_state(batch);
                result = std::make_shared<const BatchResult>(std::move(batch));
            } catch (const std::exception& failure) {
                state = JobState::failed;
                // The preallocated fallback still yields a terminal if result allocation fails.
                try { job->emergency->error = bounded_failure_detail(failure.what()); } catch (...) {}
                result = job->emergency;
            } catch (...) {
                state = JobState::failed;
                result = job->emergency;
            }
            {
                std::lock_guard lock(mutex);
                finish(job, state, std::move(result));
            }
        }
    }

    void close() {
        // Reject synchronous self-join before mutating the shutdown state.
        if (current_registry_worker == this)
            throw std::logic_error("JobRegistry::close cannot be called from its worker");
        std::array<std::optional<std::stop_source>, job_limit> cancellations;
        std::size_t cancellation_count = 0;
        {
            std::lock_guard lock(mutex);
            closing = true;
            for (auto& [id, job] : jobs) {
                (void)id;
                if (!job->completed) {
                    cancellations[cancellation_count++].emplace(job->cancellation);
                    if (!job->running) finish_queued_cancel(job);
                }
            }
            queue.clear();
        }
        available.notify_all();
        for (std::size_t index = 0; index < cancellation_count; ++index)
            cancellations[index]->request_stop();
        std::lock_guard join_lock(join_mutex);
        for (auto& thread : workers) if (thread.joinable()) thread.join();
        std::unique_lock lock(mutex);
        admissions.wait(lock, [this] { return constructing == 0; });
    }

    const Options options;
    const BatchService service;
    std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable available;
    std::condition_variable admissions;
    std::map<std::uint64_t,std::shared_ptr<Job>> jobs;
    std::vector<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    std::uint64_t next_id{1};
    std::size_t constructing{0};
    std::optional<std::pair<std::uint64_t,bool>> poll_cursor;
    bool ids_exhausted{false};
    bool closing{false};
};

JobRegistry::JobRegistry() : JobRegistry(Options{}, BatchService{}) {}
JobRegistry::JobRegistry(Options options, BatchService service)
    : impl_(std::make_unique<Impl>(options, std::move(service))) {}
JobRegistry::~JobRegistry() noexcept {
    try { impl_->close(); }
    catch (...) { std::terminate(); } // An active worker cannot safely outlive its registry.
}
JobSubmission JobRegistry::submit(const BatchRequest& request) { return impl_->submit(request); }
JobStatus JobRegistry::cancel(std::uint64_t id) { return impl_->cancel(id); }
std::vector<JobEvent> JobRegistry::poll(std::size_t count) { return impl_->poll(count); }
JobStatus JobRegistry::acknowledge(const std::vector<std::pair<std::uint64_t,std::uint64_t>>& events) {
    return impl_->acknowledge(events);
}
JobStatus JobRegistry::release(std::uint64_t id) { return impl_->release(id); }
void JobRegistry::close() { impl_->close(); }
const char* job_state_name(JobState state) noexcept {
    constexpr const char* names[]{"Queued", "Building", "Running", "Checking", "Completed", "Cancelled", "Failed"};
    const auto index = static_cast<unsigned>(state);
    return index < std::size(names) ? names[index] : "Unknown";
}
const char* job_error_name(JobError code) noexcept {
    constexpr const char* names[]{"None", "Closed", "CapacityExceeded", "UnknownJob", "NotCompleted",
        "TerminalUnacknowledged", "InvalidAcknowledgement", "IdExhausted"};
    const auto index = static_cast<unsigned>(code);
    return index < std::size(names) ? names[index] : "Unknown";
}
} // namespace cpn
