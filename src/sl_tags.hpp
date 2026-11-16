// MGPU Bridge - SLT1: the Streamline tag tap.
//
// WHAT IT IS FOR. SL2a measured this title's Streamline surface and got
// ROUTE=TAGGED, slSetTagForFrame, 18 of 18 documented entry points exported.
// That means the game declares its own depth, motion vectors, colour and UI
// to Streamline inside this process, with the native resource pointer, the
// format and the extent attached. That declaration is AUTHORITATIVE. Our
// ranking is not: on Battlefield 6 the lateral probe found 15 motion-vector
// candidates that were identical in size, format, SRV count and bind count,
// so a raw barrier tiebreak decided which one the transport used, and the
// chosen source was observed changing between reports inside one run.
//
// This file reads the declaration instead of ranking guesses.
//
// ---- WHERE IT SITS, AND WHY THAT IS THE SAFE PLACE ----
//
// The tap is an IAT swap: the game's import slot for slSetTagForFrame is
// pointed at a thunk here, which reads three fields and then calls the real
// function with the arguments untouched.
//
// THE THUNK RUNS ON THE GAME'S OWN CALLING THREAD, SYNCHRONOUSLY, BEFORE THE
// CALL ENTERS sl.interposer AT ALL. There is no second thread, no re-entry
// into Streamline, no lock taken and nothing held: for a few dozen
// nanoseconds we ARE the game's thread, standing outside every Streamline
// lock, reading a struct the game has finished building and has not yet
// handed over. Nothing here can race the thing it is observing, because it
// runs in the gap before the thing happens.
//
// That is worth stating against the rung it should be compared to. R102, the
// calibrator's data-section scan, writes into other modules' memory while
// those modules may be executing. This writes one import slot once, and then
// only ever reads.
//
// ---- THE ABI, AND WHAT WAS NOT ASSUMED ----
//
// Every offset below comes from the public Streamline headers, read rather
// than recalled. That distinction already paid: sl::BaseStructure is `next`
// FIRST, then structType, then a size_t structVersion. Written from memory it
// would have been structType first, every offset after it would have been
// wrong, and reading a tag would have meant dereferencing a field that is not
// a pointer - which is precisely the class of fault this whole round exists
// to avoid causing.
//
// ONE THING IS STILL NOT PINNED: sizeof(sl::Extent), the trailing member of
// ResourceTag, and therefore the stride of the tag ARRAY. It is not assumed.
// Every ResourceTag begins with its own structType GUID, so the stride is
// MEASURED from the data - scan forward for the next occurrence of that GUID
// and the delta is the stride - and every element is validated against the
// GUID before a single field of it is read. A stride that cannot be measured
// means one tag is read and the rest are skipped, which costs information and
// cannot cost stability.
//
// ---- DEFERRED ON PURPOSE ----
//
// The reporter's rig still faults where ours stopped faulting, and the most
// economical explanation on the table is that his machine reaches the racy
// window and ours no longer does once shaders are warm. Every instrument in
// this file therefore installs LATE, after the title has been presenting for
// a set number of frames, rather than at load. An install that happens after
// startup has finished cannot race a startup.
//
// OFF BY DEFAULT. SLTags=0 in mgpu.ini touches nothing at all.
#pragma once

namespace mgpu::sltags
{
    // Called once per periodic probe report, from the site that already
    // reports acquisition. Installs the tap the first time `mode` is non-zero
    // AND `frames` has passed the deferral threshold; after that it is a
    // counter read and a comparison.
    //
    // mode  0 off, nothing touched (default)
    //       1 install the tap, log each buffer type once
    //       2 as 1, and re-log a buffer type whenever its resource pointer
    //         changes, which is what a resolution change or a resource
    //         recreation looks like from here
    void tick(unsigned long long frames, int mode);

    // The authoritative motion-vector resource the game declared, or 0 if the
    // tap never fired or never saw kBufferTypeMotionVectors. Read by whoever
    // wants to override a ranked guess; zero means "no answer", which every
    // existing caller already treats as "keep the ranked pick".
    unsigned long long mvec_handle();

    // R300d (Dom's R221, T1.5). Optional immediate-capture callback for the officially tagged
    // motion-vector resource, run synchronously from the game's slSetTagForFrame / slSetTag call before
    // Streamline consumes the tag. `cmd_buffer` is sl::CommandBuffer* (the native
    // ID3D12GraphicsCommandList* on D3D12), `state` is sl::Resource.state as declared by the game, and
    // `lifecycle` the ResourceTag lifecycle. Installed only with SLTagsCapture=1|2; none = observer only.
    using mvec_capture_fn = void (*)(void *cmd_buffer, unsigned long long resource,
                                     unsigned state, int lifecycle);
    void set_mvec_capture(mvec_capture_fn fn);

    // R376 (0.5.1 discovery). The same for the depth the game tags (kBufferTypeDepth): run synchronously from
    // the game's tagging call with its command buffer and the state it declared. Installed by dllmain only when
    // DepthSource is in mgpu.ini; none = one load per depth tag, nothing else.
    void set_depth_capture(mvec_capture_fn fn);

    // R379 (0.5.1 discovery; sl_camera.cpp, FGCamera=1 only). The import-slot swap this file uses for its own
    // two thunks, for one more Streamline export: every slot in the game's modules (sl.* and the bridge skipped)
    // whose value is `find` gets `repl`. Returns the number of slots written. Called with the pair reversed to
    // put them back.
    unsigned swap_import_external(void *find, void *repl);

    // The same for the colour buffers that matter to HDR work:
    // kBufferTypeHUDLessColor, then kBufferTypeScalingInputColor. Zero when
    // neither was ever tagged.
    unsigned long long hudless_handle();
    unsigned long long scaling_input_handle();

    // The periodic SLT1 line: what was hooked, how many tagging calls went
    // through it, and the three handles it learned. Safe to call every
    // report; it says nothing until the tap is installed.
    void report();

    // Put the import slots back. Idempotent.
    void uninstall();
}
