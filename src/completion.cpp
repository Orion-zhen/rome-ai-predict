#include "completion.h"
#include "unicode.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

namespace rome {
namespace {
class RequestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Body {
    std::string text;
    bool tooLarge = false;
};

size_t receive(char *data, size_t size, size_t count, void *opaque) {
    auto &body = *static_cast<Body *>(opaque);
    const auto bytes = size * count;
    constexpr size_t limit = 1024 * 1024;
    if (bytes > limit - body.text.size()) {
        body.tooLarge = true;
        return 0;
    }
    body.text.append(data, bytes);
    return bytes;
}

std::vector<std::string> parseResponse(const std::string &body, int maximum) {
    Json::CharReaderBuilder builder;
    builder["stackLimit"] = 32;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &errors)) {
        throw RequestError("invalid JSON response");
    }
    if (!root.isObject() || !root["choices"].isArray()) {
        throw RequestError("response must contain choices[]");
    }
    if (root["choices"].empty()) return {};
    const auto &choice = root["choices"][0];
    if (!choice.isObject() || !choice["logprobs"].isObject()) {
        throw RequestError("response lacks logprobs for the first completion");
    }
    const auto &logprobs = choice["logprobs"];
    struct RankedToken {
        std::string text;
        double logprob;
    };
    std::vector<RankedToken> ranked;
    const auto score = [](const Json::Value &value) {
        if (!value.isNumeric() || !std::isfinite(value.asDouble())) {
            throw RequestError("token logprob must be a finite number");
        }
        return value.asDouble();
    };
    if (logprobs.isMember("content")) {
        // 当前 llama.cpp 的 Completions 接口返回与 Chat API 相同的 token 对象。
        const auto &positions = logprobs["content"];
        if (!positions.isArray()) throw RequestError("logprobs.content must be an array");
        if (positions.empty()) return {};
        if (!positions[0].isObject() || !positions[0]["top_logprobs"].isArray()) {
            throw RequestError("first position must contain top_logprobs[]");
        }
        for (const auto &entry : positions[0]["top_logprobs"]) {
            if (!entry.isObject() || !entry["token"].isString() || !entry.isMember("bytes")) {
                throw RequestError("top_logprobs entries require token, bytes and logprob");
            }
            const auto probability = score(entry["logprob"]);
            const auto &bytes = entry["bytes"];
            std::string text;
            if (!bytes.isNull()) {
                if (!bytes.isArray()) throw RequestError("token bytes must be an array or null");
                for (const auto &byte : bytes) {
                    if (!byte.isUInt() || byte.asUInt() > 255) {
                        throw RequestError("token bytes must be integers in 0..255");
                    }
                    text.push_back(static_cast<char>(byte.asUInt()));
                }
            }
            // bytes 才是可上屏内容，token 字段可能含 tokenizer 的展示标记。
            ranked.push_back({std::move(text), probability});
        }
    } else if (logprobs.isMember("top_logprobs")) {
        // 标准 Completions：每个位置是一张 token -> logprob 映射表。
        const auto &positions = logprobs["top_logprobs"];
        if (!positions.isArray()) throw RequestError("logprobs.top_logprobs must be an array");
        if (positions.empty()) return {};
        if (!positions[0].isObject()) throw RequestError("first top_logprobs position must be a map");
        for (const auto &token : positions[0].getMemberNames()) {
            ranked.push_back({token, score(positions[0][token])});
        }
    } else {
        throw RequestError("unsupported logprobs format: no token probabilities");
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.logprob > b.logprob;
    });
    // 先取概率最高的 N 项，再过滤不可直接上屏的项，不拿更低概率的 token 补位。
    if (ranked.size() > static_cast<size_t>(maximum)) ranked.resize(maximum);
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (auto &token : ranked) {
        auto &text = token.text;
        const auto boundaries = detail::unicodeBoundaries(text);
        if (text.empty() || text.find_first_not_of(' ') == std::string::npos ||
            !boundaries || boundaries->size() - 1 > 128 ||
            std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
            continue;
        }
        // 保留完整 token 和英文前导空格，不裁剪 token 内容。
        if (seen.insert(text).second) result.push_back(std::move(text));
    }
    return result;
}

