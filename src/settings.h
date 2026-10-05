#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace rome {

struct Settings {
    bool enabled = false;
    std::string baseUrl = "http://127.0.0.1:8000/v1";
    std::string model;
    std::string apiKey;
    int candidates = 3;
    double temperature = 0.7;
    int timeoutMs = 2000;
    int contextCharacters = 256;
};

// 平台负责定位路径。默认配置必须存在，用户配置可缺省或不存在。
Settings loadSettings(const std::filesystem::path &defaults,
                      const std::optional<std::filesystem::path> &overrides = std::nullopt);
// 校验合并后的 API 配置。错误由插件边界转成禁用状态，不影响输入。
void validateSettings(const Settings &settings);

} // namespace rome
