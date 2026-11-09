#pragma once

// MGPU Bridge - R316 residual pass (private test build, off by default)
//
// WHAT THIS IS FOR
//
// A second, separate implementation of the residual idea (run DLSS-NR at a
// smaller extent, add its difference back onto a full-size anchor), written to
// fit this add-on's pass chain instead of the game-side proxy it came from.
// It is NOT the NRScale path (R299, N01950's; that path and its keys are left
// exactly as they are): this module has its own keys, its own scratch, its own
// kernels, its own log tag, and it can carry the residual on PASS 1 or on PASS 2.
//
//   pass 1 carries it:  tex_in --down--> small_in; NR(small_in -> small_out);
//                       tex_out = tex_in + up(small_out - small_in)
//   pass 2 carries it:  pass 1 runs at native as today; then
//                       tex_out --down--> small_in; NR(small_in -> small_out);
//                       tex_pong = tex_out + up(small_out - small_in)
//
// THE CONTRACT
//
//   - OPTIONAL MODULE, keys in mgpu.ini, read at arm:
//       ResidualPass        1 or 2: which pass carries the residual. Absent or
//                           any other value: this module does not exist - no
//                           key read beyond this one, no texture, no kernel,
//                           no extra pass, no log line.
//       ResidualScale       0.30-1.00, default 0.75: the work extent as a
//                           fraction of the native size (X and Y).
//       ResidualRender      1: the work extent is the game's RENDER size from
//                           the NGX table instead (R318). When the table has
//                           no render size at arm, ResidualScale is used and
//                           one line says so.
//       ResidualDepth       1: the resolve's silhouette guard runs, reading
//                           depth at ANY size (the taps are scaled; render-size
//                           depth from DepthTransport=3 works). Absent: the
//                           resolve gets no depth - the state both reporters
//                           preferred on the R299 path (Starfield, Cyberpunk).
//       ResidualGain        0.0-2.0, default 1.0 (the model's contribution).
//       ResidualSharpness   0.0-1.0, default 0.0 (RCAS on the final image; the
//                           anchor on pass 2 is already denoised, so off).
//       ResidualColor       0.0-1.0, default 0.90 (chroma vs luma of the edit).
//       ResidualMode        1 (default): native anchor + scaled delta;
//                           0: the model output itself, upsampled.
//   - If NRScale is present in the ini (R299 on), this module stays OFF and
//     says so once. One residual path at a time; theirs wins.
//   - R319: arms whatever the live Passes value is (x1 / x2 is switched live from
//     the panel); the carrying pass's feature is created at the work extent at
//     arm, and the residual runs on every evaluate of that pass. With x1 the
//     feature exists and is not evaluated, as the native pass-2 feature is today.
//   - R316a: the frame shape is checked ONCE, at arm. The carrying pass's
//     feature is created at the work extent, so the module only turns on when
//     every frame of the run can take the residual: SRUpscale off, Subrect=100,
//     CostCurve off, no ScaleMode, no Pass1Fovea / Pass2Region / Pass2Fovea, and
//     (ResidualPass=1 only) SrgbInput off. Any of those set: OFF, one line naming
//     it, and every feature is created at its usual size. The per-evaluate gate
//     stays as a second guard.
//   - R332 (0.5.0): the module is PER PASS. Keys, read at arm:
//       NRS                 absent or 0: everything above, unchanged (R316).
//                           1: PASS 1 carries our residual (the 0.5.0 path). The
//                           module exists with NRS=1 even when ResidualPass is
//                           absent. The sRGB refusal above becomes a path: NR
//                           reads our linear-typed scratch and one line says the
//                           P7.8b free-decode experiment does not apply to pass 1.
//       Pass2               with NRS=1 only. full: pass 2 (at x2) is a full NR
//                           pass. nrs: pass 2 carries our residual too, anchored
//                           on pass 1's resolved native output. Absent: nrs when
//                           ResidualPass=2 (the panel's Medium / Light keep their
//                           meaning for the 2nd pass), full otherwise.
//       Pass3 / Pass4       (R350, Experimental) with Passes=3 / 4, any pass-1 setting
//                           (R366): full | nrs for that pass, as Pass2. Absent: full. The
//                           capacity is 4 sets; a set is allocated only for a pass
//                           that carries AND is built, so Passes 1 / 2 cost what
//                           they did.
//     Each carrying pass owns its scratch pair, work extent (one recipe for both),
//     tracked states and counters; kernels, root signature and heap are shared.
//     "One residual path at a time" stands: NRScale set = NRScale wins.
//   - OFF COSTS: one file read at arm, then one relaxed load per evaluate.
//   - Holds no gpu1_context type: gpu1_context hands it D3D12 objects, the
//     NGX parameter block and plain values. Everything runs on the bridge
//     thread, inside the stream's command list, like nr16.
//   - Shaders: adapted from xenmods' DLSSNR-Cost-Scaler (MIT, THIRD_PARTY.md),
//     compiled at arm with d3dcompiler_47 exactly as nr16 compiles its kernels.
//     A compile or allocation failure leaves the module OFF and never fails
//     the arm.

