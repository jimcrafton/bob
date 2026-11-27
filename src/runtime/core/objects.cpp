#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"

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
        writeError("bob: out of memory\n");
        ExitProcess(static_cast<UINT>(bob::abi::trapExitCode));
    }
    header->isa = cls;
    header->refCount = 1;
    _InterlockedIncrement64(&liveObjects);
    _InterlockedExchangeAdd64(&liveBytes, static_cast<long long>(cls->instanceSize));
    return header;
}

void bobRetain(void* object)
{
    if (object != nullptr) {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(&static_cast<BobObjectHeader*>(object)->refCount));
    }
}

void bobRelease(void* object)
{
    if (object == nullptr) {
        return;
    }
    auto* header = static_cast<BobObjectHeader*>(object);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&header->refCount)) != 0) {
        return;
    }
    BobClass* cls = header->isa;
    if (cls->destroy != nullptr) {
        cls->destroy(object);
    }
    _InterlockedDecrement64(&liveObjects);
    _InterlockedExchangeAdd64(&liveBytes, -static_cast<long long>(cls->instanceSize));
    HeapFree(GetProcessHeap(), 0, object);
}

// Slow path of bob_msgSend: walk the class chain for the selector (selectors
// are unique pointers), remember the result in the receiver class's cache.
void* bobMsgSendLookup(BobClass* cls, const char* selector)
{
    for (BobClass* current = cls; current != nullptr; current = current->superclass) {
        for (uint32_t i = 0; i < current->methodCount; ++i) {
            if (current->methods[i].selector == selector) {
                void* imp = current->methods[i].imp;
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
    writeError("bob: runtime trap: '");
    writeError(cls->name);
    writeError("' doesn't understand '");
    writeError(selector);
    writeError("'\n");
    ExitProcess(static_cast<UINT>(bob::abi::trapExitCode));
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