std::vector<std::string> complete(const Settings &s, const std::string &prefix,
                                  const Cancellation &cancelled, std::stop_token stop) {
    using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    using Multi = std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)>;
    using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
    Easy easy(curl_easy_init(), curl_easy_cleanup);
    Multi multi(curl_multi_init(), curl_multi_cleanup);
    if (!easy || !multi) throw RequestError("could not create HTTP request");

    Json::Value request;
    request["model"] = s.model;
    request["prompt"] = prefix;
    request["n"] = 1;
    request["max_tokens"] = 1;
    request["logprobs"] = s.candidates;
    request["temperature"] = s.temperature;
    request["stream"] = false;
    request["echo"] = false;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const auto payload = Json::writeString(writer, request);
    const auto url = s.baseUrl + "/completions";
    Headers headers(curl_slist_append(nullptr, "Content-Type: application/json"), curl_slist_free_all);
    if (!headers) throw RequestError("could not create HTTP headers");
    if (!s.apiKey.empty()) {
        auto *appended = curl_slist_append(headers.get(), ("Authorization: Bearer " + s.apiKey).c_str());
        if (!appended) throw RequestError("could not create authorization header");
        static_cast<void>(headers.release());
        headers.reset(appended);
    }
    Body body;
    const auto option = [&easy](CURLoption key, auto value) {
        if (curl_easy_setopt(easy.get(), key, value) != CURLE_OK) {
            throw RequestError("could not configure HTTP request");
        }
    };
    option(CURLOPT_URL, url.c_str());
    option(CURLOPT_HTTPHEADER, headers.get());
    option(CURLOPT_POSTFIELDS, payload.c_str());
    option(CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    option(CURLOPT_WRITEFUNCTION, &receive);
    option(CURLOPT_WRITEDATA, &body);
    option(CURLOPT_TIMEOUT_MS, static_cast<long>(s.timeoutMs));
    option(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min(s.timeoutMs, 1000)));
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_FOLLOWLOCATION, 0L);
    option(CURLOPT_PROTOCOLS_STR, "http,https");
    if (curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) {
        throw RequestError("could not start HTTP request");
    }
    struct Detach {
        CURLM *multi;
        CURL *easy;
        ~Detach() { curl_multi_remove_handle(multi, easy); }
    } detach{multi.get(), easy.get()};

    int running = 0;
    do {
        if (cancelled->load() || stop.stop_requested()) return {};
        if (curl_multi_perform(multi.get(), &running) != CURLM_OK) {
            throw RequestError("HTTP transfer failed");
        }
        if (running && curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr) != CURLM_OK) {
            throw RequestError("HTTP polling failed");
        }
    } while (running);

    int messages = 0;
    auto *message = curl_multi_info_read(multi.get(), &messages);
    if (!message || message->msg != CURLMSG_DONE) throw RequestError("HTTP transfer did not finish");
    if (body.tooLarge) throw RequestError("response exceeds 1 MiB limit");
    if (message->data.result != CURLE_OK) {
        throw RequestError(std::string("HTTP request failed: ") + curl_easy_strerror(message->data.result));
    }
    long status = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw RequestError("HTTP status " + std::to_string(status));
    return parseResponse(body.text, s.candidates);
}
} // namespace

CompletionClient::CompletionClient(Dispatch dispatch,
                                   std::function<void(PredictionResult)> callback)
    : dispatch_(std::move(dispatch)), callback_(std::move(callback)) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        throw std::runtime_error("could not initialize libcurl");
    }
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

CompletionClient::~CompletionClient() {
    {
        std::lock_guard lock(mutex_);
        worker_.request_stop();
        if (active_) active_->store(true);
        if (pending_) pending_->cancelled->store(true);
    }
    ready_.notify_one();
    worker_.join();
    curl_global_cleanup();
}

Cancellation CompletionClient::submit(uint64_t generation, Settings settings, std::string prefix) {
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        if (active_) active_->store(true);
        if (pending_) pending_->cancelled->store(true);
        pending_ = Request{generation, std::move(settings), std::move(prefix), cancelled};
    }
    ready_.notify_one();
    return cancelled;
}

void CompletionClient::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stop.stop_requested() || pending_.has_value(); });
        if (stop.stop_requested()) return;
        auto request = std::move(*pending_);
        pending_.reset();
        active_ = request.cancelled;
        lock.unlock();
        if (request.cancelled->load()) continue;

        PredictionResult result{request.generation, std::vector<std::string>{}};
        try {
            result.value = complete(request.settings, request.prefix, request.cancelled, stop);
        } catch (const RequestError &error) {
            result.value = std::string(error.what());
        }
        if (stop.stop_requested()) return;
        dispatch_([callback = callback_, token = request.cancelled,
                   result = std::move(result)]() mutable {
            if (!token->load()) callback(std::move(result));
        });
    }
}
} // namespace rome
