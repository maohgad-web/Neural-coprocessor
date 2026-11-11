// MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
//
// T1: register the add-on with stock ReShade, write one init log line.
// T2: per device event, a provisional game-LUID capture (first d3d12
//     device) and a one-time adapter-table enumeration; the selection
//     itself is deferred to the swapchain-derived game LUID (init_swapchain)
//     - the four rules of brief section 06 live in adapter.hpp/adapter.cpp.
// T3: the bridge thread (spawned on the game thread from
//     on_init_swapchain only, never from on_init_device and never from
//     DllMain) creates the private device on the selected adapter;
//     destroy_device events are logged, and the game's own device
//     release triggers the bridge thread's orderly teardown.
// T4: captures this module's own HMODULE at DLL_PROCESS_ATTACH and exposes
//     it via mgpu::module_handle() - the window class's hInstance must be
//     the add-on's handle, and GetModuleHandle(nullptr) returns the game's
//     module, not ours (brief section 00, exception 1).
//
// Convention verified against crosire/reshade v6.8.0
// (18deaa52de0c425a78b329e9cb3c497281cd00ec), include/reshade.hpp,
// include/reshade_events.hpp and examples/01-fps_limit: add-ons export
// NAME and DESCRIPTION, and DllMain calls reshade::register_addon(hModule),
// which fails cleanly if ReShade is absent or its add-on API is older
// than RESHADE_API_VERSION.
//
// API 20 note (verified in reshade_events.hpp at the pinned SHA): there
// is no reshade_init / reshade_unload addon event, and no
// device_removed / device_restored. The device lifecycle event is
// destroy_device - void (api::device *) - fired before
// ID3D12Device::Release. Teardown is therefore driven from
// destroy_device on the game's device.
//
// get_native() returns uint64_t: unwrapping it to ID3D12Device * needs
// reinterpret_cast, not static_cast (integer to pointer is only
// reinterpret_cast). GetAdapterLuid() takes no parameters and returns
// the LUID by value - there is no SUCCEEDED to check.

#include <windows.h>
#include <d3d12.h>
#include <dwmapi.h>   // [PORT 2/3b] DwmGetCompositionTimingInfo: the output refresh, independent of our presents
#include <dxgi.h>   // R241d: IDXGISwapChain::GetFullscreenState
#include <cstdio>     // P1.6: snprintf. This file had no formatted logging before.
#include <cstring>    // R137: strstr, to spot the depth tap by its effect name.
#include <cwchar>     // R142: wcsrchr, to split our own module path for [R142].
#include <atomic>     // R138: the present counter and the said-once flags.
#include <chrono>     // T1.1 FrameCap (FrameCap line present only)
#include <thread>     // T1.1 FrameCap

// ---- R141: BUILD IDENTITY, SAID ONCE, FIRST ----
//
// The add-on has never logged what it is. ReShade prints
// 'Registered add-on "MGPU Bridge" v0.0.0.0' - the version field was never
// filled in - so every report has had to identify the build by the FILE SIZE
// IN BYTES. Two days of issue 15 triage were spent that way, and one run was
// nearly taken against a stale binary because 382,464 and 416,768 look alike
// in a folder listing.
//
// __DATE__ and __TIME__ are deliberate: they need no CI change and no build
// system co-operation, and a timestamp cannot be stale in the way a version
// constant that somebody forgot to bump can be. The version string beside it
// is bumped by hand at release and the two disagreeing is itself information.
// R173. BUMPED 2026-09-19, and the comment above had already named this exact
// hazard: "the version string beside it is bumped by hand at release and the
// two disagreeing is itself information". It was not bumped when this tree
// became 0.2.4, so the first Cyberpunk run of the R167 build reported
// "MGPU Bridge 0.2.3 | built Sep 19 2026 14:44:47" - a correct timestamp under
// a wrong name, on the one line whose entire job is to stop a report
// misidentifying a build. Caught by a control run rather than by a reader,
// which is the only reason it is not in somebody's issue thread.
// R141: the log must say which build wrote it. 0.2.5 + R210 + R208 (FP16, off).
// R369: bumped to 0.5.1. It was NOT bumped for 0.5.0 - every 0.5.0 log says "MGPU Bridge 0.4.0" (the
// R173 hazard above, again; the first 0.5.0 report quoted it).
#define MGPU_VERSION_STR "0.5.1"


// ---- P6.4: the overlay panel ----
//
// reshade.hpp wires up the ImGui function table ONLY when IMGUI_VERSION_NUM is
// already defined when it is included - the block is #if'd on that - and the
// table is bound inside register_addon. So imgui.h has to come first, in THIS
// translation unit, because this is where register_addon is called.
//
// GUARDED, because the ImGui headers are a dependency this repo may not carry
// and a build that fails on the rig at 3am is worse than a build with no panel.
// Without them the add-on compiles exactly as before and the P6.3 hotkeys are
// the control; the startup log says which of the two you got, so a missing
// panel is never a mystery.
//
// P7.1: TWO THINGS THE HEADER DEMANDS AND WILL NOT INFER.
//   1. ImTextureID must be 8 bytes. reshade_overlay.hpp has a static_assert on
//      exactly that, because ReShade passes a resource_view through it. ImGui's
//      default is a void*, which IS 8 bytes on x64 - the assert would pass - but
//      ReShade itself is compiled with ImTextureID=ImU64 and a type that merely
//      happens to be the same width is not the same type in the structs either
//      side of the table. Define it, the way ReShade's own build does.
//   2. The version must be EXACTLY 19250 (ImGui 1.92.5). reshade_overlay.hpp
//      #errors on anything else - the function table is a version-numbered
//      struct of raw pointers, so a near-miss is a silent ABI mismatch and the
//      header refuses rather than letting it happen. Vendor that exact tag.
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    define ImTextureID ImU64
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "presenter.hpp"      // [PORT PRESENTER H2] the display layer (nc-base, byte-identical)
#include "ngx_hook.hpp"   // [PORT B2] the NGX contract at nvngx's own entry
#include "ffx_hook.hpp"   // [FFX] the FSR contract at ffxDispatch (same table, other API)
#include "nr16.hpp"       // R219-2: the panel reads the 16-bit keys with nr16's reader
#include "residual.hpp"   // R319a: one relaxed load, to word the x2 line
#include "chain_colour_space.hpp"   // R353: ChainColorSpace=auto follows the game chain's colour space
#include "discovery.hpp"  // D1 (0.3.0 R&D): the discovery calibrator, off unless Discovery=1
#include "live_panel.hpp"     // PanelOverPicture (N01950): own file, off unless PanelOverPicture=1
#include "panel_simple.hpp"   // the player-facing panel; its layout is panel_simple.txt
#include "latency_probe.hpp"  // D1.5a (0.3.0 R&D): read-only latency probe, off unless LatencyProbe=1
#include "journal.hpp"        // D2.0: what / when / why journal, off unless Journal=1
#include "stall_watch.hpp"    // D2.0: stall watch, off unless StallWatch=N
#include "own_reflex.hpp"     // D2.0: our Reflex for case-B titles, off unless OwnReflex=1
#include "log_queue.hpp"      // D2.0: the log queue (its worker starts only with a D2.0 line)
#include "fg_map.hpp"         // D2.0 NR step one: read-only frame-gen vector map, off unless NRFrameFilter=1
#include "fg_gpu1.hpp"        // [PORT FG-PANEL] hot on/off from the FULL panel too (ui_set_enabled)
#include "dx11_probe.hpp"     // DX11 transport-hop probe, off unless Dx11Probe=1 (DX11_CONTRACT_LEDGER.md)
#include "dx11_producer.hpp"  // R227: the DX11 producer, off unless DX11=1 (DX11_CONTRACT_LEDGER.md section 7)
#include "mvec_census.hpp"
#include "mvec_extract.hpp"   // R280: the virtual velocity target (D3D12, no NGX contract) - own file pair, own keys
#include "tex_census.hpp"     // R279: TexCensus=1 instrument, off by default (removable)    // R253: what a D3D11 title exposes for vectors, off unless MVecCensus=1 (section 13)
#include "calibrator.hpp"
#include "probe.hpp"
#include "sl_probe.hpp"   // SL1
#include "sl_tags.hpp"    // R300c/R300d (Dom's R220/R221): the Streamline-tagged vectors
#include "t119_keys.hpp"  // R300 (T1.19): Dom's keys, every one absent = off
#include "startup_trace.hpp" // R313
#include "worker.hpp"

extern "C" __declspec(dllexport) const char *NAME = "MGPU Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Cross-Adapter Bridge P0: second ReShade effect runtime on a second GPU";

#define MGPU_STR2(s) #s
#define MGPU_STR(s) MGPU_STR2(s)

// T4 (brief section 00, exception 1): the add-on's own module handle,
// captured at DLL_PROCESS_ATTACH. GetModuleHandle(nullptr) returns the
// *game's* module, not ours, so the T4 window class's hInstance must be
// this handle. The accessor is declared in worker.cpp (the file manifest
// is closed, so no new header is added); it is ordinary C++ linkage,
// resolved within this DLL at link time.
static HMODULE g_module = nullptr;

namespace mgpu
{
    HMODULE module_handle()
    {
        return g_module;
    }
}

// T2: both init_device and init_swapchain are the game's own separate API
// calls - ReShade's CreateDXGIFactory1 hook is fully unwound when they
// fire, so the enumeration's factory creation does not recurse into
// add-on init. (Swapchain CREATION stays off the game thread entirely -
// that is the re-entrancy hazard, handled in T4+.)
// The bridge thread is deliberately NOT started here. UE5 probes every
// adapter before settling, and ReShade loads and unloads this add-on once
// per probe - five cycles per launch on this rig. A thread started here
// would wait on a ready event that cannot be set (rule 2: no selection is
// possible before a swapchain exists), and would then be torn down inside
// ReShade's unload window. That killed the game process on the rig: the
// module unmaps while the thread is still executing inside it and the
// thread faults on unmapped code, with no log line possible because the
// code that would write it is gone. It is a race - the same binary
// launched cleanly minutes earlier - so the fix is to have no thread
// during a probe cycle at all, not to make the teardown faster.
// ensure_started() therefore lives only in on_init_swapchain, which fires
// once, on the real render device.
static void on_init_device(reshade::api::device *device)
{
    // Every device event is logged; the first d3d12 one also captures the
    // provisional game LUID and builds the adapter table. No selection
    // here - brief rule 2 defers it to the swapchain-derived game LUID
    // (on_init_swapchain).
    mgpu::adapter::on_device(device);

    // ---- R101b RETRACTED. init_device was the wrong place. ----
    //
    // MEASURED: Dragon Sword and Dawnwalker stopped LAUNCHING - "Failed to
    // load add-on" on a SECOND D3D12CreateDevice, after the first instance
    // had unregistered. Plague Tale survived because its init order creates
    // the device once.
    //
    // The install site was not the real defect; it EXPOSED one. We patch
    // GetProcAddress slots in ~150 modules and nothing ever put them back, so
    // an add-on unload left every one of those slots pointing into a DLL that
    // is no longer there. install() at init_device made that unload/reload
    // cycle happen with the patches live. uninstall() is now called at
    // detach - see DllMain - and the install goes back to the swapchain
    // one-shot, which launched on every title.
    //
    // The early-install idea is NOT dead, but it needs a module walk that
    // does not depend on ToolHelp and an unload path that is proven first.
}

// ---- R78: THE MOTION VECTOR TRANSPORT'S ONE LINE OF GLUE ----
//
// The probe calls this from the render-target bind event, mid-frame, with the
// game's command list already open and the source already transitioned to
// copy_source - the probe issues that barrier and puts it back, because the
// barrier is a ReShade type and gpu1_context holds none.
//
// A FUNCTION POINTER RATHER THAN A CALL. probe.cpp does not include
// gpu1_context.hpp and must not start: the probe is a diagnostic that has to
// keep working in a build where the stream is inert, and a direct call would
// make the acquisition layer depend on the transport layer. So the transport
// hands the probe a pointer at startup and the probe knows nothing about what
// is on the other end of it.
//
// Inert unless MVec=3 and the stream is armed - the check is inside
// stream_mvec_copy, under the mutex that owns the answer.
// D2.0: finish_present is subscribed in DllMain with every other event; with
// OwnReflex off the handler returns after one load. Declared here because
// on_finish_present is defined beside on_present.
static void on_finish_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain);

static void mgpu_mvec_transport_hook(void *cmd_list_native, unsigned long long resource)
{
    mgpu::gpu1::stream_mvec_copy(cmd_list_native, resource);
}
// R300d (Dom's R221, T1.5). SLT1 callback: capture while the game's ResourceTag is valid. Installed only
// with SLTagsCapture=1|2 (on_init_swapchain, after the ini is read).
static void mgpu_tagged_mvec_capture_hook(void *cmd_list_native, unsigned long long resource,
                                          unsigned declared_state, int lifecycle)
{
    mgpu::gpu1::stream_mvec_copy_tagged(cmd_list_native, resource, declared_state, lifecycle);
}
// R232: the D3D11 twin - the game's context and its DLSS MotionVectors go to
// the DX11 producer, which hops them. Inert unless DX11=1 (one load).
static void mgpu_mvec_transport_hook_d3d11(void *context, unsigned long long resource)
{
    mgpu::dx11producer::on_mvec(context, reinterpret_cast<void *>(static_cast<uintptr_t>(resource)));
}

// R241d. DirectComposition composes through DWM, and DWM does not compose
// over an exclusive-fullscreen surface: a ROOTED visual shows nothing (Skyrim
// SK-3: Windowed=FALSE, DISCARD, ALLOW_MODE_SWITCH; NR on screen per the log,
// nothing visible). Our own IDXGISwapChain query on the game chain, one line.
static void note_game_fullscreen(reshade::api::swapchain *swapchain, const char *when)
{
    if (swapchain == nullptr || !mgpu::gpu1::dcomp_overlay_mode()) return;
    IDXGISwapChain *sc = reinterpret_cast<IDXGISwapChain *>(static_cast<uintptr_t>(swapchain->get_native()));
    if (sc == nullptr) return;
    BOOL fs = FALSE;
    if (FAILED(sc->GetFullscreenState(&fs, nullptr))) return;
    if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_GT_SWAP, "game-fullscreen-state", fs ? "EXCLUSIVE" : "windowed/borderless");   // R313
    char l[640];
    if (fs)
    {
        snprintf(l, sizeof l,
                 "[MGPU][R241] the game's swap chain is EXCLUSIVE FULLSCREEN (%s). With DcompOverlay=1 the neural "
                 "output is a GPU 1 surface that reaches the display only through DWM composition; while the game "
                 "holds the display's direct flip (fullscreen, with or without optimizations) it is NOT shown - it "
                 "appears only on alt-tab. Set the game to windowed or borderless (Skyrim launcher: Windowed Mode + "
                 "Borderless), or leave DcompForceWindowed on so the add-on does it (R242).", when);
        mgpu::diag::error(l);
    }
    else
    {
        snprintf(l, sizeof l, "[MGPU][R241] the game's swap chain is windowed/borderless (%s): DirectComposition can show over it.", when);
        mgpu::diag::info(l);
    }
}
// R242. With DcompOverlay=1 on a D3D11 title, the composed output is not
// shown while the game holds the display's direct flip (fullscreen with
// optimizations: visible only on alt-tab, SK-3). Our output is a GPU 1
// surface and reaches a GPU 0 display only through DWM composition, so the
// game's chain must be windowed: created windowed here, and a later request
// for fullscreen refused. The game's fullscreen window is already a popup
// covering the monitor, so windowed in it is borderless - ReShade's own
// ForceWindowed does the same. D3D12 titles and DcompOverlay=0 are untouched.
// DcompForceWindowed=0 turns it off. Our own composition chain is D3D12 and
// never reaches the condition.
static std::atomic<unsigned> g_force_windowed_n{0};
static int g_mvec_tap12 = 0;   // R277: MVecTap on a D3D12 game chain, read once at init
static bool g_r280o_eval_yield = false;   // R280o: the evaluate route has delivered - the tap yields for the session (game render thread only)
// R273c. The R242 rule under the R273 detection: on a rig with more than one
// display the composition decision can still be PENDING when the game creates
// its first D3D11 chain (create_swapchain fires before the chain exists, so
// before the window and the game's card are known). A chain created exclusive
// fullscreen there would hold the display's direct flip, and a composition
// accepted a moment later would show nothing - SK-3's symptom, with no error
// anywhere. So while the decision is pending the chain is kept windowed as if
// composition were on; if the decision then refuses, the game simply runs
// windowed under the own-window mode, which works. Single display: the mode
// is never pending, so this is R242 exactly as it was.
static bool force_windowed_active(reshade::api::device_api api)
{
    // R277: D3D12 too. RE4 (2026-10-07) asked for exclusive fullscreen every
    // frame for two minutes; a D3D12 title that KEEPS it hides the composed
    // output exactly as SK-3 did on D3D11 (the chain holds the direct flip).
    // Same ReShade events, same mechanism; DcompForceWindowed=0 opts out.
    // R277c: back to D3D11 only. RE4-4 (mv52): refusing a D3D12 title's
    // exclusive request left it in a BORDERED window; on its own the game
    // ended borderless. A D3D12 title that really keeps exclusive still hides
    // the composition - a known limit until a title shows it.
    if (api != reshade::api::device_api::d3d11 || !mgpu::gpu1::dcomp_force_windowed()) return false;
    return mgpu::gpu1::dcomp_overlay_mode() || mgpu::gpu1::dcomp_overlay_undecided();
}
static bool on_create_swapchain(reshade::api::device_api api, reshade::api::swapchain_desc &desc, void *)
{
    if (!force_windowed_active(api) || !desc.fullscreen_state) return false;
    desc.fullscreen_state = false;
    const unsigned n = g_force_windowed_n.fetch_add(1) + 1;
    char l[360];
    snprintf(l, sizeof l,
             "[MGPU][R242] the game asked for an EXCLUSIVE FULLSCREEN D3D11 chain; created WINDOWED instead so the composed "
             "neural output can show (DcompOverlay=1%s). Its window already covers the monitor: borderless in effect. "
             "DcompForceWindowed=0 in mgpu.ini leaves the game's mode alone. (%u so far)",
             mgpu::gpu1::dcomp_overlay_undecided() ? ", composition decision still PENDING - kept windowed so it can show if accepted; if refused the game runs windowed under the own window" : "", n);
    mgpu::diag::warn(l);
    return true;
}
static bool on_set_fullscreen_state(reshade::api::swapchain *swapchain, bool fullscreen, void *)
{
    reshade::api::device *dev = (swapchain != nullptr) ? swapchain->get_device() : nullptr;
    if (fullscreen && dev != nullptr && force_windowed_active(dev->get_api()))
    {
        const unsigned n = g_force_windowed_n.fetch_add(1) + 1;
        if (n <= 8u || (n % 100u) == 0u)
        {
            char l[300];
            snprintf(l, sizeof l, "[MGPU][R242] the game asked to enter EXCLUSIVE FULLSCREEN (set_fullscreen_state); refused, "
                                  "the chain stays windowed so the composed output can show. (%u so far)", n);
            mgpu::diag::warn(l);
        }
        return true;   // prevent
    }
    // R277: capped like the R242 lines. RE4 called SetFullscreenState every
    // frame: 5156 observe lines in two minutes, one log post per frame on the
    // game thread (G1).
    static std::atomic<unsigned> s_observe_n{0};
    const unsigned on = s_observe_n.fetch_add(1) + 1;
    if (on <= 8u || (on % 100u) == 0u)
    {
        char l[240];
        snprintf(l, sizeof l, fullscreen ? "[MGPU][R241] set_fullscreen_state -> EXCLUSIVE FULLSCREEN: the composed neural output will not be visible from here (%u so far)"
                                         : "[MGPU][R241] set_fullscreen_state -> windowed: the composed neural output can be visible again (%u so far)", on);
        mgpu::diag::info(l);
    }
    return false;   // observe only
}

// R353: the GAME chain's colour space, as ReShade reports it, handed to the colour-space module as a
// small code of our own (no ReShade type crosses the file boundary). Game chain only: the bridge's own
// chain raises init_swapchain on the bridge thread, and its present never comes through on_present
// with the game chain's native. One relaxed store; nothing else.
static std::atomic<void *> g_r353_game_chain{nullptr};
static void r353_note_colour_space(reshade::api::swapchain *swapchain)
{
    if (swapchain == nullptr) return;
    int code = 0;
    switch (swapchain->get_color_space())
    {
    case reshade::api::color_space::srgb_nonlinear:       code = 1; break;
    case reshade::api::color_space::extended_srgb_linear: code = 2; break;
    case reshade::api::color_space::hdr10_st2084:         code = 3; break;
    case reshade::api::color_space::hdr10_hlg:            code = 4; break;
    default:                                              code = 0; break;
    }
    mgpu::chain::note_game_colour_space(code);
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    // D2.0: journal and stall watch read their keys once, here (never in
    // DllMain). Off: one ini read each, then one load.
    mgpu::lq::rearm();   // a new game device after a destroy_device in the same load
    // D2.0-9: start the log worker now, whatever the keys. From here on every
    // mgpu::diag line is posted to the queue and written by the worker, never
    // on the game's render thread or under s.cs (Requiem, C6 14:42:19-25:
    // our lines took 0.4-1.4 s each to write; DEFECT E).
    mgpu::lq::start();
    // R313: StartupTrace / DcompSafeMode keys, read once here (the same place the
    // journal and the stall watch read theirs - never in DllMain). Key absent:
    // one ini read, then every trace point is one relaxed load.
    mgpu::strace::init();
    const double r313_t_enter = mgpu::strace::now_ms();
    // The bridge's own chain raises this event on the bridge thread: the role follows the thread.
    const mgpu::strace::role r313_role = mgpu::strace::is_bridge_thread() ? mgpu::strace::R_BT : mgpu::strace::R_GT_SWAP;
    if (mgpu::strace::on())
        mgpu::strace::ev(r313_role, "init_swapchain-enter", resize ? "resize" : "init");
    mgpu::journal::init(MGPU_VERSION_STR " built " __DATE__ " " __TIME__);
    mgpu::gpu1::set_build_string(MGPU_VERSION_STR);   // R267
    mgpu::stallwatch::init();
    mgpu::fgmap::init();   // D2.0 NR step one: reads NRFrameFilter once; off = nothing
    if (swapchain != nullptr && !mgpu::strace::is_bridge_thread() && !mgpu::chain::own_call())   // R353: the game's chain, not ours (R369: nor our own live SetColorSpace1 on the pacing thread)
    {
        g_r353_game_chain.store(reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())), std::memory_order_relaxed);
        // [PORT DISPLAY-LAYER 2/3b, 2026-11-11] THE OUTPUT REFRESH, PUSHED ONCE.
        //
        // The absolute pacer must not measure its own cadence: with the pacer
        // presenting, present-to-present is what the pacer CHOSE, so feeding it
        // back makes any initial error permanent (measured: it locked at 41
        // real frames/s against a 50 fps source, and the source followed it
        // down). DWM's composition clock is an independent fact: qpcRefreshPeriod
        // is the QPC ticks of one refresh.
        {
            DWM_TIMING_INFO dti{};
            dti.cbSize = sizeof dti;
            LARGE_INTEGER fq{};
            QueryPerformanceFrequency(&fq);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &dti)) &&
                dti.qpcRefreshPeriod != 0ull && fq.QuadPart > 0)
            {
                const double hz = (double)fq.QuadPart / (double)dti.qpcRefreshPeriod;
                mgpu::gpu1::set_display_refresh(hz);
                char lbf[220];
                snprintf(lbf, sizeof lbf,
                         "[MGPU][R343] OUTPUT REFRESH %.2f Hz from DWM's composition clock (qpcRefreshPeriod) - the absolute pacer uses this, not its own presents.",
                         hz);
                mgpu::diag::info(lbf);
            }
        }    }
    // DX11 probe: a D3D11 game swap chain only (the bridge's own chain is
    // D3D12). Reads Dx11Probe once; off = one ini read, then one load.
    if (swapchain != nullptr)
    {
        if (reshade::api::device *pd = swapchain->get_device())
        {
            if (pd->get_api() == reshade::api::device_api::d3d11)
            {
                const reshade::api::resource bb0 = swapchain->get_back_buffer(0);
                const reshade::api::resource_desc bd0 = pd->get_resource_desc(bb0);
                mgpu::dx11probe::init(
                    reinterpret_cast<void *>(static_cast<uintptr_t>(pd->get_native())),
                    reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())),
                    bd0.texture.width, bd0.texture.height, (unsigned)bd0.texture.format);
                // R227: the DX11 producer. Off unless DX11=1 (one ini read, then one
                // load). A second D3D11 chain after it is up is a rebuild.
                if (mgpu::dx11producer::on())
                    mgpu::dx11producer::on_chain_rebuilt(bd0.texture.width, bd0.texture.height,
                                                         (unsigned)bd0.texture.format);
                else
                    mgpu::dx11producer::init(
                        reinterpret_cast<void *>(static_cast<uintptr_t>(pd->get_native())),
                        reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())),
                        bd0.texture.width, bd0.texture.height, (unsigned)bd0.texture.format);
                // R231: the GAME's HWND for DirectComposition (V49), as the D3D12
                // path does below once the chain is proved to be the game's. On
                // D3D11 the proof is the producer being up on this chain. Without
                // this, TR-8 fell back to the bridge's own window ("the game's HWND
                // was never seen") and the NR output was not seen on screen.
                // R273: the game's HWND before anything latches the composition
                // mode - the gate now reads which card the window's display is on.
                if (mgpu::dx11producer::on()) { if (void *ghwnd0 = (void *)swapchain->get_hwnd()) mgpu::gpu1::set_game_hwnd(ghwnd0); }
                if (mgpu::dx11producer::on()) note_game_fullscreen(swapchain, "D3D11 game chain at init");   // R241d
                // R260: own Reflex on the D3D11 game chain. The same note the
                // D3D12 site makes once its chain is proved the game's; here
                // the proof is the producer being up on this chain (R231). The
                // NvAPI_D3D_* calls own_reflex makes take an IUnknown device
                // and are the same for D3D11 and D3D12. Off (one ini read)
                // unless OwnReflex=1; a rebuilt chain notes again (fresh settle).
                if (mgpu::dx11producer::on())
                {
                    mgpu::own_reflex::note_game_chain(
                        reinterpret_cast<void *>(static_cast<uintptr_t>(pd->get_native())),
                        reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
                    // R261: our GPU 1 device for OwnReflexDevice=gpu1|both (an
                    // AddRef'd reference; own_reflex releases it, at once when
                    // the key is not set).
                    void *g1 = nullptr;
                    if (mgpu::own_reflex::device_mode() != 0 && mgpu::gpu1::device_ref_for_reflex(&g1))
                        mgpu::own_reflex::note_gpu1_device(g1);
                }
                mgpu::mvec_census::init(bd0.texture.width, bd0.texture.height);   // R253: off unless MVecCensus=1
                if (mgpu::dx11producer::on() && mgpu::gpu1::dcomp_overlay_mode())
                {
                    if (void *ghwnd = (void *)swapchain->get_hwnd())
                        mgpu::gpu1::set_game_hwnd(ghwnd);
                }
            }
            else if (pd->get_api() == reshade::api::device_api::d3d12)
            {
                {   // R279: the texture census instrument, off unless TexCensus=1
                    const reshade::api::resource bb12 = swapchain->get_back_buffer(0);
                    const reshade::api::resource_desc bd12 = pd->get_resource_desc(bb12);
                    // R303: tex_census::init moved below, behind r280_game_chain. Called here
                    // for every D3D12 chain, the bridge's own 1280x720 one reset the census's
                    // scene size on Far Cry 6 (every report said 1280x720).
                    // R280: the virtual velocity target - off on MVecExtractOff=1 / MVecLearned=99, inert on a contract
                    // R280f: only on the GAME's D3D12 chain. SK-R1: on Skyrim (D3D11) the bridge's own
                    // D3D12 swapchain on GPU 1 (1280x720) armed it. A chain whose adapter is not the
                    // known game adapter is the bridge's; an unknown game adapter is not a reason to skip.
                    bool r280_game_chain = true;
                    {
                        mgpu::adapter::selection_result sel280;
                        mgpu::adapter::get_selection(sel280);
                        if (sel280.game_luid_known)
                            if (auto *d280 = reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(pd->get_native())))
                            {
                                const LUID l280 = d280->GetAdapterLuid();
                                r280_game_chain = (l280.LowPart == sel280.game_luid.LowPart && l280.HighPart == sel280.game_luid.HighPart);
                            }
                    }
                    if (r280_game_chain)
                    {
                        mgpu::tex_census::init(reinterpret_cast<void *>(static_cast<uintptr_t>(pd->get_native())), bd12.texture.width, bd12.texture.height);   // R279/R303
                        mgpu::mvec_extract::init(reinterpret_cast<void *>(static_cast<uintptr_t>(pd->get_native())), bd12.texture.width, bd12.texture.height);
                    }
                    else
                    {   // R280g: never silent - a wrong provisional LUID would otherwise read as a title with no source
                        char l280[200];
                        snprintf(l280, sizeof l280, "[MGPU][R280] D3D12 chain %ux%u is not on the game's adapter (R280f): not armed here.", bd12.texture.width, bd12.texture.height);
                        mgpu::diag::info(l280);
                    }
                }
                // R277: the D3D12 tap key, read once per game chain. Off = 0.
                const int t = mgpu::gpu1::ui_ini_read("MVecTap", 0);
                g_mvec_tap12 = (t == 1 || t == 2) ? t : 0;
                if (g_mvec_tap12 != 0)
                    mgpu::diag::warn(g_mvec_tap12 == 2
                        ? "[MGPU][R277] MVecTap=2 on a D3D12 chain: the census candidate is copied at finish_effects when no barrier delivered it, before-state ASSUMED common. Off by default."
                        : "[MGPU][R277] MVecTap=1 on a D3D12 chain: the census candidate is copied at finish_effects when no barrier delivered it, before-state ASSUMED shader_resource. Off by default.");
            }
        }
    }
    mgpu::stallwatch::scope sw_sc(mgpu::stallwatch::S_SWAPCHAIN);
    // D1: the discovery map. Off (one load) unless Discovery=1.
    if (swapchain != nullptr)
        mgpu::discovery::on_swapchain(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())), true, resize);
    // The only place the bridge thread is started - see the note above
    // on_init_device. This event fires on the real render device, after
    // the probe cycles are over, so the thread is never alive inside an
    // add-on unload window. Started before on_swapchain so the thread is
    // already waiting when the selection is decided.
    mgpu::worker::ensure_started();
    // The swapchain's device is the authoritative game render device. Its
    // LUID overrides the provisional init_device value and runs the
    // one-shot selection (or a terminal refusal) - see adapter.cpp.
    mgpu::adapter::on_swapchain(swapchain, resize);

    // P9.1. The presented size, handed to the probe so its candidate size band
    // is a fraction of the user's resolution rather than a pixel count somebody
    // guessed. Read here and not at the first frame because a filter that is
    // wrong for the first few hundred resources is a filter that missed the
    // engine's startup allocations, which is when scene buffers are created.
    //
    // R33 DEFECT. THIS WAS UNFILTERED AND IT SILENTLY BROKE THE SIZE BAND.
    // init_swapchain fires for BOTH runtimes in this process, and the bridge's
    // own present chain is 1280x720. It came up after the game's, so the band
    // ended up a fraction of 1280x720 instead of the game's 2560x1440 - and
    // 1664x936 scene depth is 1.69x the area of 1280x720, which the band's
    // upper bound (1.05x) then REJECTS. The lane's candidates survived only
    // because they were catalogued before the bridge chain existed; anything
    // created after it was dropped, silently, and the R33 turn-open test that
    // asks in_scene_band per clear returned false for every frame of a whole
    // run. That is what a wrong filter looks like: not an error, an absence.
    //
    // The fix is the check note_frame twelve lines below has always had. When
    // the game's LUID is not yet known nothing is passed at all, which leaves
    // the band at its permissive fallback (w>=640 && h>=360) - being late with
    // the real size costs a few loose candidates; being wrong about it costs
    // the ones that matter.
    if (swapchain != nullptr)
    {
        if (reshade::api::device *sd = swapchain->get_device())
        {
            mgpu::adapter::selection_result ssel;
            mgpu::adapter::get_selection(ssel);
            bool sc_is_game = false;
            if (ssel.game_luid_known && sd->get_api() == reshade::api::device_api::d3d12)
            {
                if (auto *sd12 = reinterpret_cast<ID3D12Device *>(sd->get_native()))
                {
                    // SL1. Stored, not queried here: the census line wants
                    // both devices and this is the side that can see the
                    // game's. No reference is taken.
                    mgpu::slprobe::note_game_device(sd12);

                    const LUID sl = sd12->GetAdapterLuid();
                    sc_is_game = (sl.LowPart == ssel.game_luid.LowPart &&
                                  sl.HighPart == ssel.game_luid.HighPart);
                    // D1.5a: the latency probe takes the game's device and chain
                    // only here, where the chain is proved by LUID to be the
                    // game's. Off (one ini read, then one load) unless LatencyProbe=1.
                    if (sc_is_game)
                        mgpu::latprobe::note_game_chain(
                            sd12, reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
                    // D2.0: own Reflex and the stall watch take the game's chain
                    // from the same LUID-proved site. Off: one ini read, then a load.
                    if (sc_is_game)
                    {
                        void *gch = reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native()));
                        mgpu::gpu1::note_game_d3d12();   // R282: one store; read once by the probe's scene-hold decision
                        mgpu::own_reflex::note_game_chain(sd12, gch);
                        {   // R261: same hand-over on the D3D12 site
                            void *g1 = nullptr;
                            if (mgpu::own_reflex::device_mode() != 0 && mgpu::gpu1::device_ref_for_reflex(&g1))
                                mgpu::own_reflex::note_gpu1_device(g1);
                        }
                        mgpu::stallwatch::note_game_chain(gch);
                        mgpu::journal::note_game_chain(gch);
                        mgpu::strace::note_game_device(sd12);   // R313: a store; read only by the record
                        mgpu::strace::note_game_chain(gch);     // R313: so on_present counts the GAME's presents only
                        if (mgpu::strace::on())
                            mgpu::strace::ev(mgpu::strace::R_GT_SWAP, "game-chain-proved", resize ? "resize" : "init");
                    }
                }
            }
            const reshade::api::resource bb = swapchain->get_back_buffer(0);
            // V49. The GAME's HWND, for DirectComposition to bind a topmost
            // visual to. Taken HERE because this is the one place that has
            // already proved, by LUID, that this swapchain is the game's and
            // not the bridge's own - so it is authoritative rather than a
            // window we went looking for.
            //
            // GATED. In mode 0 not one instruction of this runs: the ini read
            // is the whole cost of the ghost existing, and that read is the
            // same one the present chain and the window creation already do.
            if (sc_is_game) { if (void *ghwnd0 = (void *)swapchain->get_hwnd()) mgpu::gpu1::set_game_hwnd(ghwnd0); }   // R273: before the latch
            if (sc_is_game) note_game_fullscreen(swapchain, resize ? "D3D12 game chain at resize" : "D3D12 game chain at init");   // R241d
            if (sc_is_game && mgpu::gpu1::dcomp_overlay_mode())
            {
                if (void *ghwnd = (void *)swapchain->get_hwnd())
                    mgpu::gpu1::set_game_hwnd(ghwnd);
            }

            if (sc_is_game && bb.handle != 0)
            {
                const reshade::api::resource_desc bd = sd->get_resource_desc(bb);
                mgpu::probe::note_scene_size(bd.texture.width, bd.texture.height);
            }
        }
    }

    // P9.1. The probe's one-shot init, here rather than in DllMain: this is a
    // render thread with the loader lock released, so the ini read is a plain
    // file read and register_event is on the thread ReShade raises events on.
    // Guarded because init_swapchain fires again on resize, and re-reading the
    // ini there would silently undo a toggle made in the overlay.
    {
        static bool probe_started = false;
        if (!probe_started)
        {
            probe_started = true;

            // ---- R78: INSTALL THE HOOK BEFORE THE FIRST set_mode ----
            //
            // The only ordering requirement in this block. The probe LATCHES
            // its mvec lane on when a hook is installed, and that latch is
            // read INSIDE set_mode - so a hook installed afterwards would
            // leave the lane at whatever the ini asked for, and the latch
            // would not take effect until the next mode change, which may
            // never come.
            //
            // Unconditional, and deliberately not gated on the ini here: that
            // would mean reading the same key in two files and being able to
            // disagree with ourselves about it. The hook is inert unless
            // MVec=3 and the stream is armed.
            mgpu::probe::set_mvec_hook(&mgpu_mvec_transport_hook);

            mgpu::probe::set_mode(mgpu::probe::mode_from_ini());
            if (mgpu::t119::sltags_capture() != 0)   // R300d: key absent = SLT1 stays observer-only
                mgpu::sltags::set_mvec_capture(&mgpu_tagged_mvec_capture_hook);

            // ---- R101: THE NGX TAP, INSTALLED ON THE SAME ONE-SHOT ----
            //
            // After mode_from_ini, because that call is what parses the ini
            // and therefore what Calib= has been read by. Installing here
            // rather than in DllMain matters for the same reason the probe's
            // init does: the loader lock is released on this thread, and
            // walking every module's import table under the loader lock is
            // how you deadlock a game at startup.
            //
            // Calib=0 returns immediately and touches nothing.
            // [PORT B2, 2026-11-11] NgxHook=1 replaces the contract ROUTE: the
            // inline hook reads the game's NGX block at nvngx's own entry, and
            // the calibrator's pointer-swap installation is kept OFF so the two
            // routes can never write the same table in the same frame.
            const int ngx_hook_on = mgpu::gpu1::ui_ini_read("NgxHook", 0);
            mgpu::calibrator::install(ngx_hook_on != 0 ? 0 : mgpu::probe::calib_mode());
            if (ngx_hook_on != 0)
                mgpu::diag::info("[MGPU][R803] CONTRACT SOURCE = INLINE HOOK ONLY (NgxHook=1). The "
                                 "calibrator's installation is off: one route, one writer. Depth and vectors "
                                 "come from the same captured block.");
            else if (mgpu::probe::calib_mode() == 0)
                mgpu::diag::info("[MGPU][R803] CONTRACT SOURCE = NONE (NgxHook=0, Calib=0): no contract "
                                 "latching; the velocity tap is the only vector route.");
            mgpu::calibrator::set_jitter_mode(mgpu::probe::jitter_mode());

            // R106. The SAME transport hook the probe uses - one copy path,
            // two possible triggers, so the two can be compared directly
            // instead of being two different pieces of code.
            mgpu::calibrator::set_mvec_hook(&mgpu_mvec_transport_hook);
            mgpu::calibrator::set_mvec_hook_d3d11(&mgpu_mvec_transport_hook_d3d11);   // R232
            mgpu::calibrator::set_eval_copy(mgpu::probe::eval_copy_mode());
            mgpu::calibrator::set_sf_path(mgpu::probe::sf_path_mode());   // R180
            // [PORT B2] install the inline hook once _nvngx.dll is resident; install()
            // is idempotent and cheap, and the present path retries it until it lands.
            (void)mgpu::ngxhook::install(ngx_hook_on);

            // [FFX, 2026-11-11] The other contract route, for a title whose upscaler is FSR.
            // Default 0: a DLSS title never loads the module and the hook would be dead code
            // there anyway, but the verified path stays the default. FfxHook=1 costs one
            // GetModuleHandle per present until the module appears, then nothing.
            const int ffx_hook_on = mgpu::gpu1::ui_ini_read("FfxHook", 0);
            if (ffx_hook_on != 0)
                mgpu::diag::info("[MGPU][R820] CONTRACT SOURCE += FFX (FfxHook=1): ffxDispatch in the "
                                 "FidelityFX API module is hooked; its upscale descriptor fills the SAME "
                                 "table the NGX hook fills. Whichever API the title actually calls becomes "
                                 "the writer for that frame.");
            (void)mgpu::ffxhook::install(ffx_hook_on);
        }
    }
    if (mgpu::strace::on())   // R313
    {
        char r313d[96];
        snprintf(r313d, sizeof r313d, "%.2f ms inside the handler", mgpu::strace::now_ms() - r313_t_enter);
        mgpu::strace::ev(r313_role, "init_swapchain-exit", r313d);
    }
}

