// MGPU Bridge - D2.0: the journal. See journal.hpp.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "journal.hpp"
#include "log_queue.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::journal
{
namespace
{
std::atomic<int> g_mode{-1};                 // -1 not read, 0 off, 1 on
HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<void *> g_game_chain{nullptr};

// ---- CRT-free formatting for the DllMain-time writes ----
struct rbuf { char b[1100]; unsigned n = 0; };
void r_app(rbuf &o, const char *s) { while (s && *s && o.n < sizeof o.b - 3) o.b[o.n++] = *s++; }
void r_u(rbuf &o, unsigned long long v, int width = 0)
{
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 23);
    while (i < width && i < 23) t[i++] = '0';
    while (i > 0 && o.n < sizeof o.b - 3) o.b[o.n++] = t[--i];
}
void r_stamp(rbuf &o)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    r_u(o, st.wHour, 2); r_app(o, ":"); r_u(o, st.wMinute, 2); r_app(o, ":");
    r_u(o, st.wSecond, 2); r_app(o, "."); r_u(o, st.wMilliseconds, 3);
    r_app(o, " qpc="); r_u(o, (unsigned long long)q.QuadPart);
    r_app(o, " tid="); r_u(o, GetCurrentThreadId());
    r_app(o, " | ");
}
void r_emit(rbuf &o)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    o.b[o.n++] = '\r';
    o.b[o.n++] = '\n';
    DWORD w = 0;
    WriteFile(g_file, o.b, o.n, &w, nullptr);
}
std::atomic<unsigned long long> g_posted{0}, g_written{0};
LARGE_INTEGER g_freq{};

bool addon_dir(wchar_t *out, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

int read_mode()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "Journal");
    return (v != nullptr && atoi(v) == 1) ? 1 : 0;
}

// "hh:mm:ss.mmm" local time, then QPC and thread.
int stamp(char *out, size_t n)
{
    FILETIME ft{}, lft{};
    GetSystemTimePreciseAsFileTime(&ft);
    FileTimeToLocalFileTime(&ft, &lft);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&lft, &st);
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return snprintf(out, n, "%02u:%02u:%02u.%03u qpc=%lld tid=%lu | ", st.wHour, st.wMinute,
                    st.wSecond, st.wMilliseconds, (long long)q.QuadPart, GetCurrentThreadId());
}

void write_now(const char *line)
{
    if (g_file == INVALID_HANDLE_VALUE || line == nullptr) return;
    char buf[1100];
    size_t n = strlen(line);
    if (n > sizeof buf - 3u) n = sizeof buf - 3u;
    memcpy(buf, line, n);
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD w = 0;
    WriteFile(g_file, buf, (DWORD)n, &w, nullptr);
}

// Previous session check on the existing file's last 1 MB.
void check_previous(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    const long long want = 1024 * 1024;
    LARGE_INTEGER off{};
    off.QuadPart = sz.QuadPart > want ? sz.QuadPart - want : 0;
    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    static char tail[1024 * 1024 + 1];
    DWORD got = 0;
    ReadFile(h, tail, (DWORD)(sz.QuadPart - off.QuadPart), &got, nullptr);
    CloseHandle(h);
    tail[got] = '\0';
    if (got == 0) return;
    const char *open = nullptr, *close = nullptr;
    for (const char *p = tail; (p = strstr(p, "SESSION OPEN")) != nullptr; ++p) open = p;
    for (const char *p = tail; (p = strstr(p, "SESSION CLOSE")) != nullptr; ++p) close = p;
    if (close != nullptr && (open == nullptr || close > open)) return;   // the last session closed
    if (open == nullptr) open = tail;   // its OPEN is older than the last 1 MB: still unclean
    // Did its teardown begin? Then the missing CLOSE means an exit without
    // DLL detach (TerminateProcess and the like), not a session cut off in
    // the middle.
    const char *chain_end = nullptr, *dev_end = nullptr;
    for (const char *p = open; (p = strstr(p, "GAME CHAIN DESTROYED (resize=0)")) != nullptr; ++p) chain_end = p;
    for (const char *p = open; (p = strstr(p, "GAME DEVICE DESTROYED")) != nullptr; ++p) dev_end = p;
    const bool teardown_began = (dev_end != nullptr) ||
        (chain_end != nullptr && strchr(chain_end, '\n') != nullptr &&
         strstr(strchr(chain_end, '\n'), "| why:") == nullptr);   // nothing journaled after it
    if (teardown_began)
    {
        mgpu::diag::info("[MGPU][D20] JOURNAL: the previous session has no SESSION CLOSE, but its "
                         "teardown had begun (game chain or device destroyed was its last event): "
                         "an exit without DLL detach. Not counted as a poison suspect.");
        write_now("PREVIOUS SESSION: no SESSION CLOSE, but its teardown had begun - exit without DLL "
                  "detach; not a poison suspect.");
        return;
    }

    // Unclean: report its last 20 lines.
    const char *end = tail + got;
    const char *starts[21];
    int ns = 0;
    const char *p = end;
    while (p > open && ns < 21)
    {
        const char *q = p;
        if (q > tail && q[-1] == '\n') --q;
        if (q > tail && q[-1] == '\r') --q;
        while (q > open && q[-1] != '\n') --q;
        starts[ns++] = q;
        p = q;
    }
    char l[1100];
    snprintf(l, sizeof l, "[MGPU][D20] JOURNAL: the PREVIOUS session did not close cleanly (SESSION "
             "OPEN without SESSION CLOSE). Its last %d line(s) follow, oldest first. A session that "
             "did not close is the first suspect for a poisoned launch.", ns > 20 ? 20 : ns);
    mgpu::diag::warn(l);
    write_now("PREVIOUS SESSION DID NOT CLOSE CLEANLY - its last lines, oldest first:");
    for (int i = (ns > 20 ? 20 : ns) - 1; i >= 0; --i)
    {
        const char *s = starts[i];
        const char *e = strchr(s, '\n');
        size_t n = e != nullptr ? (size_t)(e - s) : strlen(s);
        while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) --n;
        if (n > 900) n = 900;
        char row[960];
        snprintf(row, sizeof row, "  prev> %.*s", (int)n, s);
        write_now(row);
        char rl[1000];
        snprintf(rl, sizeof rl, "[MGPU][D20] JOURNAL prev> %.*s", (int)n, s);
        mgpu::diag::warn(rl);
    }
}
}   // namespace

