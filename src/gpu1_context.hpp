// MGPU Bridge - the private D3D12 device on the selected adapter (T3;
// the device-removal poll accessor arrives with T4)
//
// Bridge thread only: every GPU 1 object is created, used and destroyed
// on the bridge thread. T3 does exactly one thing - create the device
// against the T2 selection, re-verify the binding LUID, log. The window
// and swapchain arrive with T4/T5.
#pragma once

#include <windows.h>

#include "adapter.hpp"

namespace mgpu::gpu1
{
    // Bridge thread only. Returns false when no device was created
    // (invalid selection, creation failure, or LUID mismatch). Every
    // failure is already logged with its inputs; there is never a
    // fallback to the game's adapter.
    bool create_device(const adapter::selection_result &sel);

    // Bridge thread only. Releases the device if one exists.
    //
    // T5 (extension, not replacement): first releases the present chain
    // if one exists - waiting for the GPU to idle (one final fence signal
    // + wait), then releasing the chain objects in reverse creation
    // order - and only then the device, exactly as before. The chain goes
    // first because the swapchain holds a reference to the window it was
    // created against and must not outlive DestroyWindow (worker.cpp
    // reorders its teardown accordingly). Still a no-op for the chain
    // when none was created (the no-window path is unaffected).
    void shutdown();

    bool has_device();

    // T4 (brief section 00, exception 4): poll the device's removal reason
    // without exposing the ID3D12Device. The raw pointer never leaves this
    // translation unit - handing it out would put it outside the mutex that
    // guards it, and shutdown() could release it between the caller's read
    // and its use. Returns false when no device exists (nothing to poll;
    // `out` is set to S_OK). Returns true and sets `out` to
    // ID3D12Device::GetDeviceRemovedReason() (S_OK when healthy, otherwise
    // the removal reason) when a device exists. Takes this file's lock, so
    // the call is made with the device pointer still guarded. The value is
    // sticky once removed (brief section 09), so the caller logs only on the
    // transition away from S_OK.
    bool device_removed_reason(HRESULT &out);

    // ---- T5: the present chain (brief section 06) ----
    //
    // All D3D12/DXGI objects live behind this door: the raw pointers never
    // leave this translation unit - the same guarantee as the device, which
    // is why they sit behind the same mutex. T6 will need the native device
    // / queue / swapchain pointers for create_effect_runtime; the accessor
    // for those is written with T6, not now.

    // Bridge thread only. Creates, on the T3 device: the command queue
    // (D3D12_COMMAND_LIST_TYPE_DIRECT), the DXGI swapchain against
    // `hwnd` (FLIP_DISCARD, R8G8B8A8_UNORM, 2 buffers, windowed), the
    // RTV heap, the command allocator, the command list, and the fence
    // + event. Queries the window's client rect itself (the T5 window is
    // non-resizable, so the size is fixed for the chain's lifetime). The
    // swapchain lands on the queue's adapter - the T3 device's adapter -
    // which is the entire mechanism that makes this a GPU 1 swapchain;
    // there is no adapter parameter to get wrong. Returns false with
    // everything released on any failure; every failure is logged with
    // the failing call and its HRESULT.
    bool create_present_chain(HWND hwnd);

    // Bridge thread only. One frame: PRESENT -> RENDER_TARGET barrier,
    // ClearRenderTargetView on the current backbuffer's RTV, the barrier
    // back to PRESENT, Close, ExecuteCommandLists, Present(1, 0) (vsync -
    // the loop's pacing), Signal, and a wait on the fence before
    // returning (the command allocator is single, so the next frame may
    // not Reset it until the GPU has finished this one). Returns false on
    // the first failure of any step; that failure is logged once (the
    // step, its HRESULT, and the removal reason) and later failures are
    // silent - the caller stops presenting after the first.
    bool present_frame(float r, float g, float b);

    // Any thread (takes this file's lock). True when the present chain
    // exists.
    bool has_present_chain();