// ---- P1.6: what is actually painted on each runtime ----
//
// INCIDENT 6, and the cheapest one to have prevented. The bridge window showed
// the neural output with the colours destroyed - psychedelic banding over
// black - and three separate code hypotheses were formed and shipped against
// it. None was the cause. gpu1.ini, the preset ReShade assigns to the BRIDGE
// runtime, still carried "Techniques=Lumenite_QuantMotion@lumenite_QuantMotion.fx"
// with "DEBUG_FLOW=1" from an earlier debugging session. A motion-flow debug
// view was being drawn on top of every neural frame, and nothing in any log
// said so.
//
// The instrument could not see the one thing that was wrong, because the run
// CONFIGURATION - which preset each runtime loaded, and which techniques it
// enabled - was never recorded anywhere. A bisect run under a contaminated
// preset returns a confident, correctly-formatted, wrong answer, which is the
// section 00 failure shape exactly.
//
// So both runtimes now state, once each, the preset they loaded and every
// technique enabled in it. After this, "something was painted over the output"
// is a line in the log rather than a hypothesis about our code.
//
// The technique list is not read on the first event: at that point effects may
// still be compiling and an empty list would be recorded as "nothing enabled",
// which is the same wrong answer in a different costume. The read is retried
// until the runtime enumerates at least one technique, and if it never does,
// THAT is said explicitly instead.
namespace
{
    struct preset_probe
    {
        bool done = false;
        unsigned attempts = 0;
        char names[900] = {};
        size_t used = 0;
        unsigned enabled = 0;
        unsigned total = 0;

        // ---- R137: THE TECHNIQUE LANE, WHICH WAS DOCUMENTED AND NEVER EMITTED ----
        //
        // assets/README.txt has told users since 0.2.0 to check a line reading
        //
        //   [MGPU][R53] TECHNIQUE LANE: ... TAP = ON
        //
        // and to treat TAP = ABSENT as "mgpu_depth_tap.fx is not in the shader
        // path or failed to compile, and with Depth=1 the bridge will never
        // arm". THAT LINE HAS NEVER EXISTED. It appears in no log this project
        // has collected - not 007 First Light on either driver, not Lunar
        // Eclipse, not Resonance. A user was asked for it twice during issue 15
        // triage and answered, correctly, with ReShade's own "Successfully
        // compiled" message, because that was the only evidence available to
        // him. Compiled is not enabled, and enabled is not "ReShade bound a
        // depth buffer" - three different claims, and the log could only make
        // the first.
        //
        // The enumeration needed to answer it was already here: P1.6 walks
        // every technique with its effect name. It was only ever asked which
        // ones were ENABLED, and it discarded everything else.
        //
        // THREE STATES, NOT TWO. The README described ON and ABSENT. OFF is
        // real and is a different fault: the add-on enables this technique
        // itself, so present-but-off means that did not happen, which is a
        // bridge problem rather than an install problem. Collapsing it into
        // ABSENT would send the reader to re-check a file that is already in
        // the right place.
        bool tap_present = false;
        bool tap_enabled = false;
    };

    // The effect that carries the depth tap. Matched on the effect name rather
    // than the technique name because the technique can be renamed inside the
    // file without the install instructions changing.
    constexpr const char *TAP_EFFECT = "mgpu_depth_tap";

    void technique_cb(reshade::api::effect_runtime *rt,
                      reshade::api::effect_technique tech, void *user)
    {
        preset_probe *p = static_cast<preset_probe *>(user);
        ++p->total;

        // R137. The effect name is read for EVERY technique now, enabled or
        // not. The tap has to be findable while it is switched off - that is
        // the state the old two-valued line could not express - and the early
        // return below used to discard the name before anything looked at it.
        char tn[128] = {}; size_t tns = sizeof tn - 1;
        rt->get_technique_name(tech, tn, &tns);
        char en[128] = {}; size_t ens = sizeof en - 1;
        rt->get_technique_effect_name(tech, en, &ens);

        const bool is_tap = (std::strstr(en, TAP_EFFECT) != nullptr) ||
                            (std::strstr(tn, TAP_EFFECT) != nullptr);
        if (is_tap)
        {
            p->tap_present = true;
            if (rt->get_technique_state(tech)) p->tap_enabled = true;
        }

        if (!rt->get_technique_state(tech)) return;
        ++p->enabled;

        const int wrote = snprintf(p->names + p->used, sizeof p->names - p->used,
                                   "%s%s@%s", (p->used != 0) ? ", " : "", tn, en);
        if (wrote > 0 && (size_t)wrote < sizeof p->names - p->used)
            p->used += (size_t)wrote;
    }

    // R142. Where OUR mgpu_depth_tap.fx actually is. The add-on sits beside
    // dxgi.dll, which sits beside the executable, so the add-on's folder is
    // also the game's - and the shipped layout puts the tap under
    // reshade-shaders\Shaders\ inside it. Derived rather than assumed,
    // because the whole point of [R142] is to print a path the reader can
    // compare against their own.
    //
    // Never writes anything. Returns false if the module path cannot be split,
    // and the caller prints that rather than an empty string that would read
    // as a real answer.
    bool tap_shader_dir(char *out, size_t out_n)
    {
        if (out == nullptr || out_n < 8) return false;
        out[0] = '\0';

        wchar_t mod[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(mgpu::module_handle(), mod, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return false;

        wchar_t *slash = wcsrchr(mod, L'\\');
        if (slash == nullptr) return false;
        *(slash + 1) = L'\0';

        char dir[MAX_PATH * 2] = {};
        if (WideCharToMultiByte(CP_UTF8, 0, mod, -1, dir, (int)sizeof dir,
                                nullptr, nullptr) == 0)
            return false;

        snprintf(out, out_n, "%sreshade-shaders\\Shaders\\", dir);
        return out[0] != '\0';
    }

    // `tag` is "GAME" or "BRIDGE". Two independent probes, because the two
    // runtimes load different presets and either one can change what is on
    // screen.
    void log_preset_once(reshade::api::effect_runtime *runtime, const char *tag,
                         preset_probe &p)
    {
        if (p.done) return;
        ++p.attempts;

        p.used = 0; p.enabled = 0; p.total = 0; p.names[0] = '\0';
        runtime->enumerate_techniques(nullptr, technique_cb, &p);

        // Still compiling: say nothing yet rather than record an empty list as
        // a fact. 900 events is roughly fifteen seconds at 60 fps, comfortably
        // past shader compilation on this rig.
        if (p.total == 0 && p.attempts < 900) return;
        p.done = true;

        char preset[512] = {}; size_t ps = sizeof preset - 1;
        runtime->get_current_preset_path(preset, &ps);

        char line[1600];
        if (p.total == 0)
            snprintf(line, sizeof line,
                     "[MGPU][P1.6] %s runtime preset=\"%s\" - NO TECHNIQUES ENUMERATED after %u "
                     "frames. Either the preset is empty or its effects failed to compile. "
                     "Nothing is being drawn on this runtime.",
                     tag, preset, p.attempts);
        else
            snprintf(line, sizeof line,
                     "[MGPU][P1.6] %s runtime preset=\"%s\" | %u of %u techniques ENABLED%s%s. "
                     "This line exists because a stale gpu1.ini once drew a motion-flow debug "
                     "view over the neural output and three code hypotheses were spent on it. "
                     "If anything unexpected is listed here, what is on screen is not what this "
                     "add-on produced.",
                     tag, preset, p.enabled, p.total,
                     (p.enabled != 0) ? ": " : "",
                     (p.enabled != 0) ? p.names : "");
        mgpu::diag::info(line);

        // ---- R137 / R142: THE LINE THE README HAS ALWAYS PROMISED ----
        //
        // Emitted per runtime, right after P1.6, from the same enumeration.
        //
        // R142. UNTIL 0.2.3 THIS LINE DID NOT SAY WHICH RUNTIME GOVERNS DEPTH,
        // and that cost a user several days. Both lines print. The BRIDGE one
        // called its own OFF state "a bridge fault" - which it is not, because
        // the bridge runtime has no depth buffer to tap and its tap state
        // cannot affect whether anything arms - so the reader chased the
        // runtime that cannot matter while the GAME line sat beside it holding
        // the real answer. Every branch now names its runtime and what that
        // runtime governs.
        const bool is_game_rt = (std::strcmp(tag, "GAME") == 0);
        const char *tap = p.tap_present ? (p.tap_enabled ? "ON" : "OFF")
                                        : "ABSENT";

        // R145. THE PUSH THAT USED TO BE HERE IS GONE, AND THIS NOTE IS WHY.
        //
        // R142 sent the idle screen its tap state from this spot. It is the
        // wrong spot: log_preset_once is one-shot on p.done and fires at the
        // FIRST non-empty enumeration, which is before probe::tech_scan has
        // had a chance to self-enable the technique. Measured 2026-09-16 on
        // Resonance - [R53] below printed TAP = OFF for a tap that was
        // present, compiled and working, and the screen would have shown
        // ERROR 203 for the whole session on the strength of it.
        //
        // The live value is pushed every frame from on_finish_effects instead,
        // beside the depth_source() call that maintains it. The R53 line here
        // still reports this snapshot, because a snapshot is the honest thing
        // for a line that says "at the moment the effects settled".

        char t53[2200];   // R142: longest branch measured at 743 with the prefix.
        snprintf(t53, sizeof t53,
                 "[MGPU][R53] TECHNIQUE LANE: %s runtime | %u technique(s) enumerated | "
                 "TAP = %s. %s%s",
                 tag, p.total, tap,
                 is_game_rt
                     ? "THIS IS THE RUNTIME THAT GOVERNS DEPTH - it owns the game's depth "
                       "buffer, and mgpu_depth_tap.fx can only supply depth from here. "
                     : "THIS RUNTIME DOES NOT GOVERN DEPTH. The bridge has no depth buffer "
                       "to tap, the add-on's self-enable deliberately does not run here, and "
                       "nothing this line says about the tap can affect whether the stream "
                       "arms. It is printed so a reader can see it was checked, not because "
                       "it is a fault. ",
                 p.tap_present
                     ? (p.tap_enabled
                            ? "mgpu_depth_tap.fx is compiled AND its technique is enabled. "
                              "THIS DOES NOT PROVE DEPTH IS BOUND: the tap keeps ReShade's "
                              "depth buffer alive, and whether ReShade picked the right one "
                              "is a separate question the [R63] line answers."
                            : "mgpu_depth_tap.fx IS present and compiled, and its technique "
                              "is SWITCHED OFF. R191: THIS DOES NOT PREDICT WHETHER DEPTH "
                              "BINDS AND IT IS NOT A FAULT. Runs that transport depth on "
                              "every frame read OFF here - Battlefield 6 and RoboCop Rogue "
                              "City both did, with arm held 0 frames and zero invalid seals. "
                              "This line was previously worded as though present-but-off on "
                              "the GAME runtime meant the self-enable had failed; it was "
                              "chased as the cause on three separate triages in one week and "
                              "was the cause of none of them. [R63] is the line that says "
                              "whether depth is bound. On the bridge runtime OFF is the "
                              "shipped state - gpu1.ini ships with an empty Techniques= on "
                              "purpose.")
                     : "NO technique from mgpu_depth_tap.fx was enumerated on this runtime. "
                       "DO NOT ASSUME THE FILE IS MISSING - the more common cause by far is "
                       "that this runtime's EffectSearchPaths does not reach the folder the "
                       "file is in. The [R142] line below prints both paths. R137: this line "
                       "did not exist before 0.2.3, so a log without it is an older build "
                       "and not a missing tap.");
        mgpu::diag::info(t53);

        // ---- R142: READ THEIR CONFIG AND SAY WHAT IS ACTUALLY WRONG ----
        //
        // READ ONLY, AND THAT IS A DECISION RATHER THAN A LIMITATION. A
        // working ReShade.ini belongs to the user and this project does not
        // rewrite it behind their back. What this removes is the guesswork
        // from the fault that has cost the most support time: the GAME runtime
        // searching a folder the tap is not in, while the file sits exactly
        // where the install guide put it.
        //
        // Runs only when the GAME runtime came up without the tap, so a
        // healthy install pays one strcmp for it and nothing else.
        if (is_game_rt && !p.tap_present)
        {
            char paths[1024] = {};
            size_t pn = sizeof paths - 1;
            const bool got = reshade::get_config_value(runtime, "GENERAL",
                                                       "EffectSearchPaths", paths, &pn);

            char want[MAX_PATH * 2] = {};
            const bool know = tap_shader_dir(want, sizeof want);

            // Worst case measured at ~2250: 700 of fixed text plus a 1023-byte
            // path list plus a 520-byte folder. Sized past it rather than to
            // it, because the one line that explains the whole fault is the
            // last line that should ever truncate.
            char r142[3400];
            snprintf(r142, sizeof r142,
                     "[MGPU][R142] ERROR 204 - THE GAME RUNTIME CANNOT SEE THE TAP. Its "
                     "EffectSearchPaths = %s | mgpu_depth_tap.fx ships at %s | THE FIX: add "
                     "that folder to EffectSearchPaths in the ReShade.ini beside the game "
                     "executable and relaunch - for a default install that is "
                     "EffectSearchPaths=.\\reshade-shaders\\Shaders\\** (and the matching "
                     "TextureSearchPaths). WHY THIS IS NOT \"the file is missing\": the "
                     "BRIDGE runtime loads the same file through its own ReShade2.ini and is "
                     "unaffected, so every other line in this log looks healthy. Depth can "
                     "only come from the GAME runtime. With Depth=1 the stream holds at the "
                     "arm from here - it will not crash, it will not stop the game, and it "
                     "will not say anything further. The bridge window shows ERROR 204.",
                     got ? paths : "(could not be read)",
                     know ? want : "(could not resolve the add-on's folder)");
            mgpu::diag::error(r142);
        }
    }
}

// ---- T1.17 (Dom; PanelStats=1 only): CPU present cadence of the GAME swapchain ----
// Not GPU1 jobs, not GPU latency. No timestamps, fences or waits. Adapted for 0.3.1: the game's LUID is
// cached once adapter selection knows it, so the selection mutex is not taken on every present (the P7.10
// note in adapter.cpp keeps present-path callers off that lock).
static std::atomic<long>     g_t119_luid_lo{0};
static std::atomic<long>     g_t119_luid_hi{0};
static std::atomic<bool>     g_t119_luid_known{false};
static bool t119_is_game_d3d12(reshade::api::swapchain *sc)
{
    if (sc == nullptr) return false;
    reshade::api::device *dev = sc->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d12) return false;
    auto *native = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (native == nullptr) return false;
    if (!g_t119_luid_known.load(std::memory_order_acquire))
    {
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (!sel.game_luid_known) return false;
        g_t119_luid_lo.store((long)sel.game_luid.LowPart, std::memory_order_relaxed);
        g_t119_luid_hi.store(sel.game_luid.HighPart, std::memory_order_relaxed);
        g_t119_luid_known.store(true, std::memory_order_release);
    }
    const LUID l = native->GetAdapterLuid();
    return (long)l.LowPart == g_t119_luid_lo.load(std::memory_order_relaxed) &&
           l.HighPart == g_t119_luid_hi.load(std::memory_order_relaxed);
}
static std::atomic<double> g_t117_present_fps{0.0};
static std::atomic<double> g_t117_present_ms{0.0};
static std::atomic<unsigned long long> g_t117_present_samples{0};
static std::atomic<long long> g_t117_window_start{0};
static std::atomic<unsigned> g_t117_window_frames{0};
static std::atomic<unsigned> g_t117_report_windows{0};
static void t117_count_game_present(reshade::api::swapchain *sc)
{
    if (!t119_is_game_d3d12(sc)) return;
    LARGE_INTEGER t, frequency;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&frequency);
    long long start = g_t117_window_start.load(std::memory_order_relaxed);
    if (!start) {
        g_t117_window_start.compare_exchange_strong(start, t.QuadPart, std::memory_order_relaxed);
        return;
    }
    g_t117_window_frames.fetch_add(1, std::memory_order_relaxed);
    const double elapsed = double(t.QuadPart - start) / double(frequency.QuadPart);
    if (elapsed < 1.0) return;
    if (!g_t117_window_start.compare_exchange_strong(start, t.QuadPart, std::memory_order_relaxed)) return;
    const unsigned frames = g_t117_window_frames.exchange(0, std::memory_order_relaxed);
    if (!frames) return;
    const double fps = frames / elapsed;
    g_t117_present_fps.store(fps, std::memory_order_relaxed);
    g_t117_present_ms.store(1000.0 / fps, std::memory_order_relaxed);
    g_t117_present_samples.fetch_add(frames, std::memory_order_relaxed);
    const unsigned windows = g_t117_report_windows.fetch_add(1, std::memory_order_relaxed) + 1;
    if (windows % 10 == 0)
    {
        char report[256];
        std::snprintf(report, sizeof(report), "[MGPU][R300l] GAME PRESENT cadence %.2f FPS | %.3f ms/present | window_frames=%u | window_seconds=%.3f (CPU cadence, NOT GPU latency)", fps, 1000.0 / fps, frames, elapsed);
        mgpu::diag::info(report);
    }
}

// ---- T1.1 (Dom; a FrameCap line present, value 20..500): synchronized whole-game frame cap ----
// Paces the GAME present, never the GPU1 worker by itself: a GPU1-only cap would manufacture the frame-age
// problem the control is meant to test. Sleeps on the present thread by design (like OwnReflex). The value
// comes from t119_keys (the ini at init, then the panel), not from a file read per present.
static void mgpu_frame_cap_tick(reshade::api::swapchain *swapchain)
{
    const int cap = mgpu::t119::frame_cap();
    if (cap < 20 || cap > 500) return;
    if (!t119_is_game_d3d12(swapchain)) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point next{};
    static int last_cap = 0;
    const auto now = clock::now();
    const auto period = std::chrono::duration<double>(1.0 / (double)cap);
    if (last_cap != cap || next.time_since_epoch().count() == 0 || now > next + period * 4)
    {
        next = now;
        last_cap = cap;
    }
    next += std::chrono::duration_cast<clock::duration>(period);
    const auto coarse = next - std::chrono::microseconds(500);
    if (coarse > clock::now()) std::this_thread::sleep_until(coarse);
    while (clock::now() < next) YieldProcessor();
}

#if defined(MGPU_HAVE_IMGUI)
// The panel. Drawn into the BRIDGE runtime's overlay - press Home over the
// bridge window to open it.
//
// Everything it touches goes through the plain-scalar accessors in
// gpu1_context.hpp, so this stays the only file that knows about both ReShade
// and the stream.
//
// Deliberately read-then-write, never read-modify-write across a frame: the
// callback runs on the presenting thread, the stream runs on the bridge thread,
// and holding a value across the gap is how a slider fights with a hotkey.
// ---- R153: IS THE GAME AT DLAA? ASK IT, DO NOT TELL THE USER TO ----
//
// The panel used to be able to say only what the two upscaling modes ARE. It
// could not say which one this title needs, so the honest options were a
// static line of advice under the radios - "use Experimental if you have DLAA"
// - or silence. Advice is the wrong instrument here for the same reason the
// old [C2-SR] refusal was: it states a rule and leaves the user to apply it to
// a fact only the add-on can see, and a rule that cannot be checked against
// the run is a rule nobody reports as wrong.
//
// The game declares the fact on every EvaluateFeature.
// DLSS.Render.Subrect.Dimensions against DLSS.Width/Height is render extent
// against display extent, the calibrator captures both (KEY_RENDER_EXT and
// KEY_DISPLAY_EXT), and R152 already decides whether R is inheritable from
// exactly this comparison. So the panel reads the same fact from the same
// table and CANNOT disagree with the refusal - one fact, two readers.
//
// THREE ANSWERS, AND THE THIRD IS NOT A GUESS:
//    1  render == display. The game is at native or DLAA, Native Upscaling has
//       nothing to inherit, and R152 will refuse it. Say so.
//    0  render <  display. The game is upscaling; Native Upscaling is correct
//       and the panel stays quiet. Silence is the right output here - a line
//       that does not apply is what trains people past the coloured text.
//   -1  NO ANSWER. Calib=0, or nothing captured yet because the game has not
//       evaluated DLSS. Also the case on a title with no DLSS at all. The
//       panel says nothing: reporting "not DLAA" from an absent table is the
//       empty-list-recorded-as-a-fact mistake, and it would contradict the
//       refusal on a Calib=0 run where the buffer extent is all R152 has.
//
// Called only while the panel is drawn, so the read costs nothing on the
// present path. Live rather than latched on purpose: a preset change mid-run
// moves render_w within a frame, and the header two boxes down already tracks
// state that way.
// R171. The game's own DLSS mode, by the NGX PerfQuality value the calibrator
// already captures. Named here because a person can check "Quality" against
// their own game menu and cannot check "perf_quality=2" against anything.
static const char *mgpu_dlss_mode_name(int q)
{
    switch (q)
    {
    case 0: return "Max Perf";
    case 1: return "Balanced";
    case 2: return "Quality";
    case 3: return "Ultra Perf";
    case 4: return "Ultra Quality";
    case 5: return "DLAA";
    default: return nullptr;
    }
}

static int mgpu_game_is_native(int *out_quality = nullptr)
{
    if (out_quality != nullptr) *out_quality = -1;
    mgpu::calibrator::table t{};
    if (!mgpu::calibrator::read(t)) return -1;
    if (out_quality != nullptr && (t.have & mgpu::calibrator::KEY_QUALITY) != 0u)
        *out_quality = (int)t.perf_quality;
    if ((t.have & mgpu::calibrator::KEY_RENDER_EXT)  == 0u) return -1;
    if ((t.have & mgpu::calibrator::KEY_DISPLAY_EXT) == 0u) return -1;
    if (t.render_w == 0u || t.render_h == 0u ||
        t.display_w == 0u || t.display_h == 0u) return -1;

    // The same STRICTLY-SMALLER test R152 uses to decide inheritability, so
    // the two can never answer differently on the same table.
    return (t.render_w < t.display_w && t.render_h < t.display_h) ? 0 : 1;
}

