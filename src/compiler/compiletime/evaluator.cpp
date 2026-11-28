#include "compiletime/evaluator.h"

#include "abi.h"
#include "bobRuntime.h"
#include "codegen/codeGenerator.h"
#include "hostedRuntime.h"
#include "layout.h"
#include "objectModel.h"

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <cstring>
#include <map>
#include <set>

namespace bob::compiletime {

namespace {

using sema::CompileTimeItem;
using sema::ConstValue;
using sema::Type;
using sema::TypeKind;

// Compile-time code that runs longer than this is stopped (an endless loop).
constexpr uint32_t timeoutMilliseconds = 20000;

const char* trapText(int32_t reason)
{
    switch (static_cast<abi::TrapReason>(reason)) {
    case abi::TrapReason::integerOverflow:
        return "integer overflow";
    case abi::TrapReason::divideByZero:
        return "division by zero";
    case abi::TrapReason::conversionOutOfRange:
        return "conversion out of range";
    case abi::TrapReason::shiftOutOfRange:
        return "shift amount out of range";
    case abi::TrapReason::valueOutOfRange:
        return "value out of range";
    case abi::TrapReason::ordinalOutOfRange:
        return "no next or previous member";
    case abi::TrapReason::indexOutOfRange:
        return "index out of range";
    case abi::TrapReason::keyNotFound:
        return "key not found";
    case abi::TrapReason::outOfMemory:
        return "out of memory";
    case abi::TrapReason::doesNotUnderstand:
        return "message not understood";
    default:
        return "runtime trap";
    }
}

// Reads a value of `type` that generated code stored at `address` (host memory,
// laid out as on the target: both are x64 Windows).
ConstValue readValue(const uint8_t* address, const Type* type)
{
    type = sema::canonical(type);
    ConstValue value;
    value.type = type;
    switch (type->kind) {
    case TypeKind::boolType:
        value.bits = *address != 0 ? 1 : 0;
        break;
    case TypeKind::integer:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
        std::memcpy(&value.bits, address, sema::sizeOf(type));
        break;
    case TypeKind::real:
        if (type->bits == 32) {
            float f = 0;
            std::memcpy(&f, address, 4);
            value.real = f;
        }
        else {
            std::memcpy(&value.real, address, 8);
        }
        break;
    case TypeKind::string: {
        const BobString* s = *reinterpret_cast<BobString* const*>(address);
        if (s != nullptr) {
            value.text.assign(reinterpret_cast<const char*>(s->bytes), static_cast<size_t>(s->byteCount));
        }
        break;
    }
    case TypeKind::record:
        for (size_t i = 0; i < type->record->fields.size(); ++i) {
            value.elements.push_back(readValue(address + sema::fieldOffset(type->record, i), type->record->fields[i].type));
        }
        break;
    case TypeKind::fixedArray:
        for (uint64_t i = 0; i < type->count; ++i) {
            value.elements.push_back(readValue(address + i * sema::sizeOf(type->inner), type->inner));
        }
        break;
    case TypeKind::optional:
        value.present = *address != 0;
        if (value.present) {
            uint64_t offset = std::max<uint64_t>(1, sema::alignmentOf(type->inner));
            value.elements.push_back(readValue(address + offset, type->inner));
        }
        break;
    case TypeKind::array: {
        const auto* array = *reinterpret_cast<BobArray* const*>(address);
        if (array != nullptr) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(array) + abi::arrayDataOffset;
            for (uint64_t i = 0; i < array->count; ++i) {
                value.elements.push_back(readValue(data + i * sema::sizeOf(type->inner), type->inner));
            }
        }
        break;
    }
    case TypeKind::set:
        if (sema::isBitSet(type)) {
            std::memcpy(&value.bits, address, sema::sizeOf(type));
            break;
        }
        [[fallthrough]];
    case TypeKind::dictionary:
    case TypeKind::hashtable: {
        void* table = *reinterpret_cast<void* const*>(address);
        bool isSet = type->kind == TypeKind::set;
        const Type* keyType = isSet ? type->inner : type->key;
        for (int64_t i = bobTableNext(table, 0); i >= 0; i = bobTableNext(table, i + 1)) {
            ConstValue key = readValue(static_cast<const uint8_t*>(bobTableKeyAt(table, i)), keyType);
            if (isSet) {
                value.elements.push_back(key);
            }
            else {
                value.entries.push_back({key, readValue(static_cast<const uint8_t*>(bobTableValueAt(table, i)), type->inner)});
            }
        }
        break;
    }
    default:
        break;
    }
    return value;
}

class Evaluator {
public:
    Evaluator(const ast::Unit* unit, sema::SemanticModel& model, DiagnosticEngine& diagnostics)
        : unit_(unit), model_(model), diagnostics_(diagnostics)
    {
    }

