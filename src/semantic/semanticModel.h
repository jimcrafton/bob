#pragma once

#include "ast/ast.h"
#include "parser/parser.h"
#include "types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bob::sema {

enum class OverflowMode : uint8_t {
    trap,
    wrap,
    saturate,
};

struct ClassInfo;
struct MessageInfo;
struct ModuleInfo;

// Where a declaration comes from: `module` is null for the unit being compiled,
// otherwise the imported module (design §11.5). `exported` marks the unit's own
// declarations in its `interface { }` section.
struct Origin {
    const ModuleInfo* module = nullptr;
    bool exported = false;
};

// How a field or variable holds a class reference (design §6.1).
enum class Ownership : uint8_t {
    strong,
    // `@[weak]`: doesn't keep the object alive and reads as nil once it's destroyed (type `C?`).
    weak,
    // `@[unowned]`: doesn't keep the object alive; reading it once the object is destroyed traps.
    unowned,
};

// What a class emits for reflection (design §9); each level includes the ones before.
enum class ReflectLevel : uint8_t {
    none,
    identity,
    introspect,
    invoke,
};

const char* reflectLevelName(ReflectLevel level);
bool parseReflectLevel(std::string_view name, ReflectLevel& level);

struct FieldInfo {
    std::string name;
    const Type* type = nullptr;
    Ownership ownership = Ownership::strong;
    uint32_t index = 0;
    const ast::FieldDecl* decl = nullptr;
    // Class fields only: declaring class, visibility, initializer run by `new`.
    const ClassInfo* owner = nullptr;
    bool isPublic = true;
    const ast::Node* initializer = nullptr;
    // A non-optional class field with no initializer: default-constructed when its
    // class allows it, otherwise `required` (a constructor message must pass it).
    bool constructsDefault = false;
    bool required = false;
    // Record fields: `@[align: n]`, the least alignment of the field's offset (0 = its natural one).
    uint64_t align = 0;
};

struct RecordInfo {
    Origin origin;
    std::string name;
    std::vector<FieldInfo> fields;
    bool packed = false;
    // `@[align: n]` on the record: its least alignment (0 = the natural one); the size rounds up to it.
    uint64_t align = 0;
    const ast::RecordDecl* decl = nullptr;
    Type* type = nullptr;
    bool resolved = false;
    bool resolving = false;

    const FieldInfo* field(std::string_view fieldName) const;
    std::string constructorSelector() const;
};

struct BlockInfo;

struct LocalInfo {
    std::string name;
    const Type* type = nullptr;
    Ownership ownership = Ownership::strong;
    const ast::Node* decl = nullptr;
    bool isParameter = false;
    bool readOnly = false;
    uint32_t index = 0;
    // The block function that owns this variable's storage; null = the message itself.
    const BlockInfo* owner = nullptr;
    // Some block captures it by reference (loads of references are then retained).
    bool captured = false;
    // `@[align: n] buffer: …;`: the least alignment of the variable's storage (0 = its type's).
    uint64_t align = 0;
};

// A block literal. Inlined blocks (literal arguments of ifTrue:, to:do:, ...)
// run in the enclosing function and own no variables. Other blocks compile to
// their own function and reach captured variables through an environment
// (capture by reference): a stack record when the block doesn't outlive its
// frame (SemanticModel::stackBlocks), otherwise a heap closure.
struct BlockInfo {
    const ast::BlockLiteral* node = nullptr;
    const Type* type = nullptr;
    bool inlined = false;
    const BlockInfo* parent = nullptr;
    std::vector<LocalInfo*> params;
    std::vector<LocalInfo*> locals;
    std::vector<const LocalInfo*> captures;
    std::string symbolName;
};

struct MessageInfo {
    Origin origin;
    // Declared in an imported interface: no body here, the code is in its library.
    bool imported = false;
    std::string selector;
    std::vector<LocalInfo*> params;
    std::vector<LocalInfo*> locals;
    const Type* returnType = nullptr;
    const ast::MessageDecl* decl = nullptr;
    std::string symbolName;
    OverflowMode overflow = OverflowMode::trap;
    bool isMain = false;

