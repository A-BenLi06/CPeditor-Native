#pragma once
#include "CoreWire.h"
#include "cpn/api.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace cpn::windows {
class CoreRunner final {
public:
    struct Options {
        cpn_engine_options engine{};
        std::chrono::milliseconds event_poll_interval{25};
        std::size_t maximum_commands{64};
        std::uint32_t maximum_drain_events{64};
    };
    CoreRunner(Options, std::function<void(RunEvent)>);
    ~CoreRunner();
    CoreRunner(CoreRunner const&) = delete;
    CoreRunner& operator=(CoreRunner const&) = delete;
    void submit(RunRequest);
    void cancel(std::string token);
    [[nodiscard]] bool delivery_failed() const noexcept;
    // The event handler runs on the coordinator thread and must dispatch rather than close/destroy this runner.
    // Blocking join; same-worker calls throw before changing shutdown state.
    // The window invokes this only after switching to a background apartment.
    void close();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::mutex join_mutex_;
};
}
