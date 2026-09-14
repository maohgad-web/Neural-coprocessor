// MGPU Bridge - SL1: the Streamline interposer probe.
//
// WHAT IT IS FOR. Battlefield 6 killed the bridge inside sl.common.dll on
// 2026-09-14, on our own worker thread, with sl.interposer between our
// frames and the fault. sl.interposer wraps D3D12CreateDevice and
// CreateDXGIFactory for the whole process, so a device we create on the
// SECOND adapter can be a Streamline proxy without us asking for one, and
// every call we make on it re-enters Streamline. This file answers, at
// runtime and before anything is armed: is it, and what is underneath.
//
// HOW IT ASKS. NVIDIA documents the route for exactly our case - a third
// party library that is not the Streamline host and does not link the SDK:
//
//   "Use special GUID {ADEC44E2-61F0-45C3-AD9F-1B37379284FF} to obtain the
//    underlying native interface if SL proxy is used."
//
// So the whole instrument is one QueryInterface. No slInit, no sl.interposer
// import, no SDK header, no link library, and nothing inferred from a
// disassembly. It is a public, documented interface used as documented.
//
// SELF-CONTAINED, like probe.cpp / calibrator.cpp / screen.cpp. It holds no
// ReShade types and no gpu1_context types: every interface arrives as a
// void* that the caller already owns. Nothing here creates a device, opens
// an NGX session, or allocates on any adapter, so it is safe to call on the
// first frame and inside a recovery launch.
#pragma once

namespace mgpu::slprobe
{
    // What one interface turned out to be.
    enum class kind
    {
        unknown = 0,   // nothing was asked - null pointer, or the call threw
        native  = 1,   // no SL proxy: the interface is the real thing
        proxy   = 2    // an SL proxy wraps a native interface
    };

    // Classify one COM interface. `iface` is an IUnknown* - ID3D12Device*,
    // ID3D12GraphicsCommandList*, IDXGISwapChain* all qualify.
    //
    // On `proxy`, *out_native receives the underlying interface FOR IDENTITY
    // ONLY: the reference QueryInterface took is released before returning,
    // so the pointer is safe to print and compare and must not be called.
    // Use acquire_native when the intention is to keep it.
    //
    // out_native may be null. A null `iface` returns `unknown` and asks
    // nothing.
    kind classify(void *iface, void **out_native);

    // The same query, keeping the reference. Returns the native interface
    // with one reference the CALLER OWNS AND MUST RELEASE, or null when the
    // interface is not a proxy. This is the escape hatch: work driven on the
    // returned interface does not re-enter Streamline.
    void *acquire_native(void *iface);

    // Is sl.interposer.dll resident in this process. Loader query only.
    bool interposer_resident();

    // Remember the game's ID3D12Device for the census line. Called from the
    // side of the add-on that holds ReShade types, because the side that
    // creates the bridge device deliberately does not. Stores the pointer
    // and nothing else - no reference is taken, so this must be a device the
    // process already keeps alive, which the game's is.
    void note_game_device(void *dev);

    // The [MGPU][SL] census line: which sl.* modules are resident, the
    // interposer's file version, and the classification of both devices.
    // Said once per process; later calls are ignored, so it is safe to place
    // at more than one site and let whichever runs first win.
    //
    // our_device  - the ID3D12Device we created on the bridge adapter, or null
    // game_device - the game's device, or null to use whatever
    //               note_game_device last recorded
    void report(void *our_device, void *game_device);

    // The acquisition verdict, from counters the lateral probe already keeps.
    // Said once. Zero binds with a live barrier count is the signature that
    // something sits between the game and ReShade on the bind path and not on
    // the barrier path - which is what Battlefield 6 showed across 103,000
    // frames.
    void report_acquisition(unsigned long long bind_fires,
                            unsigned long long rp_fires,
                            unsigned long long bar_fires);

    // ---- SL2a: THE API SHAPE, READ WITHOUT CALLING ANYTHING ----
    //
    // WHAT IT ANSWERS. Whether this title drives Streamline through the
    // TAGGED route - the host hands Streamline its depth, motion vectors,
    // colour and UI by tagging resources, and Streamline evaluates - or
    // through some other shape. If it is the tagged route then the game's
    // own declaration of its motion vector buffer exists inside this
    // process, with the extent and format attached, and it is authoritative
    // in a way no ranking of ours can be. It is also where a HDR colour
    // buffer would be named rather than guessed at from a format.
    //
    // HOW IT ASKS. GetProcAddress on the already-loaded sl.interposer.dll,
    // by documented public name, and NOTHING IS CALLED. A non-null pointer
    // means the export exists; that is the entire measurement. No slInit, no
    // SDK struct, no argument passed to anything, and therefore no ABI this
    // file could get wrong - the failure mode that makes an instrument into
    // the fault it was sent to find.
    //
    // Said once, from report(), so it needs no call site of its own.
    void report_api_shape();
}
