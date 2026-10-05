#include "squirrel_host.h"
#include "unicode.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#include <algorithm>
#include <dlfcn.h>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace rome {
namespace {
struct ReleaseCF {
    void operator()(const void *value) const { CFRelease(value); }
};
template <typename T> using Owned = std::unique_ptr<std::remove_pointer_t<T>, ReleaseCF>;

Owned<CFTypeRef> attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) return {};
    return Owned<CFTypeRef>(value);
}

bool stringEquals(const Owned<CFTypeRef> &value, CFStringRef expected) {
    return value && CFGetTypeID(value.get()) == CFStringGetTypeID() && CFEqual(value.get(), expected);
}

bool squirrelActive() {
    Owned<TISInputSourceRef> source(TISCopyCurrentKeyboardInputSource());
    if (!source) return false;
    const auto identifier = static_cast<CFStringRef>(
        TISGetInputSourceProperty(source.get(), kTISPropertyInputSourceID));
    return identifier && CFStringHasPrefix(identifier, CFSTR("im.rime.inputmethod.Squirrel"));
}

class AXTarget final : public TextTarget {
public:
    AXTarget(AXUIElementRef element, pid_t pid) : element_(element), pid_(pid) { CFRetain(element); }
    bool equals(const TextTarget &other) const override {
        const auto *target = dynamic_cast<const AXTarget *>(&other);
        return target && pid_ == target->pid_ && CFEqual(element_.get(), target->element_.get());
    }
    AXUIElementRef element() const { return element_.get(); }
    pid_t pid() const { return pid_; }
private:
    Owned<AXUIElementRef> element_;
    pid_t pid_;
};

std::shared_ptr<const AXTarget> focusedTarget() {
    if (!AXIsProcessTrusted() || IsSecureEventInputEnabled() || !squirrelActive()) return {};
    Owned<AXUIElementRef> system(AXUIElementCreateSystemWide());
    // AX 调用是同步 IPC。无响应的应用不能以默认长超时阻塞输入事件线程。
    if (AXUIElementSetMessagingTimeout(system.get(), 0.05f) != kAXErrorSuccess) return {};
    auto focus = attribute(system.get(), kAXFocusedUIElementAttribute);
    if (!focus || CFGetTypeID(focus.get()) != AXUIElementGetTypeID()) return {};
    auto element = reinterpret_cast<AXUIElementRef>(const_cast<void *>(focus.get()));
    if (AXUIElementSetMessagingTimeout(element, 0.05f) != kAXErrorSuccess) return {};
    pid_t pid = 0;
    if (AXUIElementGetPid(element, &pid) != kAXErrorSuccess || pid <= 0 ||
        NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != pid) return {};
    auto role = attribute(element, kAXRoleAttribute);
    auto subrole = attribute(element, kAXSubroleAttribute);
    if (stringEquals(subrole, kAXSecureTextFieldSubrole)) return {};
    // 仅支持明确的文本角色。TextField 缺少 subrole 时不能排除密码框。
    if (!stringEquals(role, kAXTextAreaRole) &&
        !(stringEquals(role, kAXTextFieldRole) && subrole &&
          CFGetTypeID(subrole.get()) == CFStringGetTypeID())) return {};
    return std::make_shared<AXTarget>(element, pid);
}

struct Selection {
    CFIndex characters;
    CFRange range;
    bool operator==(const Selection &other) const {
        return characters == other.characters && range.location == other.range.location &&
               range.length == other.range.length;
    }
};

std::optional<Selection> selection(AXUIElementRef element) {
    auto count = attribute(element, kAXNumberOfCharactersAttribute);
    auto selected = attribute(element, kAXSelectedTextRangeAttribute);
    if (!count || !selected || CFGetTypeID(count.get()) != CFNumberGetTypeID() ||
        CFGetTypeID(selected.get()) != AXValueGetTypeID()) return std::nullopt;
    Selection result{};
    if (!CFNumberGetValue(static_cast<CFNumberRef>(count.get()), kCFNumberCFIndexType, &result.characters) ||
        AXValueGetType(static_cast<AXValueRef>(selected.get())) != kAXValueCFRangeType ||
        !AXValueGetValue(static_cast<AXValueRef>(selected.get()), kAXValueCFRangeType, &result.range) ||
        result.characters < 0 || result.range.location < 0 || result.range.length < 0 ||
        result.range.location > result.characters ||
        result.range.length > result.characters - result.range.location) return std::nullopt;
    return result;
}

Owned<CFTypeRef> substring(AXUIElementRef element, CFRange range) {
    Owned<AXValueRef> parameter(AXValueCreate(kAXValueCFRangeType, &range));
    CFTypeRef value = nullptr;
    if (AXUIElementCopyParameterizedAttributeValue(element, kAXStringForRangeParameterizedAttribute,
            parameter.get(), &value) != kAXErrorSuccess) return {};
    Owned<CFTypeRef> result(value);
    if (!result || CFGetTypeID(result.get()) != CFStringGetTypeID() ||
        CFStringGetLength(static_cast<CFStringRef>(result.get())) != range.length) return {};
    return result;
}

constexpr CFIndex kSelectionLimit = 16384;
constexpr CFIndex kWindowLimit = 40000;
constexpr CFIndex kSuffixUnits = 128;

