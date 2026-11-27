#include "driver/driver.h"

#include "driver/linker.h"
#include "driver/objectEmitter.h"
#include "driver/toolchain.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

namespace bob::driver {

Driver::Driver(llvm::raw_ostream& out, llvm::raw_ostream& err) : out_(out), err_(err)
{
}

int Driver::fail(const std::string& message)
{
    err_ << "bobc: error: " << message << "\n";
    return 1;
}

int Driver::run(const DriverOptions& options)
{
    if (options.showHelp) {
        out_ << driverUsage();
        return 0;
    }
    if (options.showVersion) {
        out_ << "bobc " << BOB_VERSION_STRING << "\n";
        return 0;
    }

    llvm::StringRef input(options.inputPath);
    if (input.ends_with(".bob")) {
        return fail("compiling bob source is not implemented yet; phase 0 accepts LLVM IR (.ll) only");
    }
    if (!input.ends_with(".ll")) {
        return fail("unsupported input '" + options.inputPath + "' (expected a .ll file)");
    }
    if (!llvm::sys::fs::exists(options.inputPath)) {
        return fail("input file '" + options.inputPath + "' not found");
    }

    std::string outputPath = defaultOutputPath(options);

    if (options.compileOnly) {
        if (std::optional<std::string> error = emitObjectFromIrFile(options.inputPath, outputPath)) {
            return fail(*error);
        }
        return 0;
    }

    Toolchain toolchain(options);
    ToolchainPaths paths;
    if (std::optional<std::string> error = toolchain.resolve(paths)) {
        return fail(*error);
    }
    if (paths.kernel32FromSdk && options.verbose) {
        err_ << "bobc: note: using Windows SDK " << paths.kernel32Library << "\n";
    }

    llvm::SmallString<260> objectPath;
    if (std::error_code ec = llvm::sys::fs::createTemporaryFile("bobc", "obj", objectPath)) {
        return fail("cannot create a temporary object file: " + ec.message());
    }
    std::string object = std::string(objectPath);

    int result = 0;
    if (std::optional<std::string> error = emitObjectFromIrFile(options.inputPath, object)) {
        result = fail(*error);
    }
    else {
        LinkJob job;
        job.objects.push_back(object);
        job.outputPath = outputPath;
        std::string commandLine;
        std::optional<std::string> linkError = runLink(job, paths, &commandLine);
        if (options.verbose) {
            err_ << commandLine << "\n";
        }
        if (linkError) {
            result = fail(*linkError);
        }
    }

    llvm::sys::fs::remove(object);
    return result;
}

}
