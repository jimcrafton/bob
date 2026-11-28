#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"
#include "runtimeInternal.h"

#include <Windows.h>
#include <intrin.h>

namespace {

volatile long long liveObjects = 0;
volatile long long liveBytes = 0;

struct CacheEntry {
    const char* selector;
    void* imp;
};

}

struct BobMethodCache {
    uint64_t mask;
    CacheEntry entries[1];
};

namespace {

constexpr uint64_t cacheCapacity = 64;

void writeError(const char* text)
{
    uint64_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    bobRuntimeWrite(static_cast<int32_t>(bob::abi::StreamId::error), reinterpret_cast<const uint8_t*>(text), length);
}

void writeClassName(const BobClass* cls)
{
    if (cls->name == nullptr) {
        writeError("(unnamed class: reflect level 'none')");
        return;
    }
    bobRuntimeWrite(static_cast<int32_t>(bob::abi::StreamId::error), cls->name->bytes, cls->name->byteCount);
}

bool sameText(const char* text, const BobString* string)
{
    uint64_t count = string != nullptr ? string->byteCount : 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (text[i] == '\0' || static_cast<uint8_t>(text[i]) != string->bytes[i]) {
            return false;
        }
    }
    return text[count] == '\0';
}

void writeNumber(long long value)
{
    char digits[24];
    int count = 0;
    unsigned long long magnitude = value < 0 ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0 && count < 23);
    char text[26];
    int length = 0;
    if (value < 0) {
        text[length++] = '-';
    }
    while (count > 0) {
        text[length++] = digits[--count];
    }
    text[length] = '\0';
    writeError(text);
}

volatile long* flagsOf(BobObjectHeader* header)
{
    return reinterpret_cast<volatile long*>(&header->flags);
}

void freeObject(BobObjectHeader* header)
{
    BobClass* cls = header->isa;
    _InterlockedDecrement64(&liveObjects);
    uint64_t size = cls->instanceSize != 0 ? cls->instanceSize : HeapSize(GetProcessHeap(), 0, header);
    _InterlockedExchangeAdd64(&liveBytes, -static_cast<long long>(size));
    HeapFree(GetProcessHeap(), 0, header);
}

BobMethodCache* cacheOf(BobClass* cls)
{
    if (cls->cache == nullptr) {
        uint64_t size = sizeof(BobMethodCache) + (cacheCapacity - 1) * sizeof(CacheEntry);
        auto* cache = static_cast<BobMethodCache*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size));
        cache->mask = cacheCapacity - 1;
        cls->cache = cache;
    }
    return cls->cache;
}

}

extern "C" {

void* bobAllocObject(BobClass* cls)
{
    auto* header = static_cast<BobObjectHeader*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cls->instanceSize));
    if (header == nullptr) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::outOfMemory));
    }
    header->isa = cls;
    header->refCount = 1;
    _InterlockedIncrement64(&liveObjects);
    _InterlockedExchangeAdd64(&liveBytes, static_cast<long long>(cls->instanceSize));
    return header;
}

// Strings, arrays and tables have a runtime class with instanceSize 0: their size
// varies, so it comes from the heap.
void* bobAllocBuffer(BobClass* cls, uint64_t size)
{
    auto* header = static_cast<BobObjectHeader*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size));
    if (header == nullptr) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::outOfMemory));
    }
    header->isa = cls;
    header->refCount = 1;
    _InterlockedIncrement64(&liveObjects);
    _InterlockedExchangeAdd64(&liveBytes, static_cast<long long>(size));
    return header;
}

void* bobGrowBuffer(void* buffer, uint64_t size)
{
    uint64_t old = HeapSize(GetProcessHeap(), 0, buffer);
    void* grown = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, buffer, size);
    if (grown == nullptr) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::outOfMemory));
    }
    _InterlockedExchangeAdd64(&liveBytes, static_cast<long long>(size) - static_cast<long long>(old));
    return grown;
}

void bobRetain(void* object)
{
    if (object != nullptr && (static_cast<BobObjectHeader*>(object)->flags & bobObjectImmortal) == 0) {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(&static_cast<BobObjectHeader*>(object)->refCount));
    }
}

void bobRelease(void* object)
{
    if (object == nullptr) {
        return;
    }
    auto* header = static_cast<BobObjectHeader*>(object);
    if ((header->flags & bobObjectImmortal) != 0) {
        return;
    }
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&header->refCount)) != 0) {
        return;
    }
    // Hold the memory while destroying: the object's own fields may drop the
    // last weak reference to it.
    _InterlockedExchangeAdd(flagsOf(header), static_cast<long>(bobWeakCountUnit));
    _InterlockedOr(flagsOf(header), static_cast<long>(bobObjectDestroyed));
    BobClass* cls = header->isa;
    if (cls->destroy != nullptr) {
        cls->destroy(object);
    }
    bobWeakRelease(object);
}

