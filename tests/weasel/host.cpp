#include "host.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        auto host = rome::weasel::createWindowsHost();
        if (host->capture("", "tsf") || host->capture("not-a-client.exe", "") ||
            host->capture("not-a-client.exe", "imm")) throw std::runtime_error("unbound client accepted");
        rome::weasel::Focus missing;
        if (host->current(missing) || host->wake(missing)) throw std::runtime_error("missing target accepted");
        // 无绑定时连 UIA 文本读取都不进入；wake 也不得注入按键。
        rome::weasel::TextReader reader;
        if (host->read(reader, missing))
            throw std::runtime_error("unbound target reached UIA");
        std::cout << "PASS: unbound/IMM clients rejected, no desktop read or wake\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
