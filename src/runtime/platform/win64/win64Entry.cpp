#include "bobRuntime.h"
#include "win64Platform.h"

#include <Windows.h>

extern "C" void bobStart()
{
    bob::runtime::win64::consoleInit();
    int32_t result = bobMain();
    ExitProcess(static_cast<UINT>(result));
}