    // Class messages. Instance messages take (self, params...);
    // class-side (@[static]) messages take only their params.
    ClassInfo* owner = nullptr;
    bool isInstance = false;
    bool isProtocolDefault = false;
    bool isRequired = false;
    LocalInfo* self = nullptr;
    // `msg init { }`: run automatically after construction (superclass first); not sendable.
    bool isInit = false;
    // The generated `copy` (no declaration): a shallow copy, then `init`.
    bool isCopy = false;
    // `@[compiletime] msg`: callable only from compile-time code; not in the program.
    bool compileTimeOnly = false;
    // `@[extern: "C"]` (design §6.6): class-side, C types only, the C ABI under `cSymbol`.
    // Without a body it imports that C function (calls need unsafe); with one it is
    // also exported under that name.
    bool externC = false;
    bool externImport = false;
    std::string cSymbol;
    // What the body uses directly, for ordering compile-time evaluation
    // (filled during checking; vtable and protocol sends list their target).
    std::vector<const MessageInfo*> calls;
    std::vector<const ClassInfo*> constructs;
    std::vector<struct ConstantInfo*> constantsUsed;
    std::vector<struct CompileTimeItem*> itemsUsed;
    // Escape summary (analyzerEscape.cpp): whether the message may keep `self` or a
    // parameter beyond the call (store it in a field or container, return it ...).
    bool selfEscapes = false;
    std::vector<bool> paramEscapes;
};

struct ClassInfo {
    Origin origin;
    // The unit that declares it (its symbols are `bob.<unit>.<Class>...`).
    std::string unitName;
    std::string name;
    const ast::ClassDecl* decl = nullptr;
    bool isProtocol = false;
    // After the unit default and the build cap; an imported class keeps its module's.
    ReflectLevel reflectLevel = ReflectLevel::introspect;
    ClassInfo* superclass = nullptr;
    std::vector<ClassInfo*> protocols;
    std::vector<ClassInfo*> subclasses;
    std::vector<FieldInfo> fields;
    // Own instance messages (including protocol defaults adopted here), in order.
    std::vector<MessageInfo*> methodList;
    std::unordered_map<std::string, MessageInfo*> methods;
    std::unordered_map<std::string, MessageInfo*> classMethods;
    // Vtable slots: inherited slots first, then one per new selector.
    std::vector<std::string> slotSelectors;
    std::vector<const MessageInfo*> vtable;
    // Context for field initializers, which run when an object is constructed.
    MessageInfo* initializer = nullptr;
    MessageInfo* initMessage = nullptr;
    // `x: C;` can construct one with every field defaulted (no required fields).
    bool defaultable = false;
    int defaultState = 0;
    Type* type = nullptr;
    bool resolved = false;
    bool resolving = false;

    const FieldInfo* field(std::string_view fieldName) const;
    // Class: self or an ancestor or a conformed protocol; protocol: self or a refined protocol.
    bool isKindOf(const ClassInfo* other) const;
    // Instance message visible on this class or protocol (own, inherited, protocol default).
    MessageInfo* lookup(const std::string& selector) const;
    int slotOf(const std::string& selector) const;
    // Protocol: its witness-table slots. Refined protocols' messages come first
    // (in the order they're refined), then its own in declaration order.
    std::vector<std::string> witnessSelectors() const;
    int witnessSlotOf(const std::string& selector) const;
    // Class: every protocol it conforms to, directly, by refinement or through a superclass.
    std::vector<const ClassInfo*> conformedProtocols() const;
};

struct EnumMemberInfo {
    std::string name;
    // Two's-complement bits of the member's value in the storage type.
    uint64_t value = 0;
    uint32_t ordinal = 0;
    const ast::EnumMember* decl = nullptr;
};

