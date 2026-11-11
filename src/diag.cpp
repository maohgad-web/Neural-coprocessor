// MGPU Bridge - structured diagnostic logging (brief, section 08)
//
// D2.0-5 (0.3.0 R&D): NO LOG WRITE ON A HOT THREAD.
//
// Requiem, 2026-10-05: at one spot of the game (loading into gameplay) a
// ReShade log line took ~1 s to write instead of ~1 ms (C6: 14:42:21-25). Our
// code wrote log lines from the game's render thread and, on the bridge
// thread, while holding s.cs - the mutex the game's render thread takes every
// frame. A slow line therefore held the GAME: 49 s freezes (R1, C2, C4, C5), a
// 7 s slowdown (C6), and the ~1 s stall at every stream arm on three titles.
// This is DEFECT E (Starfield, R166): a log write under the mutex the render
// thread takes. R166 fixed it in the present gate only; this fixes it for
// every line, at the one place every line passes through.
//
// info / warn / error now POST the line to the log queue (log_queue.hpp): a
// lock-free copy into a ring, no wait, no I/O. The queue's worker thread writes
// it to ReShade's log. A slow disk now delays our log, never a frame; a full
// ring drops the line and counts it.
//
// When the worker is not running - DllMain, ReShade's add-on load / unload
// cycles before the first swap chain, after the game device is destroyed - the
// line is written directly, as before. Those windows are not on a game frame,
// and no thread of ours may be started in them.
//
// *_direct always writes now; the queue's worker uses them.
//
// ===========================================================================
// [PORT B3-log, 2026-11-11] OUR OWN SESSION LOG, BOUNDED TO FIVE FILES.
//
// WHY. ReShade.log is truncated every launch. A crash therefore leaves nothing
// of its own: the next launch overwrites exactly the lines that would have
// explained it. Every line we emit is ALSO appended to
// <exe dir>\mgpu_run.<pid>.log (one file per process, so the main process and
// its child cannot truncate each other); the last session is the newest of
// those files.
//
// WHAT CHANGED versus the tree this came from (nc-base diag.cpp, R491c/R578/
// R582). That version rotated a single ".1" and never pruned the per-pid
// files, so a folder collected one file per launch forever - measured on PBZ,
// 11-11: ~250 mgpu_run.*.log files. This one:
//   * keeps at most FIVE session files in the directory it writes to, newest
//     first, and deletes the rest at open time (one bounded sweep per process);
//   * rotates a session that reaches the 200,000-line ceiling through
//     generations .1 .. .4 (five files including the live one) instead of one.
// The sweep only touches names that start with "mgpu_run" and end with ".log",
// and it never deletes the file this process just opened.
//
// The fallback order is the one R578 established: exe dir, then
// %LOCALAPPDATA%\MGPU, then %TEMP%\MGPU. Append + unbuffered + flush/commit
// per line are R520/R582 and are kept: the failure they protect against - a
// hard kill losing the last lines - is the one that already cost a crash we
// could not read.
// ===========================================================================
#include <reshade.hpp>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <io.h>

#include "diag.hpp"
#include "log_queue.hpp"

namespace
{
    constexpr int   SESSION_LOGS_KEPT = 5;       // files on disk, newest first
    constexpr int   SESSION_GENERATIONS = 4;     // .1 .. .4 kept beside the live file
    constexpr unsigned long long SESSION_MAX_LINES = 200000ull;

    struct run_log
    {
        std::mutex m;
        FILE *f = nullptr;
        bool tried = false;
        unsigned long long lines = 0;
        unsigned long long dropped = 0;
        wchar_t path[MAX_PATH * 3] = {};
    };

    run_log &rl()
    {
        static run_log v;
        return v;
    }

    // Deletes every mgpu_run*.log in dir beyond the newest SESSION_LOGS_KEPT,
    // never touching `keep` (the file this process is opening). A housekeeping
    // failure is silent by design: it must not cost a log line. Returns how many
    // were deleted so the caller can put the number in the header.
    int prune_session_logs(const wchar_t *dir, const wchar_t *keep)
    {
        if (dir == nullptr) return 0;
        wchar_t pat[MAX_PATH * 3] = {};
        swprintf_s(pat, L"%s\\mgpu_run*.log", dir);

        struct ent { wchar_t name[MAX_PATH * 3]; unsigned long long t; };
        static ent found[512];
        int n = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if (n >= (int)(sizeof found / sizeof found[0])) break;
            wcscpy_s(found[n].name, fd.cFileName);
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            found[n].t = u.QuadPart;
            ++n;
        } while (FindNextFileW(h, &fd) != 0);
        FindClose(h);

