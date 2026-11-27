#pragma once

#include "types.h"

#include <cstdint>

// Memory layout of bob types on the x64 target, as the code generator lays
// them out (C rules: fields in order, each aligned to its own alignment, the
// whole rounded up to the largest). The semantic library has no LLVM, so it
// computes this itself; `Layout.MatchesTheCodeGenerator` checks the two agree.
namespace bob::sema {

struct ClassInfo;

uint64_t sizeOf(const Type* type);
uint64_t alignmentOf(const Type* type);
// Offset of field `index` within a record.
uint64_t fieldOffset(const RecordInfo* record, size_t index);
// An object: its 16-byte header, then the fields (inherited ones first).
uint64_t objectSize(const ClassInfo* info);
uint64_t objectFieldOffset(const ClassInfo* info, size_t index);

}
