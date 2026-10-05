#pragma once

#include "settings.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <fcitx-utils/eventdispatcher.h>

namespace fcitx { class EventLoop; }

namespace rome {
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct PredictionResult {
    uint64_t generation;
    // string 表示请求错误，不包含服务端正文或认证信息。
    std::variant<std::vector<std::string>, std::string> value;
};

// 最多一个进行中的请求和一个待处理请求。所有回调都在 Fcitx 事件循环执行。
class CompletionClient {
public:
    CompletionClient(fcitx::EventLoop &loop, std::function<void(PredictionResult)> callback);
    ~CompletionClient();
    Cancellation submit(uint64_t generation, Settings settings, std::string prefix);

private:
    struct Request {
        uint64_t generation;
        Settings settings;
        std::string prefix;
        Cancellation cancelled;
    };
    void run(std::stop_token stop);

    fcitx::EventDispatcher dispatcher_;
    std::function<void(PredictionResult)> callback_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    Cancellation active_;
    std::jthread worker_;
};
} // namespace rome
