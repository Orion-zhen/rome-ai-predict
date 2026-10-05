#include "completion.h"
#include "settings.h"

#include <chrono>
#include <iostream>
#include <string>
#include <fcitx-utils/event.h>
#include <json/json.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: rome-ai-probe /path/rome-ai-predict.yaml '光标前文'\n";
        return 2;
    }
    try {
        auto settings = rome::loadSettings(argv[1]);
        rome::validateSettings(settings);
        fcitx::EventLoop loop;
        int exitCode = 0;
        const auto started = std::chrono::steady_clock::now();
        rome::CompletionClient client(loop, [&](rome::PredictionResult result) {
            if (auto *error = std::get_if<std::string>(&result.value)) {
                std::cerr << *error << '\n';
                exitCode = 1;
            } else {
                Json::Value output;
                output["endpoint"] = settings.baseUrl + "/completions";
                output["model"] = settings.model;
                output["elapsed_ms"] = static_cast<Json::Int64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count());
                output["candidates"] = Json::Value(Json::arrayValue);
                for (const auto &text : std::get<std::vector<std::string>>(result.value)) {
                    output["candidates"].append(text);
                }
                Json::StreamWriterBuilder writer;
                writer["emitUTF8"] = true;
                std::cout << Json::writeString(writer, output) << '\n';
            }
            loop.exit();
        });
        client.submit(1, settings, argv[2]);
        loop.exec();
        return exitCode;
    } catch (const std::exception &error) {
        std::cerr << "Configuration/probe error: " << error.what() << '\n';
        return 2;
    }
}
