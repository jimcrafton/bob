#pragma once

#include <optional>
#include <string>

namespace bob::driver {

struct DriverOptions;

struct ToolchainPaths {
    std::string linker;
    std::string runtimeLibrary;
    std::string kernel32Library;
    bool kernel32FromSdk = false;
};

class Toolchain {
public:
    explicit Toolchain(const DriverOptions& options);

    std::optional<std::string> resolve(ToolchainPaths& paths) const;

    static std::optional<std::string> findSdkKernel32(const std::string& sdkLibPath);

private:
    std::optional<std::string> findLinker() const;

    const DriverOptions& options_;
};

}
