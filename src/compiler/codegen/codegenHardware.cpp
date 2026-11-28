// I/O ports and processor instructions (design §6.7). Each is a fixed LLVM
// inline asm with its registers named exactly and a memory clobber, so the
// optimiser never moves memory accesses across it, never merges two of them
// and never drops one whose result is unused.

#include "codegenInternal.h"

#include <llvm/IR/InlineAsm.h>

namespace bob::codegen::detail {

namespace {

struct Instruction {
    const char* text;
    const char* constraints;
};

}

llvm::Value* Generator::emitInlineAsm(llvm::Type* result, const char* text, const char* constraints, const std::vector<llvm::Value*>& args)
{
    std::vector<llvm::Type*> params;
    for (llvm::Value* arg : args) {
        params.push_back(arg->getType());
    }
    llvm::FunctionType* type = llvm::FunctionType::get(result, params, false);
    llvm::InlineAsm* instruction = llvm::InlineAsm::get(type, text, constraints, true);
    return builder_.CreateCall(type, instruction, args);
}

llvm::Value* Generator::emitHardwareSend(const Node* node, const sema::SendInfo& send)
{
    const Node* receiver = nullptr;
    std::vector<const Node*> arguments;
    if (auto unary = nodeCast<UnarySend>(node)) {
        receiver = unary->receiver;
    }
    else if (auto keyword = nodeCast<KeywordSend>(node)) {
        receiver = keyword->receiver;
        arguments.assign(keyword->arguments.begin(), keyword->arguments.end());
    }
    llvm::Type* i16 = builder_.getInt16Ty();
    llvm::Type* i32 = builder_.getInt32Ty();
    llvm::Type* i64 = builder_.getInt64Ty();
    const Type* type = sema::canonical(send.operandType);
    // A port number is 0 to: 65535; a wider or signed value outside that traps.
    auto portNumber = [&](llvm::Value* wide) {
        trapIf(builder_.CreateICmpUGT(wide, builder_.getInt64(65535)), abi::TrapReason::valueOutOfRange);
        return builder_.CreateTrunc(wide, i16, "port");
    };
    auto combine = [&](llvm::Value* pair) {
        llvm::Value* low = builder_.CreateZExt(builder_.CreateExtractValue(pair, {0}), i64);
        llvm::Value* high = builder_.CreateZExt(builder_.CreateExtractValue(pair, {1}), i64);
        return builder_.CreateOr(builder_.CreateShl(high, 32), low);
    };
    auto argument = [&](size_t i, unsigned bits) { return emitAs(arguments[i], model_.types.integer(bits, false)); };

    switch (send.kind) {
    case SendKind::portFromInteger: {
        llvm::Value* value = emitAs(receiver, type);
        if (type->bits <= 16 && !type->isSigned) {
            return builder_.CreateZExtOrTrunc(value, i16, "port");
        }
        return portNumber(type->isSigned ? builder_.CreateSExt(value, i64) : builder_.CreateZExtOrTrunc(value, i64));
    }
    case SendKind::portCast:
    case SendKind::portToInteger:
        return emitAs(receiver, type);
    case SendKind::portAdvance: {
        llvm::Value* port = builder_.CreateZExt(emitAs(receiver, type), i64);
        return portNumber(builder_.CreateAdd(port, emitAs(arguments[0], model_.types.int64())));
    }
    case SendKind::portIn: {
        static const Instruction ins[] = {{"inb %dx, %al", "={al},{dx},~{memory}"}, {"inw %dx, %ax", "={ax},{dx},~{memory}"},
                                          {"inl %dx, %eax", "={eax},{dx},~{memory}"}};
        const Instruction& in = ins[type->inner->bits == 8 ? 0 : type->inner->bits == 16 ? 1 : 2];
        return emitInlineAsm(builder_.getIntNTy(type->inner->bits), in.text, in.constraints, {emitAs(receiver, type)});
    }
    case SendKind::portOut: {
        static const Instruction outs[] = {{"outb %al, %dx", "{al},{dx},~{memory}"}, {"outw %ax, %dx", "{ax},{dx},~{memory}"},
                                           {"outl %eax, %dx", "{eax},{dx},~{memory}"}};
        const Instruction& out = outs[type->inner->bits == 8 ? 0 : type->inner->bits == 16 ? 1 : 2];
        llvm::Value* port = emitAs(receiver, type);
        emitInlineAsm(builder_.getVoidTy(), out.text, out.constraints, {emitAs(arguments[0], type->inner), port});
        return nullptr;
    }
    case SendKind::cpuInstruction:
        break;
    default:
        return nullptr;
    }

    const std::string& op = send.op;
    llvm::Type* none = builder_.getVoidTy();
    llvm::StructType* pair = llvm::StructType::get(context_, {i32, i32});
    if (op == "disableInterrupts" || op == "enableInterrupts" || op == "halt" || op == "pause") {
        const char* text = op == "disableInterrupts" ? "cli" : op == "enableInterrupts" ? "sti" : op == "halt" ? "hlt" : "pause";
        emitInlineAsm(none, text, "~{memory}", {});
        return nullptr;
    }
    if (op == "timestamp") {
        return combine(emitInlineAsm(pair, "rdtsc", "={eax},={edx},~{memory}", {}));
    }
    if (op == "cr0" || op == "cr2" || op == "cr3" || op == "cr4") {
        std::string text = "movq %" + op + ", $0";
        return emitInlineAsm(i64, text.c_str(), "=r,~{memory}", {});
    }
    if (op == "cr0:" || op == "cr3:" || op == "cr4:") {
        std::string text = "movq $0, %" + op.substr(0, 3);
        emitInlineAsm(none, text.c_str(), "r,~{memory}", {argument(0, 64)});
        return nullptr;
    }
    if (op == "readMsr:") {
        return combine(emitInlineAsm(pair, "rdmsr", "={eax},={edx},{ecx},~{memory}", {argument(0, 32)}));
    }
    if (op == "writeMsr:value:") {
        llvm::Value* msr = argument(0, 32);
        llvm::Value* value = argument(1, 64);
        llvm::Value* low = builder_.CreateTrunc(value, i32);
        llvm::Value* high = builder_.CreateTrunc(builder_.CreateLShr(value, 32), i32);
        emitInlineAsm(none, "wrmsr", "{ecx},{eax},{edx},~{memory}", {msr, low, high});
        return nullptr;
    }
    if (op == "invalidatePage:") {
        emitInlineAsm(none, "invlpg ($0)", "r,~{memory}", {argument(0, 64)});
        return nullptr;
    }
    if (op == "loadGdt:limit:" || op == "loadIdt:limit:") {
        // The 10-byte pseudo-descriptor: limit (uint16), then base (uint64), unaligned.
        llvm::Value* base = argument(0, 64);
        llvm::Value* limit = argument(1, 16);
        llvm::AllocaInst* descriptor = entryAlloca(llvm::ArrayType::get(builder_.getInt8Ty(), 10), "descriptor");
        builder_.CreateAlignedStore(limit, descriptor, llvm::Align(1));
        builder_.CreateAlignedStore(base, builder_.CreateConstGEP1_32(builder_.getInt8Ty(), descriptor, 2), llvm::Align(1));
        emitInlineAsm(none, op == "loadGdt:limit:" ? "lgdt ($0)" : "lidt ($0)", "r,~{memory}", {descriptor});
        return nullptr;
    }
    if (op == "cpuid:" || op == "cpuid:subleaf:") {
        llvm::Value* leaf = argument(0, 32);
        llvm::Value* subleaf = arguments.size() > 1 ? argument(1, 32) : static_cast<llvm::Value*>(builder_.getInt32(0));
        llvm::StructType* four = llvm::StructType::get(context_, {i32, i32, i32, i32});
        llvm::Value* registers = emitInlineAsm(four, "cpuid", "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}", {leaf, subleaf});
        llvm::Value* result = llvm::UndefValue::get(llvmType(send.resultType));
        for (unsigned i = 0; i < 4; ++i) {
            result = builder_.CreateInsertValue(result, builder_.CreateExtractValue(registers, {i}), {i});
        }
        return result;
    }
    return nullptr;
}

}
