#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bob::sema {

struct RecordInfo;
struct ProgramInfo;
struct ClassInfo;
struct EnumInfo;

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
    classType,
    optional,
    nilType,
    enumType,
    subrange,
    string,
    character,
    array,
    dictionary,
    hashtable,
    set,
    fixedArray,
    unionType,
    // The result of `x meta`: compile-time information about `inner` (a type).
    metaInfo,
    // The `compiler` object, reachable only from compile-time code.
    compilerObject,
    // `s bytes`, `s codepoints`, `s graphemes`: the string itself, seen as a collection.
    // `count` is the view (0 bytes, 1 codepoints, 2 graphemes), `inner` the element type.
    stringView,
};

// `meta` is the class side of something named in an expression: the program
// (`hello fact: 5`, `self` in a static message) or a type (`Point x: 1 y: 2`,
// `Color red`).
// `placeholder` is a built-in type name that exists but isn't implemented yet
// (slice, pointer, ...): it can be named, but values of it can't be used.
// `block` is a block type; `blockToInfer` is the bare type name `block`, whose
// parameter and result types are inferred from how it's used.
struct Type {
    TypeKind kind = TypeKind::error;
    std::string name;
    // integer, real; enums and subranges carry their storage integer here too.
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

    // classType: the class or protocol (instances are references); meta of a
    // class uses `instance`.
    ClassInfo* classInfo = nullptr;
    // optional: the wrapped type. subrange: the base (an integer or enum type).
    // array, set, fixedArray: the element type. dictionary, hashtable: the value type.
    const Type* inner = nullptr;
    // hashtable: the key type. fixedArray: the index type (enum or subrange).
    const Type* key = nullptr;
    // fixedArray: number of elements.
    uint64_t count = 0;

    // enumType
    EnumInfo* enumInfo = nullptr;
    // subrange bounds: integer values for integer subranges, ordinals for enum ones.
    int64_t low = 0;
    int64_t high = 0;
    // A distinct named type (`type PhysAddr := uint64;`): the type it was made from.
    // It has the same kind and storage, but its own identity.
    const Type* distinctOf = nullptr;

    // Containers whose element type is inferred from first use (`b: array;`)
    // start pending; once known, `resolved` points to the canonical type.
    const Type* resolved = nullptr;

    // unionType: the member types, in a canonical order.
    std::vector<const Type*> members;
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
    const Type* int64() const { return integer(64, true); }
    const Type* uint8() const { return integer(8, false); }
    const Type* real64() const { return real(64); }
    const Type* string() const { return string_; }
    const Type* character() const { return character_; }

    // Primitive and placeholder names (`int32`, `byte`, `string`, ...); nullptr if not built in.
    const Type* builtin(std::string_view name) const;

    Type* addRecord(RecordInfo* record);
    const Type* metaOf(const Type* instance);
    Type* addProgram(ProgramInfo* program);

    Type* addClass(ClassInfo* info);
    const Type* optionalOf(const Type* inner);
    const Type* nilType() const { return nil_; }

    Type* addEnum(EnumInfo* info, const Type* storage);
    const Type* subrange(const Type* base, int64_t low, int64_t high, const std::string& name);
    const Type* distinct(const Type* base, const std::string& name);

    const Type* arrayOf(const Type* element);
    const Type* dictionaryOf(const Type* value);
    const Type* hashtableOf(const Type* key, const Type* value);
    const Type* setOf(const Type* element);
    const Type* fixedArrayOf(const Type* element, const Type* index, uint64_t count);
    const Type* unionOf(std::vector<const Type*> members);
    const Type* stringView(int view);
    // The type of `x meta` for a subject type, and of the `compiler` object.
    const Type* metaInfoOf(const Type* subject);
    const Type* compilerObject() const { return compiler_; }
    // A container whose element (and key) types are still to be inferred.
    Type* pendingContainer(TypeKind kind);

    const Type* block(std::vector<const Type*> params, const Type* result);
    // A fresh, mutable block type whose shape is filled in by inference.
    Type* pendingBlock();

private:
    Type* own(Type type);

    std::vector<std::unique_ptr<Type>> owned_;
    std::unordered_map<std::string, const Type*> builtins_;
    std::unordered_map<const Type*, const Type*> metas_;
    std::unordered_map<const Type*, const Type*> optionals_;
    std::map<std::pair<int, std::pair<const Type*, const Type*>>, const Type*> containers_;
    std::map<std::vector<const Type*>, const Type*> unions_;
    const Type* nil_ = nullptr;
    const Type* error_ = nullptr;
    const Type* void_ = nullptr;
    const Type* bool_ = nullptr;
    const Type* string_ = nullptr;
    const Type* character_ = nullptr;
    const Type* views_[3] = {};
    const Type* compiler_ = nullptr;
    std::unordered_map<const Type*, const Type*> metaInfos_;
};

// A pending container's canonical type once its element type is known; otherwise itself.
const Type* canonical(const Type* type);

bool isInteger(const Type* type);
bool isReal(const Type* type);
bool isNumeric(const Type* type);
bool isBlock(const Type* type);
bool isClass(const Type* type);
bool isOptional(const Type* type);
bool isEnum(const Type* type);
bool isSubrange(const Type* type);
bool isString(const Type* type);
bool isUnion(const Type* type);
bool isStringView(const Type* type);
// array, dictionary, hashtable and non-bitmask set: heap buffers with value semantics.
bool isDynamicContainer(const Type* type);
// A `set` of enum members, stored as a bitmask.
bool isBitSet(const Type* type);
// Ordinal types: enums and enum subranges.
bool isOrdinal(const Type* type);
// Class/protocol instances and optionals of them: identity, may be nil.
bool isReference(const Type* type);
// Values held by one reference-counted pointer: references (nil = null), strings
// and dynamic containers (empty = null).
bool isManaged(const Type* type);
// Values that need retain/release when copied or dropped (managed values,
// and aggregates that contain them).
bool needsCleanup(const Type* type);
// An optional held as {present: i1, value}: every optional except a class optional.
bool isValueOptional(const Type* type);
bool isValueType(const Type* type);
bool sameBlockShape(const Type* a, const Type* b);
std::string blockTypeName(const Type* type);
// Subranges behave as their base type in arithmetic and comparisons.
const Type* decay(const Type* type);

// Lossless implicit conversions only: wider integer of compatible signedness,
// real32 -> real64, subrange -> its base, T -> T?, or identity.
bool canWiden(const Type* from, const Type* to);

// The type both operands widen to, or nullptr.
const Type* commonType(const Type* a, const Type* b);

bool integerFits(uint64_t magnitude, bool negative, const Type* type);

std::string typeName(const Type* type);

}
