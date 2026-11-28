#include "hostedRuntime.h"

#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"

#include <Windows.h>

#include <cstring>
#include <mutex>

extern "C" void __chkstk();

namespace {
int fltusedMarker = 0;
}

namespace {

std::mutex collected;
bob::hosted::RunResult* current = nullptr;

struct ThreadStart {
    void (*entry)(void*);
    void* argument;
};

DWORD WINAPI runThread(LPVOID parameter)
{
    auto* start = static_cast<ThreadStart*>(parameter);
    start->entry(start->argument);
    return 0;
}

}

extern "C" {

// Compile-time code is the only caller: record the reason and end its thread.
void bobRuntimeTrap(int32_t reason)
{
    {
        std::lock_guard<std::mutex> lock(collected);
        if (current != nullptr) {
            current->trapReason = reason;
        }
    }
    ExitThread(1);
}

void bobRuntimeWrite(int32_t, const uint8_t* bytes, uint64_t count)
{
    std::lock_guard<std::mutex> lock(collected);
    if (current != nullptr && bytes != nullptr) {
        current->output.append(reinterpret_cast<const char*>(bytes), static_cast<size_t>(count));
    }
}

// `compiler error: text` and friends; `node` is the AST node of the send.
void bobCompilerDiagnostic(int32_t kind, void* text, int64_t node)
{
    auto* string = static_cast<BobString*>(text);
    std::lock_guard<std::mutex> lock(collected);
    if (current != nullptr) {
        bob::hosted::Diagnostic diagnostic;
        diagnostic.kind = kind;
        if (string != nullptr) {
            diagnostic.text.assign(reinterpret_cast<const char*>(string->bytes), static_cast<size_t>(string->byteCount));
        }
        diagnostic.node = reinterpret_cast<const void*>(node);
        current->diagnostics.push_back(diagnostic);
    }
}

}

