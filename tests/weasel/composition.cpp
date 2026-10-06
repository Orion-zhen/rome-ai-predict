#include "text_host.h"
#include <cstdio>
#include <stdexcept>
#include <utility>

using namespace rome;
using namespace rome::weasel;
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
Sample sample(const std::string& text, size_t cursor, size_t anchor,
              std::optional<std::pair<size_t, size_t>> composing = {}) {
    auto snapshot = SurroundingSnapshot::fromUtf8(text, cursor, anchor, OffsetUnit::Utf16);
    require(snapshot.has_value(), "invalid test selection");
    std::optional<SurroundingSnapshot> composition;
    if (composing) {
        composition = SurroundingSnapshot::fromUtf8(text, composing->second, composing->first, OffsetUnit::Utf16);
        require(composition.has_value(), "invalid test composition");
    }
    return {reinterpret_cast<HWND>(1), {7, 8}, std::move(*snapshot), std::move(composition)};
}
}
int main() {
    try {
        // 只有明确的组字范围可以替换；这不是实际 UIA/TSF provider 测试。
        require(sample("前文 ", 2, 2).afterCommit("去") == sample("前文去 ", 3, 3), "ordinary space was removed");
        require(sample("前文 ", 2, 2, {{2, 3}}).afterCommit("去") == sample("前文去", 3, 3), "composition was appended");
        require(sample("前qu后", 2, 2, {{1, 3}}).afterCommit("去") == sample("前去后", 2, 2), "inline preedit was mishandled");
        require(sample("前原词后", 3, 1).afterCommit("去") == sample("前去后", 2, 2), "selection replacement changed");
        const auto unicode = sample("今天😀qu后", 6, 6, {{4, 6}}).afterCommit("去");
        require(unicode == sample("今天😀去后", 5, 5), "UTF-16 offsets were mishandled");
        const auto retained = sample("今天😀去后", 5, 5, {{4, 5}});
        require(retained.matches(unicode, false), "retained UIA range blocked matching committed text");
        require(sample("今天😀去后", 5, 5, {{0, 1}}).matches(unicode, false), "stale range outside selection blocked plain comparison");
        require(sample("前文", 2, 2, {{2, 2}}).afterCommit("去") == sample("前文去", 3, 3), "empty composition was mishandled");
        const auto base = sample("前文", 2, 2);
        const auto menu = sample("前文 ", 2, 2, {{2, 3}});
        require(!menu.matches(base, false), "unowned composition bypassed validation");
        require(menu.matches(base, true), "owned menu composition rejected");
        require(!sample("前文 ", 2, 2).matches(base, true), "ordinary space ignored");
        require(!sample("错文 ", 2, 2, {{2, 3}}).matches(base, true), "prefix edit ignored");
        require(!sample("前文 错", 2, 2, {{2, 3}}).matches(base, true), "suffix edit ignored");
        require(!sample("前 文", 1, 1, {{1, 2}}).matches(base, true), "displaced composition accepted");
        auto other = base;
        other.identity = {9};
        require(!other.matches(base, true), "different UIA target accepted");
        std::puts("PASS: composition replacement, spaces, selection, UTF-16, menu ownership and target identity");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
