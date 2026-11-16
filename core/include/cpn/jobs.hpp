// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/batch.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cpn {
enum class JobState { queued, building, running, checking, completed, cancelled, failed };
enum class JobError { none, closed, capacity_exceeded, unknown_job, not_completed,
                      terminal_unacknowledged, invalid_acknowledgement, id_exhausted };
struct JobStatus {
    JobError code{JobError::none};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return code == JobError::none; }
};
struct JobSubmission {
    JobStatus status;
    std::uint64_t job_id{0};
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(status); }
};
struct JobEvent {
    std::uint64_t job_id{0};
    std::uint64_t sequence{0};
    JobState state{JobState::queued};
    std::string testcase_id;
    // Present only on a terminal event. The immutable result survives registry release.
    std::shared_ptr<const BatchResult> result;
};

// Public methods are thread-safe; destruction requires callers to stop accessing this object.
// Injected BatchService ports must support concurrent execution for max_workers > 1.
// close() is blocking and must not be invoked from an injected service port/cancellation callback.
// Destroying this registry from its own worker violates its lifetime contract and terminates.
class JobRegistry final {
public:
    struct Options {
        std::size_t max_workers{2};
        // Includes admission copies, queued/running jobs, and completed jobs until release().
        std::size_t max_jobs{16};
    };
    static constexpr std::size_t worker_limit = 32;
    static constexpr std::size_t job_limit = 256;
    JobRegistry();
    explicit JobRegistry(Options, BatchService service = BatchService{});
    ~JobRegistry() noexcept;
    JobRegistry(const JobRegistry&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;
    JobRegistry(JobRegistry&&) = delete;
    JobRegistry& operator=(JobRegistry&&) = delete;

    // Reserves capacity before copying; close before acceptance rejects and rolls back the slot.
    // Copies the request before acceptance. Successful IDs are monotonic and never reused.
    [[nodiscard]] JobSubmission submit(const BatchRequest&);
    [[nodiscard]] JobStatus cancel(std::uint64_t job_id);
    // Non-destructive. At most one latest progress and one retained terminal per job.
    // Events within each job are returned in sequence order; acknowledge after consumption.
    // Repeated small polls rotate across stable event slots even when consumers have not acked.
    [[nodiscard]] std::vector<JobEvent> poll(std::size_t max_events);
    // Stale acknowledgements are harmless. Unknown IDs/future sequences reject atomically.
    // A terminal must have appeared in poll() before it can be acknowledged.
    [[nodiscard]] JobStatus acknowledge(const std::vector<std::pair<std::uint64_t,std::uint64_t>>&);
    [[nodiscard]] JobStatus release(std::uint64_t job_id);
    // Cancels queued/running jobs, joins fixed workers, drains admission copies, and retains terminals.
    // Repeated/concurrent close calls are supported. Poll/ack/release remain available.
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] const char* job_state_name(JobState) noexcept;
[[nodiscard]] const char* job_error_name(JobError) noexcept;
} // namespace cpn
