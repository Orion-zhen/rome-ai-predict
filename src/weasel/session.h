#pragma once
#include "completion.h"
#include "host.h"
#include <memory>
#include <string>
#include <vector>

namespace rome::weasel {
struct Command {
    enum Kind { Watch, Arm, Commit, Cancel, Select, Displayed, Composing, Idle } kind;
    std::string text;
    size_t index = 0;
    uint64_t epoch = 0;
    std::optional<Focus> focus;
    std::optional<Sample> before;
};
struct Event {
    std::string state;
    std::vector<std::string> tokens;
    std::string commit;
    bool woken = false;
    std::optional<Focus> focus;
};
// Lua 线程只提交命令。快照与确认在 MTA 工作线程处理，HTTP 使用共享异步客户端。
class Session {
public:
    Session(Settings settings, std::unique_ptr<Host> host);
    ~Session();
    void watch(std::string_view app, std::string_view clientType);
    void arm(std::string_view app, std::string_view clientType);
    void send(Command command);
    std::optional<Event> take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rome::weasel