static void draw_mgpu_overlay(reshade::api::effect_runtime *)
{
    // P7.10: THE UNSUPPORTED-API MESSAGE HAS TO LIVE HERE, and it is worth
    // saying why, because the obvious place was the bridge window's title and
    // that place cannot work. On a D3D11 or Vulkan title no D3D12 render device
    // is ever found, so no adapter is selected, so no device is created, so no
    // window exists to carry a title. This panel is registered on the GAME's
    // runtime as well, and ReShade's overlay draws on D3D11 and Vulkan - so it
    // is the one surface that still exists when the add-on has nothing to do.
    // A user who installs this on a D3D11 game and sees nothing at all has been
    // given no way to find out why, and will reasonably conclude it is broken.
    {
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (!sel.valid && mgpu::adapter::non_d3d12_swapchain_events() != 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                               "D3D11 and Vulkan are not supported. This add-on does nothing here.");
            ImGui::TextDisabled("It hooks ReShade's D3D12 path. Some Unity titles accept -force-d3d12.");
            return;
        }
    }

    // V19. Tell the bridge the panel is open, so AutoArm waits. Settings are
    // read at arm time, so a menu that gets armed out from under the reader is
    // a menu that does nothing.
    mgpu::gpu1::ui_panel_drawn();

    // The simple mode REPLACES the panel instead of greying parts of it out. `Advanced >>` at
    // its bottom, the checkbox in the editor section, and CTRL+ALT+F4 all switch back. Every
    // control it draws calls the same setter the full panel does - same state, less on screen.
    if (mgpu::psimple::enabled())
    {
        mgpu::psimple::draw_simple_panel();
        return;
    }


    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);

    if (mgpu::t119::panel_stats())   // T1.17 (PanelStats=1): measured from game-swapchain present events
    {
        ImGui::SeparatorText("GAME PRESENT (live)");
        if (g_t117_present_samples.load(std::memory_order_relaxed))
            ImGui::Text("%.1f FPS | %.2f ms/present", g_t117_present_fps.load(std::memory_order_relaxed), g_t117_present_ms.load(std::memory_order_relaxed));
        else
            ImGui::TextDisabled("Waiting for game presents (about 1 second)...");
        ImGui::TextDisabled("CPU present cadence; not GPU execution time or input latency.");
    }

    // One status line, not three. It changes rather than accumulating.
    if (!st.armed)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Not armed - CTRL+ALT+F10 in gameplay. Settings below apply on arm.");
    else if (st.summarised)
        ImGui::TextUnformatted("Finished - ran to its bound. Restart the game to run another.");

    // ========================= BOX 1: THE BRIDGE =========================
    //
    // Split into two boxes in V11 because the panel had become one column of
    // unrelated things. This box is the NEURAL side - what the bridge does with
    // the frame and how fast it is doing it. The DLSS box below is the
    // UPSCALER's state, which is a different feature with a different handle
    // and, unlike everything here, cannot be changed while armed.
