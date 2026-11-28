#include "explainDispatch.h"

#include <algorithm>
#include <vector>

namespace bob::sema {

namespace {

using namespace bob::ast;

const char* overflowName(OverflowMode mode)
{
    switch (mode) {
    case OverflowMode::trap:
        return "trap";
    case OverflowMode::wrap:
        return "wrap";
    case OverflowMode::saturate:
        return "saturate";
    }
    return "trap";
}

uint32_t selectorOffset(const Node* node)
{
    if (auto unary = nodeCast<UnarySend>(node)) {
        return unary->selectorRange.begin;
    }
    if (auto binary = nodeCast<BinarySend>(node)) {
        return binary->selectorRange.begin;
    }
    if (auto keyword = nodeCast<KeywordSend>(node)) {
        return keyword->partRanges.empty() ? node->range.begin : keyword->partRanges.front().begin;
    }
    return node->range.begin;
}

std::string selectorOf(const Node* node)
{
    if (auto unary = nodeCast<UnarySend>(node)) {
        return unary->selector;
    }
    if (auto binary = nodeCast<BinarySend>(node)) {
        return binary->selector;
    }
    if (auto keyword = nodeCast<KeywordSend>(node)) {
        return keyword->selector;
    }
    return "?";
}

std::string describe(const SendInfo& send, const SemanticModel& model)
{
    std::string type = send.operandType != nullptr ? send.operandType->name : "";
    switch (send.kind) {
    case SendKind::arithmetic:
    case SendKind::shift:
    case SendKind::negate:
    case SendKind::absolute:
        return "tier 0, intrinsic " + type + " (overflow: " + overflowName(send.overflow) + ")";
    case SendKind::wrapArithmetic:
        return "tier 0, intrinsic " + type + " (always wraps)";
    case SendKind::saturateArithmetic:
        return "tier 0, intrinsic " + type + " (always saturates)";
    case SendKind::bitwise:
    case SendKind::comparison:
    case SendKind::minimum:
    case SendKind::maximum:
    case SendKind::boolNot:
        return "tier 0, intrinsic " + type;
    case SendKind::logicalAnd:
    case SendKind::logicalOr:
        return "tier 0, intrinsic short-circuit branch";
    case SendKind::conversion:
        return "tier 0, intrinsic checked conversion " + type + " -> " + send.resultType->name;
    case SendKind::staticCall:
        return "tier 1, direct call to '" + model.program.name + " " + send.target->selector + "'";
    case SendKind::recordConstructor:
        return "tier 1, record construction (no call)";
    case SendKind::blockInvoke:
        return "block call, indirect through the block's function pointer";
    case SendKind::inlineIf:
    case SendKind::inlineToDo:
    case SendKind::inlineTimesRepeat:
        return "tier 0, inlined control flow (no closure created)";
    case SendKind::inlineIfNil:
        return "tier 0, inlined nil test (no closure created)";
    case SendKind::referenceCompare:
        return "tier 0, intrinsic reference identity";
    case SendKind::newObject:
        return "allocation of a '" + send.classInfo->name + "' (runs its field initializers)";
    case SendKind::constructObject:
        return "construction of a '" + send.classInfo->name + "' (" + std::to_string(send.fields.size()) +
               " field(s) passed, then the other fields' initializers and each class's init)";
    case SendKind::classStaticCall:
        return "tier 1, direct call to class-side '" + send.target->owner->name + " " + send.target->selector + "'";
    case SendKind::directMethodCall:
        return "tier 1, direct call to '" + send.target->owner->name + ">>" + send.target->selector +
               "' (the only implementation in the hierarchy below '" + send.classInfo->name + "')";
    case SendKind::superCall:
        return "tier 1, direct call to '" + send.target->owner->name + ">>" + send.target->selector + "' (super)";
    case SendKind::vtableCall:
        return "tier 1v, vtable slot " + std::to_string(send.slot) + " of '" + send.classInfo->name + "' (overridden below it)";
    case SendKind::dynamicSend:
        return "tier 2, itable '" + send.classInfo->name + "' slot " + std::to_string(send.slot) + " (receiver is a protocol)";
    case SendKind::fieldGet:
        return "tier 0, inlined field read";
    case SendKind::fieldSet:
        return "tier 0, inlined field write";
    case SendKind::interval:
    case SendKind::enumRange:
    case SendKind::collectionRange:
        return "";
    case SendKind::constant:
        return "tier 0, compile-time constant";
    case SendKind::constantValue:
        return "tier 0, compile-time constant (embedded, no code runs)";
    case SendKind::metaInfo:
        return "";
    case SendKind::metaFields:
        return "tier 0, an array of FieldMeta built from compile-time constants";
    case SendKind::compilerDiagnostic:
        return "compile time only";
    case SendKind::inlineEnumDo:
    case SendKind::inlineDo:
        return "tier 0, inlined loop over " + type + " (no closure created)";
    case SendKind::printString:
        return "tier 1, runtime call (" + type + " printString)";
    case SendKind::stringConcat:
    case SendKind::stringCompare:
    case SendKind::stringPredicate:
    case SendKind::stringSplit:
    case SendKind::stringJoin:
    case SendKind::stringFromUtf8:
    case SendKind::stringFromUtf8Lossy:
    case SendKind::stringToUtf8:
    case SendKind::stringFromChar:
    case SendKind::stringViewCount:
    case SendKind::enumName:
    case SendKind::arrayLiteral:
    case SendKind::setLiteral:
    case SendKind::containerAdd:
    case SendKind::containerRemove:
    case SendKind::containerIncludes:
    case SendKind::containerAtIfAbsent:
    case SendKind::arrayRemoveLast:
    case SendKind::dictionaryKeys:
    case SendKind::dictionaryValues:
        return "tier 1, runtime call";
    case SendKind::classOf:
        return "tier 0, intrinsic load of the object's class";
    case SendKind::namedClass:
        return "tier 0, the class descriptor of '" + send.classInfo->name + "' (a constant)";
    case SendKind::className:
    case SendKind::classSuperclass:
        return "tier 0, intrinsic read of the class descriptor";
    case SendKind::objectIsOnStack:
        return "tier 0, intrinsic test of the object header's stack flag";
    case SendKind::classIsKindOf:
    case SendKind::classRespondsTo:
    case SendKind::classFields:
    case SendKind::classMessages:
    case SendKind::classProtocols:
        return "tier 1, runtime call (reflection)";
    case SendKind::classGetField:
        return "tier 1, runtime field lookup (checked against the stored type), then a load";
    case SendKind::classSetField:
        return "tier 1, runtime field lookup (checked against the stored type, needs invoke), then a store";
    case SendKind::classPerform:
        return "tier 3, runtime message lookup by selector text (checked against the stored signature), then a call through its thunk";
    case SendKind::classNew:
        return "tier 1, runtime call (the class's default constructor, checked against the expected type)";
    case SendKind::classNamed:
        return "tier 1, runtime search of the class registry";
    case SendKind::classQualifiedName:
        return "tier 0, intrinsic read of the class's introspection table";
    case SendKind::containerAt:
    case SendKind::containerAtPut:
        return send.operandType != nullptr && (send.operandType->kind == TypeKind::array || send.operandType->kind == TypeKind::fixedArray)
                   ? "tier 0, intrinsic " + std::string(send.operandType->kind == TypeKind::fixedArray ? "unchecked index (the index type is the range)" : "bounds-checked index")
                   : "tier 1, runtime hash lookup";
    case SendKind::portIn:
    case SendKind::portOut:
        return std::string("tier 0, one '") + (send.kind == SendKind::portIn ? "in" : "out") + "' instruction (" + type + ")";
    case SendKind::cpuInstruction:
        return "tier 0, the processor instruction for 'Cpu " + send.op + "'";
    default:
        return "tier 0, intrinsic";
    }
    return "";
}

}

std::string explainDispatch(const SemanticModel& model, const ast::AstArena& arena, const SourceFile& file)
{
    struct Line {
        uint32_t offset;
        std::string text;
    };
    std::vector<Line> lines;
    auto position = [&](uint32_t offset) {
        LineColumn at = file.lineColumn(offset);
        return file.path() + ":" + std::to_string(at.line) + ":" + std::to_string(at.column) + ": ";
    };

    for (const auto& [id, send] : model.sends) {
        const Node* node = arena.byId(id);
        std::string description = describe(send, model);
        if (node == nullptr || description.empty()) {
            continue;
        }
        uint32_t offset = selectorOffset(node);
        lines.push_back({offset, position(offset) + "'" + selectorOf(node) + "' -> " + description});
    }
    // Where each object a message creates lives.
    for (const auto& [id, classInfo] : model.defaultObjects) {
        const Node* node = arena.byId(id);
        if (node != nullptr && model.stackObjects.count(id) == 0) {
            lines.push_back({node->range.begin, position(node->range.begin) + "'" + classInfo->name + "' object -> heap (it may outlive this frame)"});
        }
    }
    for (const auto& [id, object] : model.stackObjects) {
        const Node* node = arena.byId(id);
        if (node != nullptr) {
            lines.push_back({node->range.begin, position(node->range.begin) + "'" + object.classInfo->name + "' object '" + object.local->name +
                                                    "' -> stack (it doesn't outlive this frame; no reference counting)"});
        }
    }
    for (const auto& [id, block] : model.blocks) {
        const Node* node = arena.byId(id);
        if (node == nullptr || block->inlined) {
            continue;
        }
        std::string text = position(node->range.begin) + "block -> compiled to function '" + block->symbolName + "'";
        if (block->captures.empty()) {
            text += ", captures nothing";
        }
        else if (model.stackBlocks.count(block) != 0) {
            text += ", captures ";
            for (size_t i = 0; i < block->captures.size(); ++i) {
                text += (i == 0 ? "" : ", ") + block->captures[i]->name;
            }
            text += " by reference (stack environment, no allocation)";
        }
        else {
            text += ", captures ";
            for (size_t i = 0; i < block->captures.size(); ++i) {
                const LocalInfo* captured = block->captures[i];
                text += (i == 0 ? "" : ", ") + captured->name + (captured->readOnly ? " (copied)" : " (boxed)");
            }
            auto why = model.blockEscapes.find(block);
            text += " -> heap closure" + (why != model.blockEscapes.end() ? " (" + why->second + ")" : std::string());
        }
        lines.push_back({node->range.begin, text});
    }

    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.offset < b.offset; });
    std::string out;
    for (const Line& line : lines) {
        out += line.text + "\n";
    }
    return out;
}

}
