#include "driver/driver.h"
#include "driver/driverOptions.h"

#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

int main(int argc, char** argv)
{
    llvm::InitLLVM initLlvm(argc, argv);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    bob::driver::DriverOptions options;
    if (std::optional<std::string> error = bob::driver::parseDriverArguments(args, options)) {
        llvm::errs() << "bobc: error: " << *error << "\n\n" << bob::driver::driverUsage();
        return 1;
    }

    if (options.runtimeDir.empty()) {
        std::string exe = llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void*>(&main));
        options.runtimeDir = std::string(llvm::sys::path::parent_path(exe));
    }

    bob::driver::Driver driver(llvm::outs(), llvm::errs());
    return driver.run(options);
}
