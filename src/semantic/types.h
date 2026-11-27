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
    block,
    blockToInfer,
};

// `meta` is the class side of something named in an expression: the program
// (`hello fact: 5`, `self` in a static message) or a record type (`Point x: 1 y: 2`).
// `placeholder` is a built-in type name that exists but isn't implemented yet
// (array, string, ...): it can be named, but values of it can't be used.
// `block` is a block type; `blockToInfer` is the bare type name `block`, whose
// parameter and result types are inferred from how it's used.
struct Type {
    TypeKind kind = TypeKind::error;
    std::string name;
    uint32_t bits = 0;
    bool isSigned = false;
    RecordInfo* record = nullptr;
    const Type* instance = nullptr;
    ProgramInfo* program = nullptr;

    // block: parameter types and result (void = no result). While a bare `block`
    // parameter is being inferred, `pending` is set and entries may be null.
    std::vector<const Type*> params;
    const Type* result = nullptr;
    bool pending = false;
    bool arityKnown = false;
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

    const Type* block(std::vector<const Type*> params, const Type* result);
    // A fresh, mutable block type whose shape is filled in by inference.
    Type* pendingBlock();

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
bool isBlock(const Type* type);
bool isValueType(const Type* type);
bool sameBlockShape(const Type* a, const Type* b);
std::string blockTypeName(const Type* type);

// Lossless implicit conversions only: wider integer of compatible signedness,
// real32 -> real64, or identity.
bool canWiden(const Type* from, const Type* to);

// The type both operands widen to, or nullptr.
const Type* commonType(const Type* a, const Type* b);

bool integerFits(uint64_t magnitude, bool negative, const Type* type);

std::string typeName(const Type* type);

}
