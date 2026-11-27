#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bob::driver {

struct ToolchainPaths;

struct LinkJob {
    std::vector<std::string> objects;
    std::string outputPath;

    std::vector<std::string> buildArguments(const ToolchainPaths& paths) const;
};

std::optional<std::string> runLink(const LinkJob& job, const ToolchainPaths& paths, std::string* commandLine);

}