struct EnumInfo {
    Origin origin;
    std::string name;
    const ast::EnumDecl* decl = nullptr;
    const Type* storage = nullptr;
    std::vector<EnumMemberInfo> members;
    Type* type = nullptr;
    // Values are 0, 1, 2 ... in declaration order, so ordinal = value.
    bool dense = true;

    const EnumMemberInfo* member(std::string_view memberName) const;
    const EnumMemberInfo* memberWithValue(uint64_t value) const;
};

// `type Name := ...;`
struct NamedTypeInfo {
    Origin origin;
    std::string name;
    const ast::TypeAliasDecl* decl = nullptr;
    const Type* type = nullptr;
    bool isAlias = false;
    bool resolving = false;
};

struct ProgramInfo {
    std::string name;
    std::vector<MessageInfo*> messages;
    std::unordered_map<std::string, MessageInfo*> bySelector;
    MessageInfo* main = nullptr;
    Type* type = nullptr;
};

// A value computed at compile time and embedded in the program (design §8).
struct ConstValue {
    const Type* type = nullptr;
    // Integers, bool, enums, characters, enum sets: two's-complement bits.
    uint64_t bits = 0;
    double real = 0.0;
    std::string text;                // string
    std::vector<ConstValue> elements; // array, fixed array, set, record fields, the value of an optional
    std::vector<std::pair<ConstValue, ConstValue>> entries; // dictionary, hashtable
    bool present = true;             // optional
};

// Something evaluated during compilation: a unit-level constant, a
// `@[compiletime] (expr)` or a `@[compiletime] { statements }` block.
struct CompileTimeItem {
    enum class Kind : uint8_t { constant, expression, block } kind = Kind::expression;
    const ast::Node* node = nullptr;
    // The code to run: an expression, or a statement group for blocks.
    const ast::Node* code = nullptr;
    // The synthesized message it's checked and compiled as (its locals, its calls).
    MessageInfo* context = nullptr;
    const Type* type = nullptr;
    const ConstValue* value = nullptr;
    bool evaluated = false;
};

// `@[constant] NAME := expr;` or `@[constant] name: T := expr;` at unit level: an
// immutable value computed at compile time.
struct ConstantInfo {
    Origin origin;
    std::string name;
    // The declaration (an Assignment or a VariableDecl), its name, its written type and value.
    const ast::Node* decl = nullptr;
    SourceRange nameRange;
    const ast::Node* typeNode = nullptr;
    const ast::Node* value = nullptr;
    CompileTimeItem* item = nullptr;
    const Type* type = nullptr;
    int state = 0; // 1 while checking (cycles), 2 when checked
};

// An object created by `x: C;` or `x := C f: …` that lives in its creator's
// frame because it provably doesn't outlive it (design §4.3a).
struct StackObject {
    const ClassInfo* classInfo = nullptr;
    const LocalInfo* local = nullptr;
    // The `x: C;` declaration, or the constructor send.
    const ast::Node* allocation = nullptr;
    // The function whose frame holds it: a block function, or the message (block == null).
    const MessageInfo* message = nullptr;
    const BlockInfo* block = nullptr;
    bool forced = false;
};

enum class SendKind : uint8_t {
    arithmetic,
    bitwise,
    shift,
    comparison,
    logicalAnd,
    logicalOr,
    negate,
    absolute,
    boolNot,
    conversion,
    minimum,
    maximum,
    wrapArithmetic,
    saturateArithmetic,
    staticCall,
    recordConstructor,
    interval,
    blockInvoke,
    inlineIf,
    inlineToDo,
    inlineTimesRepeat,
    inlineIfNil,
    newObject,
    classStaticCall,
    directMethodCall,
    vtableCall,
    dynamicSend,
    superCall,
    fieldGet,
    fieldSet,
    referenceCompare,
    constructObject,   // `Point x: 4 y: 7`: `fields` are the public fields passed, in order

