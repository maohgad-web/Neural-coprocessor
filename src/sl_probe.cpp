// MGPU Bridge - SL1: the Streamline interposer probe. See sl_probe.hpp.
//
// Everything here is a loader query or one QueryInterface. No D3D12 header
// is needed: an ID3D12Device is an IUnknown for our purposes, and taking the
// argument as void* is what keeps this file free of both D3D and ReShade.
#include "sl_probe.hpp"
#include "diag.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <string.h>

namespace mgpu::slprobe
{
namespace
{
    // ---- THE DOCUMENTED THIRD-PARTY ROUTE ----
    //
    // {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}. NVIDIA's programming guide
    // names this GUID as the way a library that is not the Streamline host
    // reaches the native interface behind a proxy. It is not resolved from
    // a module and it is not an ordinal, so nothing about it can drift the
    // way NvAPI's ordinals did - a wrong GUID returns E_NOINTERFACE and the
    // answer is simply "not a proxy".
    const GUID SL_NATIVE_INTERFACE =
        { 0xADEC44E2, 0x61F0, 0x45C3,
          { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF } };

    // The modules worth naming. Order is the order they are printed in.
    // Kept as a table rather than a loop over the loader so the line reads
    // the same on every title, with absent entries visible as absent.
    const wchar_t *const SL_MODULES[] =
    {
        L"sl.interposer.dll",
        L"sl.common.dll",
        L"sl.dlss.dll",
        L"sl.dlss_g.dll",
        L"sl.dlss_d.dll",
        L"sl.reflex.dll",
        L"sl.pcl.dll",
        L"sl.nis.dll",
    };
    const char *const SL_SHORT[] =
    {
        "interposer", "common", "dlss", "dlss_g",
        "dlss_d", "reflex", "pcl", "nis",
    };
    const unsigned SL_MODULE_N =
        (unsigned)(sizeof SL_MODULES / sizeof SL_MODULES[0]);

    bool  g_said_census      = false;
    bool  g_said_acquisition = false;
    bool  g_said_shape       = false;   // SL2a
    void *g_game_device      = nullptr;

    const char *kind_name(kind k)
    {
        switch (k)
        {
        case kind::native: return "native";
        case kind::proxy:  return "SL-PROXY";
        default:           return "not-asked";
        }
    }

