#include "driver/objectEmitter.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
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

}

std::optional<std::string> emitObjectFromIrFile(const std::string& irPath, const std::string& objectPath)
{
    initializeTargets();

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module = llvm::parseIRFile(irPath, diagnostic, context);
    if (!module) {
        std::string message;
        llvm::raw_string_ostream stream(message);
        diagnostic.print("bobc", stream, false);
        return message;
    }

    std::string verifyMessage;
    llvm::raw_string_ostream verifyStream(verifyMessage);
    if (llvm::verifyModule(*module, &verifyStream)) {
        return irPath + ": invalid IR: " + verifyMessage;
    }

    llvm::Triple triple(windowsTargetTriple);
    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupError);
    if (target == nullptr) {
        return "no LLVM target for " + triple.str() + ": " + lookupError;
    }

    llvm::TargetOptions targetOptions;
    std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, "x86-64", "", targetOptions, llvm::Reloc::PIC_));
    if (!machine) {
        return std::string("could not create an x86-64 target machine");
    }

    module->setTargetTriple(triple);
    module->setDataLayout(machine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream out(objectPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        return "cannot open '" + objectPath + "': " + ec.message();
    }

    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return std::string("the x86-64 target cannot emit object files");
    }
    passes.run(*module);
    out.flush();
    return std::nullopt;
}

}
