// MGPU Bridge — Cross-Adapter Bridge, Milestone 0 (Gate P0)
//
// T1: register the add-on with stock ReShade and write exactly one init
// log line. No events, no threads, no D3D12 — those are T2 and later.
//
// Convention verified against crosire/reshade v6.8.0
// (18deaa52de0c425a78b329e9cb3c497281cd00ec), include/reshade.hpp and
// examples/01-fps_limit: add-ons export NAME and DESCRIPTION, and
// DllMain calls reshade::register_addon(hModule), which fails cleanly
// if ReShade is absent or its add-on API is older than
// RESHADE_API_VERSION.

#include <windows.h>
#include <reshade.hpp>

extern "C" __declspec(dllexport) const char *NAME = "MGPU Bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Cross-Adapter Bridge P0: second ReShade effect runtime on a second GPU";

#define MGPU_STR2(s) #s
#define MGPU_STR(s) MGPU_STR2(s)

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
        {
            // ReShade absent (e.g. add-on-disabled build) or add-on API
            // version mismatch. No log path is guaranteed to exist here,
            // and returning FALSE from DllMain would terminate the game
            // process — so stay silent, as in ReShade's own samples. The
            // missing [MGPU][T1] line is the signal.
            break;
        }
        reshade::log::message(reshade::log::level::info,
            "[MGPU][T1] MGPU Bridge add-on registered — stock ReShade, add-on API version "
            MGPU_STR(RESHADE_API_VERSION));
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
