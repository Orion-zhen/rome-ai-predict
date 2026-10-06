#pragma once
#include "surrounding.h"
#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <optional>
#include <string>
#include <vector>

namespace rome::weasel {
using Microsoft::WRL::ComPtr;
struct Sample {
    HWND target;
    std::vector<int> identity;
    SurroundingSnapshot snapshot;
    // UIA 提供的可选范围提示，可能是已提交范围，不能据此判断是否正在组字。
    std::optional<SurroundingSnapshot> composition;
    bool operator==(const Sample&) const = default;
    Sample afterCommit(std::string_view text) const {
        const auto& range = composition ? *composition : snapshot;
        return {target, identity, range.afterCommit(text), std::nullopt};
    }
    Sample plain() const { return {target, identity, snapshot, std::nullopt}; }
    bool matches(const Sample& expected, bool ownsComposition) const {
        if (target == expected.target && identity == expected.identity && snapshot == expected.snapshot) return true;
        return ownsComposition && composition && !expected.composition &&
            expected.snapshot.cursor() == expected.snapshot.anchor() &&
            composition->anchor() == expected.snapshot.cursor() && afterCommit("") == expected;
    }
};

enum class TextRead { Plain, CompositionHint };

// UIA 对象的创建、使用和释放均在同一个 MTA 工作线程。
class TextReader {
public:
    TextReader();
    ~TextReader();
    std::optional<Sample> readFocused(HWND root, HWND control, DWORD process, TextRead mode = TextRead::Plain);
private:
    std::optional<Sample> readElement(HWND target, IUIAutomationElement* element, TextRead mode);
    ComPtr<IUIAutomation> automation_;
};
std::wstring wide(std::string_view utf8);
} // namespace rome::weasel
