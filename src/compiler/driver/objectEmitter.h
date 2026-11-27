#pragma once

#include <optional>
#include <string>

namespace llvm {
class Module;
}

namespace bob::driver {

inline constexpr const char* windowsTargetTriple = "x86_64-pc-windows-msvc";

// Sets the target triple and data layout, verifies the module, runs the
// optimization pipeline for `optLevel` (0-3) and writes a COFF object.
std::optional<std::string> emitObjectFile(llvm::Module& module, const std::string& objectPath, int optLevel);

std::optional<std::string> emitObjectFromIrFile(const std::string& irPath, const std::string& objectPath, int optLevel);

// Sets triple and data layout only (for --dump-llvm).
std::optional<std::string> prepareModule(llvm::Module& module);

}
