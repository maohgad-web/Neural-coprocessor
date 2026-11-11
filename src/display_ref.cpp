// [PORT H6, 2026-11-11] THE REFERENCE TREE'S OWN PRESENTATION LAYER, AS A TU.
//
// WHY THIS FILE EXISTS. The operator's verdict on the 8-slot layer was "其他的都会闪"
// (everything else flickers), and the ask was explicit: replace the working tree's
// presentation layer with the reference tree's one and run it once. This file is that
// replacement: ref-fg-driver/src/display_layer.inc (95 KB, 6-slot ring, R448 cut 3,
// the layer XeFG's own slices were measured against) compiled as its own translation
// unit, with a thin adapter that exposes the same mgpu::presenter::* names the rest of
// the add-on already calls.
//
// IT IS ONE TU ON PURPOSE, AND IN THE SAME ORDER AS THE REFERENCE. xefg_smoke.inc
// brings the mirrored Intel ABI (efg_api / xell_api), display_layer.inc is the layer,
// and xefg_display.inc is the XeFG slice that the layer calls DIRECTLY (its forward
// declarations sit at the top of display_layer.inc). Splitting them - as the previous
// round did through presenter.hpp's hook table - is what left the table empty for two
// runs.
//
// WHAT IS *NOT* HERE, DELIBERATELY: the 8-slot ring, the absolute pacer (R530/R532),
// the burst-drop rules (R526), DispDirect, the zombie killer and the panel-in-presenter
// path. Those are nc-base's additions on top of this layer; the point of this arm is to
// run WITHOUT them. The two host services the reference layer expects from
// gpu1_context.cpp - st() and the marquee - are shimmed below.

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "diag.hpp"
#include "fg_mode.hpp"        // mgpu::fgmode: who owns the output, and the multiplier
#include "fg_camera.hpp"      // mgpu::fgcamera: the projection XeFG's frame constants carry
#include "presenter.hpp"      // the DECLARATIONS the rest of the add-on calls, and its own
                              // mgpu::presenter::disp_fg_in / disp_fg_params
#include "gpu1_context.hpp"   // mgpu::gpu1::ui_ini_read, display_refresh_hz

#include "../third_party/xess_fg/xefg_swapchain.h"
#include "../third_party/xess_fg/xefg_swapchain_d3d12.h"
#include "../third_party/xess_fg/xell.h"
#include "../third_party/xess_fg/xell_d3d12.h"

// NOTE ON THE TWO PAIRS OF STRUCTS. presenter.hpp defines mgpu::presenter::disp_fg_in /
// disp_fg_params (field-for-field the same four and six fields), and display_layer.inc
// defines ::disp_fg_in / ::disp_fg_params at GLOBAL scope. Different names, identical
// layout - which is how the reference tree and nc-base drifted apart - and why the
// adapter below crosses between them with an explicit cast instead of a `using`
// (a using-declaration at global scope makes the two collide: C2011 at
// display_layer.inc:49, measured).

// ---- the two host services the reference layer reads from its includer ----
//
// st().device / st().queue: in the reference tree these are the bridge's own D3D12
// objects (gpu1_context.cpp's stream state). Here they are the same two objects, handed
// in by disp_init() before the layer is asked to create anything.
// mq_*: the "marquee" - a colour-coded frame tag stamped into the picture for
// measurement runs. Not ported: both stubs say "nothing to draw", which is what the
// layer does when those keys are absent in the reference tree too.
namespace
{
struct disp_host_state
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
};
disp_host_state &st()
{
    static disp_host_state s{};
    return s;
}

// The marquee stubs must live in the SAME unnamed namespace display_layer.inc declares
// them in, or its forward declaration and this definition are two different functions
// and every call site becomes ambiguous (measured: C2668 at display_layer.inc:1537).
bool mq_tag_copy(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT, UINT, int, int) { return false; }
ID3D12Resource *mq_marker(unsigned) { return nullptr; }
}

// The reference tree keeps this in gpu1_context.cpp. True while the operator's panel is
// drawn over the picture: the output steps aside instead of fighting it for the screen.
std::atomic<bool> g_panel_live{false};

// Forward declarations the reference tree keeps in gpu1_context.cpp just before the
// include (it is the includer that knows the display layer exists). display_layer.inc
// calls both from disp_init.
bool mgpu_xefg_handover_wanted();
bool mgpu_xefg_display_handover(ID3D12Device *dev, ID3D12CommandQueue *q, HWND hwnd,
                                const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIFactory2 *fac,
                                IDXGISwapChain1 **out_sc, IDXGISwapChain3 **out_sc3,
                                const char *where);

