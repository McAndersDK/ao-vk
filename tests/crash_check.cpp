// The crash and hang reports (proxy/native/crash.cpp) in the cases that matter: tools/crash-check.sh.
// 1. A fault in our own code. 2. A fault while this thread holds the process heap lock and the loader lock (a report
// that allocates or asks the loader would hang here). 3. A hang: one thread holds the heap lock forever, the main thread
// blocks on it - the watchdog must still write every thread's place. Exits by itself, never with a dialog.
#include "native/native.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace {

void Print(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// clang-cl catches a hardware fault in __try only from a call (no /EHa), so the faulting store is its own function.
// Two of them: reports are one per fault address.
__declspec(noinline) void Poke(volatile int* p) { *p = 1; }
__declspec(noinline) void PokeLocked(volatile int* p) { *p = 2; }

void FaultAt(void (*poke)(volatile int*), uintptr_t address)
{
    __try {
        poke(reinterpret_cast<volatile int*>(address));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using LdrLockFn = LONG(NTAPI*)(ULONG flags, ULONG* state, ULONG_PTR* cookie);
using LdrUnlockFn = LONG(NTAPI*)(ULONG flags, ULONG_PTR cookie);

// The frozen sessions' picture: one thread holds the process heap (and a lock others want) and never lets go.
CRITICAL_SECTION g_held;

DWORD WINAPI HoldHeap(void*)
{
    EnterCriticalSection(&g_held);
    HeapLock(GetProcessHeap());
    Sleep(INFINITE);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    rnative::SetLog(&Print);
    rnative::InstallCrashLog();

    Print("check: fault in our code");
    FaultAt(&Poke, 0);

    Print("check: fault holding the heap and loader locks");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto lock = reinterpret_cast<LdrLockFn>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    auto unlock = reinterpret_cast<LdrUnlockFn>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    ULONG_PTR cookie = 0;
    HeapLock(GetProcessHeap());
    lock(0, nullptr, &cookie);
    FaultAt(&PokeLocked, 0x14);                                  // qnix's address
    unlock(0, cookie);
    HeapUnlock(GetProcessHeap());
    Print("check: still running after both");

    if (argc > 1 && !std::strcmp(argv[1], "--no-hang")) return 0;
    Print("check: hang (a thread keeps the heap lock and a critical section, this one waits on the section)");
    InitializeCriticalSection(&g_held);
    rnative::HangBeat();
    CreateThread(nullptr, 0, &HoldHeap, nullptr, 0, nullptr);
    Sleep(200);
    // Kills the process once the watchdog had its time (RANDYVK_HANG_SECONDS + 5 s): this thread is stuck in HeapAlloc.
    CreateThread(nullptr, 0, [](void*) -> DWORD {
        char s[8] = "20";
        GetEnvironmentVariableA("RANDYVK_HANG_SECONDS", s, sizeof(s));
        Sleep(DWORD(std::atoi(s) + 5) * 1000);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, nullptr, 0, nullptr);
    EnterCriticalSection(&g_held);                 // blocks for good
    Print("check: unexpected - the section was free");
    return 1;
}
