// MGPU Bridge — Cross-Adapter Bridge, Milestone 0 (Gate P0)
//
// T1: register the add-on with stock ReShade, write one init log line.
// T2: one-shot adapter enumeration + LUID selection on the first
//     device/swapchain (the selection rule lives in adapter.hpp).
// T3: the bridge thread (spawned on the game thread here, never from
//     DllMain) creates the private device on the selected adapter;
//     device lifecycle events are logged.
//
// Convention verified against crosire/reshade v6.8.0
// (18deaa52de0c425a78b329e9cb3c497281cd00ec), include/reshade.hpp and
// examples/01-fps_limit: add-ons export NAME and DESCRIPTION, and
// DllMain calls reshade::register_addon(hModule), which fails cleanly
// if ReShade is absent or its add-on API is older than
// RESHADE_API_VERSION.

#include <windows.h>
#include <reshade.hpp>

#include "adapter.hpp"
#include "worker.hpp"

extern "C" __declspec(dllexport) const char *NAME = "MGPU Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Cross-Adapter Bridge P0: second ReShade effect runtime on a second GPU";

#define MGPU_STR2(s) #s
#define MGPU_STR(s) MGPU_STR2(s)

// T2 trigger. Both init_device and init_swapchain are the game's own
// separate API calls — ReShade's CreateDXGIFactory1 hook is fully unwound
// when they fire, so creating a DXGI factory and enumerating adapters here
// does not recurse into add-on init. (Swapchain CREATION stays off the
// game thread entirely — that is the re-entrancy hazard, handled in T4+.)
static void on_init_device(reshade::api::device *device)
{
    mgpu::worker::ensure_started();
    if (mgpu::adapter::run_once(device, "init_device"))
        return;
    // A later D3D12CreateDevice in the process — at P0 that is our own
    // T3 device. Free, independent confirmation of the T3 binding.
    mgpu::adapter::log_device_luid("init_device (subsequent)", device);
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    (void)swapchain;
    (void)resize;
    mgpu::worker::ensure_started();
    mgpu::adapter::run_once(nullptr, "init_swapchain");
}

// T3 instrumentation: device_removed must not appear in a clean run
// (acceptance). If it does, the line carries the device's LUID so the
// log names whose device it was.
static void on_device_removed(reshade::api::device *device)
{
    mgpu::adapter::log_device_luid("device_removed", device);
}

static void on_device_restored(reshade::api::device *device)
{
    mgpu::adapter::log_device_luid("device_restored", device);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
        {
            // ReShade absent (e.g. an add-on-disabled build) or add-on
            // API version mismatch. No log path is guaranteed here — we
            // were refused — and returning FALSE from DllMain would
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
        // T2: one-shot enumeration + selection on the first device/swapchain.
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        // T3: device lifecycle instrumentation.
        reshade::register_event<reshade::addon_event::device_removed>(on_device_removed);
        reshade::register_event<reshade::addon_event::device_restored>(on_device_restored);
        break;

    case DLL_PROCESS_DETACH:
        // Join the bridge thread before unregistering so its final log
        // lines still reach ReShade's log; the join has a timeout so
        // process exit can never hang.
        mgpu::worker::stop();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
