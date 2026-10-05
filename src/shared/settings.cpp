#include "settings.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

namespace rome {
namespace {
void apply(Settings &s, const YAML::Node &node) {
    if (!node || node.IsNull()) {
        return;
    }
    if (!node.IsMap()) {
        throw std::runtime_error("configuration must be a map");
    }
    const std::unordered_set<std::string> names{
        "enabled", "base_url", "model", "api_key", "candidates",
        "temperature", "timeout_ms", "context_chars"};
    for (const auto &item : node) {
        if (!names.contains(item.first.as<std::string>())) {
            throw std::runtime_error("unknown rome-ai-predict configuration key");
        }
    }
    if (node["enabled"]) s.enabled = node["enabled"].as<bool>();
    if (node["base_url"]) s.baseUrl = node["base_url"].as<std::string>();
    if (node["model"]) s.model = node["model"].as<std::string>();
    if (node["api_key"]) s.apiKey = node["api_key"].as<std::string>();
    if (node["candidates"]) s.candidates = node["candidates"].as<int>();
    if (node["temperature"]) s.temperature = node["temperature"].as<double>();
    if (node["timeout_ms"]) s.timeoutMs = node["timeout_ms"].as<int>();
    if (node["context_chars"]) s.contextCharacters = node["context_chars"].as<int>();
}
}

Settings loadSettings(const std::filesystem::path &defaults,
                      const std::optional<std::filesystem::path> &overrides) {
    Settings s;
    apply(s, YAML::LoadFile(defaults.string()));
    if (overrides && std::filesystem::exists(*overrides)) {
        apply(s, YAML::LoadFile(overrides->string()));
    }
    while (s.baseUrl.ends_with('/')) s.baseUrl.pop_back();
    return s;
}

void validateSettings(const Settings &s) {
    if (!s.baseUrl.starts_with("http://") && !s.baseUrl.starts_with("https://")) {
        throw std::runtime_error("base_url must start with http:// or https://");
    }
    const auto authority = s.baseUrl.find("://") + 3;
    if (authority >= s.baseUrl.size() || s.baseUrl[authority] == '/' ||
        s.baseUrl.find_first_of("?#@ \t\r\n") != std::string::npos ||
        s.baseUrl.ends_with("/completions")) {
        throw std::runtime_error("base_url must be an API root, e.g. http://127.0.0.1:8000/v1");
    }
    if (s.model.empty()) throw std::runtime_error("model is required");
    if (s.apiKey.find_first_of("\r\n") != std::string::npos) {
        throw std::runtime_error("api_key must not contain line breaks");
    }
    if (s.candidates < 1 || s.candidates > 9) throw std::runtime_error("candidates must be 1..9");
    if (!std::isfinite(s.temperature) || s.temperature < 0 || s.temperature > 2) {
        throw std::runtime_error("temperature must be 0..2");
    }
    if (s.timeoutMs < 100 || s.timeoutMs > 30000) throw std::runtime_error("timeout_ms must be 100..30000");
    if (s.contextCharacters < 1 || s.contextCharacters > 8192) {
        throw std::runtime_error("context_chars must be 1..8192");
    }
}
} // namespace rome
