#pragma once

#include "semanticModel.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
}

namespace bob::codegen {

// Lowers an analyzed unit (no semantic errors) to an LLVM module. The module
// defines `bobMain`, which the runtime's entry point calls. `dataLayout` is the
// target's layout string (object sizes for class infos depend on it).
std::unique_ptr<llvm::Module> generateModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                             const std::string& dataLayout);

}
