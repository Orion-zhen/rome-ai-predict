#include "surrounding.h"
#include "unicode.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rome {
SurroundingSnapshot::SurroundingSnapshot(std::string text, size_t cursor, size_t anchor,
                                       std::vector<size_t> bytes)
    : text_(std::move(text)), cursor_(cursor), anchor_(anchor), bytes_(std::move(bytes)) {}

std::optional<SurroundingSnapshot> SurroundingSnapshot::fromUtf8(
    std::string text, size_t cursor, size_t anchor, OffsetUnit unit) {
    auto boundaries = detail::unicodeBoundaries(text);
    if (!boundaries) return std::nullopt;
    if (unit == OffsetUnit::Utf16) {
        const auto position = [&](size_t offset) -> std::optional<size_t> {
            const auto found = std::lower_bound(boundaries->begin(), boundaries->end(), offset,
                [](const detail::UnicodeBoundary &boundary, size_t value) {
                    return boundary.utf16 < value;
                });
            if (found == boundaries->end() || found->utf16 != offset) return std::nullopt;
            return static_cast<size_t>(found - boundaries->begin());
        };
        auto convertedCursor = position(cursor);
        auto convertedAnchor = position(anchor);
        if (!convertedCursor || !convertedAnchor) return std::nullopt;
        cursor = *convertedCursor;
        anchor = *convertedAnchor;
    } else if (cursor >= boundaries->size() || anchor >= boundaries->size()) {
        return std::nullopt;
    }
    std::vector<size_t> bytes;
    bytes.reserve(boundaries->size());
    for (const auto &boundary : *boundaries) bytes.push_back(boundary.byte);
    return SurroundingSnapshot(std::move(text), cursor, anchor, std::move(bytes));
}

bool SurroundingSnapshot::operator==(const SurroundingSnapshot &other) const {
    return text_ == other.text_ && cursor_ == other.cursor_ && anchor_ == other.anchor_;
}

std::string SurroundingSnapshot::prefix(size_t maximum) const {
    const auto start = cursor_ > maximum ? cursor_ - maximum : 0;
    return text_.substr(bytes_[start], bytes_[cursor_] - bytes_[start]);
}

SurroundingSnapshot SurroundingSnapshot::afterCommit(std::string_view commit) const {
    const auto boundaries = detail::unicodeBoundaries(commit);
    if (!boundaries) throw std::invalid_argument("commit must be valid UTF-8");
    const auto begin = std::min(cursor_, anchor_);
    const auto end = std::max(cursor_, anchor_);
    const auto position = begin + boundaries->size() - 1;
    auto text = text_.substr(0, bytes_[begin]);
    text.append(commit);
    text.append(text_, bytes_[end], std::string::npos);
    return *fromUtf8(std::move(text), position, position, OffsetUnit::CodePoints);
}
} // namespace rome
