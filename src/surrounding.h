#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rome {
enum class OffsetUnit { CodePoints, Utf16 };

// 仅表示应用提供的文本和选区，不证明焦点、控件身份或文本读取的可靠性。
// 偏移相对于传入的文本。平台读取局部窗口时，必须先减去窗口起点。
class SurroundingSnapshot {
public:
    static std::optional<SurroundingSnapshot> fromUtf8(
        std::string text, size_t cursor, size_t anchor, OffsetUnit unit);

    const std::string &text() const { return text_; }
    // 内部统一使用 Unicode 码点，拒绝落在 UTF-16 代理对中间的偏移。
    size_t cursor() const { return cursor_; }
    size_t anchor() const { return anchor_; }
    bool operator==(const SurroundingSnapshot &other) const;

    std::string prefix(size_t maximum) const;
    // 计算替换选区后的预期文本。它不是应用已经接收提交的证据。
    // commit 必须是有效 UTF-8，否则抛出 invalid_argument。
    SurroundingSnapshot afterCommit(std::string_view commit) const;

private:
    SurroundingSnapshot(std::string text, size_t cursor, size_t anchor,
                        std::vector<size_t> bytes);
    std::string text_;
    size_t cursor_;
    size_t anchor_;
    std::vector<size_t> bytes_;
};
} // namespace rome