void bobWeakRetain(void* object)
{
    if (object != nullptr && (static_cast<BobObjectHeader*>(object)->flags & bobObjectImmortal) == 0) {
        _InterlockedExchangeAdd(flagsOf(static_cast<BobObjectHeader*>(object)), static_cast<long>(bobWeakCountUnit));
    }
}

void bobWeakRelease(void* object)
{
    if (object == nullptr) {
        return;
    }
    auto* header = static_cast<BobObjectHeader*>(object);
    if ((header->flags & bobObjectImmortal) != 0) {
        return;
    }
    long after = _InterlockedExchangeAdd(flagsOf(header), -static_cast<long>(bobWeakCountUnit)) - static_cast<long>(bobWeakCountUnit);
    if ((static_cast<uint32_t>(after) >> bobWeakCountShift) == 0 && (after & bobObjectDestroyed) != 0) {
        freeObject(header);
    }
}

void* bobWeakLoad(void* object)
{
    if (object == nullptr) {
        return nullptr;
    }
    auto* header = static_cast<BobObjectHeader*>(object);
    if ((header->flags & bobObjectImmortal) != 0) {
        return object;
    }
    // Retain only while the count is above 0: once it reaches 0 the object is gone for good.
    auto* count = reinterpret_cast<volatile long*>(&header->refCount);
    long seen = *count;
    while (seen != 0) {
        long previous = _InterlockedCompareExchange(count, seen + 1, seen);
        if (previous == seen) {
            return object;
        }
        seen = previous;
    }
    return nullptr;
}

void* bobUnownedLoad(void* object)
{
    void* strong = bobWeakLoad(object);
    if (strong == nullptr && object != nullptr) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::unownedDestroyed));
    }
    return strong;
}

bool bobObjectIsOnStack(void* object)
{
    return object != nullptr && (static_cast<BobObjectHeader*>(object)->flags & bobObjectStack) != 0;
}

void* const* bobWitness(const BobClass* cls, const void* protocol)
{
    for (uint32_t i = 0; i < cls->conformanceCount; ++i) {
        if (cls->conformances[i].protocol == protocol) {
            return cls->conformances[i].witness;
        }
    }
    writeError("bob: '");
    writeClassName(cls);
    writeError("' doesn't conform to '");
    writeError(static_cast<const BobProtocol*>(protocol)->name);
    writeError("'\n");
    bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::doesNotUnderstand));
}

// Slow path of bob_msgSend: walk the class chain for the selector (selectors
// are unique pointers), remember the result in the receiver class's cache.
void* bobMsgSendLookup(BobClass* cls, const char* selector)
{
    for (BobClass* current = cls; current != nullptr; current = current->superclass) {
        for (uint32_t i = 0; i < current->methodCount; ++i) {
            if (current->methods[i].selector == selector) {
                void* imp = current->methods[i].imp;
                if (imp == nullptr) {
                    writeError("bob: '");
                    writeClassName(cls);
                    writeError("' can't be sent '");
                    writeError(selector);
                    writeError("' by reflection; declare it @[reflect: invoke]\n");
                    bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::doesNotUnderstand));
                }
                BobMethodCache* cache = cacheOf(cls);
                uint64_t slot = (reinterpret_cast<uint64_t>(selector) >> 3) & cache->mask;
                bool stored = false;
                for (uint64_t probe = 0; probe < cache->mask && !stored; ++probe) {
                    CacheEntry& entry = cache->entries[(slot + probe) & cache->mask];
                    if (entry.selector == nullptr || entry.selector == selector) {
                        entry.imp = imp;
                        entry.selector = selector;
                        stored = true;
                    }
                }
                // Keep at least one empty entry so the probe loop in msgSend.asm terminates.
                if (!stored) {
                    for (uint64_t k = 0; k <= cache->mask; ++k) {
                        cache->entries[k].selector = nullptr;
                        cache->entries[k].imp = nullptr;
                    }
                    cache->entries[slot].selector = selector;
                    cache->entries[slot].imp = imp;
                }
                return imp;
            }
        }
    }
    writeError("bob: '");
    writeClassName(cls);
    writeError("' doesn't understand '");
    writeError(selector);
    writeError("'\n");
    bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::doesNotUnderstand));
}

