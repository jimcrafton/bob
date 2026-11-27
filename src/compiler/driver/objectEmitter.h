#pragma once

#include <optional>
#include <string>

namespace bob::driver {

inline constexpr const char* windowsTargetTriple = "x86_64-pc-windows-msvc";

std::optional<std::string> emitObjectFromIrFile(const std::string& irPath, const std::string& objectPath);

}