    // ---- P1.0: the NGX probe ----
    //
    // Bridge thread only. Called exactly once, after create_present_chain()
    // succeeds and before the present loop starts - nothing is presenting
    // yet, so CreateFeature's ~1.16 s (measured on the reference run) stalls
    // nothing.
    //
    // Answers one question: does NGX initialise and create a feature on a
    // headless, non-game adapter? Everything downstream in this milestone
    // assumes it does, and nothing had tested it.
    //
    // Resolves the entry points by hand - no static library, no new link
    // library - from the driver's own _nvngx.dll (NGX Core) and, for names
    // the core does not export, from nvngx_dlssnr.dll (the DLSS-NR feature
    // snippet, which sits beside dxgi.dll). It then initialises NGX against
    // the GPU 1 device, takes the capability parameter map, creates
    // NVSDK_NGX_Feature_Reserved18 at the given size on a private command
    // list, and tears all of it down again. The device pointer stays inside
    // gpu1_context.cpp exactly as it does everywhere else - there is no
    // accessor, and the probe runs where the pointer already is.
    //
    // Every NGX call's numeric result is logged before the next is
    // attempted; every failure path releases what it created and returns
    // false. A false return never stops the bridge: the window, the present
    // loop and the teardown behave exactly as P0 shipped them. This is a
    // probe, not a dependency.
    bool ngx_probe(UINT width, UINT height);

// P1.3: does a buffer cross between the two adapters intact? Creates its
// OWN device on the game's adapter - the game's device is never touched -
// tries a shared cross-adapter buffer first and a host-pinned heap second,
// and verifies the payload byte for byte. Bridge thread only; safe to
// ignore the return value, the probe logs its own verdict.
// P1.3g: `tag` names the run in every log line it produces. "startup" is the
// automatic run that fires while shaders are still compiling; a manual run
// triggered by the hotkey passes its own label. The same launch can therefore
// contain a contaminated sample and a settled one, and the DIFFERENCE between
// them is the measurement of the contamination itself.
bool transit_probe(const char *tag = "startup");

// ---- P1.5: the host's real frame ----
//
// Everything before this transported a pattern we generated. P1.5 transports
// the game's finished colour buffer: 2560x1440 R10G10B10A2_UNORM on this rig,
// a resource we do not own, in a state we did not set.
//
// Three calls, in this order, and each is a no-op until the one before it has
// succeeded:
//
//   capture_on_finish_effects  the ReShade event. Called on the GAME's thread
//                              with the GAME's command list. Filters by adapter
//                              LUID - the bridge's own runtime raises this
//                              event too, and acting on it would capture our
//                              own window. First call allocates and arms;
//                              the second records the copies. One shot.
//   capture_poll               bridge thread, once per present. Does nothing
//                              until the capture has been recorded and the
//                              handoff is known to have completed, then reads,
//                              compares and reports.
//
// P2.0 CLOSES THE SYNCHRONISATION GAP. P1.5 inferred "the copy has completed"
// from frames elapsed, because we do not own the game's queue. P2.0 creates a
// fence with SHARED | SHARED_CROSS_ADAPTER on the game's device, opens the same
// fence on the bridge's device, and signals it on the game's own queue on the
// frame AFTER the copies were recorded - queue order then guarantees the signal
// lands behind them. capture_poll waits on that fence instead of counting.
// The frame counter is kept as a labelled fallback for the case where the
// shared fence cannot be created, and the sentinel fill stays in either mode:
// it is what turns "read too early" into a named diagnosis rather than a
// plausible wrong answer.
// Bridge thread. Until this is called the capture path is inert: the event
// handler returns immediately and the game's command list is never touched.
// Without it P1.5 would fire on the first two frames of the process and
// capture a loading screen, spending its one shot on a black frame.
void capture_request();
// P2.0 adds cmd_queue: the game's immediate command queue, as a native
// ID3D12CommandQueue*. It is the one object we need that the P1.5 signature
// did not carry - without it the shared fence can be created and opened but
// never signalled, and the wait would hang instead of measuring. It is passed
// as void* for the same reason as the others: this header names no ReShade and
// no D3D12 types.
void capture_on_finish_effects(void *runtime, void *cmd_list, void *cmd_queue,
                               unsigned long long rtv_handle);
void capture_poll();
}