        for (int i = 1; i < n; ++i)   // newest first
            for (int j = i; j > 0 && found[j].t > found[j - 1].t; --j)
            {
                ent tmp = found[j]; found[j] = found[j - 1]; found[j - 1] = tmp;
            }

        const wchar_t *keep_name = nullptr;
        if (keep != nullptr)
        {
            const wchar_t *sl = wcsrchr(keep, L'\\');
            keep_name = (sl != nullptr) ? sl + 1 : keep;
        }

        int kept = 0, gone = 0;
        for (int i = 0; i < n; ++i)
        {
            if (keep_name != nullptr && _wcsicmp(found[i].name, keep_name) == 0) { ++kept; continue; }
            if (kept < SESSION_LOGS_KEPT)
            {
                ++kept;
                continue;
            }
            wchar_t full[MAX_PATH * 3] = {};
            swprintf_s(full, L"%s\\%s", dir, found[i].name);
            DeleteFileW(full);
            ++gone;
        }
        return gone;
    }

    void rotate_session_log(run_log &r)
    {
        if (r.f == nullptr || r.path[0] == 0) return;
        fclose(r.f);
        r.f = nullptr;

        // .4 -> gone, .3 -> .4 ... .1 -> .2, live -> .1  (five files incl. live)
        for (int g = SESSION_GENERATIONS; g >= 1; --g)
        {
            wchar_t from[MAX_PATH * 3] = {}, to[MAX_PATH * 3] = {};
            swprintf_s(from, L"%s.%d", r.path, g);
            if (g == SESSION_GENERATIONS) { DeleteFileW(from); continue; }
            swprintf_s(to, L"%s.%d", r.path, g + 1);
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
        }
        {
            wchar_t first[MAX_PATH * 3] = {};
            swprintf_s(first, L"%s.1", r.path);
            MoveFileExW(r.path, first, MOVEFILE_REPLACE_EXISTING);
        }
        if (_wfopen_s(&r.f, r.path, L"ab") != 0) { r.f = nullptr; return; }
        setvbuf(r.f, nullptr, _IONBF, 0);
        r.lines = 0;
        fprintf(r.f, "=== MGPU Bridge session log ROTATED at %llu lines; generations .1-%d hold the "
                     "previous files ===\n", SESSION_MAX_LINES, SESSION_GENERATIONS);
    }

    void open_session_log(run_log &r)
    {
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash != nullptr) *slash = 0;

        wchar_t base[MAX_PATH * 3] = {};
        swprintf_s(base, L"%s\\mgpu_run.%lu.log", exe, (unsigned long)GetCurrentProcessId());

        // R578: three directories. The exe directory first (unchanged), then two
        // a stale handle from a previous load cannot block.
        wchar_t cand[3][MAX_PATH * 3] = {};
        wcscpy_s(cand[0], base);
        {
            wchar_t lad[MAX_PATH * 3] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH * 3) > 0)
            {
                wchar_t d[MAX_PATH * 3] = {};
                swprintf_s(d, L"%s\\MGPU", lad);
                CreateDirectoryW(d, nullptr);
                swprintf_s(cand[1], L"%s\\mgpu_run.%lu.log", d, (unsigned long)GetCurrentProcessId());
            }
        }
        {
            wchar_t td[MAX_PATH] = {};
            if (GetTempPathW(MAX_PATH, td) > 0)
            {
                wchar_t d[MAX_PATH * 3] = {};
                swprintf_s(d, L"%sMGPU", td);
                CreateDirectoryW(d, nullptr);
                swprintf_s(cand[2], L"%s\\mgpu_run.%lu.log", d, (unsigned long)GetCurrentProcessId());
            }
        }

        // R580: a per-PID name is the same for every add-on load in this process.
        // One name per LOAD is the only form that survives a title loading us more
        // than once (measured on FC6: four loads in one second).
        errno_t oe = ENOENT;
        for (int ci = 0; ci < 3 && r.f == nullptr; ++ci)
        {
            if (cand[ci][0] == 0) continue;
            for (int ss = 1; ss <= 6 && r.f == nullptr; ++ss)
            {
                wchar_t one[MAX_PATH * 3] = {};
                if (ss == 1)
                {
                    wcscpy_s(one, cand[ci]);
                }
                else
                {
                    wchar_t stem[MAX_PATH * 3] = {};
                    wcscpy_s(stem, cand[ci]);
                    wchar_t *dot = wcsrchr(stem, L'.');
                    if (dot != nullptr) *dot = 0;
                    swprintf_s(one, L"%s_%d.log", stem, ss);
                }
                oe = _wfopen_s(&r.f, one, L"ab");
                if (oe == 0) { wcscpy_s(r.path, one); break; }
                r.f = nullptr;
            }
        }

        if (oe != 0)
        {
            r.f = nullptr;
            reshade::log::message(reshade::log::level::error,
                "[MGPU][R520] SESSION LOG OPEN FAILED - every line of this run then exists ONLY in "
                "ReShade.log, which the next launch overwrites. Said out loud because 0 bytes, no file, "
                "and never reached are three different findings and used to look identical.");
            return;
        }
        setvbuf(r.f, nullptr, _IONBF, 0);

        // [PORT B3-log] the bounded sweep: at most SESSION_LOGS_KEPT session files
        // beside this one, newest first. Runs once per process, on the thread that
        // emits the first line (the log queue's worker during gameplay). The count
        // is written below: the first run after a pile-up deletes the backlog, and
        // a load-time stall must be attributable rather than mysterious.
        int pruned = 0;
        {
            wchar_t dir[MAX_PATH * 3] = {};
            wcscpy_s(dir, r.path);
            wchar_t *sl = wcsrchr(dir, L'\\');
            if (sl != nullptr) { *sl = 0; pruned = prune_session_logs(dir, r.path); }
        }
        if (pruned > 0)
            fprintf(r.f, "=== sweep: %d older session file(s) deleted, %d kept ===\n",
                    pruned, SESSION_LOGS_KEPT);

        SYSTEMTIME bo{};
        GetLocalTime(&bo);
        fprintf(r.f, "=== MGPU Bridge session log OPEN  pid=%lu  %04u-%02u-%02u %02u:%02u:%02u  "
                     "(append + unbuffered; keeping %d session files) ===\n",
                (unsigned long)GetCurrentProcessId(), (unsigned)bo.wYear, (unsigned)bo.wMonth,
                (unsigned)bo.wDay, (unsigned)bo.wHour, (unsigned)bo.wMinute, (unsigned)bo.wSecond,
                SESSION_LOGS_KEPT);
        fprintf(r.f, "=== path: %ls\n", r.path);
    }

    void append(char lvl, const char *line)
    {
        run_log &r = rl();
        std::lock_guard<std::mutex> lk(r.m);
        if (!r.tried)
        {
            r.tried = true;
            open_session_log(r);
        }
        if (r.f == nullptr) return;
        if (r.lines >= SESSION_MAX_LINES) rotate_session_log(r);
        if (r.f == nullptr) { ++r.dropped; return; }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(r.f, "%02u:%02u:%02u.%03u %c %s\n", (unsigned)st.wHour, (unsigned)st.wMinute,
                (unsigned)st.wSecond, (unsigned)st.wMilliseconds, lvl, line);
        ++r.lines;
        // R520/R582 kept the bytes readable from another process; they did it with
        // a commit PER LINE. [PORT B3-log, 2026-11-11] On the GAME thread, during a
        // load, that is exactly the DEFECT E shape this file opens with: a disk sync
        // per log line inside a load callback. _IONBF makes every line a WriteFile,
        // so the page cache is already up to date and another process can read it;
        // _commit only forces the platter. Policy: errors and warnings always,
        // info every 64 lines. A crash loses at most 63 info lines, and a stall at
        // load cannot be blamed on a log nobody can see.
        fflush(r.f);
        if (lvl != 'I' || (r.lines & 63ull) == 0ull)
        {
            if (_commit(_fileno(r.f)) != 0 && (r.lines % 1000ull) == 0ull)
                ++r.dropped;   // counted, never silent
        }
    }
}

namespace mgpu::diag
{
void info_direct (const char *line) { reshade::log::message(reshade::log::level::info, line);    append('I', line); }
void warn_direct (const char *line) { reshade::log::message(reshade::log::level::warning, line); append('W', line); }
void error_direct(const char *line) { reshade::log::message(reshade::log::level::error, line);   append('E', line); }

void info(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_INFO, line);
    else info_direct(line);
}

void warn(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_WARN, line);
    else warn_direct(line);
}

void error(const char *line)
{
    if (mgpu::lq::running()) mgpu::lq::post(mgpu::lq::TO_ERROR, line);
    else error_direct(line);
}
}
