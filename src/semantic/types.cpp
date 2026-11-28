#include "types.h"

#include "semanticModel.h"

#include <algorithm>

namespace bob::sema {

TypeTable::TypeTable()
{
    error_ = own({TypeKind::error, "<error>"});
    nil_ = own({TypeKind::nilType, "nil"});
    void_ = own({TypeKind::voidType, "void"});
    bool_ = own({TypeKind::boolType, "bool"});

    const uint32_t widths[] = {8, 16, 32, 64};
    for (uint32_t bits : widths) {
        builtins_["int" + std::to_string(bits)] = own({TypeKind::integer, "int" + std::to_string(bits), bits, true});
        builtins_["uint" + std::to_string(bits)] = own({TypeKind::integer, "uint" + std::to_string(bits), bits, false});
    }
    builtins_["real32"] = own({TypeKind::real, "real32", 32});
    builtins_["real64"] = own({TypeKind::real, "real64", 64});
    builtins_["bool"] = bool_;
    builtins_["byte"] = builtins_["uint8"];
    builtins_["intptr"] = builtins_["int64"];
    builtins_["uintptr"] = builtins_["uint64"];
    string_ = own({TypeKind::string, "string"});
    builtins_["string"] = string_;
    // A Unicode scalar value, stored as uint32.
    character_ = own({TypeKind::character, "char", 32, false});
    builtins_["char"] = character_;

    // The bare container names: the element type is inferred (pending containers).
    builtins_["array"] = own({TypeKind::array, "array"});
    builtins_["dictionary"] = own({TypeKind::dictionary, "dictionary"});
    builtins_["hashtable"] = own({TypeKind::hashtable, "hashtable"});
    builtins_["set"] = own({TypeKind::set, "set"});

    const char* placeholders[] = {"slice", "pointer", "int128", "uint128"};
    for (const char* name : placeholders) {
        builtins_[name] = own({TypeKind::placeholder, name});
    }
    builtins_["block"] = own({TypeKind::blockToInfer, "block"});

    compiler_ = own({TypeKind::compilerObject, "compiler"});
    classObject_ = own({TypeKind::classObject, "Class"});
    builtins_["Class"] = classObject_;
    reflector_ = own({TypeKind::reflector, "Reflector"});
    builtins_["Reflector"] = reflector_;

    const char* viewNames[] = {"string bytes", "string codepoints", "string graphemes"};
    const Type* viewElements[] = {integer(8, false), character_, string_};
    for (int i = 0; i < 3; ++i) {
        Type* view = own({TypeKind::stringView, viewNames[i]});
        view->count = static_cast<uint64_t>(i);
        view->inner = viewElements[i];
        views_[i] = view;
    }
}

Type* TypeTable::addClass(ClassInfo* info)
{
    Type* type = own({TypeKind::classType, info->name});
    type->classInfo = info;
    return type;
}

const Type* TypeTable::optionalOf(const Type* inner)
{
    auto it = optionals_.find(inner);
    if (it != optionals_.end()) {
        return it->second;
    }
    std::string name = inner->name.find(' ') != std::string::npos ? "[" + inner->name + "]?" : inner->name + "?";
    Type* type = own({TypeKind::optional, name});
    type->inner = inner;
    type->classInfo = inner->classInfo;
    optionals_[inner] = type;
    return type;
}

Type* TypeTable::addEnum(EnumInfo* info, const Type* storage)
{
    Type* type = own({TypeKind::enumType, info->name, storage->bits, storage->isSigned});
    type->enumInfo = info;
    return type;
}

const Type* TypeTable::subrange(const Type* base, int64_t low, int64_t high, const std::string& name)
{
    Type* type = own({TypeKind::subrange, name, base->bits, base->isSigned});
    type->inner = base;
    type->low = low;
    type->high = high;
    type->enumInfo = base->enumInfo;
    return type;
}

const Type* TypeTable::distinct(const Type* base, const std::string& name)
{
    Type copy = *base;
    copy.name = name;
    copy.distinctOf = base;
    copy.resolved = nullptr;
    return own(std::move(copy));
}

namespace {

std::string elementName(const Type* type)
{
    return type->name.find(' ') != std::string::npos ? "[" + type->name + "]" : type->name;
}

}

// Blocks match by shape, but containers are keyed by their element type's
// identity, so a block element is replaced by the one type that stands for its shape.
const Type* TypeTable::blockShape(const Type* type)
{
    type = canonical(type);
    if (type == nullptr || type->kind != TypeKind::block || type->pending) {
        return type;
    }
    std::vector<const Type*> parts;
    for (const Type* param : type->params) {
        parts.push_back(blockShape(param));
    }
    parts.push_back(blockShape(type->result));
    auto it = blockShapes_.find(parts);
    if (it != blockShapes_.end()) {
        return it->second;
    }
    blockShapes_[parts] = type;
    return type;
}

const Type* TypeTable::arrayOf(const Type* element)
{
    element = blockShape(element);
    auto key = std::make_pair(static_cast<int>(TypeKind::array), std::make_pair(element, static_cast<const Type*>(nullptr)));
    auto it = containers_.find(key);
    if (it != containers_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::array, "array of: " + elementName(element)});
    type->inner = element;
    containers_[key] = type;
    return type;
}

