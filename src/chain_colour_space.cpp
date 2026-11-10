// MGPU Bridge - swap-chain colour-space declaration (H1). See the header.
//
// This file is deliberately standalone: create_present_chain() runs before the
// bridge's own ini helpers (ini_find / ini_slurp) are reached in the same
// translation unit, so the key is read here with a small, local, tolerant
// parser. It reads the same mgpu.ini that sits next to the add-on module.
#include "chain_colour_space.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "diag.hpp"

namespace mgpu {
namespace chain {

namespace {

const int kAuto = INT_MIN + 1;                 // R353: ChainColorSpace=auto
std::atomic<int> g_game_cs{0};                 // R353: 0 unknown .. 4 HLG (see the header)

// R369 (0.5.1): `auto` keeps following the game after the bridge chain exists. R353 decided once, at
// creation - and every DXGI chain starts sRGB, the game states HDR with a LATER SetColorSpace1. When
// the bridge chain was created first (Gothic 1 Remake, by 5 ms) auto read sRGB and never looked again.
//   g_auto     1 only when the key read `auto` at creation. 0 = follow_game_colour_space is one load.
//   g_acted    the game code (0..4) the chain was last decided for; -1 = decide again (our own resize).
//   g_set      the DXGI value this file last set with success; -1 = never set, the chain's default.
//   g_test_s   ChainColorSpaceTest=<seconds> (debug, absent = 0 = off): pretend the game states HDR10
//              from <seconds> to 2x<seconds> after the first present, then go back to the real value.
std::atomic<int> g_auto{0};
std::atomic<int> g_acted{0};
std::atomic<int> g_set{-1};
std::atomic<int> g_test_s{0};
std::atomic<unsigned long long> g_test_t0{0ull};
// True on the calling thread while this file is inside SetColorSpace1 on the BRIDGE chain. ReShade's
// hook raises destroy/init_swapchain for that chain from inside the call, on this thread - and with
// FrameGen=4 the present runs on the pacing thread, which dllmain would otherwise take for the game's.
thread_local bool t_own_call = false;

// Reads "<key>=<integer>" from the mgpu.ini next to this module.
// Returns false when the file or the key is missing - the caller must treat
// that as "do nothing", never as "use a default".
bool read_ini_int(const wchar_t *key, int *value) {
    wchar_t path[MAX_PATH * 2] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&read_ini_int), &self))
        return false;
    if (!GetModuleFileNameW(self, path, (DWORD)(sizeof path / sizeof path[0])))
        return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr)
        return false;
    slash[1] = L'\0';
    if (wcscat_s(path, L"mgpu.ini") != 0)
        return false;

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
        return false;
    char buf[64 * 1024] = {};
    const size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (got == 0)
        return false;

    char name[96] = {};
    const int name_len_i = WideCharToMultiByte(CP_ACP, 0, key, -1, name,
                                               (int)sizeof name - 1, nullptr, nullptr);
    if (name_len_i <= 0)
        return false;
    const size_t name_len = (size_t)(name_len_i - 1);

    const char *p = buf;
    const char *end = buf + got;
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (eol == nullptr)
            eol = end;
        const char *k = p;
        while (k < eol && (*k == ' ' || *k == '\t'))
            ++k;
        if (k < eol && *k != ';' && *k != '#' && *k != '[') {
            size_t i = 0;
            while (i < name_len && (k + i) < eol && k[i] == name[i])
                ++i;
            if (i == name_len && (k + i) < eol && k[i] == '=') {
                const char *v = k + i + 1;
                while (v < eol && (*v == ' ' || *v == '\t'))
                    ++v;
                // R353: "auto" (any case) -> the AUTO sentinel; numbers as before.
                if (v < eol && (*v == 'a' || *v == 'A')) {
                    *value = kAuto;
                    return true;
                }
                *value = (int)strtol(v, nullptr, 10);
                return true;
            }
        }
        p = (eol < end) ? eol + 1 : end;
    }
    return false;
}

void note(const char *text) { mgpu::diag::info(text); }

