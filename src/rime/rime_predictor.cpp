#include "rime_predictor.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/translation.h>

#include <utility>

namespace rome {
namespace {
constexpr auto kCommitTimeout = std::chrono::seconds(1);
constexpr const char *kOption = "rome_ai_predict";
constexpr const char *kTag = "rome_ai_predict";
// Squirrel 可同时保留多个应用的 librime 会话，只有最近收到按键的组件可预测。
RimePredictor *active = nullptr;
}

RimePredictor::RimePredictor(const rime::Ticket &ticket, std::unique_ptr<SquirrelHost> host)
    : Processor(ticket), host_(std::move(host)) {
    auto *ctx = engine_->context();
    // sink 提供经过 formatter 后的最终文字，也包括直接上屏的标点。
    commitConnection_ = engine_->sink().connect([this](const std::string &text) { committed(text); });
    updateConnection_ = ctx->update_notifier().connect([this](rime::Context *) { updated(); });
    selectConnection_ = ctx->select_notifier().connect([this](rime::Context *) { selected(); });
    abortConnection_ = ctx->abort_notifier().connect([this](rime::Context *) { invalidate(false); });
    optionConnection_ = ctx->option_update_notifier().connect(
        [this](rime::Context *, const std::string &option) { optionChanged(option); });
    reload();
    // Schema 初始化会设置 switches/reset，必须在初始化之后应用配置的启动开关。
    host_->dispatch([this, destroyed = destroyed_] {
        if (!destroyed->load()) engine_->context()->set_option(kOption, configured_ && settings_.enabled);
    });
}

RimePredictor::~RimePredictor() {
    destroyed_->store(true);
    commitConnection_.disconnect();
    updateConnection_.disconnect();
    selectConnection_.disconnect();
    abortConnection_.disconnect();
    optionConnection_.disconnect();
    // Engine 析构时，translator/segmentor 可能已经销毁，不能再触发 Compose。
    host_->stopMonitor();
    if (session_ && session_->request) session_->request->store(true);
    session_.reset();
    before_.reset();
    menu_.reset();
    client_.reset();
    if (active == this) active = nullptr;
}

bool RimePredictor::reload() {
    configured_ = false;
    try {
        settings_ = host_->loadConfiguration();
        validateSettings(settings_);
        configured_ = true;
    } catch (const std::exception &) {
        // 配置边界只报告状态码，不输出 YAML 正文或认证信息。
        status("configuration-error");
    }
    return configured_;
}

bool RimePredictor::enabled() const {
    const auto *ctx = engine_->context();
    return configured_ && engine_->active_engine() == engine_ && ctx->get_option(kOption) &&
           !ctx->get_option("ascii_mode");
}

void RimePredictor::status(const char *code) {
    engine_->context()->set_property("rome_ai_predict/status", code);
}

void RimePredictor::refresh() {
    engine_->context()->set_property("_refresh_ui",
        "source=rome_ai_predict&kind=full&serial=" + std::to_string(++uiSerial_));
}

bool RimePredictor::ownsPanel() const {
    const auto *ctx = engine_->context();
    const auto &comp = ctx->composition();
    if (!menu_ || !ctx->input().empty() || comp.empty() || comp.front().menu != menu_ ||
        !comp.front().HasTag(kTag)) return false;
    // OnSelect 会在零长联想段后追加一个空段，仍属于本次联想。
    for (const auto &segment : comp) {
        if (segment.start != 0 || segment.end != 0) return false;
    }
    return true;
}

bool RimePredictor::current() const {
    if (active != this || !enabled() || !session_) return false;
    auto actual = host_->read(session_->expected);
    return actual && *actual == session_->expected;
}

void RimePredictor::monitor() {
    host_->startMonitor([this, destroyed = destroyed_] {
        if (!destroyed->load()) tick();
    });
}

void RimePredictor::cancelSession(bool updateUI) {
    if (session_ && session_->request) session_->request->store(true);
    session_.reset();
    const bool owned = ownsPanel();
    menu_.reset();
    // 先放弃所有权再清理上下文，避免 update_notifier 重入取消流程。
    internal_ = true;
    if (owned) engine_->context()->Clear();
    if (panelOptions_) {
        engine_->context()->set_option("soft_cursor", panelOptions_->softCursor);
        engine_->context()->set_option("_auto_commit", panelOptions_->autoCommit);
        panelOptions_.reset();
    }
    internal_ = false;
    if (owned && updateUI) refresh();
    if (!before_) host_->stopMonitor();
}

void RimePredictor::invalidate(bool updateUI) {
    before_.reset();
    cancelSession(updateUI);
}

rime::ProcessResult RimePredictor::ProcessKeyEvent(const rime::KeyEvent &key) {
    if (key.release()) return rime::kNoop;
    if (active && active != this) active->invalidate(true);
    active = this;
    auto *ctx = engine_->context();
    if (!enabled()) {
        invalidate(false);
        return rime::kNoop;
    }
    if (session_ && session_->stage == Stage::Showing && ownsPanel() && current()) {
        const bool plain = !key.ctrl() && !key.alt() && !key.super() && !key.shift();
        int index = -1;
        if (plain) {
            if (key.keycode() == XK_Tab) index = 0;
            if (key.keycode() >= XK_1 && key.keycode() <= XK_9) index = key.keycode() - XK_1;
            if (key.keycode() >= XK_KP_1 && key.keycode() <= XK_KP_9) index = key.keycode() - XK_KP_1;
        }
        if (index >= 0 && static_cast<size_t>(index) < session_->tokens.size()) {
            select(static_cast<size_t>(index));
            return rime::kAccepted;
        }
        if (plain && key.keycode() == XK_Escape) {
            invalidate(false);
            status("dismissed");
            return rime::kAccepted;
        }
    }
    cancelSession(false);
    if (key.ctrl() || key.alt() || key.super()) {
        before_.reset();
        host_->stopMonitor();
        return rime::kNoop;
    }
    if (!ctx->IsComposing()) {
        before_ = host_->capture(static_cast<size_t>(settings_.contextCharacters));
        if (before_) {
            monitor();
        } else {
            host_->stopMonitor();
            status("surrounding-unavailable");
        }
    } else if (before_ && !host_->currentTarget(*before_->target)) {
        before_.reset();
        host_->stopMonitor();
    }
    return rime::kNoop;
}

void RimePredictor::committed(const std::string &text) {
    auto before = std::move(before_);
    before_.reset();
    cancelSession(false);
    if (text.empty() || active != this || !enabled() || !before ||
        !host_->currentTarget(*before->target)) {
        status("commit-unconfirmed");
        return;
    }
    session_ = Session{++generation_, before->afterCommit(text), Stage::WaitingCommit,
                       host_->now() + kCommitTimeout, {}, {}};
    status("waiting-commit");
    monitor();
}

void RimePredictor::tick() {
    if (active != this || !enabled() || (before_ && !host_->currentTarget(*before_->target)) ||
        (session_ && !host_->currentTarget(*session_->expected.target))) {
        invalidate(true);
        status("context-invalidated");
        return;
    }
    if (!session_) return;
    if (session_->stage == Stage::WaitingCommit) {
        if (host_->now() >= session_->deadline) {
            invalidate(true);
            status("commit-timeout");
            return;
        }
        // 提交通知之后 Rime 会 Clear，但可能又开始了新的组字。
        if (engine_->context()->IsComposing()) {
            invalidate(true);
            status("composition-active");
            return;
        }
        if (!current()) return;
        const auto prefix = session_->expected.snapshot.prefix(settings_.contextCharacters);
        if (prefix.empty()) {
            invalidate(true);
            status("surrounding-unavailable");
            return;
        }
        if (!client_) {
            client_ = std::make_unique<CompletionClient>(
                [this](CompletionClient::Task task) { host_->dispatch(std::move(task)); },
                [this](PredictionResult value) { result(std::move(value)); });
        }
        session_->stage = Stage::Generating;
        session_->request = client_->submit(session_->generation, settings_, prefix);
        status("generating");
    } else if (!current() || (engine_->context()->IsComposing() && !ownsPanel())) {
        invalidate(true);
        status("context-invalidated");
    }
}

void RimePredictor::result(PredictionResult value) {
    if (!session_ || session_->generation != value.generation ||
        session_->stage != Stage::Generating) return;
    if (!current() || engine_->context()->IsComposing()) {
        invalidate(true);
        status("stale-result");
        return;
    }
    if (std::holds_alternative<std::string>(value.value)) {
        invalidate(true);
        status("api-error");
        return;
    }
    auto tokens = std::get<std::vector<std::string>>(std::move(value.value));
    if (tokens.empty()) {
        invalidate(true);
        status("empty-result");
        return;
    }
    show(std::move(tokens));
}

void RimePredictor::show(std::vector<std::string> tokens) {
    auto *ctx = engine_->context();
    panelOptions_ = PanelOptions{ctx->get_option("soft_cursor"), ctx->get_option("_auto_commit")};
    internal_ = true;
    // 不把软光标写入应用。不让普通 selector 绕过同步复核直接提交联想。
    ctx->set_option("soft_cursor", false);
    ctx->set_option("_auto_commit", false);
    internal_ = false;
    auto translation = std::make_shared<rime::FifoTranslation>();
    for (const auto &text : tokens) {
        // Candidate::text 必须稳定，librime C API 会分两次调用它来分配和复制。
        translation->Append(std::make_shared<rime::SimpleCandidate>(kTag, 0, 0, text, "AI"));
    }
    menu_ = std::make_shared<rime::Menu>();
    menu_->AddTranslation(translation);
    rime::Segment segment(0, 0);
    segment.tags = {kTag, "placeholder", "phony"};
    segment.status = rime::Segment::kGuess;
    segment.menu = menu_;
    ctx->composition().AddSegment(std::move(segment));
    session_->tokens = std::move(tokens);
    session_->stage = Stage::Showing;
    menu_->Prepare(session_->tokens.size());
    status("showing");
    refresh();
}

void RimePredictor::select(size_t index) {
    if (!session_ || session_->stage != Stage::Showing || !ownsPanel() || !current() ||
        index >= session_->tokens.size()) {
        invalidate(true);
        status("stale-candidate");
        return;
    }
    auto text = session_->tokens[index];
    auto before = session_->expected;
    invalidate(false);
    before_ = std::move(before);
    // 直接交给宿主的上屏通道，不经过词库学习、formatter 或简繁转换。
    engine_->sink()(text);
    refresh();
}

void RimePredictor::selected() {
    if (internal_ || !ownsPanel()) return;
    // Engine::OnSelect 已执行。_auto_commit=false 保留零长段供这里复核鼠标选词。
    select(engine_->context()->composition().front().selected_index);
}

void RimePredictor::updated() {
    if (internal_) return;
    if (menu_ && !ownsPanel()) cancelSession(true);
    if (before_ && !engine_->context()->IsComposing() && !session_) {
        before_.reset();
        host_->stopMonitor();
    }
}

void RimePredictor::optionChanged(const std::string &option) {
    if (internal_) return;
    if (option == "ascii_mode") {
        invalidate(true);
    } else if (option == kOption) {
        invalidate(true);
        if (engine_->context()->get_option(kOption) && !reload()) {
            engine_->context()->set_option(kOption, false);
        }
    }
}
} // namespace rome
