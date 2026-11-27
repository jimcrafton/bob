#include "bobRuntime.h"
#include "objectModel.h"
#include "win64Platform.h"

#include <Windows.h>

namespace {

void releaseArg(void* element)
{
    bobRelease(*static_cast<void**>(element));
}

void retainArg(void* element)
{
    bobRetain(*static_cast<void**>(element));
}

const BobElementType argElement = {sizeof(void*), retainArg, releaseArg, 1, 0};

SIZE_T wideLength(const wchar_t* text)
{
    SIZE_T length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

void* utf8String(const wchar_t* wide, int count)
{
    if (count == 0) {
        return nullptr;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, count, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return nullptr;
    }
    char stackBuffer[512];
    char* buffer = bytes <= static_cast<int>(sizeof(stackBuffer)) ? stackBuffer
                                                                  : static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(bytes)));
    WideCharToMultiByte(CP_UTF8, 0, wide, count, buffer, bytes, nullptr, nullptr);
    void* s = bobStringFromBytes(reinterpret_cast<const uint8_t*>(buffer), static_cast<uint64_t>(bytes));
    if (buffer != stackBuffer) {
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return s;
}

// The arguments after the program name, split by the Windows convention
// (CommandLineToArgvW's rules: quotes group, 2n backslashes + quote -> n
// backslashes and a quote toggle, 2n+1 -> n and a literal quote).
void* commandLineArguments()
{
    const wchar_t* line = GetCommandLineW();
    void* args = nullptr;
    wchar_t* word = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (wideLength(line) + 1) * sizeof(wchar_t)));
    bool first = true;
    const wchar_t* p = line;
    while (*p != 0) {
        while (*p == L' ' || *p == L'\t') {
            ++p;
        }
        if (*p == 0) {
            break;
        }
        int length = 0;
        bool quoted = false;
        if (first) {
            // The program name: quotes delimit it, backslashes are literal.
            if (*p == L'"') {
                ++p;
                while (*p != 0 && *p != L'"') {
                    ++p;
                }
                if (*p == L'"') {
                    ++p;
                }
            }
            else {
                while (*p != 0 && *p != L' ' && *p != L'\t') {
                    ++p;
                }
            }
            first = false;
            continue;
        }
        while (*p != 0 && (quoted || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'\\') {
                int slashes = 0;
                while (*p == L'\\') {
                    ++slashes;
                    ++p;
                }
                if (*p == L'"') {
                    for (int i = 0; i < slashes / 2; ++i) {
                        word[length++] = L'\\';
                    }
                    if (slashes % 2 == 1) {
                        word[length++] = L'"';
                        ++p;
                    }
                }
                else {
                    for (int i = 0; i < slashes; ++i) {
                        word[length++] = L'\\';
                    }
                }
            }
            else if (*p == L'"') {
                if (quoted && p[1] == L'"') {
                    word[length++] = L'"';
                    p += 2;
                    continue;
                }
                quoted = !quoted;
                ++p;
            }
            else {
                word[length++] = *p++;
            }
        }
        void* arg = utf8String(word, length);
        bobArrayPush(&args, &arg, &argElement);
    }
    HeapFree(GetProcessHeap(), 0, word);
    return args;
}

}

extern "C" void bobStart()
{
    bob::runtime::win64::consoleInit();
    void* args = commandLineArguments();
    int32_t result = bobMain(args);
    bobRelease(args);
    bobRuntimeReportLeaks();
    ExitProcess(static_cast<UINT>(result));
}
