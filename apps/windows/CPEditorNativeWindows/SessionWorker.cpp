#ifndef CPN_PORTABLE_ADAPTER_TEST
#include "pch.h"
#endif
#include "SessionWorker.h"
#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <utility>

namespace cpn::windows {
struct SessionWorker::State {
    struct Command { bool saving; std::string payload; std::promise<std::string> completion; };
    Options options;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Command> commands;
    cpn_engine_handle engine{};
    bool closing{};
    bool running{};
    bool active{};
    std::thread::id thread;
    static void check(cpn_status status, cpn_error const& error) {
        if (status != CPN_OK) throw std::runtime_error(error.message);
    }
    void run() noexcept {
        { std::lock_guard lock(mutex); running = true; thread = std::this_thread::get_id(); }
        cpn_engine_handle owned_engine{};
        std::exception_ptr failure;
        try {
            cpn_error error{}; error.struct_size = sizeof(error);
            check(cpn_engine_create(&options.engine, &owned_engine, &error), error);
            { std::lock_guard lock(mutex); engine = owned_engine; }
            while (true) {
                Command command;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return closing || !commands.empty(); });
                    if (closing) break;
                    command = std::move(commands.front()); commands.pop_front(); active = true;
                }
                try {
                    cpn_buffer buffer{};
                    auto payload = cpn_bytes{reinterpret_cast<std::uint8_t const*>(command.payload.data()), command.payload.size()};
                    check(command.saving ? cpn_engine_session_save_json(owned_engine, payload, &buffer, &error)
                        : cpn_engine_session_load(owned_engine, payload, &buffer, &error), error);
                    struct OwnedBuffer { cpn_buffer_handle handle; ~OwnedBuffer() { if (handle) cpn_buffer_release(handle, nullptr); } } release{buffer.handle};
                    std::string result;
                    if (buffer.size) {
                        if (!buffer.data) throw std::logic_error("Session ABI returned a null payload.");
                        result.assign(reinterpret_cast<char const*>(buffer.data), buffer.size);
                    }
                    command.completion.set_value(std::move(result));
                } catch (...) { command.completion.set_exception(std::current_exception()); }
                { std::lock_guard lock(mutex); active = false; }
            }
        } catch (...) { failure = std::current_exception(); }
        {
            std::lock_guard lock(mutex);
            closing = true; active = false; engine = 0;
            if (!failure) failure = std::make_exception_ptr(std::runtime_error("The session worker is closed."));
            for (auto& command : commands) command.completion.set_exception(failure);
            commands.clear();
        }
        if (owned_engine) { cpn_engine_close(owned_engine, nullptr); cpn_engine_destroy(owned_engine, nullptr); }
        { std::lock_guard lock(mutex); running = false; }
    }
};
SessionWorker::SessionWorker(Options options) : state_(std::make_shared<State>()) {
    if (!options.maximum_commands) throw std::invalid_argument("Session command capacity must be positive.");
    state_->options = options;
    worker_ = std::thread([state = state_] { state->run(); });
}
SessionWorker::~SessionWorker() { close(); }
std::future<std::string> SessionWorker::enqueue(bool saving, std::string payload) {
    std::lock_guard lock(state_->mutex);
    if (state_->closing) throw std::runtime_error("The session worker is closed.");
    if (payload.empty() || (saving && state_->options.engine.max_request_bytes && payload.size() > state_->options.engine.max_request_bytes) ||
        (!saving && (payload.size() > 32768 || payload.find('\0') != std::string::npos)))
        throw std::invalid_argument("Session command payload exceeds its ABI bounds.");
    if (state_->commands.size() >= state_->options.maximum_commands) throw std::runtime_error("The session command queue is full.");
    State::Command command{saving, std::move(payload), {}};
    auto future = command.completion.get_future();
    state_->commands.push_back(std::move(command)); state_->changed.notify_one(); return future;
}
std::future<std::string> SessionWorker::load(std::string path_utf8) { return enqueue(false, std::move(path_utf8)); }
std::future<std::string> SessionWorker::save(std::string request_json) { return enqueue(true, std::move(request_json)); }
bool SessionWorker::operation_active() const { std::lock_guard lock(state_->mutex); return state_->active; }
void SessionWorker::close() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->running && state_->thread == std::this_thread::get_id()) throw std::logic_error("Cannot close the session worker from its worker thread.");
    }
    std::lock_guard joining(joining_);
    cpn_engine_handle engine{};
    { std::lock_guard lock(state_->mutex); state_->closing = true; engine = state_->engine; }
    state_->changed.notify_one();
    // Cancellation bypasses the command queue: a save may be waiting on a cross-process file lock.
    if (engine) cpn_engine_close(engine, nullptr);
    if (worker_.joinable()) worker_.join();
}
}