#include <windows.h>
#include <d3d12.h>

struct NVSDK_NGX_Parameter;

namespace mgpu::residual
{
// At arm, after the stream's textures exist and after the R299 block has run.
// `nrscale_on` is R299's verdict (true = their path is on, ours stays off).
// `passes` is the number of pass features the arm creates (R319), not live Passes.
// `render_w/h` are the table's render extent or 0 when unknown. Reads the keys;
// when on, builds the kernels and the scratch pair. False (and off) otherwise.
// R316a: `shape_refusal` names the first arm-time setting the residual cannot run
// with (nullptr = none); `srgb_in` is SrgbInput=1 (refused for pass 1 only).
bool arm(ID3D12Device *dev, unsigned width, unsigned height, DXGI_FORMAT fmt,
         unsigned passes, bool nrscale_on, unsigned render_w, unsigned render_h,
         const char *shape_refusal, bool srgb_in);

// One relaxed load.
bool active();

// R332: true when 0-based pass `pi` carries the residual and is live (one array read).
bool carries(unsigned pi);

// R332: true when the NRS key is 1 (pass 1 carries; read at arm).
bool nrs_on();

// 0-based index of the LOWEST pass that carries the residual (valid when active()).
// R316 callers; with NRS absent it is the ResidualPass pass, as before.
unsigned pass();

// The work extent (valid when active()). The feature of a carrying pass is
// created at this size; every other pass keeps its own size. The pass form (R332)
// leaves w/h untouched when `pi` does not carry.
void work_extent(unsigned &w, unsigned &h);
void work_extent(unsigned pi, unsigned &w, unsigned &h);

// Before CreateFeature: binds Color/Output and the subrects to the scratch pair,
// as the proxy does at create time. The caller sets Width/Height per pass. The
// pass form (R332) binds that pass's own pair; no-op when `pi` does not carry.
void bind_create(NVSDK_NGX_Parameter *p);
void bind_create(NVSDK_NGX_Parameter *p, unsigned pi);

// Before the evaluate of carrying pass `pi`. `anchor` is the pass's input, in
// NON_PIXEL_SHADER_RESOURCE. Records the downsample on `cl`, binds the scratch
// pair, the work extent and the scaled vector scale on `p`. `parity` picks the
// descriptor set.
void before(ID3D12GraphicsCommandList *cl, unsigned pi, unsigned parity, NVSDK_NGX_Parameter *p,
            ID3D12Resource *anchor);

// After the evaluate. `dst` is the pass's output, in UNORDERED_ACCESS. Records
// the resolve into `dst` (or skips it when the evaluate failed) and puts the
// caller's bindings back: Color/Output, Width/Height, the subrects
// (base/size as the caller had them) and the vector scale.
void after(ID3D12GraphicsCommandList *cl, unsigned pi, unsigned parity, NVSDK_NGX_Parameter *p,
           ID3D12Resource *anchor, ID3D12Resource *dst, bool evaluate_ok,
           ID3D12Resource *nr_color, ID3D12Resource *nr_output,
           unsigned native_w, unsigned native_h,
           unsigned rbx, unsigned rby, unsigned rw, unsigned rh);

// From stream_release, when the GPU no longer uses the stream's resources.
void release();

// R364: live Residual (on) / Native (off) for one pass, on the bridge thread between frames; the caller then
// recreates that pass's feature at the new extent. `line` receives the one log line to print (info on success,
// warn on refusal). Returns false and leaves the pass as it was when the switch is refused.
bool set_pass_live(unsigned pi, bool on, char *line, size_t cap);
}
