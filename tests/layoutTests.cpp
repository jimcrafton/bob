// The semantic library computes layouts itself (no LLVM); `meta size`, offsets
// and compile-time value reading depend on it matching the code generator.

#include "analyzer.h"
#include "codegen/codeGenerator.h"
#include "layout.h"
#include "parser/parser.h"

#include <gtest/gtest.h>

#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <memory>
#include <string>

namespace {

std::string hostDataLayout()
{
    llvm::InitializeNativeTarget();
    std::string error;
    std::string triple = llvm::sys::getDefaultTargetTriple();
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (target == nullptr) {
        return "";
    }
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(llvm::Triple(triple), "generic", "", {}, std::nullopt));
    return machine->createDataLayout().getStringRepresentation();
}

}

TEST(Layout, MatchesTheCodeGenerator)
{
    std::string source = R"(program layout, 1.0.0
{
    enum Small : uint8 { a; b; }
    enum Wide : int64 { x; y; }
    enum Perm { read; write; exec; }
    record Mixed { a: uint8; b: int64; c: uint16; d: bool; }
    @[packed] record Packed { a: uint8; b: int64; c: uint16; }
    record Nested { m: Mixed; p: Packed; e: Small; r: real32; }
    record Holder { maybe: int32?; flags: set of: Perm; grid: array of: uint16 by: [0 to: 6]; byEnum: array of: Mixed by: Small; }
    class Base { public { a: uint8; b: int64; } }
    class Derived inherits Base { public { c: uint16; s: string; n: Nested; } }
    extends layout {
        @[static] msg main: args:array [int32] {
            u := [1, "x", 2.5];
            o: Mixed? := nil;
            c: char := 'a';
            return 0;
        }
    }
})";
    bob::DiagnosticEngine diagnostics;
    bob::parser::ParsedUnit parsed = bob::parser::parseSource(source, diagnostics);
    bob::sema::SemanticModel model;
    bob::sema::analyze(parsed.unit, model, diagnostics);
    ASSERT_EQ(diagnostics.errorCount(), 0u);
    std::string dataLayout = hostDataLayout();
    ASSERT_FALSE(dataLayout.empty());

    std::vector<const bob::sema::Type*> types;
    for (const auto& record : model.records) {
        types.push_back(record->type);
        for (const auto& field : record->fields) {
            types.push_back(field.type);
        }
    }
    for (const auto& local : model.locals) {
        if (bob::sema::isValueType(local->type)) {
            types.push_back(local->type);
            if (bob::sema::isUnion(bob::sema::canonical(local->type)->inner)) {
                types.push_back(bob::sema::canonical(local->type)->inner);
            }
        }
    }
    ASSERT_GT(types.size(), 15u);
    for (const bob::sema::Type* type : types) {
        EXPECT_EQ(bob::sema::sizeOf(type), bob::codegen::llvmSizeOf(type, model, dataLayout)) << bob::sema::typeName(type);
    }
    // Objects: the instance size the class info records.
    const bob::sema::ClassInfo* derived = model.classNamed("Derived");
    EXPECT_EQ(bob::sema::objectSize(derived), 16u + 16 + 8 + 8 + bob::sema::sizeOf(derived->fields[4].type));
    EXPECT_EQ(bob::sema::fieldOffset(model.recordNamed("Mixed"), 2), 16u);
    EXPECT_EQ(bob::sema::fieldOffset(model.recordNamed("Packed"), 2), 9u);
}
