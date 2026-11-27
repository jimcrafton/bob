#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bob::sema {

struct RecordInfo;
struct ProgramInfo;

enum class TypeKind : uint8_t {
    error,
    voidType,
    boolType,
    integer,
    real,
    record,
    meta,
    placeholder,
};

// `meta` is the class side of something named in an expression: the program
// (`hello fact: 5`, `self` in a static message) or a record type (`Point x: 1 y: 2`).
// `placeholder` is a built-in type name that exists but isn't implemented yet
// (array, string, ...): it can be named, but values of it can't be used.
struct Type {
    TypeKind kind = TypeKind::error;
    std::string name;
    uint32_t bits = 0;
    bool isSigned = false;
    RecordInfo* record = nullptr;
    const Type* instance = nullptr;
    ProgramInfo* program = nullptr;
};

class TypeTable {
public:
    TypeTable();

    const Type* error() const { return error_; }
    const Type* voidType() const { return void_; }
    const Type* boolType() const { return bool_; }
    const Type* integer(uint32_t bits, bool isSigned) const;
    const Type* real(uint32_t bits) const;
    const Type* int32() const { return integer(32, true); }
    const Type* real64() const { return real(64); }

    // Primitive and placeholder names (`int32`, `byte`, `array`, ...); nullptr if not built in.
    const Type* builtin(std::string_view name) const;

    Type* addRecord(RecordInfo* record);
    const Type* metaOf(const Type* instance);
    Type* addProgram(ProgramInfo* program);

private:
    Type* own(Type type);

    std::vector<std::unique_ptr<Type>> owned_;
    std::unordered_map<std::string, const Type*> builtins_;
    std::unordered_map<const Type*, const Type*> metas_;
    const Type* error_ = nullptr;
    const Type* void_ = nullptr;
    const Type* bool_ = nullptr;
};

bool isInteger(const Type* type);
bool isReal(const Type* type);
bool isNumeric(const Type* type);
bool isValueType(const Type* type);

// Lossless implicit conversions only: wider integer of compatible signedness,
// real32 -> real64, or identity.
bool canWiden(const Type* from, const Type* to);

// The type both operands widen to, or nullptr.
const Type* commonType(const Type* a, const Type* b);

bool integerFits(uint64_t magnitude, bool negative, const Type* type);

std::string typeName(const Type* type);

}
