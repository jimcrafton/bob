#include "driver/objectEmitter.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>

namespace bob::driver {

namespace {

void initializeTargets()
{
    static std::once_flag once;
    std::call_once(once, []() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
}

llvm::CodeGenOptLevel codeGenLevel(int optLevel)
{
    switch (optLevel) {
    case 0:
        return llvm::CodeGenOptLevel::None;
    case 1:
        return llvm::CodeGenOptLevel::Less;
    case 3:
        return llvm::CodeGenOptLevel::Aggressive;
    default:
        return llvm::CodeGenOptLevel::Default;
    }
}

std::unique_ptr<llvm::TargetMachine> createTargetMachine(int optLevel, std::string& error)
{
    initializeTargets();
    llvm::Triple triple(windowsTargetTriple);
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (target == nullptr) {
        error = "no LLVM target for " + triple.str() + ": " + error;
        return nullptr;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "x86-64", "", options, llvm::Reloc::PIC_, std::nullopt, codeGenLevel(optLevel)));
    if (!machine) {
        error = "could not create an x86-64 target machine";
    }
    return machine;
}

void optimize(llvm::Module& module, llvm::TargetMachine& machine, int optLevel)
{
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder builder(&machine);
    builder.registerModuleAnalyses(modules);
    builder.registerCGSCCAnalyses(cgscc);
    builder.registerFunctionAnalyses(functions);
    builder.registerLoopAnalyses(loops);
    builder.crossRegisterProxies(loops, functions, cgscc, modules);
    llvm::OptimizationLevel level = optLevel == 1 ? llvm::OptimizationLevel::O1
                                    : optLevel == 3 ? llvm::OptimizationLevel::O3
                                                    : llvm::OptimizationLevel::O2;
    llvm::ModulePassManager passes = builder.buildPerModuleDefaultPipeline(level);
    passes.run(module, modules);
}

}

std::string targetDataLayout(std::string& error)
{
    std::unique_ptr<llvm::TargetMachine> machine = createTargetMachine(0, error);
    if (!machine) {
        return {};
    }
    return machine->createDataLayout().getStringRepresentation();
}

std::optional<std::string> prepareModule(llvm::Module& module)
{
    std::string error;
    std::unique_ptr<llvm::TargetMachine> machine = createTargetMachine(0, error);
    if (!machine) {
        return error;
    }
    module.setTargetTriple(llvm::Triple(windowsTargetTriple));
    module.setDataLayout(machine->createDataLayout());
    return std::nullopt;
}

std::optional<std::string> emitObjectFile(llvm::Module& module, const std::string& objectPath, int optLevel)
{
    std::string error;
    std::unique_ptr<llvm::TargetMachine> machine = createTargetMachine(optLevel, error);
    if (!machine) {
        return error;
    }
    module.setTargetTriple(llvm::Triple(windowsTargetTriple));
    module.setDataLayout(machine->createDataLayout());

    std::string verifyMessage;
    llvm::raw_string_ostream verifyStream(verifyMessage);
    if (llvm::verifyModule(module, &verifyStream)) {
        return "invalid IR: " + verifyMessage;
    }
    if (optLevel > 0) {
        optimize(module, *machine, optLevel);
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(objectPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        return "cannot open '" + objectPath + "': " + ec.message();
    }
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return std::string("the x86-64 target cannot emit object files");
    }
    passes.run(module);
    out.flush();
    return std::nullopt;
}

std::optional<std::string> emitObjectFromIrFile(const std::string& irPath, const std::string& objectPath, int optLevel)
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module = llvm::parseIRFile(irPath, diagnostic, context);
    if (!module) {
        std::string message;
        llvm::raw_string_ostream stream(message);
        diagnostic.print("bobc", stream, false);
        return message;
    }
    if (std::optional<std::string> error = emitObjectFile(*module, objectPath, optLevel)) {
        return irPath + ": " + *error;
    }
    return std::nullopt;
}

}