    // phase 5
    constant,          // a compile-time value: `constant` holds its bits, resultType its type
    enumValue,         // `c value`: the storage integer
    enumOrdinal,       // `c ordinal`
    enumSucc,
    enumPred,
    enumFromValue,     // `Color fromValue: n` -> Color?
    enumName,          // `c name` -> string
    enumRange,         // the iterable of `for c in Color` / `Color red to: Color blue`
    inlineEnumDo,      // `Color do: { [c] ... }`
    distinctValue,     // `p value` on a distinct type: same bits, base type
    distinctFromValue, // `PhysAddr fromValue: n`
    subrangeFromValue, // `Percent fromValue: n` -> Percent?
    optionalCompare,   // `x = nil`, `x <> nil` on an optional of a plain value
    bitSetLiteral,     // `[Perm read, Perm write]` as a `set of: Perm`
    bitSetOp,          // `+ - *` on bit sets
    bitSetIncludes,
    bitSetCount,
    bitSetValue,
    bitSetFromValue,
    bitSetMutate,      // `s add: m`, `s remove: m`
    bitSetIsEmpty,

    // strings (`op` holds the selector where several share a kind)
    stringConcat,      // a + b
    stringCompare,     // = <> < <= > >= (byte-wise)
    stringView,        // s bytes / codepoints / graphemes: the same string, as a view
    stringViewCount,   // s bytes count ...
    stringByteAt,      // s bytes at: n
    stringIsEmpty,
    stringPredicate,   // startsWith: endsWith: includesSubstring:
    stringSplit,       // s split: ","  -> array of: string
    stringJoin,        // ", " join: names
    stringFromUtf8,    // string fromUtf8: bytes -> string?
    stringFromUtf8Lossy,
    stringToUtf8,      // s toUtf8 -> array of: uint8
    printString,       // operandType: the receiver's type
    charValue,         // c value -> uint32
    charFromValue,     // char fromValue: n -> char?
    stringFromChar,    // c asString

    // containers
    arrayLiteral,      // resultType: the array; elements converted to its element type
    setLiteral,
    fixedArrayLiteral,
    containerCount,
    containerIsEmpty,
    containerAt,       // array/fixed array: index; dictionary/hashtable: key -> V?
    containerAtPut,
    containerAtIfAbsent,
    containerIncludes, // array/set: element; dictionary/hashtable: key (includesKey:)
    containerAdd,      // array add:, set add:
    containerRemove,   // set remove:, dictionary removeKey:
    arrayFirst,
    arrayLast,
    arrayRemoveLast,
    dictionaryKeys,
    dictionaryValues,
    inlineDo,          // do:, keysDo:, keysAndValuesDo: with a literal block (op = selector)
    collectionRange,   // the iterable of `for x in collection`

    // compile time (phase 8 of the design, phase 6 of the plan)
    constantValue,     // `value` holds it: meta results, compile-time expressions, constants
    metaInfo,          // `x meta` itself (`op` is the variable's name, if any)
    metaFields,        // `T meta fields`: an array of FieldMeta, built where it's used
    compilerDiagnostic,// `compiler error:` / `warning:` / `note:`

    // reflection (design §9)
    classOf,           // `obj class`: the receiver's class
    namedClass,        // `Button class`: classInfo
    className,         // `c name`
    classSuperclass,   // `c superclass` -> Class?
    classIsKindOf,
    classRespondsTo,
    classCompare,      // `c = d`, `c <> d` (op)
    objectIsOnStack,   // `obj isOnStack`
    classFields,       // `c fields` -> array of: FieldMeta (classInfo)
    classMessages,     // `c messages` -> array of: MessageMeta (classInfo)
    classProtocols,    // `c protocols` -> array of: string
    classGetField,     // `c get: "name" of: obj`; resultType from context
    classQualifiedName,// `c qualifiedName` -> "unit.Class"
    classSetField,     // `c set: "name" of: obj to: value`; operandType is the value's type
    classPerform,      // `c perform: "sel" on: obj with: …`; resultType from context (void without one)
    classNew,          // `c new`; resultType from context, classInfo the expected class or protocol
    classNamed,        // `Class named: "unit.Button"` -> Class?

