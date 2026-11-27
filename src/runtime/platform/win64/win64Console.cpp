#include "bobRuntime.h"
#include "win64Platform.h"

#include <Windows.h>

namespace {

constexpr uint64_t consoleChunkBytes = 1024;

struct StreamState {
    HANDLE handle;
    bool isConsole;
};

StreamState outStream = {};
StreamState errorStream = {};

StreamState openStream(DWORD stdHandleId)
{
    StreamState state = {};
    state.handle = GetStdHandle(stdHandleId);
    DWORD mode = 0;
    state.isConsole = state.handle != nullptr && state.handle != INVALID_HANDLE_VALUE && GetConsoleMode(state.handle, &mode) != 0;
    return state;
}

uint64_t utf8ChunkEnd(const uint8_t* bytes, uint64_t start, uint64_t count)
{
    uint64_t end = start + consoleChunkBytes;
    if (end >= count) {
        return count;
    }
    while (end > start && (bytes[end] & 0xC0) == 0x80) {
        --end;
    }
    if (end == start) {
        return start + consoleChunkBytes;
    }
    return end;
}

void writeConsole(HANDLE handle, const uint8_t* bytes, uint64_t count)
{
    wchar_t wide[consoleChunkBytes];
    uint64_t start = 0;
    while (start < count) {
        uint64_t end = utf8ChunkEnd(bytes, start, count);
        int wideCount = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes + start),
                                            static_cast<int>(end - start), wide, static_cast<int>(consoleChunkBytes));
        if (wideCount > 0) {
            DWORD written = 0;
            WriteConsoleW(handle, wide, static_cast<DWORD>(wideCount), &written, nullptr);
        }
        start = end;
    }
}

void writeBytes(HANDLE handle, const uint8_t* bytes, uint64_t count)
{
    while (count > 0) {
        DWORD request = count > 0x40000000ull ? 0x40000000u : static_cast<DWORD>(count);
        DWORD written = 0;
        if (WriteFile(handle, bytes, request, &written, nullptr) == 0 || written == 0) {
            return;
        }
        bytes += written;
        count -= written;
    }
}

}

namespace bob::runtime::win64 {

void consoleInit()
{
    outStream = openStream(STD_OUTPUT_HANDLE);
    errorStream = openStream(STD_ERROR_HANDLE);
}

}

extern "C" void bobRuntimeWrite(int32_t stream, const uint8_t* bytes, uint64_t count)
{
    const StreamState& state = stream == 2 ? errorStream : outStream;
    if (state.handle == nullptr || state.handle == INVALID_HANDLE_VALUE || bytes == nullptr || count == 0) {
        return;
    }
    if (state.isConsole) {
        writeConsole(state.handle, bytes, count);
    }
    else {
        writeBytes(state.handle, bytes, count);
    }
}
