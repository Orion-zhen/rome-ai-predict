#include "completion.h"
#include "settings.h"

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx/action.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

namespace rome {
using namespace fcitx;

constexpr uint64_t kSurroundingTimeoutUs = 1'000'000;

// Fcitx surrounding text 的偏移单位是 Unicode 码点，不是 UTF-8 字节。
struct Snapshot {
    std::string text;
    unsigned int cursor;
    unsigned int anchor;
    bool operator==(const Snapshot &) const = default;

    static std::optional<Snapshot> read(InputContext *ic) {
        const auto &s = ic->surroundingText();
        if (!s.isValid()) {
            return std::nullopt;
        }
        return Snapshot{s.text(), s.cursor(), s.anchor()};
    }

    Snapshot afterCommit(const std::string &commit) const {
        const auto begin = std::min(cursor, anchor);
        const auto end = std::max(cursor, anchor);
        const auto first = utf8::ncharByteLength(text.begin(), begin);
        const auto last = utf8::ncharByteLength(text.begin(), end);
        const auto position = begin + static_cast<unsigned int>(utf8::length(commit));
        return {text.substr(0, first) + commit + text.substr(last), position, position};
    }

    std::string prefix(unsigned int maximum) const {
        const auto start = cursor > maximum ? cursor - maximum : 0;
        const auto first = utf8::ncharByteLength(text.begin(), start);
        const auto last = utf8::ncharByteLength(text.begin(), cursor);
        return text.substr(first, last - first);
    }
};

class PhraseCandidate final : public CandidateWord {
public:
    PhraseCandidate(std::string phrase, std::function<void(InputContext *)> select)
        : CandidateWord(Text(std::move(phrase))), select_(std::move(select)) {
        setComment(Text("AI"));
    }

    void select(InputContext *ic) const override {
        // 上屏会清除列表。复制回调，避免调用过程中销毁正在执行的函数对象。
        auto action = select_;
        action(ic);
    }

private:
    std::function<void(InputContext *)> select_;
};

class PredictAddon final : public AddonInstance {
public:
    explicit PredictAddon(Instance *instance) : instance_(instance) {
        timer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, 0, 0, [this](EventSourceTime *, uint64_t) {
                timer_->setEnabled(false);
                cancel("surrounding-timeout");
                return true;
            });
        timer_->setEnabled(false);
        releaseCleanup_ = instance_->eventLoop().addDeferEvent([this](EventSource *) {
            releaseRefresh_.reset();
            return false;
        });
        releaseCleanup_->setEnabled(false);

        watch(EventType::InputContextCommitString, EventWatcherPhase::PreInputMethod,
              [this](Event &event) { onCommit(static_cast<CommitStringEvent &>(event)); });
        watch(EventType::InputContextSurroundingTextUpdated, EventWatcherPhase::Default,
              [this](Event &event) { onSurrounding(context(event)); });
        watch(EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
              [this](Event &event) { onKey(static_cast<KeyEvent &>(event)); });
        watch(EventType::InputContextKeyEvent, EventWatcherPhase::PostInputMethod,
              [this](Event &) { clearReleaseRefresh(); });
        for (auto type : {EventType::InputContextFocusOut,
                          EventType::InputContextDestroyed,
                          EventType::InputContextReset,
                          EventType::InputContextSwitchInputMethod,
                          EventType::InputContextInputMethodDeactivated,
                          EventType::InputContextCapabilityAboutToChange,
                          EventType::InputContextCommitStringWithCursor,
                          EventType::InputContextDeleteSurroundingText}) {
            watch(type, EventWatcherPhase::PreInputMethod, [this](Event &event) {
                if (owns(context(event))) {
                    cancel("context-invalidated");
                }
            });
        }
        configPath_ = StandardPaths::global().userDirectory(StandardPathsType::PkgConfig) /
                      "conf/rome-ai-predict.yaml";
        toggle_.setCheckable(true);
        toggle_.setIcon("input-keyboard");
        toggle_.connect<SimpleAction::Activated>([this](InputContext *ic) {
            if (enabled_) {
                enabled_ = false;
                cancel("menu-disabled");
            } else {
                // 点击开启时重新读取 API 参数，允许先配置再从菜单直接启用。
                enabled_ = readConfiguration(true);
            }
            updateToggle(ic);
        });
        instance_->userInterfaceManager().registerAction("rome-ai-predict-toggle", &toggle_);
        for (auto type : {EventType::InputContextFocusIn,
                          EventType::InputContextInputMethodActivated,
                          EventType::InputContextSwitchInputMethod}) {
            watch(type, EventWatcherPhase::Default,
                  [this](Event &event) { attachToggle(context(event)); });
        }
        watch(EventType::InputContextUpdateUI, EventWatcherPhase::PreInputMethod,
              [this](Event &event) {
                  auto &update = static_cast<InputContextUpdateUIEvent &>(event);
                  if (update.component() == UserInterfaceComponent::StatusArea) {
                      // 松键若改变了 Rime 模式，优先保留原输入法的新状态。
                      if (owns(update.inputContext())) clearReleaseRefresh();
                      attachToggle(update.inputContext());
                  } else if (update.component() == UserInterfaceComponent::InputPanel) {
                      restoreAfterRelease(update.inputContext());
                  }
              });
        watch(EventType::GlobalConfigReloaded, EventWatcherPhase::Default,
              [this](Event &) { reloadConfig(); });
        reloadConfig();
        FCITX_INFO() << "rome-ai-predict: loaded (completions API, menu toggle)";
    }

