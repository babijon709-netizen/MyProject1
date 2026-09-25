#include "logfile.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: logfile_test <log-path>\n";
        return 2;
    }

    LogSetPathForTesting(argv[1]);
    if (!LogOpen()) {
        std::cerr << "logger failed to open its test path (errno=" << LogLastError() << ")\n";
        return 1;
    }
    if (std::string(LogPath()) != argv[1]) {
        std::cerr << "logger reported the wrong path\n";
        LogClose();
        return 1;
    }

    LogLine("аим: выбор — союзники %d", 2);
    LogLine("TeamData roster stage=%s", "members-read");
    LogClose();

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "log file was not created\n";
        return 1;
    }
    const std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    if (content.find("diagnostic session started") == std::string::npos ||
        content.find("аим: выбор — союзники 2") == std::string::npos ||
        content.find("TeamData roster stage=members-read") == std::string::npos ||
        content.find("diagnostic session ended") == std::string::npos) {
        std::cerr << "expected logger records are missing\n";
        return 1;
    }
    std::cout << "file logger writes session and formatted aim diagnostics: OK\n";
    return 0;
}
