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
#include <cstdio>     // P1.6: snprintf. This file had no formatted logging before.
#include <cstring>    // R137: strstr, to spot the depth tap by its effect name.
#include <cwchar>     // R142: wcsrchr, to split our own module path for [R142].
#include <atomic>     // R138: the present counter and the said-once flags.

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
#define MGPU_VERSION_STR "0.2.3"


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
#include "calibrator.hpp"
#include "probe.hpp"
#include "sl_probe.hpp"   // SL1
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
static void mgpu_mvec_transport_hook(void *cmd_list_native, unsigned long long resource)
{
    mgpu::gpu1::stream_mvec_copy(cmd_list_native, resource);
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
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
            mgpu::calibrator::install(mgpu::probe::calib_mode());
            mgpu::calibrator::set_jitter_mode(mgpu::probe::jitter_mode());

            // R106. The SAME transport hook the probe uses - one copy path,
            // two possible triggers, so the two can be compared directly
            // instead of being two different pieces of code.
            mgpu::calibrator::set_mvec_hook(&mgpu_mvec_transport_hook);
            mgpu::calibrator::set_eval_copy(mgpu::probe::eval_copy_mode());
        }
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
                              "is SWITCHED OFF. The add-on enables it itself on the GAME "
                              "runtime, so present-but-off THERE means that did not take. "
                              "On the bridge runtime this is the shipped state and is "
                              "expected - gpu1.ini ships with an empty Techniques= on "
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
static int mgpu_game_is_native()
{
    mgpu::calibrator::table t{};
    if (!mgpu::calibrator::read(t)) return -1;
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

    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);

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
    // Tall enough that neither column scrolls in the common case, short enough
    // to leave the probe section visible underneath.
    const float col_h   = 560.0f;

    ImGui::BeginChild("mgpu_box_bridge", ImVec2(col_w, col_h), true);
    ImGui::SeparatorText("MGPU BRIDGE  -  neural rendering");
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
    if (passes >= 2)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                           "x2 is free to the game but holds the second GPU near its power "
                           "limit. x1 recommended.");
    else
        ImGui::TextDisabled("x1 leaves the second GPU about half idle.");

    bool neural = st.neural;
    if (ImGui::Checkbox("neural stage", &neural))
        mgpu::gpu1::ui_set_neural(neural);
    ImGui::SameLine();
    ImGui::TextDisabled(st.nr_ok ? "(running)" : "(transport only)");

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
    for (unsigned i = 0; i < st.passes && i < st.max_passes; ++i)
    {
        char lab[24];
        snprintf(lab, sizeof lab, "pass %u", i + 1);
        float v = st.intensity[i];
        if (ImGui::SliderFloat(lab, &v, 0.0f, 2.0f, "%.2f"))
            mgpu::gpu1::ui_set_intensity(i + 1, v);
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
    ImGui::TextDisabled("Split: input left of the seam, output right, same frame.");
    ImGui::TextDisabled("CTRL+ALT+LEFT/RIGHT move it with no overlay open (SHIFT = coarse).");

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

        snprintf(lv, sizeof lv, "fps   game %.1f   GPU 1 %.1f",
                 st.fps_produced, st.fps_consumed);
        ImGui::TextUnformatted(lv);
        if (st.fps_consumed + 1.0 < st.fps_produced)
            ImGui::TextDisabled("GPU 1 behind the game - frames are being skipped, not dropped.");

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
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Requested but not running - see [MGPU][C2-SR] in the log.");
            if (mgpu_game_is_native() == 1)
            {
                ImGui::TextDisabled("The game is at DLAA, so it renders at display resolution and");
                ImGui::TextDisabled("there is nothing to upscale.");
            }
            ImGui::TextDisabled("Created once, at arm. Restart to retry - changing the game's");
            ImGui::TextDisabled("DLSS setting or the upscaler mode now will not start it.");
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
                    mgpu::gpu1::ui_ini_write("SRScale",    0);
                    mgpu::gpu1::ui_ini_write("SRMvLowRes", 0);
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
            if (want_match && mgpu_game_is_native() == 1)
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
                if (st.sr_on)
                {
                    ImGui::TextColored(y, "DLAA - Native Upscaling detected.");
                    ImGui::TextColored(y, "Change the game to DLSS, or enable Experimental.");
                }
                else if (st.sr_requested)
                    // R164. "Enable DLSS in-game or Experimental" put both
                    // routes behind one verb and one place, so Experimental
                    // read as something to find in the GAME's menu. It is a
                    // control in THIS panel. The row above already had the
                    // right shape - one verb per route, each naming where it
                    // lives - so this row now uses it verbatim and adds only
                    // the restart.
                {
                    ImGui::TextColored(y, "DLAA - Native Upscaling detected. Change the game");
                    ImGui::TextColored(y, "to DLSS or enable Experimental - RESTART TO APPLY");
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

                // R165. The Experimental half of what R163 did for Native:
                // say which mode is detected and whether this state needs
                // anything done about it. Dim when there is nothing to do -
                // yellow is for a pending action, and spending it on "all
                // good" is what trains people past it.
                if (sr_live_matches)
                {
                    char xl[120];
                    snprintf(xl, sizeof xl,
                             "Experimental Upscaling detected. Running at %u%% - "
                             "no restart needed.", want_pct);
                    ImGui::TextDisabled("%s", xl);
                }
                else
                {
                    char xl[120];
                    snprintf(xl, sizeof xl,
                             "Experimental Upscaling detected. Restart once to apply %u%%.",
                             want_pct);
                    ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f), "%s", xl);
                }
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
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.23f, 1.0f),
                                   "Super Resolution is ON but not running on this title.");

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
                const int nat = mgpu_game_is_native();
                if (nat == 1)
                {
                    // R162. This branch IS the refused case - sr_requested
                    // with no handle - so there is one situation and it needs
                    // one line. The state above it already said SR is not
                    // running; this says why and what to do.
                    // R163. Same words as the line under the radios, because
                    // two different sentences for one state is how somebody
                    // ends up believing they are two different problems.
                    ImGui::TextDisabled("Change the game to DLSS or enable Experimental");
                    ImGui::TextDisabled("Upscaler, then restart once. Changing either now");
                    ImGui::TextDisabled("will not start it.");
                }
                else
                {
                    ImGui::TextDisabled("The Super resolution box above says why. Look for the");
                    ImGui::TextDisabled("C2-SR line in ReShade.log - it names where R came from.");
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
    ImGui::TextDisabled("github.com/maohgad-web/Neural-coprocessor");
}
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
static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr) return;
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
    if (mgpu::gpu1::dcomp_overlay_mode())
    {
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

static void on_present(reshade::api::command_queue *queue,
                       reshade::api::swapchain *swapchain,
                       const reshade::api::rect *, const reshade::api::rect *,
                       uint32_t, const reshade::api::rect *)
{
    if (queue == nullptr) return;
    mgpu::gpu1::stream_on_present(
        reinterpret_cast<void *>(static_cast<uintptr_t>(queue->get_native())));

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
    if (swapchain != nullptr && mgpu::gpu1::dcomp_explicit_off_single_display())
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

                std::atomic<int>  &pend = p_is_game ? g_ov_pend_game : g_ov_pend_bridge;
                std::atomic<bool> &echo = p_is_game ? g_ov_echo_game : g_ov_echo_bridge;
                std::atomic<void *> &slot = p_is_game ? g_rt_game : g_rt_bridge;

                auto *rt = (reshade::api::effect_runtime *)slot.load(std::memory_order_relaxed);
                if (rt != nullptr)
                {
                    const int want = pend.exchange(-1, std::memory_order_relaxed);
                    if (want >= 0)
                    {
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
                        mgpu::calibrator::table ct;
                        if (mgpu::calibrator::read(ct) &&
                            (ct.have & mgpu::calibrator::KEY_MVEC) != 0u)
                            mgpu::probe::set_mvec_override(ct.mvec);
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
        if (tbl != said_tbl || depth_h != said_tap)
        {
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
    const unsigned long long mvec_h = rt_is_game ? mgpu::probe::mvec_source() : 0ull;

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

// T3 instrumentation: in a clean run, no destroy_device with the game's
// LUID appears while the game is running (acceptance). Every line carries
// the device's LUID, so a removal names whose device it was. When the
// destroyed device is the game's, ReShade uninit is imminent: signal the
// bridge thread to begin its orderly teardown (signal only - see
// worker::stop for why nothing here waits).
static void on_destroy_device(reshade::api::device *device)
{
    mgpu::adapter::log_device_luid("destroy_device", device);

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
            mgpu::diag::info("[MGPU][T3] FreeLibrary unload - closing the NGX session from "
                             "DllMain, because ReShade does not always raise destroy_device "
                             "and an unclosed session poisons the next launch.");
            mgpu::gpu1::stream_shutdown();
        }
#if defined(MGPU_HAVE_IMGUI)
        reshade::unregister_overlay("MGPU Bridge", draw_mgpu_overlay);
#endif
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
        mgpu::calibrator::uninstall();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}