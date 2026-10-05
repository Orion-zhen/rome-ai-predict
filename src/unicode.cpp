#include "unicode.h"

#include <cstdint>

namespace rome::detail {
std::optional<std::vector<UnicodeBoundary>> unicodeBoundaries(std::string_view text) {
    std::vector<UnicodeBoundary> boundaries{{0, 0}};
    size_t utf16 = 0;
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        size_t width;
        uint32_t codepoint;
        uint32_t minimum;
        if (first < 0x80) {
            width = 1;
            codepoint = first;
            minimum = 0;
        } else if (first >= 0xc2 && first <= 0xdf) {
            width = 2;
            codepoint = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3;
            codepoint = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4;
            codepoint = first & 0x07;
            minimum = 0x10000;
        } else {
            return std::nullopt;
        }
        if (width > text.size() - i) return std::nullopt;
        for (size_t j = 1; j < width; ++j) {
            const auto byte = static_cast<unsigned char>(text[i + j]);
            if ((byte & 0xc0) != 0x80) return std::nullopt;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return std::nullopt;
        }
        i += width;
        utf16 += codepoint > 0xffff ? 2 : 1;
        boundaries.push_back({i, utf16});
    }
    return boundaries;
}
} // namespace rome::detail
