#pragma once
#include "text_host.h"
#include <memory>
#include <optional>
#include <string_view>

namespace rome::weasel {
struct Focus {
    HWND root = nullptr;
    HWND control = nullptr;
    DWORD process = 0;
    HKL layout = nullptr;
    bool operator==(const Focus&) const = default;
};

// capture/current 不读取正文；wake 只向匹配的前台发送 F24。
// UIA 读取在 Session 的 MTA 工作线程执行。
class Host {
public:
    virtual ~Host() = default;
    virtual std::optional<Focus> capture(std::string_view app, std::string_view clientType) const = 0;
    virtual bool current(const Focus& focus) const = 0;
    virtual std::optional<Sample> read(TextReader& reader, const Focus& focus, TextRead mode = TextRead::Plain) const = 0;
    virtual bool wake(const Focus& focus) const = 0;
};
std::unique_ptr<Host> createWindowsHost();
} // namespace rome::weasel