    ~PredictAddon() override {
        cancel("unload");
        client_.reset();
        instance_->userInterfaceManager().unregisterAction(&toggle_);
    }

    void reloadConfig() override {
        cancel("configuration-reloaded");
        enabled_ = readConfiguration(false);
        updateToggle(nullptr);
    }

private:
    enum class Stage { WaitingSurrounding, Generating, Showing };
    struct Session {
        TrackableObjectReference<InputContext> ic;
        uint64_t generation;
        Snapshot before;
        Snapshot expected;
        Stage stage;
        std::weak_ptr<CandidateList> candidates;
        Cancellation request;
    };
    struct ReleaseRefresh {
        uint64_t generation;
        std::vector<std::string> tokens;
    };

    void clearReleaseRefresh() {
        releaseRefresh_.reset();
        releaseCleanup_->setEnabled(false);
    }

    void restoreAfterRelease(InputContext *ic) {
        if (!releaseRefresh_ || !owns(ic)) return;
        auto saved = std::move(*releaseRefresh_);
        // show() 会再次发出 UI 更新，先消耗这次恢复权限，避免重入。
        clearReleaseRefresh();
        if (session_->generation != saved.generation || session_->stage != Stage::Showing) return;
        // 仅接回被松键清空的面板，不能覆盖 Rime 新的组合或其他扩展内容。
        if (ic->inputPanel().empty()) show(saved.tokens);
    }

    bool readConfiguration(bool activate) {
        configured_ = false;
        try {
            settings_ = loadSettings(configPath_);
            validateSettings(settings_);
            configured_ = true;
        } catch (const std::exception &) {
            // 配置错误只使 AI 不可用，不能阻止 Rime 初始化或上屏。
            FCITX_DEBUG() << "rome-ai-predict: API configuration invalid or incomplete";
            return false;
        }
        if (!(activate || settings_.enabled)) return false;
        if (!client_) {
            client_ = std::make_unique<CompletionClient>(instance_->eventLoop(),
                [this](PredictionResult result) { onResult(std::move(result)); });
        }
        return true;
    }

    void updateToggle(InputContext *ic) {
        toggle_.setChecked(enabled_);
        toggle_.setShortText(!configured_ ? "AI 联想（未配置）" :
                             enabled_ ? "AI 联想：开" : "AI 联想：关");
        toggle_.setLongText("点击启停上屏后联想，API 参数见 conf/rome-ai-predict.yaml");
        toggle_.update(ic);
        if (ic) ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    }

