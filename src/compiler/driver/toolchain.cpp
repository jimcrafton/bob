#include "driver/toolchain.h"

#include "abi.h"
#include "driver/driverOptions.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>

#include <vector>

namespace bob::driver {

namespace {

std::string joinPath(const std::string& dir, const char* file)
{
    llvm::SmallString<260> path(dir);
    llvm::sys::path::append(path, file);
    return std::string(path);
}

bool isFile(const std::string& path)
{
    return !path.empty() && llvm::sys::fs::is_regular_file(path);
}

int compareVersionNames(const std::string& a, const std::string& b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() || j < b.size()) {
        unsigned long long va = 0;
        unsigned long long vb = 0;
        while (i < a.size() && a[i] != '.') {
            va = va * 10 + static_cast<unsigned long long>(a[i] - '0');
            ++i;
        }
        while (j < b.size() && b[j] != '.') {
            vb = vb * 10 + static_cast<unsigned long long>(b[j] - '0');
            ++j;
        }
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    return 0;
}

bool isVersionName(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!(c == '.' || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

}

Toolchain::Toolchain(const DriverOptions& options) : options_(options)
{
}

std::optional<std::string> Toolchain::findLinker() const
{
    if (!options_.linkerPath.empty()) {
        if (isFile(options_.linkerPath)) {
            return options_.linkerPath;
        }
        return std::nullopt;
    }
#ifdef BOB_DEFAULT_LINKER
    if (isFile(BOB_DEFAULT_LINKER)) {
        return std::string(BOB_DEFAULT_LINKER);
    }
#endif
    llvm::ErrorOr<std::string> found = llvm::sys::findProgramByName("lld-link");
    if (found) {
        return *found;
    }
    std::string programFiles = "C:/Program Files/LLVM/bin/lld-link.exe";
    if (isFile(programFiles)) {
        return programFiles;
    }
    return std::nullopt;
}

std::optional<std::string> Toolchain::findSdkKernel32(const std::string& sdkLibPath)
{
    if (!sdkLibPath.empty()) {
        std::string candidate = joinPath(sdkLibPath, abi::kernel32LibraryName);
        if (isFile(candidate)) {
            return candidate;
        }
    }

    const std::string kitsLib = "C:/Program Files (x86)/Windows Kits/10/Lib";
    std::error_code ec;
    std::string bestVersion;
    for (llvm::sys::fs::directory_iterator it(kitsLib, ec), end; it != end && !ec; it.increment(ec)) {
        std::string name = std::string(llvm::sys::path::filename(it->path()));
        if (!isVersionName(name)) {
            continue;
        }
        std::string candidate = kitsLib + "/" + name + "/um/x64/kernel32.lib";
        if (isFile(candidate) && (bestVersion.empty() || compareVersionNames(name, bestVersion) > 0)) {
            bestVersion = name;
        }
    }
    if (bestVersion.empty()) {
        return std::nullopt;
    }
    return kitsLib + "/" + bestVersion + "/um/x64/kernel32.lib";
}

std::optional<std::string> Toolchain::resolve(ToolchainPaths& paths) const
{
    std::optional<std::string> linker = findLinker();
    if (!linker) {
        if (!options_.linkerPath.empty()) {
            return "linker not found at '" + options_.linkerPath + "'";
        }
        return std::string("lld-link not found; install LLVM or pass --linker=<path>");
    }
    paths.linker = *linker;

    paths.runtimeLibrary = joinPath(options_.runtimeDir, abi::runtimeLibraryName);
    if (!isFile(paths.runtimeLibrary)) {
        return "runtime library not found at '" + paths.runtimeLibrary + "' (use --runtime-dir=<dir>)";
    }

    std::string generated = joinPath(options_.runtimeDir, abi::kernel32LibraryName);
    if (isFile(generated)) {
        paths.kernel32Library = generated;
        paths.kernel32FromSdk = false;
        return std::nullopt;
    }

    std::optional<std::string> sdk = findSdkKernel32(options_.sdkLibPath);
    if (!sdk) {
        return "kernel32.lib not found in '" + options_.runtimeDir + "' and no Windows SDK fallback was found (use --sdk-lib-path=<dir>)";
    }
    paths.kernel32Library = *sdk;
    paths.kernel32FromSdk = true;
    return std::nullopt;
}

}