// ---- V12: TWO COLUMNS, NOT ONE TALL STACK ----
    //
    // The single column had grown past a screen, so the DLSS box was below the
    // fold and you had to scroll to find it. Two children side by side, each
    // scrolling on its own, puts both in view at once.
    //
    // IT FALLS BACK TO STACKED BELOW 640 px. A two-column layout in a narrow
    // overlay is two unreadable columns, which is worse than scrolling. The
    // overlay's width is the user's to change and we do not control it.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const bool  two_col = (avail_w >= 640.0f);
    const float col_w   = two_col
        ? (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f
        : avail_w;
    // [OURS] 2026-10-12: THE OPERATOR'S POINT, AND IT WAS RIGHT. 560 was a guess, and it
    // was the whole bug: the two boxes stopped there and everything below - inside a window
    // that already reaches the display's height - was dead space, while the left box's own
    // content ran past 560 and needed the panel scrollbar (that is exactly how the Neural
    // scale controls got "hidden", and why the log box that briefly filled this space was
    // the only thing visible in it). Take the region that is actually available, minus the
    // room the Probe section and the warning lines need underneath the boxes.
    float col_h   = st.nrs_on ? (ImGui::GetContentRegionAvail().y - 130.0f) : 560.0f;   // R299: main's 560 unless NRScale is set
    // T1.19 UI4K1 (Dom; PanelAutoHeight=1 only, and not when R299 already sized it): the two boxes take the
    // height ReShade gives the add-on, keeping 170 px for the section below, with 560 px as the floor. It
    // does not resize ReShade's own window. Key absent: main's 560.
    if (!st.nrs_on && mgpu::t119::panel_auto_height())
    {
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const float reserve_below = 170.0f;
        if (avail_h > 560.0f + reserve_below) col_h = avail_h - reserve_below;
    }

    ImGui::BeginChild("mgpu_box_bridge", ImVec2(col_w, col_h), true);
    ImGui::SeparatorText("MGPU BRIDGE  -  neural rendering");

    // [OURS] 2026-10-12. The two columns are the BOX pair above (mgpu_box_bridge and
    // mgpu_box_dlss), each sized to the region that is actually available. An earlier attempt
    // of mine used ImGui::Columns() here instead; it was removed because columns lay out at
    // their CONTENT's height, so they can never claim the space below - which is the whole
    // thing the operator was pointing at.
    // PANEL SCALE, and the honest reason for it.
    //
    // The operator's actual complaint, in their words: the left column hides the Neural
    // scale controls below the fold and the panel's own scrollbar has to be dragged to
    // reach them - "make both columns taller so they use the black area below, then I do
    // not need the wheel". The black area is not unused column space: ReShade's overlay
    // window is already at the display's height, so there is nothing below to grow into.
    // The only way to show everything at once is to make the content SHORTER - so the whole
    // tab is scaled down (io.FontGlobalScale). PanelScale=0.85 by default; 0.75 fits more,
    // 1.00 is ReShade's own size.
    {
        static float panel_scale = -1.0f;
        // [PORT B0-a, 2026-11-11] PanelScale used to be applied only when NRScale
        // was set (st.nrs_on), so on a title that runs NRS=1 without an NRScale
        // line the key was read and then silently ignored. The key's own default
        // (0.85) must not change anyone's panel when the line is ABSENT, so the
        // test is "the key was present", not "a value was computed".
        static bool panel_scale_key = false;
        if (panel_scale < 0.0f)
        {
            float v = 0.85f;
            // read once per session; the panel is not the place to re-read a file every frame
            wchar_t ini[MAX_PATH] = {};
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&draw_mgpu_overlay, &self);
            char mod[MAX_PATH] = {};
            if (self != nullptr && GetModuleFileNameA(self, mod, MAX_PATH) > 0)
            {
                char *slash = strrchr(mod, '\\');
                if (slash != nullptr)
                {
                    *slash = 0;
                    char path[MAX_PATH] = {};
                    snprintf(path, sizeof path, "%s\\mgpu.ini", mod);
                    FILE *f = fopen(path, "rb");
                    if (f != nullptr)
                    {
                        char buf[65536];
                        const size_t got = fread(buf, 1, sizeof buf - 1, f);
                        buf[got] = 0;
                        fclose(f);
                        const char *k = strstr(buf, "PanelScale=");
                        if (k != nullptr)
                        {
                            const float pv = (float)atof(k + 11);
                            if (pv > 0.5f && pv <= 1.5f) { v = pv; panel_scale_key = true; }
                        }
                    }
                }
            }
            panel_scale = v;
        }
        if (st.nrs_on || panel_scale_key) ImGui::GetIO().FontGlobalScale = panel_scale;   // R299 + [PORT B0-a]: the key alone is enough
    }
    {
    // ---- Model tuning, first and always visible ----
    //
    // Moved to the top in P7.10. These are the controls the reference
    // implementation sets and this one leaves at the feature's defaults, so
    // they are the first thing to reach for when the image looks wrong - and
    // "the image looks wrong" is why most people will open this panel at all.
    // Burying them under four other groups meant nobody found them.
    ImGui::SeparatorText("Model");

    int style_i = (int)(st.style + 0.5f);
    ImGui::TextUnformatted("Style");
    ImGui::SameLine();
    if (ImGui::RadioButton("A", &style_i, 0)) mgpu::gpu1::ui_set_tuning_value(3, 0.0f);
    ImGui::SameLine();
    if (ImGui::RadioButton("B", &style_i, 1)) mgpu::gpu1::ui_set_tuning_value(3, 1.0f);
    ImGui::SameLine();
    if (ImGui::RadioButton("C", &style_i, 2)) mgpu::gpu1::ui_set_tuning_value(3, 2.0f);

    float tone = st.tone_strength;
    if (ImGui::SliderFloat("tone", &tone, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(0, tone);
    float structure = st.structure_strength;
    if (ImGui::SliderFloat("structure", &structure, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(1, structure);
    float skin = st.skin_strength;
    if (ImGui::SliderFloat("skin", &skin, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_tuning_value(2, skin);
    bool mask = st.auto_mask;
    if (ImGui::Checkbox("auto mask", &mask))
        mgpu::gpu1::ui_set_tuning_value(4, mask ? 1.0f : 0.0f);

    if (!st.tuning_on)
        ImGui::TextDisabled("Not applied yet - move any control above to enable the group.");
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Applied - this is a tuning run.");
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) mgpu::gpu1::ui_set_tuning(false);
    }
    ImGui::TextDisabled("Washed or flat colour? Take tone down - 0.00 fixed it on the dev rig.");

    // ---- Passes ----
    ImGui::SeparatorText("Passes");
    int passes = (int)st.passes;
    for (unsigned i = 1; i <= st.max_passes; ++i)
    {
        char lab[8];
        snprintf(lab, sizeof lab, "x%u", i);
        if (i > 1) ImGui::SameLine();
        if (ImGui::RadioButton(lab, &passes, (int)i))
            mgpu::gpu1::ui_set_passes(i);
    }
    // ONE line, and it goes red when the setting it warns about is active. The
    // panel used to carry six lines of explanation here; a warning nobody
    // finishes reading is not a warning. The reasoning is in mgpu.ini.
    // R319a: with the residual 2nd pass armed (Light / Medium) the second pass is
    // lighter, so the line says what is true for it; Full keeps the original line.
    if (passes >= 2 && mgpu::residual::carries(1u))   // R332: pass 2 itself, not "any pass" (NRS=1 + Pass2=full)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                           "x2 adds a lighter second pass on the second GPU. "
                           "It uses more power than x1.");
    else if (passes >= 2)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                           "x2 is free to the game but holds the second GPU near its power "
                           "limit. x1 recommended.");
    else
        ImGui::TextDisabled("x1 leaves the second GPU about half idle.");

    // ---- R352 / R355 (0.5.0, Marcelo): the pass settings and what is running, at the top level ----
    // Marcelo's shape (23:43): per pass, "Network" with two buttons - Light-FP8 (the small network,
    // NRPreset<i>=0) in GREEN with "FPS+" under it, Full-FP8 (the big network, NRPreset<i>=1) in RED with
    // "FPS-" under it; below, "Resolution" with two buttons - Residual Neural (our residual on that pass)
    // GREEN / FPS+, Native (a full pass) RED / FPS-. The selected button is drawn solid, the other dimmed.
    // To the right, one button "Best Quality-Performance" restores Marcelo's default: every pass Light-FP8 +
    // Residual, the LAST pass Full-FP8 + Residual (the big network on the final pass is what removes the
    // structure strobe; N2b).
    // Keys (all read at arm; the file is written, the restart line shows while it differs from what loaded):
    //   network: NRPreset<i> 0 | 1 (R349)
    //   pass 1 resolution: NRS 1 | 0 (R332)
    //   pass 2 resolution: with NRS=1 -> Pass2 nrs | full; with NRS=0 -> ResidualPass 2 | 0 (the 0.4.0 keys,
    //            Medium / Full), so pass 2 can be Residual whatever pass 1 is
    //   pass 3 / 4 resolution: Pass3 / Pass4 nrs | full (R350; the module reads them with NRS=1 only, so
    //            they are enabled while pass 1 is Residual)
    //   frame generation on the neural card: FrameGen 4 | 0 (R341)
    {
        static bool pm_loaded = false;
        static int  pm_net_file[4] = {}, pm_net_want[4] = {};   // 0 Light-FP8 (small), 1 Full-FP8 (big)
        static int  pm_res_file[4] = {}, pm_res_want[4] = {};   // 1 Residual Neural, 0 Native
        static bool pm_fg_file = false, pm_fg_want = false;
        static int  pm_x2_file = 0, pm_x2_want = 0;              // R361: pass 1's network at x2+ (NRPreset1x2; absent = NRPreset1)
        // Literals, not stack buffers: ui_ini_read / ui_ini_write keep the key POINTER in their mirror.
        static const char *const pm_net_key[4]  = { "NRPreset1", "NRPreset2", "NRPreset3", "NRPreset4" };
        static const char *const pm_pass_key[4] = { "NRS", "Pass2", "Pass3", "Pass4" };
        auto load_file = [&]()
        {
            const int fb = mgpu::gpu1::ui_ini_read("NRPreset", 0);
            const bool nrs1 = (mgpu::gpu1::ui_ini_read("NRS", 0) == 1);
            const int rp = mgpu::gpu1::ui_ini_read("ResidualPass", 0);
            for (unsigned i = 0; i < 4u; ++i)
            {
                const int v = mgpu::gpu1::ui_ini_read(pm_net_key[i], fb);
                pm_net_file[i] = (v == 1) ? 1 : 0;   // 2 (small, explicit) shows as Light-FP8
                if (i == 0u) pm_res_file[i] = nrs1 ? 1 : 0;
                else
                {
                    char w[16] = {};
                    mgpu::gpu1::ui_ini_read_word(pm_pass_key[i], w, sizeof w);
                    if (i == 1u)
                        pm_res_file[i] = nrs1 ? ((w[0] != 0) ? ((w[0] == 'n' || w[0] == 'N') ? 1 : 0) : (rp == 2 ? 1 : 0))
                                              : (rp == 2 ? 1 : 0);
                    else
                        pm_res_file[i] = (nrs1 && (w[0] == 'n' || w[0] == 'N')) ? 1 : 0;
                }
                pm_net_want[i] = pm_net_file[i];
                pm_res_want[i] = pm_res_file[i];
            }
            pm_fg_file = (mgpu::gpu1::ui_ini_read("FrameGen", 0) == 4);
            pm_fg_want = pm_fg_file;
            {
                char w[16] = {};
                mgpu::gpu1::ui_ini_read_word("NRPreset1x2", w, sizeof w);   // R361
                pm_x2_file = (w[0] == 0) ? pm_net_file[0] : ((atoi(w) == 1) ? 1 : 0);
                pm_x2_want = pm_x2_file;
            }
        };
        if (!pm_loaded) { pm_loaded = true; load_file(); }
        // write one pass's resolution choice; returns whether the file took it
        auto write_res = [&](unsigned i, int res) -> bool
        {
            if (i == 0u) return mgpu::gpu1::ui_ini_write("NRS", res);
            if (i == 1u)
            {
                if (pm_res_want[0] == 1) return mgpu::gpu1::ui_ini_write_word("Pass2", res ? "nrs" : "full");
                return mgpu::gpu1::ui_ini_write("ResidualPass", res ? 2 : 0);
            }
            return mgpu::gpu1::ui_ini_write_word(pm_pass_key[i], res ? "nrs" : "full");
        };
        // one choice button: solid in its colour when selected, dimmed when not
        auto pick = [&](const char *label, bool selected, bool green, float w) -> bool
        {
            const ImVec4 on  = green ? ImVec4(0.18f, 0.62f, 0.30f, 1.0f) : ImVec4(0.72f, 0.24f, 0.24f, 1.0f);
            const ImVec4 off = green ? ImVec4(0.18f, 0.62f, 0.30f, 0.28f) : ImVec4(0.72f, 0.24f, 0.24f, 0.28f);
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? on : off);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, on);
            const bool clicked = ImGui::Button(label, ImVec2(w, 0.0f));
            ImGui::PopStyleColor(3);
            return clicked;
        };
        const float bw = 150.0f, x0 = 110.0f, gap = 8.0f;

        ImGui::SeparatorText("Pass settings");
        bool differs = false;
        const unsigned np = (st.max_passes < 4u) ? st.max_passes : 4u;
        // the restore button, right-aligned on the header row
        {
            const float rw = 215.0f;
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > rw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - rw);
            if (ImGui::Button("Best Quality-Performance", ImVec2(rw, 0.0f)))
            {
                // Marcelo's default (R360): Residual on every pass; Full-FP8 on pass 1 AND the last pass, Light-FP8
                // on the passes between (3 / 4 only).
                // Each write that lands moves the panel's choice; what loaded at panel start stays as
                // the reference, so the restart line still says whether this session differs.
                if (mgpu::gpu1::ui_ini_write("NRS", 1)) pm_res_want[0] = 1;
                const bool live_b = st.armed && st.nr_ok;
                for (unsigned i = 0; i < np; ++i)   // the ini first (R364), every key
                {
                    const int net = (i == 0u || i + 1u == np) ? 1 : 0;   // R360
                    if (mgpu::gpu1::ui_ini_write(pm_net_key[i], net)) { pm_net_want[i] = net; pm_net_file[i] = net; }
                    if (i >= 1u && mgpu::gpu1::ui_ini_write_word(pm_pass_key[i], "nrs")) { pm_res_want[i] = 1; if (live_b) pm_res_file[i] = 1; }
                }
                if (mgpu::gpu1::ui_ini_write("NRPreset1x2", 0)) pm_x2_want = 0;   // R361: Light-FP8 on pass 1 while x2+ runs
                if (live_b)   // then the live requests (R363 / R364)
                {
                    for (unsigned i = 0; i < np; ++i)
                    {
                        const int net = (i == 0u || i + 1u == np) ? 1 : 0;
                        mgpu::gpu1::ui_set_network(i, (i == 0u && st.passes >= 2u) ? 0 : net);
                        mgpu::gpu1::ui_set_resolution(i, true);
                    }
                }
                if (mgpu::gpu1::ui_ini_write("FrameGen", 0)) pm_fg_want = false;
            }
            ImGui::TextDisabled("Restores the default: Residual on every pass; Full-FP8 on pass 1 at x1 and on the last pass at x2+.");
            ImGui::TextDisabled("Network and resolution apply live. Frame generation needs a restart.");
        }
        for (unsigned i = 0; i < np; ++i)
        {
            char lab[48];
            ImGui::Text("Pass %u", i + 1u);
            // Network row - LIVE (R363): the buttons show the network this pass runs right now (what its feature
            // was built with) and a click rebuilds the pass on the bridge thread before its next evaluates
            // (one CreateFeature, ~100-300 ms). The ini key is written too, so the next launch starts there:
            // pass 1 while two or more passes run writes NRPreset1x2 (the x1 value stays), else NRPreset<i>.
            ImGui::TextDisabled("Network");
            ImGui::SameLine(x0);
            const bool live = st.armed && st.nr_ok;
            const int running = live ? ((st.nr_preset_pass[i] == 1) ? 1 : 0) : pm_net_want[i];
            const char *key_now = (i == 0u && st.passes >= 2u) ? "NRPreset1x2" : pm_net_key[i];
            snprintf(lab, sizeof lab, "Light-FP8##net%u", i);
            if (pick(lab, running == 0, true, bw) && running != 0)
            {   // R364: the ini FIRST, then the request - the rebuild writes ArmCrashed to the same file (R6d: WRITE FAILED x2)
                if (mgpu::gpu1::ui_ini_write(key_now, 0)) { pm_net_want[i] = 0; if (live) pm_net_file[i] = 0; }
                if (live) mgpu::gpu1::ui_set_network(i, 0);
            }
            ImGui::SameLine(x0 + bw + gap);
            snprintf(lab, sizeof lab, "Full-FP8##net%u", i);
            if (pick(lab, running == 1, false, bw) && running != 1)
            {
                if (mgpu::gpu1::ui_ini_write(key_now, 1)) { pm_net_want[i] = 1; if (live) pm_net_file[i] = 1; }
                if (live) mgpu::gpu1::ui_set_network(i, 1);
            }
            ImGui::SetCursorPosX(x0); ImGui::TextDisabled("FPS+");
            ImGui::SameLine(x0 + bw + gap); ImGui::TextDisabled("FPS-");
            // R362: no "at x2+" pair here (Marcelo 09:31: noisy). NRPreset1x2 is set by Best Quality-Performance
            // and the ini; "Running now" shows the network pass 1 actually has at the current pass count.
            // Resolution row
            // Resolution row - LIVE too (R364): the buttons show whether the pass carries our residual right now
            // (residual::carries), a click flips the module for that pass and rebuilds its feature at the new
            // extent on the bridge thread; the ini is written first so the next launch starts there.
            // R366: no "pass 1 first" gate (Pass3 / Pass4 are read for any pass-1 setting now).
            // R367: on N01950's NRScale path the row works the same way, per pass, live - Residual = the NRScale
            // path on this pass, Native = a full pass (the stream flips his per-pass mask, no recreate). The ini
            // can only say "pass 1" or "every pass" for his path (NRSPass 1 | 2), so the file is written when the
            // live set matches one of those and left alone otherwise.
            const bool nrscale_here = live && st.nrs_on;
            const bool res_ok = true;
            const int res_running = live ? ((nrscale_here ? ((st.nrs_pass_mask >> i) & 1u) != 0u : mgpu::residual::carries(i)) ? 1 : 0)
                                         : pm_res_want[i];
            auto write_res_nrscale = [&](unsigned pass, int v)
            {
                const unsigned bit = 1u << pass;
                const unsigned all = (np >= 32u) ? ~0u : ((1u << np) - 1u);
                const unsigned m = v ? (st.nrs_pass_mask | bit) : (st.nrs_pass_mask & ~bit);
                if (m == all) (void)mgpu::gpu1::ui_ini_write("NRSPass", 2);
                else if (m == 1u) (void)mgpu::gpu1::ui_ini_write("NRSPass", 1);
            };
            ImGui::TextDisabled("Resolution");
            ImGui::SameLine(x0);
            ImGui::BeginDisabled(!res_ok);
            snprintf(lab, sizeof lab, "Residual Neural##res%u", i);
            if (pick(lab, res_running == 1, true, bw) && res_running != 1)
            {
                if (nrscale_here) write_res_nrscale(i, 1);
                else if (write_res(i, 1)) { pm_res_want[i] = 1; if (live) pm_res_file[i] = 1; }
                if (live) mgpu::gpu1::ui_set_resolution(i, true);
            }
            ImGui::SameLine(x0 + bw + gap);
            snprintf(lab, sizeof lab, "Native##res%u", i);
            if (pick(lab, res_running == 0, false, bw) && res_running != 0)
            {
                if (nrscale_here) write_res_nrscale(i, 0);
                else if (write_res(i, 0)) { pm_res_want[i] = 0; if (live) pm_res_file[i] = 0; }
                if (live) mgpu::gpu1::ui_set_resolution(i, false);
            }
            ImGui::EndDisabled();
            ImGui::SetCursorPosX(x0); ImGui::TextDisabled("FPS+");
            ImGui::SameLine(x0 + bw + gap); ImGui::TextDisabled("FPS-");
            if (nrscale_here && i + 1u == np) ImGui::TextDisabled("Residual here = N01950's NRScale path on that pass (NRScale is set this session).");
            if (!live && (pm_net_want[i] != pm_net_file[i] || pm_res_want[i] != pm_res_file[i])) differs = true;   // R363/R364: both live when armed
            if (i + 1u < np) ImGui::Spacing();
        }
        bool fg = pm_fg_want;
        if (ImGui::Checkbox("frame generation on the neural card", &fg))
        {
            if (mgpu::gpu1::ui_ini_write("FrameGen", fg ? 4 : 0)) pm_fg_want = fg;
            // [PORT FG-PANEL] HOT on/off: off stops generation on the next frame, on resumes
            // with a fresh pair. Only a backend/multiplier change needs a restart.
            mgpu::fg::ui_set_enabled(fg);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(experimental; off by default)");
        if (pm_fg_want != pm_fg_file) differs = true;
        // R363: NRPreset1x2 is live too (the Best button applies it through ui_set_network)
        if (differs)
            ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                               "Restart the game once to apply.");

        // ---- Running now: the arm's own state, not the file ----
        ImGui::SeparatorText("Running now");
        if (!st.armed)
            ImGui::TextDisabled("Not armed yet.");
        else if (!st.nr_ok)
            ImGui::TextDisabled("Neural stage not running (transport only).");
        else
        {
            // the DLL's default network: small from 310.9.16, big before it
            unsigned va = 0, vb = 0, vc = 0, vd = 0;
            const bool vparsed = (sscanf_s(st.nr_dll_version, "%u.%u.%u.%u", &va, &vb, &vc, &vd) == 4);
            const bool small_ok = vparsed && (va > 310u || (va == 310u && (vb > 9u || (vb == 9u && vc >= 16u))));
            for (unsigned i = 0; i < st.passes && i < 4u; ++i)
            {
                const int hint = st.nr_preset_any ? st.nr_preset_pass[i] : 0;
                const char *net = (hint == 1) ? "Full-FP8"
                                : (hint == 2) ? "Light-FP8"
                                : (!vparsed ? "DLL default" : (small_ok ? "Light-FP8 (DLL default)" : "Full-FP8 (DLL default: one network)"));
                const char *mode = mgpu::residual::carries(i) ? "Residual Neural"
                                 : ((st.nrs_on && ((st.nrs_pass_mask >> i) & 1u)) ? "Residual (NRScale, N01950)" : "Native");
                ImGui::Text("pass %u: %s, %s", i + 1u, net, mode);
            }
            ImGui::Text("StageCopy %s | frame generation %s | DLSS-NR DLL %s",
                        st.stage_on ? "on" : "off", st.fg_on ? "on" : "off", st.nr_dll_version);
        }
    }

    bool neural = st.neural;
    if (ImGui::Checkbox("neural stage", &neural))
        mgpu::gpu1::ui_set_neural(neural);
    ImGui::SameLine();
    ImGui::TextDisabled(st.nr_ok ? "(running)" : "(transport only)");

    // ---- R219: EXPERIMENTAL SETTINGS (operator, 2026-10-04) ----
    //
    // Behind a toggle that is off by default, so it is clear these are not
    // something to bother with yet. Crop: Off | On (crop, the key switches it
    // and the whole frame) | Auto crop (CropAuto=1: whole frame while the game
    // has no motion vectors, which is also why it does not work with the
    // game's frame generation). Written to mgpu.ini and read at arm, so before
    // arm it applies to this session and after arm on the next launch. The
    // size (ScaleStep) and the key (CropKey) stay in mgpu.ini.
    {
        static bool show_exp = false;
        static bool crop_loaded = false;
        static unsigned want_mode = 0u;
        static bool want_auto = true;
        if (!crop_loaded)
        {
            crop_loaded = true;
            mgpu::gpu1::ui_ini_read_crop(want_mode, want_auto);
        }
        ImGui::Checkbox("Experimental settings", &show_exp);
        if (show_exp)
        {
            ImGui::SeparatorText("Crop");
            // R216 turns crop off at arm whenever super resolution is on: the file
            // asks for it (only 1 turns it on, as the reader has it) and, once the
            // stream runs, it actually started - arm can refuse it (Passes above
            // 1, Subrect), and then crop runs.
            const bool sr_ini = (mgpu::gpu1::ui_ini_read("SRUpscale", 0) == 1) &&
                                (!st.armed || st.sr_on);
            // ScaleMode=whole set by hand selects no radio.
            int crop_i = (want_mode == 0u) ? 0 : ((want_mode == 1u) ? (want_auto ? 2 : 1) : -1);
            ImGui::BeginDisabled(sr_ini);
            if (ImGui::RadioButton("Off##crop", &crop_i, 0))
            {
                // The panel follows the file: it changes only when the write lands.
                if (mgpu::gpu1::ui_ini_write("ScaleMode", 0)) want_mode = 0u;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("On##crop", &crop_i, 1))
            {
                if (mgpu::gpu1::ui_ini_write("CropAuto", 0))
                {
                    want_auto = false;
                    if (mgpu::gpu1::ui_ini_write("ScaleMode", 1)) want_mode = 1u;
                }
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Auto crop##crop", &crop_i, 2))
            {
                if (mgpu::gpu1::ui_ini_write("CropAuto", 1))
                {
                    want_auto = true;
                    if (mgpu::gpu1::ui_ini_write("ScaleMode", 1)) want_mode = 1u;
                }
            }
            ImGui::TextDisabled("Auto crop does not work with frame generation.");
            ImGui::EndDisabled();
            // R165's rule: the restart line only while the file differs from
            // what this session armed with.
            if (sr_ini)
                ImGui::TextDisabled("Crop runs with DLSS Super Resolution on GPU 1 off.");
            else if (st.armed && st.crop_ini_ok && (want_mode != st.crop_ini_mode ||
                                  (want_mode == 1u && want_auto != st.crop_ini_auto)))
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                   "Restart the game once to apply.");

            // R362: the R319 "2nd pass (x2)" Medium / Light / Full block was here. The top-level "Pass settings"
            // (R355) sets the same keys (Residual Neural = Medium, Native = Full); Light (ResidualRender=1, the
            // residual at the game's render size) stays an ini key, documented there. Marcelo 09:26: two x2 settings
            // in one panel.
            // ---- R321: Pass 1 scale (NRS, N01950's R299 path) - Option A, one residual at a time ----
            // On  = NRScale=0.75 + NRScaleResolveDepth=0 (C5: N01950's default + Dom's fix).
            // Off = NRScale=0 (no key-removal writer; R299 needs a scale above 0.05 to turn on, so 0
            //       is off, as an absent line is). While it is on, the residual 2nd pass stays off
            //       (R316) and x2 runs the full native 2nd pass. Read at arm: restart.
            {
                static bool nrs_loaded = false;
                static bool nrs_file = false, nrs_want = false;
                if (!nrs_loaded)
                {
                    nrs_loaded = true;
                    char v[32] = {};
                    mgpu::gpu1::ui_ini_read_word("NRScale", v, sizeof v);
                    nrs_file = (v[0] != 0 && atof(v) > 0.05);
                    nrs_want = nrs_file;
                }
                ImGui::SeparatorText("Pass 1 scale (NRS)");
                int pick = nrs_want ? 1 : 0;
                bool changed = false;
                if (ImGui::RadioButton("Off##nrs1", &pick, 0)) changed = true;
                ImGui::SameLine();
                if (ImGui::RadioButton("On##nrs1", &pick, 1)) changed = true;
                if (changed && (pick == 1) != nrs_want)
                {
                    bool ok = true;
                    if (pick == 1)
                        ok = mgpu::gpu1::ui_ini_write_float("NRScale", 0.75f) &&
                             mgpu::gpu1::ui_ini_write("NRScaleResolveDepth", 0);
                    else
                        ok = mgpu::gpu1::ui_ini_write_float("NRScale", 0.0f);
                    if (ok) nrs_want = (pick == 1);
                }
                ImGui::TextDisabled("Pass 1 runs at 0.75 and adds its detail back at full size.");
                ImGui::TextDisabled("While it is on, x2 runs the full 2nd pass.");
                if (nrs_want != nrs_file)
                    ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                       "Restart the game once to apply.");
            }

            // ---- R219-2: 16-bit colour (R208, NRInput16 / NRInput16Power) ----
            // Read at arm like crop. nr16 is on only for NRInput16=4 and takes a
            // power of 1.0-3.0, else 2.2. The slider writes when it is let go,
            // not on every frame of the drag.
            static bool n16_loaded = false;
            static bool want_n16 = false;
            static float want_pw = 2.2f, file_pw = 2.2f;
            if (!n16_loaded)
            {
                n16_loaded = true;
                // nr16's own reader, so the panel parses exactly what arm will.
                mgpu::nr16::ini_file_values(want_n16, file_pw);
                want_pw = file_pw;
            }
            ImGui::SeparatorText("16-bit colour");
            bool n16 = want_n16;
            if (ImGui::Checkbox("16-bit colour", &n16))
            {
                if (mgpu::gpu1::ui_ini_write("NRInput16", n16 ? 4 : 0)) want_n16 = n16;
            }
            ImGui::BeginDisabled(!want_n16);
            // AlwaysClamp: a CTRL+click typed value stays in nr16's 1.0-3.0.
            ImGui::SliderFloat("power##n16", &want_pw, 1.0f, 3.0f, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (mgpu::gpu1::ui_ini_write_float("NRInput16Power", want_pw)) file_pw = want_pw;
                else want_pw = file_pw;
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Runs with x1, Super Resolution off and crop off.");
            if (st.armed && st.n16_ini_ok &&
                (want_n16 != st.n16_ini_on ||
                 (want_n16 && (file_pw - st.n16_ini_power > 0.005f ||
                               st.n16_ini_power - file_pw > 0.005f))))
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                   "Restart the game once to apply.");

            // ---- Frame generation (D2.0 NR2 / NR3), moved here (2026-10-08) ----
            // Live switches on fg_map's own state, as before; each change is
            // now also written to mgpu.ini (NRFrameFilter, NRFrameKeep), so it
            // is the launch default from the next start. Off by default (the
            // shipped mgpu.ini has no NRFrameFilter). With frame generation on,
            // NRFrameFilter=1 crashed Dawnwalker at the bridge start on some
            // launches (2026-10-08) - hence the warning. Mode 1 (the read-only
            // map) stays ini-only: the radios show none selected while it runs.
            ImGui::SeparatorText("Frame generation");
            ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                               "Warning: may crash the game at startup. Off by default.");
            int fm = mgpu::fgmap::ui_mode();
            if (ImGui::RadioButton("off##fg", &fm, 0))
            { mgpu::fgmap::ui_set_mode(0); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 0); }
            ImGui::SameLine();
            if (ImGui::RadioButton("fix vectors##fg", &fm, 2))
            { mgpu::fgmap::ui_set_mode(2); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 2); }
            ImGui::SameLine();
            if (ImGui::RadioButton("fix + thin##fg", &fm, 3))
            { mgpu::fgmap::ui_set_mode(3); (void)mgpu::gpu1::ui_ini_write("NRFrameFilter", 3); }
            if (fm == 3)
            {
                int n = (int)mgpu::fgmap::ui_keep();
                if (ImGui::SliderInt("keep 1 generated frame in", &n, 2, 8))
                    mgpu::fgmap::ui_set_keep((unsigned)n);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    (void)mgpu::gpu1::ui_ini_write("NRFrameKeep", (int)mgpu::fgmap::ui_keep());
            }
            if (fm == 1)
                ImGui::TextDisabled("Map only (NRFrameFilter=1): reading, not acting.");
            bool fg_now = false;
            unsigned long long reused = 0, thinned = 0;
            mgpu::fgmap::ui_counters(fg_now, reused, thinned);
            char fl[200];
            snprintf(fl, sizeof fl, "frame generation %s   reused %llu   thinned %llu",
                     fg_now ? "ACTIVE" : "not seen", reused, thinned);
            ImGui::TextUnformatted(fl);
            ImGui::TextDisabled("fix: one presented step of motion on every frame; a frame without a");
            ImGui::TextDisabled("fresh vector binds the kept one. thin: evaluate 1 generated frame in N.");
            if (st.mvec_mode != 3)
                ImGui::TextDisabled("Needs MVec=3 (the game's own vectors). This run is not.");
        }
    }

    // ---- Intensity ----
    ImGui::SeparatorText("Intensity");
    int pr = st.preset;
    if (ImGui::RadioButton("manual", &pr, 0))       mgpu::gpu1::ui_set_preset(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("front-loaded", &pr, 1)) mgpu::gpu1::ui_set_preset(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("back-loaded", &pr, 2))  mgpu::gpu1::ui_set_preset(2);

    float all = st.intensity[0];
    if (ImGui::SliderFloat("all passes", &all, 0.0f, 2.0f, "%.2f"))
        mgpu::gpu1::ui_set_intensity(0, all);
    if (ImGui::IsItemDeactivatedAfterEdit())   // R300n: persist once, on release
        mgpu::gpu1::ui_persist_intensity(0, all);
    for (unsigned i = 0; i < st.passes && i < st.max_passes; ++i)
    {
        char lab[24];
        snprintf(lab, sizeof lab, "pass %u", i + 1);
        float v = st.intensity[i];
        if (ImGui::SliderFloat(lab, &v, 0.0f, 2.0f, "%.2f"))
            mgpu::gpu1::ui_set_intensity(i + 1, v);
        if (ImGui::IsItemDeactivatedAfterEdit())   // R300n
            mgpu::gpu1::ui_persist_intensity(i + 1, v);
    }
    if (st.passes == 1)
        ImGui::TextDisabled("At x1 the three shapes are identical - there is one pass to shape.");

    // ---- View ----
    ImGui::SeparatorText("View");
    int pm = st.present_mode;
    if (ImGui::RadioButton("output", &pm, 0)) mgpu::gpu1::ui_set_present_mode(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("input", &pm, 1))  mgpu::gpu1::ui_set_present_mode(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("split", &pm, 2))  mgpu::gpu1::ui_set_present_mode(2);
    float sp = st.split_pos;
    if (ImGui::SliderFloat("seam", &sp, 0.0f, 1.0f, "%.2f"))
        mgpu::gpu1::ui_set_split_pos(sp);
    if (ImGui::IsItemDeactivatedAfterEdit())   // R300n
        mgpu::gpu1::ui_persist_split_pos(sp);
    ImGui::TextDisabled("Split: input left of the seam, output right, same frame.");
    ImGui::TextDisabled("CTRL+ALT+LEFT/RIGHT move it with no overlay open (SHIFT = coarse).");

    // ---- T1.1 Frame pacing (Dom; only when the ini has a FrameCap line) ----
    if (mgpu::t119::frame_cap_present())
    {
        ImGui::SeparatorText("Frame pacing");
        int frame_cap = mgpu::t119::frame_cap();
        bool cap_on = frame_cap >= 20;
        if (ImGui::Checkbox("synchronized FPS cap", &cap_on))
        {
            frame_cap = cap_on ? (frame_cap >= 20 ? frame_cap : 80) : 0;
            mgpu::t119::set_frame_cap(frame_cap);
            mgpu::gpu1::ui_ini_write("FrameCap", frame_cap);
        }
        if (cap_on)
        {
            if (frame_cap < 20) frame_cap = 80;
            if (ImGui::SliderInt("target FPS", &frame_cap, 30, 240))
            {
                mgpu::t119::set_frame_cap(frame_cap);
                mgpu::gpu1::ui_ini_write("FrameCap", frame_cap);
            }
            ImGui::TextDisabled("Caps the GAME present and MGPU production together; it never caps GPU 1 alone.");
        }
    }

    // ---- Counters ----
    if (st.armed)
    {
        ImGui::SeparatorText("Counters");
        char c1[200];
        snprintf(c1, sizeof c1,
                 "produced %llu  consumed %llu  dropped %llu  overrun %llu  skipped %llu",
                 st.produced, st.consumed, st.dropped, st.overrun, st.skipped);
        ImGui::TextUnformatted(c1);
        if (st.overrun != 0)
            ImGui::TextDisabled("Overrun climbing = GPU 1 is past its budget at this pass count.");
    }

    // ---- Live ----
    //
    // EVERY NUMBER HERE IS THE ONE ITS END-OF-RUN LOG LINE PRINTS, over the
    // same divisor. If the panel and the log ever disagree, the panel is wrong
    // and it is a bug, not a second opinion.
    if (st.armed)
    {
        ImGui::SeparatorText("Live");
        char lv[260];

        // [OURS] 2026-10-12. THE OPERATOR WAS RIGHT ABOUT THIS PAIR. "GPU 1" counts what
        // the consumer FINISHED, and a finished frame is not necessarily a shown one, so
        // it reads high. The displayed cadence is the bridge's own present ring - and it
        // is given as percentiles, because an average hides the stalls an eye notices.
        if (st.nrs_on)   // R299: the new readout only with NRScale set; main's line otherwise
            snprintf(lv, sizeof lv, "ON SCREEN  %.1f fps   p50 %.2f ms   p99 %.2f ms   1%% low %.2f ms   (n=%u)",
                     st.present_fps, st.present_p50_ms, st.present_p99_ms, st.present_low1_ms, st.present_n);
        else
            snprintf(lv, sizeof lv, "fps   game %.1f   GPU 1 %.1f",
                     st.fps_produced, st.fps_consumed);
        ImGui::TextUnformatted(lv);
        if (st.nrs_on)
        {
            snprintf(lv, sizeof lv, "game render %.1f   GPU 1 finished %.1f   (neither is what you see)",
                     st.fps_produced, st.fps_consumed);
            ImGui::TextDisabled(lv);
        }
        if (mgpu::t119::panel_stats())   // T1.17 (PanelStats=1): derived, not measured
        {
            snprintf(lv, sizeof lv, "frame interval (derived)   game %.2f ms   GPU 1 %.2f ms",
                     st.fps_produced > 0.0 ? 1000.0 / st.fps_produced : 0.0,
                     st.fps_consumed > 0.0 ? 1000.0 / st.fps_consumed : 0.0);
            ImGui::TextUnformatted(lv);
            ImGui::TextDisabled("Derived from sealed/consumed rates; not actual Present frametime.");
            snprintf(lv, sizeof lv, "transport payload  requested %.2f GiB/s   consumed-equivalent %.2f GiB/s",
                     st.bus_requested_mib_s / 1024.0, st.bus_consumed_mib_s / 1024.0);
            ImGui::TextUnformatted(lv);
            ImGui::TextDisabled("Derived from slot bytes x frame rate; NOT a hardware PCIe throughput counter.");
            unsigned long long mvn = 0;
            for (unsigned q = 0; q < 8; ++q) mvn += st.mv_age[q];
            if (mvn != 0)
            {
                const double a1 = 100.0 * (double)st.mv_age[0] / (double)mvn;
                const double a2 = 100.0 * (double)st.mv_age[1] / (double)mvn;
                unsigned long long a3n = 0; for (unsigned q = 2; q < 8; ++q) a3n += st.mv_age[q];
                const double a3 = 100.0 * (double)a3n / (double)mvn;
                snprintf(lv, sizeof lv, "MV frame age  1: %.1f%%   2: %.1f%%   3+: %.1f%%   worst %llu",
                         a1, a2, a3, st.mv_age_worst);
                if (a1 < 90.0) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", lv);
                else ImGui::TextUnformatted(lv);
            }
        }
        if (st.fps_consumed + 1.0 < st.fps_produced)
            ImGui::TextDisabled("GPU 1 behind the game - frames are being skipped, not dropped.");

        // ---- [OURS] the neural scale, live (2026-10-12) ----
        if (st.nrs_on)   // R299: the whole section only with NRScale set
        {
            ImGui::SeparatorText("Neural scale (our own path)");
            float sc = st.nrs_scale;
            ImGui::SetNextItemWidth(200.0f);
            // 2026-10-12: the top of the range is 1.00 now. Above it the snippet would need a
            // larger FEATURE, and rebuilding a feature leaks NGX memory it never returns
            // (measured: +1.5-2.4 GB per rebuild). Below it, a change only resizes our own
            // two scratch textures, so dragging this is free.
            const float nr_top = (st.nrs_cap > 1.0f) ? st.nrs_cap : 1.0f;   // ini NRScaleCap
            if (ImGui::SliderFloat("NRScale", &sc, 0.30f, nr_top, "%.2f"))
                mgpu::gpu1::ui_nrs_stage(sc);
            ImGui::SameLine();
            // A per-knob way back. The operator pulled a slider too far and could not remember
            // where it started; asking a log file for the default is not a user interface.
            if (ImGui::SmallButton("default##nrs_scale")) mgpu::gpu1::ui_nrs_stage(0.75f);
            ImGui::TextDisabled("Applied on the next frame; rebuilds the feature in place (~0.3 s, one Reset frame).");
            snprintf(lv, sizeof lv, "work %ux%u  ->  native %ux%u    rebuilds %llu   last %.1f s ago",
                     st.nrs_work_w, st.nrs_work_h, st.nrs_native_w, st.nrs_native_h,
                     st.nrs_rebuilds, st.since_rebuild_ms / 1000.0);
            ImGui::TextUnformatted(lv);
            if (st.nrs_probe_d >= 0.0)
                snprintf(lv, sizeof lv, "neural effect   mean|composite-native| = %.4f", st.nrs_probe_d);
            else
                snprintf(lv, sizeof lv, "neural effect   (set NRProbe=1 in mgpu.ini to measure it)");
            ImGui::TextUnformatted(lv);
            float g = st.nrs_gain, sh = st.nrs_sharpness, co = st.nrs_color;
            int   mo = (int)st.nrs_mode;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("detail gain", &g, 0.0f, 2.0f, "%.2f"))      mgpu::gpu1::ui_nrs_set(0, g);
            ImGui::SameLine();
            if (ImGui::SmallButton("default##nrs_gain")) mgpu::gpu1::ui_nrs_set(0, 1.00f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("sharpness", &sh, 0.0f, 1.0f, "%.2f"))        mgpu::gpu1::ui_nrs_set(1, sh);
            ImGui::SameLine();
            if (ImGui::SmallButton("default##nrs_sharp")) mgpu::gpu1::ui_nrs_set(1, 0.50f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("colour strength", &co, 0.0f, 1.0f, "%.2f"))  mgpu::gpu1::ui_nrs_set(2, co);
            ImGui::SameLine();
            if (ImGui::SmallButton("default##nrs_col")) mgpu::gpu1::ui_nrs_set(2, 0.90f);
            if (ImGui::RadioButton("residual", mo == 1)) mgpu::gpu1::ui_nrs_set(3, 1.0f);
            ImGui::SameLine();
            if (ImGui::RadioButton("direct", mo == 0))   mgpu::gpu1::ui_nrs_set(3, 0.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("default##nrs_mode")) mgpu::gpu1::ui_nrs_set(3, 1.0f);
            ImGui::TextDisabled("Defaults: 0.75 / gain 1.00 / sharp 0.50 / colour 0.90 / residual.");
            ImGui::TextDisabled("These three are live with no rebuild, and they persist to mgpu.ini.");
        ImGui::TextDisabled("NRScale: the feature is built once at 1.00 and never rebuilt. A change");
        ImGui::TextDisabled("resizes only our scratch pair (~30-45 MB), 350 ms after you let go.");
        }

        if (st.gpu1_ts_ok)
        {
            snprintf(lv, sizeof lv,
                     "GPU 1 ms  copy %.3f   unpack %.3f   EVALUATE %.3f   out %.3f",
                     st.gpu1_copy_ms, st.gpu1_unpack_ms, st.gpu1_eval_ms, st.gpu1_out_ms);
            ImGui::TextUnformatted(lv);
            ImGui::TextDisabled(st.sr_on
                ? "Evaluate spans every pass AND the reduce and upscale - not the neural cost alone."
                : "Evaluate spans every pass - divide by the pass count to compare against one.");
        }

        snprintf(lv, sizeof lv, "latency  ready-to-consume %.2f ms   submit-to-consume %.2f ms",
                 st.lat_ready_ms, st.lat_submit_ms);
        ImGui::TextUnformatted(lv);
        ImGui::TextDisabled("Bridge share only - the game's render before and the present after are not in it.");

        snprintf(lv, sizeof lv, "GPU 0 queue  mean %.2f   max %llu frames behind",
                 st.backlog_mean, st.backlog_max);
        if (st.backlog_mean >= 2.5)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lv);
            ImGui::TextDisabled("Queueing deep. Reflex=1 in mgpu.ini, or turn it on in the game.");
        }
        else
        {
            ImGui::TextUnformatted(lv);
            if (st.backlog_mean > 0.0 && st.backlog_mean <= 1.6)
                ImGui::TextDisabled("1.0 is the structural floor, not an error. This is what low latency looks like.");
        }

        snprintf(lv, sizeof lv, "ring  %u slots%s   skipped by window %llu",
                 st.ring_depth,
                 (st.ring_window != 0) ? "  (window ON)" : "  (window off)",
                 st.ring_skipped);
        ImGui::TextUnformatted(lv);

        if (st.reordered != 0 || st.bad_magic != 0 || st.contract != 0)
        {
            snprintf(lv, sizeof lv, "SEAL FAULTS  reordered %llu  bad magic %llu  contract %llu",
                     st.reordered, st.bad_magic, st.contract);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lv);
        }
    }

    }
    ImGui::EndChild();   // ---- end BOX 1 ----

    if (two_col) ImGui::SameLine();

    // ====================== BOX 2: DLSS AND REFLEX ======================
    //
    // READ-ONLY, AND THAT IS A DECISION RATHER THAN AN OMISSION. Quality,
    // preset and R are baked into an NGX feature handle at arm time. Changing
    // one means releasing and rebuilding that handle mid-stream: a 200-450 ms
    // stall on the bridge thread, down a teardown path that has crashed
    // before. These are set in mgpu.ini and take effect on the next arm.
    ImGui::BeginChild("mgpu_box_dlss", ImVec2(col_w, col_h), true);

    ImGui::SeparatorText("DLSS  -  super resolution and Reflex");
    {
        char dl[300];

        // ================= BEFORE ARM: THE SETTINGS MENU =================
        //
        // EVERY CONTROL HERE WRITES mgpu.ini IMMEDIATELY, and the arm reads
        // that file, so a change made before arming applies to THIS session
        // with no restart. That is the whole reason the menu is worth having,
        // and the reason AutoArm holds while this panel is open.
        //
        // Only settings a user should touch are here. Passes, Depth, MVec and
        // CopyQueue stay out on purpose: they are pipeline internals, and each
        // one can produce a configuration nobody can support from a bug report.
        if (!st.armed && !st.summarised)
        {
            // V26. Reflex is not read here - that control is gone. SRScale IS,
            // because the mode buttons below now write it.
            static bool m_loaded = false;
            static bool m_sr = false;
            static int  m_preset = 0, m_mode = 2;
            // V27: the match-game toggle, and a restart breadcrumb.
            //
            // m_match is DERIVED from SRScale rather than stored in a key of
            // its own. SRScale=0 already means "inherit R from the game's
            // render extent" and there is no second meaning for it, so a
            // separate ini key would be a second source of truth for one fact
            // - which is how SRScale and SRMvLowRes got out of step in the
            // first place. Read it, do not store it.
            static bool m_match = false;
            if (!m_loaded)
            {
                m_loaded = true;
                m_sr     = (mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0);
                m_preset = mgpu::gpu1::ui_ini_read("SRPreset", 0);
                m_mode   = mgpu::gpu1::ui_ini_read("SRQuality", 2);
                m_match  = (mgpu::gpu1::ui_ini_read("SRScale", 0) == 0);
            }

            // ---- V26: THE MODE SETS R. THE SAME LADDER THE ARMED VIEW USES ----
            //
            // quality 67, balanced 58, performance 50 - percent per axis, and
            // 67 is DLSS Quality's own ratio. Identical to the armed view's
            // APPLY and to stream_state::AUTO_LADDER, because three different
            // ladders in one add-on is how a setting stops meaning anything.
            //
            // WHY THIS REPLACES THE DLAA TOGGLE ENTIRELY. Until now R was
            // INHERITED from the game's render extent, so if the title was
            // rendering at native - DLAA, or no DLSS at all - there was
            // nothing smaller to enlarge and every button in this box did
            // nothing. Four of the five titles measured 2026-09-13 were in
            // exactly that state. The user saw a DLSS section with modes and
            // presets that changed nothing, and a separate "DLAA mode" toggle
            // whose relationship to it was not guessable.
            //
            // With R chosen here, the modes mean the same thing on every
            // title whatever the game's own DLSS is set to, and the DLAA
            // concept disappears from the interface because it was never a
            // mode - it was the absence of one.
            //
            // SRMvLowRes RIDES WITH THE SCALE, ALWAYS. The flag without the
            // scale is the combination that returned FAIL_PlatformError on
            // 2026-09-12. They are one setting and they are written together
            // in every path below.
            //
            // ---- V27: AND THE MATCH-GAME BRANCH WRITES THE OTHER PAIR ----
            //
            // Ledger 6k measured two configurations and they are both valid:
            //
            //   SRScale=67 SRMvLowRes=1   R is ours. Works on every title,
            //                             including one rendering at native.
            //                             The default, and cheaper.
            //   SRScale=0  SRMvLowRes=0   R is the game's own render extent,
            //                             so the vectors ARE at R and MV_Scale
            //                             is untouched. Only does anything
            //                             when the game is itself upscaling.
            //
            // 6k's A/B: evaluate 6.492 ms against 7.459 ms, and L2 10.34
            // against 11.40. That ~1 ms is the AREA, not the flag - run A's R
            // was the game's 1485x835 against run B's 1280x720. The flag
            // itself is a boolean and a scale handed to DLSS.
            //
            // 6k also recorded that mode 0 with the vectors NOT at R crashed
            // at arm, which is why the pair is written together here and why
            // the toggle cannot produce the third combination.
            const auto write_mode = [](int mode)
            {
                mgpu::gpu1::ui_ini_write("SRQuality", mode);
                if (m_match)
                {
                    mgpu::gpu1::ui_ini_write("SRScale",    0);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 0);
                }
                else
                {
                    const int scale = (mode == 2) ? 67 : ((mode == 1) ? 58 : 50);
                    mgpu::gpu1::ui_ini_write("SRScale",    scale);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 1);
                }
            };

            ImGui::SeparatorText("SECOND GPU");
            // V27. BEFORE THE CONTROLS, for the reason the V12 block below
            // records: a warning printed after the buttons arrives too late
            // for the person who already clicked one. Everything here writes
            // mgpu.ini on the click, and mgpu.ini is read when the stream
            // ARMS - so nothing in this section changes a running session.
            if (ImGui::Checkbox("Enable DLSS on GPU 1", &m_sr))
            {
                mgpu::gpu1::ui_ini_write("SRUpscale", m_sr ? 1 : 0);
                // Turning it on must leave a usable R behind even when the
                // ini came from 0.1.0 and has SRScale=0 in it. write_mode
                // honours m_match, so an ini that already said SRScale=0 comes
                // back with the toggle on rather than being silently reset.
                if (m_sr) write_mode(m_mode);
            }
            ImGui::TextDisabled("Neural rendering runs at a reduced resolution, DLSS enlarges it back.");
            ImGui::TextDisabled("Works whatever the game's own DLSS is set to, including off.");

            // ---- V27: GREYED, NEVER HIDDEN ----
            //
            // These used to be inside "if (m_sr)", so with DLSS on GPU 1 off
            // the whole box vanished. People running it once on a title at
            // DLAA never learned the controls existed at all - they saw an
            // empty section and concluded there was nothing to set. Greying
            // shows what is there and why it is not doing anything, which is
            // the thing a hidden control can never say.
            ImGui::Spacing();
            // ---- R167: "DLSS IS DISABLED" WAS A CLAIM ABOUT THE WRONG DLSS ----
            //
            // m_sr is OUR Super Resolution feature on GPU 1 - the thing the
            // add-on creates - and nothing else. The old text read "DLSS on
            // GPU 1 is DISABLED", which a person running the game at DLSS
            // Quality reads as a statement that DLSS is off. It is not: their
            // game's DLSS is running normally on GPU 0 and the add-on has no
            // opinion about it. The only fact here is that this run did not
            // ask for a second Super Resolution pass on the bridge, so none
            // was created.
            //
            // The word DISABLED goes too. Nothing failed and nothing was
            // turned off - it was never requested, which is a different
            // sentence and the one the person can act on.
            // ---- R170: THIS LINE REPORTS THE GAME, NOT OUR FEATURE ----
            //
            // It has carried the wrong subject through three rewrites. m_sr is
            // the ini, st.sr_on is our feature on GPU 1, and neither belongs
            // at the top of this box - the Super resolution section below
            // already reports the feature, and saying it twice in different
            // words is what turned one fact into a paragraph.
            //
            // What the person needs here is the one thing they cannot see from
            // inside this panel and that everything else depends on: IS THE
            // GAME AT DLSS OR AT DLAA. One line, read live from the game's own
            // declaration, and silent when the game has not said yet.
            {
                const int nat = mgpu_game_is_native();
                if (nat == 1)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Game is at DLAA.");
                else if (nat == 0)
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Game is at DLSS.");
            }

            ImGui::BeginDisabled(!m_sr);

            ImGui::TextUnformatted("preset");
            ImGui::SameLine();
            if (ImGui::RadioButton("title default##p", m_preset == 0))
            { m_preset = 0;  mgpu::gpu1::ui_ini_write("SRPreset", 0); }
            ImGui::SameLine();
            if (ImGui::RadioButton("K##p", m_preset == 11))
            { m_preset = 11; mgpu::gpu1::ui_ini_write("SRPreset", 11); }
            ImGui::SameLine();
            if (ImGui::RadioButton("L##p", m_preset == 12))
            { m_preset = 12; mgpu::gpu1::ui_ini_write("SRPreset", 12); }
            ImGui::SameLine();
            if (ImGui::RadioButton("M##p", m_preset == 13))
            { m_preset = 13; mgpu::gpu1::ui_ini_write("SRPreset", 13); }
            ImGui::TextDisabled("A DLL that lacks the preset asked for uses its own instead.");

            // ---- MATCH GAME, BEFORE THE MODE ROW ----
            //
            // It comes first because it decides whether the mode row means
            // anything, and the V12 block further down this file records what
            // happens otherwise: "SAY IT BEFORE THE BUTTONS, NOT AFTER... the
            // first person to use this panel clicked a preset, saw no change,
            // and reasonably concluded the control was broken."
            // Two named modes rather than a checkbox, because they are two
            // ways of choosing R and neither is an option on the other. Both
            // write SRScale and SRMvLowRes through write_mode, so the pair can
            // never be set independently - the combination that crashed at arm
            // on 2026-09-12 is unreachable from this panel.
            ImGui::TextUnformatted("upscaling");
            if (ImGui::RadioButton("Native Upscaling", m_match))
            { m_match = true;  write_mode(m_mode); }
            ImGui::TextDisabled("Upscales from the game's own render resolution. Higher quality.");

            if (ImGui::RadioButton("Experimental Upscaler", !m_match))
            { m_match = false; write_mode(m_mode); }
            ImGui::TextDisabled("Works from a downscaled resolution. More performance, possible");
            ImGui::TextDisabled("cost in quality. Untested.");

            // Greyed rather than hidden, for the same reason as the block
            // above: all three write SRScale, Native Upscaling overrides it,
            // and a row that disappears teaches nobody it was ever an option.
            ImGui::BeginDisabled(m_match);
            ImGui::TextUnformatted("mode  ");
            ImGui::SameLine();
            if (ImGui::RadioButton("quality##m", m_mode == 2))
            { m_mode = 2; write_mode(2); }
            ImGui::SameLine();
            if (ImGui::RadioButton("balanced##m", m_mode == 1))
            { m_mode = 1; write_mode(1); }
            ImGui::SameLine();
            if (ImGui::RadioButton("performance##m", m_mode == 0))
            { m_mode = 0; write_mode(0); }
            ImGui::TextDisabled("Sets the resolution neural rendering runs at, the way DLSS does:");
            ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%% of the display.");
            ImGui::EndDisabled();
            if (m_match)
                ImGui::TextDisabled("Greyed: Native Upscaling is on, so the game chooses the resolution.");

            // V28: THE OLD NOTICE IS GONE. It said "if you see ghosting or
            // smearing in motion, turn this off", written when this section
            // had one toggle and "this" could only mean SR. With Native and
            // Experimental named above it, "this" no longer has one referent,
            // and both blurbs already carry the honest framing and the ask for
            // reports. A warning nobody can resolve to a control is noise.

            ImGui::EndDisabled();


            // ---- V26: THERE IS NO "DLAA MODE" ANY MORE, AND THAT IS THE FIX ----
            //
            // V24 removed the DLAA toggle on the grounds that SRScale=50 plus
            // SRMvLowRes=1 was the only panel setting that had ever produced
            // FAIL_PlatformError, and that the V42 guard fixing it had never
            // completed a run. BOTH HALVES OF THAT WERE STALE.
            //
            // The FAIL_PlatformError had one cause - the old guard forcing
            // MVLowRes on with no MV_Scale correction when the vectors were at
            // the display extent - and V42 fixed it. The guard is not unproven
            // either: it took the correct branch on Dawnwalker on 2026-09-13,
            // and a full 6000-frame run at SRScale=50 arrived at
            // "CreateFeature(SuperSampling) Success | R=1280x720 -> D=2560x1440,
            // MVLowRes=1, MVScale fix 0.5000/0.5000" with zero ghosting at
            // 42 fps on GPU 0 - which is the HARD case for ghosting, not the
            // easy one. Every DLAA attempt that failed before that died in
            // CreateFeature(Reserved18), which was the unrelated cubin crash.
            //
            // So DLAA does not come back as a toggle, because it was never a
            // mode. It was the state of having no R of our own. The mode
            // buttons above now choose R on every title, so the controls mean
            // the same thing whatever the game's DLSS is set to, and the thing
            // that used to need a second confusing switch is just the default.
            //
            // FORCE REFLEX stays out, and the reason is a one-way door. The
            // driver mode is PER DEVICE and survives the process, so a launch
            // that crashes leaves GPU 0 in low latency with nothing to undo
            // it - restore() only ever runs from stream_shutdown(), and all
            // three of its call sites are clean-exit paths. Worse still,
            // restore() is gated on `applied`, which is only set when
            // Reflex=1 engaged, so a user who ticks this, crashes, and then
            // UNTICKS IT to be safe has made the residue permanent. A toggle
            // whose off position cannot undo its on position is a trap, and
            // it is not shipping in a panel.
            //
            // The Reflex key still exists in mgpu.ini for our own testing. It
            // is simply not one click away for someone who has no idea what it
            // does. When the residue is self-healing it can come back.
            //
            // ---- ONE RULE, AND IT IS NEVER WRONG ----
            //
            // Everything left in this panel is read AT ARM, so arming is
            // technically enough for it. The panel still says RESTART, because
            // one rule that is sometimes stronger than needed beats two rules
            // where the reader has to work out which one applies. "Applied
            // when you arm" is not a sentence a non-native reader should have
            // to decode to find out whether their setting took.
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Saved automatically. RESTART THE GAME to use these settings.");
            ImGui::TextDisabled("The file is mgpu.ini, next to the game's exe.");

            ImGui::Spacing();
            if (ImGui::Button("  START NOW  ")) mgpu::gpu1::ui_request_arm();
            ImGui::SameLine();
            ImGui::TextDisabled("starts the bridge with the settings already saved");
        }
        else
        {
        // NOT a return above: this block sits inside BeginChild, and returning
        // from here would skip EndChild and leave ImGui's stack unbalanced for
        // the rest of the frame. An else costs one brace and cannot do that.
        // ---- R159: THE THIRD SR STATUS BLOCK, AND IT WAS STILL GUESSING ----
        //
        // R153 stopped two places in this panel asserting DLAA without
        // checking. This was the third and it was missed, so the readout kept
        // saying "the game already renders at display resolution" whatever the
        // reason - including after the person had changed the game to a DLSS
        // preset, at which point it is simply false.
        //
        // MEASURED 2026-09-17: launch at a DLSS preset and switch to DLAA and
        // the panel reads correctly; launch at DLAA and switch to a preset and
        // it does not, because SR NEVER CREATED. That asymmetry is real and it
        // is not a defect - stream_sr_create runs ONCE, at arm, and the
        // refusal sets sr_on to 0 with no retry path. Nothing in this panel
        // said so, which is why it read as the add-on losing track of DLSS.
        // The restart is now stated where the state is, not left to be
        // inferred from two launches.
        ImGui::SeparatorText("Super resolution");
        if (!st.sr_requested)
        {
            ImGui::TextDisabled("Off. Tick the box in the DLSS Super Resolution section above.");
        }
        else if (!st.sr_on)
        {
            // R175. The stats readout says the state and points at the log.
            // The what-to-do lives in the Super Resolution box above and is
            // not repeated here in different words - one fact, one place.
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Requested but not running - see [MGPU][C2-SR] in the log.");
            if (mgpu_game_is_native() == 1)
                ImGui::TextDisabled("The game is at DLAA.");
            ImGui::TextDisabled("Created once, at arm. Restart to retry.");
        }
        else
        {
            snprintf(dl, sizeof dl, "%ux%u  ->  %ux%u   (%.2f Mpx -> %.2f Mpx)",
                     st.sr_w, st.sr_h, st.out_w, st.out_h,
                     (double)st.sr_w * (double)st.sr_h / 1000000.0,
                     (double)st.out_w * (double)st.out_h / 1000000.0);
            ImGui::TextUnformatted(dl);

            static const char *qn[] = { "Max Perf", "Balanced", "Max Quality",
                                        "Ultra Perf", "Ultra Quality", "DLAA" };
            const int qi = (st.sr_quality >= 0 && st.sr_quality < 6) ? st.sr_quality : 1;
            snprintf(dl, sizeof dl, "quality %s   preset %s   R %s",
                     qn[qi],
                     (st.sr_preset == 0) ? "title default" : "forced",
                     (st.sr_scale_pct == 0) ? "inherited from the game"
                                            : "chosen by SRScale");
            ImGui::TextUnformatted(dl);

            snprintf(dl, sizeof dl, "snippet  %s%s",
                     st.sr_snippet_driver ? "DRIVER's copy" : "the game's own copy",
                     (st.sr_snippet_requested && !st.sr_snippet_driver)
                         ? "  (driver copy asked for but not found)" : "");
            ImGui::TextUnformatted(dl);
            if (!st.sr_snippet_driver)
                ImGui::TextDisabled("Preset availability is whatever that DLL carries - an old one lacks K.");

            snprintf(dl, sizeof dl, "motion vectors  mode %u, flag %s, MV scale x%.4f/%.4f",
                     st.sr_mv_mode, st.sr_mv_lowres ? "ON" : "off",
                     st.sr_mv_fix_x, st.sr_mv_fix_y);
            ImGui::TextUnformatted(dl);
            if (st.sr_mv_mode == 1)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "EXPERIMENTAL. May reduce ghosting - or cause it. Not confirmed on any other rig or game.");
            else if (st.sr_mv_mode == 2)
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                   "Mode 2 derives the flag WITHOUT correcting the scale - this is the combination that ghosted.");
        }

        // ---- V28: THE REFLEX READOUT IS GONE, AND IT IS NOT COMING BACK ----
        //
        // It printed "driver reported before X after Y" from GetSleepStatus,
        // and that call cannot see the TITLE'S OWN Reflex. A game with Reflex
        // enabled in its own settings produced a reading indistinguishable
        // from one where this add-on had set it - so the line was reporting a
        // fact it does not have access to.
        //
        // The panel already admitted the readout was unreliable in two
        // TextDisabled lines beneath it. A number with a disclaimer saying not
        // to believe it is worse than no number: it still anchors the reader,
        // and a bug report quoting it costs a session to unpick.
        //
        // Reflex itself is unchanged - the key still works, it is still 0.3.0,
        // and the GPU 0 queue figure above is the honest instrument. The
        // ordinals and the run C result are in ledger 6l.

        // ---- V12: THE CONTROLS. STAGE AND COMMIT. ----
        //
        // Quality and preset are baked into the NGX feature handle when it is
        // created, so neither can be a live slider: changing one means
        // releasing that handle and building a new one. So the radio buttons
        // stage a choice and APPLY commits it - the rebuild happens at the top
        // of the next poll on the bridge thread, which is the only moment GPU 1
        // has nothing outstanding.
        //
        // R DOES NOT CHANGE with either value, so nothing R-sized is rebuilt:
        // not the transport, not the ring, not the neural stage. That is
        // exactly why these two are safe to expose and why the DLAA lever
        // below is not.
        // ---- V31: THIS SECTION IS NEVER HIDDEN ----
        //
        // It used to be "if (st.sr_on)", so with super resolution off the
        // whole block vanished from the ARMED view - and that is the view
        // almost everyone sees, because AutoArm=1 arms the stream within
        // seconds of the game presenting. Someone opening the panel with Home
        // on a title where SR was off found no quality or preset controls at
        // all, and no way to learn they existed.
        // ---- V73: THE TWO RESTART-ONLY CONTROLS, MOVED UP HERE ----
        //
        // These used to sit in a second box further down, alongside a DUPLICATE
        // set of preset and quality radios. Two boxes with the same radios, only
        // one of which did anything in the running session, and the code knew:
        // the old comment said "separate live Change quality or preset block
        // further up". The duplicates are gone. What is left here is the two
        // controls that genuinely cannot take effect until the game restarts,
        // placed immediately above the live box so the order on screen matches
        // the order you use them in: turn it on, restart once, then tune live.
        {
            static bool ini_loaded = false;
            static bool want_sr = false;
            static bool want_match = true;
            static int  want_m = 2;
            static bool wrote_any = false;
            if (!ini_loaded)
            {
                ini_loaded = true;
                want_sr    = (mgpu::gpu1::ui_ini_read("SRUpscale", 0) != 0);
                want_m     = mgpu::gpu1::ui_ini_read("SRQuality", 2);
                want_match = (mgpu::gpu1::ui_ini_read("SRScale", 0) == 0);
            }

            const auto write_sr_mode = [](int mode, bool match)
            {
                mgpu::gpu1::ui_ini_write("SRQuality", mode);
                if (match)
                {
                    // ---- R180: NATIVE WRITES THE DERIVATION, NOT A FORCED OFF ----
                    //
                    // MEASURED, Cyberpunk 2077 2026-09-17, three runs one
                    // variable: SR off was clean, Native with SRMvLowRes=0
                    // smeared, Experimental with the flag on and a 1.003 scale
                    // was clean. Frame alignment was WORSE on both clean runs
                    // (57.3% and 53.6% at one frame, against 76.8% on the run
                    // that smeared), so staleness was not the cause in either
                    // direction - the vector space declaration was.
                    //
                    // Native inherits R from the game's render extent, so on a
                    // title that is upscaling the vectors ARE at R. Writing 0
                    // told DLSS they spanned the display, and it decoded every
                    // vector about 1.5x too long - an over-projected history,
                    // which is a smear. The code comment that justified the 0
                    // read the flag as "lower than the input"; it means "at
                    // input resolution rather than output resolution".
                    //
                    // 2 DERIVES it and is correct in all three cases: vectors
                    // at R -> on with scale 1.0; vectors at neither -> on with
                    // the R/mvec correction; vectors at the display extent ->
                    // off and untouched, which is the DLAA case that returned
                    // FAIL_PlatformError on 2026-09-12 when the flag was
                    // forced on. Experimental still writes 1 explicitly, so
                    // nothing about that pairing changes.
                    mgpu::gpu1::ui_ini_write("SRScale",    0);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 2);
                }
                else
                {
                    const int sc = (mode == 2) ? 67 : ((mode == 1) ? 58 : 50);
                    mgpu::gpu1::ui_ini_write("SRScale",    sc);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 1);
                }
                return true;
            };

            if (ImGui::Checkbox("DLSS Super Resolution on GPU 1", &want_sr))
                wrote_any |= mgpu::gpu1::ui_ini_write("SRUpscale", want_sr ? 1 : 0);
            ImGui::TextDisabled("Native Parameters ON");

            // ---- R165: IS A RESTART ACTUALLY NEEDED? ASK, DO NOT ASSERT ----
            //
            // wrote_any is STATIC, so once anything in this box had been
            // written the yellow "restart the game once" stayed on screen for
            // the rest of the session - including after the restart, and
            // including when the panel already matched what GPU 1 was
            // running. A reminder that never clears is indistinguishable from
            // a fault, and it is the same defect as V74's "turn it ON" shown
            // to somebody who already had.
            //
            // The honest question is not "did we write the ini", it is
            // "does the ini now disagree with the running feature". That is
            // answerable: sr_scale_pct is what GPU 1 armed with - 0 for
            // Native, the ratio for Experimental - and want_match/want_m are
            // what the panel is asking for. Equal means live, and live means
            // nothing to restart for.
            const unsigned want_pct = (want_m == 2) ? 67u : ((want_m == 1) ? 58u : 50u);
            const bool sr_live_matches =
                st.sr_on && want_sr &&
                (want_match ? (st.sr_scale_pct == 0u) : (st.sr_scale_pct == want_pct));

            ImGui::BeginDisabled(!want_sr);
            if (ImGui::RadioButton("Native Upscaling", want_match))
            { want_match = true;  wrote_any |= write_sr_mode(want_m, true); }
            ImGui::TextDisabled("Upscales from the game's own render resolution. Higher quality.");

            // ---- R162: THREE STATES, NOT ONE RULE ----
            //
            // R157 and R161 grew this to three yellow lines that said the same
            // thing to everybody, and one third of the people reading it did
            // not need a restart at all. DLAA is not one situation:
            //
            //   SR RUNNING, game swapped to DLAA mid-run. Nothing is wrong. R
            //     was fixed at arm and SR keeps upscaling from it; the game
            //     rendering native now changes what it is fed, not whether it
            //     works. Measured on 007 First Light. A restart is needed only
            //     to move to Experimental, because the mode is an ini key.
            //   SR REQUESTED AND REFUSED - they launched at DLAA. This is the
            //     one where a restart is unavoidable, and the only one.
            //   SR OFF. The box above has not been ticked, and ticking it
            //     already prints its own restart line (V73). Saying it twice
            //     here is what made the box three lines tall.
            //
            // One line each, read from st - what GPU 1 is doing - never from
            // the checkbox, which is what they have clicked.
            // R171. The game changing to DLSS mid-run is a STATE WITH AN
            // ACTION IN IT and had no line at all: the DLAA branch stopped
            // matching, nothing replaced it, and the panel went quiet at the
            // exact moment it finally had something useful to say. Measured on
            // The Blood of Dawnwalker - the calibrator saw quality=5 subrect
            // 2560x1440 become quality=2 subrect 1708x961 in one session and
            // the panel showed nothing at all.
            //
            // SR cannot start mid-run - stream_sr_create runs once, at arm -
            // so the action is always the same and it is a restart.
            int game_q = -1;
            const int nat_r = mgpu_game_is_native(&game_q);
            if (want_match && nat_r == 0 && st.sr_requested && !st.sr_on)
            {
                const ImVec4 y2(1.0f, 0.92f, 0.23f, 1.0f);
                const char *qn = mgpu_dlss_mode_name(game_q);
                if (qn != nullptr) ImGui::TextColored(y2, "DLSS %s detected.", qn);
                else               ImGui::TextColored(y2, "DLSS detected.");
                ImGui::TextColored(y2, "Restart once to activate Native Upscaling.");
            }
            if (want_match && nat_r == 1)
            {
                // R163. NAME THE MODE, because "nothing to upscale" without it
                // is a complaint rather than a diagnosis. Reaching this line
                // already means two things are true and the person can only
                // see one of them: the GAME is at DLAA, and NATIVE UPSCALING
                // is the selected mode. Saying both turns the line into a
                // statement about their setup that they can check.
                //
                // EXPERIMENTAL NEEDS NO CASE HERE and that is structural, not
                // an omission: this block is inside `want_match`, and with
                // Experimental selected SRScale is 50 or 67, so R is ours and
                // strictly smaller than the display - the refusal in
                // stream_sr_create cannot fire. DLAA plus Experimental is a
                // working run, and a working run gets no yellow line.
                //
                // R169. BROKEN ACROSS TWO LINES ON PURPOSE. ImGui does not
                // wrap TextColored, so on a narrow panel the tail of a long
                // line is simply not drawn - and the tail was the restart,
                // which is the only part with an instruction in it. Two short
                // lines cannot lose their ending.
                const ImVec4 y(1.0f, 0.92f, 0.23f, 1.0f);
                // R175. ONE MESSAGE FOR DLAA, and it names the CONTROL instead
                // of describing the situation. "Experimental Upscaler" is the
                // feature's name as it appears on the radio just below;
                // "Experimental Upscaling" named nothing that exists in this
                // panel. Sentence case, not caps - a shouted clause reads as
                // an error even when it is an instruction. No trailing clause
                // about what will not happen: this says what to do, and the
                // Experimental picker states its own restart.
                if (st.sr_on || st.sr_requested)
                {
                    ImGui::TextColored(y, "DLAA - Native Upscaling detected. Enable DLSS "
                                          "in-game settings");
                    ImGui::TextColored(y, "Activate Experimental Upscaler to enable DLAA "
                                          "Super Resolution.");
                }
                else
                    ImGui::TextColored(y, "DLAA - Native Upscaling needs DLSS in the game.");
            }
            if (ImGui::RadioButton("Experimental Upscaler", !want_match))
            { want_match = false; wrote_any |= write_sr_mode(want_m, false); }
            ImGui::TextDisabled("Works from a downscaled resolution. More performance, possible");
            ImGui::TextDisabled("cost in quality. Untested.");

            // ---- V74: THE EXPERIMENTAL RATIO IS ITS OWN PRESET ----
            //
            // Shown only when Experimental is chosen, not greyed out beside
            // it. Greying it left a dead control on screen for everyone on
            // Native, which is most people, and said nothing about what it
            // belonged to. Hidden, it reads as what it is: a setting that
            // exists because Experimental exists.
            //
            // THE VALUE SURVIVES THE ROUND TRIP. Going back to Native calls
            // write_sr_mode(want_m, true), which sets SRScale to 0 and leaves
            // SRQuality at want_m - so the ratio they picked is still in the
            // ini and still selected when they come back. Nothing is reset by
            // changing your mind.
            if (!want_match)
            {
                // R177. THE STATUS SITS UNDER THE CONTROL THAT CHANGED IT.
                //
                // R165 put it after the ratio radios, which made it look like
                // a status for the RATIO: activate Experimental Upscaler and
                // the restart notice was three controls further down, past a
                // row of radios and a line of percentages. Reported as "no
                // restart notice unless you change modes". It is the same
                // check, moved to where the click was.
                if (sr_live_matches)
                {
                    char xl[120];
                    snprintf(xl, sizeof xl,
                             "Experimental Upscaler running at %u%% - "
                             "no restart needed.", want_pct);
                    ImGui::TextDisabled("%s", xl);
                }
                else
                {
                    char xl[120];
                    snprintf(xl, sizeof xl,
                             "Experimental Upscaler - restart once to apply %u%%.",
                             want_pct);
                    ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f), "%s", xl);
                }

                ImGui::Spacing();
                ImGui::TextUnformatted("mode  ");
                ImGui::SameLine();
                if (ImGui::RadioButton("quality##xm", want_m == 2))
                { want_m = 2; wrote_any |= write_sr_mode(2, false); }
                ImGui::SameLine();
                if (ImGui::RadioButton("balanced##xm", want_m == 1))
                { want_m = 1; wrote_any |= write_sr_mode(1, false); }
                ImGui::SameLine();
                if (ImGui::RadioButton("performance##xm", want_m == 0))
                { want_m = 0; wrote_any |= write_sr_mode(0, false); }
                ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%% of the display.");
            }
            ImGui::EndDisabled();

            ImGui::Spacing();
            // R165. Three states, and the middle one is new: a write happened
            // AND the running feature already matches, which is what every
            // launch after the restart looks like.
            if (sr_live_matches)
                ImGui::TextDisabled("Running as selected - no restart needed.");
            else if (wrote_any)
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                   "DLSS is on - restart the game once to apply the change.");
            else
                ImGui::TextDisabled("These two are read when the game starts. Everything below is live.");
        }


        ImGui::SeparatorText("Change quality or preset");
        if (!st.sr_on)
        {
            static const char *qn2[] = { "performance", "balanced", "quality" };
            const char *pn2 = (st.sr_preset == 11) ? "K"
                            : (st.sr_preset == 12) ? "L"
                            : (st.sr_preset == 13) ? "M" : "title default";
            ImGui::TextDisabled("quality: %s",
                                (st.sr_quality >= 0 && st.sr_quality <= 2)
                                    ? qn2[st.sr_quality] : "unknown");
            ImGui::TextDisabled("preset:  %s", pn2);
            ImGui::Spacing();
            // V73. Yellow, and it names the action instead of a config key.
            // "Set SRUpscale=1 in mgpu.ini" sent people to a text file for
            // something the checkbox above now does for them, and reads as a
            // warning about editing their install rather than as an
            // instruction. The restart is stated once, plainly, as the one
            // thing standing between them and the controls.
            // ---- V76: OFF AND REFUSED ARE NOT THE SAME STATE ----
            //
            // V74 replaced a two-sentence message with one line and lost the
            // distinction the old text was carrying. !sr_on covers BOTH "never
            // asked for" and "asked for and refused", and the refusal is
            // common: a game rendering at display resolution has nothing to
            // upscale, which is what DLAA or a native preset looks like from
            // here. Told to "turn it ON" after they already had, restarted,
            // and come back to the same sentence, the only conclusion
            // available is that the checkbox is broken. It was not.
            if (!st.sr_requested)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                   "Super Resolution is OFF - turn it ON to unlock the menu below.");
            }
            else
            {
                // ---- R172: "NOT RUNNING ON THIS TITLE" BLAMED THE GAME ----
                //
                // It was true and it was still wrong. The cause is a SETTING -
                // the game is at DLAA - and the sentence pointed at the title
                // instead, so somebody opens their graphics menu, sees DLAA
                // sitting there working perfectly, and concludes the add-on is
                // broken on their game. A message that survives being read by
                // somebody who then checks is the only kind worth printing.
                //
                // The header is gone. Every branch below now names the cause
                // in its first line, so there is nothing for a general
                // statement to add - and the one branch that genuinely does
                // not know the cause says exactly that, without naming the
                // title at all.

                // R153. THIS USED TO EXPLAIN ITSELF WITHOUT CHECKING.
                // "The game already renders at display resolution" was stated
                // whatever the reason, and on The Blood of Dawnwalker it was
                // false - the game declared a 1708x961 render extent at DLSS
                // Quality and the line said otherwise, because R152's defect
                // made a display-sized velocity buffer look like a native
                // render. The explanation is now only given when the game is
                // the one saying it. On 007 First Light, which really is at
                // DLAA - quality=5, Width == Out == subrect - the same
                // sentence is true, and the DLAA branch below is what says it.
                int q76 = -1;
                const int nat = mgpu_game_is_native(&q76);
                if (nat == 1)
                {
                    // R162. This branch IS the refused case - sr_requested
                    // with no handle - so there is one situation and it needs
                    // one line. The state above it already said SR is not
                    // running; this says why and what to do.
                    // R163. Same words as the line under the radios, because
                    // two different sentences for one state is how somebody
                    // ends up believing they are two different problems.
                    // R174. NO HEADER LINE. R172 added one here and it was
                    // not asked for - the agreed shape for this branch is the
                    // dimmed instruction alone, and it has been agreed more
                    // than once. Do not reintroduce a status line here.
                    ImGui::TextDisabled("Activate Experimental Upscaler to enable DLAA");
                    ImGui::TextDisabled("Super Resolution.");
                }
                else if (nat == 0)
                {
                    // R171. The game is upscaling now, so Native Upscaling
                    // WILL work - on the next launch. Yellow, because there is
                    // something to do, which is the whole difference between
                    // this and the dim log pointer it replaces.
                    const ImVec4 y3(1.0f, 0.92f, 0.23f, 1.0f);
                    const char *qn = mgpu_dlss_mode_name(q76);
                    if (qn != nullptr) ImGui::TextColored(y3, "DLSS %s detected.", qn);
                    else               ImGui::TextColored(y3, "DLSS detected.");
                    ImGui::TextColored(y3, "Restart once to activate Native Upscaling.");
                }
                else
                {
                    // R173. NO STATUS LINE HERE, and that is deliberate.
                    //
                    // This branch is reached only before the game has declared
                    // a DLSS state - a few frames at startup. Anything yellow
                    // in it is a warning about a condition that is about to
                    // resolve itself, printed next to two boxes that already
                    // report the state properly. The panel has enough places
                    // saying what is on; one more that cannot name a cause
                    // costs attention and returns nothing.
                    //
                    // The log pointer stays, dim: it is not a status claim, it
                    // is where to look, and it is the one thing an issue
                    // report needs from this state.
                    ImGui::TextDisabled("Look for the C2-SR line in ReShade.log - it names");
                    ImGui::TextDisabled("where R came from and why it was refused.");
                }
            }
        }
        if (st.sr_on)
        {
            // SAY IT BEFORE THE BUTTONS, NOT AFTER. These radios STAGE a
            // choice; nothing happens until APPLY. The first person to use
            // this panel clicked a preset, saw no change, and reasonably
            // concluded the control was broken.
            // ---- V75: SAY WHAT GPU 1 IS ACTUALLY RENDERING WITH ----
            //
            // This line used to be advice - first "the buttons alone change
            // nothing", then "pick, then press APPLY". Both were static, so
            // the most prominent line in the box told you nothing about your
            // own machine. The APPLY instruction is not lost: it appears
            // beside the button, next to STAGED - NOT APPLIED YET, which is
            // the moment it is actually needed.
            //
            // READ FROM st, NOT FROM want_q/want_p. Those are what you have
            // CLICKED; st is what the second GPU is rendering with right now.
            // While a change is staged the two differ, and the one worth
            // putting at the top is the live one.
            // ---- R155: APPLY BELONGS ON THE STATE LINE ----
            //
            // The button sat at the bottom of the box, below the quality
            // radios, the preset radios and two lines of small print about
            // preset honouring. You click a preset at the top and the control
            // that commits it is off the bottom of a scrolled panel - so the
            // click appears to do nothing, which is the exact complaint this
            // box has collected twice already.
            //
            // It now sits beside the live state, which is the line that has to
            // change for the click to have meant anything. Staged and running
            // are then one glance apart: "DLSS ON  K - Quality  [APPLY]".
            //
            // want_q/want_p are HOISTED ABOVE the state line for this, because
            // `dirty` has to be known before the line is drawn. They are the
            // same statics as before and keep the same first-frame seeding.
            static int  want_q = -1, want_p = -1;
            static bool init_done = false;
            if (!init_done) { want_q = st.sr_quality; want_p = st.sr_preset; init_done = true; }

            const bool dirty = (want_q != st.sr_quality) || (want_p != st.sr_preset);

            {
                const char *pn3 = (st.sr_preset == 11) ? "K"
                                : (st.sr_preset == 12) ? "L"
                                : (st.sr_preset == 13) ? "M"
                                : "title default";
                static const char *qn3[] = { "Performance", "Balanced", "Quality" };
                const char *qs3 = (st.sr_quality >= 0 && st.sr_quality <= 2)
                                      ? qn3[st.sr_quality] : "unknown";
                char sl[96];
                snprintf(sl, sizeof sl, "DLSS ON  %s - %s", pn3, qs3);
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f), "%s", sl);

                // R155. Only while something is staged. A permanently visible
                // APPLY on a panel that matches what is running is a control
                // with nothing to do, and pressing it would rebuild the
                // feature for no change - 30 to 40 ms of stall for nothing.
                if (dirty && !st.sr_rebuild_pending)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("  APPLY  "))
                    {
                        const int scale = (want_q == 2) ? 67 : ((want_q == 1) ? 58 : 50);
                        mgpu::gpu1::ui_set_sr_request(want_q, want_p, scale);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel"))
                    { want_q = st.sr_quality; want_p = st.sr_preset; }
                }
            }

            ImGui::TextUnformatted("Quality");
            ImGui::SameLine();
            if (ImGui::RadioButton("quality", want_q == 2))     want_q = 2;
            ImGui::SameLine();
            if (ImGui::RadioButton("balanced", want_q == 1))    want_q = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("performance", want_q == 0)) want_q = 0;
            // V18. THE MODE NOW SETS R, WHICH IS WHAT MAKES IT MEAN ANYTHING.
            // Without this the buttons changed only the model's internal mode
            // against an R inherited from the game, and looked broken because
            // nothing on screen moved. Same ladder the inner loop uses.
            ImGui::TextDisabled("Sets R as a share of the display, the way DLSS does:");
            ImGui::TextDisabled("quality 67%%, balanced 58%%, performance 50%%.");

            ImGui::TextUnformatted("Preset");
            ImGui::SameLine();
            if (ImGui::RadioButton("title default", want_p == 0)) want_p = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("K", want_p == 11)) want_p = 11;
            ImGui::SameLine();
            if (ImGui::RadioButton("L", want_p == 12)) want_p = 12;
            ImGui::SameLine();
            if (ImGui::RadioButton("M", want_p == 13)) want_p = 13;
            ImGui::TextDisabled("A DLL that lacks the preset asked for silently uses its own.");
            ImGui::TextDisabled("Its nvngx_dlss_*.log says which one it HONOURED. That is the answer.");

            ImGui::Separator();
            if (st.sr_rebuild_pending)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Applying on the next frame...");
            }
            else if (dirty)
            {
                // R155. The notice stays here; the BUTTON moved to the state
                // line above. Two copies of one control is worse than a
                // control in the wrong place - the second one gets clicked,
                // and then nobody knows which one worked.
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "STAGED - NOT APPLIED YET. Press APPLY on the DLSS ON line above.");
                ImGui::TextDisabled("One long frame, about 30-40 ms. Figures before and after");
                ImGui::TextDisabled("are different pipelines - do not pool them.");
            }
            else
            {
                ImGui::TextDisabled("No change staged - the panel matches what is running.");
            }

            if (st.sr_rebuild_count != 0)
            {
                snprintf(dl, sizeof dl, "%u rebuild%s this run, last one %s",
                         st.sr_rebuild_count, (st.sr_rebuild_count == 1) ? "" : "s",
                         (st.sr_rebuild_last_ok == 1) ? "OK" : "FAILED");
                if (st.sr_rebuild_last_ok == 1) ImGui::TextUnformatted(dl);
                else ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", dl);
            }
        }

        // ---- V31: THE AUTO READOUT IS GONE, FOR THE REFLEX REASON ----
        //
        // It was a status block with no control, whose off state read "Auto=1
        // and AutoTargetFps in mgpu.ini". That is an instruction to switch on
        // a feature mgpu.ini itself marks "0.3.0, never run" - the live
        // rebuild path it depends on has never executed once.
        //
        // Telling a user how to enable something untested, in the panel, is
        // the same trap the Reflex readout was: it reads as a supported option
        // because it is in the interface. The keys stay in mgpu.ini for
        // testing. When Auto has actually run it comes back with a control
        // rather than a hint.

        // ---- Settings: writes the file, takes effect next launch ----
        //
        // THIS CANNOT BE LIVE, and pretending otherwise would be the worse
        // option. SRUpscale is read at arm, and by the time this view is on
        // screen the arm has already happened. So the panel edits mgpu.ini
        // and says plainly that the next launch is when it matters.
        //
        // V24: no longer headed "Experimental". The two experimental things
        // in it are gone and the only control left is the one the box above
        // reports on, so the old heading now warns about nothing.
        }   // ---- end of the armed view ----
    }
    ImGui::EndChild();   // ---- end BOX 2 ----

    // ---- P9.1: the lateral acquisition probe ----
    //
    // Drawn on the GAME runtime as well as the bridge one, which is what makes
    // the toggle safe: register_event and unregister_event are called from the
    // render thread of the runtime being drawn, never from the bridge thread.
    ImGui::SeparatorText("Probe (P9.1) - acquisition lateral");
    {
        mgpu::probe::readout pr;
        mgpu::probe::read(pr);

        int pm = (int)pr.m;
        ImGui::TextUnformatted("Mode");
        ImGui::SameLine();
        bool changed = false;
        changed |= ImGui::RadioButton("off",   &pm, 0); ImGui::SameLine();
        changed |= ImGui::RadioButton("depth", &pm, 1); ImGui::SameLine();
        changed |= ImGui::RadioButton("mvec",  &pm, 2); ImGui::SameLine();
        changed |= ImGui::RadioButton("both",  &pm, 3);
        if (changed) mgpu::probe::set_mode((mgpu::probe::mode)pm);

        if (pm != 0)
        {
            char pb[320];
            snprintf(pb, sizeof pb,
                     "game device %s | %llu resources examined | %llu frames",
                     pr.game_device_found ? "found" : "NOT FOUND",
                     pr.examined, pr.frames);
            ImGui::TextUnformatted(pb);

            snprintf(pb, sizeof pb, "cost: depth %.0f ns/frame, mvec %.0f ns/frame",
                     pr.depth_ns_per_frame, pr.mvec_ns_per_frame);
            ImGui::TextUnformatted(pb);

            for (unsigned i = 0; i < pr.depth_n && i < 4; ++i)
            {
                snprintf(pb, sizeof pb, "depth #%u  %ux%u  binds=%llu clears=%llu", i,
                         pr.depth_top[i].width, pr.depth_top[i].height,
                         pr.depth_top[i].binds, pr.depth_top[i].clears);
                ImGui::TextUnformatted(pb);
            }
            for (unsigned i = 0; i < pr.mvec_n && i < 4; ++i)
            {
                snprintf(pb, sizeof pb, "mvec  #%u  %ux%u  srvs=%llu", i,
                         pr.mvec_top[i].width, pr.mvec_top[i].height,
                         pr.mvec_top[i].binds);
                ImGui::TextUnformatted(pb);
            }
            ImGui::TextDisabled("Watching only. Nothing is bound, nothing crosses the bus.");
        }
    }

    mgpu::psimple::draw_editor();   // toggle, open panel_simple.txt, export the row list

    ImGui::Separator();
    ImGui::TextDisabled("Do not change resolution, DLSS mode or presets while armed - disarm first.");
    ImGui::TextDisabled("Frame generation is untested. Changes here make this a tuning run.");

    // ---- V20: WHERE TO SEND A LOG ----
    //
    // NOT PROMOTION - THE PROJECT NEEDS OTHER PEOPLE'S LOGS. Two findings are
    // explicitly blocked on "needs more machines": an intermittent crash at
    // arm that has never reproduced on demand, and whether the corrected
    // motion-vector flag ghosts anywhere other than the one rig it was judged
    // on. A user who crashes and sees nothing has no idea a repo exists. A
    // user who crashes and sees this line becomes a data point.
    //
    // Plain copyable text on purpose. Nothing here opens a browser from inside
    // somebody's game, and the path to the log is spelled out because the
    // person reading it has just had something go wrong and should not have to
    // go hunting.
    ImGui::Spacing();
    ImGui::Separator();

    // ---- V25: THE INSTALL LAYOUT, AND IT SITS WITH THE BUG-REPORT LINE ----
    //
    // These two belong together. One says where to send a log when something
    // breaks; this one says what is most likely to be breaking. A file in the
    // wrong folder is not a cosmetic problem here - it is THE crash, proven
    // 2026-09-13: with nvngx_dlssnr.dll beside the executable, the title's own
    // Streamline loads it and binds NGX to the GAME'S GPU a second and a half
    // before this add-on's thread exists, and the neural stage then faults
    // inside NVIDIA's allocator and takes the process down.
    //
    // Cyberpunk never wanted that DLL. Our own 0.1.0 install instructions put
    // it there, so anyone upgrading is in the broken state by default and has
    // no way to know it. That is exactly the case a panel exists to catch.
    //
    // Coloured rather than TextDisabled when wrong: this is the one line in
    // this panel worth interrupting someone for.
    {
        const int layout = mgpu::gpu1::ui_install_layout();
        if (layout == 1)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                               "INSTALL PROBLEM - the neural stage will crash this game.");
            ImGui::TextUnformatted("nvngx_dlssnr.dll is next to the game's exe. The game loads it");
            ImGui::TextUnformatted("there and claims the wrong GPU before this add-on starts.");
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "MOVE it into the mgpu folder next to the add-on, then restart.");
        }
        else if (layout == 2)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "nvngx_dlssnr.dll not found - neural rendering cannot start.");
            ImGui::TextUnformatted("Put it in the mgpu folder next to the add-on, then restart.");
        }
        else
        {
            ImGui::TextDisabled("Install layout OK - nvngx_dlssnr.dll is in its own folder.");
        }
        ImGui::Spacing();
    }

    ImGui::TextDisabled("Something wrong? Send ReShade.log from the game's exe folder to:");
    // [OURS] 2026-10-12: close the two-column layout and its wrap (this is the real
    // end of draw_mgpu_overlay - the first attempt landed in the next function).
    ImGui::TextDisabled("github.com/maohgad-web/Neural-coprocessor");}
