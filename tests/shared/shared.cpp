#include "completion.h"
#include "settings.h"
#include "surrounding.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <json/json.h>

using namespace rome;
using namespace std::chrono_literals;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action> void rejects(Action action, const char *message) {
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void surroundingTest() {
    const auto read = [](std::string text, size_t cursor, size_t anchor, OffsetUnit unit) {
        auto value = SurroundingSnapshot::fromUtf8(std::move(text), cursor, anchor, unit);
        require(value.has_value(), "valid snapshot rejected");
        return *value;
    };
    const std::string text = "A😀中é尾"; // e 和组合重音是两个码点。
    auto codepoints = read(text, 5, 5, OffsetUnit::CodePoints);
    auto utf16 = read(text, 6, 6, OffsetUnit::Utf16);
    require(codepoints == utf16, "offset units must normalize to the same snapshot");
    require(utf16.prefix(4) == "😀中é", "prefix slices Unicode codepoints, not bytes");
    require(utf16.prefix(2) == "é", "combining codepoints retained");
    require(utf16.prefix(0).empty(), "zero-length window");
    require(utf16.prefix(std::numeric_limits<size_t>::max()) == "A😀中é", "large window");
    require(read(text, 2, 4, OffsetUnit::CodePoints).afterCommit("🚀好") ==
            read("A😀🚀好́尾", 4, 4, OffsetUnit::CodePoints), "forward selection replacement");
    require(read(text, 5, 3, OffsetUnit::Utf16).afterCommit("🚀好") ==
            read("A😀🚀好́尾", 6, 6, OffsetUnit::Utf16), "reversed UTF-16 selection replacement");
    require(read("中尾", 1, 1, OffsetUnit::CodePoints).afterCommit("😀") ==
            read("中😀尾", 3, 3, OffsetUnit::Utf16), "commit cursor uses codepoints");
    require(read(text, 2, 4, OffsetUnit::CodePoints).afterCommit("").text() == "A😀́尾",
            "empty replacement deletes selection");
    require(read("", 0, 0, OffsetUnit::Utf16).prefix(2).empty(), "empty document is valid");
    require(!(utf16 == read(text, 5, 6, OffsetUnit::Utf16)), "selection changes invalidate snapshot");
    require(!(utf16 == read("A😀中é改", 6, 6, OffsetUnit::Utf16)), "suffix changes invalidate snapshot");
    require(!(utf16 == read(text, 5, 5, OffsetUnit::Utf16)), "cursor changes invalidate snapshot");
    require(!SurroundingSnapshot::fromUtf8(text, 2, 2, OffsetUnit::Utf16), "split surrogate cursor");
    require(!SurroundingSnapshot::fromUtf8(text, 3, 2, OffsetUnit::Utf16), "split surrogate anchor");
    require(!SurroundingSnapshot::fromUtf8(text, 7, 0, OffsetUnit::CodePoints), "out of range cursor");
    require(!SurroundingSnapshot::fromUtf8(text, 0, 8, OffsetUnit::Utf16), "out of range anchor");
    require(!SurroundingSnapshot::fromUtf8(text, std::numeric_limits<size_t>::max(), 0,
                                         OffsetUnit::Utf16), "overflow-sized offset");
    for (const std::string invalid : {"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
                                     "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
                                     "\xe5", "\xe5\xb0", "\xe5x\x80", "\xff"}) {
        require(!SurroundingSnapshot::fromUtf8(invalid, 0, 0, OffsetUnit::CodePoints),
                "invalid UTF-8 accepted");
        rejects([&] { utf16.afterCommit(invalid); }, "invalid commit accepted");
    }
    // 预期文本只能用来匹配应用回读，不能把旧快照当作提交确认。
    const auto before = read("今天尾", 2, 2, OffsetUnit::Utf16);
    const auto expected = before.afterCommit("😀");
    require(!(before == expected), "old snapshot cannot acknowledge a commit");
    require(expected == read("今天😀尾", 4, 4, OffsetUnit::Utf16), "post-commit acknowledgement");
    require(!(expected == read("今天😀尾", 2, 2, OffsetUnit::Utf16)), "stale caret rejected");
}

void settingsTest(const std::filesystem::path &work) {
    auto directory = work / std::filesystem::path(u8"settings-中文-😀-");
    directory += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directory(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    const auto defaults = directory / "defaults.yaml";
    const auto overrides = directory / "user.yaml";
    const auto write = [](const auto &path, const std::string &text) {
        std::ofstream file(path);
        file << text;
        require(file.good(), "cannot write test configuration");
    };
    write(defaults, "enabled: true\nbase_url: https://example.invalid/v1///\nmodel: shared-model\n"
                    "api_key: global-key\ncandidates: 5\ntemperature: 0.7\ncontext_chars: 1024\n"
                    "timeout_ms: 2000\n");
    auto global = loadSettings(defaults);
    validateSettings(global);
    require(global.enabled && global.baseUrl == "https://example.invalid/v1" &&
            global.model == "shared-model" && global.apiKey == "global-key" &&
            global.candidates == 5 && global.contextCharacters == 1024, "global settings");
    require(loadSettings(defaults, overrides).model == global.model, "missing optional user config");
    write(overrides, "enabled: false\napi_key: ''\ncandidates: 3\ncontext_chars: 256\n");
    auto merged = loadSettings(defaults, overrides);
    validateSettings(merged);
    require(!merged.enabled && merged.apiKey.empty() && merged.candidates == 3 &&
            merged.contextCharacters == 256 && merged.model == global.model &&
            merged.temperature == global.temperature && merged.timeoutMs == global.timeoutMs,
            "partial user override preserves global fields");
    for (const std::string empty : {"", "null\n", "{}\n"}) {
        write(overrides, empty);
        require(loadSettings(defaults, overrides).apiKey == "global-key", "empty override");
    }
    for (const std::string invalid : {"[]\n", "unknown: true\n", "candidates: nope\n"}) {
        write(overrides, invalid);
        rejects([&] { loadSettings(defaults, overrides); }, "invalid user config accepted");
    }
    rejects([&] { loadSettings(directory / "missing.yaml"); }, "missing defaults accepted");
    write(defaults, "[]\n");
    rejects([&] { loadSettings(defaults); }, "invalid defaults accepted");
    for (const std::string invalid : {"ftp://example.invalid", "http://", "http:///v1",
                                     "http://user@example.invalid", "http://example.invalid?v=1",
                                     "http://example.invalid#v1", "http://example.invalid/completions"}) {
        auto settings = global;
        settings.baseUrl = invalid;
        rejects([&] { validateSettings(settings); }, "invalid URL accepted");
    }
    for (const int candidates : {0, 10}) {
        auto settings = global;
        settings.candidates = candidates;
        rejects([&] { validateSettings(settings); }, "invalid candidate count accepted");
    }
    for (const double temperature : {-0.1, 2.1, std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::quiet_NaN()}) {
        auto settings = global;
        settings.temperature = temperature;
        rejects([&] { validateSettings(settings); }, "invalid temperature accepted");
    }
    for (const int limit : {0, 8193}) {
        auto settings = global;
        settings.contextCharacters = limit;
        rejects([&] { validateSettings(settings); }, "invalid context window accepted");
    }
    for (const int timeout : {99, 30001}) {
        auto settings = global;
        settings.timeoutMs = timeout;
        rejects([&] { validateSettings(settings); }, "invalid timeout accepted");
    }
    auto invalid = global;
    invalid.model.clear();
    rejects([&] { validateSettings(invalid); }, "missing model accepted");
    invalid = global;
    invalid.apiKey = "key\r\nInjected: header";
    rejects([&] { validateSettings(invalid); }, "header injection accepted");
}

// 用可控队列模拟宿主事件循环。没有 Fcitx、librime 或桌面会话依赖。
class Queue {
public:
    void push(CompletionClient::Task task) {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
        ready_.notify_one();
    }
    CompletionClient::Task take() {
        std::unique_lock lock(mutex_);
        require(ready_.wait_for(lock, 5s, [&] { return !tasks_.empty(); }), "dispatch timeout");
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<CompletionClient::Task> tasks_;
};

void clientTest(const std::string &url, const std::string &scenario) {
    Settings settings;
    settings.baseUrl = url;
    settings.model = "shared-test-model";
    settings.apiKey = scenario == "auth" ? "shared-test-key" : "";
    settings.timeoutMs = scenario == "timeout" ? 100 : 2000;
    validateSettings(settings);
    Queue queue;
    std::vector<PredictionResult> results;
    const auto mainThread = std::this_thread::get_id();
    auto client = std::make_unique<CompletionClient>(
        [&](CompletionClient::Task task) { queue.push(std::move(task)); },
        [&](PredictionResult result) {
            require(std::this_thread::get_id() == mainThread, "callback bypassed host dispatcher");
            results.push_back(std::move(result));
        });
    auto token = client->submit(1, settings, "今天😀一起");
    if (scenario.starts_with("active-")) {
        // Python 等服务器收到第一个请求后才放行，避免用 sleep 猜测请求状态。
        std::cout << "ready" << std::endl;
        require(std::cin.get() == '\n', "missing server handshake");
        if (scenario == "active-shutdown") {
            const auto start = std::chrono::steady_clock::now();
            client.reset();
            require(std::chrono::steady_clock::now() - start < 1s, "shutdown did not cancel active HTTP");
        } else {
            if (scenario == "active-cancel") token->store(true);
            client->submit(2, settings, "second");
            if (scenario == "active-replaced") client->submit(3, settings, "latest");
            while (results.empty()) queue.take()();
            require(results.size() == 1 && results[0].generation ==
                    (scenario == "active-replaced" ? 3u : 2u), "stale active result delivered");
        }
    } else {
        auto first = queue.take();
        require(results.empty(), "callback ran before host dispatched it");
        if (scenario == "queued-cancel") {
            token->store(true);
            first();
            require(results.empty(), "cancelled queued callback delivered");
        } else if (scenario == "queued-replaced") {
            client->submit(2, settings, "second");
            auto second = queue.take();
            first();
            second();
            require(results.size() == 1 && results[0].generation == 2, "superseded queued callback delivered");
        } else if (scenario == "queued-destroyed") {
            client.reset();
            first();
            require(results.empty(), "queued callback survived client destruction");
        } else {
            first();
            require(results.size() == 1 && results[0].generation == 1, "result generation lost");
        }
    }
    Json::Value output;
    output["results"] = Json::Value(Json::arrayValue);
    for (const auto &result : results) {
        Json::Value entry;
        entry["generation"] = Json::UInt64(result.generation);
        if (const auto *error = std::get_if<std::string>(&result.value)) {
            entry["error"] = *error;
        } else {
            entry["tokens"] = Json::Value(Json::arrayValue);
            for (const auto &text : std::get<std::vector<std::string>>(result.value)) entry["tokens"].append(text);
        }
        output["results"].append(entry);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::cout << Json::writeString(writer, output) << std::endl;
}

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "test scenario required");
        const std::string scenario = argv[1];
        if (scenario == "surrounding") {
            surroundingTest();
        } else if (scenario == "settings") {
            require(argc == 3, "settings work directory required");
            settingsTest(argv[2]);
        } else {
            require(argc == 3, "HTTP URL required");
            clientTest(argv[2], scenario);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
