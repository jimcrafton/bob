#include "driver/linker.h"

#include "abi.h"
#include "driver/toolchain.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Program.h>

namespace bob::driver {

std::vector<std::string> LinkJob::buildArguments(const ToolchainPaths& paths) const
{
    std::vector<std::string> args;
    args.push_back(paths.linker);
    args.push_back("/nologo");
    args.push_back("/machine:x64");
    args.push_back("/subsystem:console");
    args.push_back("/nodefaultlib");
    args.push_back(std::string("/entry:") + abi::entrySymbol);
    args.push_back("/out:" + outputPath);
    for (const std::string& object : objects) {
        args.push_back(object);
    }
    args.push_back(paths.runtimeLibrary);
    args.push_back(paths.kernel32Library);
    return args;
}

std::optional<std::string> runLink(const LinkJob& job, const ToolchainPaths& paths, std::string* commandLine)
{
    std::vector<std::string> args = job.buildArguments(paths);
    std::vector<llvm::StringRef> argRefs;
    for (const std::string& arg : args) {
        argRefs.push_back(arg);
    }

    if (commandLine != nullptr) {
        commandLine->clear();
        for (const std::string& arg : args) {
            if (!commandLine->empty()) {
                *commandLine += ' ';
            }
            *commandLine += arg.find(' ') != std::string::npos ? "\"" + arg + "\"" : arg;
        }
    }

    std::string error;
    bool executionFailed = false;
    int result = llvm::sys::ExecuteAndWait(paths.linker, argRefs, std::nullopt, {}, 0, 0, &error, &executionFailed);
    if (executionFailed) {
        return "could not run linker '" + paths.linker + "': " + error;
    }
    if (result != 0) {
        return "linker failed with exit code " + std::to_string(result);
    }
    return std::nullopt;
}

}