const Type* TypeTable::dictionaryOf(const Type* value)
{
    value = blockShape(value);
    auto key = std::make_pair(static_cast<int>(TypeKind::dictionary), std::make_pair(value, static_cast<const Type*>(nullptr)));
    auto it = containers_.find(key);
    if (it != containers_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::dictionary, "dictionary of: " + elementName(value)});
    type->inner = value;
    type->key = string_;
    containers_[key] = type;
    return type;
}

const Type* TypeTable::hashtableOf(const Type* keyType, const Type* value)
{
    value = blockShape(value);
    auto key = std::make_pair(static_cast<int>(TypeKind::hashtable), std::make_pair(value, keyType));
    auto it = containers_.find(key);
    if (it != containers_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::hashtable, "hashtable keys: " + elementName(keyType) + " values: " + elementName(value)});
    type->inner = value;
    type->key = keyType;
    containers_[key] = type;
    return type;
}

const Type* TypeTable::setOf(const Type* element)
{
    auto key = std::make_pair(static_cast<int>(TypeKind::set), std::make_pair(element, static_cast<const Type*>(nullptr)));
    auto it = containers_.find(key);
    if (it != containers_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::set, "set of: " + elementName(element)});
    type->inner = element;
    if (isEnum(element)) {
        // Bit n is the member whose value is n: the smallest of 8/16/32/64 bits that
        // holds the largest value, or the enum's storage if that's wider.
        uint64_t largest = 0;
        for (const EnumMemberInfo& member : element->enumInfo->members) {
            largest = std::max(largest, member.value);
        }
        uint32_t bits = 8;
        while (bits < 64 && largest >= bits) {
            bits *= 2;
        }
        type->bits = std::max(bits, element->bits);
    }
    containers_[key] = type;
    return type;
}

const Type* TypeTable::fixedArrayOf(const Type* element, const Type* index, uint64_t count)
{
    auto key = std::make_pair(static_cast<int>(TypeKind::fixedArray), std::make_pair(element, index));
    auto it = containers_.find(key);
    if (it != containers_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::fixedArray, "array of: " + elementName(element) + " by: " + elementName(index)});
    type->inner = element;
    type->key = index;
    type->count = count;
    containers_[key] = type;
    return type;
}

const Type* TypeTable::metaInfoOf(const Type* subject)
{
    auto it = metaInfos_.find(subject);
    if (it != metaInfos_.end()) {
        return it->second;
    }
    Type* type = own({TypeKind::metaInfo, typeName(subject) + " meta"});
    type->inner = subject;
    metaInfos_[subject] = type;
    return type;
}

const Type* TypeTable::stringView(int view)
{
    return views_[view];
}

