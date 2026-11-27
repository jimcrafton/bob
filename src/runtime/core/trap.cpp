#include "abi.h"
#include "bobRuntime.h"

#include <Windows.h>

namespace {

void writeError(const char* text)
{
    uint64_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    bobRuntimeWrite(static_cast<int32_t>(bob::abi::StreamId::error), reinterpret_cast<const uint8_t*>(text), length);
}

const char* reasonText(int32_t reason)
{
    switch (static_cast<bob::abi::TrapReason>(reason)) {
    case bob::abi::TrapReason::integerOverflow:
        return "integer overflow";
    case bob::abi::TrapReason::divideByZero:
        return "division by zero";
    case bob::abi::TrapReason::conversionOutOfRange:
        return "conversion out of range";
    case bob::abi::TrapReason::shiftOutOfRange:
        return "shift amount out of range";
    case bob::abi::TrapReason::unreachable:
        return "unreachable code reached";
    }
    return "unknown trap";
}

}

extern "C" void bobRuntimeTrap(int32_t reason)
{
    writeError("bob: runtime trap: ");
    writeError(reasonText(reason));
    writeError("\n");
    ExitProcess(static_cast<UINT>(bob::abi::trapExitCode));
}