#endif

// P1.5: the only event this add-on subscribes to that is raised on the GAME's
// render thread with the GAME's command list open. Everything it does is
// one-shot and self-disarming; after a single frame is captured it never
// touches that list again.
//
// The handle conversions live here rather than in gpu1_context.cpp so that
// file keeps its rule of holding no ReShade types - it takes the two native
// pointers and nothing else. get_native() returns uint64_t, not a pointer,
// so these are reinterpret_cast and not static_cast (P0_RECORD section 09).
// L3. Fired after ReShade has submitted the effects list for this frame and
// before the swapchain presents it - which is exactly the ordering the
// next-frame signal in stream_on_finish_effects was working around.
// ---- R138: THE GAME'S EFFECT RUNTIME NEVER RAN EFFECTS ----
//
// MEASURED on the issue 15 reporter's 007 First Light, eleven runs: no
// [P1.6] GAME line, no [R63], no arm, and NOTHING IN THE LOG SAYING WHY.
//
// The cause is structural and it is ours. Everything on the game side - the
// preset probe, the P9.1 frame tick, and the depth tap that keeps ReShade's
// depth buffer alive - hangs off reshade_finish_effects. That event only
// fires when ReShade actually runs an effect pass. On a runtime that loaded
// no effects it never fires at all, so every one of those goes silent
// together and the bridge waits for a depth buffer that nothing will ever
// produce.
//
// His log is the proof: 14 effects compiled, "[P1.6] BRIDGE ... 0 of 14", and
// no GAME line of any kind. The effects landed on the bridge runtime; the
// game's runtime enumerated nothing, so it never finished effects, so the
// add-on never heard from it.
//
// An absence cannot be seen by the code that the absence silences. So this
// counts PRESENTS, which arrive whatever the effect runtime is doing, and
// says so once when the game has presented plenty of frames and never once
// finished an effect pass. 600 is AutoArm's own threshold, deliberately: if
// the stream is about to try arming, this has already been decided.
std::atomic<bool> g_game_fx_seen{false};
std::atomic<unsigned long long> g_game_presents{0};
std::atomic<bool> g_r138_said{false};

