#pragma once

#include "squirrel_host.h"

#include <rime/processor.h>

namespace rime { class Menu; }

namespace rome {
// librime 的组件和上下文只由主线程访问，HTTP 工作线程仅负责请求。
class RimePredictor final : public rime::Processor {
public:
    RimePredictor(const rime::Ticket &ticket, std::unique_ptr<SquirrelHost> host);
    ~RimePredictor() override;
    rime::ProcessResult ProcessKeyEvent(const rime::KeyEvent &key) override;

private:
    enum class Stage { WaitingCommit, Generating, Showing };
    struct Session {
        uint64_t generation;
        TextWindow expected;
        Stage stage;
        SquirrelHost::Time deadline;
        Cancellation request;
        std::vector<std::string> tokens;
    };
    struct PanelOptions { bool softCursor; bool autoCommit; };

    bool enabled() const;
    bool reload();
    bool ownsPanel() const;
    bool current() const;
    void status(const char *code);
    void refresh();
    void monitor();
    void tick();
    void cancelSession(bool updateUI);
    void invalidate(bool updateUI);
    void committed(const std::string &text);
    void updated();
    void optionChanged(const std::string &option);
    void result(PredictionResult result);
    void show(std::vector<std::string> tokens);
    void select(size_t index);
    void selected();

    std::unique_ptr<SquirrelHost> host_;
    Settings settings_;
    bool configured_ = false;
    bool internal_ = false;
    uint64_t generation_ = 0;
    uint64_t uiSerial_ = 0;
    // 排队的初始化和监测回调不可越过组件析构。
    Cancellation destroyed_ = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<CompletionClient> client_;
    std::optional<TextWindow> before_;
    std::optional<Session> session_;
    std::shared_ptr<rime::Menu> menu_;
    std::optional<PanelOptions> panelOptions_;
    rime::connection commitConnection_;
    rime::connection updateConnection_;
    rime::connection selectConnection_;
    rime::connection abortConnection_;
    rime::connection optionConnection_;
};
} // namespace rome
