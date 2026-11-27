#include "types.h"

#include "semanticModel.h"

namespace bob::sema {

TypeTable::TypeTable()
{
    error_ = own({TypeKind::error, "<error>"});
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

    const char* placeholders[] = {"array", "string", "dictionary", "hashtable", "set", "slice", "pointer", "char", "int128", "uint128"};
    for (const char* name : placeholders) {
        builtins_[name] = own({TypeKind::placeholder, name});
    }
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

bool isValueType(const Type* type)
{
    return type != nullptr && (type->kind == TypeKind::boolType || type->kind == TypeKind::integer ||
                               type->kind == TypeKind::real || type->kind == TypeKind::record);
}

bool canWiden(const Type* from, const Type* to)
{
    if (from == to) {
        return true;
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
        return b;
    }
    if (canWiden(b, a)) {
        return a;
    }
    return nullptr;
}

bool integerFits(uint64_t magnitude, bool negative, const Type* type)
{
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
    return type == nullptr ? "<none>" : type->name;
}

}