    // ---- VERSION, WITHOUT TOUCHING CMakeLists ----
    //
    // GetFileVersionInfo lives in version.dll. Linking it would mean editing
    // the build, and the build has been a ship blocker in this project once
    // already, so it is resolved at run time exactly as d3dcompiler_47 is in
    // the SR path. A failure here costs the version field and nothing else.
    typedef DWORD (WINAPI *pfn_gfvis)(LPCWSTR, LPDWORD);
    typedef BOOL  (WINAPI *pfn_gfvi )(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL  (WINAPI *pfn_vqv  )(LPCVOID, LPCWSTR, LPVOID *, PUINT);

    bool module_version(HMODULE m, char *out, size_t out_n)
    {
        if (out == nullptr || out_n == 0) return false;
        out[0] = '\0';
        if (m == nullptr) return false;

        wchar_t path[MAX_PATH * 2] = {};
        if (GetModuleFileNameW(m, path, MAX_PATH * 2) == 0) return false;

        HMODULE v = LoadLibraryW(L"version.dll");
        if (v == nullptr) return false;

        pfn_gfvis p_size = (pfn_gfvis)GetProcAddress(v, "GetFileVersionInfoSizeW");
        pfn_gfvi  p_get  = (pfn_gfvi )GetProcAddress(v, "GetFileVersionInfoW");
        pfn_vqv   p_q    = (pfn_vqv  )GetProcAddress(v, "VerQueryValueW");
        if (p_size == nullptr || p_get == nullptr || p_q == nullptr)
        { FreeLibrary(v); return false; }

        DWORD ignored = 0;
        const DWORD sz = p_size(path, &ignored);
        if (sz == 0) { FreeLibrary(v); return false; }

        // Bounded on purpose. A version block this large is not a version
        // block, and a heap allocation sized by another module's header is
        // not something this file is going to make.
        if (sz > 65536u) { FreeLibrary(v); return false; }

        void *blob = LocalAlloc(LPTR, sz);
        if (blob == nullptr) { FreeLibrary(v); return false; }

        bool ok = false;
        if (p_get(path, 0, sz, blob))
        {
            VS_FIXEDFILEINFO *ffi = nullptr;
            UINT n = 0;
            if (p_q(blob, L"\\", (LPVOID *)&ffi, &n) &&
                ffi != nullptr && n >= sizeof(VS_FIXEDFILEINFO))
            {
                snprintf(out, out_n, "%u.%u.%u.%u",
                         (unsigned)HIWORD(ffi->dwFileVersionMS),
                         (unsigned)LOWORD(ffi->dwFileVersionMS),
                         (unsigned)HIWORD(ffi->dwFileVersionLS),
                         (unsigned)LOWORD(ffi->dwFileVersionLS));
                ok = true;
            }
        }

        LocalFree(blob);
        FreeLibrary(v);
        return ok;
    }
} // namespace

kind classify(void *iface, void **out_native)
{
    if (out_native != nullptr) *out_native = nullptr;
    if (iface == nullptr) return kind::unknown;

    IUnknown *u = reinterpret_cast<IUnknown *>(iface);
    IUnknown *nat = nullptr;

    // E_NOINTERFACE is the ordinary answer on a title with no Streamline,
    // and it is not a fault. Only S_OK with a DIFFERENT pointer means a
    // proxy: an object that answers this GUID with itself is telling us it
    // is already the native interface.
    const HRESULT hr = u->QueryInterface(SL_NATIVE_INTERFACE, (void **)&nat);
    if (FAILED(hr) || nat == nullptr) return kind::native;

    const bool is_proxy = (reinterpret_cast<void *>(nat) != iface);
    if (out_native != nullptr && is_proxy) *out_native = nat;

    // The reference is released either way. classify() promises identity,
    // not ownership - acquire_native is the call that keeps one.
    nat->Release();
    return is_proxy ? kind::proxy : kind::native;
}

void *acquire_native(void *iface)
{
    if (iface == nullptr) return nullptr;

    IUnknown *u = reinterpret_cast<IUnknown *>(iface);
    IUnknown *nat = nullptr;
    const HRESULT hr = u->QueryInterface(SL_NATIVE_INTERFACE, (void **)&nat);
    if (FAILED(hr) || nat == nullptr) return nullptr;

    if (reinterpret_cast<void *>(nat) == iface)
    {
        // Not a proxy. Give back the reference we just took rather than
        // handing out a second one to the same object.
        nat->Release();
        return nullptr;
    }
    return reinterpret_cast<void *>(nat);
}

bool interposer_resident()
{
    return GetModuleHandleW(L"sl.interposer.dll") != nullptr;
}

void note_game_device(void *dev)
{
    if (dev != nullptr) g_game_device = dev;
}

void report(void *our_device, void *game_device)
{
    if (g_said_census) return;
    g_said_census = true;

    if (game_device == nullptr) game_device = g_game_device;

    char mods[512] = {};
    int  w = 0;
    unsigned resident = 0;
    char ver[64] = {};

    for (unsigned i = 0; i < SL_MODULE_N; ++i)
    {
        HMODULE m = GetModuleHandleW(SL_MODULES[i]);
        if (m == nullptr) continue;
        ++resident;
        if (i == 0) (void)module_version(m, ver, sizeof ver);
        if (w >= 0 && w < (int)sizeof mods - 24)
            w += snprintf(mods + w, sizeof mods - (size_t)w, "%s%s",
                          (resident > 1) ? " " : "", SL_SHORT[i]);
    }
    if (resident == 0) snprintf(mods, sizeof mods, "none");
    if (ver[0] == '\0') snprintf(ver, sizeof ver, "unknown");

    void *our_nat = nullptr, *game_nat = nullptr;
    const kind ours  = classify(our_device,  &our_nat);
    const kind theirs = classify(game_device, &game_nat);

    char line[1500];
    snprintf(line, sizeof line,
        "[MGPU][SL] STREAMLINE CENSUS: %u sl module(s) resident (%s) | "
        "sl.interposer version %s | our GPU 1 device=%s (0x%p",
        resident, mods, ver, kind_name(ours), our_device);

    size_t at = strlen(line);
    if (ours == kind::proxy)
        snprintf(line + at, sizeof line - at, " -> native 0x%p)", our_nat);
    else
        snprintf(line + at, sizeof line - at, ")");

    at = strlen(line);
    snprintf(line + at, sizeof line - at,
        " | game device=%s (0x%p%s", kind_name(theirs), game_device,
        (theirs == kind::proxy) ? " -> native " : ")");
    if (theirs == kind::proxy)
    {
        at = strlen(line);
        snprintf(line + at, sizeof line - at, "0x%p)", game_nat);
    }

    at = strlen(line);
    snprintf(line + at, sizeof line - at,
        ". HOW TO READ IT. our GPU 1 device=SL-PROXY means sl.interposer "
        "wrapped a device WE created on the SECOND adapter, so every call we "
        "make on it re-enters Streamline - which is where the 2026-09-14 "
        "Battlefield 6 fault was, on our own thread, in sl.common. The native "
        "pointer beside it is the escape: work driven on that interface does "
        "not go back through the interposer. native on both with modules "
        "resident is the ordinary case and needs nothing. A 0 module count "
        "means this title does not ship Streamline and none of this applies.");

    if (ours == kind::proxy) mgpu::diag::warn(line);
    else                     mgpu::diag::info(line);

    // SL2a. Straight after the census, on the same one-shot, so the two
    // lines are always read together and neither needs a call site.
    report_api_shape();
}

// ---- SL2a: THE API SHAPE ----
//
// Every name below is a documented public Streamline entry point. The
// measurement is presence, by GetProcAddress, and nothing is called - see
// sl_probe.hpp for why that restraint is the point rather than caution.
//
// WHAT THE ANSWER MEANS.
//
//   slSetTagForFrame present  - Streamline 2.x tagging with an explicit
//                               frame token. The current route, and the one
//                               a 2.9 interposer is expected to carry.
//   slSetTag present alone    - the deprecated single-argument tagging call.
//   both absent, interposer   - the title links Streamline but does not tag
//   resident                    through the interposer's own exports, which
//                               would make the tag route unreachable from
//                               here and is itself the finding.
//   slGetNativeInterface      - the documented escape hatch as a FUNCTION,
//                               beside the GUID the census already uses.
//   slUpgradeInterface        - present means proxies can be produced for
//                               interfaces created before slInit.
//
// This says what the surface is. It does not say the game uses it - only a
// tap on the call can say that, and a tap is an interception, which is the
// class of thing currently under suspicion on this very title. So the
// surface is measured first and separately, and the decision about the tap
// is taken with this line in hand.
void report_api_shape()
{
    if (g_said_shape) return;
    g_said_shape = true;

    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (sl == nullptr)
    {
        mgpu::diag::info(
            "[MGPU][SL2] API SHAPE: sl.interposer.dll is not loaded in this process, so there "
            "is no Streamline surface to describe and nothing on this route applies to this "
            "title. This is not a fault and needs no action.");
        return;
    }

    // The documented surface, grouped so the line reads as three questions:
    // does it tag, can it be interrogated, and can interfaces be unwrapped.
    static const char *const NAMES[] =
    {
        "slInit", "slShutdown",
        "slSetTagForFrame", "slSetTag",
        "slGetNewFrameToken", "slSetConstants",
        "slIsFeatureSupported", "slIsFeatureLoaded", "slSetFeatureLoaded",
        "slGetFeatureVersion", "slGetFeatureRequirements",
        "slGetFeatureFunction", "slEvaluateFeature",
        "slAllocateResources", "slFreeResources",
        "slGetNativeInterface", "slUpgradeInterface",
        "slSetD3DDevice",
    };
    const unsigned N = (unsigned)(sizeof NAMES / sizeof NAMES[0]);

    char have[900] = {};
    char miss[600] = {};
    int hw = 0, mw = 0;
    unsigned nhave = 0;
    bool tag_frame = false, tag_old = false;

    for (unsigned i = 0; i < N; ++i)
    {
        const bool present = (GetProcAddress(sl, NAMES[i]) != nullptr);
        if (present)
        {
            ++nhave;
            if (strcmp(NAMES[i], "slSetTagForFrame") == 0) tag_frame = true;
            if (strcmp(NAMES[i], "slSetTag") == 0)         tag_old   = true;
            if (hw >= 0 && hw < (int)sizeof have - 32)
                hw += snprintf(have + hw, sizeof have - (size_t)hw,
                               "%s%s", (nhave > 1) ? " " : "", NAMES[i]);
        }
        else if (mw >= 0 && mw < (int)sizeof miss - 32)
        {
            mw += snprintf(miss + mw, sizeof miss - (size_t)mw,
                           "%s%s", (mw > 0) ? " " : "", NAMES[i]);
        }
    }
    if (have[0] == '\0') snprintf(have, sizeof have, "none");
    if (miss[0] == '\0') snprintf(miss, sizeof miss, "none");

    const char *route =
        tag_frame ? "TAGGED, slSetTagForFrame (Streamline 2.x, frame-token form)"
                  : (tag_old ? "TAGGED, slSetTag only (the deprecated form)"
                             : "NOT TAGGED THROUGH THE INTERPOSER'S EXPORTS");

    char l[2000];
    snprintf(l, sizeof l,
        "[MGPU][SL2] API SHAPE: %u of %u documented entry point(s) exported | ROUTE=%s | "
        "present: %s | absent: %s. NOTHING WAS CALLED - this is GetProcAddress and a null "
        "check, so no Streamline struct crossed this boundary and no ABI was assumed. HOW TO "
        "READ IT. ROUTE=TAGGED means the game's own depth, motion vector, colour and UI "
        "buffers are declared to Streamline inside this process, with extent and format "
        "attached. That declaration is AUTHORITATIVE where our candidate ranking is a guess: "
        "on this title 15 motion-vector candidates were identical in size, format and bind "
        "count, so the ranking is decided by a barrier tiebreak and the source has been seen "
        "to change between reports. It is also where a HDR colour buffer is NAMED rather than "
        "inferred from R10G10B10A2. ROUTE=NOT TAGGED closes that route and is worth just as "
        "much: it says the declaration is not reachable from here and the barrier path stays "
        "the only answer. Reading the tag VALUES needs a tap on the call, which is an "
        "interception of the same class as the calibrator, and that is a separate decision "
        "taken with this line in hand - not a thing this build does.",
        nhave, N, route, have, miss);
    mgpu::diag::info(l);
}

void report_acquisition(unsigned long long bind_fires,
                        unsigned long long rp_fires,
                        unsigned long long bar_fires)
{
    if (g_said_acquisition) return;
    // Nothing to say until at least one lane has fired; a run that is asked
    // too early would otherwise report a blindness that is only earliness.
    if (bind_fires == 0 && rp_fires == 0 && bar_fires == 0) return;
    g_said_acquisition = true;

    const bool blind = (bind_fires == 0 && rp_fires == 0 && bar_fires > 0);

    char line[1200];
    snprintf(line, sizeof line,
        "[MGPU][SL] BIND PATH VISIBILITY: OMSetRenderTargets fired=%llu | "
        "BeginRenderPass fired=%llu | ResourceBarrier fired=%llu -> %s. "
        "HOW TO READ IT. No D3D12 engine binds no render targets, so zero on "
        "BOTH bind events with a live barrier count does not describe an "
        "engine - it describes something sitting between the game and ReShade "
        "on the bind path and not on the barrier path. Read it beside the "
        "STREAMLINE CENSUS line: resident sl modules with a blind bind path "
        "is the shape Battlefield 6 showed over 103,000 frames, and on that "
        "engine the barrier lane was the only signal the acquisition probe "
        "had. Both lanes live is the ordinary case and the ranked pick means "
        "what it says.",
        bind_fires, rp_fires, bar_fires,
        blind ? "BIND PATH IS BLIND TO US" : "bind path visible");

    if (blind) mgpu::diag::warn(line);
    else       mgpu::diag::info(line);
}

} // namespace mgpu::slprobe
