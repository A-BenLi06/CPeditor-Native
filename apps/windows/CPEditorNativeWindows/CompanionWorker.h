#pragma once
#include "cpn/api.h"
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cpn::windows {
class CompanionWorker final {
public:
    enum class Operation { page, accept, prepare, mark_applied, listener_start, listener_stop, listener_info };
    struct Options { cpn_engine_options engine{}; std::size_t maximum_commands{16}; };
    explicit CompanionWorker(Options);
    ~CompanionWorker();
    CompanionWorker(CompanionWorker const&) = delete;
    CompanionWorker& operator=(CompanionWorker const&) = delete;
    [[nodiscard]] std::future<std::string> request(Operation, std::string owned_json);
    [[nodiscard]] bool operation_active() const;
    void close();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::mutex joining_;
};
}