Type* TypeTable::pendingContainer(TypeKind kind)
{
    Type type{kind, builtins_.at(kind == TypeKind::array        ? "array"
                                 : kind == TypeKind::dictionary ? "dictionary"
                                 : kind == TypeKind::hashtable  ? "hashtable"
                                                                : "set")
                        ->name};
    type.pending = true;
    if (kind == TypeKind::dictionary) {
        type.key = string_;
    }
    return own(std::move(type));
}

const Type* TypeTable::block(std::vector<const Type*> params, const Type* result)
{
    Type type{TypeKind::block, ""};
    type.params = std::move(params);
    type.result = result;
    type.arityKnown = true;
    type.name = blockTypeName(&type);
    return own(std::move(type));
}

Type* TypeTable::pendingBlock()
{
    Type type{TypeKind::block, "block"};
    type.pending = true;
    return own(std::move(type));
}

Type* TypeTable::own(Type type)
{
    owned_.push_back(std::make_unique<Type>(std::move(type)));
    return owned_.back().get();
}

const Type* TypeTable::integer(uint32_t bits, bool isSigned) const
{
    return builtins_.at((isSigned ? "int" : "uint") + std::to_string(bits));
}

const Type* TypeTable::real(uint32_t bits) const
{
    return builtins_.at("real" + std::to_string(bits));
}

const Type* TypeTable::builtin(std::string_view name) const
{
    auto it = builtins_.find(std::string(name));
    return it == builtins_.end() ? nullptr : it->second;
}

Type* TypeTable::addRecord(RecordInfo* record)
{
    Type* type = own({TypeKind::record, record->name});
    type->record = record;
    return type;
}

const Type* TypeTable::metaOf(const Type* instance)
{
    auto it = metas_.find(instance);
    if (it != metas_.end()) {
        return it->second;
    }
    Type* meta = own({TypeKind::meta, instance->name + " class"});
    meta->instance = instance;
    metas_[instance] = meta;
    return meta;
}

Type* TypeTable::addProgram(ProgramInfo* program)
{
    Type* type = own({TypeKind::meta, program->name});
    type->program = program;
    return type;
}

const Type* canonical(const Type* type)
{
    while (type != nullptr && type->resolved != nullptr) {
        type = type->resolved;
    }
    return type;
}

bool isInteger(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::integer;
}

bool isReal(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::real;
}

bool isNumeric(const Type* type)
{
    return isInteger(type) || isReal(type);
}

bool isBlock(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::block;
}

bool isClass(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::classType;
}

bool isOptional(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::optional;
}

bool isEnum(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::enumType;
}

bool isSubrange(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::subrange;
}

bool isString(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::string;
}


bool isStringView(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::stringView;
}

bool isBitSet(const Type* type)
{
    return type != nullptr && type->kind == TypeKind::set && isEnum(type->inner);
}

bool isDynamicContainer(const Type* type)
{
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
    case TypeKind::array:
    case TypeKind::dictionary:
    case TypeKind::hashtable:
        return true;
    case TypeKind::set:
        return !isBitSet(canonical(type));
    default:
        return false;
    }
}

bool isOrdinal(const Type* type)
{
    return isEnum(type) || (isSubrange(type) && isEnum(type->inner));
}

bool isReference(const Type* type)
{
    return isClass(type) || (isOptional(type) && isClass(type->inner));
}

bool isManaged(const Type* type)
{
    if (type == nullptr) {
        return false;
    }
    if (isOptional(type)) {
        return isClass(type->inner);
    }
    return isClass(type) || isString(type) || isStringView(type) || isDynamicContainer(type);
}

bool needsCleanup(const Type* type)
{
    type = canonical(type);
    if (isManaged(type) || isBlock(type)) {
        return true;
    }
    if (isOptional(type)) {
        return needsCleanup(type->inner);
    }
    if (type != nullptr && type->kind == TypeKind::fixedArray) {
        return needsCleanup(type->inner);
    }
    return false;
}

bool isValueOptional(const Type* type)
{
    return isOptional(type) && !isClass(type->inner);
}

