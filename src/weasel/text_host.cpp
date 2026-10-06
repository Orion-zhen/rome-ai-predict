#include "text_host.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rome::weasel {
namespace {
constexpr int textLimit = 40000;
struct Bstr {
    BSTR value = nullptr;
    ~Bstr() { SysFreeString(value); }
    std::wstring_view view() const { return value ? std::wstring_view(value, SysStringLen(value)) : std::wstring_view{}; }
};
struct RangeBounds {
    size_t begin, end;
    bool operator==(const RangeBounds&) const = default;
};
ComPtr<IUIAutomationTextRange> selectionOf(IUIAutomationTextPattern* pattern) {
    ComPtr<IUIAutomationTextRangeArray> ranges;
    ComPtr<IUIAutomationTextRange> selection;
    int count = 0;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges || FAILED(ranges->get_Length(&count)) || count != 1 ||
        FAILED(ranges->GetElement(0, &selection))) return {};
    return selection;
}
std::optional<RangeBounds> boundsOf(IUIAutomationTextRange* document, IUIAutomationTextRange* range,
                                    std::wstring_view full) {
    ComPtr<IUIAutomationTextRange> before, through;
    if (FAILED(document->Clone(&before)) || !before || FAILED(document->Clone(&through)) || !through ||
        FAILED(before->MoveEndpointByRange(TextPatternRangeEndpoint_End, range, TextPatternRangeEndpoint_Start)) ||
        FAILED(through->MoveEndpointByRange(TextPatternRangeEndpoint_End, range, TextPatternRangeEndpoint_End))) return {};
    Bstr prefix, end;
    if (FAILED(before->GetText(textLimit + 1, &prefix.value)) ||
        FAILED(through->GetText(textLimit + 1, &end.value))) return {};
    const auto beginOffset = prefix.view().size(), endOffset = end.view().size();
    if (beginOffset > endOffset || endOffset > full.size() || endOffset - beginOffset > 16384 ||
        full.substr(0, beginOffset) != prefix.view() || full.substr(0, endOffset) != end.view()) return {};
    return RangeBounds{beginOffset, endOffset};
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = static_cast<int>(text.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size,
                                          nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid UTF-16");
    std::string bytes(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size,
                            bytes.data(), count, nullptr, nullptr)) {
        throw std::runtime_error("UTF-16 conversion failed");
    }
    return bytes;
}
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("UTF-8 string too long");
    const int size = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count))
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}

TextReader::TextReader() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        throw std::runtime_error("cannot initialize UI Automation apartment");
    const auto hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&automation_));
    if (FAILED(hr)) {
        CoUninitialize();
        throw std::runtime_error("cannot initialize UI Automation");
    }
    ComPtr<IUIAutomation2> timeouts;
    if (FAILED(automation_.As(&timeouts)) || FAILED(timeouts->put_ConnectionTimeout(500)) ||
        FAILED(timeouts->put_TransactionTimeout(500))) {
        timeouts.Reset(); automation_.Reset(); CoUninitialize();
        throw std::runtime_error("cannot configure UI Automation timeouts");
    }
}
TextReader::~TextReader() { automation_.Reset(); CoUninitialize(); }

