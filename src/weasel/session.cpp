#include "session.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
namespace rome::weasel {
struct Session::Impl {
    enum class Input { Unknown, Idle, Composing, Menu };
    struct Cached { Focus focus; Sample sample; };
    struct Stamp { uint64_t revision; Input input; };
    Impl(Settings settings, std::unique_ptr<Host> host)
        : settings_(std::move(settings)), host_(std::move(host)),
          worker_([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() {
        { std::lock_guard lock(mutex_); ++epoch_; event_.reset(); }
        worker_.request_stop();
        ready_.notify_one();
        worker_.join();
    }
    void send(Command command) {
        std::lock_guard lock(mutex_);
        if (finished_.load()) return;
        if ((command.kind == Command::Idle && input_ == Input::Idle) ||
            (command.kind == Command::Composing && input_ == Input::Composing)) return;
        if (command.kind == Command::Arm && input_ == Input::Idle && cached_ && command.focus == cached_->focus)
            command.before = cached_->sample;
        if (command.kind == Command::Watch || command.kind == Command::Arm || command.kind == Command::Cancel) {
            ++epoch_; event_.reset();
        }
        switch (command.kind) {
        case Command::Watch: input_ = Input::Idle; ++revision_; cached_.reset(); break;
        case Command::Arm:
        case Command::Commit:
        case Command::Cancel: input_ = Input::Unknown; ++revision_; cached_.reset(); break;
        case Command::Composing: input_ = Input::Composing; ++revision_; cached_.reset(); break;
        case Command::Idle: input_ = Input::Idle; ++revision_; break;
        case Command::Displayed: input_ = Input::Menu; ++revision_; cached_.reset(); break;
        case Command::Select: break;
        }
        command.epoch = epoch_;
        commands_.push_back(std::move(command));
        ready_.notify_one();
    }
    bool latest() {
        std::lock_guard lock(mutex_);
        return workerEpoch_ == epoch_;
    }
    std::optional<Stamp> stamp() {
        std::lock_guard lock(mutex_);
        if (workerEpoch_ != epoch_) return {};
        return Stamp{revision_, input_};
    }
    bool unchanged(const Stamp& value) {
        std::lock_guard lock(mutex_);
        return workerEpoch_ == epoch_ && revision_ == value.revision && input_ == value.input;
    }
    std::optional<Event> take() {
        std::lock_guard lock(mutex_);
        auto event = std::exchange(event_, {});
        if (event && (event->state == "showing" || event->state == "commit") &&
            ((input_ != Input::Idle && !(event->state == "commit" && input_ == Input::Menu)) ||
             !event->focus || !host_->current(*event->focus)))
            return Event{"cancelled", {}, {}};
        return event;
    }
    bool commandQueued(Command::Kind kind) {
        std::lock_guard lock(mutex_);
        return std::any_of(commands_.begin(), commands_.end(), [this, kind](const Command& command) {
            return command.kind == kind && command.epoch == workerEpoch_;
        });
    }
    void publish(Event event) {
        std::lock_guard lock(mutex_);
        if (workerEpoch_ != epoch_) return;
        event.focus = focus_;
        event_ = std::move(event);
    }
    void wake() {
        std::lock_guard lock(mutex_);
        if (!event_ || event_->woken || !event_->focus) return;
        const auto& state = event_->state;
        if (state == "idle" || state == "armed" || state == "waiting" || state == "generating") return;
        event_->woken = host_->wake(*event_->focus);
    }
    void run(std::stop_token stop) {
        try { loop(stop); }
        catch (const std::exception&) { publish({"error", {}, {}}); wake(); }
        finished_.store(true);
    }
    void loop(std::stop_token stop) {
        TextReader reader;
        std::optional<Sample> before, expected;
        Cancellation request;
        std::vector<std::string> tokens;
        enum Stage { Idle, Arming, Armed, Waiting, Generating, Showing, CommitReady } stage = Idle;
        uint64_t generation = 0;
        bool ownsComposition = false;
        auto deadline = std::chrono::steady_clock::now();
        auto validFocus = [&] { return focus_ && host_->current(*focus_); };
        auto cancel = [&] {
            if (request) request->store(true);
            ++generation;
            before.reset(); expected.reset(); tokens.clear(); stage = Idle; ownsComposition = false;
        };
        auto current = [&] {
            if (!expected || !validFocus()) return false;
            const auto read = stamp();
            const bool allowOwned = ownsComposition && (stage == Showing || stage == CommitReady);
            if (!read || (read->input != Input::Idle && !(allowOwned && read->input == Input::Menu))) return false;
            auto actual = host_->read(reader, *focus_, allowOwned ? TextRead::CompositionHint : TextRead::Plain);
            return actual && validFocus() && actual->matches(*expected, allowOwned) && unchanged(*read) &&
                (stage != Waiting || std::chrono::steady_clock::now() < deadline);
        };
        // 初次输入可能还没有空闲快照。仅在 Rime 确认正在组字时，才尝试投影 UIA 的范围提示。
        // 此处不能把缺少提示的正文当成组字前基线，也不能在提交之后补采这个基线。
        auto acquireBefore = [&] {
            const auto read = stamp();
            if (stage != Arming || !read || read->input != Input::Composing || !validFocus()) return;
            auto sample = host_->read(reader, *focus_, TextRead::CompositionHint);
            if (sample && sample->composition && validFocus() && unchanged(*read)) {
                before = sample->afterCommit("");
                stage = Armed;
                publish({"armed", {}, {}});
            }
        };
        CompletionClient client(
            [this](CompletionClient::Task task) {
                std::lock_guard lock(mutex_);
                callbacks_.push_back(std::move(task)); ready_.notify_one();
            },
            [&](PredictionResult result) {
                if (result.generation != generation || stage != Generating) return;
                if (!current()) { cancel(); publish({"cancelled", {}, {}}); return; }
                if (!std::holds_alternative<std::vector<std::string>>(result.value)) {
                    cancel(); publish({"api-error", {}, {}}); return;
                }
                tokens = std::get<std::vector<std::string>>(std::move(result.value));
                stage = Showing; publish({"showing", tokens, {}});
            });
        while (!stop.stop_requested()) {
            std::deque<Command> commands;
            std::deque<CompletionClient::Task> callbacks;
            {
                std::unique_lock lock(mutex_);
                ready_.wait_for(lock, 30ms, [&] { return stop.stop_requested() || !commands_.empty() || !callbacks_.empty(); });
                commands.swap(commands_); callbacks.swap(callbacks_);
            }
            if (stop.stop_requested()) break;
            for (auto& command : commands) {
                workerEpoch_ = command.epoch;
                if (!latest()) continue;
                switch (command.kind) {
                case Command::Watch:
                    cancel(); focus_ = command.focus; publish({"idle", {}, {}}); break;
                case Command::Arm:
                    cancel(); focus_ = command.focus;
                    before = std::move(command.before);
                    if (!validFocus()) { before.reset(); publish({"unavailable", {}, {}}); }
                    else {
                        stage = before ? Armed : Arming;
                        if (before) publish({"armed", {}, {}});
                    }
                    break;
                case Command::Composing:
                case Command::Idle:
                    break;
                case Command::Commit:
                    if (stage == Armed && before && validFocus()) expected = before->afterCommit(command.text);
                    else if (stage == CommitReady && expected && validFocus()) expected = expected->afterCommit(command.text);
                    else { cancel(); publish({"unavailable", {}, {}}); break; }
                    before.reset(); stage = Waiting; ownsComposition = false;
                    deadline = std::chrono::steady_clock::now() + 1s;
                    publish({"waiting", {}, {}});
                    break;
                case Command::Cancel:
                    cancel(); focus_.reset(); publish({"cancelled", {}, {}}); break;
                case Command::Displayed:
                    if (stage == Showing) ownsComposition = true;
                    break;
                case Command::Select:
                    if (stage == Showing && command.index < tokens.size() && current()) {
                        auto text = tokens[command.index]; tokens.clear(); stage = CommitReady;
                        deadline = std::chrono::steady_clock::now() + 1s;
                        publish({"commit", {}, std::move(text)});
                    } else { cancel(); publish({"cancelled", {}, {}}); }
                    break;
                }
            }
            acquireBefore();
            for (auto& callback : callbacks) callback();
            if (stage == Waiting) {
                if (!validFocus()) { cancel(); publish({"cancelled", {}, {}}); }
                else if (std::chrono::steady_clock::now() >= deadline) { cancel(); publish({"timeout", {}, {}}); }
                else if (current()) {
                    const auto prefix = expected->snapshot.prefix(settings_.contextCharacters);
                    if (prefix.empty()) { cancel(); publish({"unavailable", {}, {}}); }
                    else {
                        request = client.submit(generation, settings_, prefix);
                        stage = Generating; publish({"generating", {}, {}});
                    }
                }
            } else if ((stage == Generating || stage == Showing || stage == CommitReady) && !current()) {
                if (!(stage == CommitReady && commandQueued(Command::Commit)) &&
                    !(stage == Showing && commandQueued(Command::Displayed))) {
                    cancel(); publish({"cancelled", {}, {}});
                }
            } else if (stage == CommitReady && std::chrono::steady_clock::now() >= deadline && !commandQueued(Command::Commit)) {
                cancel(); publish({"timeout", {}, {}});
            } else if ((stage == Armed || stage == Arming) && !validFocus()) { cancel(); publish({"cancelled", {}, {}}); }

            // 只缓存整段读取都位于同一空闲期的真实快照。Arm 在 Lua 线程冻结它，跨越按键的晚到读取作废。
            const auto idle = stamp();
            if (idle && idle->input == Input::Idle && validFocus() && (stage == Idle || stage == Armed)) {
                auto sample = host_->read(reader, *focus_, TextRead::Plain);
                const bool valid = validFocus();
                std::lock_guard lock(mutex_);
                if (workerEpoch_ == epoch_ && revision_ == idle->revision && input_ == Input::Idle) {
                    if (sample && valid) cached_ = Cached{*focus_, sample->plain()};
                    else cached_.reset();
                }
            }
            wake();
        }
        cancel();
    }
    Settings settings_;
    std::unique_ptr<Host> host_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Command> commands_;
    std::deque<CompletionClient::Task> callbacks_;
    std::optional<Event> event_;
    std::optional<Focus> focus_;
    std::optional<Cached> cached_;
    uint64_t epoch_ = 0, workerEpoch_ = 0, revision_ = 0;
    Input input_ = Input::Unknown;
    std::atomic_bool finished_ = false;
    std::jthread worker_;
};
Session::Session(Settings settings, std::unique_ptr<Host> host) : impl_(std::make_unique<Impl>(std::move(settings), std::move(host))) {}
Session::~Session() = default;
void Session::watch(std::string_view app, std::string_view clientType) {
    Command command{Command::Watch, {}};
    command.focus = impl_->host_->capture(app, clientType);
    impl_->send(std::move(command));
}
void Session::arm(std::string_view app, std::string_view clientType) {
    Command command{Command::Arm, {}};
    command.focus = impl_->host_->capture(app, clientType);
    impl_->send(std::move(command));
}
void Session::send(Command command) { impl_->send(std::move(command)); }
std::optional<Event> Session::take() { return impl_->take(); }
} // namespace rome::weasel