    bool run()
    {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        for (const auto& item : model_.compileTimeItems) {
            if (!evaluate(item.get())) {
                return false;
            }
        }
        return true;
    }

private:
    // Constants and nested compile-time expressions that the item's code can
    // reach: its own references, and those of every message it may call and
    // every class it may construct (vtable and protocol sends: every
    // implementation of the selector).
    std::vector<CompileTimeItem*> dependencies(const CompileTimeItem* item)
    {
        std::set<const sema::MessageInfo*> seen;
        std::set<const sema::ClassInfo*> classes;
        std::vector<const sema::MessageInfo*> pending{item->context};
        std::vector<CompileTimeItem*> result;
        auto addClass = [&](const sema::ClassInfo* info) {
            for (const sema::ClassInfo* c = info; c != nullptr; c = c->superclass) {
                if (!classes.insert(c).second) {
                    continue;
                }
                pending.push_back(c->initializer);
                pending.push_back(c->initMessage);
                for (const sema::FieldInfo& field : c->fields) {
                    if (field.constructsDefault && field.type->classInfo != nullptr) {
                        pending.push_back(field.type->classInfo->initializer);
                        pending.push_back(field.type->classInfo->initMessage);
                    }
                }
            }
        };
        while (!pending.empty()) {
            const sema::MessageInfo* message = pending.back();
            pending.pop_back();
            if (message == nullptr || !seen.insert(message).second) {
                continue;
            }
            for (sema::ConstantInfo* constant : message->constantsUsed) {
                result.push_back(constant->item);
            }
            for (CompileTimeItem* nested : message->itemsUsed) {
                if (nested != item) {
                    result.push_back(nested);
                }
            }
            for (const sema::MessageInfo* target : message->calls) {
                pending.push_back(target);
                if (target->isInstance) {
                    for (const auto& other : model_.messages) {
                        if (other->isInstance && other->selector == target->selector) {
                            pending.push_back(other.get());
                        }
                    }
                }
            }
            for (const sema::ClassInfo* info : message->constructs) {
                addClass(info);
            }
        }
        return result;
    }

    std::string describe(const CompileTimeItem* item) const
    {
        for (const auto& constant : model_.constants) {
            if (constant->item == item) {
                return "constant '" + constant->name + "'";
            }
        }
        return item->kind == CompileTimeItem::Kind::block ? "this compile-time block" : "this compile-time expression";
    }

    bool evaluate(CompileTimeItem* item)
    {
        if (item->evaluated) {
            return true;
        }
        int& state = state_[item];
        if (state == 1) {
            diagnostics_.error(item->node->range, "compiletime.cycle", describe(item) + " needs its own value to be computed");
            return false;
        }
        if (state == 3) {
            return false;
        }
        state = 1;
        for (CompileTimeItem* dependency : dependencies(item)) {
            if (!evaluate(dependency)) {
                state = 3;
                return false;
            }
        }
        bool ok = execute(item);
        state = ok ? 2 : 3;
        item->evaluated = ok;
        return ok;
    }

    bool fail(const CompileTimeItem* item, const std::string& message)
    {
        diagnostics_.error(item->node->range, "compiletime.failed", "couldn't run " + describe(item) + ": " + message);
        return false;
    }

