// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cpn/companion_inbox.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cpn {
struct CompanionListenerOptions {
    std::uint16_t port{10045}; // Zero explicitly requests an ephemeral port.
    bool enable_ipv6{true};
    std::size_t max_connections{16};
    std::size_t max_admissions{4}; // Includes the currently executing admission.
    std::size_t max_body_bytes{16 * 1024 * 1024};
    std::size_t max_inflight_body_bytes{32 * 1024 * 1024};
    std::size_t max_header_bytes{16384};
    std::size_t max_header_count{64};
    std::size_t max_response_bytes{65536};
    std::size_t socket_send_buffer_bytes{8192}; // OS hint, not a hard kernel-memory guarantee.
    std::chrono::milliseconds read_timeout{5000};
    std::chrono::milliseconds write_timeout{5000};
    std::chrono::milliseconds request_timeout{15000};
    std::chrono::milliseconds lock_timeout{5000};
};
enum class CompanionListenerStartStatus { started, already_started, closed, bind_failed, failed };
struct CompanionListenerStartResult {
    CompanionListenerStartStatus status{CompanionListenerStartStatus::failed};
    std::uint16_t port{0};
    bool ipv6{false};
    int native_error{0};
    std::string detail;
};
struct CompanionListenerStatistics {
    bool running{false}, closed{false}, failed{false};
    std::size_t active_connections{0}, outstanding_admissions{0}, inflight_body_bytes{0};
    std::uint64_t accepted{0}, duplicates{0}, admission_failures{0};
    std::uint64_t connection_rejections{0}, admission_rejections{0};
    std::uint64_t read_timeouts{0}, write_timeouts{0}, request_timeouts{0};
};

// One asynchronous network thread and one bounded, synchronous inbox worker.
// HTTP 202 means the raw payload was durably accepted (or exactly duplicated).
// Requests never create source files, open URLs, execute tools, or invoke UI callbacks.
// Invoke start/stop from a native background coordinator. Stop joins both threads;
// native file I/O already in progress remains subject to the FileStore's guarantees.
// Start and stop reject calls from the listener's own threads before joining locks.
class CompanionListener final {
public:
    CompanionListener(CompanionListenerOptions, std::string workspace_id, CompanionInboxPaths,
        std::shared_ptr<FileStore> = native_file_store(), CompanionInboxBounds = {});
    ~CompanionListener();
    CompanionListener(const CompanionListener&) = delete;
    CompanionListener& operator=(const CompanionListener&) = delete;
    [[nodiscard]] CompanionListenerStartResult start();
    void stop(); // Idempotent; a closed instance cannot be restarted.
    [[nodiscard]] CompanionListenerStatistics statistics() const noexcept;
    [[nodiscard]] std::string failure_detail() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] const char* companion_listener_start_status_name(CompanionListenerStartStatus) noexcept;
} // namespace cpn
