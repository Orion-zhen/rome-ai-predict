#include "host.h"
#include <filesystem>

namespace rome::weasel {
namespace {
std::optional<Focus> foreground() {
    Focus focus;
    focus.root = GetForegroundWindow();
    if (!focus.root) return {};
    const auto thread = GetWindowThreadProcessId(focus.root, &focus.process);
    GUITHREADINFO info{sizeof(info)};
    if (!thread || !focus.process || !GetGUIThreadInfo(thread, &info) || !info.hwndFocus) return {};
    focus.control = info.hwndFocus;
    if (GetAncestor(focus.control, GA_ROOT) != focus.root) return {};
    DWORD owner = 0;
    GetWindowThreadProcessId(focus.control, &owner);
    if (owner != focus.process) return {};
    focus.layout = GetKeyboardLayout(thread);
    if (!focus.layout) return {};
    return focus;
}
class WindowsHost final : public Host {
public:
    std::optional<Focus> capture(std::string_view app, std::string_view clientType) const override {
        // 这些属性由小狼毫为 Rime 会话设置，不从当前窗口猜测输入法客户端。
        if (clientType != "tsf" || app.empty()) return {};
        auto focus = foreground();
        if (!focus || focus->process == GetCurrentProcessId()) return {};
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, focus->process);
        if (!process) return {};
        wchar_t path[32768];
        DWORD length = static_cast<DWORD>(std::size(path));
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!queried) return {};
        try {
            const auto name = std::filesystem::path(std::wstring(path, length)).filename().wstring();
            const auto client = wide(app);
            if (CompareStringOrdinal(name.c_str(), static_cast<int>(name.size()), client.c_str(),
                                     static_cast<int>(client.size()), TRUE) != CSTR_EQUAL) return {};
        } catch (const std::exception&) { return {}; }
        return current(*focus) ? focus : std::nullopt;
    }
    bool current(const Focus& focus) const override {
        const auto now = foreground();
        return now && *now == focus;
    }
    std::optional<Sample> read(TextReader& reader, const Focus& focus, TextRead mode) const override {
        if (!current(focus)) return {};
        auto sample = reader.readFocused(focus.root, focus.control, focus.process, mode);
        return current(focus) ? std::move(sample) : std::nullopt;
    }
    bool wake(const Focus& focus) const override {
        if (!current(focus)) return false;
        for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
            if (GetAsyncKeyState(key) & 0x8000) return false;
        INPUT keys[2]{};
        for (auto& key : keys) {
            key.type = INPUT_KEYBOARD;
            key.ki.wVk = VK_F24;
            key.ki.dwExtraInfo = 0x524f4d45;
        }
        keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(2, keys, sizeof(INPUT)) == 2;
    }
};
}
std::unique_ptr<Host> createWindowsHost() { return std::make_unique<WindowsHost>(); }
} // namespace rome::weasel
