#include "session.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace rome;
using namespace rome::weasel;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Sample sample(std::string text, size_t cursor, size_t anchor,
              std::optional<std::pair<size_t, size_t>> range = {}) {
    auto body = SurroundingSnapshot::fromUtf8(text, cursor, anchor, OffsetUnit::Utf16);
    require(body.has_value(), "bad fixture snapshot");
    std::optional<SurroundingSnapshot> hint;
    if (range) hint = SurroundingSnapshot::fromUtf8(text, range->second, range->first, OffsetUnit::Utf16);
    return {reinterpret_cast<HWND>(2), {3, 4}, std::move(*body), std::move(hint)};
}
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    Sample value = sample("前文😀", 4, 4, {{0, 1}});
    bool focused = true, pauseNext = false, paused = false, hints = true;
    unsigned reads = 0;
    void set(Sample next) { std::lock_guard lock(mutex); value = std::move(next); }
    void focus(bool on) { std::lock_guard lock(mutex); focused = on; }
    void pause() { std::lock_guard lock(mutex); pauseNext = true; }
    void release() { std::lock_guard lock(mutex); pauseNext = paused = false; changed.notify_all(); }
    unsigned readCount() { std::lock_guard lock(mutex); return reads; }
    void waitReads(unsigned count) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 3s, [&] { return reads >= count; }), "idle cache did not sample");
    }
    void waitPaused() {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 3s, [&] { return paused; }), "read did not enter controlled pause");
    }
};
class MemoryHost final : public Host {
public:
    explicit MemoryHost(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::optional<Focus> capture(std::string_view app, std::string_view type) const override {
        return app == "fixture" && type == "tsf" && current(focus_) ? std::optional(focus_) : std::nullopt;
    }
    bool current(const Focus& focus) const override {
        std::lock_guard lock(state_->mutex);
        return state_->focused && focus == focus_;
    }
    std::optional<Sample> read(TextReader&, const Focus&, TextRead mode) const override {
        std::unique_lock lock(state_->mutex);
        ++state_->reads;
        if (state_->pauseNext) {
            state_->pauseNext = false;
            state_->paused = true;
            state_->changed.notify_all();
            state_->changed.wait(lock, [&] { return !state_->paused; });
        }
        state_->changed.notify_all();
        if (!state_->focused) return {};
        return mode == TextRead::Plain || !state_->hints ? state_->value.plain() : state_->value;
    }
    bool wake(const Focus& focus) const override { return current(focus); }
private:
    std::shared_ptr<State> state_;
    Focus focus_{reinterpret_cast<HWND>(1), reinterpret_cast<HWND>(2), 3, reinterpret_cast<HKL>(4)};
};
Event wait(Session& session, const std::string& wanted, const std::string& alternative = {}) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto event = session.take()) {
            if (event->state == wanted || event->state == alternative) return *event;
            require(event->state != "showing" && event->state != "api-error" && event->state != "error" &&
                    event->state != "unavailable" && event->state != "timeout" && event->state != "cancelled",
                    ("unexpected event: " + event->state + ", wanted " + wanted).c_str());
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("missing event: " + wanted);
}
void noCandidates(Session& session) {
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto event = session.take())
            require(event->state != "showing" && event->state != "commit" && event->state != "generating", "stale output escaped");
        std::this_thread::sleep_for(2ms);
    }
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: test-weasel-session scenario base_url");
        const std::string scenario = argv[1];
        Settings settings;
        settings.enabled = true; settings.model = "weasel-session-test";
        settings.baseUrl = argv[2]; settings.candidates = 2;
        auto state = std::make_shared<State>();
        state->hints = scenario != "no-text-edit" && scenario != "cold-recovery";
        Session session(settings, std::make_unique<MemoryHost>(state));
        struct Unblock { State& value; ~Unblock() { value.release(); } } unblock{*state};
        const bool cold = scenario == "cold-preedit" || scenario == "cold-missing" || scenario == "late-baseline" || scenario == "cold-recovery";
        if (scenario == "late-baseline") {
            state->pause(); session.watch("fixture", "tsf"); state->waitPaused();
        } else if (!cold) {
            session.watch("fixture", "tsf");
            // 第二次读取已开始，第一次空闲快照必定已经完成并进入缓存。
            state->waitReads(2);
        }
        session.arm("fixture", "tsf");
        const auto preedit = sample("前文😀qu", 6, 6, cold && scenario != "cold-preedit" ?
            std::nullopt : std::optional(std::pair<size_t, size_t>{4, 6}));
        state->set(preedit);
        session.send({Command::Composing, {}});
        if (scenario == "late-baseline") state->release();
        if (scenario != "cold-missing" && scenario != "late-baseline" && scenario != "cold-recovery") wait(session, "armed");
        auto committed = sample("前文😀去", 5, 5, {{0, 1}});
        if (scenario == "changed-target") committed.identity = {99};
        if (scenario == "changed-cursor") committed = sample("前文😀去", 4, 4);
        if (scenario == "changed-suffix") committed = sample("前文😀去错", 5, 5);
        state->set(committed);
        if (scenario == "focus-lost") state->focus(false);
        if (scenario == "read-race") state->pause();
        session.send({Command::Commit, "去"});
        session.send({scenario == "preview-block" ? Command::Composing : Command::Idle, {}});
        if (scenario == "read-race") {
            state->waitPaused(); session.send({Command::Composing, {}}); state->release();
        }
        if (scenario == "cold-missing" || scenario == "late-baseline" || scenario == "focus-lost" || scenario == "cold-recovery") {
            // 焦点可能在 Commit 入队前被监测到。两条拒绝路径均不得产生请求。
            wait(session, "unavailable", scenario == "focus-lost" ? "cancelled" : ""); noCandidates(session);
            if (scenario == "cold-recovery") {
                state->waitReads(state->readCount() + 2);
                session.arm("fixture", "tsf");
                state->set(sample("前文😀去chi", 8, 8));
                session.send({Command::Composing, {}}); wait(session, "armed");
                state->set(sample("前文😀去吃", 6, 6));
                session.send({Command::Commit, "吃"}); session.send({Command::Idle, {}});
                wait(session, "showing");
            }
        } else if (scenario == "preview-block" || scenario == "read-race" || scenario == "changed-target" ||
                   scenario == "changed-cursor" || scenario == "changed-suffix") {
            wait(session, "timeout"); noCandidates(session);
        } else if (scenario == "late-model") {
            wait(session, "generating");
            std::cout << "request-pending" << std::endl;
            std::string signal; std::getline(std::cin, signal);
            session.send({Command::Cancel, {}});
            std::cout << "cancelled" << std::endl;
            std::getline(std::cin, signal);
            noCandidates(session);
        } else {
            auto shown = wait(session, "showing");
            require(shown.tokens == std::vector<std::string>{"吃", "看"}, "ranked candidates changed");
            if (scenario == "new-composition") {
                session.send({Command::Composing, {}}); wait(session, "cancelled");
            } else if (scenario == "unowned-space") {
                state->set(sample("前文😀去 ", 5, 5, {{5, 6}})); wait(session, "cancelled");
            } else if (scenario == "menu-selection" || scenario == "shifted-menu") {
                session.send({Command::Displayed, {}});
                state->set(scenario == "shifted-menu" ? sample("前文😀 去", 4, 4, {{4, 5}}) :
                                                      sample("前文😀去 ", 5, 5, {{5, 6}}));
                if (scenario == "shifted-menu") wait(session, "cancelled");
                else {
                    session.send({Command::Select, {}, 0});
                    const auto choice = wait(session, "commit");
                    require(choice.commit == "吃", "wrong selection");
                    state->set(sample("前文😀去吃", 6, 6, {{0, 1}}));
                    session.send({Command::Commit, choice.commit}); session.send({Command::Idle, {}});
                    wait(session, "showing");
                }
            }
        }
        std::cout << "PASS " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
