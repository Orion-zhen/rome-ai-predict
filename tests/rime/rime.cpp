#include "squirrel_host.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/service.h>
#include <rime_api.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace rome;
using namespace std::chrono_literals;

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

class Target final : public TextTarget {
public:
    explicit Target(int identity) : identity(identity) {}
    bool equals(const TextTarget &other) const override {
        const auto *target = dynamic_cast<const Target *>(&other);
        return target && target->identity == identity;
    }
    int identity;
};

struct State {
    std::filesystem::path directory;
    std::optional<SurroundingSnapshot> document;
    int identity = 1;
    bool focused = true;
    bool readable = true;
    size_t origin = 0;
    int captures = 0;
    SquirrelHost::Time clock{};
    std::function<void()> timer;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<CompletionClient::Task> tasks;

    void queue(CompletionClient::Task task) {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
        ready.notify_one();
    }
    void drain() {
        std::deque<CompletionClient::Task> batch;
        {
            std::lock_guard lock(mutex);
            batch.swap(tasks);
        }
        for (auto &task : batch) task();
    }
    void tick() {
        drain();
        auto callback = timer;
        if (callback) callback();
    }
    void awaitQueued() {
        std::unique_lock lock(mutex);
        require(ready.wait_for(lock, 5s, [&] { return !tasks.empty(); }), "request result was not queued");
    }
};

std::vector<std::shared_ptr<State>> states;