class AXHost final : public SquirrelHost {
public:
    explicit AXHost(std::filesystem::path userDirectory)
        : userDirectory_(std::move(userDirectory)) {}
    ~AXHost() override { stopMonitor(); }
    Settings loadConfiguration() override {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<const void *>(&createSquirrelHost), &info) || !info.dli_fname) {
            throw std::runtime_error("cannot locate Squirrel plugin defaults");
        }
        const auto defaults = std::filesystem::path(info.dli_fname).parent_path() /
                              "rome-ai-predict.defaults.yaml";
        return loadSettings(defaults, userDirectory_ / "rome-ai-predict.yaml");
    }
    void dispatch(CompletionClient::Task task) override {
        dispatch_async(dispatch_get_main_queue(), ^{ task(); });
    }
    Time now() const override { return std::chrono::steady_clock::now(); }
    void startMonitor(std::function<void()> tick) override {
        stopMonitor();
        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                                  50 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer_, ^{ tick(); });
        dispatch_resume(timer_);
    }
    void stopMonitor() override {
        if (timer_) {
            dispatch_source_cancel(timer_);
            timer_ = nullptr;
        }
    }
    bool currentTarget(const TextTarget &target) override {
        @autoreleasepool {
            auto focused = focusedTarget();
            return focused && target.equals(*focused);
        }
    }
    std::optional<TextWindow> capture(size_t contextChars) override {
        @autoreleasepool {
            static bool prompted = false;
            if (!AXIsProcessTrusted()) {
                if (!prompted) {
                    prompted = true;
                    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)
                        @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES});
                }
                return std::nullopt;
            }
            auto target = focusedTarget();
            if (!target) return std::nullopt;
            auto state = selection(target->element());
            if (!state || state->range.length > kSelectionLimit) return std::nullopt;
            const auto margin = static_cast<CFIndex>(2 * contextChars);
            const auto origin = std::max<CFIndex>(0, state->range.location - margin);
            const auto selectionEnd = state->range.location + state->range.length;
            const auto end = selectionEnd + std::min(kSuffixUnits, state->characters - selectionEnd);
            return sample(target, *state, CFRangeMake(origin, end - origin), true);
        }
    }
    std::optional<TextWindow> read(const TextWindow &expected) override {
        @autoreleasepool {
            const auto *target = dynamic_cast<const AXTarget *>(expected.target.get());
            if (!target || !currentTarget(*target) || expected.originUtf16 >
                static_cast<size_t>(std::numeric_limits<CFIndex>::max())) return std::nullopt;
            auto state = selection(target->element());
            if (!state) return std::nullopt;
            const auto boundaries = detail::unicodeBoundaries(expected.snapshot.text());
            const auto length = boundaries->back().utf16;
            const auto origin = static_cast<CFIndex>(expected.originUtf16);
            if (length > static_cast<size_t>(kWindowLimit) || origin > state->characters) return std::nullopt;
            // 应用尚未接收提交时，文档可能比预期窗口短。回读真实可用范围等待确认。
            const auto available = std::min(static_cast<CFIndex>(length), state->characters - origin);
            auto borrowed = std::make_shared<AXTarget>(target->element(), target->pid());
            return sample(borrowed, *state, CFRangeMake(origin, available), false);
        }
    }
private:
    std::optional<TextWindow> sample(const std::shared_ptr<const AXTarget> &target,
                                    const Selection &state, CFRange range, bool trimEdges) {
        if (range.length > kWindowLimit) return std::nullopt;
        auto text = substring(target->element(), range);
        if (!text) return std::nullopt;
        // 验证两次文本和选区读取一致，并重新核实焦点。AX 没有原子快照接口。
        auto again = substring(target->element(), range);
        auto newState = selection(target->element());
        if (!again || !CFEqual(text.get(), again.get()) || !newState || *newState != state ||
            !currentTarget(*target)) return std::nullopt;
        NSString *string = (__bridge NSString *)static_cast<CFStringRef>(text.get());
        if (trimEdges && string.length) {
            if (CFStringIsSurrogateLowCharacter([string characterAtIndex:0])) {
                string = [string substringFromIndex:1];
                ++range.location;
                --range.length;
            }
            if (string.length && CFStringIsSurrogateHighCharacter([string characterAtIndex:string.length - 1])) {
                string = [string substringToIndex:string.length - 1];
                --range.length;
            }
        }
        const auto end = range.location + range.length;
        const auto caret = state.range.location + state.range.length;
        if (state.range.location < range.location || caret > end) return std::nullopt;
        NSData *utf8 = [string dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
        if (!utf8) return std::nullopt;
        std::string bytes;
        if (utf8.length) bytes.assign(static_cast<const char *>(utf8.bytes), utf8.length);
        auto snapshot = SurroundingSnapshot::fromUtf8(std::move(bytes),
            static_cast<size_t>(caret - range.location),
            static_cast<size_t>(state.range.location - range.location), OffsetUnit::Utf16);
        if (!snapshot) return std::nullopt;
        return TextWindow{target, static_cast<size_t>(range.location), std::move(*snapshot)};
    }
    std::filesystem::path userDirectory_;
    dispatch_source_t timer_ = nullptr;
};
} // namespace

std::unique_ptr<SquirrelHost> createSquirrelHost(const std::filesystem::path &userDirectory) {
    return std::make_unique<AXHost>(userDirectory);
}
} // namespace rome