    void attachToggle(InputContext *ic) {
        const auto actions = ic->statusArea().actions(StatusGroup::AfterInputMethod);
        const bool attached = std::find(actions.begin(), actions.end(), &toggle_) != actions.end();
        const bool rime = instance_->inputMethod(ic) == "rime";
        if (rime && !attached) {
            ic->statusArea().addAction(StatusGroup::AfterInputMethod, &toggle_);
        } else if (!rime && attached) {
            ic->statusArea().removeAction(&toggle_);
        }
    }

    void onResult(PredictionResult result) {
        if (!session_ || session_->generation != result.generation) return;
        if (auto *error = std::get_if<std::string>(&result.value)) {
            FCITX_WARN() << "rome-ai-predict: " << *error;
            cancel("request-failed");
            return;
        }
        auto &phrases = std::get<std::vector<std::string>>(result.value);
        if (phrases.empty()) {
            cancel("no-candidates");
            return;
        }
        show(phrases);
    }

    static InputContext *context(Event &event) {
        return static_cast<InputContextEvent &>(event).inputContext();
    }

    void watch(EventType type, EventWatcherPhase phase, EventHandler callback) {
        handlers_.push_back(instance_->watchEvent(type, phase, std::move(callback)));
    }

    bool owns(InputContext *ic) const {
        return session_ && session_->ic.get() == ic;
    }

