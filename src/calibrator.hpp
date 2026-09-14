// ---------------------------------------------------------------------------
// calibrator.hpp - R101. THE CALIBRATOR.
//
// WHAT THIS IS FOR, in one paragraph, because the reason matters more than the
// mechanism. Every acquisition round from R70 to R100 asked the same question -
// "which of these resources is the velocity buffer, and what units is it in" -
// and answered it by inference: RTV bind counts, SRV creation counts, barrier
// counts, size filters, uniform-fraction rules, stability gates. Four of those
// rules shipped and three broke a working title on contact. Plague Tale's 67%
// is the current bill for that approach. The game already knows the answer. It
// hands the answer to NVIDIA, by name, every single frame, in a parameter block
// with a documented vtable. This file reads that block.
//
// THE COMMON DENOMINATOR, and it survived a scan that could have killed it.
// Dragon Sword links the NGX SDK statically and resolves
// NVSDK_NGX_D3D12_EvaluateFeature out of the driver's _nvngx.dll by name.
// Plague Tale does not call NGX at all - it goes through Streamline, and
// sl.common.dll is what carries the NGX symbols. Two different producers. But
// Streamline does not reimplement DLSS; sl.dlss.dll calls the same driver
// export. So ONE hook, on the driver-side EvaluateFeature, sees both. That is
// the whole reason this is worth building rather than writing two readers.
//
// NO THIRD-PARTY CODE. An earlier note in this session said MinHook. That was
// wrong and worse than what is here: both routes resolve through
// GetProcAddress, so the interception point is an IMPORT TABLE ENTRY, which is
// a pointer swap. No trampoline, no length disassembler, nothing vendored, and
// nothing patched in the middle of anyone's code. Disabling is the same pointer
// put back.
//
// THIS FILE PUBLISHES; IT DOES NOT YET STEER. read() hands out a table and
// nothing in the bridge is required to believe it. That is deliberate: Dragon
// Sword is a title we already drive correctly at 99.67% alignment, so it is the
// bound match. Run it with the tap live, diff the table against what we feed
// today, and either the reader is proven or the difference IS the drift table.
// Only after that does MvecSource=ngx switch acquisition over. A reader that
// cannot break a working title is a reader we can afford to be wrong about
// once.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

namespace mgpu
{
namespace calibrator
{

// ---- WHAT THE GAME TOLD NVIDIA, for one frame ----
//
// Every field here is READ, never inferred. A field the game did not set reads
// back as its "absent" value below and the *_ok bits say which is which - a
// zero MV scale is a real answer in some engines and a missing key in others,
// and the difference is exactly the kind of thing this project has been burned
// by. Resources are held as unsigned long long, not ID3D12Resource*, for the
// same reason the seal does: this struct crosses threads and is never used to
// call a method, only to compare identity.
struct table
{
    // The resources, by identity. THESE ARE THE POINT OF THE FILE.
    unsigned long long color;
    unsigned long long depth;
    unsigned long long mvec;
    unsigned long long output;

    // Motion vector units. The question R83 through R100 kept guessing at.
    float mv_scale_x;
    float mv_scale_y;

    // Sub-pixel jitter. DLSS-NR has no jitter parameter of its own - this is
    // recorded because it tells us whether the colour we are denoising is
    // jittered, which is a fact about the input we have never actually had.
    float jitter_x;
    float jitter_y;

    // ---- THE CREATE FLAGS WORD, and it is worth more than the rest ----
    //
    // R101a. The first draft read a key called DLSS.Depth.Inverted. There is
    // no such key. Depth inversion is bit 3 of DLSS.Feature.Create.Flags, set
    // ONCE at CreateFeature, and reading it as a per-frame parameter would
    // have reported "this title does not set it" forever.
    //
    // Checking that against the pinned header paid for itself twice, because
    // the same word also carries MVLowRes - whether the game's motion vectors
    // are at RENDER resolution or DISPLAY resolution. That is the question
    // deferred option 11 was opened for, and the game has been answering it
    // every run.
    unsigned int create_flags;
    unsigned int depth_inverted;   // flags bit 3
    unsigned int mv_low_res;       // flags bit 1  <- deferred option 11
    unsigned int mv_jittered;      // flags bit 2
    unsigned int is_hdr;           // flags bit 0
    unsigned int auto_exposure;    // flags bit 6

