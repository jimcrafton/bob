#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The runtime built into the compiler (bobrtHosted), for compile-time code
// (design §8.2). It's the same core as bobrt; only the platform layer differs:
// a trap ends the worker thread running the code instead of the process, and
// output and `compiler error:` sends are collected for the compiler to report.
namespace bob::hosted {

struct Diagnostic {
    int32_t kind = 0; // 0 error, 1 warning, 2 note
    std::string text;
    const void* node = nullptr;
};

struct RunResult {
    bool completed = false;
    bool timedOut = false;
    int32_t trapReason = 0;
    std::string output;
    std::vector<Diagnostic> diagnostics;
};

// Runs `entry(argument)` on a fresh thread with a large stack, waiting up to
// `timeoutMilliseconds`.
RunResult run(void (*entry)(void*), void* argument, uint32_t timeoutMilliseconds);

struct Symbol {
    const char* name;
    void* address;
};

// Every runtime function and global generated code may refer to.
std::vector<Symbol> runtimeSymbols();

}
