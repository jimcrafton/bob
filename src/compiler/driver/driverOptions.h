#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

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
    bool explainDispatch = false;
    bool syntaxOnly = false;
    int optLevel = 0;
    std::string outputPath;
    std::string linkerPath;
    std::string runtimeDir;
    std::string sdkLibPath;
    // Where `import` looks for interfaces (after the source file's own directory).
    std::vector<std::string> modulePaths;
    bool compileOnly = false;
    bool verbose = false;
    bool showHelp = false;
    bool showVersion = false;
};

std::optional<std::string> parseDriverArguments(std::span<const std::string> args, DriverOptions& options);

std::string defaultOutputPath(const DriverOptions& options);

std::string replaceExtension(const std::string& path, const char* extension);

const char* driverUsage();

}
