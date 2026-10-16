// Crash and hang reports (native.h: InstallCrashLog, HangBeat), written straight into randy-vk.log.
//
// The freezes we have seen (docs/stability-plan.md) are threads that faulted while holding the process heap or the
// loader lock: everything else then waits on that lock forever. So nothing here may take either. No GetModuleHandle*,
// no heap, no CRT stdio: modules are found by walking the loader's list in the PEB, lines are
// formatted into a static buffer and written with WriteFile to a handle opened at install, and the watchdog finds the
// threads with NtGetNextThread (resolved at install). Nothing here may fault either, not even under __try: the game's
// own crash handler ends the game on any access violation it sees (scene.cpp), so every read is checked first
// (VirtualQuery, a system call that takes neither lock). The fault handler is registered again at the first frame, to
// run before any handler the game added while starting.
//
// Faults: the first report of each distinct fault address, 16 at most, first chance (before any handler - the game's
// or mss32's - gets it; it may still be handled). Hangs: the game thread's frames (Randy_t::Flip) stop for 20 s -> each
// thread's registers and the return addresses on its stack, 3 reports at most, the next only after frames resumed.
#include "native/native.h"

#include <intrin.h>

#include <algorithm>

#include <cstdint>

namespace rnative {

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------- lines, without the CRT
struct Line {
    char text[512];
    unsigned n = 0;
    Line& Put(const char* s)
    {
        while (*s && n < sizeof(text) - 2) text[n++] = *s++;
        return *this;
    }
    Line& Hex(uint32_t v, int digits = 8)          // at least `digits` digits
    {
        while (digits < 8 && (v >> (digits * 4))) ++digits;
        for (int i = digits - 1; i >= 0 && n < sizeof(text) - 2; --i) text[n++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
        return *this;
    }
    Line& Dec(uint32_t v)
    {
        char d[10];
        int k = 0;
        do d[k++] = char('0' + v % 10); while ((v /= 10) && k < 10);
        while (k && n < sizeof(text) - 2) text[n++] = d[--k];
        return *this;
    }
    void Write()
    {
        text[n++] = '\n';
        DWORD written;
        if (g_file != INVALID_HANDLE_VALUE) WriteFile(g_file, text, n, &written, nullptr);
        n = 0;
    }
};

// ---------------------------------------------------------------- modules, from the PEB
// x86 layouts (winternl.h keeps most of these fields reserved): PEB+0x0C Ldr; PEB_LDR_DATA+0x0C InLoadOrderModuleList;
// LDR_DATA_TABLE_ENTRY: +0x00 InLoadOrderLinks, +0x18 DllBase, +0x20 SizeOfImage, +0x2C BaseDllName.
static_assert(sizeof(void*) == 4, "crash.cpp reads the 32-bit PEB layout");

struct Module {
    uintptr_t base = 0;
    uint32_t size = 0;
    const wchar_t* name = nullptr;
    unsigned short nameBytes = 0;
};

// Whether [p, p + bytes) is committed, readable memory. A few pages are remembered (one report reads the same few).
struct Pages {
    uintptr_t page[32];
    bool ok[32];
    int count = 0, next = 0;
} g_pages;

bool PageReadable(uintptr_t page)
{
    for (int i = 0; i < g_pages.count; ++i)
        if (g_pages.page[i] == page) return g_pages.ok[i];
    MEMORY_BASIC_INFORMATION mbi{};
    const bool ok = VirtualQuery(reinterpret_cast<const void*>(page), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    (mbi.Protect & 0xFF) != 0;
    g_pages.page[g_pages.next] = page;
    g_pages.ok[g_pages.next] = ok;
    g_pages.next = (g_pages.next + 1) % 32;
    if (g_pages.count < 32) ++g_pages.count;
    return ok;
}

bool Readable(const void* p, size_t bytes)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x10000 || a + bytes < a) return false;
    for (uintptr_t page = a & ~uintptr_t(0xFFF); page < a + bytes; page += 0x1000)
        if (!PageReadable(page)) return false;
    return true;
}

template <typename T>
bool Read(const void* p, T* out)
{
    if (!Readable(p, sizeof(T))) return false;
    *out = *static_cast<const T*>(p);
    return true;
}

bool FindModule(uintptr_t address, Module* out)
{
    const uint8_t* ldr = nullptr;
    if (!Read(reinterpret_cast<const uint8_t*>(__readfsdword(0x30)) + 0x0C, &ldr) || !ldr) return false;
    const auto* head = reinterpret_cast<const LIST_ENTRY*>(ldr + 0x0C);
    const LIST_ENTRY* e = nullptr;
    if (!Read(&head->Flink, &e)) return false;
    for (int guard = 0; e && e != head && guard < 1024; ++guard) {
        const auto* entry = reinterpret_cast<const uint8_t*>(e);
        if (!Readable(entry, 0x34)) return false;
        const uintptr_t base = *reinterpret_cast<const uintptr_t*>(entry + 0x18);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(entry + 0x20);
        if (address >= base && address - base < size) {
            out->base = base;
            out->size = size;
            out->nameBytes = *reinterpret_cast<const unsigned short*>(entry + 0x2C);
            out->name = *reinterpret_cast<const wchar_t* const*>(entry + 0x30);
            return true;
        }
        e = e->Flink;
    }
    return false;
}

// "module+offset", or the bare address.
void PutWhere(Line& line, uintptr_t address)
{
    Module m;
    if (!FindModule(address, &m)) {
        line.Hex(uint32_t(address));
        return;
    }
    const unsigned chars = std::min(m.nameBytes / 2u, 64u);
    if (Readable(m.name, chars * 2)) {
        for (unsigned i = 0; i < chars; ++i) {
            const wchar_t c = m.name[i];
            const char text[2] = {c < 128 ? char(c) : '?', 0};
            line.Put(text);
        }
    } else {
        line.Put("?");
    }
    line.Put("+").Hex(uint32_t(address - m.base), 1);
}

// Copies up to `count` dwords of a stack from `esp` up (stops at the first unreadable page); returns how many.
int CopyStack(uintptr_t esp, uintptr_t* out, int count)
{
    const auto* stack = reinterpret_cast<const uintptr_t*>(esp);
    int i = 0;
    for (; i < count; ++i) {
        if ((reinterpret_cast<uintptr_t>(stack + i) & 0xFFF) == 0 || i == 0)
            if (!Readable(stack + i, 4)) break;
        out[i] = stack[i];
    }
    return i;
}

// Up to `most` return-address candidates in a copied stack (values pointing into a module).
void PutStack(Line& line, const uintptr_t* stack, int count, int most)
{
    Module m;
    int shown = 0;
    for (int i = 0; i < count && shown < most; ++i) {
        const uintptr_t v = stack[i];
        if (v < 0x10000 || !FindModule(v, &m)) continue;
        line.Put("    [esp+").Hex(uint32_t(i * 4), 3).Put("] ");
        PutWhere(line, v);
        line.Write();
        ++shown;
    }
}

void PutRegisters(Line& line, const CONTEXT& c)
{
    line.Put("    eip ");
    PutWhere(line, c.Eip);
    line.Put(" eax ").Hex(c.Eax).Put(" ebx ").Hex(c.Ebx).Put(" ecx ").Hex(c.Ecx).Put(" edx ").Hex(c.Edx);
    line.Put(" esi ").Hex(c.Esi).Put(" edi ").Hex(c.Edi).Put(" ebp ").Hex(c.Ebp).Put(" esp ").Hex(c.Esp);
    line.Write();
}

// One writer at a time; a thread that faults while another reports (or inside its own report) is skipped, not queued.
volatile LONG g_busy;
Line g_line;
uintptr_t g_stack[1024];

// ---------------------------------------------------------------- faults
volatile LONG g_faults;
uintptr_t g_faultAt[16];

const char* FaultName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "divide by zero";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun";
    default: return nullptr;
    }
}

