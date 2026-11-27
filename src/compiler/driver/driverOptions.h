#pragma once

#include <optional>
#include <span>
#include <string>

namespace bob::driver {

enum class AstDumpFormat {
    none,
    text,
    json,
};

struct DriverOptions {
    std::string inputPath;
    AstDumpFormat dumpAst = AstDumpFormat::none;
    bool dumpTokens = false;
    bool dumpLlvm = false;
    bool syntaxOnly = false;
    int optLevel = 0;
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
