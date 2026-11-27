// `dictionary`, `hashtable` and `set`: open addressing with linear probing.
// Like arrays they have value semantics: shared until changed, then copied.

#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"
#include "runtimeInternal.h"

namespace {

constexpr uint64_t emptyHash = 0;
constexpr uint64_t removedHash = 1;
// Entries live in a separate runtime buffer (counted in the leak report) after
// its 16-byte object header.
constexpr uint64_t entriesHeader = 16;

BobClass entriesClass = {nullptr, "table entries", 0, nullptr, nullptr, 0, 0, nullptr};

uint64_t padded(uint64_t size)
{
    return (size + 7) & ~uint64_t(7);
}

uint64_t entrySize(const BobTable* table)
{
    return 8 + padded(table->key->size) + (table->value != nullptr ? padded(table->value->size) : 0);
}

uint8_t* entryAt(const BobTable* table, uint64_t index)
{
    return table->entries + entriesHeader + index * entrySize(table);
}

uint64_t& hashOf(uint8_t* entry)
{
    return *reinterpret_cast<uint64_t*>(entry);
}

uint8_t* keyOf(uint8_t* entry)
{
    return entry + 8;
}

uint8_t* valueOf(const BobTable* table, uint8_t* entry)
{
    return entry + 8 + padded(table->key->size);
}

void forEachEntry(BobTable* table, void (*visit)(BobTable*, uint8_t*))
{
    for (uint64_t i = 0; i < table->capacity; ++i) {
        uint8_t* entry = entryAt(table, i);
        if (hashOf(entry) > removedHash) {
            visit(table, entry);
        }
    }
}

void releaseEntry(BobTable* table, uint8_t* entry)
{
    if (table->key->release != nullptr) {
        table->key->release(keyOf(entry));
    }
    if (table->value != nullptr && table->value->release != nullptr) {
        table->value->release(valueOf(table, entry));
    }
}

void retainEntry(BobTable* table, uint8_t* entry)
{
    if (table->key->retain != nullptr) {
        table->key->retain(keyOf(entry));
    }
    if (table->value != nullptr && table->value->retain != nullptr) {
        table->value->retain(valueOf(table, entry));
    }
}

void destroyTable(void* object)
{
    auto* table = static_cast<BobTable*>(object);
    forEachEntry(table, releaseEntry);
    bobRelease(table->entries);
}

uint8_t* newEntries(const BobTable* table, uint64_t capacity)
{
    return static_cast<uint8_t*>(bobAllocBuffer(&entriesClass, entriesHeader + capacity * entrySize(table)));
}

BobTable* newTable(const BobElementType* key, const BobElementType* value, uint64_t capacity)
{
    auto* table = static_cast<BobTable*>(bobAllocBuffer(&bobTableClass, sizeof(BobTable)));
    table->key = key;
    table->value = value;
    table->capacity = capacity;
    table->entries = newEntries(table, capacity);
    return table;
}

// The entry holding `key`, or the free entry where it would go.
uint8_t* probe(const BobTable* table, const void* key, uint64_t hash, bool& found)
{
    uint64_t mask = table->capacity - 1;
    uint8_t* firstFree = nullptr;
    for (uint64_t i = hash & mask;; i = (i + 1) & mask) {
        uint8_t* entry = entryAt(table, i);
        uint64_t h = hashOf(entry);
        if (h == emptyHash) {
            found = false;
            return firstFree != nullptr ? firstFree : entry;
        }
        if (h == removedHash) {
            firstFree = firstFree != nullptr ? firstFree : entry;
        }
        else if (h == hash && bob::runtime::elementsEqual(table->key, keyOf(entry), key)) {
            found = true;
            return entry;
        }
    }
}

uint64_t hashKey(const BobTable* table, const void* key)
{
    uint64_t hash = bob::runtime::elementHash(table->key, key);
    return hash <= removedHash ? hash + 2 : hash;
}

// Rebuilds the entries with `capacity` slots (dropping removed markers).
void rehash(BobTable* table, uint64_t capacity)
{
    uint8_t* old = table->entries;
    uint64_t oldCapacity = table->capacity;
    uint64_t size = entrySize(table);
    table->capacity = capacity;
    table->entries = newEntries(table, capacity);
    table->used = table->count;
    for (uint64_t i = 0; i < oldCapacity; ++i) {
        uint8_t* entry = old + entriesHeader + i * size;
        if (hashOf(entry) > removedHash) {
            bool found = false;
            uint8_t* target = probe(table, keyOf(entry), hashOf(entry), found);
            bob::runtime::copyBytes(target, entry, size);
        }
    }
    bobRelease(old);
}

BobTable* makeUnique(void** slot, const BobElementType* key, const BobElementType* value)
{
    auto* table = static_cast<BobTable*>(*slot);
    if (table == nullptr) {
        table = newTable(key, value, 8);
        *slot = table;
        return table;
    }
    if (table->header.refCount == 1) {
        return table;
    }
    BobTable* copy = newTable(key, value, table->capacity);
    bob::runtime::copyBytes(copy->entries + entriesHeader, table->entries + entriesHeader, table->capacity * entrySize(table));
    copy->count = table->count;
    copy->used = table->used;
    forEachEntry(copy, retainEntry);
    bobRelease(table);
    *slot = copy;
    return copy;
}

}