// The checked call, as apply_colour_space made it before R369 (same lines). Returns true when
// SetColorSpace1 succeeded.
bool checked_set(IDXGISwapChain3 *sc3, int requested) {
    const DXGI_COLOR_SPACE_TYPE cs = (DXGI_COLOR_SPACE_TYPE)requested;
    UINT support = 0;
    const HRESULT chr = sc3->CheckColorSpaceSupport(cs, &support);
    const bool can_present = SUCCEEDED(chr) &&
                             (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;

    char line[512] = {};
    if (!can_present) {
        snprintf(line, sizeof line,
                 "[MGPU][H1] bridge chain colour space: requested %d, "
                 "CheckColorSpaceSupport hr=0x%08X support=0x%X - the chain cannot present "
                 "this colour space, SetColorSpace1 not called (frame stays sRGB gamma 2.2).",
                 requested, (unsigned)chr, (unsigned)support);
        mgpu::diag::error(line);
        return false;
    }

    t_own_call = true;    // R369
    const HRESULT shr = sc3->SetColorSpace1(cs);
    t_own_call = false;
    snprintf(line, sizeof line,
             "[MGPU][H1] bridge chain colour space: requested %d, "
             "CheckColorSpaceSupport hr=0x%08X support=0x%X -> present supported, "
             "SetColorSpace1 hr=0x%08X.%s",
             requested, (unsigned)chr, (unsigned)support, (unsigned)shr,
             SUCCEEDED(shr) ? ((requested == 1) ? " The chain now interprets the frame as scRGB linear, not sRGB."   // R353
                               : (requested == 0) ? " The chain now interprets the frame as sRGB gamma 2.2."        // R369
                                                  : " The chain now interprets the frame as ST.2084/Rec.2020, not sRGB.")
                            : " The chain still interprets the frame as sRGB gamma 2.2.");
    if (SUCCEEDED(shr))
        mgpu::diag::info(line);
    else
        mgpu::diag::error(line);
    if (SUCCEEDED(shr))
        g_set.store(requested, std::memory_order_relaxed);   // R369
    return SUCCEEDED(shr);
}

} // namespace

void apply_colour_space(IDXGISwapChain3 *sc3) {
    if (sc3 == nullptr)
        return;

    int requested = INT_MIN;
    if (!read_ini_int(L"ChainColorSpace", &requested)) {
        note("[MGPU][H1] bridge chain colour space: ChainColorSpace absent (or mgpu.ini "
             "unreadable) - SetColorSpace1 not called. The chain keeps its default "
             "sRGB gamma 2.2. Write 12 for ST.2084/PQ, 0 for sRGB.");
        return;
    }
    if (requested == kAuto) {
        // R353: follow the game's chain. What dllmain recorded last (init, then every present).
        const int g = g_game_cs.load(std::memory_order_relaxed);
        // R369: from here on follow_game_colour_space watches for a change. The test key is read once.
        g_acted.store(g, std::memory_order_relaxed);
        g_auto.store(1, std::memory_order_relaxed);
        {
            int ts = 0;
            if (read_ini_int(L"ChainColorSpaceTest", &ts) && ts > 0 && ts <= 3600) {
                g_test_s.store(ts, std::memory_order_relaxed);
                char tl[220] = {};
                snprintf(tl, sizeof tl,
                         "[MGPU][H1][R369] ChainColorSpaceTest=%d (debug): the game's colour space is "
                         "PRETENDED to be HDR10 from %d s to %d s after the first present.", ts, ts, 2 * ts);
                mgpu::diag::warn(tl);
            }
        }
        char al[300] = {};
        if (g == 1) {
            note("[MGPU][H1][R353] ChainColorSpace=auto: the game's chain is sRGB - SetColorSpace1 "
                 "not called, the chain keeps its default (sRGB gamma 2.2).");
            return;
        }
        if (g == 2) requested = 1;        // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 (scRGB linear)
        else if (g == 3) requested = 12;  // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10 PQ)
        else {
            snprintf(al, sizeof al,
                     "[MGPU][H1][R353] ChainColorSpace=auto: the game's chain colour space is %s - "
                     "SetColorSpace1 not called, the chain keeps its default (sRGB gamma 2.2). Write "
                     "12 or 1 by hand if the picture is washed out.",
                     (g == 4) ? "HDR10 HLG (no RGB DXGI colour space for it)" : "not known yet");
            note(al);
            return;
        }
        snprintf(al, sizeof al, "[MGPU][H1][R353] ChainColorSpace=auto: the game's chain is %s -> asking DXGI for %d.",
                 (g == 2) ? "scRGB linear (FP16)" : "HDR10 ST.2084", requested);
        note(al);
    }
    if (requested < 0) {
        note("[MGPU][H1] bridge chain colour space: ChainColorSpace<0 - SetColorSpace1 "
             "not called, the chain keeps its default sRGB gamma 2.2.");
        return;
    }

    checked_set(sc3, requested);
}

