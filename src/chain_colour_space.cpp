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
        return;
    }

    const HRESULT shr = sc3->SetColorSpace1(cs);
    snprintf(line, sizeof line,
             "[MGPU][H1] bridge chain colour space: requested %d, "
             "CheckColorSpaceSupport hr=0x%08X support=0x%X -> present supported, "
             "SetColorSpace1 hr=0x%08X.%s",
             requested, (unsigned)chr, (unsigned)support, (unsigned)shr,
             SUCCEEDED(shr) ? ((requested == 1) ? " The chain now interprets the frame as scRGB linear, not sRGB."   // R353
                                                : " The chain now interprets the frame as ST.2084/Rec.2020, not sRGB.")
                            : " The chain still interprets the frame as sRGB gamma 2.2.");
    if (SUCCEEDED(shr))
        mgpu::diag::info(line);
    else
        mgpu::diag::error(line);
}

void note_game_colour_space(int code) {
    if (code < 0 || code > 4)
        code = 0;
    g_game_cs.store(code, std::memory_order_relaxed);
}

} // namespace chain
} // namespace mgpu