    // pointers and slices (design §6.5); operandType is the pointer or slice type
    pointerAddress,    // `x address`: the receiver is a place; frameBound when it's a local's
    pointerLoad,       // `p contents`
    pointerStore,      // `p contents: v`
    pointerAdvance,    // `p advancedBy: n` (elements)
    pointerFromInteger,// `n asPointer` -> resultType (a pointer or optional pointer)
    pointerCast,       // `p asPointer`, `p asPointer: T`
    pointerToInteger,  // `p asUIntPtr`
    pointerSlice,      // `p slice: n`
    sliceCount,
    sliceIsEmpty,
    sliceAt,           // bounds-checked
    sliceAtPut,
    cFunctionInvoke,   // `f value: a` on a cFunction (operandType), through the C ABI

    // I/O ports and processor instructions (design §6.7); operandType is the port type
    portFromInteger,   // `n asPort` -> resultType; traps outside 0 to: 65535
    portCast,          // `p asPort`, `p asPort: T`: the same port number, another width
    portIn,            // `p contents`: one `in` of the port's width
    portOut,           // `p contents: v`: one `out`
    portAdvance,       // `p advancedBy: n`; traps outside 0 to: 65535
    portToInteger,     // `p asUInt16`
    cpuInstruction,    // `Cpu halt`, `Cpu readMsr: n` …: `op` is the selector
};

// How a send node is implemented. `operandType` is the type every operand is
// converted to before the operation; `op` is the operator ("+", "<") for
// arithmetic-like kinds.
struct SendInfo {
    SendKind kind = SendKind::staticCall;
    std::string op;
    const Type* operandType = nullptr;
    const Type* resultType = nullptr;
    OverflowMode overflow = OverflowMode::trap;
    const MessageInfo* target = nullptr;
    const RecordInfo* record = nullptr;
    const ClassInfo* classInfo = nullptr;
    const FieldInfo* field = nullptr;
    int slot = -1;
    // constant: the value's bits. enum sends: the enum.
    uint64_t constant = 0;
    const EnumInfo* enumInfo = nullptr;
    std::vector<const FieldInfo*> fields;
    const ConstValue* value = nullptr;
    // `perform:`: the type each `with:` argument is passed as.
    std::vector<const Type*> argumentTypes;
    // `x address` of a local (or a field of one): the pointer must not outlive the frame.
    bool frameBound = false;
};

// One case label as an inclusive range of the subject's values, stored as
// two's-complement bit patterns and compared with the subject's signedness.
struct CaseRange {
    uint64_t low = 0;
    uint64_t high = 0;
};

// All results are keyed by AST node id so tools can query them for any node.
// An imported module or lib, read from its interface file.
struct ModuleInfo {
    std::string name;
    std::string version;
    std::string fingerprint;
    // Its interface says it acts through reflection (`@[reflector: "needed"]`).
    bool needsReflector = false;
    std::string interfacePath;
    // The compiled code next to the interface (`.lib`, else `.obj`); empty if missing.
    std::string libraryPath;
    // The modules this one was compiled against, with their interfaces' fingerprints.
    std::vector<std::pair<std::string, std::string>> dependencies;
    ProgramInfo program;
    Type* type = nullptr;
    std::string text;
    std::unique_ptr<parser::ParsedUnit> parsed;
};

class SemanticModel {
public:
    TypeTable types;
    ProgramInfo program;
    std::vector<std::unique_ptr<RecordInfo>> records;
    std::vector<std::unique_ptr<MessageInfo>> messages;
    std::vector<std::unique_ptr<LocalInfo>> locals;
    std::vector<std::unique_ptr<BlockInfo>> blockInfos;
    std::vector<std::unique_ptr<ClassInfo>> classes;
    std::vector<std::unique_ptr<EnumInfo>> enums;
    std::vector<std::unique_ptr<NamedTypeInfo>> namedTypes;