// R369. Bridge side, once per present, before Present. Not `auto`: one relaxed load. `auto` and
// nothing changed: three relaxed loads. A change: the same checked call the creation makes.
void follow_game_colour_space(IDXGISwapChain3 *sc3) {
    if (g_auto.load(std::memory_order_relaxed) == 0 || sc3 == nullptr)
        return;
    int g = g_game_cs.load(std::memory_order_relaxed);
    const int ts = g_test_s.load(std::memory_order_relaxed);
    if (ts > 0) {   // debug key only
        const unsigned long long now = GetTickCount64();
        unsigned long long t0 = g_test_t0.load(std::memory_order_relaxed);
        if (t0 == 0ull) { t0 = now; g_test_t0.store(t0, std::memory_order_relaxed); }
        const unsigned long long el = (now - t0) / 1000ull;
        if (el >= (unsigned long long)ts && el < 2ull * (unsigned long long)ts)
            g = 3;
    }
    if (g == 0)
        return;   // not known: keep what the chain has, wait for a known value
    if (g == g_acted.load(std::memory_order_relaxed))
        return;
    const int was = g_acted.exchange(g, std::memory_order_relaxed);   // decided once per change, never per frame
    const int set = g_set.load(std::memory_order_relaxed);
    static const char *const name[5] = { "not known", "sRGB", "scRGB linear (FP16)", "HDR10 ST.2084", "HDR10 HLG" };
    char l[360] = {};
    int requested = -1;
    if (g == 2) requested = 1;         // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
    else if (g == 3) requested = 12;   // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
    else if (g == 1) {
        if (set <= 0)
            return;                    // the chain is at sRGB already: no call, no line (as before R369)
        requested = 0;                 // back to sRGB after 12 / 1
    } else {
        snprintf(l, sizeof l,
                 "[MGPU][H1][R369] ChainColorSpace=auto: the game's chain is now %s (no RGB DXGI colour "
                 "space for it) - SetColorSpace1 not called, the chain keeps what it has.", name[g]);
        mgpu::diag::warn(l);
        return;
    }
    if (was == -1 && set == requested) {
        snprintf(l, sizeof l,
                 "[MGPU][H1][R369] ChainColorSpace=auto: the bridge chain was resized - stating the colour "
                 "space again (game's chain: %s -> %d).", name[g], requested);
    } else {
        snprintf(l, sizeof l,
                 "[MGPU][H1][R369] ChainColorSpace=auto: the game's chain changed from %s to %s after the "
                 "bridge chain was created -> asking DXGI for %d.",
                 (was >= 0 && was <= 4) ? name[was] : "not decided", name[g], requested);
    }
    mgpu::diag::info(l);
    checked_set(sc3, requested);
}

// R369. See t_own_call.
bool own_call() { return t_own_call; }

// R369. Our own ResizeBuffers ran (the format can change there): decide again on the next present.
void bridge_chain_resized() {
    if (g_auto.load(std::memory_order_relaxed) != 0)
        g_acted.store(-1, std::memory_order_relaxed);
}

void note_game_colour_space(int code) {
    if (code < 0 || code > 4)
        code = 0;
    g_game_cs.store(code, std::memory_order_relaxed);
}

} // namespace chain
} // namespace mgpu