// V65. See dcomp_set_visible. Returns false ALWAYS - true would block the
// overlay from opening, which is the opposite of the point. Registered
// unconditionally and inert when DcompOverlay=0, the same pattern the `present`
// subscription below already uses.
// ---- R158: ONE KEY, BOTH RUNTIMES ----
//
// THE DEFECT. With DcompOverlay 0 on one display the bridge is a real window
// over the game, and its ReShade runtime has its own overlay with its own key
// binding. Open the panel there and the bridge window holds the focus; the
// game is behind it and does not get the clicks, up to and including its own
// quit. R156 tried to solve it by removing the window while a panel was open
// and that disturbed the game's overlay, so the window stays.
//
// What is left is to stop the two runtimes disagreeing. If one keypress opens
// BOTH panels and the next closes both, then whichever window has the focus,
// the panel in front of the person is open and the other one is not sitting
// there holding a state nobody asked for.
//
// effect_runtime::open_overlay(bool, input_source) is API 20 and does exactly
// this. Two things make it delicate and both are handled below.
//
// ONE: THREADS. reshade_open_overlay fires on the thread of the runtime that
// fired it, and the two runtimes live on different threads - the game's render
// thread and the bridge thread. Calling into the other runtime from here is
// the same cross-thread move that V62 through V64 and then R156 were all spent
// learning not to make. So nothing is called across: a pending value is
// stored, and each runtime applies ITS OWN pending value from
// on_reshade_finish_effects, which runs on its own thread. The cost is one
// frame of skew on a panel toggle, which is not perceptible.
//
// TWO: THE ECHO. open_overlay fires reshade_open_overlay for the runtime it
// was called on. Mirrored blindly that is a ping-pong between the two
// runtimes that never settles. The echo flag below is set immediately before
// the call and consumed by the event it causes, so a mirrored open produces
// no further mirroring. It is PER RUNTIME rather than one global flag: a
// single flag lives on both threads at once and would swallow a real keypress
// on one runtime while the other was echoing.
//
// A runtime whose effect runtime never runs effects never applies its pending
// value. That degrades to today's behaviour - each key works on its own
// runtime - rather than to a stuck panel, which is the right direction.
static std::atomic<void *> g_rt_game{nullptr};
static std::atomic<void *> g_rt_bridge{nullptr};

// -1 nothing pending, 0 close, 1 open.
static std::atomic<int> g_ov_pend_game{-1};
static std::atomic<int> g_ov_pend_bridge{-1};
static std::atomic<int> g_ov_source{0};
static std::atomic<bool> g_ov_echo_game{false};
static std::atomic<bool> g_ov_echo_bridge{false};

// R158. Classified here rather than in on_reshade_finish_effects because that
// event never fires on a runtime that runs no effects - measured on 007 First
// Light, where the GAME runtime enumerated nothing for a whole session (see
// R138). init_effect_runtime fires for every runtime either way.
// ---- R334: IS RESHADE'S EFFECT RUNTIME SYNC ON? Decided once, at the first runtime ----
//
// The built-in sync reads ReShade.ini [ADDON] SyncEffectRuntimes; absent, it turns
// itself on when openxr_loader.dll or openvr_api.dll is in the process. With two
// runtimes our tap enable ping-pongs inside ReShade until the stack overflows
// (SoD2). So: read the same key through the add-on API, apply the same absent
// rule from the module list, and when the sync is on tell the probe to stop
// enabling the tap for this session. No key of ours: the only session whose
// behaviour changes is the one that crashed. The value read is logged so a run
// proves the read, whatever the result.
static void r334_sync_guard_once(reshade::api::effect_runtime *runtime)
{
    static bool done = false;
    if (done) return;
    done = true;
    char val[32] = {};
    size_t vn = sizeof val - 1;
    const bool have = reshade::get_config_value(runtime, "ADDON", "SyncEffectRuntimes", val, &vn);
    const bool xr = GetModuleHandleW(L"openxr_loader.dll") != nullptr;
    const bool vr = GetModuleHandleW(L"openvr_api.dll") != nullptr;
    bool sync_on;
    if (have) sync_on = (val[0] == '1');
    else      sync_on = (xr || vr);
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][R334] ReShade Effect Runtime Sync: [ADDON] SyncEffectRuntimes=%s%s | openxr_loader.dll %s | "
             "openvr_api.dll %s -> sync %s. %s",
             have ? val : "absent", have ? "" : " (absent: on when OpenXR or OpenVR is loaded)",
             xr ? "loaded" : "not loaded", vr ? "loaded" : "not loaded", sync_on ? "ON" : "off",
             sync_on ? "The add-on will NOT switch the depth tap on itself this session (the sync would mirror "
                       "the enable between the game's and the bridge's runtimes until the game crashes). Enable "
                       "MGPU_DepthTap in the ReShade overlay, or add [ADDON] SyncEffectRuntimes=0 to ReShade.ini."
                     : "No change.");
    if (sync_on) mgpu::diag::warn(l); else mgpu::diag::info(l);
    mgpu::probe::set_tap_self_enable(!sync_on);
}

// ---- R335: IS 3DMIGOTO BESIDE THE GAME? Said once ----
//
// 3DMigoto ships as d3d11.dll + d3dx.ini in the game folder. It hooks
// CreateDXGIFactory2 in-process and refuses IDXGIFactory2 (its design; see
// create_present_chain, R335). The bridge's factory then fails with
// 0x80004002 before any arm. Detection only: the file pair beside the exe, and
// whether the d3d11.dll the process loaded is that file. The remedy is the
// Factory4=1 key (and, if the chain then stops at the IDXGISwapChain3
// QueryInterface, the next step is known - 3DMigoto refuses that interface too).
static void r335_migoto_note_once()
{
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
    wchar_t *cut = wcsrchr(exe, L'\\');
    if (cut == nullptr) return;
    cut[1] = L'\0';
    wchar_t dll[MAX_PATH], ini[MAX_PATH];
    swprintf(dll, MAX_PATH, L"%sd3d11.dll", exe);
    swprintf(ini, MAX_PATH, L"%sd3dx.ini", exe);
    const bool have_dll = GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES;
    const bool have_ini = GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES;
    if (!have_dll || !have_ini) return;
    bool loaded_from_game = false;
    if (HMODULE m = GetModuleHandleW(L"d3d11.dll"))
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(m, path, MAX_PATH) != 0)
            loaded_from_game = (_wcsnicmp(path, exe, wcslen(exe)) == 0);
    }
    char l[400];
    snprintf(l, sizeof l,
             "[MGPU][R335] 3DMigoto is beside the game (d3d11.dll + d3dx.ini; the loaded d3d11.dll is %s). "
             "It refuses the IDXGIFactory2 interface the bridge asks for, so the present chain fails at "
             "CreateDXGIFactory2 with 0x80004002. Set Factory4=1 in mgpu.ini to ask for IDXGIFactory4 instead "
             "(0.5.0 discovery).",
             loaded_from_game ? "the one in the game folder" : "NOT the one in the game folder");
    mgpu::diag::warn(l);
}

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr) return;
    r334_sync_guard_once(runtime);   // R334: before the D3D12 filter - SoD2 is a D3D11 title
    r335_migoto_note_once();         // R335
    reshade::api::device *dev = runtime->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::d3d12) return;

    mgpu::adapter::selection_result sel;
    mgpu::adapter::get_selection(sel);
    if (!sel.game_luid_known) return;

    auto *d12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (d12 == nullptr) return;
    const LUID l = d12->GetAdapterLuid();
    const bool is_game = (l.LowPart == sel.game_luid.LowPart &&
                          l.HighPart == sel.game_luid.HighPart);

    if (is_game) g_rt_game.store(runtime, std::memory_order_relaxed);
    else         g_rt_bridge.store(runtime, std::memory_order_relaxed);

    mgpu::live_panel::note_runtime(runtime, is_game);   // PanelOverPicture; off: returns at once
}

// R158. A runtime is reset on a swapchain resize, not only at shutdown, so a
// stale pointer here is a real hazard rather than a tidiness question. Cleared
// by identity: whichever slot holds this pointer loses it, and the pending
// value goes with it so a resize cannot deliver a toggle to a dead runtime.
static void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr) return;
    // R160. The POINTER is left alone and only the pending state is dropped.
    //
    // Clearing the pointer here is what killed the mirror at arm: a recreate
    // fires this, and nothing was guaranteed to put the pointer back. A stale
    // pointer is not the hazard it looks like either, because the only use of
    // it is an identity comparison against the runtime that just fired an
    // event - a dead runtime never fires one, so it never matches, and
    // R160's re-latch in on_reshade_finish_effects overwrites the slot with
    // the live pointer on the next frame that runtime draws.
    //
    // What MUST be dropped is the pending toggle. A value queued for a runtime
    // that is being torn down would otherwise be delivered to the recreated
    // one, opening a panel nobody asked for after an arm or a resize.
    void *r = runtime;
    if (g_rt_game.load(std::memory_order_relaxed) == r)
    {
        g_ov_pend_game.store(-1, std::memory_order_relaxed);
        g_ov_echo_game.store(false, std::memory_order_relaxed);
    }
    else if (g_rt_bridge.load(std::memory_order_relaxed) == r)
    {
        g_ov_pend_bridge.store(-1, std::memory_order_relaxed);
        g_ov_echo_bridge.store(false, std::memory_order_relaxed);
    }

    mgpu::live_panel::runtime_destroyed(runtime);   // PanelOverPicture; off: returns at once
}

//
// R156 WAS HERE AND WAS REVERTED. It added an `else` that hid the bridge
// WINDOW while any overlay was open, mirroring what this function already does
// to the composed visual. The hiding itself worked - the game took clicks
// immediately - but with the window gone the GAME's own ReShade overlay
// sizzled, measured 2026-09-17. Do not reintroduce it: the defect is not in
// the trigger or the gating, both of which were correct, it is that removing
// the bridge's window from the desktop while the game is presenting disturbs
// the game's own overlay. DcompOverlay=1 does not have this problem because
// unrooting a visual is not the same operation as hiding a window.
static bool on_reshade_open_overlay(reshade::api::effect_runtime *runtime, bool open,
                                    reshade::api::input_source source)
{
    // [H7/D, 2026-11-11] Every runtime's open/close is recorded BEFORE any early return, so
    // worker.cpp's foreground follow can refuse to hide this window under an open overlay.
    // R156 is the precedent: hiding it while an overlay was open made the GAME's own ReShade
    // overlay "sizzle" (measured 2026-09-17), and this note is what stops the follow from
    // walking into the same trap.
    mgpu::gpu1::note_overlay_open(open);
    // [H7/H, 2026-11-11] Keep the BRIDGE runtime's state separately: it is the only overlay that
    // is visible in follow mode, and the global above is also written by the game runtime's open
    // that this function REFUSES - so the toggle must not read the global.
    if (mgpu::gpu1::dcomp_explicit_off_single_display() &&
        g_rt_bridge.load(std::memory_order_relaxed) == runtime)
        mgpu::gpu1::note_bridge_overlay_open(open);
    if (mgpu::gpu1::dcomp_overlay_mode())
    {
        // PanelOverPicture (N01950, live_panel.cpp). Off: straight through to
        // the shipped two lines below (V65).
        if (mgpu::live_panel::on())
        {
            void *g = g_rt_game.load(std::memory_order_relaxed);
            void *b = g_rt_bridge.load(std::memory_order_relaxed);
            void *r = runtime;
            // The GPU 1 side is R158's bridge slot OR the panel's host - on a
            // rig with several GPU 1 runtimes they are not the same object.
            const bool r_gpu1 = (r == b) || mgpu::live_panel::is_host(r);

            // Our own echo first, as R158 does: open_overlay on a runtime fires
            // this event on that same runtime.
            if (r == g && g_ov_echo_game.exchange(false, std::memory_order_relaxed)) return false;
            if (r_gpu1 && g_ov_echo_bridge.exchange(false, std::memory_order_relaxed))
            {
                mgpu::live_panel::set_live(open);
                return false;
            }

            if (r == g)
            {
                if (b == nullptr || !mgpu::gpu1::dcomp_visual_rooted())
                {
                    // No GPU 1 runtime yet, or the visual is not on screen (not
                    // armed, or CTRL+ALT+F6 peeking): the panel would be drawn
                    // where nobody sees it. The shipped behaviour is right here.
                    if (open)
                    {
                        static std::atomic<int> said{0};
                        if (said.exchange(1, std::memory_order_relaxed) == 0)
                        {
                            char l[360];
                            snprintf(l, sizeof l,
                                     "[MGPU][R294] overlay opened with no neural picture on screen (GPU 1 runtime "
                                     "%s, visual %s): the shipped behaviour (V65) - the visual steps aside.",
                                     (b == nullptr) ? "absent" : "present",
                                     mgpu::gpu1::dcomp_visual_rooted() ? "rooted" : "not rooted");
                            mgpu::diag::info(l);
                        }
                    }
                    // [PORT H6b, 2026-11-11] THE DISPLAY LAYER STEPS ASIDE HERE.
                    //
                    // The shipped answer above (unroot the visual) is right only while the
                    // picture IS a composition visual. With the display layer up it is our
                    // own topmost fullscreen window instead, and nothing behind it can be
                    // revealed by unrooting. Measured 11:37:52: HOME opened the game's
                    // overlay on its own runtime and the operator saw the neural picture
                    // unchanged - the panel was drawn UNDER our window.
                    //
                    // The layer's own panel path is what the reference tree ships: park
                    // (hide the window, drop the queue), and un-park on close.
                    if (mgpu::gpu1::presenter_owns_output())
                    {
                        mgpu::presenter::disp_set_panel(open);
                        char l2[260];
                        snprintf(l2, sizeof l2, "[MGPU][R294] overlay %s: the display layer %s so the game's "
                                 "overlay is the one on screen (%s)", open ? "opened" : "closed",
                                 open ? "parks (window hidden, queue dropped)" : "takes the screen back",
                                 runtime == g_rt_game.load(std::memory_order_relaxed) ? "game runtime" : "gpu1 runtime");
                        mgpu::diag::info(l2);
                        return false;
                    }
                    if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_GT_OVERLAY, "overlay-toggle", open ? "open (visual unroots)" : "close (visual roots)");   // R313
                    mgpu::gpu1::dcomp_set_visible(!open);
                    return false;
                }

                if (!open) return false;   // a close on the game's runtime we did not cause: nothing to do

                // The overlay key on the game's runtime: refuse it there (it would
                // be under the visual) and move it to the GPU 1 runtime.
                const bool want = !mgpu::live_panel::live();
                mgpu::live_panel::set_live(want);
                g_ov_source.store((int)source, std::memory_order_relaxed);
                g_ov_pend_bridge.store(want ? 1 : 0, std::memory_order_relaxed);
                mgpu::diag::info(want
                    ? "[MGPU][R294] panel open: refused on the game's runtime, opened on the GPU 1 runtime over "
                      "the neural frame. The game's input is blocked while the panel is drawn (R293)."
                    : "[MGPU][R294] panel close: the GPU 1 runtime's panel is closed; the game's input is released.");
                return true;    // block: ReShade leaves the overlay closed on the game's runtime
            }

            if (r_gpu1)
            {
                // The GPU 1 runtime's own state changed (ESC, or a close we did
                // not queue): keep ours in step.
                mgpu::live_panel::set_live(open);
                return false;
            }
            return false;
        }
        if (mgpu::strace::on()) mgpu::strace::ev(mgpu::strace::R_GT_OVERLAY, "overlay-toggle", open ? "open (visual unroots)" : "close (visual roots)");   // R313
        mgpu::gpu1::dcomp_set_visible(!open);
        return false;   // dcomp mode has no window to disagree with
    }

    // R158. See above. Gated to the one measured configuration.
    if (!mgpu::gpu1::dcomp_explicit_off_single_display()) return false;

    void *g = g_rt_game.load(std::memory_order_relaxed);
    void *b = g_rt_bridge.load(std::memory_order_relaxed);
    void *r = runtime;

    // Consume our own echo BEFORE deciding to mirror, or this open is
    // attributed to the person and bounced back to where it came from.
    if (r == g && g_ov_echo_game.exchange(false, std::memory_order_relaxed))   return false;
    if (r == b && g_ov_echo_bridge.exchange(false, std::memory_order_relaxed)) return false;

    // Both runtimes have to be known. Until the second one initialises this
    // does nothing, which is today's behaviour rather than half of the new one.
    if (g == nullptr || b == nullptr) return false;

    // [H7/E, 2026-11-11] THE GAME'S OVERLAY IS THE ONE THAT STEALS THE MOUSE.
    //
    // Measured in the 16:50 run: with both runtimes' overlays open (R158 mirrors them), the
    // GAME's overlay captured the mouse (wantCapture=1) and the bridge window - the one actually
    // on screen under the follow - stopped receiving mouse messages entirely ([R457] froze at
    // 167 msgs / 2 clicks for the whole session). The panel was clickable only while another
    // window held the foreground, because that is when the game releases capture.
    //
    // With DispFollow=1 the bridge window is the screen whenever the game is foreground, so the
    // game's overlay can never be seen - its only observable effect is that capture. Block it and
    // let the bridge runtime own the panel, exactly as the DcompOverlay=1 branch already does.
    // DispFollow absent/0 (the shipped arm) keeps R158's mirror untouched.
    if (mgpu::gpu1::ui_ini_read("DispFollow", 0) != 0)
    {
        if (r == g)
        {
            // [H7/G, 2026-11-11] TOGGLE, NOT MIRROR. Because this runtime's open is REFUSED,
            // ReShade never flips ITS overlay state - so the next Home press on this runtime is
            // another "open", and mirroring it verbatim sends "open" again: the panel could be
            // opened from the keyboard and never closed (measured 17:0x: Home did not close it,
            // only switching windows did, because that path uses the close request). Ask the
            // bridge for the NEGATION of what is actually on screen.
            const bool bridge_open = mgpu::gpu1::bridge_overlay_open();
            const bool want = open && !bridge_open;
            g_ov_source.store((int)source, std::memory_order_relaxed);
            g_ov_pend_bridge.store(want ? 1 : 0, std::memory_order_relaxed);
            // [H7/I, 2026-11-11] RELEASE THE GAME'S MOUSE CAPTURE WHEN THE PANEL OPENS.
            // Measured 18:17: the panel opened at 18:17:02, ReShade's feed reported
            // wantCapture=1 at 18:17:05, and the bridge window's click counter stayed at 2 -
            // the game's UE5 window holds SetCapture (mouse-look), so every mouse message goes
            // there and the overlay on the bridge window gets nothing until something makes
            // the game release it (the operator's workaround: switch windows for ten seconds
            // and come back). WM_CANCELMODE makes DefWindowProc release capture and cancel the
            // window's mode; ClipCursor(NULL) drops any clip the game installed. Both are what
            // a menu-open is supposed to do to a game, and neither touches the stream.
            if (want)
            {
                HWND game = (HWND)mgpu::gpu1::dcomp_game_hwnd();
                if (game != nullptr && IsWindow(game))
                    PostMessageW(game, WM_CANCELMODE, 0, 0);
                ClipCursor(nullptr);
                static std::atomic<int> said_release{0};
                if (said_release.fetch_add(1, std::memory_order_relaxed) < 3)
                    mgpu::diag::info("[MGPU][R294] panel open: WM_CANCELMODE + ClipCursor(NULL) sent to "
                                     "the game window to release its mouse capture/clip, so the overlay "
                                     "on the bridge window gets input now instead of after a focus round-trip.");
            }
            static std::atomic<bool> said_follow{false};
            if (open && !said_follow.exchange(true))
                mgpu::diag::info("[MGPU][R294] panel key owned by the bridge runtime: the game's open is "
                                 "refused (DispFollow=1 means the bridge window IS the screen) and the bridge is "
                                 "asked for the OPPOSITE of its current state. Mirroring 'open' verbatim was the "
                                 "R158 toggle trap: the game runtime never flips, so Home could open the panel "
                                 "and never close it.");
            return true;   // block: the game's runtime must never own the overlay in follow mode
        }
        if (r == b) return false;   // the visible one: ReShade handles it normally
    }

    g_ov_source.store((int)source, std::memory_order_relaxed);
    if      (r == g) g_ov_pend_bridge.store(open ? 1 : 0, std::memory_order_relaxed);
    else if (r == b) g_ov_pend_game.store(open ? 1 : 0, std::memory_order_relaxed);
    else             return false;   // neither: nothing to mirror to

    // R160. SAID ONCE, because R158 shipped with no logging at all and a whole
    // run came back as "it stopped working" with nothing in the file to say at
    // which link. One line proves the mirror was reached; the absence of it
    // now means the gate or the identity, not the apply.
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true, std::memory_order_relaxed))
            mgpu::diag::info("[MGPU][R160] overlay mirror is live - a keypress on one runtime is "
                             "now queued for the other. Said once. IF THE KEY STOPS MOVING BOTH "
                             "PANELS LATER IN THE RUN this line is still the last word on it: "
                             "the queue is refilled from the runtime's own frame, so the link "
                             "that can still fail is the apply, not the identity.");
    }

    return false;   // never block the overlay that was actually asked for
}

// D2.0: after the game's Present returns; anchor=finish sleeps here. Off
// (one load each) unless StallWatch / OwnReflex are set.
static void on_finish_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain)
{
    // [PORT B2] the hook needs _nvngx.dll resident, and the game loads it when
    // its own DLSS initialises - which can be after our one-shot. install() is
    // idempotent, returns immediately once live, and logs at most three times
    // while the module is absent, so this costs a GetModuleHandle per frame.
    if (mgpu::ngxhook::wanted() && !mgpu::ngxhook::installed())
        (void)mgpu::ngxhook::install(1);
    // [FFX] The FSR module loads when the title initialises its upscaler, which can be long
    // after this callback starts firing. Idempotent; logs at most three times while absent.
    if (mgpu::ffxhook::wanted() && !mgpu::ffxhook::installed())
        (void)mgpu::ffxhook::install(1);

    mgpu::stallwatch::scope sw(mgpu::stallwatch::S_FINISH_CB);
    if (swapchain != nullptr)
        mgpu::own_reflex::on_finish_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
}

