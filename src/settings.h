#pragma once

#include <filesystem>
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

// 读取 Fcitx5 数据目录中的全局默认配置，再应用可选用户配置的字段覆盖。
Settings loadSettings(const std::filesystem::path &path);
// 校验合并后的 API 配置。错误由插件边界转成禁用状态，不影响输入。
void validateSettings(const Settings &settings);

} // namespace rome