LONG CALLBACK FaultReport(EXCEPTION_POINTERS* e)
{
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    const char* name = FaultName(code);
    if (!name || g_faults >= 16) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    bool seen = false;
    for (LONG i = 0; i < g_faults; ++i) seen |= g_faultAt[i] == at;
    if (!seen) {
        g_pages.count = 0;
        g_faultAt[g_faults] = at;
        InterlockedIncrement(&g_faults);
        Line& line = g_line;
        line.Put("randy-vk crash: fault ").Hex(code).Put(" (").Put(name).Put(") at ");
        PutWhere(line, at);
        if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR* info = e->ExceptionRecord->ExceptionInformation;
            line.Put(info[0] == 8 ? " executing " : info[0] ? " writing " : " reading ").Hex(uint32_t(info[1]));
        }
        line.Put(", thread ").Hex(GetCurrentThreadId(), 4).Put(" - first chance, may still be handled");
        line.Write();
        PutRegisters(line, *e->ContextRecord);
        PutStack(line, g_stack, CopyStack(e->ContextRecord->Esp, g_stack, 1024), 24);
    }
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- hangs
using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags,
                                      HANDLE* next);
NtGetNextThreadFn g_nextThread;
volatile LONG g_beats;
volatile DWORD g_lastBeat;
DWORD g_gameThread;