static void on_present(reshade::api::command_queue *queue,
                       reshade::api::swapchain *swapchain,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *)
{
    // D2.0: the stall watch marks this callback and each step in it. Off: one load per scope.
    mgpu::stallwatch::scope sw_cb(mgpu::stallwatch::S_PRESENT_CB);
    if (swapchain != nullptr)
        mgpu::stallwatch::on_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    // R313: the game's present cadence - one relaxed load (key absent) or one
    // relaxed increment (key on) when this is the GAME's chain. Nothing else.
    if (swapchain != nullptr)
        mgpu::strace::note_game_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    // R353: a game can switch its chain to HDR after init (SetColorSpace1 at runtime); refresh the
    // recorded colour space from the game chain's presents. One getter + one relaxed store per present.
    if (swapchain != nullptr &&
        reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())) == g_r353_game_chain.load(std::memory_order_relaxed))
        r353_note_colour_space(swapchain);
    // DX11 probe: one load when off. The immediate context and the current
    // back buffer, as natives; the module holds no ReShade type.
    if (swapchain != nullptr && queue != nullptr && mgpu::dx11probe::on())
    {
        if (reshade::api::command_list *icl = queue->get_immediate_command_list())
            mgpu::dx11probe::on_present(
                reinterpret_cast<void *>(static_cast<uintptr_t>(icl->get_native())),
                reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_current_back_buffer().handle)));
    }
    // D1: the discovery map counts presents per chain. Off (one load) unless Discovery=1.
    if (swapchain != nullptr)
    {
        mgpu::stallwatch::scope sw(mgpu::stallwatch::S_DISCOVERY);
        mgpu::discovery::on_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    }
    // D1.5a: the latency probe samples on the game chain. Off (one load) unless LatencyProbe=1.
    if (swapchain != nullptr)
    {
        mgpu::stallwatch::scope sw(mgpu::stallwatch::S_PROBE);
        mgpu::latprobe::on_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    }
    // D2.0: our Reflex (anchor=present sleeps here, before the game's Present). Off (one load)
    // unless OwnReflex=1.
    if (swapchain != nullptr)
        mgpu::own_reflex::on_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    if (queue == nullptr) return;
    if (mgpu::t119::panel_stats()) t117_count_game_present(swapchain);   // T1.17 (PanelStats=1)
    if (mgpu::t119::frame_cap() != 0) mgpu_frame_cap_tick(swapchain);    // T1.1 (FrameCap=20..500)
    {
        mgpu::stallwatch::scope sw(mgpu::stallwatch::S_STREAM);
        mgpu::gpu1::stream_on_present(
            reinterpret_cast<void *>(static_cast<uintptr_t>(queue->get_native())));
    }

    // ---- R166: THE OVERLAY MIRROR APPLIES FROM PRESENT, NOT FROM EFFECTS ----
    //
    // MEASURED twice on The Blood of Dawnwalker. R158 put the apply in
    // reshade_finish_effects and R160 proved the queue was being filled - its
    // "mirror is live" line printed and the key still moved one panel. The
    // remaining link is the one R160's own text named, and the log says why in
    // a line that has been there all along:
    //
    //   [R53] TECHNIQUE LANE: BRIDGE runtime | 1 technique(s) enumerated
    //   0 of 1 techniques ENABLED
    //
    // reshade_finish_effects fires when ReShade FINISHES AN EFFECT PASS. A
    // runtime with no enabled technique has no pass to finish, so the bridge
    // runtime simply stops delivering that event - and with it the apply. This
    // is R138's rule for the third time in this file: DO NOT HANG WORK OFF THE
    // EFFECT EVENT UNLESS THE WORK IS ABOUT EFFECTS.
    //
    // present arrives whatever the effect runtime is doing, once per
    // swapchain, ON THE THREAD THAT PRESENTS IT - which is the game's render
    // thread for the game's chain and the bridge thread for the bridge's. So
    // this keeps the one property that made the deferral necessary (the call
    // lands on the runtime's own thread) and drops the dependency that broke
    // it. The runtime POINTER still comes from R160's re-latch and from
    // init_effect_runtime; only the moment of the call moved.
    if (swapchain != nullptr && (mgpu::gpu1::dcomp_explicit_off_single_display() ||
                                 (mgpu::live_panel::on() && mgpu::gpu1::dcomp_overlay_mode())))   // PanelOverPicture; key first - off never asks the mode here
    {
        reshade::api::device *pd = swapchain->get_device();
        if (pd != nullptr && pd->get_api() == reshade::api::device_api::d3d12)
        {
            mgpu::adapter::selection_result psel;
            mgpu::adapter::get_selection(psel);
            auto *pd12 = reinterpret_cast<ID3D12Device *>(pd->get_native());
            if (psel.game_luid_known && pd12 != nullptr)
            {
                const LUID pl = pd12->GetAdapterLuid();
                const bool p_is_game = (pl.LowPart == psel.game_luid.LowPart &&
                                        pl.HighPart == psel.game_luid.HighPart);

                // PanelOverPicture: the game's input is blocked while the panel is
                // drawn over the neural frame (R293 guard inside). This Present is
                // the game chain's, so the call is on that runtime's own thread.
                if (p_is_game)
                    mgpu::live_panel::block_game_input(
                        (reshade::api::effect_runtime *)g_rt_game.load(std::memory_order_relaxed));

                std::atomic<int>  &pend = p_is_game ? g_ov_pend_game : g_ov_pend_bridge;
                std::atomic<bool> &echo = p_is_game ? g_ov_echo_game : g_ov_echo_bridge;

                // PanelOverPicture: on the GPU 1 side the panel needs the runtime
                // whose swapchain IS the neural picture (no window), which is not
                // always R158's bridge slot. Off: R158's slot, as shipped.
                const bool live_mode = mgpu::live_panel::on() && mgpu::gpu1::dcomp_overlay_mode();
                void *bslot = g_rt_bridge.load(std::memory_order_relaxed);
                void *target = p_is_game ? g_rt_game.load(std::memory_order_relaxed)
                                         : (live_mode ? mgpu::live_panel::host_target(bslot) : bslot);

                auto *rt = (reshade::api::effect_runtime *)target;
                if (rt != nullptr)
                {
                    // [H7/E, 2026-11-11] The foreground follow asks for the panel to close when the
                    // game loses the foreground; here is where each runtime applies it, on its OWN
                    // thread. R158's rule: never call open_overlay across threads.
                    if (mgpu::gpu1::overlay_close_requested())
                        pend.store(0, std::memory_order_relaxed);
                    const int want = pend.exchange(-1, std::memory_order_relaxed);
                    if (want >= 0)
                    {
                        if (live_mode && !p_is_game)
                            mgpu::live_panel::note_opened_on((void *)rt, want != 0);
                        echo.store(true, std::memory_order_relaxed);
                        rt->open_overlay(want != 0,
                                         (reshade::api::input_source)g_ov_source.load(
                                             std::memory_order_relaxed));
                    }
                }
            }
        }
    }

    // R138. Log-only. Nothing below changes what the bridge does.
    if (swapchain == nullptr || g_r138_said.load(std::memory_order_relaxed)) return;
    if (g_game_fx_seen.load(std::memory_order_relaxed)) return;

    reshade::api::device *sd = swapchain->get_device();
    if (sd == nullptr || sd->get_api() != reshade::api::device_api::d3d12) return;

    mgpu::adapter::selection_result sel;
    mgpu::adapter::get_selection(sel);
    if (!sel.game_luid_known) return;

    auto *sd12 = reinterpret_cast<ID3D12Device *>(sd->get_native());
    if (sd12 == nullptr) return;
    const LUID l = sd12->GetAdapterLuid();
    if (l.LowPart != sel.game_luid.LowPart || l.HighPart != sel.game_luid.HighPart)
        return;

    const unsigned long long n =
        g_game_presents.fetch_add(1, std::memory_order_relaxed) + 1;

    // ---- R143: TELL THE SCREEN, NOT ONLY THE LOG ----
    //
    // Everything below this line writes to ReShade.log. R142 added an idle
    // screen for the missing tap so that the fault would be visible WITHOUT
    // the log - and then fed that screen from log_preset_once, which hangs off
    // reshade_finish_effects like every other depth diagnostic in this project
    // and dies with it. Removing the tap from a runtime with no other effects
    // silenced the enumeration, left the screen's tap state at "not determined
    // yet", and put the screen back on ARMING: the exact failure R142 was
    // written to end, reintroduced by R142's own plumbing.
    //
    // Reaching this line means the two early returns above have already
    // established both halves: the swapchain is the GAME's, and
    // reshade_finish_effects has never fired on it. 600 is R138's threshold
    // and AutoArm's, so the screen changes as the stream starts trying rather
    // than before there is anything to report.
    if (n >= 600) mgpu::gpu1::ui_set_game_fx_absent(true);

    // ---- R140: THE ARM-HELD REPORT, FROM A PATH THAT CANNOT GO SILENT ----
    //
    // THE DEFECT THIS FIXES IS THE LOG, NOT THE STREAM.
    //
    // Eleven runs of 007 First Light on one reporter's machine produced eleven
    // identical non-answers: no arm, and nothing saying why. The reason is
    // structural. EVERY depth diagnostic this project has - [P1.6], [R53],
    // [R63] stream arm HELD, the P9.1 frame tick, the band line - is emitted
    // from reshade_finish_effects. When the game's effect runtime never runs a
    // pass, that event never fires, and all of them go quiet together. The log
    // does not then say "depth is missing". It says NOTHING, which is
    // indistinguishable from "the add-on never loaded".
    //
    // An absence cannot be reported by the code the absence silences. So this
    // reports from on_present, which arrives whatever the effect runtime is
    // doing, and states the WHOLE chain every time rather than the one link
    // that happens to be reachable.
    //
    // Cadence, not spam: first at 600 presents (AutoArm's own threshold, so
    // the first report lands as the stream starts trying), then every 1800.
    // Stops the moment the stream is armed - a held report after the hold has
    // cleared is noise, and noise is what trains people to skip the line that
    // matters.
    if (n >= 600 && (n == 600 || (n % 1800) == 0))
    {
        mgpu::gpu1::ui_state ust{};
        mgpu::gpu1::ui_read(ust);
        if (!ust.armed)
        {
            const unsigned long long tap = mgpu::probe::depth_source();
            const unsigned long long mv  = mgpu::probe::mvec_source();
            mgpu::calibrator::table ct{};
            const bool have_tbl = mgpu::calibrator::read(ct);
            const bool fx = g_game_fx_seen.load(std::memory_order_relaxed);

            char h140[1900];
            snprintf(h140, sizeof h140,
                     "[MGPU][R140] ARM HELD at present %llu | game finish_effects seen: %s | "
                     "depth: tap=0x%llx, game's NGX table=0x%llx | mvec: probe source=0x%llx, "
                     "table=0x%llx. %s READ THE CHAIN LEFT TO RIGHT: a depth tap of 0 with "
                     "finish_effects NO means the game's ReShade runtime has never run an "
                     "effect pass, so mgpu_depth_tap.fx has never executed and nothing is "
                     "keeping a depth buffer alive - that is an install or runtime problem and "
                     "NOT the game. A depth tap of 0 with finish_effects YES means the pass "
                     "runs but ReShade has bound no depth, which is the [R63] case and usually "
                     "clears once a scene is on screen. A non-zero tap here means depth is "
                     "available and something else is holding the arm - read [R78] for the "
                     "velocity lane. This line is printed from the PRESENT path on purpose: "
                     "every other depth line in this log comes from the effect event, and when "
                     "that event is the thing that failed they all disappear together.",
                     n, fx ? "YES" : "NO",
                     tap, have_tbl ? ct.depth : 0ull,
                     mv, have_tbl ? ct.mvec : 0ull,
                     (tap == 0ull && !fx)
                         ? "ERROR 204 - THE GAME'S EFFECT RUNTIME HAS NEVER RUN, AND THAT IS "
                           "THE FAULT. Same code as the bridge window is showing. [R138] above "
                           "states it once with what to check, and "
                           "github.com/maohgad-web/Neural-coprocessor/issues if that does not "
                           "resolve it."
                         : ((tap == 0ull)
                                ? "The effect pass runs; ReShade has no depth bound yet."
                                : "Depth is available - the hold is elsewhere."));
            mgpu::diag::error(h140);
        }
    }

    if (n < 600) return;
    if (g_r138_said.exchange(true, std::memory_order_relaxed)) return;

    char l138[1500];
    snprintf(l138, sizeof l138,
             "[MGPU][R138] THE GAME'S EFFECT RUNTIME HAS NEVER RUN AN EFFECT PASS - %llu "
             "presents on the game's swapchain and not one reshade_finish_effects event. "
             "THIS IS WHY NOTHING ELSE IS IN THIS LOG. The preset probe, the frame tick and "
             "mgpu_depth_tap.fx all run from that event, so with Depth=1 the stream will "
             "wait forever for a depth buffer that nothing is keeping alive, and it will do "
             "it without an error because there is no failure - there is an absence. WHAT TO "
             "CHECK, in order: (1) does the [MGPU][P1.6] GAME line appear anywhere above - if "
             "only BRIDGE appears, the effects loaded on the wrong runtime; (2) the game's "
             "ReShade.ini [GENERAL] EffectSearchPaths, which must point at the folder holding "
             "mgpu_depth_tap.fx; (3) whether ReShade compiled the effects at all, and how "
             "many - a BRIDGE line reading '0 of N' with no GAME line means all N went to the "
             "bridge. Measured on 007 First Light, 2026-09-15: 14 effects compiled, all of "
             "them enumerated by the BRIDGE runtime, none by the game's. IF THAT DOES NOT "
             "RESOLVE IT: github.com/maohgad-web/Neural-coprocessor/issues - attach this file.",
             n);
    mgpu::diag::error(l138);
}

// [PORT B2.1, 2026-11-11] READ-ONLY descriptor probe for the depth-source
// comparison. Reads geometry/format from a handle whose lifetime is owned by
// someone else (ReShade's tap output, or the game's D3D12 depth), so it is
// SEH-wrapped: a handle that has just been released must cost one missing
// field in one log line, never a crash. It changes nothing.
static bool b21_desc(unsigned long long hv, unsigned &w, unsigned &h, unsigned &fmt)
{
    w = h = fmt = 0u;
    if (hv == 0ull) return false;
    __try
    {
        ID3D12Resource *r = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)hv);
        const D3D12_RESOURCE_DESC dd = r->GetDesc();
        w = (unsigned)dd.Width;
        h = (unsigned)dd.Height;
        fmt = (unsigned)dd.Format;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void on_reshade_finish_effects(reshade::api::effect_runtime *runtime,
                                      reshade::api::command_list *cmd_list,
                                      reshade::api::resource_view rtv,
                                      reshade::api::resource_view rtv_srgb)
{
    (void)rtv_srgb;
    if (runtime == nullptr || cmd_list == nullptr) return;

    reshade::api::device *dev = runtime->get_device();

    // R138. This event firing at all is the fact the R138 line reports the
    // absence of. Set unconditionally and cheaply; the game/bridge split below
    // refines it, but any finish_effects at all means the runtime is alive.
    g_game_fx_seen.store(true, std::memory_order_relaxed);

    // P12.2/R63. Hoisted, because the depth barrier below is only legal on the
    // GAME runtime's command list while the stream call sits outside the block
    // that knows which runtime this is. Issuing a transition for a GPU 0
    // resource on the bridge runtime's GPU 1 list is not a subtle error.
    bool rt_is_game = false;
    if (dev == nullptr) return;

    // 2026-10-06, Rise of the Tomb Raider in DX11 (DX11_CONTRACT_LEDGER.md
    // section 6): everything below hands D3D12 natives to the capture, the
    // stream and the probe. stream_on_finish_effects casts the queue native
    // to ID3D12CommandQueue and calls GetDevice on it once per process for
    // the Reflex residue clear; on a D3D11 runtime that native is an
    // ID3D11DeviceContext, whose same vtable slot is VSSetConstantBuffers.
    // That was the crash on every DX11 title at the first finish_effects,
    // whatever the ini. A non-D3D12 runtime has nothing for this handler -
    // except the R227 DX11 producer, which takes the D3D11 natives and feeds
    // the existing producer on its own D3D12 device (see dx11_producer.hpp).
    if (dev->get_api() != reshade::api::device_api::d3d12)
    {
        if (dev->get_api() == reshade::api::device_api::d3d11 && mgpu::dx11producer::on())
        {
            // The depth tap, resolved the same way as on D3D12 (R63): the
            // probe reads MGPU_DepthOutTex through ReShade's API, which is
            // API-neutral, and publishes the native handle - an
            // ID3D11Texture2D here.
            mgpu::probe::note_effects(runtime, cmd_list);
            // R238: the calibrator's frame counter is what hook_evaluate11's
            // once-per-frame copy filter compares against; the D3D12 branch
            // advances it below and this branch never reached that line, so
            // every evaluate after the first was skipped (TR-14: vectors=1).
            mgpu::calibrator::note_frame();
            mgpu::mvec_census::note_frame();   // R253
            const unsigned long long depth11 = mgpu::probe::depth_source();
            mgpu::gpu1::ui_set_tap_state(mgpu::probe::tap_state());
            mgpu::gpu1::ui_set_game_fx_absent(false);
            const reshade::api::resource res11 = dev->get_resource_from_view(rtv);
            reshade::api::command_queue *q11 = runtime->get_command_queue();
            reshade::api::command_list *icl = (q11 != nullptr) ? q11->get_immediate_command_list() : nullptr;
            // R254: the tap. The census's candidate for THIS frame goes to the
            // producer through the same entry the calibrator feeds on a
            // contract title; a frame with no bound target feeds nothing and
            // the producer's absent/hold logic reads it as "no vectors".
            // R278d: the learning turned the tap on at arm (D3D11: one atomic load per frame when off).
            if (!mgpu::mvec_census::tap_on() && mgpu::gpu1::mvec_tap_live() != 0) mgpu::mvec_census::tap_enable_live();
            if (icl != nullptr && mgpu::mvec_census::tap_on())
            {
                unsigned tw = 0, th = 0, tf = 0; unsigned long long fb = 0, ft = 0;
                const unsigned long long tap = mgpu::mvec_census::tap_candidate(&tw, &th, &tf, &fb, &ft);
                static unsigned long long s_tap_said = 0;
                if (tap != 0)
                {
                    if (s_tap_said != tap)
                    {
                        s_tap_said = tap;
                        char tl[300];
                        snprintf(tl, sizeof tl,
                                 "[MGPU][R254] MVEC TAP: feeding the game's target 0x%016llX %ux%u (format %u) to the "
                                 "producer - bound in %llu of %llu frames so far. Vectors from here on are the game's own.",
                                 tap, tw, th, tf, fb, ft);
                        mgpu::diag::info(tl);
                    }
                    mgpu::dx11producer::on_mvec(
                        reinterpret_cast<void *>(static_cast<uintptr_t>(icl->get_native())),
                        reinterpret_cast<void *>(static_cast<uintptr_t>(tap)));
                }
            }
            if (res11.handle != 0 && icl != nullptr)
                mgpu::dx11producer::on_finish_effects(
                    reinterpret_cast<void *>(static_cast<uintptr_t>(icl->get_native())),
                    reinterpret_cast<void *>(static_cast<uintptr_t>(res11.handle)),
                    reinterpret_cast<void *>(static_cast<uintptr_t>(depth11)));
        }
        return;
    }

    // P1.6. Which runtime is this? The same LUID comparison on_destroy_device
    // already makes. Log-only and one-shot per side; nothing below changes
    // behaviour, and the adapter filtering that actually gates the capture and
    // stream paths still happens inside gpu1_context where the game's LUID
    // lives. Placed here rather than at stream arm on purpose: the preset is
    // painting frames from the moment the runtime exists, which is long before
    // anything is armed, so the record has to start there too.
    {
        static preset_probe game_probe, bridge_probe;
        mgpu::adapter::selection_result sel;
        mgpu::adapter::get_selection(sel);
        if (sel.game_luid_known && dev->get_api() == reshade::api::device_api::d3d12)
        {
            if (auto *dev12 = reinterpret_cast<ID3D12Device *>(dev->get_native()))
            {
                const LUID luid = dev12->GetAdapterLuid();
                const bool is_game = (luid.LowPart == sel.game_luid.LowPart &&
                                      luid.HighPart == sel.game_luid.HighPart);
                log_preset_once(runtime, is_game ? "GAME" : "BRIDGE",
                                is_game ? game_probe : bridge_probe);
                // P9.1. The probe's frame tick and its periodic dump. Placed
                // here because this is the one spot that already knows which
                // runtime it is on, and the probe counts the GAME's frames -
                // the bridge runtime presents on its own schedule and counting
                // both would make the ns/frame figure meaningless.
                // P11.0 (R48). note_effects FIRST: it only stores the
                // runtime pointer, and dump() - which note_frame may call on
                // this very line - is what enumerates through it. Reversed,
                // the first dump of the process would have no runtime and the
                // semantic lane would report NOT RUN for one window.
                if (is_game)
                {
                    rt_is_game = true;
                    mgpu::probe::note_effects(runtime, cmd_list);
                    mgpu::probe::note_frame();
                    // R101. Same tick, same runtime, same reason: the tap's
                    // ns/frame figure has to be against the GAME's frames.
                    mgpu::calibrator::note_frame();

                    // ---- R103: HAND THE TRANSPORT THE GAME'S OWN HANDLE ----
                    //
                    // Only when the calibrator actually has one. read()
                    // returns false until something has been captured, and
                    // KEY_MVEC is only set when the Get succeeded, so a title
                    // where the calibrator never resolved falls through to the
                    // barrier probe's pick with no branch of its own.
                    //
                    // Calib=0 is the off switch: no calibrator, no table, no
                    // override, and this build behaves exactly like R99.
                    {
                        // R300c (Dom's R220; SLTagsMvec=1 only, needs SLTags). Streamline's own ResourceTag
                        // declaration outranks the calibrator table and the lateral ranking when SLT1 has
                        // one. This changes RESOURCE IDENTITY only; the barrier / R118 triggers still decide
                        // WHEN a copy is made. Key absent, or no tag seen yet: the live R103 path below.
                        const unsigned long long tagged_mv =
                            mgpu::t119::sltags_mvec() ? mgpu::sltags::mvec_handle() : 0ull;
                        if (tagged_mv != 0ull)
                        {
                            mgpu::probe::set_mvec_override(tagged_mv);
                            static unsigned long long r308_said = 0ull;
                            if (r308_said != tagged_mv)
                            {
                                r308_said = tagged_mv;
                                char r308[512];
                                snprintf(r308, sizeof r308,
                                    "[MGPU][R300c] TAGGED MVEC SOURCE ACTIVE: Streamline declared 0x%llx; this "
                                    "handle now overrides the calibrator / ranked source selection "
                                    "(SLTagsMvec=1). Copy timing is unchanged (barrier / R118).",
                                    tagged_mv);
                                mgpu::diag::info(r308);
                            }
                        }
                        else
                        {
                            mgpu::calibrator::table ct;
                            if (mgpu::calibrator::read(ct) &&
                                (ct.have & mgpu::calibrator::KEY_MVEC) != 0u)
                                mgpu::probe::set_mvec_override(ct.mvec);
                        }
                    }
                    // R315 (MVecBarrierSource=1 only): the probe's fire count to gpu1 for its [R315] line.
                    if (mgpu::probe::mvec_barrier_source() != 0)
                        mgpu::gpu1::r315_note(mgpu::probe::mvec_barrier_source(), mgpu::probe::mvec_barrier_source_fires());
                    // ---- R312 (DepthTransport >= 2): push the tap's geometry, once per launch ----
                    //
                    // The tap's .fx reads MGPU_DEPTH_FORMAT / _WIDTH / _HEIGHT with defaults equal to
                    // today's (R32F at BUFFER size). 2: the format alone, pushed on the first game frame.
                    // 3 / 4: the render size, pushed once the calibrator's table carries it (the game's
                    // own DLSS has run). ReShade reloads the effect once; the arm holds for depth until
                    // the tap's resource is the pushed geometry (gpu1_context, r312_set_target).
                    // Key absent (1): this block is three loads and nothing else.
                    {
                        static bool r312_done = false;
                        const int r312 = r312_done ? 1 : mgpu::gpu1::depth_transport();
                        // R342a (E8): the pushed definitions persist in ReShadePreset.ini across launches, so
                        // a launch with DepthTransport absent / 1 after a run at 2-4 kept the tap at the OLD
                        // geometry while this add-on expected the full frame. Once per launch: if any
                        // MGPU_DEPTH_* definition is present, push the .fx defaults back (R32F, BUFFER size)
                        // - one reload, as R312's own push is. Nothing present: nothing pushed.
                        static bool r342_checked = false;
                        if (!r342_checked && !r312_done && r312 == 1)   // the KEY value: r312 reads 1 only before any push here
                        {
                            r342_checked = true;
                            // R342a2 (finding #1, suite run A-K1): the push-back itself leaves the three definitions in
                            // ReShadePreset.ini AT their default values, so a presence test fired and reloaded the effect
                            // on every later launch. Push only when a definition is present AND differs from its default.
                            auto stale = [&](const char *name, const char *def) -> bool
                            {
                                char cur[64]; size_t cn = sizeof cur;
                                if (!runtime->get_preprocessor_definition_for_effect("mgpu_depth_tap.fx", name, cur, &cn)) return false;
                                cur[sizeof cur - 1] = 0;
                                return strcmp(cur, def) != 0;
                            };
                            const bool any = stale("MGPU_DEPTH_WIDTH", "BUFFER_WIDTH") ||
                                             stale("MGPU_DEPTH_HEIGHT", "BUFFER_HEIGHT") ||
                                             stale("MGPU_DEPTH_FORMAT", "R32F");
                            if (any)
                            {
                                runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_FORMAT", "R32F");
                                runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_WIDTH", "BUFFER_WIDTH");
                                runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_HEIGHT", "BUFFER_HEIGHT");
                                mgpu::diag::info("[MGPU][R342] DEPTH LANE: DepthTransport is absent / 1 but mgpu_depth_tap.fx still carried MGPU_DEPTH_* "
                                                 "definitions from an earlier launch at 2-4 - the defaults (R32F at the buffer size) are pushed back; "
                                                 "the effect reloads once (E8).");
                            }
                        }
                        if (r312 >= 2)
                        {
                            const bool want_size = (r312 >= 3);
                            const bool want_half = (r312 == 2 || r312 == 4);
                            unsigned rw = 0, rh = 0;
                            bool ready = !want_size;
                            if (want_size)
                            {
                                mgpu::calibrator::table rt_t;
                                if (mgpu::calibrator::read(rt_t) && rt_t.render_w != 0u && rt_t.render_h != 0u)
                                { rw = rt_t.render_w; rh = rt_t.render_h; ready = true; }
                                else if (mgpu::calibrator::read(rt_t) && rt_t.sub_w != 0u && rt_t.sub_h != 0u)
                                { rw = rt_t.sub_w; rh = rt_t.sub_h; ready = true; }
                                // R327 (E13): KCD2 fills Width/Height and OutWidth/OutHeight the other way round
                                // (2560x1440 / 1485x835). A render size LARGER than the output is not a thing an
                                // upscaler means, so in that one case the vector subrect (what the game rendered
                                // into) is the render size. Every other table (equal, below, dynamic resolution,
                                // DLAA, FG) reads exactly as before.
                                if (ready && rt_t.display_w != 0u && rt_t.display_h != 0u &&
                                    rw > rt_t.display_w && rh > rt_t.display_h &&
                                    rt_t.sub_w != 0u && rt_t.sub_h != 0u && rt_t.sub_w <= rt_t.display_w && rt_t.sub_h <= rt_t.display_h)
                                {
                                    char sw[220];
                                    snprintf(sw, sizeof sw, "[MGPU][R327] the table's render size %ux%u is larger than its output %ux%u (swapped fields): "
                                                            "the vector subrect %ux%u is taken as the render size.", rw, rh, rt_t.display_w, rt_t.display_h, rt_t.sub_w, rt_t.sub_h);
                                    mgpu::diag::info(sw);
                                    rw = rt_t.sub_w; rh = rt_t.sub_h;
                                }
                            }
                            if (ready)
                            {
                                r312_done = true;
                                char wv[16], hv[16];
                                if (want_half) runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_FORMAT", "R16F");
                                if (want_size)
                                {
                                    snprintf(wv, sizeof wv, "%u", rw); snprintf(hv, sizeof hv, "%u", rh);
                                    runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_WIDTH", wv);
                                    runtime->set_preprocessor_definition_for_effect("mgpu_depth_tap.fx", "MGPU_DEPTH_HEIGHT", hv);
                                }
                                mgpu::gpu1::r312_set_target(want_size ? rw : 0u, want_size ? rh : 0u,
                                                            want_half ? (unsigned)DXGI_FORMAT_R16_FLOAT : 0u);
                                char r312l[360];
                                snprintf(r312l, sizeof r312l,
                                         "[MGPU][R312] DEPTH LANE: DepthTransport=%d - pushed %s%s%s into mgpu_depth_tap.fx (the effect "
                                         "reloads once). The arm holds for depth until the tap's resource matches.",
                                         r312, want_half ? "MGPU_DEPTH_FORMAT=R16F" : "",
                                         (want_half && want_size) ? ", " : "",
                                         want_size ? "the render size (MGPU_DEPTH_WIDTH/HEIGHT from the game's own DLSS table)" : "");
                                mgpu::diag::info(r312l);
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- R160: RE-LATCH THIS RUNTIME'S IDENTITY, EVERY FRAME ----
    //
    // MEASURED, The Blood of Dawnwalker 2026-09-17: the global overlay key
    // worked repeatedly BEFORE the stream armed and never again after. The log
    // says why in one line -
    //
    //   [P4.0] stream REQUESTED
    //   Recreated runtime environment on runtime ...22E0 ('ReShade2.ini')
    //   [P5.0] the bridge window is now showing the NEURAL OUTPUT
    //
    // - the bridge's effect runtime is RECREATED at arm. R158 got its runtime
    // pointers from init_effect_runtime and cleared them on
    // destroy_effect_runtime, so the destroy half fired, the pointer went
    // null, and the mirror hit its "both runtimes must be known" guard for the
    // rest of the session. Silently, because nothing logged it.
    //
    // THE LESSON IS THE ONE THIS FILE KEEPS RELEARNING: do not hold state that
    // depends on a lifecycle event firing in pairs. This handler already knows
    // which runtime it is on, it runs every frame, and a runtime that is
    // running effects is a runtime that is alive - so identity is re-asserted
    // here from a fact rather than remembered from an event. A destroy and
    // recreate now repairs itself on the next frame the runtime draws, whether
    // or not init_effect_runtime fires. init_effect_runtime is KEPT because it
    // is the only thing that names a runtime which never runs effects at all
    // (R138's case); it is now the optimisation and this is the source of
    // truth, which is the correct way round.
    {
        std::atomic<void *> &slot = rt_is_game ? g_rt_game : g_rt_bridge;
        if (slot.load(std::memory_order_relaxed) != (void *)runtime)
            slot.store(runtime, std::memory_order_relaxed);
    }

    // R166. THE APPLY THAT USED TO BE HERE HAS MOVED TO on_present, and this
    // note is so nobody moves it back. This event does not arrive on a runtime
    // with no enabled technique, which is exactly the bridge runtime's normal
    // state - measured as "0 of 1 techniques ENABLED" on every title so far.
    // The identity re-latch above stays here, because a runtime running
    // effects is the only cheap PROOF that a pointer is live.

    // The resource behind the view, not the view: the copy source has to be
    // the texture. Adapter filtering happens inside gpu1_context, which is
    // where the game's LUID already lives.
    const reshade::api::resource res = dev->get_resource_from_view(rtv);
    if (res.handle == 0) return;

    // P2.0: the game's immediate queue. effect_runtime declares
    // `virtual command_queue *get_command_queue() = 0;` - it is the queue
    // ReShade itself submits the list above on, which is exactly the queue our
    // copies will be executed on, and therefore the only queue a signal placed
    // behind them can be ordered against. A null queue is not fatal: the
    // capture path falls back to the P1.5 frame counter and says so.
    reshade::api::command_queue *q = runtime->get_command_queue();
    void *q_native = (q != nullptr)
                       ? reinterpret_cast<void *>(static_cast<uintptr_t>(q->get_native()))
                       : nullptr;

    mgpu::gpu1::capture_on_finish_effects(
        reinterpret_cast<void *>(runtime),
        reinterpret_cast<void *>(static_cast<uintptr_t>(cmd_list->get_native())),
        q_native,
        static_cast<unsigned long long>(res.handle));

    // P4.0 rides the same event. Both paths are inert until requested and are
    // independent by construction, so neither can leave the other half-armed.
    //
    // ---- R63: THE DEPTH TRANSITION IS ISSUED HERE, NOT IN gpu1_context ----
    //
    // Two reasons, and the second is the real one.
    //
    // 1. gpu1_context holds no ReShade types, by a design rule older than this
    //    milestone, and ReShade's barrier is a ReShade type.
    // 2. R56 proved that RESHADE'S OWN MAPPING of shader_resource is the
    //    correct transition for a resource ReShade owns - 1080 copies, no
    //    device loss, on a rig with no debug layer to catch a wrong one.
    //    P10.3 already spent a device guessing a raw D3D12 state for a
    //    resource this project did not create. This does not guess.
    //
    // The colour copy inside stream_on_finish_effects transitions the game's
    // own back buffer and stays raw D3D12. The two are different cases and are
    // deliberately not written the same way.
    //
    // Barrier, record, barrier back - all on one list, in order. A handle of 0
    // means ReShade has no depth this frame, and then nothing at all is issued.
    unsigned long long depth_h = rt_is_game ? mgpu::probe::depth_source() : 0ull;

    // R145. The idle screen's copy of the tap state, refreshed from the value
    // depth_source() has just maintained. GAME runtime only - the bridge
    // runtime has no depth to tap and its tap state is not a fault. An int
    // read and a relaxed store; tech_scan's own cost is throttled to one
    // enumeration every 300 frames and is unchanged by this.
    if (rt_is_game) mgpu::gpu1::ui_set_tap_state(mgpu::probe::tap_state());

    // R149. This handler running AT ALL, on the game's runtime, is proof that
    // the game's effect runtime runs effects - which is the exact claim
    // ui_set_game_fx_absent makes. Clear it here rather than trusting the
    // 600-present threshold to have been right: a slow first compile can cross
    // that threshold on a run that is about to work perfectly.
    if (rt_is_game) mgpu::gpu1::ui_set_game_fx_absent(false);

    // ---- R139: THE ENGINE'S OWN DEPTH, BESIDE THE ONE WE USE ----
    //
    // Depth today comes from ONE route and it is the fragile one: an effect's
    // output texture, MGPU_DepthOutTex, which exists only if the game's
    // ReShade runtime loaded effects, compiled the tap, enabled its technique,
    // raised finish_effects, and had ReShade's own generic-depth heuristic
    // pick something sensible to feed it. Six links we do not control, and the
    // issue 15 reporter breaks at the first one - see R138.
    //
    // The engine, meanwhile, HANDS NGX ITS DEPTH BY POINTER on every DLSS
    // evaluate, and the calibrator has been capturing it all along: the
    // [R101] line prints it as "THE GAME'S OWN TABLE: ... depth=0x...". That
    // is a declaration rather than a guess, and it is the same shape as the
    // R103 route that solved the velocity address.
    //
    // THIS LINE COPIES NOTHING AND CHANGES NOTHING. It exists to answer the
    // one question the transport work depends on: IS THE ENGINE'S DECLARED
    // DEPTH THE SAME RESOURCE THE TAP HAS BEEN GIVING US?
    //
    //   SAME  - an evaluate-time route can reuse the depth slot exactly as the
    //           arm already sized it, and lands outside the ring entirely.
    //   NOT   - the slot is sized at arm from the tap's extent and format, so
    //           the size guard in the copy path would reject every frame, and
    //           making the route work means changing what the ARM sizes from.
    //           That is ring work and it is not a hotfix.
    //
    // Said once, and again whenever either side changes, so a resolution
    // change or a re-resolve is visible rather than averaged away.
    if (rt_is_game)
    {
        static unsigned long long said_tap = ~0ull, said_tbl = ~0ull;
        mgpu::calibrator::table ct{};
        const bool have_tbl = mgpu::calibrator::read(ct);
        const unsigned long long tbl = have_tbl ? ct.depth : 0ull;
        const bool changed = (tbl != said_tbl || depth_h != said_tap);

        // ---- R188: RATE LIMIT. MEASURED 2026-09-20 ----
        //
        // "Said once, and again whenever either side changes" was written for a
        // title that changes it rarely. Starfield changes the declared depth
        // pointer about 27 TIMES A SECOND: one 11-minute capture is 17,703
        // R139 lines out of 20,699 - 86% of the log is this one line. On a 4K
        // rig with more transitions it is worse, and a log a tester cannot
        // read is a log that does not come back.
        //
        // The full line still fires for the FIRST FEW changes, because the
        // first ones are the informative ones - they say whether the tap and
        // the engine's declared depth are the same resource at all. After that
        // it collapses to a count, carried on the periodic line below, which
        // is the same information at 1% of the volume.
        //
        // NOT a filter on the CONDITION - the comparison is unchanged and
        // every change is still counted. Only the printing is limited.
        static unsigned long long r139_changes = 0ull;
        static unsigned long long r139_said = 0ull;
        static unsigned long long r139_last_report_ms = 0ull;
        const unsigned long long R139_FULL_LINES = 6ull;
        const unsigned long long R139_REPORT_MS  = 30000ull;

        if (changed) ++r139_changes;

        if (changed && r139_said >= R139_FULL_LINES)
        {
            said_tbl = tbl; said_tap = depth_h;
            const unsigned long long now139 = GetTickCount64();
            if (r139_last_report_ms == 0ull) r139_last_report_ms = now139;
            else if (now139 - r139_last_report_ms >= R139_REPORT_MS)
            {
                r139_last_report_ms = now139;
                char p139[700];
                snprintf(p139, sizeof p139,
                         "[MGPU][R188] DEPTH SOURCE CHURN: the declared depth pointer has changed "
                         "%llu time(s) this run - currently tap 0x%llx | table 0x%llx | SAME: %s. "
                         "The full [R139] line is printed for the first %llu changes and then "
                         "collapsed to this one, because on a title that rotates depth buffers "
                         "every frame it was 86%% of the log. THE COUNT IS COMPLETE; only the "
                         "printing is limited. A count in the thousands is NORMAL on such a "
                         "title and is not a fault - what matters is the SAME answer above.",
                         r139_changes, depth_h, tbl,
                         (have_tbl && tbl != 0ull && tbl == depth_h) ? "YES" : "NO",
                         R139_FULL_LINES);
                mgpu::diag::info(p139);
            }
        }
        else if (changed)
        {
            ++r139_said;
            said_tbl = tbl; said_tap = depth_h;
            char d139[1400];
            snprintf(d139, sizeof d139,
                     "[MGPU][R139] DEPTH SOURCES: tap MGPU_DepthOutTex -> 0x%llx | game's own "
                     "NGX table depth -> 0x%llx | SAME RESOURCE: %s. WHY THIS LINE EXISTS: the "
                     "tap is the only route depth has, and it needs the game's ReShade runtime "
                     "to have loaded effects at all - which is exactly what fails on some "
                     "installs (see R138). The engine declares its depth to NGX on every "
                     "evaluate and the calibrator already reads it. SAME means an evaluate-time "
                     "depth route can reuse the slot as the arm already sized it. DIFFERENT "
                     "means the arm would have to be sized from the table instead, which is "
                     "transport work rather than a fallback. A table depth of 0 means this "
                     "title created no DLSS feature this run, so there is no declaration to "
                     "read and the tap is the only route available here.",
                     depth_h, tbl,
                     (tbl != 0ull && tbl == depth_h) ? "yes"
                         : (tbl == 0ull ? "no table" : "NO"));
            mgpu::diag::info(d139);
            // [PORT B2.1, 2026-11-11] the two numbers [R139] cannot show: EXTENT and FORMAT.
            // Same cadence as the full line (the first R139_FULL_LINES changes), read-only:
            // both descs are read and printed, nothing is transitioned, copied or bound.
            // This is the evidence that decides whether a contract-depth delivery (B2.2) can
            // reuse the depth slot the arm already sized, or has to change what the arm sizes from.
            {
                unsigned aw = 0, ah = 0, af = 0, bw = 0, bh = 0, bf = 0;
                const bool aok = b21_desc(depth_h, aw, ah, af);
                const bool bok = b21_desc(tbl, bw, bh, bf);
                char l805[520];
                snprintf(l805, sizeof l805,
                         "[MGPU][R805] DEPTH COMPARE: tap 0x%llx %ux%u fmt=%u (ok=%d) | contract 0x%llx %ux%u fmt=%u (ok=%d) "
                         "| same_extent=%s same_fmt=%s | contract depth_inverted=%u | transport=%d | READ-ONLY, changes nothing",
                         depth_h, aw, ah, af, aok ? 1 : 0,
                         tbl, bw, bh, bf, bok ? 1 : 0,
                         (aok && bok) ? ((aw == bw && ah == bh) ? "YES" : "NO") : "?",
                         (aok && bok) ? ((af == bf) ? "YES" : "NO") : "?",
                         ct.depth_inverted, mgpu::gpu1::depth_transport());
                mgpu::diag::info(l805);
            // [PORT O1+O4, 2026-11-11] WOULD A CONTRACT-DEPTH COPY FIT, AND IS THE TAP
            // ALREADY READING THE SAME RESOURCE? Read-only on both counts.
            //   O1: the copy box must use the extent the game DECLARED (PBZ: 1256
            //       allocated, 1253 declared) and be format-compatible by bytes per
            //       texel (D32 and R32 are both 4).
            //   O4: tap_INPUT is the resource ReShade binds to the tap DEPTH semantic.
            //       Equal to the contract depth means the tap already samples the
            //       game own depth; different means a contract route changes CONTENT.
            if (tbl != 0ull)
            {
                unsigned tw = 0, th = 0, tf = 0, cw = 0, ch = 0, cf = 0;
                const bool tapok = b21_desc(depth_h, tw, th, tf);
                const bool cok = b21_desc(tbl, cw, ch, cf);
                const unsigned decw = (ct.depth_x != 0u) ? ct.depth_x : ct.render_w;
                const unsigned dech = (ct.depth_y != 0u) ? ct.depth_y : ct.render_h;
                auto bpt1 = [](unsigned f) -> unsigned
                {
                    switch (f) { case 19u: case 20u: case 39u: case 40u: case 41u: return 4u;
                                 case 54u: case 55u: case 56u: return 2u; default: return 0u; }
                };
                const unsigned bsrc = bpt1(cf), blane = bpt1(tf);
                const bool fit = tapok && cok && decw >= 64u && dech >= 64u && decw <= tw && dech <= th;
                const bool bpt_ok = (bsrc != 0u && bsrc == blane);
                const unsigned long long tap_in = mgpu::probe::depth_input_source();
                const bool same_in = (tap_in != 0ull && tap_in == tbl);
                char l807[560];
                snprintf(l807, sizeof l807,
                         "[MGPU][R807] DEPTH ROUTE CHECK: contract 0x%llx alloc=%ux%u declared=%ux%u fmt=%u bpt=%u | "
                         "lane(tap out) 0x%llx %ux%u fmt=%u bpt=%u | fits=%d bpt_ok=%d would_copy=%d | "
                         "tap_INPUT=0x%llx SAME_AS_CONTRACT=%s | verdict=%s",
                         tbl, cw, ch, decw, dech, cf, bsrc,
                         depth_h, tw, th, tf, blane,
                         fit ? 1 : 0, bpt_ok ? 1 : 0, (fit && bpt_ok) ? 1 : 0,
                         tap_in, same_in ? "YES" : (tap_in == 0ull ? "?" : "NO"),
                         same_in ? "tap already samples the game depth; a contract route buys the resample only"
                                 : (tap_in == 0ull ? "tap input unknown (no depth bound this frame)"
                                                   : "tap samples a DIFFERENT resource; a contract route changes content"));
                mgpu::diag::info(l807);
            }
            }
        }
    }

    const reshade::api::resource depth_res = { depth_h };

    if (depth_h != 0)
        cmd_list->barrier(depth_res, reshade::api::resource_usage::shader_resource,
                                     reshade::api::resource_usage::copy_source);

    // ---- R78: the velocity buffer, for the ARM ONLY ----
    //
    // No barrier and no copy here, and that is the difference from depth. By
    // the time this event fires the velocity target has been bound as a render
    // target again and holds whatever the engine last drew into it - the frame
    // it described is gone. Its per-frame copy is issued mid-frame from
    // mgpu_mvec_transport_hook above; all this handle does is let the arm size
    // the slot's MVec region from the real resource. Zero until the probe has
    // published one, which holds the arm - see stream_on_finish_effects.
    // R300c (SLTagsMvec=1 only): size the slot's MVec region from the Streamline-declared resource once
    // SLT1 has seen it. Key absent: the probe / calibrator source exactly as live.
    const unsigned long long tagged_mvec_h =
        (rt_is_game && mgpu::t119::sltags_mvec()) ? mgpu::sltags::mvec_handle() : 0ull;
    const unsigned long long mvec_h = rt_is_game
        ? (tagged_mvec_h != 0ull ? tagged_mvec_h : mgpu::probe::mvec_source())
        : 0ull;

    // R277: the D3D12 tap, off unless MVecTap=1/2 (read once at the game chain's
    // init). Copies the census candidate here when no barrier delivered it this
    // frame. See probe.cpp tap_finish_effects for the state assumption.
    // R278b: the key, or the mode the first-launch learning turned on at arm.
    // R280: the virtual velocity target's extraction for this frame (D3D12, game
    // runtime only; a no-op unless the module armed at the chain's init). BEFORE
    // the tap: the target must hold this frame's x,y, in shader_resource, when
    // the tap copies it.
    if (rt_is_game && dev->get_api() == reshade::api::device_api::d3d12)
        mgpu::mvec_extract::on_finish_effects(cmd_list, q);
    int tap_mode = (g_mvec_tap12 != 0) ? g_mvec_tap12 : mgpu::gpu1::mvec_tap_live();
    // R280o (Fable's ladder; SF-R1 vs SF-R2): the tap is the last rung. Once the evaluate
    // route has delivered (R118: the calibrator copies the vectors the game hands DLSS, at
    // the DLSS pass), the tap yields for the rest of the session - one route at a time
    // (R106b), and no end-of-frame copy with an assumed state on top of the right one.
    // Latched rather than per frame: the evaluate route is absent in menus, and a tap that
    // came back there would copy at the wrong moment and flip the source in and out.
    // A title whose evaluate route never delivers (RE4, no table) taps exactly as before.
    // R289 (0.3.1): the same ladder with the game's own route as a rung too - on a
    // D3D12 title with an NGX contract, copies not made by the tap that landed are the
    // title's own vectors arriving. Latched like R280o. Titles without a contract
    // (no DLSS, non-NVIDIA game card) keep the tap exactly as in 0.3.0.
    const int r289_route = (rt_is_game && tap_mode != 0 && !g_r280o_eval_yield) ? mgpu::gpu1::own_route_delivered() : 0;
    if (rt_is_game && tap_mode != 0 && (g_r280o_eval_yield || r289_route != 0))
    {
        if (!g_r280o_eval_yield)
        {
            g_r280o_eval_yield = true;
            if (r289_route == 1)
                mgpu::diag::info("[MGPU][R280o] the evaluate route has delivered this title's own vectors: the R277 tap "
                                 "yields to it for the rest of this session (one route at a time, R106b).");
            else
                mgpu::diag::info("[MGPU][R289] the game's NGX contract is seen and its own route (not the tap) has delivered "
                                 "this title's vectors: the R277 tap yields to it for the rest of this session (one route "
                                 "at a time, R106b).");
        }
        tap_mode = 0;
    }
    if (rt_is_game && tap_mode != 0)
    {
        mgpu::gpu1::tap_copy_scope(true);    // R280o: copies made inside this call are the tap's, counted apart
        mgpu::probe::tap_finish_effects(cmd_list, tap_mode);
        mgpu::gpu1::tap_copy_scope(false);   // R280o
        mgpu::gpu1::set_mvec_eligible_count(mgpu::probe::mvec_eligible_count());   // R278
        // R278f: the probe's neighbours to gpu1, and gpu1's live switch to the probe.
        mgpu::gpu1::set_mvec_neighbours(mgpu::probe::mvec_pick_index(), mgpu::probe::mvec_next_same(), mgpu::probe::mvec_next_class());
        mgpu::probe::mvec_pick_lock(mgpu::gpu1::stream_producing());   // R278f-r
        const int req = mgpu::gpu1::mvec_candidate_request();
        if (req >= 0 && req != mgpu::probe::mvec_pick_index()) mgpu::probe::mvec_pick_live(req);
    }

    // R315b (MVecBarrierSource=2 only): the tail copy of the published vector buffer, before the
    // seal. WINDOW: beside the transition copy, for the judge. TAIL: the delivery; the probe's
    // transition match is told to stop. TRANSITION: nothing. Key absent / 1: two loads.
    // R315b fix 2 / R323: did the game's own DLSS evaluate for this frame? The calibrator's evaluate
    // count moved since the last game frame. Read once here, for the tail copy (MVecBarrierSource=2)
    // and for the history gate (HistoryGate=1); -1 while the title has no table. Neither key: no read.
    const bool r315_two = rt_is_game && mgpu::probe::mvec_barrier_source() == 2 && mgpu::gpu1::stream_producing();
    const bool gate_on  = rt_is_game && mgpu::gpu1::history_gate() != 0;
    bool r315_evaluated = false;
    if (r315_two || gate_on)
    {
        static unsigned long long r315_evals_seen = 0ull;
        mgpu::calibrator::table r315_t;   // the tracked SCENE feature's count (feature_evaluates() counts every handle, FG included)
        const bool have_t = mgpu::calibrator::read(r315_t);
        const unsigned long long r315_evals_now = have_t ? r315_t.evals : r315_evals_seen;
        r315_evaluated = (r315_evals_now != r315_evals_seen);
        r315_evals_seen = r315_evals_now;
        if (gate_on)
        {
            // [FFX, 2026-11-11] THE GATE IS PROVIDER-BLIND. Its question is "did the game's own
            // DLSS evaluate this frame", and an FSR title never calls DLSS, so the answer was 0
            // forever and NR history was reset on every pass of every frame (measured 18:02:
            // [R323] reset stretch from f=1, and the operator's flicker). A contract publish
            // from the FFX hook IS this frame's hand-off. The counter is separate from the
            // NGX evaluate count on purpose: r315_evaluated also drives the MVecBarrierSource=2
            // tail copy, and letting an FFX frame start that copy would add a second writer.
            static unsigned long long cf_seen = 0ull;
            const unsigned long long cf_now = mgpu::calibrator::contract_frame_counter();
            const bool contract_frame = (cf_now != cf_seen);
            cf_seen = cf_now;
            mgpu::gpu1::note_game_dlss(!have_t ? -1 : ((r315_evaluated || contract_frame) ? 1 : 0));   // R323
        }
    }
    if (r315_two)
    {
        const int ph = mgpu::gpu1::r315_phase();
        mgpu::probe::r315_set_transition(false);   // R315b fix 3: value 2 never uses the transition route (value 1 does)
        // R315b fix 2: the tail copy only on a frame where the game evaluated DLSS. A menu, a loading
        // screen or a cutscene without DLSS records no barriers and delivers nothing - the seal goes
        // out without vectors, exactly as the transition route behaves there.
        if (ph != 2 && r315_evaluated)
        {
            mgpu::gpu1::r315_tail_scope(true);
            mgpu::probe::r315_tail_copy(cmd_list);
            mgpu::gpu1::r315_tail_scope(false);
        }
    }

    mgpu::gpu1::stream_on_finish_effects(
        reinterpret_cast<void *>(static_cast<uintptr_t>(cmd_list->get_native())),
        q_native,
        static_cast<unsigned long long>(res.handle),
        depth_h,
        mvec_h);

    if (depth_h != 0)
        cmd_list->barrier(depth_res, reshade::api::resource_usage::copy_source,
                                     reshade::api::resource_usage::shader_resource);
}

// D1: the discovery map sees a chain go away (resize, or the game recreating
// it when frame generation is switched). Off (one load) unless Discovery=1.
static void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    // [PORT PRESENTER H2, 2026-11-11] a chain going away (not a resize) is the
    // "suspected exit" notification, NOT a death sentence: the layer is told to stop
    // blocking in Present for 2 s and to check the game's TOP-LEVEL window before it
    // leaves. A mid-session rebuild (level load, resolution change) keeps the window,
    // so the layer stays. This is the H4 narrowing from 2026-11-02.
    if (!resize) mgpu::presenter::disp_note_teardown();
    // DX11 probe: everything it made lives on this chain's device. One load when off.
    // R228: only when THIS chain is the one it was built on - the bridge's own
    // present chain is resized at arm, and that resize destroyed the probe from the
    // bridge thread while the game thread was in the same resize path (TR-6 freeze).
    if (mgpu::dx11probe::on() && swapchain != nullptr &&
        mgpu::dx11probe::is_chain(reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native()))))
        mgpu::dx11probe::shutdown();
    if (swapchain != nullptr)
        mgpu::discovery::on_swapchain(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())), false, resize);
    // D1.5a: the latency probe stops using the device when the game chain goes. Off (one load)
    // unless LatencyProbe=1.
    if (swapchain != nullptr)
        mgpu::latprobe::on_chain_destroyed(
            reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native())));
    // D2.0: the game chain gone for good (resize=0) is our Reflex's teardown restore point -
    // a vendor call, so here and never in DllMain. Off (one load) unless OwnReflex=1.
    if (swapchain != nullptr)
    {
        mgpu::stallwatch::scope sw(mgpu::stallwatch::S_SWAPCHAIN);
        void *dch = reinterpret_cast<void *>(static_cast<uintptr_t>(swapchain->get_native()));
        mgpu::journal::on_chain_destroyed(dch, resize);
        mgpu::own_reflex::on_chain_destroyed(dch, resize);
    }
}

