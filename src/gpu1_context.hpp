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
    // P3.0: an externally supplied colour frame for ngx_probe.
    //
    // The pixels are CPU-side, tightly packed at `row_pitch` bytes per row, and
    // must already be in the format ngx_probe builds its colour texture with
    // (R8G8B8A8_UNORM today). `dxgi_format` is the ORIGINAL format the frame was
    // captured in and is carried for the log only - it is what says whether a
    // conversion happened on the way here, which is a real per-frame cost in any
    // production version of this path and must not become invisible.
    struct ngx_input_frame
    {
        const unsigned char *pixels = nullptr;
        UINT row_pitch = 0;
        unsigned dxgi_format = 0;

        // P3.1. false: `pixels` have already been converted to R8G8B8A8 and
        // ngx_probe builds its textures in that format - the P3.0 behaviour.
        // true: `pixels` are the frame's ORIGINAL bytes and ngx_probe builds
        // its colour and output textures in `dxgi_format` instead, asking the
        // question P3.0 deliberately left open - can DLSS-NR consume the
        // game's buffer as it is rendered, with no conversion stage at all?
        //
        // A yes deletes a full-resolution CPU pass from every frame of any
        // production version of this path. A no is worth having in writing
        // too, because it makes the conversion a permanent structural cost to
        // be budgeted on the GPU rather than wished away.
        bool native_format = false;
    };

    // ext == nullptr is the P1 behaviour, unchanged: NR runs on a generated
    // pattern. ext != nullptr is P3.0: NR runs on the frame supplied, which is
    // the game's own, and the P1.4 transit block is skipped because its control
    // belongs to the synthetic path.
    bool ngx_probe(UINT width, UINT height, const ngx_input_frame *ext = nullptr);

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

// ---- P4.0: the stream ----
//
// The first stage that RUNS rather than probes: every game frame, sealed and
// transited into a ring of slots, until a self-imposed bound.
//
// A stream is where the QUIET failures in P1_INSTRUMENT section 00 live - torn,
// stale, dropped, duplicated, reordered, slot-aliased. None of them is
// reachable by a one-shot probe and none is visible to a person watching the
// window; a stream that consistently delivers frame N-4 looks perfect on static
// content. The 64-byte seal carried in each slot is what makes them nameable,
// and section 06 committed to shipping it with the first task that transits a
// stream.
//
// Scope, so the log is not over-read: the seal proves IDENTITY, ORDER and AGE.
// It does NOT verify pixels per frame (P1.5 established the payload crosses
// byte-exact, and re-proving it per frame would measure the instrument), and
// the barcode field is written as 0 and left UNCHECKED because the shader that
// would make it an independent check does not exist yet. The neural stage is
// deliberately not attached: if both landed in one commit, a failure would not
// say which half.

// P5.2. Any thread. True only when mgpu.ini says Probes=1.
//
// DEFECT C: the one-shot probe chain (P1.3 transit, P1.5 capture and the P3.x
// ngx_probe it leads into) and the P4.1 stream both armed from the same hotkey,
// which put TWO independent NGX consumers on ONE shared parameter block -
// GetCapabilityParameters returns the core's block, not a per-caller one. The
// probe's teardown then destroyed it under the running stream. The visible
// symptom was a neural image with the colours wrong while every transport
// counter stayed clean, which is precisely the failure shape the seal cannot
// see: the bytes arrived, the consumer was broken.
//
// The destroy is now suppressed while the stream holds the block, and the
// probes themselves are opt-in and default OFF - they are answered questions,
// and re-running them under a live stream can only cost. Set Probes=1 in
// mgpu.ini to run the old chain again, with the stream deliberately not armed.
bool probes_enabled();

// ---- P6.4: what the overlay panel reads and writes ----
//
// Plain scalars on purpose. This header names no ReShade type and no ImGui
// type, and the panel that drives these lives in dllmain.cpp where those
// headers already are - the same separation that has kept gpu1_context free of
// ReShade since T3.
//
// ANY THREAD: the overlay callback runs on whichever thread presents the
// runtime it belongs to, not the bridge thread, so these take the stream's lock
// internally. They are tiny and never block - a UI callback that can stall is a
// UI callback that can stall a present.
struct ui_state
{
    bool armed = false, summarised = false, neural = false, nr_ok = false;
    bool profile = false;
    // P7.4: 0 = neural output, 1 = the frame handed TO the model, 2 = split
    // (left half input, right half output, the same frame).
    int present_mode = 0;
    // P7.4: 0 = manual, 1 = front-loaded, 2 = back-loaded. See ui_set_preset.
    int preset = 0;
    // P7.5: split seam position, 0.0 (all output) .. 1.0 (all input).
    float split_pos = 0.5f;
    unsigned passes = 1, max_passes = 6;
    float intensity[6] = {};
    unsigned long long consumed = 0, produced = 0, dropped = 0, overrun = 0, skipped = 0;
};
void ui_read(ui_state &out);