void init(const char *build)
{
    if (g_mode.load(std::memory_order_acquire) >= 0) return;
    const int m = read_mode();
    QueryPerformanceFrequency(&g_freq);
    int expect = -1;
    if (!g_mode.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) return;
    if (m != 1) return;

    wchar_t dir[MAX_PATH], sub[MAX_PATH], path[MAX_PATH], prev[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return;
    _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%lsmgpu", dir);
    CreateDirectoryW(sub, nullptr);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.log", dir);
    _snwprintf_s(prev, MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.prev.log", dir);

    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
    {
        mgpu::diag::warn("[MGPU][D20] JOURNAL ON but mgpu\\journal.log could not be opened - journal off.");
        return;
    }
    check_previous(path);
    LARGE_INTEGER sz{};
    GetFileSizeEx(g_file, &sz);
    if (sz.QuadPart > 8ll * 1024 * 1024)
    {
        // [PORT B3-log, 2026-11-11] FIVE generations, not one. The shipped file
        // rotated journal.log -> journal.prev.log and kept exactly one archive,
        // so a third session overwrote the first's evidence. journal.log + .1..4.
        CloseHandle(g_file);
        wchar_t from[MAX_PATH] = {}, to[MAX_PATH] = {};
        _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.4.log", dir);
        DeleteFileW(from);
        for (int g = 3; g >= 1; --g)
        {
            _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.%d.log", dir, g);
            _snwprintf_s(to,   MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.%d.log", dir, g + 1);
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
        }
        _snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%lsmgpu\\journal.1.log", dir);
        MoveFileExW(path, to, MOVEFILE_REPLACE_EXISTING);
        // The legacy single archive is folded into the generation scheme once.
        DeleteFileW(prev);
        g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_file == INVALID_HANDLE_VALUE) return;
    }
    char st[96]; stamp(st, sizeof st);
    char l[600];
    snprintf(l, sizeof l, "%sSESSION OPEN | build %s | pid %lu", st, build != nullptr ? build : "?",
             GetCurrentProcessId());
    write_now(l);
    g_mode.store(1, std::memory_order_release);
    mgpu::diag::warn("[MGPU][D20] JOURNAL ON (Journal=1): every interaction of D2.0 with the game's "
                     "subsystems is written to mgpu\\journal.log with what, when and why.");
}

bool on() { return g_mode.load(std::memory_order_relaxed) == 1; }

void event(const char *what, const char *why)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    char l[1024];
    int w = stamp(l, sizeof l);
    if (w < 0 || w >= (int)sizeof l) return;
    snprintf(l + w, sizeof l - (size_t)w, "%s | why: %s", what != nullptr ? what : "",
             why != nullptr ? why : "");
    g_posted.fetch_add(1, std::memory_order_relaxed);
    mgpu::lq::post(mgpu::lq::TO_JOURNAL, l);
}

void event_direct(const char *what, const char *why)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    rbuf o;
    r_stamp(o);
    r_app(o, what); r_app(o, " | why: "); r_app(o, why);
    r_emit(o);
}

void note_game_chain(void *chain)
{
    if (g_mode.load(std::memory_order_relaxed) != 1 || chain == nullptr) return;
    g_game_chain.store(chain, std::memory_order_release);
}

void on_chain_destroyed(void *chain, bool resize)
{
    if (g_mode.load(std::memory_order_relaxed) != 1 || chain == nullptr) return;
    if (chain != g_game_chain.load(std::memory_order_acquire)) return;
    event(resize ? "GAME CHAIN DESTROYED (resize=1)" : "GAME CHAIN DESTROYED (resize=0)",
          "ReShade destroy_swapchain on the game's chain");
}

void on_game_device_destroyed()
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    event("GAME DEVICE DESTROYED", "ReShade destroy_device on the game's device: teardown begins");
}

void write_raw(const char *line)
{
    write_now(line);
    g_written.fetch_add(1, std::memory_order_relaxed);
}

void session_close_direct(const char *how)
{
    if (g_mode.load(std::memory_order_relaxed) != 1) return;
    rbuf o;
    r_stamp(o);
    r_app(o, "SESSION CLOSE | "); r_app(o, how);
    r_app(o, " | journal lines posted "); r_u(o, g_posted.load(std::memory_order_relaxed));
    r_app(o, ", written "); r_u(o, g_written.load(std::memory_order_relaxed));
    r_app(o, " (the difference was still queued)");
    r_emit(o);
    FlushFileBuffers(g_file);
}
}   // namespace mgpu::journal
