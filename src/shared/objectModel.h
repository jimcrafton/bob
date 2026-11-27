#pragma once

#include <cstdint>

// Layouts shared by the runtime (C++) and generated code. The code generator
// builds matching LLVM types in codegen/codeGenerator.cpp; keep them in step.
extern "C" {

// Every object starts with this header; fields follow at offset 16.
struct BobObjectHeader {
    struct BobClass* isa;
    uint32_t refCount;
    uint32_t flags;
};

// `flags` bits.
enum : uint32_t {
    // Compile-time constants (string literals) and stack objects: never retained,
    // released or freed.
    bobObjectImmortal = 1,
    // Lives in a stack frame (an object placed by escape analysis, or a block's environment).
    bobObjectStack = 2,
    // The object was destroyed (its count reached 0) but weak or unowned
    // references keep its memory until the last of them lets go.
    bobObjectDestroyed = 4,
};
// The upper bits of `flags` count weak and unowned references.
inline constexpr uint32_t bobWeakCountShift = 8;
inline constexpr uint32_t bobWeakCountUnit = 1u << bobWeakCountShift;

struct BobMethodEntry {
    const char* selector;
    void* imp;
};

struct BobMethodCache;

// One per protocol, emitted by every module that uses it as a link-once global,
// so its address is its identity program-wide (like selectors).
struct BobProtocol {
    const char* name;
    // Witness slots: refined protocols' messages first, then the protocol's own.
    uint64_t messageCount;
};

// A class's witness table for one protocol it conforms to (directly, through
// refinement or through a superclass): one implementation per protocol slot.
struct BobConformance {
    const BobProtocol* protocol;
    void* const* witness;
};

// One per class, emitted by the compiler as a mutable global. The vtable (one
// implementation pointer per slot) follows the fixed part.
struct BobClass {
    BobClass* superclass;
    const char* name;
    uint64_t instanceSize;
    void (*destroy)(void* object);
    const BobMethodEntry* methods;
    uint32_t methodCount;
    uint32_t conformanceCount;
    // Used only by bob_msgSend (reflection); protocol sends go through `conformances`.
    BobMethodCache* cache;
    const BobConformance* conformances;
    // void* vtable[];
};

// How generated code describes a container's element (or key/value) type to the
// runtime. `retain`/`release` take the element's address; both are null for
// plain data. `kind` picks equality and hashing: 0 = the raw bytes, 1 = string.
struct BobElementType {
    uint64_t size;
    void (*retain)(void* element);
    void (*release)(void* element);
    uint32_t kind;
    uint32_t reserved;
};

// A string is a pointer to this (null = the empty string). Its bytes are always
// valid UTF-8. Literals are immortal constants emitted by the compiler.
struct BobString {
    BobObjectHeader header;
    uint64_t byteCount;
    uint8_t bytes[1];
};

// `array`: a pointer to this (null = empty), copied on write when shared.
struct BobArray {
    BobObjectHeader header;
    uint64_t count;
    uint64_t capacity;
    const BobElementType* element;
    // elements follow at offset 40
};

// `dictionary`, `hashtable` and `set` (value == null): open addressing. Each
// entry is { hash: uint64 (0 = empty, 1 = removed), key, value } with key and
// value each padded to a multiple of 8 bytes.
struct BobTable {
    BobObjectHeader header;
    uint64_t count;
    uint64_t capacity;
    uint64_t used;
    const BobElementType* key;
    const BobElementType* value;
    uint8_t* entries;
};

extern BobClass bobStringClass;
extern BobClass bobArrayClass;
extern BobClass bobTableClass;

}

namespace bob::abi {

inline constexpr uint32_t objectHeaderSize = 16;
inline constexpr uint32_t classVtableOffset = 64;
inline constexpr uint32_t stringCountOffset = 16;
inline constexpr uint32_t stringBytesOffset = 24;
inline constexpr uint32_t arrayCountOffset = 16;
inline constexpr uint32_t arrayDataOffset = 40;

}