    bool eligible(InputContext *ic) const {
        return enabled_ && ic->hasFocus() && instance_->inputMethod(ic) == "rime" &&
               ic->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
               !ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive);
    }

    void arm(uint64_t delay) {
        timer_->setTime(now(CLOCK_MONOTONIC) + delay);
        timer_->setOneShot();
    }

    void cancel(const char *reason) {
        clearReleaseRefresh();
        timer_->setEnabled(false);
        if (!session_) {
            return;
        }
        auto state = std::move(*session_);
        session_.reset();
        if (state.request) state.request->store(true);
        auto *ic = state.ic.get();
        // 只清除自己仍然拥有的候选，不覆盖 Rime 或其他 addon 的新面板。
        auto list = state.candidates.lock();
        if (ic && list && ic->inputPanel().candidateList() == list) {
            ic->inputPanel().setCandidateList(nullptr);
            ic->inputPanel().setAuxUp(Text());
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        }
        FCITX_DEBUG() << "rome-ai-predict: cancel=" << reason;
    }

    void onCommit(const CommitStringEvent &event) {
        auto *ic = event.inputContext();
        if (!eligible(ic) || event.text().empty()) {
            if (owns(ic)) {
                cancel("ineligible-commit");
            }
            return;
        }
        cancel("new-commit");
        const auto before = Snapshot::read(ic);
        if (!before) {
            FCITX_INFO() << "rome-ai-predict: skip (surrounding text unavailable)";
            return;
        }
        session_ = Session{ic->watch(), ++generation_, *before,
                           before->afterCommit(event.text()),
                           Stage::WaitingSurrounding, {}, {}};
        // commit 事件早于应用收到文本。必须等应用确认，不能直接拿旧前文预测。
        arm(kSurroundingTimeoutUs);
    }

    void onSurrounding(InputContext *ic) {
        if (!owns(ic)) {
            return;
        }
        const auto snapshot = Snapshot::read(ic);
        if (!snapshot || !eligible(ic)) {
            cancel("surrounding-unavailable");
            return;
        }
        if (session_->stage == Stage::WaitingSurrounding) {
            if (*snapshot == session_->expected) {
                timer_->setEnabled(false);
                session_->stage = Stage::Generating;
                const auto prefix = session_->expected.prefix(settings_.contextCharacters);
                session_->request = client_->submit(session_->generation, settings_, prefix);
                FCITX_DEBUG() << "rome-ai-predict: request=" << session_->generation
                              << " context_chars=" << utf8::length(prefix);
            } else if (*snapshot != session_->before) {
                cancel("unexpected-edit");
            }
        } else if (*snapshot != session_->expected) {
            cancel("surrounding-changed");
        }
    }

    bool current(InputContext *ic, uint64_t generation) const {
        return owns(ic) && session_->generation == generation && eligible(ic) &&
               Snapshot::read(ic) == session_->expected;
    }

    void show(const std::vector<std::string> &phrases) {
        auto *ic = session_->ic.get();
        if (!ic || !current(ic, session_->generation)) {
            cancel("stale-result");
            return;
        }
        auto &panel = ic->inputPanel();
        const auto existing = panel.candidateList();
        if (!panel.preedit().empty() || !panel.clientPreedit().empty() ||
            (existing && !existing->empty())) {
            cancel("composition-active");
            return;
        }
        auto list = std::make_unique<CommonCandidateList>();
        KeyList keys;
        for (size_t i = 0; i < phrases.size(); ++i) {
            keys.emplace_back(static_cast<KeySym>(FcitxKey_1 + i));
        }
        list->setSelectionKey(keys);
        list->setPageSize(settings_.candidates);
        for (const auto &phrase : phrases) {
            const auto generation = session_->generation;
            list->append<PhraseCandidate>(phrase, [this, generation, phrase](InputContext *target) {
                if (!current(target, generation) || session_->stage != Stage::Showing) {
                    return;
                }
                auto owned = session_->candidates.lock();
                if (!owned || target->inputPanel().candidateList() != owned) {
                    return;
                }
                cancel("selected");
                target->commitString(phrase);
            });
        }
        list->setGlobalCursorIndex(0);
        panel.setCandidateList(std::move(list));
        panel.setAuxUp(Text("Rome AI · Tab/数字选择 · Esc 关闭"));
        session_->candidates = panel.candidateList();
        session_->stage = Stage::Showing;
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        FCITX_DEBUG() << "rome-ai-predict: shown (candidates=" << phrases.size() << ")";
    }

    void onKey(KeyEvent &event) {
        clearReleaseRefresh();
        if (!owns(event.inputContext())) return;
        auto *ic = event.inputContext();
        if (event.isRelease()) {
            auto list = session_->candidates.lock();
            if (session_->stage == Stage::Showing && list && list == ic->inputPanel().candidateList()) {
                ReleaseRefresh saved{session_->generation, {}};
                for (int i = 0; i < list->size(); ++i) {
                    saved.tokens.push_back(list->candidate(i).text().toString());
                }
                releaseRefresh_ = std::move(saved);
                // 其他处理器可能过滤事件，使 PostInputMethod 不执行。恢复权限
                // 最迟在本轮事件循环结束时过期，不能用于之后无关的 UI 更新。
                releaseCleanup_->setOneShot();
            }
            // 仍交给 Rime 处理，不吞松键。若它清空面板，在同次 UI 刷新中恢复。
            return;
        }
        if (session_->stage == Stage::Showing) {
            auto list = session_->candidates.lock();
            if (list && list == ic->inputPanel().candidateList()) {
                // 按逻辑按键匹配，不把真实键盘携带的物理键码与配置的 0 比较。
                // digitSelection 同时支持数字行和小键盘，并检查修饰键。
                int index = event.key().digitSelection();
                if (event.key().check(FcitxKey_Tab)) index = 0;
                if (index >= 0 && index < list->size()) {
                    event.filterAndAccept();
                    list->candidate(index).select(ic);
                    return;
                }
                if (event.key().check(FcitxKey_Escape)) {
                    event.filterAndAccept();
                }
            }
        }
        // 空格和字母不被联想吞掉，清除联想后继续交给原输入法。
        cancel("key-pressed");
    }

    Instance *instance_;
    std::filesystem::path configPath_;
    Settings settings_;
    bool enabled_ = false;
    bool configured_ = false;
    SimpleAction toggle_;
    std::unique_ptr<CompletionClient> client_;
    uint64_t generation_ = 0;
    std::optional<Session> session_;
    std::unique_ptr<EventSourceTime> timer_;
    std::optional<ReleaseRefresh> releaseRefresh_;
    std::unique_ptr<EventSource> releaseCleanup_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>> handlers_;
};

class PredictFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new PredictAddon(manager->instance());
    }
};
} // namespace rome

FCITX_ADDON_FACTORY(rome::PredictFactory)
