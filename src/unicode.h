#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace rome::detail {
struct UnicodeBoundary {
    size_t byte;
    size_t utf16;
};

// 首项为 {0, 0}，之后每项对应一个 Unicode 码点的结束位置。
// 拒绝过长编码、代理码点、不完整序列和 U+10FFFF 以外的值。
std::optional<std::vector<UnicodeBoundary>> unicodeBoundaries(std::string_view text);
} // namespace rome::detail
