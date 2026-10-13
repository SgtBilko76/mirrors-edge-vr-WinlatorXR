// WinlatorXR: keep the heavy threads on the Quest's fast cores, and report which threads burn CPU.
//
// Measured 2026-10-13: the game is CPU-bound under Box64 (GPU ~45% busy). A Quest 3 has two cores
// at 2.05 GHz (cpu0-1) and four at 2.36 GHz (cpu2-5); Wine reports all six as equal, so the
// scheduler is free to put the game thread on a slow one. The clocks are read from Linux sysfs
// through Wine's \\?\unix\ prefix. Threads pinned to the fast cores:
//   - the game thread (the one that owns the game window),
//   - the render thread (the one calling Present, where this runs),
//   - DXVK's command-stream thread ("dxvk-cs"), which turns D3D9 calls into Vulkan.
// WinlatorXrCpuPin = off leaves every thread where the scheduler puts it.
// Every 600 frames the log lists the busiest threads by CPU time, with their names.

#include <tlhelp32.h>

// g_wxrCpuPin (WinlatorXrCpuPin) is defined in d3d9.cpp beside the other WinlatorXR settings.

typedef HRESULT (WINAPI* PFN_GetThreadDescription)(HANDLE, PWSTR*);

static DWORD_PTR WxrFastCoreMask(char* clocks, size_t cap)
{
    DWORD_PTR processMask = 0, systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    unsigned long fastest = 0;
    DWORD_PTR mask = 0;
    clocks[0] = 0;
    for (unsigned cpu = 0; cpu < 32; ++cpu) {
        const DWORD_PTR bit = (DWORD_PTR)1 << cpu;
        if (!(processMask & bit)) continue;
        char path[128];
        sprintf_s(path, "\\\\?\\unix\\sys\\devices\\system\\cpu\\cpu%u\\cpufreq\\cpuinfo_max_freq", cpu);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) continue;
        unsigned long khz = 0;
        const int got = fscanf_s(f, "%lu", &khz);
        fclose(f);
        if (got != 1 || !khz) continue;
        const size_t len = strlen(clocks);
        sprintf_s(clocks + len, cap - len, "%s%u:%lu", len ? " " : "", cpu, khz / 1000);
        if (khz > fastest) { fastest = khz; mask = 0; }
        if (khz == fastest) mask |= bit;
    }
    if (mask == processMask) return 0;      // all equal: nothing to gain
    return mask;
}

static bool WxrThreadName(DWORD tid, char* out, size_t cap)
{
    static PFN_GetThreadDescription getDesc =
        (PFN_GetThreadDescription)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    out[0] = 0;
    if (!getDesc) return false;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return false;
    PWSTR desc = nullptr;
    if (SUCCEEDED(getDesc(h, &desc)) && desc) {
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, out, (int)cap, nullptr, nullptr);
        LocalFree(desc);
    }
    CloseHandle(h);
    return out[0] != 0;
}

static void WxrPinThread(DWORD tid, DWORD_PTR mask, const char* what)
{
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { Log("[cpu] could not open the %s thread %lu", what, (unsigned long)tid); return; }
    const DWORD_PTR prev = SetThreadAffinityMask(h, mask);
    CloseHandle(h);
    Log("[cpu] %s thread %lu pinned to CPU mask 0x%lX%s", what, (unsigned long)tid, (unsigned long)mask,
        prev ? "" : " (SetThreadAffinityMask FAILED)");
}

struct WxrThreadSample { DWORD tid; ULONGLONG cpu100ns; };

// Called from WxrSubmitFrame on the render thread.
static void WxrCpuTick(long frame)
{
    static DWORD_PTR fastMask = 0;
    static bool probed = false;
    static DWORD gameTid = 0;
    static std::vector<DWORD> pinned;
    if (!probed) {
        probed = true;
        char clocks[160];
        fastMask = WxrFastCoreMask(clocks, sizeof(clocks));
        Log("[cpu] core clocks (cpu:MHz) %s -> fast-core mask 0x%lX%s", clocks[0] ? clocks : "unreadable",
            (unsigned long)fastMask, g_wxrCpuPin ? "" : "  (WinlatorXrCpuPin = off: not pinning)");
        if (g_wxrCpuPin && fastMask) {
            WxrPinThread(GetCurrentThreadId(), fastMask, "render (Present)");
            pinned.push_back(GetCurrentThreadId());
        }
    }
    if (!gameTid && g_wxrWindow) {
        gameTid = GetWindowThreadProcessId(g_wxrWindow, nullptr);
        if (g_wxrCpuPin && fastMask && gameTid && gameTid != GetCurrentThreadId()) {
            WxrPinThread(gameTid, fastMask, "game (window owner)");
            pinned.push_back(gameTid);
        }
    }

    if ((frame % 600) != 0) return;

    // Walk the process's threads: pin DXVK's command thread once it exists, and report CPU use.
    static std::vector<WxrThreadSample> last;
    static LARGE_INTEGER lastAt{};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double wallMs = lastAt.QuadPart ? (double)(now.QuadPart - lastAt.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
    lastAt = now;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<WxrThreadSample> cur;
    struct Busy { DWORD tid; double pct; };
    std::vector<Busy> busy;
    THREADENTRY32 te{ sizeof(te) };
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            const ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            cur.push_back({ te.th32ThreadID, t });
            for (const auto& s : last)
                if (s.tid == te.th32ThreadID && wallMs > 0 && t >= s.cpu100ns)
                    busy.push_back({ s.tid, (double)(t - s.cpu100ns) / 10000.0 / wallMs * 100.0 });
        }
        CloseHandle(h);
        if (g_wxrCpuPin && fastMask &&
            std::find(pinned.begin(), pinned.end(), te.th32ThreadID) == pinned.end()) {
            char name[64];
            if (WxrThreadName(te.th32ThreadID, name, sizeof(name)) && strncmp(name, "dxvk-cs", 7) == 0) {
                WxrPinThread(te.th32ThreadID, fastMask, "DXVK command (dxvk-cs)");
                pinned.push_back(te.th32ThreadID);
            }
        }
    }
    CloseHandle(snap);
    last.swap(cur);
    if (busy.empty()) return;
    std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.pct > b.pct; });
    char line[700]; int at = 0;
    for (size_t i = 0; i < busy.size() && i < 8; ++i) {
        char name[64];
        if (!WxrThreadName(busy[i].tid, name, sizeof(name)))
            strcpy_s(name, busy[i].tid == gameTid ? "game" : busy[i].tid == GetCurrentThreadId() ? "render" : "?");
        at += sprintf_s(line + at, sizeof(line) - at, "%s%lu %s %.0f%%", i ? ", " : "",
                        (unsigned long)busy[i].tid, name, busy[i].pct);
    }
    Log("[cpu] busiest threads over %.1f s (100%% = one core): %s  [game %lu, render %lu]",
        wallMs / 1000.0, line, (unsigned long)gameTid, (unsigned long)GetCurrentThreadId());
}
