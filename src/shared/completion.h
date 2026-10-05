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
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

namespace rome {
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct PredictionResult {
    uint64_t generation;
    // string 表示请求错误，不包含服务端正文或认证信息。
    std::variant<std::vector<std::string>, std::string> value;
};

// 最多一个进行中的请求和一个待处理请求。
// dispatch 从工作线程调用，必须将任务投递到调用方的事件循环，不得内联执行。
// submit、析构和结果回调由同一个宿主事件线程执行。settings 必须已校验。
// 调度器必须存活至客户端析构完成。已排队任务在取消、替换和析构后不调用回调。
class CompletionClient {
public:
    using Task = std::function<void()>;
    using Dispatch = std::function<void(Task)>;
    CompletionClient(Dispatch dispatch, std::function<void(PredictionResult)> callback);
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

    Dispatch dispatch_;
    std::function<void(PredictionResult)> callback_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    Cancellation active_;
    std::jthread worker_;
};
} // namespace rome
