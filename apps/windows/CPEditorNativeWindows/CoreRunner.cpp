#ifndef CPN_PORTABLE_ADAPTER_TEST
#include "pch.h"
#endif
#include "CoreRunner.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace cpn::windows {
struct CoreRunner::State {
    struct Identity { std::string token; std::string document; std::uint64_t revision; };
    Options options;
    std::function<void(RunEvent)> changed;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<RunRequest> commands;
    std::set<std::string> tokens;
    std::set<std::string> cancellations;
    bool closing{};
    bool coordinator_running{};
    std::thread::id coordinator_thread;
    std::atomic_bool delivery_failed{};

    void publish(RunEvent event) noexcept {
        // A failed UI delivery is observable and closes the coordinator; it cannot undo a drained terminal.
        try { if (changed) changed(std::move(event)); }
        catch (...) {
            delivery_failed.store(true);
            std::lock_guard lock(mutex); closing = true;
        }
    }
    static void check(cpn_status status, cpn_error const& error) {
        if (status != CPN_OK) throw std::runtime_error(error.message);
    }
    void forget(std::string const& token) {
        std::lock_guard lock(mutex);
        tokens.erase(token);
        cancellations.erase(token);
    }
    void run() noexcept {
        { std::lock_guard lock(mutex); coordinator_running = true; coordinator_thread = std::this_thread::get_id(); }
        cpn_engine_handle engine{};
        std::map<cpn_job_handle, Identity> active;
        try {
            cpn_error error{}; error.struct_size = sizeof(error);
            check(cpn_engine_create(&options.engine, &engine, &error), error);
            RunEvent ready; ready.kind = RunEvent::Kind::ready; publish(std::move(ready));
            bool stopping = false;
            while (!stopping) {
                std::deque<RunRequest> pending;
                std::set<std::string> cancelled;
                {
                    std::unique_lock lock(mutex);
                    const auto ready_to_work = [&] { return closing || !commands.empty() || !cancellations.empty(); };
                    if (!ready_to_work()) {
                        if (active.empty()) wake.wait(lock, ready_to_work);
                        else wake.wait_for(lock, options.event_poll_interval, ready_to_work);
                    }
                    stopping = closing;
                    pending.swap(commands);
                    cancelled.swap(cancellations);
                }
                for (auto& request : pending) {
                    RunEvent event; event.token = request.token;
                    try {
                        if (stopping || cancelled.contains(request.token)) {
                            event.state = "Cancelled"; event.terminal = true;
                            forget(request.token);
                        } else {
                            const auto json = CoreWire::encode(request);
                            // Reserve local tracking before the C ABI accepts a job: no post-admission allocation gap.
                            active.emplace(0, Identity{request.token, request.document_id, request.revision});
                            cpn_job_handle job{};
                            try {
                                check(cpn_engine_submit_json(engine,
                                    {reinterpret_cast<std::uint8_t const*>(json.data()), json.size()}, &job, &error), error);
                            } catch (...) { active.erase(0); throw; }
                            auto reserved = active.extract(0); reserved.key() = job;
                            if (!active.insert(std::move(reserved)).inserted) throw std::logic_error("Core reused a live job handle.");
                            event.job_id = job; event.state = "Queued";
                        }
                    } catch (std::exception const& failure) {
                        event.error = failure.what(); event.terminal = true; event.state = "Failed";
                        forget(request.token);
                    }
                    publish(std::move(event));
                }
                for (auto const& token : cancelled) {
                    for (auto const& [job, identity] : active) if (identity.token == token) {
                        check(cpn_engine_cancel(engine, job, &error), error);
                        break;
                    }
                }
                if (stopping) check(cpn_engine_close(engine, &error), error);
                drain(engine, active);
            }
            // close has joined all jobs; drain their acknowledged terminals before releasing handles.
            while (!active.empty()) drain(engine, active);
            check(cpn_engine_destroy(engine, &error), error);
            engine = 0;
        } catch (std::exception const& failure) {
            RunEvent event; event.kind = RunEvent::Kind::engine_error; event.error = failure.what();
            publish(std::move(event));
        } catch (...) {
            RunEvent event; event.kind = RunEvent::Kind::engine_error; event.error = "The Core coordinator failed.";
            publish(std::move(event));
        }
        if (engine) { cpn_engine_close(engine, nullptr); cpn_engine_destroy(engine, nullptr); }
        std::lock_guard lock(mutex);
        closing = true; coordinator_running = false; commands.clear(); tokens.clear(); cancellations.clear();
    }
    void drain(cpn_engine_handle engine, std::map<cpn_job_handle, Identity>& active) {
        cpn_error error{}; error.struct_size = sizeof(error);
        cpn_buffer buffer{};
        check(cpn_engine_drain_json(engine, options.maximum_drain_events, &buffer, &error), error);
        struct BufferOwner {
            cpn_buffer_handle handle;
            ~BufferOwner() { if (handle) cpn_buffer_release(handle, nullptr); }
        } owned{buffer.handle};
        auto events = CoreWire::decode({reinterpret_cast<char const*>(buffer.data), buffer.size});
        for (auto& event : events) {
            auto const found = active.find(event.job_id);
            if (found == active.end()) throw std::logic_error("Core returned an unknown job.");
            event.token = found->second.token;
            if (event.terminal) {
                if (!event.result || event.result->document_id != found->second.document ||
                    event.result->revision != found->second.revision)
                    throw std::logic_error("Core returned a result for a different source snapshot.");
                check(cpn_engine_release_job(engine, event.job_id, &error), error);
                forget(event.token);
                active.erase(found);
            }
            publish(std::move(event));
        }
    }
};
CoreRunner::CoreRunner(Options options, std::function<void(RunEvent)> changed) : state_(std::make_shared<State>()) {
    if (options.event_poll_interval.count() < 1 || options.maximum_commands < 1 || options.maximum_drain_events < 1)
        throw std::invalid_argument("Invalid Core coordinator options.");
    state_->options = options; state_->changed = std::move(changed);
    worker_ = std::thread([state = state_] { state->run(); });
}
CoreRunner::~CoreRunner() { close(); }
void CoreRunner::submit(RunRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->closing) throw std::runtime_error("The Core coordinator is closed.");
    if (request.token.empty()) throw std::invalid_argument("Missing local run identity.");
    if (state_->commands.size() >= state_->options.maximum_commands)
        throw std::runtime_error("The Core coordinator command queue is full.");
    auto const [position, inserted] = state_->tokens.insert(request.token);
    if (!inserted) throw std::invalid_argument("Duplicate local run identity.");
    try { state_->commands.push_back(std::move(request)); }
    catch (...) { state_->tokens.erase(position); throw; }
    state_->wake.notify_one();
}
void CoreRunner::cancel(std::string token) {
    std::lock_guard lock(state_->mutex);
    if (state_->closing || !state_->tokens.contains(token)) return;
    state_->cancellations.insert(std::move(token));
    state_->wake.notify_one();
}
bool CoreRunner::delivery_failed() const noexcept { return state_->delivery_failed.load(); }
void CoreRunner::close() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->coordinator_running && state_->coordinator_thread == std::this_thread::get_id())
            throw std::logic_error("Cannot close the coordinator from its worker callback.");
    }
    std::lock_guard joining(join_mutex_);
    { std::lock_guard lock(state_->mutex); state_->closing = true; }
    state_->wake.notify_one();
    if (worker_.joinable()) worker_.join();
}
}