    std::unordered_map<ast::NodeId, const Type*> expressionTypes;
    std::unordered_map<ast::NodeId, const BlockInfo*> blocks;
    std::unordered_map<ast::NodeId, const LocalInfo*> localRefs;
    std::unordered_map<ast::NodeId, SendInfo> sends;
    std::unordered_map<ast::NodeId, const FieldInfo*> fieldRefs;
    // Bare identifiers inside instance messages that name a field of self.
    std::unordered_map<ast::NodeId, const FieldInfo*> selfFieldRefs;
    std::unordered_map<ast::NodeId, std::vector<CaseRange>> caseRanges;
    std::unordered_set<ast::NodeId> implicitDeclarations;
    // `case` statements that cover every value of their subject (no `else` needed).
    std::unordered_set<ast::NodeId> exhaustiveCases;
    // `x: C;` declarations of class type: construct a C with every field defaulted.
    std::unordered_map<ast::NodeId, const ClassInfo*> defaultObjects;
    // Allocation statements whose object goes on the stack, keyed by the statement.
    std::unordered_map<ast::NodeId, StackObject> stackObjects;
    std::vector<std::unique_ptr<ConstantInfo>> constants;
    // Imported modules, in load order (dependencies first).
    std::vector<std::unique_ptr<ModuleInfo>> modules;
    std::vector<std::unique_ptr<CompileTimeItem>> compileTimeItems;
    std::vector<std::unique_ptr<ConstValue>> constValues;
    // Identifiers that name a unit-level constant.
    std::unordered_map<ast::NodeId, ConstantInfo*> constantRefs;
    // `@[compiletime] (expr)` and `@[compiletime] { }` nodes.
    std::unordered_map<ast::NodeId, CompileTimeItem*> compileTimeNodes;
    // The classes `meta fields` / `c fields` and `c messages` return instances of, created on first use.
    ClassInfo* fieldMetaClass = nullptr;
    ClassInfo* messageMetaClass = nullptr;
    // `CpuidResult { eax; ebx; ecx; edx }`, what `Cpu cpuid:` returns; kept out of `records`
    // so it never appears in an interface.
    std::unique_ptr<RecordInfo> cpuidRecord;
    // The unit, or a module it imports, acts through reflection (`perform:`,
    // `set:of:to:`, `new`, `Class named:`), so its program's main needs a Reflector.
    bool needsReflector = false;
    // Locals that only ever hold stack objects (or nil): no retain/release on them.
    std::unordered_set<const LocalInfo*> stackOnlyLocals;
    // Block functions whose environment lives in the creating frame, because the
    // block provably doesn't outlive it. Every other block is a heap closure.
    std::unordered_set<const BlockInfo*> stackBlocks;
    // Why a block analysed in a message body became a heap closure (for tools).
    std::unordered_map<const BlockInfo*, std::string> blockEscapes;
    // Mutable variables captured by a heap closure: they live in a heap box that
    // the frame and the closures share, so every one sees the same variable.
    std::unordered_set<const LocalInfo*> boxedLocals;
    // Capture-free block literals that become plain C functions (design §6.6).
    std::unordered_set<const BlockInfo*> cFunctionBlocks;

    const Type* typeOf(const ast::Node* node) const;
    const LocalInfo* localOf(const ast::Node* node) const;
    const SendInfo* sendOf(const ast::Node* node) const;
    const FieldInfo* fieldOf(const ast::Node* node) const;
    const BlockInfo* blockOf(const ast::Node* node) const;
    const RecordInfo* recordNamed(std::string_view name) const;
    ClassInfo* classNamed(std::string_view name) const;
    ConstantInfo* constantNamed(std::string_view name) const;
    ModuleInfo* moduleNamed(std::string_view name) const;
    EnumInfo* enumNamed(std::string_view name) const;
    const NamedTypeInfo* namedType(std::string_view name) const;
};

}
