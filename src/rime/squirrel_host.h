#pragma once

#include "completion.h"
#include "surrounding.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace rome {
// 控件身份由平台定义，不能仅以进程或文本内容判断两个输入框相同。
class TextTarget {
public:
    virtual ~TextTarget() = default;
    virtual bool equals(const TextTarget &other) const = 0;
};

struct TextWindow {
    std::shared_ptr<const TextTarget> target;
    size_t originUtf16;
    SurroundingSnapshot snapshot;
    bool operator==(const TextWindow &other) const {
        return target->equals(*other.target) && originUtf16 == other.originUtf16 &&
               snapshot == other.snapshot;
    }
    TextWindow afterCommit(std::string_view text) const {
        return {target, originUtf16, snapshot.afterCommit(text)};
    }
};

// 所有读取和监测回调在宿主主线程执行。失败时不返回猜测的文字。
class SquirrelHost {
public:
    using Time = std::chrono::steady_clock::time_point;
    virtual ~SquirrelHost() = default;
    virtual Settings loadConfiguration() = 0;
    virtual void dispatch(CompletionClient::Task task) = 0;
    virtual std::optional<TextWindow> capture(size_t contextChars) = 0;
    // 以固定窗口起点回读，不能随着光标移动重新选择一个看似相同的窗口。
    virtual std::optional<TextWindow> read(const TextWindow &expected) = 0;
    virtual bool currentTarget(const TextTarget &target) = 0;
    virtual Time now() const = 0;
    virtual void startMonitor(std::function<void()> tick) = 0;
    virtual void stopMonitor() = 0;
};

std::unique_ptr<SquirrelHost> createSquirrelHost(const std::filesystem::path &userDirectory);
} // namespace rome
