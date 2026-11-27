#include "layout.h"

#include "semanticModel.h"

#include <algorithm>

namespace bob::sema {

namespace {

uint64_t roundUp(uint64_t value, uint64_t alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

struct Aggregate {
    uint64_t size = 0;
    uint64_t alignment = 1;

    void add(uint64_t fieldSize, uint64_t fieldAlignment, bool packed, uint64_t* offset = nullptr)
    {
        if (!packed) {
            size = roundUp(size, fieldAlignment);
            alignment = std::max(alignment, fieldAlignment);
        }
        if (offset != nullptr) {
            *offset = size;
        }
        size += fieldSize;
    }

    uint64_t total() const { return roundUp(size, alignment); }
};

Aggregate recordLayout(const RecordInfo* record, size_t upTo, uint64_t* offset)
{
    Aggregate layout;
    for (size_t i = 0; i < record->fields.size(); ++i) {
        const Type* field = record->fields[i].type;
        layout.add(sizeOf(field), record->packed ? 1 : alignmentOf(field), record->packed, i == upTo ? offset : nullptr);
    }
    return layout;
}

Aggregate objectLayout(const ClassInfo* info, size_t upTo, uint64_t* offset)
{
    Aggregate layout;
    layout.add(8, 8, false);  // isa
    layout.add(4, 4, false);  // refCount
    layout.add(4, 4, false);  // flags
    for (size_t i = 0; i < info->fields.size(); ++i) {
        const Type* field = info->fields[i].type;
        layout.add(sizeOf(field), alignmentOf(field), false, i == upTo ? offset : nullptr);
    }
    return layout;
}

}

uint64_t sizeOf(const Type* type)
{
    type = canonical(type);
    switch (type->kind) {
    case TypeKind::boolType:
        return 1;
    case TypeKind::integer:
    case TypeKind::real:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
        return type->bits / 8;
    case TypeKind::set:
        return isBitSet(type) ? type->bits / 8 : 8;
    case TypeKind::record:
        return recordLayout(type->record, SIZE_MAX, nullptr).total();
    case TypeKind::fixedArray:
        return type->count * sizeOf(type->inner);
    case TypeKind::optional:
        if (isReference(type)) {
            return 8;
        }
        {
            Aggregate layout;
            layout.add(1, 1, false);
            layout.add(sizeOf(type->inner), alignmentOf(type->inner), false);
            return layout.total();
        }
    case TypeKind::block:
        return 16;
    default:
        return 8; // references, strings and containers are one pointer
    }
}

uint64_t alignmentOf(const Type* type)
{
    type = canonical(type);
    switch (type->kind) {
    case TypeKind::boolType:
        return 1;
    case TypeKind::integer:
    case TypeKind::real:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
        return type->bits / 8;
    case TypeKind::set:
        return isBitSet(type) ? type->bits / 8 : 8;
    case TypeKind::record:
        return type->record->packed ? 1 : recordLayout(type->record, SIZE_MAX, nullptr).alignment;
    case TypeKind::fixedArray:
        return alignmentOf(type->inner);
    case TypeKind::optional:
        return isReference(type) ? 8 : alignmentOf(type->inner);
    default:
        return 8;
    }
}

uint64_t fieldOffset(const RecordInfo* record, size_t index)
{
    uint64_t offset = 0;
    recordLayout(record, index, &offset);
    return offset;
}

uint64_t objectSize(const ClassInfo* info)
{
    return objectLayout(info, SIZE_MAX, nullptr).total();
}

uint64_t objectFieldOffset(const ClassInfo* info, size_t index)
{
    uint64_t offset = 0;
    objectLayout(info, index, &offset);
    return offset;
}

}
