// bob_msgSend is only for reflection now: compiled code sends through direct
// calls, vtables and itables, so nothing else exercises the method tables and
// their selector-dropping thunks. This runs a compiled class in the JIT against
// the hosted runtime and sends to it the way `perform:` will.

#include "analyzer.h"
#include "bobRuntime.h"
#include "codegen/codeGenerator.h"
#include "hostedRuntime.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <memory>
#include <string>

TEST(Reflection, MsgSendReachesMethodsThroughTheirThunks)
{
    std::string source = R"(module reflect, 1.0.0
{
    interface {
        @[protocol] class Greeter {
            msg greet: n:int64 [int64];
            msg twiceGreet: n:int64 [int64] { return (self greet: n) * 2; }
        }
        @[reflect: invoke] class Doubler implements Greeter {
            public { base: int64 := 10; }
            msg twice: n:int64 [int64] { return base + n * 2; }
            msg greet: n:int64 [int64] { return n + 1; }
        }
    }
})";
    bob::DiagnosticEngine diagnostics;
    bob::parser::ParsedUnit parsed = bob::parser::parseSource(source, diagnostics);
    bob::sema::SemanticModel model;
    bob::sema::analyze(parsed.unit, model, diagnostics);
    ASSERT_EQ(diagnostics.errorCount(), 0u);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto jit = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jit)) << llvm::toString(jit.takeError());
    llvm::orc::SymbolMap symbols;
    for (const bob::hosted::Symbol& symbol : bob::hosted::runtimeSymbols()) {
        symbols[(*jit)->mangleAndIntern(symbol.name)] = {llvm::orc::ExecutorAddr::fromPtr(symbol.address), llvm::JITSymbolFlags::Exported};
    }
    ASSERT_FALSE(static_cast<bool>((*jit)->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)))));

    auto context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module =
        bob::codegen::generateModule(parsed.unit, model, *context, (*jit)->getDataLayout().getStringRepresentation());
    module->setTargetTriple((*jit)->getTargetTriple());
    ASSERT_FALSE(static_cast<bool>((*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))));

    auto lookup = [&](const char* name) -> void* {
        auto address = (*jit)->lookup(name);
        EXPECT_TRUE(static_cast<bool>(address)) << name << ": " << llvm::toString(address.takeError());
        return address ? address->toPtr<void*>() : nullptr;
    };
    auto create = reinterpret_cast<void* (*)()>(lookup("bob.reflect.Doubler.new"));
    const char* twice = static_cast<const char*>(lookup("bob.sel.twice:"));
    const char* twiceGreet = static_cast<const char*>(lookup("bob.sel.twiceGreet:"));
    ASSERT_NE(create, nullptr);
    ASSERT_NE(twice, nullptr);
    ASSERT_NE(twiceGreet, nullptr);

    void* object = create();
    auto send = reinterpret_cast<int64_t (*)(void*, const char*, int64_t)>(&bob_msgSend);
    EXPECT_EQ(send(object, twice, 5), 20);       // the class's own message
    EXPECT_EQ(send(object, twiceGreet, 4), 10);  // a protocol default adopted by the class
    bobRelease(object);
}