bool bobClassIsKindOf(const BobClass* cls, const BobClass* other)
{
    for (const BobClass* current = cls; current != nullptr; current = current->superclass) {
        if (current == other) {
            return true;
        }
    }
    return false;
}

// By text, so a selector built at runtime works too; superclasses' messages count.
bool bobClassRespondsTo(const BobClass* cls, const BobString* selector)
{
    for (const BobClass* current = cls; current != nullptr; current = current->superclass) {
        for (uint32_t i = 0; i < current->methodCount; ++i) {
            if (sameText(current->methods[i].selector, selector)) {
                return true;
            }
        }
    }
    return false;
}

namespace {

bool sameString(const BobString* a, const BobString* b)
{
    uint64_t count = a != nullptr ? a->byteCount : 0;
    if (count != (b != nullptr ? b->byteCount : 0)) {
        return false;
    }
    return count == 0 || bob::runtime::sameBytes(a->bytes, b->bytes, count);
}

void* metaObject(BobClass* metaClass, const void* const* values, uint32_t count)
{
    void* object = bobAllocObject(metaClass);
    auto slots = reinterpret_cast<const void**>(static_cast<uint8_t*>(object) + bob::abi::metaFirstFieldOffset);
    for (uint32_t i = 0; i < count; ++i) {
        slots[i] = values[i];
    }
    return object;
}

void appendFields(const BobClass* cls, void** array, BobClass* metaClass, const BobElementType* metaElement)
{
    if (cls == nullptr) {
        return;
    }
    appendFields(cls->superclass, array, metaClass, metaElement);
    if (cls->meta == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < cls->meta->fieldCount; ++i) {
        const BobFieldEntry& field = cls->meta->fields[i];
        const void* values[] = {field.name, field.typeName, reinterpret_cast<const void*>(field.offset), reinterpret_cast<const void*>(field.size)};
        void* object = metaObject(metaClass, values, 4);
        bobArrayPush(array, &object, metaElement);
    }
}

void appendMessages(const BobClass* cls, void** array, BobClass* metaClass, const BobElementType* metaElement, const BobElementType* stringElement)
{
    if (cls == nullptr) {
        return;
    }
    appendMessages(cls->superclass, array, metaClass, metaElement, stringElement);
    if (cls->meta == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < cls->meta->messageCount; ++i) {
        const BobMessageEntry& message = cls->meta->messages[i];
        void* argumentTypes = nullptr;
        for (uint64_t k = 0; k < message.argumentCount; ++k) {
            bobArrayPush(&argumentTypes, &message.argumentTypes[k], stringElement);
        }
        const void* values[] = {message.selector, argumentTypes, message.resultType};
        void* object = metaObject(metaClass, values, 3);
        bobArrayPush(array, &object, metaElement);
    }
}

[[noreturn]] void fieldMismatch(const BobClass* cls, const BobString* name, const char* problem)
{
    writeError("bob: reflection: '");
    writeClassName(cls);
    writeError(" ");
    if (name != nullptr) {
        bobRuntimeWrite(static_cast<int32_t>(bob::abi::StreamId::error), name->bytes, name->byteCount);
    }
    writeError("' ");
    writeError(problem);
    writeError("\n");
    bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::reflectionMismatch));
}

}

void* bobClassFields(const BobClass* cls, BobClass* metaClass, const BobElementType* metaElement)
{
    void* array = nullptr;
    appendFields(cls, &array, metaClass, metaElement);
    return array;
}

void* bobClassMessages(const BobClass* cls, BobClass* metaClass, const BobElementType* metaElement, const BobElementType* stringElement)
{
    void* array = nullptr;
    appendMessages(cls, &array, metaClass, metaElement, stringElement);
    return array;
}

void* bobClassProtocols(const BobClass* cls, const BobElementType* stringElement)
{
    void* array = nullptr;
    if (cls->meta != nullptr) {
        for (uint64_t i = 0; i < cls->meta->protocolCount; ++i) {
            bobArrayPush(&array, &cls->meta->protocols[i], stringElement);
        }
    }
    return array;
}

namespace {

constexpr uint32_t invokeLevel = 3;

void requireKindOf(const BobClass* cls, const void* object)
{
    if (!bobClassIsKindOf(static_cast<const BobObjectHeader*>(object)->isa, cls)) {
        writeError("bob: reflection: the object isn't a '");
        writeClassName(cls);
        writeError("'\n");
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::reflectionMismatch));
    }
}

}