bool isValueType(const Type* type)
{
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
    case TypeKind::boolType:
    case TypeKind::integer:
    case TypeKind::real:
    case TypeKind::record:
    case TypeKind::block:
    case TypeKind::classType:
    case TypeKind::optional:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::string:
    case TypeKind::character:
    case TypeKind::array:
    case TypeKind::dictionary:
    case TypeKind::hashtable:
    case TypeKind::set:
    case TypeKind::fixedArray:
    case TypeKind::stringView:
    case TypeKind::classObject:
    case TypeKind::reflector:
        return true;
    default:
        return false;
    }
}

const Type* decay(const Type* type)
{
    return isSubrange(type) ? type->inner : type;
}

bool sameBlockShape(const Type* a, const Type* b)
{
    if (!isBlock(a) || !isBlock(b) || a->params.size() != b->params.size() || a->result != b->result) {
        return false;
    }
    for (size_t i = 0; i < a->params.size(); ++i) {
        if (a->params[i] != b->params[i]) {
            return false;
        }
    }
    return true;
}

std::string blockTypeName(const Type* type)
{
    if (type->pending && !type->arityKnown && type->result == nullptr) {
        return "block";
    }
    std::string name = "{";
    for (size_t i = 0; i < type->params.size(); ++i) {
        name += (i == 0 ? " " : ", ") + (type->params[i] != nullptr ? type->params[i]->name : std::string("?"));
    }
    if (type->result != nullptr && type->result->kind != TypeKind::voidType) {
        name += " [" + type->result->name + "]";
    }
    return name + " }";
}

bool canWiden(const Type* from, const Type* to)
{
    from = canonical(from);
    to = canonical(to);
    if (from == to) {
        return true;
    }
    if (from == nullptr || to == nullptr) {
        return false;
    }
    if (isBlock(from) && isBlock(to)) {
        return sameBlockShape(from, to);
    }
    if (from->kind == TypeKind::nilType) {
        return isOptional(to);
    }
    if (isClass(from) && isClass(to)) {
        return from->classInfo->isKindOf(to->classInfo);
    }
    if (isOptional(to)) {
        if (isOptional(from)) {
            return isReference(from) && isReference(to) ? canWiden(from->inner, to->inner) : canonical(from->inner) == canonical(to->inner);
        }
        return canWiden(from, to->inner);
    }
    // A subrange converts implicitly out to its base (always valid).
    if (isSubrange(from) && !isSubrange(to)) {
        return canWiden(from->inner, to);
    }
    // Distinct named types mix only with themselves.
    if (from->distinctOf != nullptr || to->distinctOf != nullptr) {
        return false;
    }
    if (isInteger(from) && isInteger(to)) {
        if (from->isSigned == to->isSigned) {
            return to->bits > from->bits;
        }
        return !from->isSigned && to->isSigned && to->bits > from->bits;
    }
    if (isReal(from) && isReal(to)) {
        return to->bits > from->bits;
    }
    return false;
}

const Type* commonType(const Type* a, const Type* b)
{
    if (canWiden(a, b)) {
        return canonical(b);
    }
    if (canWiden(b, a)) {
        return canonical(a);
    }
    return nullptr;
}

bool integerFits(uint64_t magnitude, bool negative, const Type* type)
{
    if (isSubrange(type) && isInteger(type->inner)) {
        if (type->isSigned) {
            if (!negative && magnitude > static_cast<uint64_t>(INT64_MAX)) {
                return false;
            }
            int64_t value = negative ? static_cast<int64_t>(~magnitude + 1) : static_cast<int64_t>(magnitude);
            return value >= type->low && value <= type->high;
        }
        return (!negative || magnitude == 0) && magnitude >= static_cast<uint64_t>(type->low) && magnitude <= static_cast<uint64_t>(type->high);
    }
    if (!isInteger(type)) {
        return false;
    }
    if (!type->isSigned) {
        if (negative && magnitude != 0) {
            return false;
        }
        return type->bits == 64 || magnitude <= ((uint64_t(1) << type->bits) - 1);
    }
    uint64_t limit = uint64_t(1) << (type->bits - 1);
    return negative ? magnitude <= limit : magnitude < limit;
}

std::string typeName(const Type* type)
{
    return type == nullptr ? "<none>" : canonical(type)->name;
}

}
