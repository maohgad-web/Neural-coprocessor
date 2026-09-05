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
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
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
    };

    void technique_cb(reshade::api::effect_runtime *rt,
                      reshade::api::effect_technique tech, void *user)
    {
        preset_probe *p = static_cast<preset_probe *>(user);
        ++p->total;
        if (!rt->get_technique_state(tech)) return;
        ++p->enabled;

        char tn[128] = {}; size_t tns = sizeof tn - 1;
        rt->get_technique_name(tech, tn, &tns);
        char en[128] = {}; size_t ens = sizeof en - 1;
        rt->get_technique_effect_name(tech, en, &ens);

        const int wrote = snprintf(p->names + p->used, sizeof p->names - p->used,
                                   "%s%s@%s", (p->used != 0) ? ", " : "", tn, en);
        if (wrote > 0 && (size_t)wrote < sizeof p->names - p->used)
            p->used += (size_t)wrote;
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
static void draw_mgpu_overlay(reshade::api::effect_runtime *)
{
    mgpu::gpu1::ui_state st;
    mgpu::gpu1::ui_read(st);

    if (!st.armed)
    {
        ImGui::TextUnformatted("Stream not armed. Press CTRL+ALT+F10 in gameplay.");
        ImGui::TextUnformatted("Values set here become the starting values.");
    }
    else if (st.summarised)
    {
        ImGui::TextUnformatted("Stream finished - it ran to its bound. Restart to run another.");
    }

    bool neural = st.neural;
    if (ImGui::Checkbox("Neural stage", &neural))
        mgpu::gpu1::ui_set_neural(neural);
    ImGui::SameLine();
    ImGui::TextDisabled(st.nr_ok ? "(up)" : "(not running - transport only)");

    int passes = (int)st.passes;
    ImGui::TextUnformatted("Passes");
    for (unsigned i = 1; i <= st.max_passes; ++i)
    {
        char lab[8];
        snprintf(lab, sizeof lab, "x%u", i);
        if (i > 1) ImGui::SameLine();
        if (ImGui::RadioButton(lab, &passes, (int)i))
            mgpu::gpu1::ui_set_passes(i);
    }
    // Formatted with snprintf and handed over finished. ImGui's Text family is
    // varargs, and this add-on cannot test-compile against the ImGui the repo
    // will actually use - a finished string cannot be mis-forwarded.
    char note[160];
    snprintf(note, sizeof note,
             "All %u handles were created at arm, so this costs nothing to change.",
             st.max_passes);
    ImGui::TextUnformatted(note);

    ImGui::Separator();
    ImGui::TextUnformatted("Intensity (0.00 - 2.00)");

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
    ImGui::TextDisabled("Live from the next frame - NGX parameters are set per evaluate.");

    if (st.armed)
    {
        ImGui::Separator();
        char c1[160], c2[200];
        snprintf(c1, sizeof c1, "produced %llu   consumed %llu", st.produced, st.consumed);
        snprintf(c2, sizeof c2, "dropped %llu   overrun %llu   nr skipped %llu",
                 st.dropped, st.overrun, st.skipped);
        ImGui::TextUnformatted(c1);
        ImGui::TextUnformatted(c2);
        ImGui::TextDisabled("skipped = seal checked, neural work not run because a newer");
        ImGui::TextDisabled("frame was already waiting. Not a drop.");
        if (st.overrun != 0)
            ImGui::TextDisabled("overrun climbing = GPU 1 is past its budget at this pass count.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Anything changed here makes this run a TUNING run, not a");
    ImGui::TextDisabled("measurement - the summary in the log will say so.");
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
static void on_reshade_finish_effects(reshade::api::effect_runtime *runtime,
                                      reshade::api::command_list *cmd_list,
                                      reshade::api::resource_view rtv,
                                      reshade::api::resource_view rtv_srgb)
{
    (void)rtv_srgb;
    if (runtime == nullptr || cmd_list == nullptr) return;

    reshade::api::device *dev = runtime->get_device();
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
            }
        }
    }

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
    mgpu::gpu1::stream_on_finish_effects(
        reinterpret_cast<void *>(static_cast<uintptr_t>(cmd_list->get_native())),
        q_native,
        static_cast<unsigned long long>(res.handle));
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
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        // T3: device lifecycle instrumentation + teardown trigger.
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        // P1.5: capture one real frame. Registered last because it is the only
        // subscription that acts on the game's own command list.
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(
            on_reshade_finish_effects);
#if defined(MGPU_HAVE_IMGUI)
        reshade::register_overlay("MGPU Bridge", draw_mgpu_overlay);
        reshade::log::message(reshade::log::level::info,
            "[MGPU][P6.4] overlay panel registered - open the ReShade overlay (Home) over the "
            "BRIDGE window to get passes, the neural on/off and the intensity sliders. The "
            "CTRL+ALT+F8/F9/F11 hotkeys still work and drive the same values.");
#else
        reshade::log::message(reshade::log::level::warning,
            "[MGPU][P6.4] NO OVERLAY PANEL IN THIS BUILD - imgui.h was not on the include path "
            "when this compiled, and reshade.hpp only wires up the ImGui function table when "
            "IMGUI_VERSION_NUM is defined ahead of it. Nothing else is affected: use the "
            "CTRL+ALT+F8 / F9 / F11 hotkeys, which drive exactly the same values. Add the "
            "ReShade deps' imgui headers to the include path to get the panel.");
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
        (void)lpReserved;
#if defined(MGPU_HAVE_IMGUI)
        reshade::unregister_overlay("MGPU Bridge", draw_mgpu_overlay);
#endif
        mgpu::worker::stop();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}