// T3 instrumentation: in a clean run, no destroy_device with the game's
// LUID appears while the game is running (acceptance). Every line carries
// the device's LUID, so a removal names whose device it was. When the
// destroyed device is the game's, ReShade uninit is imminent: signal the
// bridge thread to begin its orderly teardown (signal only - see
// worker::stop for why nothing here waits).
static void on_destroy_device(reshade::api::device *device)
{
    mgpu::adapter::log_device_luid("destroy_device", device);
    // [PORT PRESENTER H2] same "suspected exit" signal as a chain destroy; the layer
    // decides from the game's top-level window whether it is really over.
    mgpu::presenter::disp_note_teardown();
    // R227: the D3D11 game device going away is the DX11 producer's teardown
    // point, mirroring the D3D12 game-device teardown below: the stream
    // first (its drain uses our queue and fences), then the producer.
    if (device != nullptr && device->get_api() == reshade::api::device_api::d3d11 &&
        mgpu::dx11producer::on() &&
        mgpu::dx11producer::is_game_device(
            reinterpret_cast<void *>(static_cast<uintptr_t>(device->get_native()))))
    {
        mgpu::diag::info("[MGPU][T3] D3D11 game device released - signaling bridge thread "
                         "teardown (final log lines are best effort)");
        mgpu::gpu1::stream_shutdown();
        mgpu::worker::stop();
        mgpu::journal::on_game_device_destroyed();
        mgpu::strace::mark_clean_exit();   // R313
        mgpu::stallwatch::signal_stop();
        mgpu::dx11producer::shutdown();
        mgpu::mvec_census::shutdown();   // R253: unregisters its events; nothing of its own on the device
        mgpu::tex_census::shutdown();    // R279: the instrument's events, if on
        mgpu::mvec_extract::shutdown();  // R280: releases its objects, writes MVecExtract if decided
        mgpu::lq::signal_stop();
        return;
    }

    mgpu::adapter::selection_result sel;
    mgpu::adapter::get_selection(sel);
    if (sel.game_luid_known && device != nullptr &&
        device->get_api() == reshade::api::device_api::d3d12)
    {
        if (auto *dev12 = reinterpret_cast<ID3D12Device *>(device->get_native()))
        {
            const LUID luid = dev12->GetAdapterLuid();
            if (luid.LowPart == sel.game_luid.LowPart &&
                luid.HighPart == sel.game_luid.HighPart)
            {
                mgpu::diag::info("[MGPU][T3] game device released - signaling bridge thread "
                                 "teardown (final log lines are best effort)");

                // ---- V17: CLOSE THE NGX SESSION HERE TOO ----
                //
                // stream_shutdown() already runs in the bridge thread's
                // ordered teardown. That teardown only happens if the bridge
                // loop unwinds cleanly, and NOT closing the NGX session is the
                // fault that poisons the NEXT LAUNCH of the game - five clean
                // launches followed by one crash is what a cleanup that
                // usually runs looks like.
                //
                // This event is the earliest and most reliable notice we get
                // that the game's device is going. Calling here means the
                // session is closed on paths where the bridge thread never
                // gets to its teardown at all.
                //
                // SAFE TO CALL TWICE: stream_shutdown clears the pointers it
                // uses and returns early when there is nothing left, so the
                // ordered teardown finding it already done is the normal case
                // rather than an error.
                mgpu::gpu1::stream_shutdown();

                mgpu::worker::stop();

                // D2.0: our two threads leave their loops now (the queue drains
                // first), so none of them is inside this image if ReShade
                // unloads the add-on next. A new game device re-arms them.
                mgpu::journal::on_game_device_destroyed();
                mgpu::strace::mark_clean_exit();   // R313
                mgpu::stallwatch::signal_stop();
                mgpu::tex_census::shutdown();    // R279: the instrument's events, if on (D3D12 game device)
                mgpu::mvec_extract::shutdown();  // R280 (D3D12 game device)
                mgpu::lq::signal_stop();
            }
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        // T4 (exception 1): capture our own HMODULE before anything else.
        // Set unconditionally (even if registration is refused below) so
        // the accessor is never null in a loaded module.
        g_module = hModule;
        if (!reshade::register_addon(hModule))
        {
            // ReShade absent (e.g. an add-on-disabled build) or add-on
            // API version mismatch. No log path is guaranteed here - we
            // were refused - and returning FALSE from DllMain would
            // terminate the game process. One DebugView line, then load:
            //   [MGPU][T1] missing + this line present -> registration refused
            //   [MGPU][T1] missing + this line absent  -> add-on file not found
            // (see README, "Reading the log")
            OutputDebugStringA("[MGPU][T1] register_addon failed");
            break;
        }
        reshade::log::message(reshade::log::level::info,
            "[MGPU][T1] MGPU Bridge add-on registered - stock ReShade, add-on API version "
            MGPU_STR(RESHADE_API_VERSION));
        // T2: per-event logging, provisional LUID capture, one-time table
        // enumeration; the selection itself is deferred to the
        // swapchain-derived game LUID.
        {
            // R141. Before anything else this add-on says, say what it is.
            char v[420];
            snprintf(v, sizeof v,
                     "[MGPU][R141] MGPU Bridge %s | built %s %s | add-on API 20. IDENTIFY EVERY "
                     "LOG BY THIS LINE. ReShade's own 'Registered add-on' line carries "
                     "v0.0.0.0 because that field has never been filled in, which is why "
                     "reports have had to quote the file size in bytes instead. If this line "
                     "is absent the build predates 0.2.3.",
                     MGPU_VERSION_STR, __DATE__, __TIME__);
            mgpu::diag::info(v);
        }
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        // PanelOverPicture (N01950): reads its key once and registers its
        // reshade_overlay event only when PanelOverPicture=1 (live_panel.cpp).
        mgpu::live_panel::attach(hModule);
        // The simple panel's layout file lives beside the add-on. init() only remembers the
        // directory (R330, F2: no file read here); panel_simple.txt is read on the first panel draw.
        mgpu::psimple::init(hModule);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);   // D1
        reshade::register_event<reshade::addon_event::set_fullscreen_state>(on_set_fullscreen_state);   // R241d observe / R242 refuse
        reshade::register_event<reshade::addon_event::create_swapchain>(on_create_swapchain);           // R242
        // T3: device lifecycle instrumentation + teardown trigger.
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        // P1.5: capture one real frame. Registered last because it is the only
        // subscription that acts on the game's own command list.
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(
            on_reshade_finish_effects);
        // L3: see stream_on_present. Registered unconditionally; the add-on
        // side decides whether to act on it, so the ini can turn the behaviour
        // on and off without a rebuild.
        reshade::register_event<reshade::addon_event::present>(on_present);
        // D2.0: after the game's Present. Registered here with every other
        // event (registering later, from a render thread, races ReShade's
        // unsynchronised event list). Off: the handler returns after a load.
        reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
        // V65: the bridge steps aside while the game's overlay is open.
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(
            on_reshade_open_overlay);
        // R158: which runtime is which, for the global overlay key. Registered
        // unconditionally and inert unless dcomp_explicit_off_single_display()
        // - these two only ever store and clear a pointer.
        reshade::register_event<reshade::addon_event::init_effect_runtime>(
            on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(
            on_destroy_effect_runtime);
        // P9.1. NOT initialised here. The probe reads mgpu.ini, and file I/O
        // inside DllMain runs under the loader lock, where the CRT is entitled
        // to load a locale DLL and deadlock against the lock we are already
        // holding. It is initialised from on_init_swapchain instead, which is
        // on the game's render thread with the loader lock long released - the
        // same thread the probe's own register_event calls belong on anyway.
#if defined(MGPU_HAVE_IMGUI)
        reshade::register_overlay("MGPU Bridge", draw_mgpu_overlay);
        reshade::log::message(reshade::log::level::info,
            "[MGPU][P6.4] overlay panel registered - open the ReShade overlay (Home) over the "
            "BRIDGE window to get passes, the neural on/off and the intensity sliders. The "
            "CTRL+ALT+F8/F9/F11 hotkeys still work and drive the same values.");
#else
        reshade::log::message(reshade::log::level::warning,
            "[MGPU][P7.1] NO OVERLAY PANEL IN THIS BUILD - imgui.h was not on the include path "
            "when this compiled, and reshade.hpp only wires up the ImGui function table when "
            "IMGUI_VERSION_NUM is defined ahead of it. Nothing else is affected: use the "
            "CTRL+ALT+F8 / F9 / F11 hotkeys, which drive exactly the same values. To get the "
            "panel, put ImGui 1.92.5's imgui.h and imconfig.h (tag 3912b3d, the commit ReShade "
            "18deaa52 pins at deps/imgui) somewhere on the include path. A target_include_"
            "directories line pointing at a directory that does not exist is silently ignored "
            "by CMake and __has_include then just says no, which is how this warning survives "
            "an edit to CMakeLists that looked correct.");
#endif
        break;

    case DLL_PROCESS_DETACH:
        // Both unload paths only signal the stop event and return.
        // DllMain runs under the loader lock, and a thread cannot finish
        // exiting without that lock (its exit dispatches
        // DLL_THREAD_DETACH to every loaded module): a join here would
        // deadlock, and a timed-out join would leave a live thread
        // pointing into a DLL that is about to unmap. The mutex is never
        // held across a wait (see worker::stop).
        //
        //   lpReserved != NULL: process exit. Windows terminated every
        //   other thread before this call; the bridge thread is dead and
        //   the OS reclaims its GPU objects. The signal is ceremonial.
        //   lpReserved == NULL: dynamic FreeLibrary (ReShade unloading
        //   us). The bridge thread is live; it tears down the GPU 1
        //   objects itself once it wakes (T4: window, class, device,
        //   adapter - in that order, on its own thread). API 20 has no
        //   reshade_unload event to drive the teardown from earlier, so if
        //   the FreeLibrary happens first the GPU 1 device is simply leaked
        //   - explicitly in scope at P0. A hang is not.
        // ---- V21: CLOSE THE NGX SESSION HERE TOO. LAST RESORT AND IT IS NEEDED. ----
        //
        // Measured 2026-09-13 on a CLEAN exit: ReShade goes
        //     Destroyed runtime environment -> Unregistered add-on -> Exiting
        // and NEVER RAISES destroy_device. So both of the other call sites -
        // on_destroy_device and the bridge thread's ordered teardown - are
        // hooked to events that did not happen, the NGX session stayed open,
        // and the NEXT launch of the game paid for it. That is the "open it
        // twice and it fixes itself" symptom.
        //
        // ONLY ON THE FreeLibrary PATH. lpReserved == NULL means ReShade is
        // unloading us while the process lives, which is when the device is
        // still valid and there is something to close. lpReserved != NULL is
        // process termination: every other thread is already dead, and calling
        // into a vendor DLL there is how DllMain rules get broken.
        //
        // Still under the loader lock, which is why this is the LAST of three
        // call sites rather than the first, and why stream_shutdown announces
        // every vendor call before making it. If the process dies here, the
        // log names which one.
        if (lpReserved == nullptr)
        {
            mgpu::mvec_extract::shutdown();   // R280c: the only teardown a clean exit reaches (V21); releases the module's objects
            mgpu::diag::info("[MGPU][T3] FreeLibrary unload - closing the NGX session from "
                             "DllMain, because ReShade does not always raise destroy_device "
                             "and an unclosed session poisons the next launch.");
            mgpu::gpu1::stream_shutdown();
        }
#if defined(MGPU_HAVE_IMGUI)
        reshade::unregister_overlay("MGPU Bridge", draw_mgpu_overlay);
#endif
        // [PORT PRESENTER H2, 2026-11-11] the display layer owns a window, a flip chain
        // and a thread. Signal only, never join here: joining under the loader lock is
        // the deadlock the worker comment above describes (the thread's exit needs
        // DLL_THREAD_DETACH, which needs this lock). The zombie kill is the unload-path
        // deadline the 2026-11-02 forensics measured: it only fires when ZombieKillMs>0
        // (PBZ only), the game's top-level window is gone and no frame arrived for a
        // second. Every other title has ZombieKillMs=0 and this is a no-op.
        mgpu::presenter::disp_begin_dying();
        if (mgpu::presenter::disp_zombie_kill_now())
        {
            TerminateProcess(GetCurrentProcess(), 0);
            return TRUE;   // not reached
        }
        // P9.1 before worker::stop: unregistering our events while the game
        // thread may still raise them is the one ordering that matters here,
        // and DllMain is single-threaded with respect to ReShade's dispatch.
        mgpu::probe::shutdown();
        mgpu::worker::stop();
        // R101c: PUT THE IMPORT SLOTS BACK BEFORE THIS DLL GOES AWAY.
        // ~150 modules hold pointers into this image. Unloading without
        // restoring them leaves every one of those slots aimed at unmapped
        // memory, and the next GetProcAddress anywhere in the process walks
        // into it. This is what stopped Dragon Sword launching.
        // D1: the same for the discovery map's NvAPI thunks. A no-op unless
        // Discovery=1 installed them.
        mgpu::discovery::uninstall();
        mgpu::calibrator::uninstall();
        // D2.0: our two threads leave their loops (waited for only on the
        // FreeLibrary path, max 300 ms each), then the journal's close line,
        // written directly. All three are no-ops when their key was off.
        mgpu::stallwatch::stop(lpReserved != nullptr);
        mgpu::lq::stop(lpReserved != nullptr);
        mgpu::journal::session_close_direct(lpReserved != nullptr ? "DLL_PROCESS_DETACH, process exit"
                                                                  : "DLL_PROCESS_DETACH, FreeLibrary unload");
        mgpu::strace::mark_clean_detach(lpReserved != nullptr);   // R313a: file only; one relaxed load when the trace is off
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