namespace bob::hosted {

RunResult run(void (*entry)(void*), void* argument, uint32_t timeoutMilliseconds)
{
    RunResult result;
    {
        std::lock_guard<std::mutex> lock(collected);
        current = &result;
    }
    ThreadStart start{entry, argument};
    // Compile-time code can recurse deeply (table generators, parsers): 64 MB of stack.
    HANDLE thread = CreateThread(nullptr, 64u << 20, runThread, &start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) {
        result.trapReason = static_cast<int32_t>(bob::abi::TrapReason::outOfMemory);
    }
    else {
        DWORD wait = WaitForSingleObject(thread, timeoutMilliseconds);
        if (wait == WAIT_TIMEOUT) {
            TerminateThread(thread, 2);
            WaitForSingleObject(thread, INFINITE);
            result.timedOut = true;
        }
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        result.completed = !result.timedOut && code == 0;
        CloseHandle(thread);
    }
    std::lock_guard<std::mutex> lock(collected);
    current = nullptr;
    return result;
}

std::vector<Symbol> runtimeSymbols()
{
    return {
        {"bobRuntimeTrap", reinterpret_cast<void*>(&bobRuntimeTrap)},
        {"bobRuntimeWrite", reinterpret_cast<void*>(&bobRuntimeWrite)},
        {"bobCompilerDiagnostic", reinterpret_cast<void*>(&bobCompilerDiagnostic)},
        {"bobAllocObject", reinterpret_cast<void*>(&bobAllocObject)},
        {"bobRetain", reinterpret_cast<void*>(&bobRetain)},
        {"bobRelease", reinterpret_cast<void*>(&bobRelease)},
        {"bobWeakRetain", reinterpret_cast<void*>(&bobWeakRetain)},
        {"bobWeakRelease", reinterpret_cast<void*>(&bobWeakRelease)},
        {"bobWeakLoad", reinterpret_cast<void*>(&bobWeakLoad)},
        {"bobUnownedLoad", reinterpret_cast<void*>(&bobUnownedLoad)},
        {"bobObjectIsOnStack", reinterpret_cast<void*>(&bobObjectIsOnStack)},
        {"bob_msgSend", reinterpret_cast<void*>(&bob_msgSend)},
        {"bobWitness", reinterpret_cast<void*>(&bobWitness)},
        {"bobClassIsKindOf", reinterpret_cast<void*>(&bobClassIsKindOf)},
        {"bobClassRespondsTo", reinterpret_cast<void*>(&bobClassRespondsTo)},
        {"bobClassFields", reinterpret_cast<void*>(&bobClassFields)},
        {"bobClassMessages", reinterpret_cast<void*>(&bobClassMessages)},
        {"bobClassProtocols", reinterpret_cast<void*>(&bobClassProtocols)},
        {"bobClassFieldOffset", reinterpret_cast<void*>(&bobClassFieldOffset)},
        {"bobClassMethod", reinterpret_cast<void*>(&bobClassMethod)},
        {"bobClassNew", reinterpret_cast<void*>(&bobClassNew)},
        {"bobStringClass", &bobStringClass},
        {"bobArrayClass", &bobArrayClass},
        {"bobTableClass", &bobTableClass},
        {"bobStringFromBytes", reinterpret_cast<void*>(&bobStringFromBytes)},
        {"bobStringConcat", reinterpret_cast<void*>(&bobStringConcat)},
        {"bobStringCompare", reinterpret_cast<void*>(&bobStringCompare)},
        {"bobStringByteCount", reinterpret_cast<void*>(&bobStringByteCount)},
        {"bobStringCodepointCount", reinterpret_cast<void*>(&bobStringCodepointCount)},
        {"bobStringGraphemeCount", reinterpret_cast<void*>(&bobStringGraphemeCount)},
        {"bobStringByteAt", reinterpret_cast<void*>(&bobStringByteAt)},
        {"bobStringDecodeAt", reinterpret_cast<void*>(&bobStringDecodeAt)},
        {"bobStringNextGrapheme", reinterpret_cast<void*>(&bobStringNextGrapheme)},
        {"bobStringSlice", reinterpret_cast<void*>(&bobStringSlice)},
        {"bobStringPredicate", reinterpret_cast<void*>(&bobStringPredicate)},
        {"bobStringSplit", reinterpret_cast<void*>(&bobStringSplit)},
        {"bobStringJoin", reinterpret_cast<void*>(&bobStringJoin)},
        {"bobStringFromUtf8", reinterpret_cast<void*>(&bobStringFromUtf8)},
        {"bobStringToUtf8", reinterpret_cast<void*>(&bobStringToUtf8)},
        {"bobStringFromInteger", reinterpret_cast<void*>(&bobStringFromInteger)},
        {"bobStringFromReal", reinterpret_cast<void*>(&bobStringFromReal)},
        {"bobStringFromBool", reinterpret_cast<void*>(&bobStringFromBool)},
        {"bobStringFromChar", reinterpret_cast<void*>(&bobStringFromChar)},
        {"bobArrayNew", reinterpret_cast<void*>(&bobArrayNew)},
        {"bobArrayPush", reinterpret_cast<void*>(&bobArrayPush)},
        {"bobArrayMakeUnique", reinterpret_cast<void*>(&bobArrayMakeUnique)},
        {"bobArrayRemoveLast", reinterpret_cast<void*>(&bobArrayRemoveLast)},
        {"bobArrayIncludes", reinterpret_cast<void*>(&bobArrayIncludes)},
        {"bobTableAt", reinterpret_cast<void*>(&bobTableAt)},
        {"bobTableAtPut", reinterpret_cast<void*>(&bobTableAtPut)},
        {"bobTableRemove", reinterpret_cast<void*>(&bobTableRemove)},
        {"bobTableCount", reinterpret_cast<void*>(&bobTableCount)},
        {"bobTableNext", reinterpret_cast<void*>(&bobTableNext)},
        {"bobTableKeyAt", reinterpret_cast<void*>(&bobTableKeyAt)},
        {"bobTableValueAt", reinterpret_cast<void*>(&bobTableValueAt)},
        {"bobTableKeys", reinterpret_cast<void*>(&bobTableKeys)},
        {"bobTableValues", reinterpret_cast<void*>(&bobTableValues)},
        {"memcpy", reinterpret_cast<void*>(&std::memcpy)},
        {"memmove", reinterpret_cast<void*>(&std::memmove)},
        {"memset", reinterpret_cast<void*>(&std::memset)},
        {"memcmp", reinterpret_cast<void*>(&std::memcmp)},
        {"__chkstk", reinterpret_cast<void*>(&__chkstk)},
        // Object files that use floating point refer to this MSVC marker.
        {"_fltused", &fltusedMarker},
    };
}

}
