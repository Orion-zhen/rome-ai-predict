#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <fcitx/action.h>
#include <fcitx/statusarea.h>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rime_api.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/addonmanager.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonloader.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

using namespace fcitx;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool aiVisible(InputContext &ic) {
    auto list = ic.inputPanel().candidateList();
    return list && !list->empty() && list->candidate(0).comment().toString() == "AI";
}

// fcitx5 可执行文件内置的 keyboard addon 不在共享库里。测试注册一个直通键盘，
// 仅用于启动默认组和验证非 Rime 状态，不参与 Rime 按键或预测。
class TestKeyboard final : public InputMethodEngine {
public:
    std::vector<InputMethodEntry> listInputMethods() override {
        std::vector<InputMethodEntry> entries;
        entries.emplace_back("keyboard-us", "Test keyboard", "en", "keyboard");
        return entries;
    }
    void keyEvent(const InputMethodEntry &, KeyEvent &) override {}
};
class KeyboardFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *) override { return new TestKeyboard; }
};

// 模拟宿主编辑器。Rime 按键处理、上屏事件、扩展加载和 UI 调度均使用系统库。
class Editor final : public InputContext {
public:
    explicit Editor(Instance &instance) : InputContext(instance.inputContextManager(), "rome-ai-test") {
        created();
        setCapabilityFlags(CapabilityFlags{CapabilityFlag::SurroundingText,
                                           CapabilityFlag::Preedit,
                                           CapabilityFlag::ClientSideInputPanel});
    }
    ~Editor() override { destroy(); }
    const char *frontend() const override { return "rome-ai-test-editor"; }

    void setDocument(std::string text, unsigned int cursor, unsigned int anchor) {
        document = std::move(text);
        position = cursor;
        selection = anchor;
        publish();
    }

    void publish() {
        surroundingText().setText(document, position, selection);
        updateSurroundingText();
    }

    bool press(const Key &raw) {
        ++keyCount;
        KeyEvent event(this, raw);
        return keyEvent(event);
    }
    bool press(const std::string &name) { return press(Key(name)); }

    void release(const Key &raw) {
        KeyEvent event(this, raw, true);
        keyEvent(event);
    }
    void release(const std::string &name) { release(Key(name)); }

    bool key(const Key &raw) {
        const bool accepted = press(raw);
        release(raw);
        return accepted;
    }
    bool key(const std::string &name) { return key(Key(name)); }

    void commitStringImpl(const std::string &text) override {
        auto start = utf8::ncharByteLength(document.begin(), std::min(position, selection));
        auto end = utf8::ncharByteLength(document.begin(), std::max(position, selection));
        document.replace(start, end - start, text);
        position = std::min(position, selection) + utf8::length(text);
        selection = position;
        commits.push_back(text);
        if (publishCommits) {
            publish();
        }
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}
    void updateClientSideUIImpl() override {
        visibleFrames.push_back(aiVisible(*this));
        if (aiVisible(*this)) {
            ++aiFrames;
        }
    }

    std::string document;
    unsigned int position = 0;
    unsigned int selection = 0;
    bool publishCommits = true;
    int aiFrames = 0;
    int keyCount = 0;
    std::vector<std::string> commits;
    std::vector<bool> visibleFrames;
};