std::optional<Sample> TextReader::readFocused(HWND root, HWND control, DWORD process, TextRead mode) {
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation_->GetFocusedElement(&element)) || !element) return {};
    int owner = 0;
    BOOL focused = FALSE;
    if (FAILED(element->get_CurrentProcessId(&owner)) || owner != static_cast<int>(process) ||
        FAILED(element->get_CurrentHasKeyboardFocus(&focused)) || !focused) return {};
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation_->get_RawViewWalker(&walker)) || !walker) return {};
    auto parent = element;
    UIA_HWND native{};
    for (unsigned depth = 0; depth < 64 && parent; ++depth) {
        if (FAILED(parent->get_CurrentNativeWindowHandle(&native))) return {};
        if (native) break;
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetParentElement(parent.Get(), &next))) return {};
        parent = std::move(next);
    }
    if (!native || GetAncestor(reinterpret_cast<HWND>(native), GA_ROOT) != root) return {};
    auto sample = readElement(control, element.Get(), mode);
    if (!sample) return {};
    ComPtr<IUIAutomationElement> now;
    BOOL same = FALSE;
    if (FAILED(automation_->GetFocusedElement(&now)) || !now ||
        FAILED(automation_->CompareElements(element.Get(), now.Get(), &same)) || !same) return {};
    return sample;
}
std::optional<Sample> TextReader::readElement(HWND target, IUIAutomationElement* element, TextRead mode) {
    BOOL password = TRUE;
    if (FAILED(element->get_CurrentIsPassword(&password)) || password) return {};
    CONTROLTYPEID role = 0;
    BOOL enabled = FALSE;
    if (FAILED(element->get_CurrentControlType(&role)) || (role != UIA_EditControlTypeId && role != UIA_DocumentControlTypeId) ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled) return {};
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) || !pattern) return {};
    ComPtr<IUIAutomationTextRange> document;
    if (FAILED(pattern->get_DocumentRange(&document)) || !document) return {};
    VARIANT readOnly;
    VariantInit(&readOnly);
    const auto readOnlyResult = document->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly);
    const bool editable = SUCCEEDED(readOnlyResult) && readOnly.vt == VT_BOOL && readOnly.boolVal == VARIANT_FALSE;
    VariantClear(&readOnly);
    if (!editable) return {};
    auto selection = selectionOf(pattern.Get());
    if (!selection) return {};
    Bstr text, again;
    if (FAILED(document->GetText(textLimit + 1, &text.value))) return {};
    const auto full = text.view();
    if (full.size() > textLimit) return {};
    const auto selected = boundsOf(document.Get(), selection.Get(), full);
    if (!selected) return {};

    // Chromium 会保留已提交的范围。只有调用方已知正在组字或拥有 AI 菜单时才查询提示。
    // 提示缺失、越界或变化只使投影不可用，不否定独立核实的正文和选区。
    std::optional<RangeBounds> composing;
    if (mode == TextRead::CompositionHint) {
        ComPtr<IUIAutomationTextEditPattern> edit;
        ComPtr<IUIAutomationTextRange> range, next;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextEditPatternId, IID_PPV_ARGS(&edit))) && edit &&
            edit->GetActiveComposition(&range) == S_OK && range) {
            composing = boundsOf(document.Get(), range.Get(), full);
            if (composing && (selected->begin < composing->begin || selected->end > composing->end ||
                edit->GetActiveComposition(&next) != S_OK || !next ||
                boundsOf(document.Get(), next.Get(), full) != composing)) composing.reset();
        }
    }
    // TextRange 可能随文档移动，复核冻结的数值边界而非只比较两个活动 Range 对象。
    auto newSelection = selectionOf(pattern.Get());
    if (!newSelection || boundsOf(document.Get(), newSelection.Get(), full) != selected) return {};
    if (FAILED(document->GetText(textLimit + 1, &again.value)) || full != again.view()) return {};
    const auto bytes = utf8(full);
    auto snapshot = SurroundingSnapshot::fromUtf8(bytes, selected->end, selected->begin, OffsetUnit::Utf16);
    if (!snapshot) return {};
    std::optional<SurroundingSnapshot> composition;
    if (composing) {
        composition = SurroundingSnapshot::fromUtf8(bytes, composing->end, composing->begin, OffsetUnit::Utf16);
    }
    SAFEARRAY* ids = nullptr;
    if (FAILED(element->GetRuntimeId(&ids)) || !ids) return {};
    struct ReleaseArray { SAFEARRAY* value; ~ReleaseArray() { SafeArrayDestroy(value); } } release{ids};
    LONG lower = 0, upper = -1;
    if (SafeArrayGetDim(ids) != 1 || FAILED(SafeArrayGetLBound(ids, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(ids, 1, &upper)) || upper < lower ||
        static_cast<int64_t>(upper) - lower >= 128) return {};
    std::vector<int> identity;
    for (LONG i = lower; i <= upper; ++i) {
        int value;
        if (FAILED(SafeArrayGetElement(ids, &i, &value))) return {};
        identity.push_back(value);
    }
    return Sample{target, std::move(identity), std::move(*snapshot), std::move(composition)};
}
} // namespace rome::weasel
