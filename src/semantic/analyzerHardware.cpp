// I/O ports and processor instructions (design §6.7): `port of: T` and `Cpu`.
// Port accesses and privileged instructions need an unsafe region; `Cpu pause`,
// `timestamp` and `cpuid:` work in any code, in user mode too.

#include "analyzerInternal.h"

namespace bob::sema::detail {

namespace {

const char* const hardwareWhy = "talks to hardware directly";
const char* const privilegedWhy = "runs a privileged processor instruction";

bool isPortData(const Type* type)
{
    type = canonical(type);
    return isInteger(type) && !type->isSigned && type->bits <= 32 && type->distinctOf == nullptr;
}

// Argument codes: 'w' uint16, 'd' uint32, 'q' uint64. Results: 'v' none, 'q' uint64, 'c' CpuidResult.
struct CpuMessage {
    const char* selector;
    bool privileged;
    const char* arguments;
    char result;
};

const CpuMessage cpuMessages[] = {
    {"disableInterrupts", true, "", 'v'},
    {"enableInterrupts", true, "", 'v'},
    {"halt", true, "", 'v'},
    {"pause", false, "", 'v'},
    {"timestamp", false, "", 'q'},
    {"cr0", true, "", 'q'},
    {"cr2", true, "", 'q'},
    {"cr3", true, "", 'q'},
    {"cr4", true, "", 'q'},
    {"cr0:", true, "q", 'v'},
    {"cr3:", true, "q", 'v'},
    {"cr4:", true, "q", 'v'},
    {"readMsr:", true, "d", 'q'},
    {"writeMsr:value:", true, "dq", 'v'},
    {"invalidatePage:", true, "q", 'v'},
    {"loadGdt:limit:", true, "qw", 'v'},
    {"loadIdt:limit:", true, "qw", 'v'},
    {"cpuid:", false, "d", 'c'},
    {"cpuid:subleaf:", false, "dd", 'c'},
};

}

const Type* Analyzer::cpuidResultType()
{
    if (model_.cpuidRecord == nullptr) {
        auto record = std::make_unique<RecordInfo>();
        record->name = "CpuidResult";
        record->type = types_.addRecord(record.get());
        for (const char* name : {"eax", "ebx", "ecx", "edx"}) {
            FieldInfo field;
            field.name = name;
            field.type = types_.integer(32, false);
            field.index = static_cast<uint32_t>(record->fields.size());
            record->fields.push_back(field);
        }
        record->resolved = true;
        model_.cpuidRecord = std::move(record);
    }
    return model_.cpuidRecord->type;
}

const Type* Analyzer::checkCpuSend(const Node* send, const std::string& selector, const std::vector<const Node*>& arguments, SourceRange selectorRange)
{
    const CpuMessage* message = nullptr;
    for (const CpuMessage& candidate : cpuMessages) {
        if (selector == candidate.selector) {
            message = &candidate;
            break;
        }
    }
    if (message == nullptr) {
        for (const Node* argument : arguments) {
            check(argument, nullptr);
        }
        std::string known;
        for (const CpuMessage& candidate : cpuMessages) {
            known += (known.empty() ? "" : ", ") + std::string(candidate.selector);
        }
        error(selectorRange, "sema.unknownMessage", "'Cpu' doesn't understand '" + selector + "'; it understands " + known);
        return types_.error();
    }
    for (size_t i = 0; i < arguments.size(); ++i) {
        char code = message->arguments[i];
        checkArgument(arguments[i], types_.integer(code == 'w' ? 16 : code == 'd' ? 32 : 64, false));
    }
    if (message->privileged && !requireUnsafe(send, "'Cpu " + selector + "'", privilegedWhy)) {
        return types_.error();
    }
    const Type* result = message->result == 'q' ? types_.integer(64, false) : message->result == 'c' ? cpuidResultType() : types_.voidType();
    addSend(send, SendKind::cpuInstruction, selector, nullptr, result);
    return result;
}

const Type* Analyzer::checkPortUnary(const UnarySend* send, const Type* receiver, const Type* expected)
{
    const std::string& selector = send->selector;
    if (receiver->kind == TypeKind::meta && receiver->instance == types_.cpu()) {
        return checkCpuSend(send, selector, {}, send->selectorRange);
    }
    const Type* type = canonical(receiver);
    if (selector == "asPort" && (isInteger(decay(type)) || isPort(type))) {
        const Type* target = expected != nullptr ? canonical(expected) : nullptr;
        if (!isPort(target)) {
            error(send->selectorRange, "sema.portNeedsType",
                  "'asPort' takes its port type from context; declare it, as in 'com1: port of: uint8 := 0x3F8 asPort', or write 'asPort: T'");
            return types_.error();
        }
        if (!requireUnsafe(send, "'asPort'", hardwareWhy)) {
            return types_.error();
        }
        addSend(send, isPort(type) ? SendKind::portCast : SendKind::portFromInteger, selector, isPort(type) ? type : decay(type), target);
        return target;
    }
    if (!isPort(type)) {
        return nullptr;
    }
    if (selector == "asUInt16") {
        addSend(send, SendKind::portToInteger, selector, type, types_.integer(16, false));
        return types_.integer(16, false);
    }
    if (selector == "contents") {
        if (!requireUnsafe(send, "'contents'", hardwareWhy)) {
            return types_.error();
        }
        addSend(send, SendKind::portIn, selector, type, type->inner);
        return type->inner;
    }
    error(send->selectorRange, "sema.unknownMessage",
          "'" + typeName(type) + "' doesn't understand '" + selector + "'; a port understands contents, contents:, advancedBy:, asPort, asPort: and asUInt16");
    return types_.error();
}

const Type* Analyzer::checkPortKeyword(const KeywordSend* send, const Type* receiver)
{
    const std::string& selector = send->selector;
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    if (receiver->kind == TypeKind::meta && receiver->instance == types_.cpu()) {
        return checkCpuSend(send, selector, std::vector<const Node*>(send->arguments.begin(), send->arguments.end()), selectorRange);
    }
    const Type* type = canonical(receiver);
    if (selector == "asPort:" && (isInteger(decay(type)) || isPort(type))) {
        const Type* meta = check(send->arguments[0], nullptr);
        if (meta == types_.error()) {
            return types_.error();
        }
        if (meta->kind != TypeKind::meta || meta->instance == nullptr || !isPortData(meta->instance)) {
            error(send->arguments[0], "sema.invalidPortType", "'asPort:' takes the width a port moves: uint8, uint16 or uint32");
            return types_.error();
        }
        if (!requireUnsafe(send, "'asPort:'", hardwareWhy)) {
            return types_.error();
        }
        const Type* result = types_.portOf(canonical(meta->instance));
        addSend(send, isPort(type) ? SendKind::portCast : SendKind::portFromInteger, selector, isPort(type) ? type : decay(type), result);
        return result;
    }
    if (!isPort(type)) {
        return nullptr;
    }
    if (selector == "contents:") {
        checkArgument(send->arguments[0], type->inner);
        if (!requireUnsafe(send, "'contents:'", hardwareWhy)) {
            return types_.error();
        }
        addSend(send, SendKind::portOut, selector, type, types_.voidType());
        return types_.voidType();
    }
    if (selector == "advancedBy:") {
        checkArgument(send->arguments[0], types_.int64());
        if (!requireUnsafe(send, "'advancedBy:'", hardwareWhy)) {
            return types_.error();
        }
        addSend(send, SendKind::portAdvance, selector, type, type);
        return type;
    }
    for (const Node* argument : send->arguments) {
        check(argument, nullptr);
    }
    error(selectorRange, "sema.unknownMessage",
          "'" + typeName(type) + "' doesn't understand '" + selector + "'; a port understands contents, contents:, advancedBy:, asPort, asPort: and asUInt16");
    return types_.error();
}

}
