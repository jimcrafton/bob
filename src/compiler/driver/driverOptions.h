#pragma once

#include <optional>
#include <span>
#include <string>

namespace bob::driver {

struct DriverOptions {
    std::string inputPath;
    std::string outputPath;
    std::string linkerPath;
    std::string runtimeDir;
    std::string sdkLibPath;
    bool compileOnly = false;
    bool verbose = false;
    bool showHelp = false;
    bool showVersion = false;
};

std::optional<std::string> parseDriverArguments(std::span<const std::string> args, DriverOptions& options);

std::string defaultOutputPath(const DriverOptions& options);

const char* driverUsage();

}
