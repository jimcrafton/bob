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

struct BobMethodEntry {
    const char* selector;
    void* imp;
};

struct BobMethodCache;

// One per class, emitted by the compiler as a mutable global. The vtable (one
// implementation pointer per slot) follows the fixed part.
struct BobClass {
    BobClass* superclass;
    const char* name;
    uint64_t instanceSize;
    void (*destroy)(void* object);
    const BobMethodEntry* methods;
    uint32_t methodCount;
    uint32_t reserved;
    BobMethodCache* cache;
    // void* vtable[];
};

}

namespace bob::abi {

inline constexpr uint32_t objectHeaderSize = 16;
inline constexpr uint32_t classVtableOffset = 56;

}