uint64_t bobClassFieldOffset(const BobClass* cls, const void* object, const BobString* name, const BobString* typeKey, uint32_t write)
{
    requireKindOf(cls, object);
    for (const BobClass* current = cls; current != nullptr; current = current->superclass) {
        if (current->meta == nullptr) {
            continue;
        }
        for (uint64_t i = 0; i < current->meta->fieldCount; ++i) {
            const BobFieldEntry& field = current->meta->fields[i];
            if (!sameString(field.name, name)) {
                continue;
            }
            if (!sameString(field.typeKey, typeKey)) {
                fieldMismatch(cls, name, "has a different type");
            }
            if (field.ownership != 0) {
                fieldMismatch(cls, name, "is weak or unowned and can't be read or written by reflection");
            }
            if (write != 0 && current->meta->level < invokeLevel) {
                fieldMismatch(cls, name, "can't be written by reflection; declare its class @[reflect: invoke]");
            }
            return field.offset;
        }
    }
    fieldMismatch(cls, name, "isn't a public field (or the class is below @[reflect: introspect])");
}

void* bobClassMethod(const BobClass* cls, const void* object, const BobString* selector, const BobString* signatureKey)
{
    requireKindOf(cls, object);
    // From the object's own class, so an override is the one that runs.
    for (const BobClass* current = static_cast<const BobObjectHeader*>(object)->isa; current != nullptr; current = current->superclass) {
        if (current->meta == nullptr) {
            continue;
        }
        for (uint64_t i = 0; i < current->meta->messageCount; ++i) {
            const BobMessageEntry& message = current->meta->messages[i];
            if (!sameString(message.selector, selector)) {
                continue;
            }
            if (!sameString(message.signatureKey, signatureKey)) {
                fieldMismatch(current, selector, "has a different signature than the call");
            }
            if (message.imp == nullptr) {
                fieldMismatch(current, selector, "can't be performed by reflection; declare its class @[reflect: invoke]");
            }
            return message.imp;
        }
    }
    fieldMismatch(static_cast<const BobObjectHeader*>(object)->isa, selector, "isn't a message the object understands");
}

void* bobClassNew(const BobClass* cls, const BobClass* expectedClass, const void* expectedProtocol)
{
    if (cls->meta == nullptr || cls->meta->create == nullptr) {
        writeError("bob: reflection: '");
        writeClassName(cls);
        writeError("' can't be created by reflection: it needs @[reflect: invoke] and a default for every field\n");
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::reflectionMismatch));
    }
    bool fits = expectedProtocol == nullptr ? bobClassIsKindOf(cls, expectedClass) : false;
    for (uint32_t i = 0; expectedProtocol != nullptr && i < cls->conformanceCount; ++i) {
        fits = fits || cls->conformances[i].protocol == expectedProtocol;
    }
    if (!fits) {
        writeError("bob: reflection: a new '");
        writeClassName(cls);
        writeError("' isn't the type the call expects\n");
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::reflectionMismatch));
    }
    return cls->meta->create();
}

// The registry's brackets (see bob::abi::classRegistrySection). The linker sorts
// `.bobcls$…` contributions by suffix and may pad between them with zeros.
#pragma section(".bobcls$a", read)
#pragma section(".bobcls$z", read)
extern "C" __declspec(allocate(".bobcls$a")) const BobClass* const bobClassRegistryStart[] = {nullptr};
extern "C" __declspec(allocate(".bobcls$z")) const BobClass* const bobClassRegistryEnd[] = {nullptr};

const BobClass* bobClassNamed(const BobString* name)
{
    const BobClass* const* first = bobClassRegistryStart + 1;
    const BobClass* const* last = bobClassRegistryEnd;
    for (const BobClass* const* entry = first; entry < last; ++entry) {
        if (*entry != nullptr && (*entry)->meta != nullptr && sameString((*entry)->meta->qualifiedName, name)) {
            return *entry;
        }
    }
    const BobClass* found = nullptr;
    for (const BobClass* const* entry = first; entry < last; ++entry) {
        if (*entry == nullptr || !sameString((*entry)->name, name)) {
            continue;
        }
        if (found != nullptr && found != *entry) {
            return nullptr;
        }
        found = *entry;
    }
    return found;
}

void bobRuntimeReportLeaks()
{
    long long objects = liveObjects;
    if (objects == 0) {
        return;
    }
    writeError("bob: leak: ");
    writeNumber(objects);
    writeError(" object(s), ");
    writeNumber(liveBytes);
    writeError(" bytes still allocated at exit\n");
}

}