int main(int argc, char **argv) {
    require(argc == 2, "scenario required");
    const std::string scenario = argv[1];
    if (scenario == "--system-addon-dir") {
        std::cout << StandardPaths::fcitxPath("addondir").string() << '\n';
        return 0;
    }
    char name[] = "rome-ai-test";
    char disable[] = "--disable=all";
    std::string enable = "--enable=keyboard,rime";
    if (scenario != "no-extension") enable += ",rome-ai-predict";
    char *args[] = {name, disable, enable.data()};
    KeyboardFactory keyboardFactory;
    StaticAddonRegistry registry{{"keyboard", &keyboardFactory}};
    Instance instance(3, args);
    instance.addonManager().registerDefaultLoader(&registry);
    instance.initialize();
    instance.setRunning(true);
    require(bool(instance.addonManager().addon("rome-ai-predict")) == (scenario != "no-extension"),
            "optional extension loading matches configuration");
    require(instance.addonManager().addon("rime", true), "load unmodified system Fcitx5-Rime");
    rime_get_api()->join_maintenance_thread();

    auto editor = std::make_unique<Editor>(instance);
    editor->focusIn();
    instance.setCurrentInputMethod(editor.get(), "rime", true);
    require(instance.inputMethod(editor.get()) == "rime", "activate system Rime");
    editor->setDocument("今天晚上", 4, 4);
    auto findToggle = [&]() -> Action * {
        for (auto *action : editor->statusArea().allActions()) {
            if (action->name() == "rome-ai-predict-toggle") return action;
        }
        return nullptr;
    };
    require(bool(findToggle()) == (scenario != "no-extension"),
            "menu action appears only when extension is installed");
    if (scenario == "menu-on") {
        require(!findToggle()->isChecked(editor.get()), "menu initially off");
        findToggle()->activate(editor.get());
        require(findToggle()->isChecked(editor.get()), "menu can enable prediction");
    }

    std::vector<std::unique_ptr<EventSourceTime>> timers;
    auto at = [&](uint64_t milliseconds, std::function<void()> action) {
        auto timer = instance.eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + milliseconds * 1000, 1000,
            [action = std::move(action)](EventSourceTime *, uint64_t) {
                action();
                return false;
            });
        timer->setOneShot();
        timers.push_back(std::move(timer));
    };

    if (scenario == "unicode") {
        std::string text;
        for (int i = 0; i < 300; ++i) {
            text += "🙂";
        }
        editor->setDocument(text + "今天晚上尾巴", 304, 304);
    } else if (scenario == "no-context") {
        editor->surroundingText().invalidate();
    } else if (scenario == "stale-context" || scenario == "timeout") {
        editor->publishCommits = false;
    } else if (scenario == "selection") {
        editor->setDocument("今天XX尾巴", 2, 4);
    } else if (scenario == "other-im") {
        instance.setCurrentInputMethod(editor.get(), "keyboard-us", true);
        require(instance.inputMethod(editor.get()) == "keyboard-us", "activate non-Rime input method");
    }

    if (scenario == "other-im") {
        editor->commitString("一起");
    } else {
        // 通过真实 Rime 码表输入并选择“一起”，而不是直接触发插件私有函数。
        for (const char c : std::string("yiqi")) {
            editor->key(std::string(1, c));
        }
        auto list = editor->inputPanel().candidateList();
        require(list && !list->empty(), "Rime generated a real dictionary candidate");
        require(list->candidate(0).text().toString() == "一起", "Rime candidate is 一起");
        if (scenario.starts_with("release-")) {
            editor->press("space");
        } else {
            editor->key("space");
        }
        require(!editor->commits.empty() && editor->commits.back() == "一起", "Rime committed 一起");
    }
    require(!aiVisible(*editor), "prediction must not block the commit or appear synchronously");
    const auto keysAfterCommit = editor->keyCount;
    std::shared_ptr<CandidateList> oldCandidates;

    int propagatedReleases = 0;
    auto releaseObserver = instance.watchEvent(EventType::InputContextKeyEvent,
        EventWatcherPhase::PostInputMethod, [&](Event &event) {
            if (static_cast<KeyEvent &>(event).isRelease()) ++propagatedReleases;
        });
    auto releaseInterceptor = instance.watchEvent(EventType::InputContextKeyEvent,
        EventWatcherPhase::PreInputMethod, [&](Event &base) {
            auto &event = static_cast<KeyEvent &>(base);
            if (!event.isRelease() || event.key() != Key("space")) return;
            if (scenario == "release-cursor") {
                editor->setDocument(editor->document, 1, 1);
            } else if (scenario == "release-switch") {
                instance.setCurrentInputMethod(editor.get(), "keyboard-us", true);
            } else if (scenario == "release-panel") {
                auto list = std::make_unique<CommonCandidateList>();
                list->append<DisplayOnlyCandidateWord>(Text("other addon"));
                editor->inputPanel().setCandidateList(std::move(list));
                event.filterAndAccept();
                editor->updateUserInterface(UserInterfaceComponent::InputPanel);
            } else if (scenario == "release-filtered") {
                event.filterAndAccept();
            }
        });
    auto releaseWithoutFlicker = [&](const std::string &key) {
        const auto firstFrame = editor->visibleFrames.size();
        const auto before = propagatedReleases;
        editor->release(key);
        require(propagatedReleases == before + 1, "release propagated through Rime, not swallowed");
        require(aiVisible(*editor), "key release must not erase asynchronous AI candidates");
        require(std::all_of(editor->visibleFrames.begin() + firstFrame, editor->visibleFrames.end(),
                            [](bool visible) { return visible; }),
                "release must not render an empty intermediate frame");
    };

    if (scenario.starts_with("physical-")) {
        at(500, [&] {
            require(aiVisible(*editor), "AI candidates visible before physical key event");
            if (scenario == "physical-digit" || scenario == "physical-keypad" || scenario == "physical-numlock") {
                const bool keypad = scenario != "physical-digit";
                const auto states = scenario == "physical-numlock"
                    ? KeyStates{KeyState::NumLock, KeyState::CapsLock} : KeyStates{};
                require(editor->key(Key(keypad ? FcitxKey_KP_2 : FcitxKey_2, states, keypad ? 88 : 11)),
                        "physical digit selects AI candidate regardless of keycode and lock state");
                require(editor->document == "今天晚上一起看", "physical 2 commits candidate, not digit");
            } else if (scenario == "physical-tab" || scenario == "physical-held-tab") {
                const auto raw = Key(FcitxKey_Tab, KeyStates{}, 23);
                require(scenario == "physical-held-tab" ? editor->press(raw) : editor->key(raw),
                        "physical Tab selects AI candidate");
                require(editor->document == "今天晚上一起吃", "physical Tab commits first candidate");
            } else if (scenario == "physical-escape") {
                require(editor->key(Key(FcitxKey_Escape, KeyStates{}, 9)), "physical Escape is consumed when dismissing AI");
            } else {
                Key raw;
                if (scenario == "physical-ctrl") raw = Key(FcitxKey_2, KeyStates{KeyState::Ctrl}, 11);
                else if (scenario == "physical-alt") raw = Key(FcitxKey_2, KeyStates{KeyState::Alt}, 11);
                else if (scenario == "physical-shift") raw = Key(FcitxKey_at, KeyStates{KeyState::Shift}, 11);
                else if (scenario == "physical-ctrl-tab") raw = Key(FcitxKey_Tab, KeyStates{KeyState::Ctrl}, 23);
                else if (scenario == "physical-out-of-range") raw = Key(FcitxKey_9, KeyStates{}, 18);
                else require(false, "unknown physical-key test scenario");
                require(!editor->key(raw), "shortcuts and out-of-range digits must not be swallowed");
                require(editor->document == "今天晚上一起", "non-selection keys must not commit AI text");
                require(!aiVisible(*editor), "non-selection keys dismiss AI and return to normal input");
            }
        });
        if (scenario == "physical-held-tab") {
            at(900, [&] {
                require(aiVisible(*editor), "next prediction shown before physical Tab release");
                const auto firstFrame = editor->visibleFrames.size();
                editor->release(Key(FcitxKey_Tab, KeyStates{}, 23));
                require(aiVisible(*editor), "physical Tab release preserves next prediction");
                require(std::all_of(editor->visibleFrames.begin() + firstFrame, editor->visibleFrames.end(),
                                    [](bool visible) { return visible; }),
                        "physical release does not render an empty intermediate frame");
            });
        }
    } else if (scenario.starts_with("release-")) {
        at(500, [&] {
            require(aiVisible(*editor), "AI candidates visible before commit key release");
            if (scenario == "release-after-show" || scenario == "release-ai-selection" ||
                scenario == "release-typing" || scenario == "release-shift") {
                releaseWithoutFlicker("space");
            } else {
                editor->release("space");
            }
            if (scenario == "release-after-show") {
                require(editor->key("Tab"), "AI candidates selectable after commit key release");
                require(editor->document == "今天晚上一起吃", "AI choice commits after key release");
            } else if (scenario == "release-ai-selection") {
                require(editor->press("Tab"), "AI candidate accepted while Tab remains held");
            } else if (scenario == "release-typing") {
                editor->key("n");
                require(!aiVisible(*editor), "typing immediately dismisses restored AI list");
            } else if (scenario == "release-shift") {
                editor->key("Shift_L");
                require(!aiVisible(*editor), "modifier press dismisses AI list");
                require(!editor->key("n"), "Rime ASCII switch still works after release restoration");
            } else if (scenario == "release-cursor" || scenario == "release-switch") {
                require(!aiVisible(*editor), "context changes during release prohibit restoration");
            }
        });
        if (scenario == "release-ai-selection") {
            at(900, [&] {
                require(aiVisible(*editor), "next prediction appears before Tab is released");
                releaseWithoutFlicker("Tab");
            });
        } else if (scenario == "release-filtered") {
            at(700, [&] {
                editor->inputPanel().reset();
                editor->updateUserInterface(UserInterfaceComponent::InputPanel);
                require(!aiVisible(*editor), "filtered release must not arm unrelated future UI refreshes");
            });
        }
    } else if (scenario == "new-request") {
        at(80, [&] {
            for (const char c : std::string("nihao")) editor->key(std::string(1, c));
            editor->key("space");
        });
        at(700, [&] {
            require(aiVisible(*editor), "new request must not wait for obsolete slow response");
            require(editor->inputPanel().candidateList()->candidate(0).text().toString() == "呀",
                    "latest context wins over obsolete request");
        });
    } else if (scenario == "menu-cancel" || scenario == "menu-off") {
        at(scenario == "menu-cancel" ? 80 : 500, [&] {
            if (scenario == "menu-off") require(aiVisible(*editor), "AI shown before switching off");
            findToggle()->activate(editor.get());
            require(!findToggle()->isChecked(editor.get()), "menu immediately switched off");
            require(!aiVisible(*editor), "switch off hides AI candidates");
        });
    } else if (scenario == "typing") {
        at(80, [&] {
            auto started = std::chrono::steady_clock::now();
            editor->key("n");
            require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(150),
                    "API wait/cancellation must not block the input thread");
        });
    } else if (scenario == "escape") {
        at(500, [&] {
            require(aiVisible(*editor), "prediction shown before Escape");
            require(editor->key("Escape"), "Escape consumed by prediction");
        });
    } else if (scenario == "cursor") {
        at(80, [&] { editor->setDocument(editor->document, 1, 1); });
    } else if (scenario == "focus") {
        at(80, [&] { editor->focusOut(); });
    } else if (scenario == "switch-im") {
        at(80, [&] { instance.setCurrentInputMethod(editor.get(), "keyboard-us", true); });
    } else if (scenario == "replaced-panel") {
        at(500, [&] {
            require(aiVisible(*editor), "prediction visible before ownership changes");
            oldCandidates = editor->inputPanel().candidateList();
            auto list = std::make_unique<CommonCandidateList>();
            list->append<DisplayOnlyCandidateWord>(Text("other addon"));
            editor->inputPanel().setCandidateList(std::move(list));
            oldCandidates->candidate(0).select(editor.get());
            require(editor->document == "今天晚上一起", "unowned candidate cannot commit");
        });
    } else if (scenario == "destroyed") {
        at(80, [&] { editor.reset(); });
    } else if (scenario == "stale-context") {
        at(500, [&] {
            require(!aiVisible(*editor), "old surrounding text must not generate candidates");
            editor->publish();
        });
    } else if (scenario == "busy-panel") {
        at(80, [&] {
            auto list = std::make_unique<CommonCandidateList>();
            list->append<DisplayOnlyCandidateWord>(Text("other addon"));
            editor->inputPanel().setCandidateList(std::move(list));
        });
    } else if (scenario == "continued" || scenario == "stale-candidate") {
        at(500, [&] {
            require(aiVisible(*editor), "first prediction automatically visible");
            require(editor->aiFrames > 0, "UI callback fired without another key");
            oldCandidates = editor->inputPanel().candidateList();
            if (scenario == "continued") {
                require(editor->key("Tab"), "Tab accepts prediction");
            } else {
                oldCandidates->candidate(0).select(editor.get());
            }
            require(editor->document == "今天晚上一起吃", "prediction inserted once");
        });
    }

    const auto finish = scenario == "shutdown" ? 80 : scenario == "timeout" ? 1400 : 1100;
    at(finish, [&] {
        if (scenario == "destroyed") {
            require(!editor, "destroyed input context remained safe");
        } else if (scenario.starts_with("physical-")) {
            const bool selected = scenario == "physical-digit" || scenario == "physical-keypad" ||
                                  scenario == "physical-numlock" || scenario == "physical-tab" ||
                                  scenario == "physical-held-tab";
            require(aiVisible(*editor) == selected, "physical key preserves intended prediction lifecycle");
        } else if (scenario == "new-request") {
            require(aiVisible(*editor), "latest request remains visible");
            require(editor->inputPanel().candidateList()->candidate(0).text().toString() == "呀",
                    "late old response must not replace latest candidates");
        } else if (scenario == "api-space") {
            require(aiVisible(*editor), "English continuation visible");
            editor->key("Tab");
            require(editor->document == "今天晚上一起 tomorrow", "leading space preserved on commit");
        } else if (scenario == "automatic" || scenario == "selection" || scenario == "stale-context" ||
                   scenario == "unicode" || scenario == "menu-on" || scenario == "api-auth" || scenario == "api-dedup" ||
                   scenario == "api-standard" || scenario == "api-first-position" ||
                   scenario == "global-config" || scenario == "partial-config" || scenario == "empty-config" ||
                   scenario == "api-bytes" || scenario == "api-special") {
            require(aiVisible(*editor), "automatic asynchronous candidates visible");
            require(editor->aiFrames > 0, "frontend received asynchronous UI refresh");
            require(editor->keyCount == keysAfterCommit, "no additional key required for UI refresh");
            require(editor->inputPanel().candidateList()->candidate(0).text().toString() == "吃",
                    "prediction used updated prefix, not pre-commit text");
            if (scenario == "api-dedup") {
                require(editor->inputPanel().candidateList()->size() == 2,
                        "duplicate tokens removed");
            }
            if (scenario == "api-bytes" || scenario == "api-special") {
                require(editor->inputPanel().candidateList()->size() == 1,
                        "uncommittable top tokens skipped without promoting lower-ranked tokens");
            }
            if (scenario == "selection") {
                require(editor->document == "今天一起尾巴", "selection replaced and right context retained");
                editor->key("2");
                require(editor->document == "今天一起看尾巴", "numeric selection commits at real cursor");
            }
        } else if (scenario == "continued" || scenario == "stale-candidate" ||
                   scenario == "release-after-show" || scenario == "release-ai-selection") {
            require(aiVisible(*editor), "next round automatically visible");
            require(editor->inputPanel().candidateList()->candidate(0).text().toString() == "饭",
                    "next prediction used accepted phrase");
            if (scenario == "stale-candidate") {
                oldCandidates->candidate(0).select(editor.get());
                require(editor->document == "今天晚上一起吃", "stale candidate cannot commit again");
            }
        } else {
            require(!aiVisible(*editor), "cancelled or unsupported request must not show candidates");
            if (scenario == "switch-im" || scenario == "other-im" || scenario == "release-switch") {
                require(!findToggle(), "AI menu action hidden outside Rime");
            }
            if (scenario == "disabled" || scenario == "missing-config" ||
                scenario == "invalid-config" || scenario == "invalid-global") {
                require(!findToggle()->isChecked(editor.get()), "unconfigured/disabled extension stays off");
                if (scenario != "disabled") {
                    findToggle()->activate(editor.get());
                    require(!findToggle()->isChecked(editor.get()), "invalid configuration cannot enable requests");
                }
            }
            if (scenario.starts_with("api-") || scenario == "no-extension") {
                editor->key("n");
                require(!editor->inputPanel().preedit().empty() || !editor->inputPanel().clientPreedit().empty(),
                        "API failure or missing extension does not break Rime");
            }
            if (scenario == "typing" || scenario == "release-typing") {
                require(!editor->inputPanel().preedit().empty() ||
                            !editor->inputPanel().clientPreedit().empty(),
                        "typing handed back to Rime");
            }
            if (scenario == "busy-panel" || scenario == "replaced-panel" || scenario == "release-panel") {
                require(editor->inputPanel().candidateList()->candidate(0).text().toString() == "other addon",
                        "extension did not overwrite another candidate panel");
            }
        }
        std::cout << "PASS: " << scenario << '\n';
        instance.exit();
    });
    instance.eventLoop().exec();
    editor.reset();
    return 0;
}
