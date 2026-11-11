#pragma once

// MGPU Bridge - XFG: XeSS Frame Generation on the second card (experimental, off by default)
//
// WHAT THIS IS FOR
//
// The bridge's terminal stage is DLSS Neural Rendering, run on GPU 1. The output
// of that stage is one presented frame per game frame, so the frame rate the
// player sees is capped by what GPU 1's NR pass can do. This module adds the
// next stage after NR: Intel XeSS Frame Generation, also on GPU 1, turning each
// pair of neural-rendered frames into a real + interpolated pair.
//
// THE ORDER MATTERS. NR runs on the game's real frames only - it never sees an
// interpolated frame - so the neural style is applied once per rendered frame
// and the interpolation just carries it forward. GPU 1's NR load is unchanged;
// only the present count doubles.
//
// THIS BUILD IS THE FIRST GATE, NOT THE WHOLE PATH.
//
// arm() does everything up to but NOT including the swapchain handover: it loads
// the two Intel libraries, creates the XeFG context against GPU 1's device,
// queries the properties (descriptor count, heap sizes, maximum interpolations)
// and builds the pipelines. That answers the one question the whole plan rests
// on - does XeSS-FG accept a Blackwell device that is driving the display but
// rendering nothing of the game itself - and it prints the numbers the next
// step needs to size the descriptor heap and the two temp heaps.
//
// Nothing is presented through the proxy in this gate. The bridge's own present
// path is untouched, so a build with XeFG=1 behaves exactly as before on screen.
//
// THE CONTRACT
//
//   - OPTIONAL MODULE, keys in mgpu.ini, read at arm:
//       XeFG          absent or 0: off. One file read at arm, then nothing.
//                     1: on - the gate runs.
//       XeFGFrames    interpolated frames per game frame, 1..7, default 1 (=2x).
//                     Read at arm; the gate only reports whether XeFG can supply
//                     that many, it does not yet produce them.
//       XeFGInverted  absent or 0: depth is 0..1 near-to-far. 1: the
//                     INVERTED_DEPTH flag is passed to pipeline build and init.
//   - OFF COSTS: one file read at arm. No LoadLibrary, no device query, no GPU
//     work, no memory.
//   - ANY failure in the gate is permanent for the arm: the module logs the
//     exact stage and the exact return code, releases what it made, and stays
//     off. The stream keeps running without frame generation.
//   - Runs on the bridge thread only (T3), like nr16 and residual: it is handed
//     GPU 1's device and holds no gpu1_context type.
//
// The Intel libraries are NOT part of this build. They are loaded at run time
// from <add-on directory>\mgpu\, beside nvngx_dlssnr.dll, and the user supplies
// them (XeSS SDK 3.0.2 or later: libxess_fg.dll and libxell.dll).

#include <cstdint>    // last_tag_id()'s uint32_t: the header must not lean on
                      // whichever include order the .cpp happens to use.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>   // phase 2: the handover interface names IDXGISwapChain

namespace mgpu::xess_fg
{
// At arm, after GPU 1's device and the stream's textures exist. Reads the keys;
// when on, loads the libraries, creates the context, queries properties and
// builds the pipelines. False when off or when any stage failed - and once it
// has returned false for a reason, it stays off for the arm.
bool arm(ID3D12Device *dev, unsigned width, unsigned height);

// One relaxed load. Off: nothing in this module runs per frame.
bool active();

// True once after a successful gate: the panel's state line reads it to say
// "XeFG gate PASSED" rather than "off".
bool passed();

// What the gate learned, for the panel and the log. Zero when off.
void status(bool &on, unsigned &frames, unsigned &inverted,
            unsigned long long &max_interp, unsigned long long &desc_count,
            unsigned long long &buf_heap, unsigned long long &tex_heap);

// From stream_release, when GPU 1 no longer uses anything.
void release();

// ---- phase 2: the swapchain handover (XeFGHandover=1, off unless set) ----
//
// True when the key is on AND the gate passed AND every handover entry the
// proxy needs resolved. The bridge asks this before it offers its chain.
bool handover_enabled();

// handover_arm takes OWNERSHIP of app_chain: Intel's contract is that the
// proxy consumes and destroys it, and the caller must never touch it again -
// it gets its own chain back through out_chain3 (a fresh reference the caller
// releases when the chain dies). Returns false and changes nothing when the
// gate did not pass, the key is off, or any Intel call fails; the caller then
// keeps presenting through its own chain exactly as before. On false the
// app_chain reference is NOT consumed and stays the caller's.
bool handover_arm(ID3D12CommandQueue *present_queue, IDXGISwapChain *app_chain,
                  unsigned width, unsigned height, DXGI_FORMAT backbuffer_format,
                  IDXGISwapChain3 **out_chain3, bool enable_generation = true);

bool handover_live();

// The one handover failure that cannot fall back to 1x: InitFromSwapChain
// consumed the app chain and GetSwapChainPtr returned nothing, so the bridge
// has no chain to present on. The caller stops presenting instead of touching
// a chain Intel already owns.
bool handover_lost();

// PRESENT thread, on the present list, after the neural frame has been copied
// into the backbuffer and BEFORE the list is closed and submitted: tag the
// colour/depth/mvec the frame-gen library must read, plus the camera matrices
// from cam_track. validity is ONLY_NOW with pCmdList = cl, so the library
// records its copies on that list - the list executed immediately before
// Present, which is the order Intel's contract requires. The consumer's fence
// was waited on the same thread one step earlier, so depth and mvec are final
// and at rest; no resource is borrowed live across threads.
// False when nothing is live or a tag failed; the frame then simply presents
// 1x (the proxy passes untagged present-ids straight through).
bool tag_frame(ID3D12GraphicsCommandList *cl, unsigned long long frame,
               ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
               unsigned color_w, unsigned color_h,
               ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
               unsigned depth_w, unsigned depth_h,
               ID3D12Resource *mvec, D3D12_RESOURCE_STATES mvec_state,
               unsigned mvec_w, unsigned mvec_h,
               const float view[16], const float proj[16],
               bool reset_history);

// Present thread, immediately before Present: which tagged frame this
// present shows. False = not live (the caller presents normally).
// The id is the interface's OWN consecutive sequence (tag_frame spends it),
// not the seal number - Intel answers FRAMES_ID_MISMATCH to gaps, and the
// consumer skips frames under backpressure. The caller passes last_tag_id()
// after a tag_frame that returned true.
bool set_present_id(unsigned long long frame);

// The id the last successful tag_frame spent (0 before the first). Present
// thread only, same thread as tag_frame and set_present_id.
uint32_t last_tag_id();

// Present thread, after Present: what the library did. frames_presented is 2
// when an interpolated frame went out, 1 when the frame passed through.
bool last_present_status(unsigned &frames_presented, bool &frame_gen_enabled);

// Unwind the handover: destroy the proxy (which releases the app chain it
// owns). The caller must have dropped its proxy reference first. Safe twice.
void handover_disarm();
}
