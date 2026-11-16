// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion_listener.hpp"
#include "cpn/text.hpp"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cpn {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;
namespace field {
constexpr std::string_view json_type = "application/json";
constexpr std::string_view path = "/";
constexpr std::string_view schema = "schema_version", status = "status", import_id = "import_id";
constexpr std::string_view digest = "disk_digest", main_replaced = "inbox_replaced", backup_replaced = "backup_replaced", detail = "detail";
}
template<class T> void count(std::atomic<T>& value) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<T>::max() &&
           !value.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {}
}
void validate_options(const CompanionListenerOptions& options, const CompanionInboxBounds& bounds) {
    const auto duration = [](auto value, auto maximum, bool zero = false) {
        return value.count() >= (zero ? 0 : 1) && value <= maximum;
    };
    using namespace std::chrono_literals;
    if (!options.max_connections || options.max_connections > 128 || !options.max_admissions || options.max_admissions > 64 ||
        !options.max_body_bytes || options.max_body_bytes > 64 * 1024 * 1024 || options.max_body_bytes > bounds.payload.max_json_bytes ||
        !options.max_inflight_body_bytes || options.max_inflight_body_bytes > 128 * 1024 * 1024 ||
        !options.max_header_bytes || options.max_header_bytes > 65536 || !options.max_header_count || options.max_header_count > 256 ||
        options.max_response_bytes < 32768 || options.max_response_bytes > 65536 ||
        options.socket_send_buffer_bytes < 1024 || options.socket_send_buffer_bytes > 128 * 1024 ||
        !duration(options.read_timeout, 60s) || !duration(options.write_timeout, 60s) ||
        !duration(options.request_timeout, 300s) || !duration(options.lock_timeout, 60s, true))
        throw std::invalid_argument("Companion listener options exceed supported bounds");
}
std::string_view ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto a = static_cast<unsigned char>(left[i]), b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}
bool json_content_type(std::string_view value) {
    value = ows(value);
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) return ascii_equal(value, field::json_type);
    if (!ascii_equal(ows(value.substr(0, semicolon)), field::json_type)) return false;
    auto parameter = ows(value.substr(semicolon + 1));
    const auto equal = parameter.find('=');
    if (equal == std::string_view::npos || !ascii_equal(ows(parameter.substr(0, equal)), "charset")) return false;
    auto charset = ows(parameter.substr(equal + 1));
    if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"') charset = charset.substr(1, charset.size() - 2);
    return ascii_equal(charset, "utf-8");
}
// This encodes diagnostics only. Accepted payload bytes are never rewritten.
void quoted(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (std::size_t i = 0; i < value.size();) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte < 0x20) {
            output += "\\u00"; output.push_back(hex[byte >> 4]); output.push_back(hex[byte & 15]); ++i;
        } else if (byte == '"' || byte == '\\') { output.push_back('\\'); output.push_back(static_cast<char>(byte)); ++i; }
        else if (byte < 0x80) { output.push_back(static_cast<char>(byte)); ++i; }
        else {
            std::size_t length = byte >= 0xc2 && byte <= 0xdf ? 2 : byte >= 0xe0 && byte <= 0xef ? 3 : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
            if (length && length <= value.size() - i && !decode_utf8(value.substr(i, length)).error_offset) {
                output.append(value.substr(i, length)); i += length;
            } else { output += "\xef\xbf\xbd"; ++i; }
        }
    }
    output.push_back('"');
}
void key(std::string& output, std::string_view value, bool comma = true) {
    if (comma) output.push_back(','); quoted(output, value); output.push_back(':');
}
struct AdmissionResult {
    CompanionInboxWriteStatus status{CompanionInboxWriteStatus::io_failed};
    std::optional<std::string> import_id, disk_digest;
    bool inbox_replaced{false}, backup_replaced{false};
    std::string detail;
};
http::status response_status(CompanionInboxWriteStatus status) {
    using S = CompanionInboxWriteStatus;
    switch (status) {
    case S::accepted: case S::duplicate: return http::status::accepted;
    case S::invalid_payload: return http::status::bad_request;
    case S::capacity_exceeded: return http::status::insufficient_storage;
    case S::invalid_existing: case S::unsupported_schema: case S::identity_mismatch:
    case S::batch_conflict: case S::storage_conflict: case S::id_exhausted: return http::status::conflict;
    default: return http::status::service_unavailable;
    }
}
struct BodyBudget {
    std::atomic<std::size_t> used{0};
    const std::size_t maximum;
    explicit BodyBudget(std::size_t maximum) : maximum(maximum) {}
};
struct BodyLease {
    std::shared_ptr<BodyBudget> budget;
    std::size_t bytes;
    BodyLease(std::shared_ptr<BodyBudget> budget, std::size_t bytes) : budget(std::move(budget)), bytes(bytes) {}
    BodyLease(const BodyLease&) = delete;
    BodyLease& operator=(const BodyLease&) = delete;
    ~BodyLease() { budget->used.fetch_sub(bytes, std::memory_order_relaxed); }
};
std::shared_ptr<BodyLease> reserve_body(const std::shared_ptr<BodyBudget>& budget, std::size_t bytes) {
    auto current = budget->used.load(std::memory_order_relaxed);
    do { if (bytes > budget->maximum - current) return {}; }
    while (!budget->used.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
    try { return std::make_shared<BodyLease>(budget, bytes); }
    catch (...) { budget->used.fetch_sub(bytes, std::memory_order_relaxed); throw; }
}
struct State;
class RequestParser final : public http::request_parser<http::string_body> {
    // Beast handles framing. This application does not accept a trailer section.
    void on_trailer_field_impl(http::field, beast::string_view, beast::string_view, Error& error) override {
        error = http::error::bad_field;
    }
};
struct Connection : std::enable_shared_from_this<Connection> {
    Connection(std::shared_ptr<State>, std::uint64_t, tcp::socket);
    void start();
    void header(Error, std::size_t);
    void body(Error, std::size_t);
    void reply(http::status, std::string_view status, std::string_view detail = {});
    void admitted(AdmissionResult);
    void write();
    void finish();
    void arm_read();
    enum class Stage { reading, admission, writing };
    void arm_stage(std::chrono::milliseconds, Stage);
    std::weak_ptr<State> owner;
    std::uint64_t id;
    tcp::socket socket;
    beast::flat_buffer buffer;
    RequestParser parser;
    http::response<http::string_body> response;
    net::steady_timer stage_timer, request_timer;
    std::uint64_t stage_generation{0};
    std::size_t header_count{0};
    std::stop_source admission_stop;
    std::shared_ptr<BodyLease> lease;
    bool finished{false};
};
struct State : std::enable_shared_from_this<State> {
    struct Command {
        std::weak_ptr<Connection> connection;
        std::stop_source cancellation;
        std::shared_ptr<BodyLease> lease;
        std::string body;
    };
    CompanionListenerOptions options;
    CompanionInboxPaths paths;
    CompanionInboxService inbox;
    net::io_context io;
    std::optional<net::executor_work_guard<net::io_context::executor_type>> guard;
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::map<std::uint64_t, std::shared_ptr<Connection>> connections;
    std::shared_ptr<BodyBudget> body_budget;
    std::mutex commands_mutex, thread_ids_mutex;
    mutable std::mutex failure_mutex;
    std::string failure_detail;
    std::condition_variable changed;
    std::deque<Command> commands;
    std::stop_source operations;
    std::thread::id io_id, worker_id;
    std::atomic<bool> running{false}, closing{false}, failed{false}, worker_finished{false};
    std::atomic<std::size_t> active_connections{0}, outstanding_admissions{0};
    std::atomic<std::uint64_t> accepted{0}, duplicates{0}, admission_failures{0}, connection_rejections{0}, admission_rejections{0};
    std::atomic<std::uint64_t> read_timeouts{0}, write_timeouts{0}, request_timeouts{0};
    std::uint64_t next_connection{1};
    std::uint16_t port{0};
    bool ipv6{false};
    State(CompanionListenerOptions options, std::string workspace, CompanionInboxPaths paths,
        std::shared_ptr<FileStore> files, CompanionInboxBounds bounds)
        : options(options), paths(std::move(paths)), inbox(std::move(workspace), std::move(files), bounds),
          body_budget(std::make_shared<BodyBudget>(options.max_inflight_body_bytes)) {}
    void accept(tcp::acceptor& acceptor) {
        if (closing.load(std::memory_order_acquire)) return;
        acceptor.async_accept([weak = weak_from_this(), &acceptor](Error error, tcp::socket socket) {
            const auto self = weak.lock(); if (!self) return;
            if (self->closing.load(std::memory_order_acquire)) return;
            if (!error) {
                if (self->connections.size() >= self->options.max_connections || self->next_connection == std::numeric_limits<std::uint64_t>::max()) {
                    count(self->connection_rejections); Error ignored; socket.close(ignored);
                } else {
                    const auto id = self->next_connection++;
                    auto connection = std::make_shared<Connection>(self, id, std::move(socket));
                    self->connections.emplace(id, connection); self->active_connections.store(self->connections.size()); connection->start();
                }
            } else if (error != net::error::operation_aborted) {
                count(self->connection_rejections);
                if (error != net::error::connection_aborted) {
                    self->failed.store(true);
                    { std::lock_guard lock(self->failure_mutex); self->failure_detail = error.message().substr(0, 4096); }
                    self->closing.store(true); self->operations.request_stop(); self->changed.notify_all(); self->io.stop(); return;
                }
            }
            if (!self->closing.load(std::memory_order_acquire)) self->accept(acceptor);
        });
    }
    bool enqueue(const std::shared_ptr<Connection>& connection, std::string body, std::shared_ptr<BodyLease> lease) {
        std::lock_guard lock(commands_mutex);
        if (closing.load(std::memory_order_acquire) || outstanding_admissions.load() >= options.max_admissions) return false;
        commands.push_back({connection, connection->admission_stop, std::move(lease), std::move(body)});
        outstanding_admissions.fetch_add(1); changed.notify_one(); return true;
    }
    void network() noexcept {
        { std::lock_guard lock(thread_ids_mutex); io_id = std::this_thread::get_id(); }
        try { io.run(); }
        catch (const std::exception& error) {
            failed.store(true);
            try { std::lock_guard lock(failure_mutex); failure_detail = std::string(error.what()).substr(0, 4096); } catch (...) {}
            closing.store(true); operations.request_stop(); changed.notify_all();
        } catch (...) { failed.store(true); closing.store(true); operations.request_stop(); changed.notify_all(); }
        running.store(false);
        for (auto& acceptor : acceptors) { Error ignored; acceptor->cancel(ignored); acceptor->close(ignored); }
        while (!connections.empty()) { auto connection = connections.begin()->second; connection->finish(); }
        guard.reset();
        // No worker may post a result after the final cancellation completions are drained.
        { std::unique_lock lock(commands_mutex); changed.wait(lock, [&] { return worker_finished.load(); }); }
        io.restart();
        try { while (io.poll() != 0) {} } catch (...) {}
    }
    void worker() noexcept {
        { std::lock_guard lock(thread_ids_mutex); worker_id = std::this_thread::get_id(); }
        while (true) {
            Command command;
            {
                std::unique_lock lock(commands_mutex);
                changed.wait(lock, [&] { return closing.load(std::memory_order_acquire) || !commands.empty(); });
                if (closing.load(std::memory_order_acquire)) {
                    outstanding_admissions.fetch_sub(commands.size()); commands.clear(); worker_finished.store(true); changed.notify_all(); return;
                }
                command = std::move(commands.front()); commands.pop_front();
            }
            AdmissionResult result;
            try {
                std::stop_callback stop(operations.get_token(), [cancel = command.cancellation]() mutable { cancel.request_stop(); });
                auto accepted_result = inbox.accept(paths, command.body, options.lock_timeout, command.cancellation.get_token());
                result.status = accepted_result.status; result.inbox_replaced = accepted_result.inbox_replaced;
                result.backup_replaced = accepted_result.backup_replaced; result.disk_digest = std::move(accepted_result.disk_digest);
                if (accepted_result.item) result.import_id = std::move(accepted_result.item->import_id);
                result.detail = std::move(accepted_result.detail);
            } catch (const std::exception& error) { try { result.detail = std::string(error.what()).substr(0, 4096); } catch (...) {} }
            catch (...) {}
            if (result.status == CompanionInboxWriteStatus::accepted) count(accepted);
            else if (result.status == CompanionInboxWriteStatus::duplicate) count(duplicates);
            else count(admission_failures);
            outstanding_admissions.fetch_sub(1);
            if (!closing.load(std::memory_order_acquire)) {
                try { net::post(io, [connection = command.connection, result = std::move(result)]() mutable {
                    if (auto target = connection.lock()) target->admitted(std::move(result));
                }); } catch (...) { count(admission_failures); }
            }
        }
    }
};
Connection::Connection(std::shared_ptr<State> state, std::uint64_t id, tcp::socket socket)
    : owner(state), id(id), socket(std::move(socket)), buffer(state->options.max_header_bytes + 8192),
      stage_timer(state->io), request_timer(state->io) {
    this->socket.set_option(net::socket_base::send_buffer_size(static_cast<int>(state->options.socket_send_buffer_bytes)));
    parser.header_limit(static_cast<std::uint32_t>(state->options.max_header_bytes)); parser.body_limit(state->options.max_body_bytes);
    response.version(11); response.keep_alive(false); response.set(http::field::content_type, field::json_type);
    response.body().reserve(state->options.max_response_bytes);
}
void Connection::arm_stage(std::chrono::milliseconds timeout, Stage stage) {
    const auto generation = ++stage_generation;
    stage_timer.expires_after(timeout);
    stage_timer.async_wait([self = shared_from_this(), generation, stage](Error error) {
        if (error || self->finished || self->stage_generation != generation) return;
        if (auto state = self->owner.lock()) {
            if (stage == Stage::reading) count(state->read_timeouts);
            else if (stage == Stage::writing) count(state->write_timeouts);
        }
        self->finish();
    });
}
void Connection::arm_read() { if (auto state = owner.lock()) arm_stage(state->options.read_timeout, Stage::reading); }
void Connection::start() {
    const auto state = owner.lock(); if (!state || state->closing.load()) { finish(); return; }
    request_timer.expires_after(state->options.request_timeout);
    request_timer.async_wait([self = shared_from_this()](Error error) {
        if (error || self->finished) return;
        if (auto state = self->owner.lock()) count(state->request_timeouts);
        self->finish();
    });
    arm_read(); http::async_read_header(socket, buffer, parser, [self = shared_from_this()](Error error, std::size_t bytes) { self->header(error, bytes); });
}
void Connection::header(Error error, std::size_t) {
    if (finished) return;
    const auto state = owner.lock(); if (!state || state->closing.load()) { finish(); return; }
    if (error) {
        if (error == http::error::header_limit) reply(http::status::request_header_fields_too_large, "HeaderLimit");
        else if (error == http::error::body_limit) reply(http::status::payload_too_large, "BodyLimit");
        else if (error != net::error::operation_aborted) reply(http::status::bad_request, "InvalidHTTP");
        else finish();
        return;
    }
    const auto& request = parser.get();
    if (request.version() != 11) { reply(http::status::http_version_not_supported, "UnsupportedHTTPVersion"); return; }
    if (request.method() != http::verb::post) { reply(http::status::method_not_allowed, "UnsupportedMethod"); return; }
    if (request.target() != field::path) { reply(http::status::not_found, "UnsupportedPath"); return; }
    const auto number_headers = static_cast<std::size_t>(std::distance(request.begin(), request.end()));
    header_count = number_headers;
    if (number_headers > state->options.max_header_count) { reply(http::status::request_header_fields_too_large, "HeaderCount"); return; }
    if (request.count(http::field::host) != 1) { reply(http::status::bad_request, "InvalidHost"); return; }
    const auto host = ows(request[http::field::host]);
    const auto suffix = ":" + std::to_string(state->port);
    const bool valid_host = ascii_equal(host, "localhost" + suffix) || host == "127.0.0.1" + suffix || host == "[::1]" + suffix ||
        (state->port == 80 && (ascii_equal(host, "localhost") || host == "127.0.0.1" || host == "[::1]"));
    if (!valid_host) { reply(http::status::bad_request, "InvalidHost"); return; }
    if (request.count(http::field::content_type) != 1 || !json_content_type(request[http::field::content_type])) {
        reply(http::status::unsupported_media_type, "UnsupportedContentType"); return;
    }
    if (request.count(http::field::content_encoding) > 1 ||
        (request.count(http::field::content_encoding) && !ascii_equal(ows(request[http::field::content_encoding]), "identity"))) {
        reply(http::status::unsupported_media_type, "UnsupportedContentEncoding"); return;
    }
    if (request.count(http::field::transfer_encoding) > 1 ||
        (request.count(http::field::transfer_encoding) && !ascii_equal(ows(request[http::field::transfer_encoding]), "chunked"))) {
        reply(http::status::bad_request, "UnsupportedTransferEncoding"); return;
    }
    if (request.count(http::field::expect)) { reply(http::status::expectation_failed, "UnsupportedExpectation"); return; }
    if (request.count(http::field::trailer)) { reply(http::status::bad_request, "UnsupportedTrailers"); return; }
    const auto length = parser.content_length();
    const auto reserved_bytes = length ? static_cast<std::size_t>(*length) : state->options.max_body_bytes;
    lease = reserve_body(state->body_budget, reserved_bytes);
    if (!lease) { count(state->admission_rejections); reply(http::status::service_unavailable, "BodyCapacity"); return; }
    arm_read(); http::async_read(socket, buffer, parser, [self = shared_from_this()](Error error, std::size_t bytes) { self->body(error, bytes); });
}
void Connection::body(Error error, std::size_t) {
    if (finished) return;
    const auto state = owner.lock(); if (!state || state->closing.load()) { finish(); return; }
    if (error) {
        if (error == http::error::body_limit) reply(http::status::payload_too_large, "BodyLimit");
        else if (error != net::error::operation_aborted) reply(http::status::bad_request, "InvalidHTTPBody");
        else finish();
        return;
    }
    if (static_cast<std::size_t>(std::distance(parser.get().begin(), parser.get().end())) != header_count) {
        reply(http::status::bad_request, "UnsupportedTrailers"); return;
    }
    stage_timer.cancel(); ++stage_generation;
    if (!state->enqueue(shared_from_this(), std::move(parser.get().body()), std::move(lease))) {
        count(state->admission_rejections); reply(http::status::service_unavailable, "AdmissionCapacity");
    }
}
void Connection::reply(http::status status, std::string_view name, std::string_view message) {
    if (finished) return;
    if (status == http::status::method_not_allowed) response.set(http::field::allow, "POST");
    response.result(status); auto& body = response.body(); body.clear(); body.push_back('{');
    key(body, field::schema, false); body.push_back('1'); key(body, field::status); quoted(body, name);
    key(body, field::detail); quoted(body, message.substr(0, 4096)); body.push_back('}'); write();
}
void Connection::admitted(AdmissionResult result) {
    if (finished) return;
    if (auto state = owner.lock(); !state || state->closing.load()) { finish(); return; }
    response.result(response_status(result.status)); auto& body = response.body(); body.clear(); body.push_back('{');
    key(body, field::schema, false); body.push_back('1'); key(body, field::status); quoted(body, companion_inbox_write_status_name(result.status));
    key(body, field::import_id); if (result.import_id) quoted(body, *result.import_id); else body += "null";
    key(body, field::digest); if (result.disk_digest) quoted(body, *result.disk_digest); else body += "null";
    key(body, field::main_replaced); body += result.inbox_replaced ? "true" : "false";
    key(body, field::backup_replaced); body += result.backup_replaced ? "true" : "false";
    key(body, field::detail); quoted(body, result.detail.substr(0, 4096)); body.push_back('}'); write();
}
void Connection::write() {
    const auto state = owner.lock(); if (!state || state->closing.load()) { finish(); return; }
    if (response.body().size() > state->options.max_response_bytes) { finish(); return; }
    response.prepare_payload(); arm_stage(state->options.write_timeout, Stage::writing);
    http::async_write(socket, response, [self = shared_from_this()](Error, std::size_t) { self->finish(); });
}
void Connection::finish() {
    if (std::exchange(finished, true)) return;
    admission_stop.request_stop(); ++stage_generation; stage_timer.cancel(); request_timer.cancel();
    Error ignored; socket.cancel(ignored); socket.shutdown(tcp::socket::shutdown_both, ignored); socket.close(ignored);
    lease.reset();
    if (auto state = owner.lock()) { state->connections.erase(id); state->active_connections.store(state->connections.size()); }
}
bool ipv6_unavailable(Error error) {
    return error == net::error::address_family_not_supported || error == net::error::operation_not_supported ||
        error == boost::system::errc::make_error_condition(boost::system::errc::address_not_available) ||
        error == net::error::network_unreachable || error == net::error::no_protocol_option;
}
std::unique_ptr<tcp::acceptor> bind(net::io_context& io, const tcp::endpoint& endpoint, int backlog, Error& error) {
    auto acceptor = std::make_unique<tcp::acceptor>(io);
    acceptor->open(endpoint.protocol(), error); if (error) return {};
    acceptor->set_option(net::socket_base::reuse_address(false), error); if (error) return {};
#if defined(_WIN32)
    const BOOL exclusive = TRUE;
    if (::setsockopt(acceptor->native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
        error = Error(::WSAGetLastError(), boost::system::system_category()); return {};
    }
#endif
    if (endpoint.protocol() == tcp::v6()) { acceptor->set_option(net::ip::v6_only(true), error); if (error) return {}; }
    acceptor->bind(endpoint, error); if (error) return {};
    acceptor->listen(backlog, error); if (error) return {};
    return acceptor;
}
} // namespace
struct CompanionListener::Impl {
    std::shared_ptr<State> state;
    std::mutex joining;
    std::thread network, worker;
    explicit Impl(std::shared_ptr<State> state) : state(std::move(state)) {}
    void require_external_thread() const {
        std::lock_guard lock(state->thread_ids_mutex);
        if (std::this_thread::get_id() == state->io_id || std::this_thread::get_id() == state->worker_id)
            throw std::logic_error("Cannot start or stop a Companion listener from one of its own threads");
    }
};
CompanionListener::CompanionListener(CompanionListenerOptions options, std::string workspace, CompanionInboxPaths paths,
    std::shared_ptr<FileStore> files, CompanionInboxBounds bounds) {
    validate_options(options, bounds);
    for (const auto* path : {&paths.inbox_file, &paths.backup_file, &paths.lock_file}) {
        if (path->empty() || !path->is_absolute() || path->native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
            path_to_utf8(*path).size() > bounds.max_path_bytes)
            throw std::invalid_argument("Invalid Companion listener storage path");
    }
    impl_ = std::make_unique<Impl>(std::make_shared<State>(options, std::move(workspace), std::move(paths), std::move(files), bounds));
}
CompanionListener::~CompanionListener() { stop(); }
CompanionListenerStartResult CompanionListener::start() {
    impl_->require_external_thread();
    std::lock_guard lock(impl_->joining); auto state = impl_->state;
    if (state->closing.load()) return {CompanionListenerStartStatus::closed, state->port, state->ipv6, 0, {}};
    if (state->running.load()) return {CompanionListenerStartStatus::already_started, state->port, state->ipv6, 0, {}};
    CompanionListenerStartResult result;
    try {
        Error error;
        auto v4 = bind(state->io, tcp::endpoint{net::ip::address_v4::loopback(), state->options.port}, static_cast<int>(state->options.max_connections), error);
        if (!v4) return {CompanionListenerStartStatus::bind_failed, 0, false, error.value(), error.message().substr(0, 4096)};
        state->port = v4->local_endpoint().port(); state->acceptors.push_back(std::move(v4));
        if (state->options.enable_ipv6) {
            auto v6 = bind(state->io, tcp::endpoint{net::ip::address_v6::loopback(), state->port}, static_cast<int>(state->options.max_connections), error);
            if (v6) { state->ipv6 = true; state->acceptors.push_back(std::move(v6)); }
            else if (!ipv6_unavailable(error)) {
                state->acceptors.clear(); return {CompanionListenerStartStatus::bind_failed, 0, false, error.value(), error.message().substr(0, 4096)};
            } else result.detail = "IPv6 loopback is unavailable";
        }
        state->guard.emplace(net::make_work_guard(state->io));
        for (auto& acceptor : state->acceptors) state->accept(*acceptor);
        impl_->worker = std::thread([state] { state->worker(); });
        state->running.store(true);
        impl_->network = std::thread([state] { state->network(); });
        result.status = CompanionListenerStartStatus::started; result.port = state->port; result.ipv6 = state->ipv6;
    } catch (const std::exception& error) {
        state->closing.store(true); state->operations.request_stop(); state->changed.notify_all(); state->io.stop();
        state->running.store(false);
        if (impl_->worker.joinable()) impl_->worker.join(); if (impl_->network.joinable()) impl_->network.join();
        state->acceptors.clear(); state->guard.reset(); result.detail = std::string(error.what()).substr(0, 4096);
    }
    return result;
}
void CompanionListener::stop() {
    impl_->require_external_thread();
    auto state = impl_->state;
    std::lock_guard lock(impl_->joining);
    state->closing.store(true); state->operations.request_stop(); state->changed.notify_all(); state->io.stop();
    if (impl_->worker.joinable()) impl_->worker.join(); if (impl_->network.joinable()) impl_->network.join();
    state->running.store(false); state->acceptors.clear(); state->guard.reset();
    { std::lock_guard lock(state->thread_ids_mutex); state->io_id = {}; state->worker_id = {}; }
}
CompanionListenerStatistics CompanionListener::statistics() const noexcept {
    const auto& state = *impl_->state;
    return {state.running.load(), state.closing.load(), state.failed.load(), state.active_connections.load(), state.outstanding_admissions.load(), state.body_budget->used.load(),
        state.accepted.load(), state.duplicates.load(), state.admission_failures.load(), state.connection_rejections.load(), state.admission_rejections.load(),
        state.read_timeouts.load(), state.write_timeouts.load(), state.request_timeouts.load()};
}
std::string CompanionListener::failure_detail() const {
    const auto& state = *impl_->state; std::lock_guard lock(state.failure_mutex); return state.failure_detail;
}
const char* companion_listener_start_status_name(CompanionListenerStartStatus status) noexcept {
    constexpr const char* names[]{"Started", "AlreadyStarted", "Closed", "BindFailed", "Failed"};
    const auto index = static_cast<unsigned>(status); return index < std::size(names) ? names[index] : "Unknown";
}
} // namespace cpn