    // The quality mode, so a preset swap mid-run is visible rather than
    // inferred from the extents changing.
    unsigned int perf_quality;

    // Dynamic resolution bounds, when the title uses them. A velocity buffer
    // whose size moves between frames is what the transport's size guard
    // refuses, so knowing the band is knowing whether that guard can fire.
    unsigned int dyn_min_w, dyn_min_h;
    unsigned int dyn_max_w, dyn_max_h;


    // The game's own history reset. When this is 1 the game is telling NGX its
    // temporal history is invalid - a cut, a teleport, a resolution change.
    unsigned int reset;

    // Extents. render_* is what the game rendered at; display_* is what it
    // asked DLSS to produce. Their ratio is the render scale, measured.
    unsigned int render_w, render_h;
    unsigned int display_w, display_h;

    // Subrects. A velocity buffer allocated at display size but written only
    // in a render-size corner is the shape that makes a whole-surface copy
    // look like garbage, and it is invisible from a resource description.
    unsigned int color_x, color_y;
    unsigned int depth_x, depth_y;
    unsigned int mvec_x, mvec_y;
    unsigned int out_x, out_y;
    unsigned int sub_w, sub_h;

    // Which keys actually came back. Bit per group, see KEY_* below.
    unsigned int have;

    // Bookkeeping. frame is OUR frame counter at capture; evals is how many
    // times the game has called EvaluateFeature on the tracked feature.
    unsigned long long frame;
    unsigned long long evals;
};

// have bits. A key group that never appears in a title is a finding about that
// title, not a bug, and the log prints these by name.
enum
{
    KEY_COLOR      = 1u << 0,
    KEY_DEPTH      = 1u << 1,
    KEY_MVEC       = 1u << 2,
    KEY_OUTPUT     = 1u << 3,
    KEY_MV_SCALE   = 1u << 4,
    KEY_JITTER     = 1u << 5,
    KEY_FLAGS      = 1u << 6,
    KEY_RESET      = 1u << 7,
    KEY_RENDER_EXT = 1u << 8,
    KEY_DISPLAY_EXT= 1u << 9,
    KEY_SUBRECTS   = 1u << 10,
    KEY_QUALITY    = 1u << 11,
    KEY_DYNAMIC    = 1u << 12
};

// R110. THE ARGUMENT IS PACKED. Low byte is the capture mode, exactly as it
// has always been; bits 8-11 are the install rung. mgpu::probe::calib_mode()
// builds it and this is its only consumer, so dllmain's one install() line
// needs no change and cannot disagree with the ini.
//
// LOW BYTE - Calib=, what the tap does once NGX is running:
//       0 = never install (no bytes touched, no imports walked)
//       1 = latch. Capture, then stop reading once the table is stable.
//       2 = live. Read every evaluate, all run. The default, because a
//           resolution or preset change replaces the resources and a latched
//           handle is a dead pointer - which is the failure mode this whole
//           file exists to remove.
//
// BITS 12-15 - CalibProbe=, how much the calibrator says about the words its
// data scan wrote. 0 nothing beyond the count; 1 names each hit and reads the
// same offset out of the module's own FILE, so a runtime cache can be told
// from static data that merely matched; 2 also loads a private copy of that
// module under mgpu\ and writes a sentinel into the same RVA. Diagnostic
// only - it never changes what is patched in the live process.
//
// BITS 8-11 - CalibRung=, which install rung is allowed to run at all:
//       0 = both, in the existing order. The default and the 0.2.1 behaviour.
//       1 = the import-table swap alone (R101).
//       2 = the data-section scan alone (R102).
//
// The rung exists because Calib never selected anything here. Install ran
// both rungs for Calib=1 and Calib=2 alike, so the two modes installed
// identically and a crash inside install could not be attributed. Running one
// rung per launch attributes it in two runs.
//
// Safe to call before any NGX module is loaded: if none is present the import
// hook stays armed and picks it up when it arrives.
void install(int packed);

// Puts every patched import entry back. Idempotent. After this returns no
// pointer in the process refers to anything in this file. Not gated on the
// rung: the reverse of a rung that never ran finds nothing to put back, and
// an unwind that can be skipped is worse than one that does nothing.
void uninstall();

// Lock-free snapshot. Returns false if nothing has ever been captured. Never
// blocks and never allocates - it is called from finish-effects, on the frame
// path, and DEFECT E is the standing lesson about waits on that thread.
bool read(table &out);

// Called once per frame from the same place the probe counts frames, so the
// tap's frame column lines up with every other counter in the log. It also
// owns the periodic R101 report and the rescan for late-loading modules, so
// dllmain needs exactly two lines total: install() and this.
void note_frame();

// Writes the R101 line: what was hooked, what was read, what was missing, and
// the measured per-frame cost in nanoseconds. Called on the same cadence as
// the other periodic reports.
void log_summary();

// ---- R104: JITTER COMPENSATION ----
//
// The colour we denoise is jittered - the table reads a live Halton offset on
// every title measured - but MVJittered=0, so the vectors do not carry it and
// NR reprojects by a slightly wrong amount. Worst on fine detail, which is the
// skin artefact.
//
// apply_jitter_offset is called from the per-frame parameter set, and is given
// the MVecScale in force because THE UNITS ARE THE WHOLE TRICK: the jitter is
// in render pixels, the vectors are pixels on one title and normalised UV on
// another, so a raw offset would be ~1700x too large on the second and would
// read as a wrong sign when it is a wrong unit.
//
// It is inert when the mode is 0, when the table has no jitter, or when the
// game already baked it in (MVJittered=1).
// ---- R105: THE LAST HAND-COPIED NUMBER ----
//
// MVecScale was still coming from the ini, typed in by hand after reading it
// out of the R101 line. The table has it every frame. This overwrites the two
// floats in place when the game has actually told us, and leaves them alone
// otherwise - so a title where the calibrator never resolved keeps the ini
// value with no special case, and an ini value stays the manual override for
// anyone who needs one.
//
// SAME BUFFER, SAME UNITS: the scale the game hands DLSS for this resource is
// the scale NR needs for the same resource. Verified on both titles - Dragon
// Sword 1.0/1.0 matched the ini exactly, Plague 1707/960 explained a visual
// fault the ini's 1.0 was causing.
void override_scale(float &sx, float &sy);

// ---- R106: COPY FROM THE EVALUATE CALL, NOT FROM A BARRIER ----
//
// THE PROBLEM IT SOLVES. The copy has always been triggered by the engine's
// unordered_access -> shader_resource barrier. On Plague a third of frames
// never emit one, so a third of frames ship with no vectors - and knowing the
// right handle does not help, because we know exactly which buffer to read and
// are simply never told it is readable. Dragon Sword loses 5% the same way.
//
// EvaluateFeature does not have that problem: the game hands DLSS that exact
// resource, once per frame, every frame, on a live command list, and it has
// finished writing it because DLSS is about to READ it.
//
// THE ONE ASSUMPTION, stated because it is the risk. We do not know the
// resource's state here the way the barrier hook does - it comes with the
// event. We assume ALL_SHADER_RESOURCE, which is what DLSS needs to read it
// and is what R82 measured the engines transitioning it to (0x8 -> 0xC0). If
// that assumption is wrong the debug layer will say so. Issuing a redundant
// transition is what crashed R85, which is why this is OFF by default and
// behind its own key: MvecFromEval=1.
void set_mvec_hook(void (*fn)(void *cmd_list, unsigned long long handle));
void set_eval_copy(int mode);

// R118. What the ini asked for: 0 off, 1 on, 2 AUTO. Read by gpu1_context so
// the auto-fallback can tell "the user wants auto" from "the user said on",
// without a second parse of mgpu.ini.
int eval_copy_mode();

void set_jitter_mode(int mode);
void apply_jitter_offset(void *nr_params, float mvec_scale_x, float mvec_scale_y);

} // namespace calibrator
} // namespace mgpu
