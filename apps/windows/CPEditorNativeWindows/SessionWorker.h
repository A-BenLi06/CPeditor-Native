#pragma once
#include "cpn/api.h"
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cpn::windows {
// Owned UTF-8 JSON is the only payload crossing this worker boundary.
class SessionWorker final {
public:
    struct Options { cpn_engine_options engine{}; std::size_t maximum_commands{16}; };
    explicit SessionWorker(Options);
    ~SessionWorker();
    SessionWorker(SessionWorker const&) = delete;
    SessionWorker& operator=(SessionWorker const&) = delete;
    [[nodiscard]] std::future<std::string> load(std::string path_utf8);
    [[nodiscard]] std::future<std::string> save(std::string request_json);
    [[nodiscard]] bool operation_active() const;
    // Blocking cancellation and join. The UI calls this after switching to a background apartment.
    void close();
private:
    struct State;
    [[nodiscard]] std::future<std::string> enqueue(bool saving, std::string payload);
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::mutex joining_;
};
}
