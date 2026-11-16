// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/companion_listener.hpp"
#include "cpn/digest.hpp"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;
using Json = nlohmann::json;
using namespace std::chrono_literals;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void reject(F operation, const char* message) {
    try { operation(); } catch (const std::exception&) { return; } throw std::runtime_error(message);
}
template<class F> void eventually(F condition, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::yield();
    }
}
class Workspace {
public:
    Workspace() : files(cpn::native_file_store()), path(files->create_workspace()) {}
    ~Workspace() { files->remove_workspace(path); }
    cpn::CompanionInboxPaths locations() const {
        return {path / cpn::path_from_utf8("HTTP 收件箱.json"), path / cpn::path_from_utf8("HTTP 此前.json"), path / "inbox.lock"};
    }
    std::shared_ptr<cpn::FileStore> files;
    std::filesystem::path path;
};
const std::string workspace_id = "http-workspace-e\u0301";
std::string payload(std::string_view url = "https://example.test/task/a") {
    auto encoded = Json{{"name", "题目 😀 e\u0301"}, {"group", "Group"}, {"url", url},
        {"timeLimit", 1000}, {"memoryLimit", 256},
        {"tests", {{{"input", std::string{"input\0", 6} + "中\r\n"}, {"output", ""}}}}}.dump();
    encoded.pop_back(); encoded += R"(, "unknown": {"huge":18446744073709551616,"float":1.000e0} })";
    return encoded;
}
std::string batched(std::string_view url, std::string_view id, unsigned size) {
    auto value = Json::parse(payload(url)); value["batch"] = {{"id", id}, {"size", size}}; return value.dump();
}
std::string headers(std::uint16_t port, std::size_t length, std::string_view extra = {},
    std::string_view method = "POST", std::string_view target = "/", std::string_view type = "application/json") {
    return std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: localhost:" + std::to_string(port) +
        "\r\nContent-Type: " + std::string(type) + "\r\nContent-Length: " + std::to_string(length) + "\r\n" + std::string(extra) + "\r\n";
}
std::string chunked(std::uint16_t port, std::string_view body, std::string_view trailer = {}) {
    const auto hex = [](std::size_t value) { std::ostringstream stream; stream << std::hex << value; return stream.str(); };
    std::string wire = "POST / HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
    const auto middle = body.size() / 2;
    for (const auto part : {body.substr(0, middle), body.substr(middle)})
        wire += hex(part.size()) + "\r\n" + std::string(part) + "\r\n";
    wire += "0\r\n" + std::string(trailer) + "\r\n";
    return wire;
}
class Client {
public:
    Client(std::uint16_t port, bool ipv6 = false, int receive_buffer = 0) : stream(io) {
        if (receive_buffer) {
            stream.socket().open(ipv6 ? tcp::v6() : tcp::v4());
            stream.socket().set_option(net::socket_base::receive_buffer_size(receive_buffer));
        }
        stream.expires_after(3s); Error result;
        stream.async_connect(tcp::endpoint{ipv6 ? net::ip::address{net::ip::address_v6::loopback()} : net::ip::address{net::ip::address_v4::loopback()}, port},
            [&](Error error) { result = error; });
        io.run(); if (result) throw boost::system::system_error(result);
    }
    ~Client() { close(); }
    void send(std::string_view bytes) {
        io.restart(); stream.expires_after(3s); Error result;
        net::async_write(stream, net::buffer(bytes.data(), bytes.size()), [&](Error error, std::size_t) { result = error; });
        io.run(); if (result) throw boost::system::system_error(result);
    }
    http::response<http::string_body> response() {
        io.restart(); stream.expires_after(3s); Error result;
        http::response_parser<http::string_body> parser; parser.body_limit(65536);
        http::async_read(stream, buffer, parser, [&](Error error, std::size_t) { result = error; });
        io.run(); if (result) throw boost::system::system_error(result);
        return parser.release();
    }
    std::string eof() {
        std::string bytes;
        while (true) {
            io.restart(); stream.expires_after(3s); Error result; std::size_t read{};
            std::array<char, 1024> part{};
            stream.async_read_some(net::buffer(part), [&](Error error, std::size_t count) { result = error; read = count; });
            io.run(); bytes.append(part.data(), read);
            expect(bytes.size() <= 65536, "bounded client response");
            if (result == net::error::eof || result == net::error::connection_reset) return bytes;
            if (result) throw boost::system::system_error(result);
        }
    }
    void close() noexcept { Error ignored; stream.socket().close(ignored); }
    net::io_context io;
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
};
http::response<http::string_body> request(std::uint16_t port, std::string wire, bool ipv6 = false) {
    Client client(port, ipv6); client.send(wire); return client.response();
}
http::response<http::string_body> post(std::uint16_t port, std::string_view body, bool ipv6 = false) {
    return request(port, headers(port, body.size()) + std::string(body), ipv6);
}
cpn::CompanionListenerOptions options() {
    cpn::CompanionListenerOptions options; options.port = 0; options.read_timeout = 2s; options.write_timeout = 2s; options.request_timeout = 5s; return options;
}
class ObservedStore final : public cpn::FileStore {
public:
    enum class Fault { none, before_write, after_write, large_diagnostic };
    explicit ObservedStore(cpn::CompanionInboxPaths paths) : files(cpn::native_file_store()), paths(std::move(paths)) {}
    std::string read(const std::filesystem::path& path, std::size_t maximum) const override { return files->read(path, maximum); }
    std::optional<std::string> read_optional(const std::filesystem::path& path, std::size_t maximum) const override { return files->read_optional(path, maximum); }
    std::unique_ptr<cpn::FileLock> acquire_lock(const std::filesystem::path& path, std::chrono::milliseconds timeout, std::stop_token stop) const override {
        auto lock = files->acquire_lock(path, timeout, stop);
        if (path == paths.lock_file) {
            if (on_locked) on_locked();
            std::unique_lock guard(mutex);
            if (blocked) {
                entered = true; changed.notify_all();
                std::stop_callback cancelled(stop, [&] { changed.notify_all(); });
                changed.wait(guard, [&] { return released || stop.stop_requested(); });
                if (stop.stop_requested()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
        }
        return lock;
    }
    void write_atomic(const std::filesystem::path& path, std::string_view bytes) const override {
        if (path == paths.inbox_file) {
            const auto mode = fault.load();
            if (mode == Fault::before_write) throw cpn::AtomicWriteError("injected before replacement", false);
            if (mode == Fault::large_diagnostic) throw cpn::AtomicWriteError(std::string(4096, '\1'), false);
            files->write_atomic(path, bytes);
            writes.fetch_add(1);
            if (mode == Fault::after_write) throw cpn::AtomicWriteError("injected after replacement", true);
        } else files->write_atomic(path, bytes);
    }
    std::filesystem::path create_workspace() const override { return files->create_workspace(); }
    void remove_workspace(const std::filesystem::path& path) const noexcept override { files->remove_workspace(path); }
    void block() { std::lock_guard lock(mutex); blocked = true; entered = false; released = false; }
    void wait_entered() {
        std::unique_lock lock(mutex); expect(changed.wait_for(lock, 3s, [&] { return entered; }), "actual inbox lock worker entered");
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
    std::shared_ptr<cpn::FileStore> files;
    cpn::CompanionInboxPaths paths;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    bool blocked{false};
    mutable bool entered{false}, released{false};
    std::atomic<Fault> fault{Fault::none};
    std::function<void()> on_locked;
    mutable std::atomic<unsigned> writes{0};
};
void basic() {
    Workspace workspace; const auto paths = workspace.locations();
    cpn::CompanionListener listener(options(), workspace_id, paths);
    const auto started = listener.start(); expect(started.status == cpn::CompanionListenerStartStatus::started && started.port, "bound ephemeral loopback port");
    expect(listener.start().status == cpn::CompanionListenerStartStatus::already_started, "already started is explicit");
    const auto body = payload(); auto response = post(started.port, body);
    auto receipt = Json::parse(response.body());
    expect(response.result() == http::status::accepted && receipt["status"] == "Accepted" && receipt["inbox_replaced"] == true, "durable accepted returns 202");
    cpn::CompanionInboxService inbox(workspace_id); auto loaded = inbox.load(paths);
    expect(loaded.snapshot && loaded.snapshot->items.size() == 1 && loaded.snapshot->items[0].candidate.original_json == body, "202 follows actual durable original bytes");
    expect(loaded.snapshot->items[0].candidate.tests[0].input == std::string{"input\0", 6} + "中\r\n" && loaded.snapshot->items[0].candidate.tests[0].output.empty(), "NUL unicode CRLF and explicit empty answer retained");
    const auto first_digest = loaded.disk_digest; const auto first_id = receipt["import_id"];
    response = request(started.port, chunked(started.port, body)); receipt = Json::parse(response.body());
    expect(response.result() == http::status::accepted && receipt["status"] == "Duplicate" && receipt["import_id"] == first_id && !receipt["inbox_replaced"].get<bool>(), "chunked unchanged body exactly duplicates");
    expect(inbox.load(paths).disk_digest == first_digest, "duplicate does not rewrite inbox");
    Client segmented(started.port); const auto head = headers(started.port, body.size());
    segmented.send(head.substr(0, 9)); segmented.send(head.substr(9));
    segmented.send(body.substr(0, body.size() / 2)); segmented.send(body.substr(body.size() / 2));
    expect(segmented.response().result() == http::status::accepted, "segmented headers and body parse");
    if (started.ipv6) expect(post(started.port, body, true).result() == http::status::accepted, "IPv6 loopback listener works");
    response = request(started.port, headers(started.port, body.size(), "Content-Encoding: identity\r\n", "POST", "/", "Application/JSON; charset=\"UTF-8\"") + body);
    expect(response.result() == http::status::accepted, "JSON charset and explicit identity supported");
    response = post(started.port, " \n" + body); receipt = Json::parse(response.body());
    loaded = inbox.load(paths);
    expect(response.result() == http::status::accepted && receipt["import_id"] != first_id && loaded.snapshot->items.size() == 2, "changed raw whitespace creates update");
    expect(loaded.snapshot->items.back().previous_import_id == first_id.get<std::string>(), "update keeps previous history");
    expect(post(started.port, batched("https://batch.test/a", "batch", 2)).result() == http::status::accepted, "batch first accepted");
    expect(post(started.port, batched("https://batch.test/b", "batch", 3)).result() == http::status::conflict, "inconsistent batch size rejected");
    listener.stop(); auto statistics = listener.statistics();
    expect(statistics.closed && !statistics.running && !statistics.active_connections && !statistics.outstanding_admissions && !statistics.inflight_body_bytes, "stop releases threads connections and body reservations");
    expect(listener.start().status == cpn::CompanionListenerStartStatus::closed, "closed listener cannot restart");
    std::cout << "basic durable HTTP tests passed\n";
}
void invalid_http() {
    Workspace workspace; const auto paths = workspace.locations();
    auto configuration = options(); configuration.max_body_bytes = 4096; configuration.max_header_bytes = 512; configuration.max_header_count = 8;
    cpn::CompanionListener listener(configuration, workspace_id, paths); const auto port = listener.start().port;
    const auto body = payload(); const auto normal = headers(port, body.size());
    struct Case { std::string wire; http::status expected; };
    const std::vector<Case> cases{
        {headers(port, body.size(), {}, "GET") + body, http::status::method_not_allowed},
        {headers(port, body.size(), {}, "POST", "/?a") + body, http::status::not_found},
        {headers(port, body.size(), {}, "POST", "/", "text/plain") + body, http::status::unsupported_media_type},
        {headers(port, body.size(), {}, "POST", "/", "application/json; charset=latin-1") + body, http::status::unsupported_media_type},
        {headers(port, body.size(), "Content-Type: application/json\r\n") + body, http::status::unsupported_media_type},
        {headers(port, body.size(), "Host: localhost:" + std::to_string(port) + "\r\n") + body, http::status::bad_request},
        {headers(port, body.size(), "Content-Encoding: gzip\r\n") + body, http::status::unsupported_media_type},
        {headers(port, body.size(), "Content-Encoding: identity\r\nContent-Encoding: identity\r\n") + body, http::status::unsupported_media_type},
        {headers(port, body.size(), "Expect: 100-continue\r\n") + body, http::status::expectation_failed},
        {headers(port, body.size(), "Trailer: X-Metadata\r\n") + body, http::status::bad_request},
        {headers(port, 4097), http::status::payload_too_large},
        {headers(port, body.size(), "X-Long: " + std::string(600, 'a') + "\r\n") + body, http::status::request_header_fields_too_large},
        {headers(port, body.size(), "X-1: a\r\nX-2: a\r\nX-3: a\r\nX-4: a\r\nX-5: a\r\nX-6: a\r\n") + body, http::status::request_header_fields_too_large},
        {chunked(port, body, "Host: attacker.test\r\n"), http::status::bad_request},
        {headers(port, 2) + "{}", http::status::bad_request},
        {headers(port, 1) + std::string(1, '\xff'), http::status::bad_request}
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto response = request(port, cases[index].wire);
        if (response.result() != cases[index].expected)
            throw std::runtime_error("invalid HTTP case " + std::to_string(index) + " status " + std::to_string(response.result_int()) + " body " + response.body());
    }
    for (const auto host : {"evil.test", "user@localhost", "localhost.", "localhost:1", "127.0.0.2", "[::ffff:127.0.0.1]", "[::1]:1"}) {
        auto wire = normal; const auto first = wire.find("Host: "); const auto end = wire.find("\r\n", first);
        wire.replace(first, end - first, "Host: " + std::string(host));
        expect(request(port, wire + body).result() == http::status::bad_request, "noncanonical or foreign host rejected");
    }
    auto missing = normal; const auto first = missing.find("Host: "); const auto end = missing.find("\r\n", first); missing.erase(first, end + 2 - first);
    expect(request(port, missing + body).result() == http::status::bad_request, "missing Host rejected");
    auto duplicate_length = headers(port, body.size(), "Content-Length: 1\r\n") + body;
    expect(request(port, duplicate_length).result() == http::status::bad_request, "conflicting content lengths rejected by Beast");
    expect(!workspace.files->read_optional(paths.inbox_file, 4096), "invalid requests have no inbox write");
    listener.stop(); std::cout << "HTTP shape and transport bounds tests passed\n";
}
void failures() {
    Workspace workspace; const auto paths = workspace.locations(); auto store = std::make_shared<ObservedStore>(paths);
    cpn::CompanionListener listener(options(), workspace_id, paths, store); const auto port = listener.start().port;
    const auto body = payload(); store->fault = ObservedStore::Fault::before_write;
    auto response = post(port, body); auto receipt = Json::parse(response.body());
    expect(response.result() == http::status::service_unavailable && receipt["status"] == "IOFailed" && !receipt["inbox_replaced"].get<bool>(), "before replacement never returns 202");
    expect(!store->read_optional(paths.inbox_file, 4096), "before replacement preserves missing destination");
    store->fault = ObservedStore::Fault::after_write; response = post(port, body); receipt = Json::parse(response.body());
    expect(response.result() == http::status::service_unavailable && receipt["status"] == "DurabilityUnknown" && receipt["inbox_replaced"].get<bool>(), "uncertain durability reports failure and replacement");
    expect(cpn::CompanionInboxService(workspace_id).load(paths).snapshot->items[0].candidate.original_json == body, "uncertain replacement retains admitted raw payload");
    store->fault = ObservedStore::Fault::none; response = post(port, body); receipt = Json::parse(response.body());
    expect(response.result() == http::status::accepted && receipt["status"] == "Duplicate", "retry after uncertain replacement safely deduplicates");
    listener.stop(); std::cout << "HTTP durable failure classification tests passed\n";
}
void capacity() {
    Workspace workspace; const auto paths = workspace.locations();
    auto configuration = options(); configuration.max_body_bytes = 4096; configuration.max_inflight_body_bytes = 4096;
    cpn::CompanionListener listener(configuration, workspace_id, paths); const auto port = listener.start().port;
    Client first(port); first.send(headers(port, 4096) + "{");
    eventually([&] { return listener.statistics().inflight_body_bytes == 4096; }, "body reservation is actually held");
    expect(post(port, payload()).result() == http::status::service_unavailable, "aggregate body budget backpressures another body");
    first.close(); eventually([&] { return listener.statistics().inflight_body_bytes == 0; }, "disconnected reader releases reservation");
    expect(post(port, payload()).result() == http::status::accepted, "released aggregate budget permits new body");
    listener.stop();
    auto store = std::make_shared<ObservedStore>(paths); store->block(); configuration = options(); configuration.max_admissions = 1;
    cpn::CompanionListener queued(configuration, workspace_id, paths, store); const auto queue_port = queued.start().port;
    Client blocked(queue_port); blocked.send(headers(queue_port, payload().size()) + payload()); store->wait_entered();
    const auto response = post(queue_port, payload("https://other.test/"));
    expect(response.result() == http::status::service_unavailable && Json::parse(response.body())["status"] == "AdmissionCapacity", "active admission counts against queue capacity");
    store->release(); expect(blocked.response().result() == http::status::accepted, "unblocked admission completes");
    queued.stop(); expect(!queued.statistics().inflight_body_bytes, "queue body lease released");
    configuration = options(); configuration.max_connections = 1;
    cpn::CompanionListener limited(configuration, workspace_id, paths); const auto limited_port = limited.start().port;
    Client idle(limited_port); idle.send("P"); eventually([&] { return limited.statistics().active_connections == 1; }, "actual active connection counted");
    Client denied(limited_port); expect(denied.eof().empty(), "excess connection closed before parsing");
    expect(limited.statistics().connection_rejections == 1, "connection backpressure counted"); limited.stop();
    std::cout << "HTTP aggregate body admission and connection budgets passed\n";
}
void timeouts_and_stop() {
    Workspace workspace; const auto paths = workspace.locations(); auto configuration = options(); configuration.read_timeout = 150ms;
    cpn::CompanionListener reader(configuration, workspace_id, paths); const auto port = reader.start().port;
    Client header(port); header.send("POST / HTTP/1.1\r\nHo"); expect(header.eof().empty(), "slow partial header closes on read deadline");
    Client body(port); body.send(headers(port, payload().size()) + "{"); expect(body.eof().empty(), "slow partial body closes on read deadline");
    expect(reader.statistics().read_timeouts == 2, "whole header/body read deadline counters"); reader.stop();
    configuration = options(); configuration.request_timeout = 150ms;
    cpn::CompanionListener total(configuration, workspace_id, paths); const auto total_port = total.start().port;
    Client whole(total_port); whole.send("P"); expect(whole.eof().empty(), "total deadline closes before stage deadline");
    expect(total.statistics().request_timeouts == 1 && !total.statistics().read_timeouts, "independent total request deadline counted"); total.stop();
    auto store = std::make_shared<ObservedStore>(paths); store->block();
    cpn::CompanionListener admission(configuration, workspace_id, paths, store); const auto admission_port = admission.start().port;
    Client waiting(admission_port); const auto raw = payload(); waiting.send(headers(admission_port, raw.size()) + raw); store->wait_entered();
    expect(waiting.eof().empty(), "total request deadline includes locked admission");
    eventually([&] { return admission.statistics().outstanding_admissions == 0; }, "timed out admission actually observes stop token");
    expect(!store->read_optional(paths.inbox_file, 4096), "cancelled locked admission creates no raw payload"); admission.stop();
    store = std::make_shared<ObservedStore>(paths); store->block(); configuration = options(); configuration.max_admissions = 2;
    cpn::CompanionListener stopping(configuration, workspace_id, paths, store); const auto stop_port = stopping.start().port;
    Client first(stop_port); first.send(headers(stop_port, raw.size()) + raw); store->wait_entered();
    Client second(stop_port); second.send(headers(stop_port, raw.size()) + raw);
    eventually([&] { return stopping.statistics().outstanding_admissions == 2; }, "actual queued and active admissions counted");
    std::barrier begin(3); std::jthread a([&] { begin.arrive_and_wait(); stopping.stop(); }); std::jthread b([&] { begin.arrive_and_wait(); stopping.stop(); });
    begin.arrive_and_wait(); a.join(); b.join();
    expect(first.eof().empty() && second.eof().empty(), "concurrent stop closes active and queued sockets");
    const auto stats = stopping.statistics();
    expect(stats.closed && !stats.running && !stats.active_connections && !stats.outstanding_admissions && !stats.inflight_body_bytes, "concurrent stop joins and releases all bounded ownership");
    expect(!store->read_optional(paths.inbox_file, 4096), "stop cancels before durable replacement");
    std::cout << "HTTP deadlines and concurrent cancellation tests passed\n";
}
void binding_and_options() {
    Workspace workspace; const auto paths = workspace.locations(); net::io_context io;
    tcp::acceptor occupied(io, tcp::endpoint{net::ip::address_v4::loopback(), 0});
    auto configuration = options(); configuration.port = occupied.local_endpoint().port();
    cpn::CompanionListener conflict(configuration, workspace_id, paths);
    expect(conflict.start().status == cpn::CompanionListenerStartStatus::bind_failed, "occupied IPv4 port fails explicitly"); conflict.stop();
    configuration = options(); configuration.enable_ipv6 = false;
    cpn::CompanionListener v4(configuration, workspace_id, paths); const auto started = v4.start(); expect(!started.ipv6, "explicit IPv4 only binding"); v4.stop();
    for (const auto invalid : {0u, 129u}) { configuration = options(); configuration.max_connections = invalid;
        reject([&] { cpn::CompanionListener rejected(configuration, workspace_id, paths); }, "invalid connection bound rejected"); }
    configuration = options(); configuration.max_body_bytes = std::numeric_limits<std::size_t>::max();
    reject([&] { cpn::CompanionListener rejected(configuration, workspace_id, paths); }, "overflow body bound rejected");
    configuration = options(); configuration.read_timeout = 0ms;
    reject([&] { cpn::CompanionListener rejected(configuration, workspace_id, paths); }, "zero deadline rejected");
    auto relative = paths; relative.inbox_file = "relative.json";
    reject([&] { cpn::CompanionListener rejected(options(), workspace_id, relative); }, "relative inbox path rejected before binding");
    cpn::CompanionListener never_started(options(), workspace_id, paths); never_started.stop();
    expect(never_started.start().status == cpn::CompanionListenerStartStatus::closed, "stop before start closes permanently");
    std::cout << "HTTP binding and option validation tests passed\n";
}
void slow_writer_and_reentrancy() {
    Workspace workspace; const auto paths = workspace.locations(); auto store = std::make_shared<ObservedStore>(paths);
    auto configuration = options(); configuration.write_timeout = 150ms; configuration.socket_send_buffer_bytes = 1024;
    store->fault = ObservedStore::Fault::large_diagnostic;
    cpn::CompanionListener writer(configuration, workspace_id, paths, store); const auto port = writer.start().port;
    Client stalled(port, false, 1024); const auto body = payload(); stalled.send(headers(port, body.size()) + body);
    eventually([&] { return writer.statistics().admission_failures == 1; }, "actual diagnostic response admission finished");
    eventually([&] { return writer.statistics().active_connections == 0; }, "slow writer closed within deadline");
    const auto write_timeouts = writer.statistics().write_timeouts;
    const auto partial = stalled.eof(); expect(partial.starts_with("HTTP/1.1 503"), "delayed receiver keeps failure status");
    // A bounded response can fit in kernel TCP buffers before the receiver reads.
    // That is valid completion, not an asynchronous write stalled past its deadline.
    if (!write_timeouts) {
        http::response_parser<http::string_body> completed; completed.eager(true); Error error;
        completed.put(net::buffer(partial), error);
        expect(!error && completed.is_done() && Json::parse(completed.get().body())["status"] == "IOFailed", "buffered bounded response completed intact");
    }
    std::cout << "delayed receiver write timeout observations: " << write_timeouts << '\n';
    expect(!store->read_optional(paths.inbox_file, 4096), "slow failure response has no inbox replacement"); writer.stop();
    store = std::make_shared<ObservedStore>(paths); std::atomic<bool> rejected{false}, start_rejected{false};
    cpn::CompanionListener* own{};
    store->on_locked = [&] {
        try { static_cast<void>(own->start()); } catch (const std::logic_error&) { start_rejected.store(true); }
        try { own->stop(); } catch (const std::logic_error&) { rejected.store(true); }
    };
    cpn::CompanionListener reentrant(options(), workspace_id, paths, store); own = &reentrant; const auto reentrant_port = reentrant.start().port;
    expect(post(reentrant_port, body).result() == http::status::accepted && rejected.load() && start_rejected.load(), "worker reentrant start and stop rejected before lifecycle mutation");
    expect(reentrant.statistics().running && !reentrant.statistics().failed, "reentrant stop preserves usable listener");
    expect(post(reentrant_port, body).result() == http::status::accepted, "another request succeeds after rejected reentrant stop");
    reentrant.stop(); std::jthread repeated([&] { reentrant.stop(); }); repeated.join();
    // External stop holds the joining mutex while this native inbox worker reenters start.
    store = std::make_shared<ObservedStore>(paths);
    std::atomic<bool> entered{false}, concurrent_start_rejected{false};
    cpn::CompanionListener stopping(options(), workspace_id, paths, store);
    store->on_locked = [&] {
        entered.store(true);
        eventually([&] { return stopping.statistics().closed; }, "external stop reached the worker");
        try { static_cast<void>(stopping.start()); } catch (const std::logic_error&) { concurrent_start_rejected.store(true); }
    };
    const auto stopping_port = stopping.start().port;
    Client pending(stopping_port); pending.send(headers(stopping_port, body.size()) + body);
    eventually([&] { return entered.load(); }, "native admission callback entered");
    std::jthread stopper([&] { stopping.stop(); }); stopper.join();
    expect(concurrent_start_rejected.load() && !stopping.statistics().running, "worker start cannot deadlock external stop joining");
    expect(pending.eof().empty(), "external stop closes the reentrant admission connection");
    std::cout << "HTTP bounded writes and worker reentrancy tests passed\n";
}
} // namespace
int main() {
    try { basic(); invalid_http(); failures(); capacity(); timeouts_and_stop(); binding_and_options(); slow_writer_and_reentrancy(); std::cout << "all Companion listener tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