// Pass count, live. Every NGX feature handle is created at arm time, so this is
// only a count change - nothing is created or destroyed, and it takes effect on
// the next consumed frame. Clamped to 1..max_passes.
void ui_set_passes(unsigned n);

// pass_1based == 0 sets every pass; otherwise that one. Clamped 0.0..2.0.
void ui_set_intensity(unsigned pass_1based, float v);

// Turn the neural stage off without tearing it down - the handles stay alive so
// it can come back without a 400 ms CreateFeature stall.
void ui_set_neural(bool on);

// P7.4. The present mode, live: 0 neural output, 1 the frame handed TO the
// model, 2 SPLIT - both halves of the SAME frame side by side, input left,
// output right, with a white seam between them.
//
// Split is the only way to compare input against output in a game: two runs
// never contain the same frame, and an exterior changes underneath you, so a
// difference between two captures can never be attributed cleanly. It changes
// nothing about the neural stage or its timing - only the copy into the
// bridge's backbuffer.
void ui_set_present_mode(int mode);

// P7.4. The intensity SHAPE, held as a mode rather than written once:
//   0 manual  - per-pass values as they are, nothing rewritten
//   1 front   - pass 1 at 2.00, every other pass at 0.10
//   2 back    - the LAST active pass at 2.00, every other pass at 0.10
//
// The shape FOLLOWS the pass count. Raising the count moves the peak with it,
// which is what keeps front and back a single clean variable while the count
// is changing live on camera. Any manual slider or hotkey step returns the
// mode to manual, so a hand-edited run is never labelled as a preset.
void ui_set_preset(int mode);

// P7.5. Move the split seam one step. dir is -1 or +1; coarse takes a tenth of
// the frame instead of a fortieth.
//
// A hotkey rather than only a slider, on purpose: the panel is the ReShade
// overlay, and an open overlay is the one thing that cannot be on screen while
// the seam is dragged across a face for the camera. This has to work with
// nothing visible but the game.
void ui_split_move(int dir, bool coarse);

// P7.5. Absolute seam position for the panel slider, 0.0 .. 1.0.
void ui_set_split_pos(float v);

// P6.3. BRIDGE THREAD ONLY - both of these are called from the hotkey handler
// in the message pump, which runs on the bridge thread, and they touch state
// that only the bridge thread reads. Do not call them from anywhere else.
//
// Intensity used to be read from mgpu.ini once at arm time, so finding a value
// cost one game launch per value. NGX parameters are live per evaluate (P1.2)
// and the pass loop sets them every frame, so a change here takes effect on the
// next frame with no re-arm and no relaunch.
//
// intensity_cycle_target picks what the steps act on: all passes, or one of
// them. intensity_step moves it by one increment, clamped, and logs the whole
// per-pass ladder each time so the log says what was on screen when.
//
// A run whose intensity was edited mid-stream says so in its own summary: the
// frames it covers were not all produced at the same strength, and its timings
// must not be quoted as a figure for any single value.
void intensity_cycle_target();
void intensity_step(int dir);

// Bridge thread. Arms the stream; inert until called, one stream per process.
// Reads Fault= from mgpu.ini beside the add-on - absent means no fault, so the
// shipped default is a clean run and a missing file is never an error.
void stream_request();

// GAME thread, every frame, with the game's command list open. Filters by
// adapter LUID; signals the previous frame's fence value before recording the
// current one, because ReShade executes our list after this returns.
void stream_on_finish_effects(void *cmd_list, void *cmd_queue,
                              unsigned long long rtv_handle);

// Bridge thread, once per present. Consumes whatever the fence says has
// arrived, checks each seal, and prints the summary once the producer has
// stopped and drained.
void stream_poll();

// P5.1. Bridge thread, called immediately before present_frame. Returns true
// when the bridge should put a frame on screen.
//
// While the stream is running with an on-screen output, that is once per NEW
// neural frame rather than once per vsync - which removes three quarters of the
// full-frame backbuffer copies and three quarters of the DWM cross-adapter
// copies of the bridge window, both of which were competing with the payload
// for the same link. When there is nothing new it blocks on the shared fence
// (outside the stream's lock) for up to `timeout_ms`, so the consumer wakes on
// a frame landing rather than on a vblank, and its cadence stops depending on
// which display GPU 1 is attached to.
//
// Returns true unconditionally when the stream is idle or in profile mode, so
// the cycling clear colour - T5's liveness proof - keeps running as it always
// has.
bool stream_present_gate(unsigned long timeout_ms);
}