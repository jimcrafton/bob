// `array`: a growable buffer with value semantics. Variables share one buffer
// (reference counted) until one of them changes it, which copies it first.

#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"
#include "runtimeInternal.h"

namespace {

uint8_t* dataOf(BobArray* array)
{
    return reinterpret_cast<uint8_t*>(array) + bob::abi::arrayDataOffset;
}

void destroyArray(void* object)
{
    auto* array = static_cast<BobArray*>(object);
    const BobElementType* type = array->element;
    if (type->release != nullptr) {
        for (uint64_t i = 0; i < array->count; ++i) {
            type->release(dataOf(array) + i * type->size);
        }
    }
}

uint64_t bytesFor(const BobElementType* type, uint64_t capacity)
{
    return bob::abi::arrayDataOffset + type->size * capacity;
}

}

extern "C" {

BobClass bobArrayClass = {nullptr, "array", 0, destroyArray, nullptr, 0, 0, nullptr};

void* bobArrayNew(const BobElementType* element, uint64_t capacity)
{
    if (capacity < 4) {
        capacity = 4;
    }
    auto* array = static_cast<BobArray*>(bobAllocBuffer(&bobArrayClass, bytesFor(element, capacity)));
    array->capacity = capacity;
    array->element = element;
    return array;
}

uint8_t* bobArrayMakeUnique(void** slot, const BobElementType* type)
{
    auto* array = static_cast<BobArray*>(*slot);
    if (array == nullptr) {
        array = static_cast<BobArray*>(bobArrayNew(type, 4));
        *slot = array;
        return dataOf(array);
    }
    if (array->header.refCount == 1 && (array->header.flags & bobObjectImmortal) == 0) {
        return dataOf(array);
    }
    auto* copy = static_cast<BobArray*>(bobArrayNew(type, array->count));
    bob::runtime::copyBytes(dataOf(copy), dataOf(array), array->count * type->size);
    copy->count = array->count;
    if (type->retain != nullptr) {
        for (uint64_t i = 0; i < copy->count; ++i) {
            type->retain(dataOf(copy) + i * type->size);
        }
    }
    bobRelease(array);
    *slot = copy;
    return dataOf(copy);
}

void bobArrayPush(void** slot, const void* element, const BobElementType* type)
{
    bobArrayMakeUnique(slot, type);
    auto* array = static_cast<BobArray*>(*slot);
    if (array->count == array->capacity) {
        uint64_t capacity = array->capacity * 2;
        array = static_cast<BobArray*>(bobGrowBuffer(array, bytesFor(type, capacity)));
        array->capacity = capacity;
        *slot = array;
    }
    bob::runtime::copyBytes(dataOf(array) + array->count * type->size, element, type->size);
    ++array->count;
}

void bobArrayRemoveLast(void** slot, void* out, const BobElementType* type)
{
    auto* array = static_cast<BobArray*>(*slot);
    if (array == nullptr || array->count == 0) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::indexOutOfRange));
    }
    uint8_t* data = bobArrayMakeUnique(slot, type);
    array = static_cast<BobArray*>(*slot);
    --array->count;
    bob::runtime::copyBytes(out, data + array->count * type->size, type->size);
    if (array->count == 0) {
        bobRelease(array);
        *slot = nullptr;
    }
}

bool bobArrayIncludes(void* object, const void* element)
{
    auto* array = static_cast<BobArray*>(object);
    if (array == nullptr) {
        return false;
    }
    for (uint64_t i = 0; i < array->count; ++i) {
        if (bob::runtime::elementsEqual(array->element, dataOf(array) + i * array->element->size, element)) {
            return true;
        }
    }
    return false;
}

}
