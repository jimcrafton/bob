#pragma once

#include <cstdint>

extern "C" {

int32_t bobMain();

void bobStart();

void bobRuntimeWrite(int32_t stream, const uint8_t* bytes, uint64_t count);

[[noreturn]] void bobRuntimeTrap(int32_t reason);

struct BobClass;
void* bobAllocObject(BobClass* cls);
void bobRetain(void* object);
void bobRelease(void* object);
void* bobMsgSendLookup(BobClass* cls, const char* selector);
void bob_msgSend();
void bobRuntimeReportLeaks();

}