namespace bob::runtime {

void retainPointer(void* element)
{
    bobRetain(*static_cast<void**>(element));
}

void releasePointer(void* element)
{
    bobRelease(*static_cast<void**>(element));
}

bool elementsEqual(const BobElementType* type, const void* a, const void* b)
{
    if (type->kind == 1) {
        return bobStringCompare(*static_cast<void* const*>(a), *static_cast<void* const*>(b)) == 0;
    }
    return sameBytes(a, b, type->size);
}

// FNV-1a over the key's bytes (a string's contents for strings).
uint64_t elementHash(const BobElementType* type, const void* element)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(element);
    uint64_t count = type->size;
    if (type->kind == 1) {
        void* s = *static_cast<void* const*>(element);
        bytes = stringBytes(s);
        count = stringCount(s);
    }
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t i = 0; i < count; ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

}

extern "C" {

BobClass bobTableClass = {nullptr, "table", 0, destroyTable, nullptr, 0, 0, nullptr};

void* bobTableAt(void* object, const void* key)
{
    auto* table = static_cast<BobTable*>(object);
    if (table == nullptr || table->count == 0) {
        return nullptr;
    }
    bool found = false;
    uint8_t* entry = probe(table, key, hashKey(table, key), found);
    if (!found) {
        return nullptr;
    }
    return table->value != nullptr ? valueOf(table, entry) : keyOf(entry);
}

void bobTableAtPut(void** slot, const void* key, const void* value, const BobElementType* keyType, const BobElementType* valueType)
{
    BobTable* table = makeUnique(slot, keyType, valueType);
    if ((table->used + 1) * 4 > table->capacity * 3) {
        rehash(table, table->count * 2 + 2 > table->capacity ? table->capacity * 2 : table->capacity);
    }
    uint64_t hash = hashKey(table, key);
    bool found = false;
    uint8_t* entry = probe(table, key, hash, found);
    if (found) {
        // The table keeps its key; the incoming copy is dropped.
        if (keyType->release != nullptr) {
            keyType->release(const_cast<void*>(key));
        }
        if (valueType != nullptr) {
            if (valueType->release != nullptr) {
                valueType->release(valueOf(table, entry));
            }
            bob::runtime::copyBytes(valueOf(table, entry), value, valueType->size);
        }
        return;
    }
    if (hashOf(entry) == emptyHash) {
        ++table->used;
    }
    hashOf(entry) = hash;
    bob::runtime::copyBytes(keyOf(entry), key, keyType->size);
    if (valueType != nullptr) {
        bob::runtime::copyBytes(valueOf(table, entry), value, valueType->size);
    }
    ++table->count;
}

bool bobTableRemove(void** slot, const void* key, const BobElementType* keyType, const BobElementType* valueType)
{
    if (*slot == nullptr || bobTableAt(*slot, key) == nullptr) {
        return false;
    }
    BobTable* table = makeUnique(slot, keyType, valueType);
    bool found = false;
    uint8_t* entry = probe(table, key, hashKey(table, key), found);
    releaseEntry(table, entry);
    hashOf(entry) = removedHash;
    --table->count;
    if (table->count == 0) {
        bobRelease(table);
        *slot = nullptr;
    }
    return true;
}

int64_t bobTableCount(void* object)
{
    return object != nullptr ? static_cast<int64_t>(static_cast<BobTable*>(object)->count) : 0;
}

int64_t bobTableNext(void* object, int64_t index)
{
    auto* table = static_cast<BobTable*>(object);
    if (table == nullptr) {
        return -1;
    }
    for (uint64_t i = static_cast<uint64_t>(index); i < table->capacity; ++i) {
        if (hashOf(entryAt(table, i)) > removedHash) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

void* bobTableKeyAt(void* object, int64_t index)
{
    return keyOf(entryAt(static_cast<BobTable*>(object), static_cast<uint64_t>(index)));
}

void* bobTableValueAt(void* object, int64_t index)
{
    auto* table = static_cast<BobTable*>(object);
    return valueOf(table, entryAt(table, static_cast<uint64_t>(index)));
}

namespace {

void* collect(void* object, bool keys)
{
    auto* table = static_cast<BobTable*>(object);
    if (table == nullptr || table->count == 0) {
        return nullptr;
    }
    const BobElementType* type = keys ? table->key : table->value;
    void* array = bobArrayNew(type, table->count);
    for (int64_t i = bobTableNext(table, 0); i >= 0; i = bobTableNext(table, i + 1)) {
        uint8_t* item = static_cast<uint8_t*>(keys ? bobTableKeyAt(table, i) : bobTableValueAt(table, i));
        if (type->retain != nullptr) {
            type->retain(item);
        }
        bobArrayPush(&array, item, type);
    }
    return array;
}

}

void* bobTableKeys(void* table)
{
    return collect(table, true);
}

void* bobTableValues(void* table)
{
    return collect(table, false);
}

}
