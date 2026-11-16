#include "pch.h"
#include "CompanionWorker.h"
#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <utility>

namespace cpn::windows {
struct CompanionWorker::State {
    struct Command { Operation operation; std::string json; std::promise<std::string> completion; };
    Options options;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Command> commands;
    cpn_engine_handle engine{};
    bool closing{}, running{}, active{};
    std::thread::id thread;
    static void check(cpn_status status, cpn_error const& error) { if (status != CPN_OK) throw std::runtime_error(error.message); }
    static cpn_status invoke(Operation operation, cpn_engine_handle engine, cpn_bytes bytes, cpn_buffer* buffer, cpn_error* error) {
        switch (operation) {
        case Operation::page: return cpn_engine_companion_page_json(engine, bytes, buffer, error);
        case Operation::accept: return cpn_engine_companion_accept_json(engine, bytes, buffer, error);
        case Operation::prepare: return cpn_engine_companion_prepare_json(engine, bytes, buffer, error);
        case Operation::mark_applied: return cpn_engine_companion_mark_applied_json(engine, bytes, buffer, error);
        case Operation::listener_start: return cpn_engine_companion_listener_start_json(engine, bytes, buffer, error);
        case Operation::listener_stop: return cpn_engine_companion_listener_stop_json(engine, bytes, buffer, error);
        case Operation::listener_info: return cpn_engine_companion_listener_info_json(engine, bytes, buffer, error);
        }
        throw std::invalid_argument("Unknown Companion worker operation.");
    }
    void run() noexcept {
        { std::lock_guard lock(mutex); running = true; thread = std::this_thread::get_id(); }
        cpn_engine_handle owned{}; std::exception_ptr failure;
        try {
            cpn_error error{}; error.struct_size = sizeof(error); check(cpn_engine_create(&options.engine, &owned, &error), error);
            { std::lock_guard lock(mutex); engine = owned; }
            while (true) {
                Command command;
                { std::unique_lock lock(mutex); changed.wait(lock, [&] { return closing || !commands.empty(); });
                    if (closing) break;
                    command = std::move(commands.front()); commands.pop_front(); active = true; }
                try {
                    cpn_buffer buffer{};
                    auto bytes = cpn_bytes{reinterpret_cast<std::uint8_t const*>(command.json.data()), command.json.size()};
                    check(invoke(command.operation, owned, bytes, &buffer, &error), error);
                    struct Buffer { cpn_buffer_handle handle; ~Buffer() { if (handle) cpn_buffer_release(handle, nullptr); } } release{buffer.handle};
                    if (buffer.size && !buffer.data) throw std::logic_error("Companion ABI returned a null payload.");
                    std::string response;
                    if (buffer.size) response.assign(reinterpret_cast<char const*>(buffer.data), buffer.size);
                    command.completion.set_value(std::move(response));
                } catch (...) { command.completion.set_exception(std::current_exception()); }
                { std::lock_guard lock(mutex); active = false; }
            }
        } catch (...) { failure = std::current_exception(); }
        { std::lock_guard lock(mutex); closing = true; active = false; engine = 0;
            if (!failure) failure = std::make_exception_ptr(std::runtime_error("The Companion coordinator is closed."));
            for (auto& command : commands) command.completion.set_exception(failure);
            commands.clear(); }
        if (owned) { cpn_engine_close(owned, nullptr); cpn_engine_destroy(owned, nullptr); }
        { std::lock_guard lock(mutex); running = false; }
    }
};
CompanionWorker::CompanionWorker(Options options) : state_(std::make_shared<State>()) {
    if (!options.maximum_commands) throw std::invalid_argument("Companion queue capacity must be positive.");
    state_->options = options; worker_ = std::thread([state = state_] { state->run(); });
}
CompanionWorker::~CompanionWorker() { close(); }
std::future<std::string> CompanionWorker::request(Operation operation, std::string json) {
    if (json.empty() || (state_->options.engine.max_request_bytes && json.size() > state_->options.engine.max_request_bytes))
        throw std::invalid_argument("Companion command exceeds its ABI request budget.");
    std::lock_guard lock(state_->mutex);
    if (state_->closing) throw std::runtime_error("The Companion coordinator is closed.");
    if (state_->commands.size() >= state_->options.maximum_commands) throw std::runtime_error("The Companion command queue is full.");
    State::Command command{operation, std::move(json), {}};
    auto result = command.completion.get_future(); state_->commands.push_back(std::move(command)); state_->changed.notify_one(); return result;
}
bool CompanionWorker::operation_active() const { std::lock_guard lock(state_->mutex); return state_->active; }
void CompanionWorker::close() {
    { std::lock_guard lock(state_->mutex);
        if (state_->running && state_->thread == std::this_thread::get_id()) throw std::logic_error("Cannot close Companion from its own coordinator thread."); }
    std::lock_guard joining(joining_); cpn_engine_handle engine{};
    { std::lock_guard lock(state_->mutex); state_->closing = true; engine = state_->engine; }
    state_->changed.notify_one();
    if (engine) cpn_engine_close(engine, nullptr);
    if (worker_.joinable()) worker_.join();
}
}