class Host final : public SquirrelHost {
public:
    explicit Host(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Host() override { stopMonitor(); }
    Settings loadConfiguration() override {
        return loadSettings(state_->directory / "rome-ai-predict.yaml");
    }
    void dispatch(CompletionClient::Task task) override { state_->queue(std::move(task)); }
    Time now() const override { return state_->clock; }
    void startMonitor(std::function<void()> tick) override { state_->timer = std::move(tick); }
    void stopMonitor() override { state_->timer = {}; }
    bool currentTarget(const TextTarget &target) override {
        return state_->focused && state_->readable && target.equals(Target(state_->identity));
    }
    std::optional<TextWindow> capture(size_t) override {
        ++state_->captures;
        if (!state_->focused || !state_->readable || !state_->document) return std::nullopt;
        return TextWindow{std::make_shared<Target>(state_->identity), state_->origin, *state_->document};
    }
    std::optional<TextWindow> read(const TextWindow &expected) override {
        if (!currentTarget(*expected.target) || !state_->document) return std::nullopt;
        return TextWindow{std::make_shared<Target>(state_->identity), state_->origin, *state_->document};
    }
private:
    std::shared_ptr<State> state_;
};

namespace rome {
std::unique_ptr<SquirrelHost> createSquirrelHost(const std::filesystem::path &directory) {
    auto state = std::make_shared<State>();
    state->directory = directory;
    states.push_back(state);
    return std::make_unique<Host>(std::move(state));
}
}

std::vector<std::string> refreshes;
void notification(void *, RimeSessionId, const char *type, const char *value) {
    if (std::string(type) == "property" && std::string(value).starts_with("_refresh_ui=")) {
        refreshes.emplace_back(value);
    }
}

SurroundingSnapshot snapshot(std::string text, size_t cursor, size_t anchor) {
    auto value = SurroundingSnapshot::fromUtf8(std::move(text), cursor, anchor, OffsetUnit::CodePoints);
    require(value.has_value(), "invalid editor fixture");
    return *value;
}

std::string takeCommit(RimeApi *api, RimeSessionId session) {
    RimeCommit commit{};
    RIME_STRUCT_INIT(RimeCommit, commit);
    if (!api->get_commit(session, &commit)) return {};
    std::string text = commit.text;
    api->free_commit(&commit);
    return text;
}

void run(RimeApi *api, RimeSessionId session, const std::string &scenario) {
    require(!states.empty(), "native processor module was not loaded");
    auto state = states.back();
    auto *ctx = rime::Service::instance().GetSession(session)->context();
    state->drain(); // 应用配置的启动开关。
    require(ctx->get_option("rome_ai_predict") == (scenario != "invalid-config"), "startup toggle");
    ctx->set_option("soft_cursor", true);
    std::string prefix = "今天晚上";
    if (scenario == "unicode") {
        prefix.clear();
        for (int i = 0; i < 300; ++i) prefix += "😀";
        prefix += "今天晚上";
    }
    auto before = snapshot(prefix + "尾巴", scenario == "unicode" ? 304 : 4,
                            scenario == "unicode" ? 304 : 4);
    if (scenario == "selection") before = snapshot("今天晚上旧词尾巴", 6, 4);
    state->document = before;
    if (scenario == "no-context") state->readable = false;
    if (scenario == "disabled") api->set_option(session, "rome_ai_predict", false);
    if (scenario == "ascii") api->set_option(session, "ascii_mode", true);
    for (const auto key : std::string("yiqi")) api->process_key(session, key, 0);
    if (scenario != "ascii") {
        // AX 在组字期间看到的可能是应用中的 marked text，不能作为前文。
        state->document = before.afterCommit("yiqi");
    }
    api->process_key(session, XK_space, 0);
    auto committed = takeCommit(api, session);
    if (scenario == "ascii") {
        state->tick();
        require(!ctx->get_option("rome_ai_predict") || !ctx->HasMenu(), "ASCII mode must not predict");
        return;
    }
    require(committed == (scenario == "formatted" ? "一起Ａ" : "一起"), "original Rime input changed");
    if (scenario == "no-context" || scenario == "disabled" || scenario == "invalid-config") {
        state->tick();
        require(!ctx->HasMenu(), "unavailable prediction must leave Rime usable");
        return;
    }
    require(state->captures == 1, "baseline was recaptured from marked text");
    state->tick();
    require(ctx->get_property("rome_ai_predict/status") == "waiting-commit", "preedit mistaken for acknowledgement");
    if (scenario == "no-ack" || scenario == "unexpected-edit") {
        if (scenario == "unexpected-edit") state->document = before.afterCommit("其他");
        state->clock += 2s;
        state->tick();
        require(ctx->get_property("rome_ai_predict/status") == "commit-timeout", "unconfirmed commit timed out");
        require(!ctx->HasMenu(), "unconfirmed text produced AI candidates");
        return;
    }
    state->document = before.afterCommit(committed);
    state->tick();
    require(ctx->get_property("rome_ai_predict/status") == "generating", "confirmed text did not request API");
    if (scenario == "destroyed") {
        state->awaitQueued();
        auto savedTick = state->timer;
        api->destroy_session(session);
        savedTick();
        state->drain();
        return;
    }
    state->awaitQueued();
    if (scenario == "busy-panel") {
        api->process_key(session, XK_n, 0);
        state->drain();
        require(ctx->input() == "n" && (!ctx->HasMenu() ||
                ctx->GetSelectedCandidate()->type() != "rome_ai_predict"), "API result overwrote new Rime input");
        return;
    }
    if (scenario == "stale-result") {
        state->document = before.afterCommit("改了");
        state->drain();
        require(!ctx->HasMenu(), "stale API result displayed");
        return;
    }
    if (scenario == "focus") state->focused = false;
    if (scenario == "same-text-other-control") state->identity = 2;
    if (scenario == "input-source") state->readable = false;
    state->tick();
    if (scenario == "focus" || scenario == "same-text-other-control" || scenario == "input-source") {
        require(!ctx->HasMenu(), "context identity invalidation ignored");
        return;
    }
    if (scenario == "api-error") {
        require(!ctx->HasMenu() && ctx->get_property("rome_ai_predict/status") == "api-error", "API error not isolated");
        return;
    }
    require(ctx->HasMenu(), "AI menu not displayed");
    require(!refreshes.empty() && refreshes.back().find("source=rome_ai_predict") != std::string::npos,
            "missing _refresh_ui notification");
    require(!ctx->get_option("soft_cursor") && !ctx->get_option("_auto_commit"), "AI panel mode isolation");
    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    require(api->get_context(session, &context), "cannot inspect native candidate menu");
    require(context.menu.num_candidates == 3 && std::string(context.menu.candidates[0].text) ==
            (scenario == "formatted" ? "吃A" : "吃") &&
            std::string(context.menu.candidates[0].comment) == "AI", "candidate ordering/comment");
    require(context.composition.length == 0, "synthetic menu leaked marked text");
    api->free_context(&context);
    const auto history = ctx->commit_history().size();
    if (scenario == "automatic" || scenario == "unicode" || scenario == "selection") return;
    if (scenario == "switch-menu") {
        api->process_key(session, XK_F4, 0);
        auto *switcher = rime::Service::instance().GetSession(session)->context();
        require(switcher != ctx && switcher->HasMenu(), "Rime switch menu not active");
        state->tick();
        require(!ctx->HasMenu() && switcher->HasMenu(), "AI cleanup overwrote the built-in switch menu");
    } else if (scenario == "replaced-panel") {
        ctx->set_input("n");
        require(ctx->input() == "n" && ctx->HasMenu() &&
                ctx->GetSelectedCandidate()->type() != "rome_ai_predict", "cleanup overwrote a replacement Rime menu");
    } else if (scenario == "new-session") {
        const auto other = api->create_session();
        require(other && api->select_schema(other, "rome_ai_test"), "second input session");
        auto otherState = states.back();
        otherState->document = state->document;
        otherState->drain();
        api->process_key(other, XK_n, 0);
        require(!ctx->IsComposing(), "inactive session kept AI candidates for the same text control");
        api->destroy_session(other);
    } else if (scenario == "schema-change") {
        require(api->select_schema(session, "rome_ai_test"), "schema reload");
        state->drain();
        require(!ctx->HasMenu(), "schema reload kept old AI menu");
    } else if (scenario == "reload") {
        api->set_option(session, "rome_ai_predict", false);
        { std::ofstream config(state->directory / "rome-ai-predict.yaml"); config << "model: ''\\n"; }
        api->set_option(session, "rome_ai_predict", true);
        require(!ctx->get_option("rome_ai_predict") && !ctx->IsComposing(), "invalid reloaded API config enabled AI");
    } else if (scenario == "release") {
        api->process_key(session, XK_space, kReleaseMask);
        require(ctx->HasMenu() && ctx->GetSelectedCandidate()->type() == "rome_ai_predict", "release cleared live AI candidates");
    } else if (scenario == "cursor" || scenario == "window-origin") {
        if (scenario == "cursor") state->document = snapshot(state->document->text(), 4, 4);
        else ++state->origin;
        state->tick();
        require(!ctx->HasMenu(), "changed caret/window origin left a live candidate");
    } else if (scenario == "escape") {
        require(api->process_key(session, XK_Escape, 0), "Escape not handled");
        require(!ctx->IsComposing() && ctx->get_option("rome_ai_predict"), "Escape disabled global option");
    } else if (scenario == "typing" || scenario == "space" || scenario == "ctrl-tab") {
        const int key = scenario == "typing" ? XK_n : scenario == "space" ? XK_space : XK_Tab;
        api->process_key(session, key, scenario == "ctrl-tab" ? kControlMask : 0);
        require(!ctx->HasMenu() || ctx->GetSelectedCandidate()->type() != "rome_ai_predict", "typing kept AI panel");
        require(takeCommit(api, session) != "吃", "typing accepted AI first candidate");
    } else if (scenario == "menu-off") {
        api->set_option(session, "rome_ai_predict", false);
        require(!ctx->IsComposing(), "toggle did not clear AI menu");
    } else if (scenario == "stale-mouse") {
        state->identity = 2;
        api->select_candidate_on_current_page(session, 0);
        require(takeCommit(api, session).empty(), "stale mouse candidate committed before synchronous validation");
        require(!ctx->IsComposing(), "stale mouse menu survived");
    } else {
        std::string expected = scenario == "formatted" ? "吃A" : "吃";
        if (scenario == "number") {
            api->process_key(session, XK_2, 0);
            expected = "看";
        } else if (scenario == "keypad") {
            api->process_key(session, XK_KP_3, kMod2Mask);
            expected = "去";
        } else if (scenario == "mouse") {
            require(api->select_candidate_on_current_page(session, 1), "mouse selection rejected");
            expected = "看";
        } else {
            require(api->process_key(session, XK_Tab, 0), "Tab selection rejected");
        }
        require(takeCommit(api, session) == expected, "selected token did not reach Rime commit sink");
        require(ctx->commit_history().size() == history, "AI selection entered Rime learning/commit history");
        require(ctx->get_option("soft_cursor") && ctx->get_option("_auto_commit"), "panel options not restored");
        require(!ctx->IsComposing(), "AI selection left a synthetic composition");
        // 接受候选也必须等应用回读确认之后才能继续预测。
        state->tick();
        require(ctx->get_property("rome_ai_predict/status") == "waiting-commit", "selected token ack bypassed");
        if (scenario == "continued") {
            state->document = state->document->afterCommit(expected);
            state->tick();
            state->awaitQueued();
            state->tick();
            require(ctx->HasMenu(), "accepted token did not continue prediction");
        }
    }
}

int main(int argc, char **argv) {
    try {
        require(argc == 3, "work directory and scenario required");
        const std::string directory = argv[1];
        const std::string scenario = argv[2];
        auto *api = rime_get_api();
        const char *modules[] = {"default", "deployer", "rome_ai_predict", nullptr};
        RimeTraits traits{};
        RIME_STRUCT_INIT(RimeTraits, traits);
        traits.shared_data_dir = directory.c_str();
        traits.user_data_dir = directory.c_str();
        traits.log_dir = directory.c_str();
        traits.app_name = "rime.rome_ai_plugin_test";
        traits.modules = modules;
        api->setup(&traits);
        api->set_notification_handler(notification, nullptr);
        api->initialize(&traits);
        require(api->deploy_config_file("default.yaml", "config_version"), "default config deployment failed");
        require(api->deploy_schema((directory + "/rome_ai_test.schema.yaml").c_str()), "schema deployment failed");
        auto session = api->create_session();
        require(session != 0 && api->select_schema(session, "rome_ai_test"), "cannot start native Rime session");
        run(api, session, scenario);
        if (scenario != "destroyed") api->destroy_session(session);
        for (const auto &state : states) state->drain();
        api->finalize();
        std::cout << "PASS: rime " << scenario << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