    bool execute(CompileTimeItem* item)
    {
        auto jit = llvm::orc::LLJITBuilder().create();
        if (!jit) {
            return fail(item, llvm::toString(jit.takeError()));
        }
        llvm::orc::SymbolMap symbols;
        for (const hosted::Symbol& symbol : hosted::runtimeSymbols()) {
            symbols[(*jit)->mangleAndIntern(symbol.name)] = {llvm::orc::ExecutorAddr::fromPtr(symbol.address), llvm::JITSymbolFlags::Exported};
        }
        if (llvm::Error error = (*jit)->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
            return fail(item, llvm::toString(std::move(error)));
        }
        // Compile-time code may call into imported modules: load their code too.
        for (const auto& module : model_.modules) {
            if (module->libraryPath.empty()) {
                continue;
            }
            if (llvm::StringRef(module->libraryPath).ends_with(".lib")) {
                auto generator = llvm::orc::StaticLibraryDefinitionGenerator::Load((*jit)->getObjLinkingLayer(), module->libraryPath.c_str());
                if (!generator) {
                    return fail(item, llvm::toString(generator.takeError()));
                }
                (*jit)->getMainJITDylib().addGenerator(std::move(*generator));
            }
            else if (auto buffer = llvm::MemoryBuffer::getFile(module->libraryPath)) {
                if (llvm::Error error = (*jit)->addObjectFile(std::move(*buffer))) {
                    return fail(item, llvm::toString(std::move(error)));
                }
            }
        }

        const std::string thunk = "bob.compiletime.thunk";
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module =
            codegen::generateEvaluationModule(unit_, model_, *context, (*jit)->getDataLayout().getStringRepresentation(),
                                              (*jit)->getTargetTriple().str(), item, thunk);
        std::string problems;
        llvm::raw_string_ostream stream(problems);
        if (llvm::verifyModule(*module, &stream)) {
            return fail(item, "internal error, invalid code was generated:\n" + problems);
        }
        if (llvm::Error error = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
            return fail(item, llvm::toString(std::move(error)));
        }
        auto address = (*jit)->lookup(thunk);
        if (!address) {
            return fail(item, llvm::toString(address.takeError()));
        }
        auto entry = address->toPtr<void (*)(void*)>();

        uint64_t size = item->type != nullptr && item->type->kind != TypeKind::voidType ? sema::sizeOf(item->type) : 8;
        std::vector<uint64_t> out((size + 7) / 8 + 1, 0);
        hosted::RunResult result = hosted::run(entry, out.data(), timeoutMilliseconds);

        bool reportedError = false;
        for (const hosted::Diagnostic& diagnostic : result.diagnostics) {
            auto node = static_cast<const ast::Node*>(diagnostic.node);
            SourceRange range = node != nullptr ? node->range : item->node->range;
            if (diagnostic.kind == 0) {
                diagnostics_.error(range, "compiletime.error", diagnostic.text);
                reportedError = true;
            }
            else {
                diagnostics_.report(diagnostic.kind == 1 ? Severity::warning : Severity::note, range,
                                    diagnostic.kind == 1 ? "compiletime.warning" : "compiletime.note", diagnostic.text);
            }
        }
        if (result.timedOut) {
            return fail(item, "it ran for more than " + std::to_string(timeoutMilliseconds / 1000) + " seconds (an endless loop?)");
        }
        if (!result.completed) {
            std::string detail = result.output.empty() ? "" : " (" + result.output.substr(0, result.output.find('\n')) + ")";
            return fail(item, std::string("it trapped: ") + trapText(result.trapReason) + detail);
        }
        if (reportedError) {
            return false;
        }
        if (item->kind != CompileTimeItem::Kind::block) {
            model_.constValues.push_back(std::make_unique<ConstValue>(readValue(reinterpret_cast<const uint8_t*>(out.data()), item->type)));
            item->value = model_.constValues.back().get();
        }
        return true;
    }

    const ast::Unit* unit_;
    sema::SemanticModel& model_;
    DiagnosticEngine& diagnostics_;
    std::map<const CompileTimeItem*, int> state_;
};

}

bool evaluate(const ast::Unit* unit, sema::SemanticModel& model, DiagnosticEngine& diagnostics)
{
    if (model.compileTimeItems.empty()) {
        return true;
    }
    Evaluator evaluator(unit, model, diagnostics);
    return evaluator.run();
}

}
