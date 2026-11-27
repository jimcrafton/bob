#include "driver/driverOptions.h"

#include <string_view>

namespace bob::driver {

namespace {

bool takeValue(std::string_view arg, std::string_view prefix, std::string& out)
{
    if (!arg.starts_with(prefix)) {
        return false;
    }
    out = std::string(arg.substr(prefix.size()));
    return true;
}

}

std::string replaceExtension(const std::string& path, const char* extension)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot) + extension;
    }
    return path + extension;
}

std::optional<std::string> parseDriverArguments(std::span<const std::string> args, DriverOptions& options)
{
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        }
        else if (arg == "--version") {
            options.showVersion = true;
        }
        else if (arg == "-c") {
            options.compileOnly = true;
        }
        else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        }
        else if (arg == "--dump-ast" || arg == "--dump-ast=text") {
            options.dumpAst = AstDumpFormat::text;
        }
        else if (arg == "--dump-ast=json") {
            options.dumpAst = AstDumpFormat::json;
        }
        else if (arg == "--dump-tokens") {
            options.dumpTokens = true;
        }
        else if (arg == "--dump-llvm") {
            options.dumpLlvm = true;
        }
        else if (arg == "--explain-dispatch") {
            options.explainDispatch = true;
        }
        else if (arg.size() == 3 && arg.starts_with("-O") && arg[2] >= '0' && arg[2] <= '3') {
            options.optLevel = arg[2] - '0';
        }
        else if (arg == "--syntax-only") {
            options.syntaxOnly = true;
        }
        else if (arg == "-o") {
            if (i + 1 >= args.size()) {
                return std::string("-o requires a path");
            }
            options.outputPath = args[++i];
        }
        else if (takeValue(arg, "--linker=", options.linkerPath)) {
        }
        else if (takeValue(arg, "--runtime-dir=", options.runtimeDir)) {
        }
        else if (takeValue(arg, "--sdk-lib-path=", options.sdkLibPath)) {
        }
        else if (arg.starts_with("--module-path=")) {
            options.modulePaths.push_back(std::string(arg.substr(14)));
        }
        else if (arg.starts_with("-")) {
            return "unknown option '" + std::string(arg) + "'";
        }
        else if (options.inputPath.empty()) {
            options.inputPath = std::string(arg);
        }
        else {
            return "only one input file is supported, got '" + options.inputPath + "' and '" + std::string(arg) + "'";
        }
    }

    if (options.showHelp || options.showVersion) {
        return std::nullopt;
    }
    if (options.inputPath.empty()) {
        return std::string("no input file");
    }
    return std::nullopt;
}

std::string defaultOutputPath(const DriverOptions& options)
{
    if (!options.outputPath.empty()) {
        return options.outputPath;
    }
    return replaceExtension(options.inputPath, options.compileOnly ? ".obj" : ".exe");
}

const char* driverUsage()
{
    return "usage: bobc [options] <input>\n"
           "\n"
           "inputs:\n"
           "  <file>.bob              bob source: a 'program' is compiled and linked into an executable;\n"
           "                          a 'module' or 'lib' into a static library (.lib) and its interface (.bobi)\n"
           "  <file>.ll               LLVM IR, compiled and linked against the bob runtime\n"
           "\n"
           "options:\n"
           "  -o <path>               output file (default: input name with .exe/.obj)\n"
           "  -c                      compile to an object file only, do not link\n"
           "  --module-path=<dir>     also look for imported modules' interfaces (.bobi) here (repeatable)\n"
           "  -O0 .. -O3              optimization level (default -O0)\n"
           "  --syntax-only           parse a .bob file and report errors, nothing else\n"
           "  --dump-tokens           print the token stream of a .bob file\n"
           "  --dump-ast[=text|json]  print the syntax tree of a .bob file\n"
           "  --dump-llvm             print the generated LLVM IR of a .bob file instead of compiling it\n"
           "  --explain-dispatch      report how each message send is compiled (intrinsic, inlined, direct, block call)\n"
           "  -v, --verbose           print the link command\n"
           "  --linker=<path>         lld-link to use (default: found at build time, then PATH)\n"
           "  --runtime-dir=<dir>     directory holding bobrt.lib and kernel32.lib (default: bobc's directory)\n"
           "  --sdk-lib-path=<dir>    fallback directory holding the Windows SDK kernel32.lib\n"
           "  --version               print version\n"
           "  -h, --help              print this help\n";
}

}