void DumpThreads(DWORD stalledMs)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    Line& line = g_line;
    line.Put("randy-vk crash: hang - no frame for ").Dec(stalledMs / 1000).Put(" s (game thread ").Hex(g_gameThread, 4)
        .Put("); every thread's place follows (thread ids as in Wine's log)");
    line.Write();
    g_pages.count = 0;
    const DWORD self = GetCurrentThreadId();
    HANDLE thread = nullptr;
    int count = 0;
    while (count < 256) {
        HANDLE next = nullptr;
        if (g_nextThread(GetCurrentProcess(), thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                         THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next) < 0)
            break;
        if (thread) CloseHandle(thread);
        thread = next;
        ++count;
        const DWORD id = GetThreadId(thread);
        if (id == self) continue;
        // Suspended only while its registers and stack are copied: a suspended thread may hold a lock WriteFile needs.
        if (SuspendThread(thread) == DWORD(-1)) {
            line.Put("  thread ").Hex(id, 4).Put(": can't suspend");
            line.Write();
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        const bool got = GetThreadContext(thread, &c) != 0;
        const int words = got ? CopyStack(c.Esp, g_stack, 1024) : 0;
        ResumeThread(thread);
        line.Put("  thread ").Hex(id, 4).Put(id == g_gameThread ? " (game thread)" : "").Put(got ? "" : ": no context");
        line.Write();
        if (got) {
            PutRegisters(line, c);
            PutStack(line, g_stack, words, 16);
        }
    }
    if (thread) CloseHandle(thread);
    line.Put("randy-vk crash: hang report ends (").Dec(uint32_t(count)).Put(" threads)");
    line.Write();
    InterlockedExchange(&g_busy, 0);
}

DWORD g_stallMs = 20000;                          // RANDYVK_HANG_SECONDS (tests)

DWORD WINAPI Watchdog(void*)
{
    const DWORD kStallMs = g_stallMs;
    int reports = 0;
    LONG reportedAt = -1;                         // the beat count of the last report: one report per stall
    for (;;) {
        Sleep(1000);
        const LONG beats = g_beats;
        if (!beats || beats == reportedAt) continue;
        const DWORD stalled = GetTickCount() - g_lastBeat;
        if (stalled < kStallMs) continue;
        reportedAt = beats;
        DumpThreads(stalled);
        if (++reports == 3) return 0;
    }
}

}  // namespace

static void* g_handler;                       // the fault handler, registered again at the first frame

void HangBeat()
{
    g_lastBeat = GetTickCount();
    if (InterlockedIncrement(&g_beats) == 1) {
        g_gameThread = GetCurrentThreadId();
        // First in line again: handlers the game (or mss32) added while starting would otherwise see a fault first.
        if (g_handler) RemoveVectoredExceptionHandler(g_handler);
        g_handler = AddVectoredExceptionHandler(1, &FaultReport);
    }
}

void InstallCrashLog()
{
    static bool done;
    if (done) return;
    done = true;
    // The log's first line of the session moves an old, large log aside (RvkLog): let that happen before the handle
    // opens, so the reports go into this session's file.
    Log("crash reports: on (faults in any module; a hang report after 20 s without a frame)");
    char path[MAX_PATH] = "randy-vk.log";
    GetEnvironmentVariableA("RANDYVK_LOG", path, sizeof(path));
    g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_handler = AddVectoredExceptionHandler(1, &FaultReport);
    char off[8] = "";
    if (GetEnvironmentVariableA("RANDYVK_NO_WATCHDOG", off, sizeof(off)) && off[0] == '1') return;
    char seconds[8] = "";
    if (GetEnvironmentVariableA("RANDYVK_HANG_SECONDS", seconds, sizeof(seconds))) {
        DWORD v = 0;
        for (const char* c = seconds; *c >= '0' && *c <= '9'; ++c) v = v * 10 + DWORD(*c - '0');
        if (v) g_stallMs = v * 1000;
    }
    g_nextThread = reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread"));
    if (!g_nextThread) {
        Log("crash reports: NtGetNextThread missing - no hang reports");
        return;
    }
    if (HANDLE t = CreateThread(nullptr, 64 * 1024, &Watchdog, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace rnative
