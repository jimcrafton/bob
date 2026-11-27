#pragma once

#include <cstdint>

struct BobClass;
struct BobElementType;

extern "C" {

// The program's `main:`; `args` is an `array of: string` (borrowed).
int32_t bobMain(void* args);

void bobStart();

void bobRuntimeWrite(int32_t stream, const uint8_t* bytes, uint64_t count);

[[noreturn]] void bobRuntimeTrap(int32_t reason);

void* bobAllocObject(BobClass* cls);
void bobRetain(void* object);
void bobRelease(void* object);
// Weak and unowned references count separately: they keep a destroyed object's
// memory (not its fields) until the last of them is gone.
void bobWeakRetain(void* object);
void bobWeakRelease(void* object);
// The object behind a weak reference, retained (+1), or null once it's destroyed.
void* bobWeakLoad(void* object);
// The same for an unowned reference, which traps once the object is destroyed.
void* bobUnownedLoad(void* object);
// Whether the object lives in a stack frame (placed there by escape analysis).
bool bobObjectIsOnStack(void* object);
void* bobMsgSendLookup(BobClass* cls, const char* selector);
// The witness table of `cls` for `protocol` (a BobProtocol). Reads only
// immutable data; a class that doesn't conform traps (well-typed code can't).
void* const* bobWitness(const BobClass* cls, const void* protocol);
void bob_msgSend();
void bobRuntimeReportLeaks();

// Runtime-owned buffers (strings, arrays, tables): counted in the leak report.
void* bobAllocBuffer(BobClass* cls, uint64_t size);
void* bobGrowBuffer(void* buffer, uint64_t size);

// Strings. A null string is the empty string. Results are owned (+1).
void* bobStringFromBytes(const uint8_t* bytes, uint64_t count);
void* bobStringConcat(void* a, void* b);
int32_t bobStringCompare(void* a, void* b);
int64_t bobStringByteCount(void* s);
int64_t bobStringCodepointCount(void* s);
int64_t bobStringGraphemeCount(void* s);
uint8_t bobStringByteAt(void* s, int64_t index);
// Decodes the codepoint at *offset and advances it.
uint32_t bobStringDecodeAt(void* s, int64_t* offset);
// The end of the grapheme cluster starting at `offset`.
int64_t bobStringNextGrapheme(void* s, int64_t offset);
void* bobStringSlice(void* s, int64_t from, int64_t to);
// kind: 0 startsWith:, 1 endsWith:, 2 includesSubstring:
bool bobStringPredicate(void* s, void* t, int32_t kind);
void* bobStringSplit(void* s, void* separator);
void* bobStringJoin(void* separator, void* strings);
// Null and *ok = 0 when the bytes aren't valid UTF-8 (unless lossy).
void* bobStringFromUtf8(void* byteArray, int32_t lossy, int32_t* ok);
void* bobStringToUtf8(void* s);
void* bobStringFromInteger(int64_t value, int32_t isUnsigned);
void* bobStringFromReal(double value);
void* bobStringFromBool(bool value);
void* bobStringFromChar(uint32_t codepoint);

// Arrays. `slot` is the address of the variable or field holding the array.
void* bobArrayNew(const BobElementType* element, uint64_t capacity);
// Appends a copy of the element's bytes; ownership of the element moves in.
void bobArrayPush(void** slot, const void* element, const BobElementType* type);
// Makes the array in *slot unshared (copying it if needed) and returns its elements.
uint8_t* bobArrayMakeUnique(void** slot, const BobElementType* type);
// Moves the last element out into `out` (owned by the caller).
void bobArrayRemoveLast(void** slot, void* out, const BobElementType* type);
bool bobArrayIncludes(void* array, const void* element);

// Tables (dictionary, hashtable, set). Keys and values are moved in.
void* bobTableAt(void* table, const void* key);
void bobTableAtPut(void** slot, const void* key, const void* value, const BobElementType* keyType, const BobElementType* valueType);
bool bobTableRemove(void** slot, const void* key, const BobElementType* keyType, const BobElementType* valueType);
int64_t bobTableCount(void* table);
// The next occupied entry at or after `index`, or -1.
int64_t bobTableNext(void* table, int64_t index);
void* bobTableKeyAt(void* table, int64_t index);
void* bobTableValueAt(void* table, int64_t index);
void* bobTableKeys(void* table);
void* bobTableValues(void* table);

}