// the prelude xefg_smoke.inc expects (its four context items, identical to xefg_smoke.cpp)
static const char *xefg_result_name(int r)
{
    switch (r)
    {
    case 0:   return "SUCCESS";
    case -2:  return "ERROR_INVALID_PARAMETER";
    case -3:  return "ERROR_OUT_OF_MEMORY";
    case -15: return "ERROR_LATENCY_REDUCTION_UNSUPPORTED";
    default:  return "ERROR(see XeSS-FG SDK)";
    }
}
static bool addon_dir_w(wchar_t *out, size_t n)
{
    out[0] = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&addon_dir_w, &hm) || hm == nullptr)
        return false;
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD len = GetModuleFileNameW(hm, path, MAX_PATH * 2);
    if (len == 0 || len >= MAX_PATH * 2) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    slash[1] = 0;
    wcsncpy_s(out, n, path, _TRUNCATE);
    return true;
}
static bool file_exists_w(const wchar_t *p)
{
    return p != nullptr && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

#include "xefg_smoke.inc"
#include "display_layer.inc"
#include "xefg_display.inc"

// ---------------------------------------------------------------------------
// The adapter: mgpu::presenter::* -> the reference layer's own functions.
// ---------------------------------------------------------------------------
//
// FORWARDERS IN A NAMED NAMESPACE FIRST. The layer's functions live in an unnamed
// namespace, so an adapter that called `disp_push(...)` from inside
// namespace mgpu::presenter would find ITSELF and recurse. Unqualified lookup from a
// sibling namespace finds the unnamed-namespace one, which is exactly what is wanted.
namespace refdisp
{
inline bool push(ID3D12Resource *src, D3D12_RESOURCE_STATES rest, double t_ms,
                 int tag_color, int tag_code, int patch_phase, double max_late,
                 const disp_fg_in *fg, const disp_fg_params *fpp, unsigned grp)
{
    return disp_push(src, rest, t_ms, tag_color, tag_code, patch_phase, max_late, fg, fpp, grp);
}
inline bool init(UINT w, UINT h, DXGI_FORMAT f) { return disp_init(w, h, f); }
inline bool alive() { return disp_alive(); }
inline void stop() { disp_stop(); }
inline void panel(bool hide) { disp_set_panel(hide); }
inline bool on_screen()
{
    return disp_alive() && !dp().panel_hide.load(std::memory_order_relaxed);
}
}   // namespace refdisp

namespace mgpu
{
namespace presenter
{
bool enabled()
{
    // The same one-shot flag-file check the layer has always had, moved here because
    // this TU is now the layer. Absent file = the shipped single-Present path, and
    // nothing in this file runs.
    static int on = -1;
    if (on < 0)
    {
        on = 0;
        wchar_t me[MAX_PATH * 2] = {};
        const DWORD mn = GetModuleFileNameW(nullptr, me, (DWORD)(MAX_PATH * 2));
        if (mn != 0 && mn < (DWORD)(MAX_PATH * 2))
        {
            wchar_t *sl = wcsrchr(me, L'\\');
            if (sl != nullptr)
            {
                *(sl + 1) = L'\0';
                wcscat_s(me, L"mgpu_display_thread.on");
                const DWORD at = GetFileAttributesW(me);
                if ((at != INVALID_FILE_ATTRIBUTES) && ((at & FILE_ATTRIBUTE_DIRECTORY) == 0)) on = 1;
            }
        }
        mgpu::diag::info(on ? "[MGPU][R448] display layer requested (mgpu_display_thread.on)"
                            : "[MGPU][R448] display layer not requested (no mgpu_display_thread.on)");
    }
    return on == 1;
}

bool shadow_only() { return false; }

bool disp_init(ID3D12Device *dev, ID3D12CommandQueue *bq, UINT w, UINT h, DXGI_FORMAT fmt)
{
    // The reference layer takes its device and queue from the includer; here the
    // caller hands them in. Both are the bridge's own objects, which is the same
    // arrangement the reference has by construction.
    st().device = dev;
    st().queue = bq;
    return refdisp::init(w, h, fmt);
}

bool disp_push(ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, double t_ms,
               int tag_color, int tag_code, int patch_phase, double max_late_ms,
               const disp_fg_in *fg, const disp_fg_params *fpp, unsigned grp, bool /*is_gen*/)
{
    // is_gen is nc-base's per-frame burst flag; the reference layer carries the group id
    // (grp) and treats it as indivisible, so the flag has no reader here and is dropped
    // rather than faked. The cast crosses the two identically-shaped struct pairs.
    return refdisp::push(src, src_state, t_ms, tag_color, tag_code, patch_phase, max_late_ms,
                         reinterpret_cast<const ::disp_fg_in *>(fg),
                         reinterpret_cast<const ::disp_fg_params *>(fpp), grp);
}

double disp_now_ms() { return ::disp_now_ms(); }
double disp_source_period_ms() { return ::disp_source_period_ms(); }
double disp_vblank_ms() { return ::disp_vblank_ms(); }
unsigned disp_burst_id() { return ::disp_burst_id(); }
bool disp_alive() { return refdisp::alive(); }
bool disp_on_screen() { return refdisp::on_screen(); }
void disp_stop() { refdisp::stop(); }
void disp_set_panel(bool hide) { refdisp::panel(hide); }
void disp_set_panel_live(bool on) { refdisp::panel(on); }

// The reference layer has no separate pacer arm: it sleeps to each frame's own
// timestamp (its [R575]/[R579] logic), so there is nothing to switch off.
void disp_set_pace(int) {}
int  disp_pace() { return 1; }

// No zombie killer in this layer (nc-base added one after a measured 4.8 GB zombie).
// The signals stay accepted so every caller compiles; the exit behaviour they guard is
// named in the report for this arm. disp_zombie_kill_now reports false = "did not have
// to kill anything", which is the truth here.
void disp_set_zombie_kill_ms(int) {}
void disp_begin_dying() {}
bool disp_zombie_kill_now() { return false; }
void disp_note_teardown() {}

// The XeFG slice is compiled into THIS T.U. and the layer calls it directly - that is
// the reference arrangement. There is no hook table here at all, so nothing fills one.

}   // namespace presenter
}   // namespace mgpu
