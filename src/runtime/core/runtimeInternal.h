#pragma once

#include <cstdint>

struct BobString;
struct BobElementType;

// Helpers shared by the runtime's own source files (not called by generated code).
namespace bob::runtime {

void copyBytes(void* dest, const void* src, uint64_t count);
bool sameBytes(const void* a, const void* b, uint64_t count);
const uint8_t* stringBytes(void* s);
uint64_t stringCount(void* s);
BobString* newString(uint64_t count);

// Element callbacks for containers of strings or other single-pointer values.
void retainPointer(void* element);
void releasePointer(void* element);

// Equality and hashing of one element, by its descriptor's `kind`.
bool elementsEqual(const BobElementType* type, const void* a, const void* b);
uint64_t elementHash(const BobElementType* type, const void* element);

}
