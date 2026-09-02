// MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
//
// T1: register the add-on with stock ReShade, write one init log line.
// T2: per device event, a provisional game-LUID capture (first d3d12
//     device) and a one-time adapter-table enumeration; the selection
//     itself is deferred to the swapchain-derived game LUID (init_swapchain)
//     - the four rules of brief section 06 live in adapter.hpp/adapter.cpp.
// T3: the bridge thread (spawned on the game thread here, never from
//     DllMain) creates the private device on the selected adapter;
//     destroy_device events are logged, and the game's own device
//     release triggers the bridge thread's orderly teardown.
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
#include <reshade.hpp>

#include "adapter.hpp"
#include "diag.hpp"
#include "worker.hpp"

extern "C" __declspec(dllexport) const char *NAME = "MGPU Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Cross-Adapter Bridge P0: second ReShade effect runtime on a second GPU";

#define MGPU_STR2(s) #s
#define MGPU_STR(s) MGPU_STR2(s)

// T2: both init_device and init_swapchain are the game's own separate API
// calls - ReShade's CreateDXGIFactory1 hook is fully unwound when they
// fire, so the enumeration's factory creation does not recurse into
// add-on init. (Swapchain CREATION stays off the game thread entirely -
// that is the re-entrancy hazard, handled in T4+.)
static void on_init_device(reshade::api::device *device)
{
    mgpu::worker::ensure_started();
    // Every device event is logged; the first d3d12 one also captures the
    // provisional game LUID and builds the adapter table. No selection
    // here - brief rule 2 defers it to the swapchain-derived game LUID
    // (on_init_swapchain).
    mgpu::adapter::on_device(device);
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    mgpu::worker::ensure_started();
    // The swapchain's device is the authoritative game render device. Its
    // LUID overrides the provisional init_device value and runs the
    // one-shot selection (or a terminal refusal) - see adapter.cpp.
    mgpu::adapter::on_swapchain(swapchain, resize);
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
        //   objects itself once it wakes. API 20 has no reshade_unload
        //   event to drive the teardown from earlier, so if the
        //   FreeLibrary happens first the GPU 1 device is simply leaked
        //   - explicitly in scope at P0. A hang is not.
        (void)lpReserved;
        mgpu::worker::stop();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
