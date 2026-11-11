// =====================================================================
// R488 - XeSS-FG (XeFG) 2a SMOKE: a throwaway chain, in its own window
// =====================================================================
//
// ONE QUESTION, ASKED ONCE: does the documented XeFG handover run in THIS
// process, on THIS GPU, from add-on code? Slice 2a touches nothing that
// already works: its own D3D12 device (same adapter, by LUID), its own DIRECT
// queue, its own topmost window, its own flip chain. The display layer, the
// transport, the present path, the panel and the NGX path are untouched.
//
// TWO FLAG FILES, BECAUSE THE FIRST BUILD TAUGHT US THAT ONE WAS NOT ENOUGH.
//
//   <add-on dir>\mgpu_xefg_smoke.on      the HARNESS only: device, queue,
//                                        window, chain, scrolling-band scene,
//                                        presented by us, libxess_fg.dll NEVER
//                                        loaded.
//   <add-on dir>\mgpu_xefg_smoke.api.on  additionally the XeFG handover:
//                                        CreateContext -> XeLL -> GetProperties
//                                        -> InitFromSwapChain -> GetSwapChainPtr
//                                        -> SetEnabled(1) -> tags -> Present.
//
// The harness file is the default-safe arm. The first launch ended ~7 s in,
// in the same second as the first XeFG call, and left no WER record and no
// TDR - so "XeFG" and "our brand new device and window" were both still
// candidates. A launch with only the harness file removes the first candidate
// completely: if that one also dies at ~7 s, the fault is ours and XeFG was
// never involved. That is the ablation this file exists to make possible.
//
// WHAT THE FIRST LAUNCH DID PROVE, AND WHAT IT COST.
//
//   CreateContext                       res=0  SUCCESS, on GPU 1 (luid ...14966)
//   GetProperties(before init)          res=0  maxSupportedInterpolations=1
//   InitFromSwapChain                   res=-15 ERROR_LATENCY_REDUCTION_UNSUPPORTED
//   XeFG's own log: "XeLL context is not set. Make sure to call
//                    xefgSwapChainSetLatencyReduction before the proxy swap
//                    chain initialization."
//
// So XeLL is not optional: the SDK's own message says the context must be set
// BEFORE init. The contract note said so too ("XeLL hard dependency") and this
// file skipped it deliberately to test the minimal path - the minimal path is
// a guaranteed -15. XeLL is now created and set, and its sleep mode is zeroed
// so the latency machinery stays out of the way (minimumIntervalUs=0 disables
// the fps cap, bLowLatencyMode=0 disables the feature).
//
// TWO MORE CORRECTIONS FROM THE SAME LOG.
//
//   1. Cleanup called SetEnabled(ctx,0) even when init had FAILED. That is a
//      call into a half-initialised context, it sits exactly where the log
//      stops, and it is our own bug. SetEnabled(0) is now gated on init_ok.
//   2. Every vendor call in the handover path is now inside __try/__except, so
//      an access violation inside 22 MB of someone else's code becomes one
//      log line instead of a dead game. (0xc0000409 fastfail is NOT catchable;
//      a plain AV is. We do not know which one killed the first launch, so
//      this narrows the gap rather than closing it.)
//
// THE SEQUENCE IS UPSTREAM'S (OptiScaler XeFG_Dx12; handover experiment in
// 40_reports/XEFG_CONTRACT_ZH.md):
//     CreateContext(device), xellD3D12CreateContext(device),
//     SetLatencyReduction(ctx, xell), GetProperties, InitFromSwapChain,
//     GetSwapChainPtr(IID_IDXGISwapChain3), re-fetch back buffers FROM THE
//     PROXY (the chain may have been re-created), SetEnabled(1)  [DEFAULT IS
//     OFF], per frame TagFrameResource(DEPTH, MVEC) + TagFrameConstants +
//     SetPresentId + Present, then GetLastPresentStatus -> framesPresented.
//
// WHY THE SMOKE DRAWS ITS OWN SCENE. framesPresented only becomes 2 once XeFG
// holds depth + velocity + view/projection for two consecutive frames AND is
// enabled; a bare clear reports 1 for ever. The scene is bright vertical bands
// scrolling left by 4 px/frame, a constant 0.5 depth plane and a CONSTANT
// velocity (-4/w, 0) that is exactly right for a pure translation - so the
// interpolated frame is a true half-step, measurable and visible.
//
// GATED BY FILES: absent flag => one GetFileAttributesW and a return.
#include <dxgi1_5.h>   // R488: IDXGISwapChain4, only to report the proxy's QI level
#include <cstdint>

namespace
{
    // ------------------------------------------------------------------
    // The mirrored ABI. The XeFG SDK headers are NOT in this build's include
    // path, so every struct handed to the DLL is declared here with its
    // sizeof and every field offset static_asserted. A wrong offset then
    // stops the BUILD instead of arriving later as a vendor error code that
    // reads like a driver answer.
    // ------------------------------------------------------------------
    struct efg_init_params
    {
        IDXGISwapChain *pApplicationSwapChain;
        uint32_t initFlags;
        uint32_t maxInterpolatedFrames;
        uint32_t creationNodeMask;
        uint32_t visibleNodeMask;
        ID3D12Heap *pTempBufferHeap;
        uint64_t bufferHeapOffset;
        ID3D12Heap *pTempTextureHeap;
        uint64_t textureHeapOffset;
        ID3D12PipelineLibrary *pPipelineLibrary;
        int uiMode;
    };
    static_assert(sizeof(efg_init_params) == 72, "xefg_swapchain_d3d12_init_params_t is 72 bytes");
    static_assert(offsetof(efg_init_params, initFlags) == 8, "initFlags at 8");
    static_assert(offsetof(efg_init_params, maxInterpolatedFrames) == 12, "maxInterpolatedFrames at 12");
    static_assert(offsetof(efg_init_params, creationNodeMask) == 16, "creationNodeMask at 16");
    static_assert(offsetof(efg_init_params, visibleNodeMask) == 20, "visibleNodeMask at 20");
    static_assert(offsetof(efg_init_params, pTempBufferHeap) == 24, "pTempBufferHeap at 24");
    static_assert(offsetof(efg_init_params, bufferHeapOffset) == 32, "bufferHeapOffset at 32");
    static_assert(offsetof(efg_init_params, pTempTextureHeap) == 40, "pTempTextureHeap at 40");
    static_assert(offsetof(efg_init_params, textureHeapOffset) == 48, "textureHeapOffset at 48");
    static_assert(offsetof(efg_init_params, pPipelineLibrary) == 56, "pPipelineLibrary at 56");
    static_assert(offsetof(efg_init_params, uiMode) == 64, "uiMode at 64");

    struct efg_res_data
    {
        int type;
        int validity;
        uint32_t base_x;
        uint32_t base_y;
        uint32_t size_x;
        uint32_t size_y;
        ID3D12Resource *pResource;
        int incomingState;
    };
    static_assert(sizeof(efg_res_data) == 40, "xefg_swapchain_d3d12_resource_data_t is 40 bytes");
    static_assert(offsetof(efg_res_data, type) == 0, "type at 0");
    static_assert(offsetof(efg_res_data, validity) == 4, "validity at 4");
    static_assert(offsetof(efg_res_data, base_x) == 8, "resourceBase.x at 8");
    static_assert(offsetof(efg_res_data, size_x) == 16, "resourceSize.x at 16");
    static_assert(offsetof(efg_res_data, pResource) == 24, "pResource at 24");
    static_assert(offsetof(efg_res_data, incomingState) == 32, "incomingState at 32");

    struct efg_frame_constants
    {
        float view[16];
        float projection[16];
        float jitter_x;
        float jitter_y;
        float mv_scale_x;
        float mv_scale_y;
        uint32_t reset_history;
        float frame_render_time_ms;
    };
    static_assert(sizeof(efg_frame_constants) == 152, "xefg_swapchain_frame_constant_data_t is 152 bytes");
    static_assert(offsetof(efg_frame_constants, jitter_x) == 128, "jitterOffsetX at 128");
    static_assert(offsetof(efg_frame_constants, mv_scale_x) == 136, "motionVectorScaleX at 136");
    static_assert(offsetof(efg_frame_constants, reset_history) == 144, "resetHistory at 144");
    static_assert(offsetof(efg_frame_constants, frame_render_time_ms) == 148, "frameRenderTime at 148");

    struct efg_present_status
    {
        uint32_t frames_presented;
        int frame_gen_result;
        uint32_t is_frame_gen_enabled;
    };
    static_assert(sizeof(efg_present_status) == 12, "xefg_swapchain_present_status_t is 12 bytes");

    struct efg_props
    {
        uint32_t required_descriptor_count;
        uint64_t temp_buffer_heap_size;
        uint64_t temp_texture_heap_size;
        uint64_t constant_buffer_size;
        uint32_t max_supported_interpolations;
    };
    static_assert(sizeof(efg_props) == 40, "xefg_swapchain_properties_t is 40 bytes");

    // xell_sleep_params_t (xell.h:86-99): 4 bytes, bitfields packed.
    struct efg_sleep_params
    {
        uint32_t minimum_interval_us;
        uint32_t flags;   // bit0 bLowLatencyMode, bit1 bLowLatencyBoost, 30 reserved
    };
    static_assert(sizeof(efg_sleep_params) == 8, "xell_sleep_params_t is 8 bytes");

    // resource types (xefg_swapchain.h:105-110), validity (:70-77), ui mode (:280-294)
    // [COPY OptiScaler, 2026-11-11] BACKBUFFER (=4) and UNTIL_NEXT_PRESENT (=0) were
    // missing from this mirror. OptiScaler tags the real frame's backbuffer region with
    // exactly that pair (XeFG_Dx12.cpp:957-966, dummy command list, only the region is
    // read - the SDK header says so at xefg_swapchain_d3d12.h:239-241); without it XeFG
    // is never told which region of the chain's backbuffer is the real frame.
    enum { EFG_RES_DEPTH = 1, EFG_RES_MVEC = 2, EFG_RES_BACKBUFFER = 4 };
    enum { EFG_RV_UNTIL_NEXT_PRESENT = 0, EFG_RV_ONLY_NOW = 1 };
    enum { EFG_UI_MODE_NONE = 1 };
    // init flags (xefg_swapchain.h:82-98). R496: we had been passing 0, i.e. telling
    // the generator that the depth is ordinary Z and that the velocity does not carry
    // jitter - both of which are wrong for this title.
    enum
    {
        EFG_INIT_FLAG_INVERTED_DEPTH = 1 << 0,
        EFG_INIT_FLAG_USE_NDC_VELOCITY = 1 << 3,
        EFG_INIT_FLAG_JITTERED_MV = 1 << 4,
    };

    // ------------------------------------------------------------------
    // The entry points. Resolved by name, never linked: libxess_fg.dll is a
    // vendor binary that only exists in titles shipping XeFG, and libxell is
    // its latency-reduction dependency (and a hard prerequisite of init).
    // ------------------------------------------------------------------
    struct efg_api
    {
        int (*create_context)(ID3D12Device *, void **);
        int (*get_properties)(void *, void *);
        int (*init_from_swapchain)(void *, ID3D12CommandQueue *, const void *);
        // R489: the sibling that creates the chain itself. Needed because DXGI
        // allows one flip chain per HWND, so a handover that requires XeFG to
        // re-create an existing chain can never work (2b-1, 0x80070005).
        int (*init_from_desc)(void *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                              ID3D12CommandQueue *, IDXGIFactory2 *, const void *);
        int (*get_swapchain_ptr)(void *, const IID &, void **);
        int (*destroy)(void *);
        int (*set_enabled)(void *, uint32_t);
        // R497: the live multiplier. Declared in xefg_swapchain.h:
        //   xefgSwapChainSetNumInterpolatedFrames(handle, uint32_t count)
        // count must be between 1 and the maximum given at swap chain init, else
        // ERROR_INVALID_ARGUMENT. This is what makes 2X/3X/4X a runtime choice
        // instead of a restart.
        int (*set_num_frames)(void *, uint32_t);
        int (*set_present_id)(void *, uint32_t);
        int (*tag_frame_constants)(void *, uint32_t, const void *);
        int (*tag_frame_resource)(void *, ID3D12CommandList *, uint32_t, const void *);
        int (*get_last_present_status)(void *, void *);
        int (*set_latency_reduction)(void *, void *);
        int (*set_logging_callback)(void *, int, void (*)(const char *, int, void *), void *);
        int (*get_version)(void *);
        int (*build_pipelines)(void *, ID3D12PipelineLibrary *, uint8_t, uint32_t);
        // Optional: absent in libxess_fg 1.3.1.78 (export table parsed).
        int (*d3d12_get_properties)(void *, const void *, uint32_t, uint32_t, int, void *);
        int (*get_init_params)(void *, void *);
    };

    struct xell_api
    {
        int (*create_context)(ID3D12Device *, void **);
        int (*destroy_context)(void *);
        int (*set_sleep_mode)(void *, const void *);
        int (*get_version)(void *);
        // R493: declared in the shipped xell.h, so these signatures are read
        // rather than reverse engineered:
        //   xellAddMarkerData(ctx, frame_id, xell_latency_marker_type_t)
        //   xellSleep(ctx, frame_id)
        //   xellGetFramesReports(ctx, xell_frame_report_t out[64])
        int (*add_marker)(void *, uint32_t, int);
        int (*sleep)(void *, uint32_t);
        int (*frames_reports)(void *, void *);
    };

    // XeFG's own log, forwarded into our log with a fixed prefix. The callback
    // may arrive on any thread; mgpu::diag's sink is already used from the
    // display and bridge threads, so one more caller is not a new hazard.
    void efg_log_cb(const char *msg, int level, void * /*user*/)
    {
        if (msg == nullptr) return;
        char line[512];
        const char *lv = (level == 0) ? "debug" : (level == 1) ? "info" : (level == 2) ? "warn" : "error";
        snprintf(line, sizeof line, "[MGPU][R488] xefg says [%s] %s", lv, msg);
        mgpu::diag::info(line);
    }

    struct efg_smoke
    {
        IDXGIFactory4 *fac = nullptr;
        ID3D12Device *dev = nullptr;
        ID3D12CommandQueue *q = nullptr;
        ID3D12CommandAllocator *al = nullptr;
        ID3D12GraphicsCommandList *cl = nullptr;
        ID3D12Fence *fe = nullptr;
        HANDLE ev = nullptr;
        UINT64 fv = 0;
        HWND hwnd = nullptr;
        IDXGISwapChain3 *app_chain = nullptr;
        IDXGISwapChain3 *proxy = nullptr;
        IDXGISwapChain3 *present = nullptr;   // proxy when XeFG took the chain, else ours
        ID3D12DescriptorHeap *rtv_heap = nullptr;
        ID3D12DescriptorHeap *dsv_heap = nullptr;
        ID3D12DescriptorHeap *uav_heap = nullptr;
        UINT rtv_inc = 0;
        ID3D12Resource *bb[4] = {};
        UINT bb_n = 0;
        ID3D12Resource *depth = nullptr;
        ID3D12Resource *mvec = nullptr;
        void *ctx = nullptr;
        void *xell = nullptr;
        bool init_ok = false;      // InitFromSwapChain SUCCEEDED: gates SetEnabled(0)
        bool api_on = false;       // the .api flag file was present
        UINT w = 480;
        UINT h = 270;
        efg_api api{};
        xell_api xa{};
    };

    LRESULT CALLBACK efg_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        return DefWindowProcW(h, m, w, l);
    }

    void efg_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                     D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cl->ResourceBarrier(1, &b);
    }

    ID3D12Resource *efg_make_texture(ID3D12Device *dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init,
                                     const D3D12_CLEAR_VALUE *clear)
    {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = flags;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource *res = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, init, clear,
                                                IID_PPV_ARGS(&res))))
            return nullptr;
        return res;
    }

    // Destroy is a vendor call on a context that may never have been
    // initialised - the exact shape of the call that ended the first launch.
    // One guard, one line, and the process keeps running either way.
    void efg_destroy_guarded(efg_smoke &s)
    {
        __try
        {
            const int r = s.api.destroy(s.ctx);
            if (r != 0)
            {
                char line[256];
                snprintf(line, sizeof line, "[MGPU][R488] smoke: xefgSwapChainDestroy res=%d (%s)",
                         r, xefg_result_name(r));
                mgpu::diag::warn(line);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char line[256];
            snprintf(line, sizeof line,
                     "[MGPU][R488] smoke: SEH FAULT 0x%08X inside xefgSwapChainDestroy - caught, the "
                     "context is leaked and the process lives", (unsigned)GetExceptionCode());
            mgpu::diag::error(line);
        }
    }

    void efg_release(efg_smoke &s)
    {
        // Order matters: our back-buffer references and the proxy go first,
        // then the XeFG context (it releases the chain when its own references
        // drop), then XeLL - which XeFG still points at until it is destroyed.
        for (UINT i = 0; i < 4; ++i) { if (s.bb[i] != nullptr) { s.bb[i]->Release(); s.bb[i] = nullptr; } }
        if (s.depth != nullptr) { s.depth->Release(); s.depth = nullptr; }
        if (s.mvec != nullptr) { s.mvec->Release(); s.mvec = nullptr; }
        if (s.rtv_heap != nullptr) { s.rtv_heap->Release(); s.rtv_heap = nullptr; }
        if (s.dsv_heap != nullptr) { s.dsv_heap->Release(); s.dsv_heap = nullptr; }
        if (s.uav_heap != nullptr) { s.uav_heap->Release(); s.uav_heap = nullptr; }
        if (s.proxy != nullptr) { s.proxy->Release(); s.proxy = nullptr; }
        s.present = nullptr;
        if (s.ctx != nullptr && s.api.destroy != nullptr)
        {
            mgpu::diag::info("[MGPU][R488] smoke: destroying the XeFG context now (guarded)");
            efg_destroy_guarded(s);
            s.ctx = nullptr;
        }
        if (s.xell != nullptr && s.xa.destroy_context != nullptr)
        {
            s.xa.destroy_context(s.xell);
            s.xell = nullptr;
        }
        if (s.app_chain != nullptr) { s.app_chain->Release(); s.app_chain = nullptr; }
        if (s.cl != nullptr) { s.cl->Release(); s.cl = nullptr; }
        if (s.al != nullptr) { s.al->Release(); s.al = nullptr; }
        if (s.fe != nullptr) { s.fe->Release(); s.fe = nullptr; }
        if (s.ev != nullptr) { CloseHandle(s.ev); s.ev = nullptr; }
        if (s.q != nullptr) { s.q->Release(); s.q = nullptr; }
        if (s.dev != nullptr) { s.dev->Release(); s.dev = nullptr; }
        if (s.fac != nullptr) { s.fac->Release(); s.fac = nullptr; }
        if (s.hwnd != nullptr) { DestroyWindow(s.hwnd); s.hwnd = nullptr; }
    }

    // The scene. Bands scroll left by a constant pixels-per-frame, so the
    // velocity handed to XeFG is the same for every pixel and is TRUE for the
    // picture - which is what makes the interpolated frame a real half-step
    // instead of an artefact we would then be measuring by accident.
    const int EFG_PERIOD = 48;
    const int EFG_BAND = 20;
    const int EFG_SPEED = 4;   // pixels per real frame

    void efg_draw(ID3D12GraphicsCommandList *cl, ID3D12DescriptorHeap *rtv_heap, UINT rtv_inc,
                  UINT idx, UINT w, UINT h, unsigned frame)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = {
            rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)rtv_inc * idx };
        const float bg[4] = { 0.02f, 0.03f, 0.06f, 1.0f };
        const float fg[4] = { 0.95f, 0.75f, 0.10f, 1.0f };
        cl->ClearRenderTargetView(rtv, bg, 0, nullptr);
        const int phase = (int)((frame * (unsigned)EFG_SPEED) % (unsigned)EFG_PERIOD);
        D3D12_RECT rects[24];
        UINT n = 0;
        for (int x0 = -EFG_PERIOD + phase; x0 < (int)w && n < 24; x0 += EFG_PERIOD)
        {
            rects[n].left = x0;                    rects[n].top = 0;
            rects[n].right = x0 + EFG_BAND;        rects[n].bottom = (LONG)h;
            ++n;
        }
        if (n != 0) cl->ClearRenderTargetView(rtv, fg, n, rects);
    }

    struct efg_arm_stats
    {
        unsigned presents = 0;
        unsigned gen_min = 0xFFFFu;
        unsigned gen_max = 0;
        unsigned long long gen_sum = 0;
        unsigned fg_on = 0;
        int hist[43] = {};      // index = result code + 21, codes -21..21
        double t0 = 0.0;
    };

    void efg_arm_note(efg_arm_stats &a, const efg_present_status &ps)
    {
        ++a.presents;
        if (ps.frames_presented < a.gen_min) a.gen_min = ps.frames_presented;
        if (ps.frames_presented > a.gen_max) a.gen_max = ps.frames_presented;
        a.gen_sum += ps.frames_presented;
        if (ps.is_frame_gen_enabled != 0) ++a.fg_on;
        const int c = ps.frame_gen_result;
        if (c >= -21 && c <= 21) ++a.hist[c + 21];
        else if (c < -21) ++a.hist[0];
        else ++a.hist[42];
    }

    void efg_arm_summary(const char *arm, const efg_arm_stats &a)
    {
        char line[512];
        const double sec = (GetTickCount64() / 1000.0) - a.t0;
        const double mean = (a.presents != 0) ? (double)a.gen_sum / (double)a.presents : 0.0;
        snprintf(line, sizeof line,
                 "[MGPU][R488] smoke %s SUMMARY: presents=%u framesPresented{min=%u max=%u mean=%.2f} "
                 "fgEnabled=%u/%u seconds=%.2f presentRate=%.1f/s",
                 arm, a.presents, a.gen_min, a.gen_max, mean, a.fg_on, a.presents,
                 sec, sec > 0.0 ? (double)a.presents / sec : 0.0);
        mgpu::diag::info(line);

        // The result codes, in one line, as counts: the arm's answer to "why
        // is framesPresented 1" when it is 1.
        char codes[300];
        codes[0] = '\0';
        for (int i = 0; i < 43; ++i)
        {
            if (a.hist[i] == 0) continue;
            const int code = i - 21;
            char one[64];
            snprintf(one, sizeof one, "%s%d:%u", (codes[0] != '\0') ? " " : "", code, a.hist[i]);
            if (strlen(codes) + strlen(one) + 1 >= sizeof codes) break;
            strcat(codes, one);
        }
        snprintf(line, sizeof line,
                 "[MGPU][R488] smoke %s result codes {code:count} = %s (0=SUCCESS 3=TOO_FEW_FRAMES "
                 "-14=INCORRECT_INPUT_RESOURCES -12=MISMATCH_INPUT_RESOURCES)",
                 arm, (codes[0] != '\0') ? codes : "none");
        mgpu::diag::info(line);
    }

    // One arm: N presents. `tags` is the ONLY variable between A and B, and it
    // is false for the whole harness arm (no XeFG context exists to tag).
    bool efg_arm(efg_smoke &s, const char *name, unsigned presents, bool tags, unsigned log_every)
    {
        efg_arm_stats st;
        st.t0 = GetTickCount64() / 1000.0;
        char line[512];
        // QPC, not GetTickCount64: the latter's 15.6 ms resolution turned every
        // 6.25 ms frame on a 160 Hz output into "0 ms" or "15 ms", and this
        // value is handed to XeFG as frameRenderTime - a real input, not a log
        // decoration. A wrong frame time is a wrong pacing input.
        LARGE_INTEGER qf = {};
        QueryPerformanceFrequency(&qf);
        LARGE_INTEGER qa = {}, qb = {};
        QueryPerformanceCounter(&qa);
        float frame_ms = 16.0f;   // replaced by the first measured frame

        for (unsigned n = 0; n < presents; ++n)
        {
            // Wait for the previous frame's submission before resetting the
            // allocator it was recorded into. One fence, one frame of lag.
            if (s.fv != 0)
            {
                if (FAILED(s.fe->SetEventOnCompletion(s.fv, s.ev)) ||
                    WaitForSingleObject(s.ev, 4000) != WAIT_OBJECT_0)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][R488] smoke %s: STOP at frame %u - the GPU fence did not signal "
                             "within 4000 ms (fv=%llu). The arm ends here; what it measured so far is above.",
                             name, n, (unsigned long long)s.fv);
                    mgpu::diag::error(line);
                    break;
                }
            }
            if (FAILED(s.al->Reset()) || FAILED(s.cl->Reset(s.al, nullptr)))
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke %s: STOP at frame %u - allocator/list reset failed",
                         name, n);
                mgpu::diag::error(line);
                break;
            }

            const UINT idx = s.present->GetCurrentBackBufferIndex();
            if (idx >= s.bb_n)
            {
                snprintf(line, sizeof line,
                         "[MGPU][R488] smoke %s: STOP at frame %u - GetCurrentBackBufferIndex=%u but we "
                         "hold %u back buffers", name, n, idx, s.bb_n);
                mgpu::diag::error(line);
                break;
            }

            ID3D12Resource *bb = s.bb[idx];
            efg_barrier(s.cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            efg_draw(s.cl, s.rtv_heap, s.rtv_inc, idx, s.w, s.h, n);
            efg_barrier(s.cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

            const uint32_t present_id = n + 1u;   // "Frame ID should be over 0" (result 3)
            if (tags)
            {
                // depth: back to a write state, cleared, then handed over in
                // the state XeFG is expected to read it from. On frame 0 the
                // resource is already in that write state (its creation state),
                // so the first transition's StateBefore is a formality the
                // runtime does not read.
                efg_barrier(s.cl, s.depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_DEPTH_WRITE);
                const D3D12_CPU_DESCRIPTOR_HANDLE dsv = { s.dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr };
                s.cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
                efg_barrier(s.cl, s.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

                // velocity: one constant for the whole picture, true for a pure
                // translation of the whole image.
                const D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu = { s.uav_heap->GetCPUDescriptorHandleForHeapStart().ptr };
                const D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = { s.uav_heap->GetGPUDescriptorHandleForHeapStart().ptr };
                efg_barrier(s.cl, s.mvec, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                const float vx = -(float)EFG_SPEED / (float)s.w;
                const float vclear[4] = { vx, 0.0f, 0.0f, 0.0f };
                s.cl->ClearUnorderedAccessViewFloat(uav_gpu, uav_cpu, s.mvec, vclear, 0, nullptr);
                efg_barrier(s.cl, s.mvec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

                efg_frame_constants fc = {};
                fc.view[0] = fc.view[5] = fc.view[10] = fc.view[15] = 1.0f;
                fc.projection[0] = fc.projection[5] = fc.projection[10] = fc.projection[15] = 1.0f;
                fc.mv_scale_x = 1.0f;   // velocities above are already normalised
                fc.mv_scale_y = 1.0f;
                fc.reset_history = (n < 2) ? 1u : 0u;
                fc.frame_render_time_ms = frame_ms;
                s.api.tag_frame_constants(s.ctx, present_id, &fc);

                for (int k = 0; k < 2; ++k)
                {
                    efg_res_data rd = {};
                    rd.type = (k == 0) ? EFG_RES_DEPTH : EFG_RES_MVEC;
                    rd.validity = EFG_RV_ONLY_NOW;
                    rd.size_x = s.w;
                    rd.size_y = s.h;
                    rd.pResource = (k == 0) ? s.depth : s.mvec;
                    rd.incomingState = (int)D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    const int r = s.api.tag_frame_resource(s.ctx, s.cl, present_id, &rd);
                    if (r != 0 && n < 8)
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][R488] smoke %s: TagFrameResource(%s) returned res=%d (%s) at frame %u",
                                 name, (k == 0) ? "DEPTH" : "MVEC", r, xefg_result_name(r), n);
                        mgpu::diag::warn(line);
                    }
                }
            }

            if (FAILED(s.cl->Close())) { mgpu::diag::error("[MGPU][R488] smoke: command list close failed"); break; }
            ID3D12CommandList *lists[1] = { s.cl };
            s.q->ExecuteCommandLists(1, lists);
            if (s.api.set_present_id != nullptr) s.api.set_present_id(s.ctx, present_id);
            if (FAILED(s.q->Signal(s.fe, ++s.fv)))
            {
                mgpu::diag::error("[MGPU][R488] smoke: queue signal failed");
                break;
            }

            const HRESULT phr = s.present->Present(1, 0);
            efg_present_status ps = {};
            if (s.api.get_last_present_status != nullptr)
                s.api.get_last_present_status(s.ctx, &ps);
            else
            {
                // Harness arm: no XeFG, so one frame per Present is the truth
                // by construction, not a measurement.
                ps.frames_presented = 1;
                ps.is_frame_gen_enabled = 0;
                ps.frame_gen_result = 0;
            }
            efg_arm_note(st, ps);

            QueryPerformanceCounter(&qb);
            if (qf.QuadPart != 0)
                frame_ms = (float)((double)(qb.QuadPart - qa.QuadPart) * 1000.0 / (double)qf.QuadPart);
            qa = qb;

            if (n < 6 || (n % log_every) == 0 || FAILED(phr))
            {
                snprintf(line, sizeof line,
                         "[MGPU][R488] smoke %s #%04u: present hr=0x%08X framesPresented=%u "
                         "fgEnabled=%u res=%d(%s) backBuffer=%u %.2fms",
                         name, n, (unsigned)phr, ps.frames_presented, ps.is_frame_gen_enabled,
                         ps.frame_gen_result, xefg_result_name(ps.frame_gen_result), idx,
                         frame_ms);
                mgpu::diag::info(line);
            }
            if (FAILED(phr))
            {
                snprintf(line, sizeof line,
                         "[MGPU][R488] smoke %s: STOP at frame %u - Present failed hr=0x%08X "
                         "(the chain is refusing; this arm has no more data to give)",
                         name, n, (unsigned)phr);
                mgpu::diag::error(line);
                break;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        efg_arm_summary(name, st);
        return st.presents != 0;
    }

    bool efg_load(efg_smoke &s, HMODULE mod, char *missing, size_t missing_n)
    {
        struct { const char *name; void **slot; bool required; } want[] = {
            { "xefgSwapChainD3D12CreateContext",      (void **)&s.api.create_context,        true  },
            { "xefgSwapChainGetProperties",           (void **)&s.api.get_properties,        true  },
            { "xefgSwapChainSetLatencyReduction",     (void **)&s.api.set_latency_reduction, true  },
            { "xefgSwapChainD3D12InitFromSwapChain",  (void **)&s.api.init_from_swapchain,   true  },
            { "xefgSwapChainD3D12GetSwapChainPtr",    (void **)&s.api.get_swapchain_ptr,     true  },
            { "xefgSwapChainDestroy",                 (void **)&s.api.destroy,               true  },
            { "xefgSwapChainSetEnabled",              (void **)&s.api.set_enabled,           true  },
            { "xefgSwapChainSetPresentId",            (void **)&s.api.set_present_id,        true  },
            { "xefgSwapChainTagFrameConstants",       (void **)&s.api.tag_frame_constants,   true  },
            { "xefgSwapChainD3D12TagFrameResource",   (void **)&s.api.tag_frame_resource,    true  },
            { "xefgSwapChainGetLastPresentStatus",    (void **)&s.api.get_last_present_status, true },
            { "xefgSwapChainSetLoggingCallback",      (void **)&s.api.set_logging_callback,  false },
            { "xefgSwapChainGetVersion",              (void **)&s.api.get_version,           false },
            { "xefgSwapChainD3D12BuildPipelines",     (void **)&s.api.build_pipelines,       false },
            { "xefgSwapChainD3D12GetProperties",      (void **)&s.api.d3d12_get_properties,  false },
            { "xefgSwapChainD3D12GetInitializationParameters", (void **)&s.api.get_init_params, false },
        };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; ++i)
        {
            *want[i].slot = (void *)GetProcAddress(mod, want[i].name);
            if (*want[i].slot == nullptr && want[i].required)
            {
                snprintf(missing, missing_n, "%s", want[i].name);
                return false;
            }
        }
        return true;
    }

    bool efg_load_xell(xell_api &xa, HMODULE mod, char *missing, size_t missing_n)
    {
        struct { const char *name; void **slot; bool required; } want[] = {
            { "xellD3D12CreateContext", (void **)&xa.create_context,  true  },
            { "xellDestroyContext",     (void **)&xa.destroy_context, true  },
            { "xellSetSleepMode",       (void **)&xa.set_sleep_mode,  false },
            { "xellGetVersion",         (void **)&xa.get_version,     false },
        };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; ++i)
        {
            *want[i].slot = (void *)GetProcAddress(mod, want[i].name);
            if (*want[i].slot == nullptr && want[i].required)
            {
                snprintf(missing, missing_n, "%s", want[i].name);
                return false;
            }
        }
        return true;
    }

    // A path for a log line: its tail, so the file name survives and a deep
    // install path cannot blow the 400-byte budget.
    void efg_tail_path(const wchar_t *p, wchar_t *out, size_t out_n)
    {
        const size_t len = (p != nullptr) ? wcslen(p) : 0;
        if (len == 0) { wcscpy_s(out, out_n, L"?"); return; }
        if (len + 1 <= out_n) { wcscpy_s(out, out_n, p); return; }
        wcscpy_s(out, out_n, L"...");
        wcscat_s(out, out_n, p + (len - (out_n - 4)));
    }

    // ------------------------------------------------------------------
    // The handover itself: module, XeLL, context, properties, init, proxy.
    // Every vendor call in here is someone else's 22 MB of code, so the whole
    // body runs under one SEH guard in efg_handover() below.
    // ------------------------------------------------------------------
    bool efg_handover_body(efg_smoke &s, char *line, size_t line_n)
    {
        wchar_t dir[MAX_PATH * 2] = {};
        if (!addon_dir_w(dir, MAX_PATH * 2))
        {
            mgpu::diag::error("[MGPU][R488] smoke: add-on directory unknown - nothing runs");
            return false;
        }
        wchar_t p[MAX_PATH * 2] = {};

        // ---- the modules. mgpu\ first on purpose: on a game whose own
        //      libxess_fg predates non-Intel support (Cyberpunk 2077's does:
        //      API 1.1.0) the game's copy cannot answer on this GPU, and
        //      dropping the newer one in mgpu\ touches no game file. ----
        _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%smgpu\\libxess_fg.dll", dir);
        HMODULE mod = file_exists_w(p) ? LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (mod == nullptr)
        {
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%slibxess_fg.dll", dir);
            if (file_exists_w(p)) mod = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (mod == nullptr) mod = LoadLibraryW(L"libxess_fg.dll");
        if (mod == nullptr)
        {
            snprintf(line, line_n,
                     "[MGPU][R488] smoke: STOP - libxess_fg.dll could not be loaded (GetLastError=%lu). "
                     "Nothing was created and nothing else was touched.", (unsigned long)GetLastError());
            mgpu::diag::error(line);
            return false;
        }
        {
            wchar_t mp[MAX_PATH * 2] = {};
            wchar_t shown[192];
            char miss[96] = {};
            if (GetModuleFileNameW(mod, mp, MAX_PATH * 2) == 0) wcscpy_s(mp, L"?");
            efg_tail_path(mp, shown, 192);
            if (!efg_load(s, mod, miss, sizeof miss))
            {
                snprintf(line, line_n,
                         "[MGPU][R488] smoke: STOP - %s is not exported by module=%ls. Nothing was created.",
                         miss, shown);
                mgpu::diag::error(line);
                return false;
            }
            snprintf(line, line_n, "[MGPU][R488] smoke: xefg module=%ls", shown);
            mgpu::diag::info(line);
        }

        // libxell is NOT loaded here: which copy matters, so both are tried in
        // the XeLL block below, after the XeFG context exists.

        // ---- PRELOAD libxell UNDER ITS OWN BASE NAME, BEFORE CreateContext ----
        //
        // The third launch settled what the failures meant: XeFG printed
        // "libxell.dll version 1.1.0" while being offered a 1.3.2 and a 1.3.0,
        // so the instance it holds is the EXE-DIRECTORY copy - and that copy
        // cannot even create a context here (create res=-1,
        // ERROR_UNSUPPORTED_DEVICE). A context is only valid to the module
        // instance that created it, so no copy we can build a context FROM is
        // the copy XeFG HOLDS.
        //
        // XeFG resolves XeLL by plain name, and Windows resolves a plain name
        // to a module of the same BASE NAME that is already in the process. So
        // if nothing has loaded libxell.dll yet, loading the modern copy here
        // - before CreateContext - makes it the module XeFG will get. If the
        // host already loaded its own, that preload cannot win, and the log
        // says so instead of pretending: that is the case where only replacing
        // the game's file can help.
        {
            wchar_t mp[MAX_PATH * 2] = {};
            wchar_t shown[192];
            HMODULE pre = GetModuleHandleW(L"libxell.dll");
            if (pre != nullptr)
            {
                if (GetModuleFileNameW(pre, mp, MAX_PATH * 2) == 0) wcscpy_s(mp, L"?");
                efg_tail_path(mp, shown, 192);
                snprintf(line, line_n,
                         "[MGPU][R488] smoke: libxell.dll was ALREADY in the process before the smoke: "
                         "module=%ls. XeFG holds THAT instance, so a context from any other copy cannot be "
                         "accepted - no preload can win this case.", shown);
                mgpu::diag::warn(line);
            }
            else
            {
                wchar_t cp[MAX_PATH * 2] = {};
                _snwprintf_s(cp, MAX_PATH * 2, _TRUNCATE, L"%smgpu\\libxell.dll", dir);
                HMODULE hp = file_exists_w(cp) ? LoadLibraryExW(cp, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
                if (hp != nullptr && GetModuleFileNameW(hp, mp, MAX_PATH * 2) != 0)
                {
                    efg_tail_path(mp, shown, 192);
                    snprintf(line, line_n,
                             "[MGPU][R488] smoke: PRELOADED libxell.dll from %ls - a plain-name load now "
                             "resolves to this copy, which is what XeFG should get", shown);
                    mgpu::diag::info(line);
                }
                else
                {
                    mgpu::diag::warn("[MGPU][R488] smoke: could not preload libxell.dll; XeFG may still "
                                    "resolve the exe-directory copy");
                }
            }
        }

        // ---- context ----
        void *ctx = nullptr;
        int r = s.api.create_context(s.dev, &ctx);
        snprintf(line, line_n, "[MGPU][R488] smoke: xefgSwapChainD3D12CreateContext res=%d (%s) ctx=%p",
                 r, xefg_result_name(r), ctx);
        mgpu::diag::info(line);
        if (r != 0) return false;
        s.ctx = ctx;

        if (s.api.set_logging_callback != nullptr)
            s.api.set_logging_callback(s.ctx, 1 /*INFO*/, efg_log_cb, nullptr);

        // ---- XeLL. XeFG refuses to init without one (the first launch's -15,
        //      and XeFG's own message named the call). The SECOND launch showed
        //      the harder half: the call now worked, but XeFG refused the
        //      context itself - "XeLL context 0x... is not supported".
        //
        //      A XeLL context is only valid to the module instance that created
        //      it, and XeFG (living in mgpu\) resolves a plain
        //      LoadLibrary("libxell.dll") against the EXE DIRECTORY first. So
        //      the exe-directory copy is tried first and mgpu\ is the fallback;
        //      whichever XeFG ACCEPTS is the one it is actually using. The
        //      version of each candidate is printed, because "rejected" and
        //      "rejected because it is a different file" are different answers.
        //
        //      Sleep mode is zeroed: minimumIntervalUs=0 disables the fps cap
        //      and bit0=0 disables low-latency mode, so XeLL exists (which init
        //      requires) but does not sleep or pace this thread.
        {
            // Three copies exist on this machine and they are NOT the same build:
            //   bin\x64\libxell.dll        0B127D64   346,984  the game's own
            //   mgpu\libxell.dll           61AEEAB7   411,504  the SDK bundle's
            //   mgpu\libxell_opti.dll      D2030DCD   415,368  OptiScaler's
            // OptiScaler bundles a libxess_fg that is BYTE-IDENTICAL to ours and
            // pairs it with the 415,368 build, so that is the first candidate.
            // The game's own copy is second: if XeFG accepts it, then XeFG is
            // loading its XeLL by plain name from the exe directory and the
            // module-identity theory is the answer. Ours is last - it is the one
            // that was already rejected, kept as the control.
            const wchar_t *cands[3] = { L"(in-process)libxell.dll", L"mgpu\\libxell_opti.dll", L"mgpu\\libxell.dll" };
            for (int ci = 0; ci < 3; ++ci)
            {
                HMODULE xm = nullptr;
                if (ci == 0)
                {
                    // Whatever instance the process holds under this base name
                    // AFTER the preload above. If XeFG resolves XeLL by plain
                    // name, this is the one instance it can ever accept.
                    xm = GetModuleHandleW(L"libxell.dll");
                }
                else
                {
                    wchar_t cp[MAX_PATH * 2] = {};
                    _snwprintf_s(cp, MAX_PATH * 2, _TRUNCATE, L"%s%s", dir, cands[ci]);
                    if (file_exists_w(cp)) xm = LoadLibraryExW(cp, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
                if (xm == nullptr)
                {
                    snprintf(line, line_n,
                             "[MGPU][R488] smoke: xell candidate %ls: absent or failed to load (err=%lu)",
                             cands[ci], (unsigned long)GetLastError());
                    mgpu::diag::info(line);
                    continue;
                }
                xell_api xa = {};
                char miss[96] = {};
                wchar_t mp[MAX_PATH * 2] = {};
                wchar_t shown[192];
                if (GetModuleFileNameW(xm, mp, MAX_PATH * 2) == 0) wcscpy_s(mp, L"?");
                efg_tail_path(mp, shown, 192);
                if (!efg_load_xell(xa, xm, miss, sizeof miss))
                {
                    snprintf(line, line_n, "[MGPU][R488] smoke: xell candidate %ls: %s not exported (%ls)",
                             cands[ci], miss, shown);
                    mgpu::diag::warn(line);
                    continue;
                }
                {
                    char vs[40];
                    snprintf(vs, sizeof vs, "n/a(no_export)");
                    if (xa.get_version != nullptr)
                    {
                        unsigned short v[4] = {};
                        if (xa.get_version(v) == 0)
                            snprintf(vs, sizeof vs, "%u.%u.%u",
                                     (unsigned)v[0], (unsigned)v[1], (unsigned)v[2]);
                    }
                    snprintf(line, line_n, "[MGPU][R488] smoke: xell candidate %ls module=%ls version=%s",
                             cands[ci], shown, vs);
                    mgpu::diag::info(line);
                }
                void *xc = nullptr;
                const int zr = xa.create_context(s.dev, &xc);
                snprintf(line, line_n, "[MGPU][R488] smoke: xell candidate %ls create res=%d ctx=%p",
                         cands[ci], zr, xc);
                mgpu::diag::info(line);
                if (zr != 0 || xc == nullptr) continue;
                if (xa.set_sleep_mode != nullptr)
                {
                    efg_sleep_params sp = {};
                    sp.minimum_interval_us = 0;   // 0 = no fps cap
                    // bit0 = bLowLatencyMode. It must be ON: with it off,
                    // SetEnabled(1) is refused with -15 and XeFG's own message
                    // says why - "Latency reduction is not enabled". Setting the
                    // context is not the same as enabling the feature.
                    sp.flags = 1u;
                    const int sr = xa.set_sleep_mode(xc, &sp);
                    snprintf(line, line_n,
                             "[MGPU][R488] smoke: xell candidate %ls setSleepMode(cap=0, lowLatency=1) res=%d",
                             cands[ci], sr);
                    mgpu::diag::info(line);
                }
                const int lr = s.api.set_latency_reduction(s.ctx, xc);
                snprintf(line, line_n,
                         "[MGPU][R488] smoke: xell candidate %ls SetLatencyReduction res=%d (%s)%s",
                         cands[ci], lr, xefg_result_name(lr),
                         (lr == 0) ? " - ACCEPTED: this is the copy XeFG itself is using"
                                   : " - rejected, trying the next candidate");
                mgpu::diag::info(line);
                if (lr == 0) { s.xell = xc; s.xa = xa; break; }
                xa.destroy_context(xc);
            }
        }
        if (s.xell == nullptr)
        {
            mgpu::diag::error("[MGPU][R488] smoke: STOP - no XeLL copy was accepted by XeFG. Init cannot "
                              "succeed without one (-15); this is a module-identity or version question and "
                              "it is now measured rather than guessed.");
            return false;
        }

        // ---- properties ----
        efg_props props = {};
        r = s.api.get_properties(s.ctx, &props);
        snprintf(line, line_n,
                 "[MGPU][R488] smoke: GetProperties(before init) res=%d (%s) maxInterp=%u reqDesc=%u "
                 "tempBufHeap=%llu tempTexHeap=%llu constBuf=%llu",
                 r, xefg_result_name(r), props.max_supported_interpolations,
                 props.required_descriptor_count,
                 (unsigned long long)props.temp_buffer_heap_size,
                 (unsigned long long)props.temp_texture_heap_size,
                 (unsigned long long)props.constant_buffer_size);
        mgpu::diag::info(line);
        if (r != 0) return false;

        // ---- init. Announced before it happens: if the log ends here, this
        //      line names the call that took the process down. ----
        efg_init_params ip = {};
        ip.pApplicationSwapChain = s.app_chain;
        ip.initFlags = 0;                 // no external heap, no inverted depth, no NDC velocity
        ip.maxInterpolatedFrames = 1;     // native 2x, explicit: a known multiplier, not "the max"
        ip.creationNodeMask = 1;
        ip.visibleNodeMask = 1;
        ip.uiMode = EFG_UI_MODE_NONE;     // interpolate the back buffer, no UI composition
        const unsigned long long t_init = GetTickCount64();
        mgpu::diag::info("[MGPU][R488] smoke: calling xefgSwapChainD3D12InitFromSwapChain - this compiles "
                         "pipelines and can block for seconds; it is on our thread, not the game's");
        r = s.api.init_from_swapchain(s.ctx, s.q, &ip);
        const unsigned long long init_ms = GetTickCount64() - t_init;
        snprintf(line, line_n, "[MGPU][R488] smoke: InitFromSwapChain res=%d (%s) elapsed=%llums",
                 r, xefg_result_name(r), init_ms);
        mgpu::diag::info(line);
        if (r != 0) return false;
        s.init_ok = true;

        // What did XeFG actually apply? pApplicationSwapChain==NULL here means
        // the chain was RE-CREATED - which is why the back buffers are taken
        // from the proxy below and never from the chain we passed in.
        if (s.api.get_init_params != nullptr)
        {
            efg_init_params out = {};
            const int r2 = s.api.get_init_params(s.ctx, &out);
            snprintf(line, line_n,
                     "[MGPU][R488] smoke: GetInitializationParameters res=%d (%s) appliedMaxInterp=%u "
                     "appChain=%s initFlags=0x%X node=%u/%u uiMode=%d",
                     r2, xefg_result_name(r2), out.maxInterpolatedFrames,
                     (out.pApplicationSwapChain == nullptr) ? "NULL(chain was re-created)" : "kept",
                     out.initFlags, out.creationNodeMask, out.visibleNodeMask, out.uiMode);
            mgpu::diag::info(line);
        }
        else
        {
            mgpu::diag::warn("[MGPU][R488] smoke: xefgSwapChainD3D12GetInitializationParameters is not "
                            "exported by this module - the applied multiplier stays unknown");
        }

        r = s.api.get_swapchain_ptr(s.ctx, __uuidof(IDXGISwapChain3), (void **)&s.proxy);
        snprintf(line, line_n, "[MGPU][R488] smoke: GetSwapChainPtr(IDXGISwapChain3) res=%d (%s) proxy=%p",
                 r, xefg_result_name(r), (void *)s.proxy);
        mgpu::diag::info(line);
        if (r != 0 || s.proxy == nullptr)
        {
            mgpu::diag::error("[MGPU][R488] smoke: STOP - the chain was consumed and no proxy came back. "
                              "This is the one unrecoverable XeFG failure mode; in 2b it would mean no "
                              "present path, which is why 2a runs it on a throwaway chain.");
            return false;
        }
        return true;
    }

    bool efg_handover(efg_smoke &s)
    {
        char line[512];
        __try
        {
            return efg_handover_body(s, line, sizeof line);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            snprintf(line, sizeof line,
                     "[MGPU][R488] smoke: SEH FAULT 0x%08X inside the XeFG handover (module load / "
                     "CreateContext / XeLL / InitFromSwapChain / GetSwapChainPtr). The guard caught it, "
                     "the process survived, and nothing further is attempted. ctx=%p xell=%p",
                     (unsigned)GetExceptionCode(), s.ctx, s.xell);
            mgpu::diag::error(line);
            return false;
        }
    }

    // Device, queue, window, chain. No vendor call except LoadLibrary, which is
    // why the harness arm can run this and nothing else.
    bool efg_setup(efg_smoke &s, LUID luid, const char *where, bool api_on)
    {
        char line[512];
        HRESULT hr = E_FAIL;
        s.api_on = api_on;

        snprintf(line, sizeof line,
                 "[MGPU][R488] smoke: BEGIN where=%s arm=%s window=%ux%u fmt=R10G10B10A2 buffers=2 "
                 "swapEffect=FLIP_DISCARD queue=DIRECT luid=0x%08X-0x%08X",
                 (where != nullptr) ? where : "?", api_on ? "XEFG" : "HARNESS-ONLY(no libxess_fg)",
                 s.w, s.h, (unsigned)luid.HighPart, (unsigned)luid.LowPart);
        mgpu::diag::info(line);

        // ---- 1. our own device, on the SAME adapter as GPU 1, by LUID ----
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&s.fac));
        if (FAILED(hr)) { snprintf(line, sizeof line, "[MGPU][R488] smoke: CreateDXGIFactory2 hr=0x%08X - nothing runs", (unsigned)hr); mgpu::diag::error(line); return false; }
        IDXGIAdapter1 *ad = nullptr;
        hr = s.fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
        if (FAILED(hr) || ad == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][R488] smoke: EnumAdapterByLuid(0x%08X-0x%08X) hr=0x%08X - nothing runs",
                     (unsigned)luid.HighPart, (unsigned)luid.LowPart, (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }
        hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.dev));
        ad->Release();
        if (FAILED(hr)) { snprintf(line, sizeof line, "[MGPU][R488] smoke: D3D12CreateDevice hr=0x%08X - nothing runs", (unsigned)hr); mgpu::diag::error(line); return false; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(hr = s.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.q))) ||
            FAILED(hr = s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.al))) ||
            FAILED(hr = s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.al, nullptr, IID_PPV_ARGS(&s.cl))) ||
            FAILED(hr = s.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fe))))
        {
            snprintf(line, sizeof line, "[MGPU][R488] smoke: queue/allocator/list/fence hr=0x%08X - nothing runs", (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }
        s.cl->Close();   // created open; every frame resets it
        s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (s.ev == nullptr) { mgpu::diag::error("[MGPU][R488] smoke: CreateEventW failed - nothing runs"); return false; }

        // ---- 2. our own window, topmost, never activated: the game keeps the
        //         focus and the keyboard. It exists so the user can SEE the
        //         scene, and because a flip chain needs an HWND. ----
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = efg_wndproc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MGPU_XEFG_SMOKE_WND";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke: RegisterClassExW failed (err=%lu) - nothing runs",
                         (unsigned long)GetLastError());
                mgpu::diag::error(line);
                return false;
            }
            s.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                     wc.lpszClassName, L"MGPU XeFG smoke 2a",
                                     WS_POPUP, 40, 40, (int)s.w, (int)s.h,
                                     nullptr, nullptr, wc.hInstance, nullptr);
            if (s.hwnd == nullptr)
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke: CreateWindowExW failed (err=%lu) - nothing runs",
                         (unsigned long)GetLastError());
                mgpu::diag::error(line);
                return false;
            }
            ShowWindow(s.hwnd, SW_SHOWNA);
            UpdateWindow(s.hwnd);
        }

        // ---- 3. our own chain, in exactly the shape the display layer uses:
        //         R10G10B10A2, two buffers, FLIP_DISCARD, the DIRECT queue that
        //         will present, vsync. If XeFG refuses this shape, 2b would have
        //         refused it too and it is better to learn it here. ----
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = s.w;
        scd.Height = s.h;
        scd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags = 0;
        IDXGISwapChain1 *c1 = nullptr;
        hr = s.fac->CreateSwapChainForHwnd(s.q, s.hwnd, &scd, nullptr, nullptr, &c1);
        if (FAILED(hr) || c1 == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][R488] smoke: CreateSwapChainForHwnd hr=0x%08X - nothing runs", (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }
        s.fac->MakeWindowAssociation(s.hwnd, DXGI_MWA_NO_ALT_ENTER);
        hr = c1->QueryInterface(IID_PPV_ARGS(&s.app_chain));
        c1->Release();
        if (FAILED(hr)) { mgpu::diag::error("[MGPU][R488] smoke: QI IDXGISwapChain3 on our own chain failed - nothing runs"); return false; }

        // ---- 4. the handover, or the harness notice that there is none ----
        if (api_on)
        {
            if (!efg_handover(s)) return false;
            s.present = s.proxy;
        }
        else
        {
            s.present = s.app_chain;
            mgpu::diag::info("[MGPU][R488] smoke: HARNESS-ONLY arm - libxess_fg.dll is NOT loaded, no "
                             "vendor code is entered, and the scene below is presented by us to our own "
                             "chain. If THIS arm also ends the game, XeFG was never the cause.");
        }

        // ---- 5. describe the chain we must present to, then build the views
        //         from ITS back buffers. After a successful handover the proxy
        //         may be a different chain with different buffers. ----
        {
            if (s.proxy != nullptr)
            {
                IDXGISwapChain4 *p4 = nullptr;
                const HRESULT h4 = s.proxy->QueryInterface(IID_PPV_ARGS(&p4));
                if (SUCCEEDED(h4) && p4 != nullptr) p4->Release();
                DXGI_SWAP_CHAIN_DESC1 d1 = {};
                if (SUCCEEDED(s.proxy->GetDesc1(&d1)))
                {
                    snprintf(line, sizeof line,
                             "[MGPU][R488] smoke: proxy desc %ux%u fmt=%u buffers=%u swapEffect=%u flags=0x%X qi4=%s",
                             d1.Width, d1.Height, (unsigned)d1.Format, d1.BufferCount,
                             (unsigned)d1.SwapEffect, d1.Flags, SUCCEEDED(h4) ? "ok" : "no");
                    mgpu::diag::info(line);
                    s.bb_n = (d1.BufferCount > 4) ? 4 : d1.BufferCount;
                    if (d1.Width != s.w || d1.Height != s.h)
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][R488] smoke: NOTE - the proxy reports %ux%u but we asked for %ux%u. "
                                 "The scene is drawn at the size the proxy reports.",
                                 d1.Width, d1.Height, s.w, s.h);
                        mgpu::diag::warn(line);
                        s.w = d1.Width;
                        s.h = d1.Height;
                    }
                }
                else { mgpu::diag::warn("[MGPU][R488] smoke: proxy->GetDesc1 failed"); s.bb_n = 2; }
            }
            else
            {
                s.bb_n = 2;   // our own chain: exactly what we asked for
                DXGI_SWAP_CHAIN_DESC1 d1 = {};
                if (SUCCEEDED(s.app_chain->GetDesc1(&d1)) && d1.BufferCount <= 4) s.bb_n = d1.BufferCount;
            }

            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = s.bb_n;
            if (FAILED(hr = s.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv_heap))))
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke: RTV heap hr=0x%08X", (unsigned)hr);
                mgpu::diag::error(line);
                return false;
            }
            s.rtv_inc = s.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE at = s.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < s.bb_n; ++i)
            {
                if (FAILED(s.present->GetBuffer(i, IID_PPV_ARGS(&s.bb[i]))))
                {
                    snprintf(line, sizeof line, "[MGPU][R488] smoke: GetBuffer(%u) failed on the chain we present to", i);
                    mgpu::diag::error(line);
                    return false;
                }
                D3D12_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                s.dev->CreateRenderTargetView(s.bb[i], &rd, at);
                at.ptr += s.rtv_inc;
            }

            D3D12_DESCRIPTOR_HEAP_DESC dd = {};
            dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dd.NumDescriptors = 1;
            if (FAILED(hr = s.dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&s.dsv_heap))))
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke: DSV heap hr=0x%08X", (unsigned)hr);
                mgpu::diag::error(line);
                return false;
            }
            D3D12_CLEAR_VALUE dcv = {};
            dcv.Format = DXGI_FORMAT_D32_FLOAT;
            dcv.DepthStencil.Depth = 0.5f;
            s.depth = efg_make_texture(s.dev, s.w, s.h, DXGI_FORMAT_D32_FLOAT,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv);
            if (s.depth == nullptr) { mgpu::diag::error("[MGPU][R488] smoke: depth texture creation failed"); return false; }
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            s.dev->CreateDepthStencilView(s.depth, &dsv, s.dsv_heap->GetCPUDescriptorHandleForHeapStart());

            D3D12_DESCRIPTOR_HEAP_DESC ud = {};
            ud.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            ud.NumDescriptors = 1;
            ud.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(hr = s.dev->CreateDescriptorHeap(&ud, IID_PPV_ARGS(&s.uav_heap))))
            {
                snprintf(line, sizeof line, "[MGPU][R488] smoke: UAV heap hr=0x%08X", (unsigned)hr);
                mgpu::diag::error(line);
                return false;
            }
            s.mvec = efg_make_texture(s.dev, s.w, s.h, DXGI_FORMAT_R16G16_FLOAT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
            if (s.mvec == nullptr) { mgpu::diag::error("[MGPU][R488] smoke: velocity texture creation failed"); return false; }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            s.dev->CreateUnorderedAccessView(s.mvec, nullptr, &uav,
                                             s.uav_heap->GetCPUDescriptorHandleForHeapStart());

            snprintf(line, sizeof line,
                     "[MGPU][R488] smoke: armed %u back buffer(s) of the chain we present to + depth "
                     "D32_FLOAT@0.5 + velocity RG16F(%.5f,0) per frame + bands every %dpx moving %dpx/frame",
                     s.bb_n, -(float)EFG_SPEED / (float)s.w, EFG_PERIOD, EFG_SPEED);
            mgpu::diag::info(line);
        }

        // ---- 6. XeFG's frame generation is OFF until this call - and only an
        //         INITIALISED context may be touched: the first launch called
        //         SetEnabled(0) on a context whose init had failed. ----
        if (s.api.set_enabled != nullptr && s.ctx != nullptr)
        {
            const int r = s.api.set_enabled(s.ctx, 1);
            snprintf(line, sizeof line, "[MGPU][R488] smoke: SetEnabled(1) res=%d (%s)", r, xefg_result_name(r));
            mgpu::diag::info(line);
            // NOT fatal any more. A context with FG switched off still presents
            // the real frames, and the arms below then answer "off" with
            // framesPresented=1 and a real result code - which is data. The
            // first version returned here, the window vanished before anyone
            // could see it, and a whole launch produced only the word "no".
            if (r != 0)
            {
                mgpu::diag::warn("[MGPU][R488] smoke: frame generation could NOT be switched on - the arms "
                                "run anyway, so the chain and the tagging are still measured");
            }
        }
        return true;
    }

    void efg_cleanup(efg_smoke &s)
    {
        char line[256];
        // Only an initialised context may be switched off. That single
        // condition is the fix for the first launch's last act.
        if (s.ctx != nullptr && s.init_ok && s.api.set_enabled != nullptr)
            s.api.set_enabled(s.ctx, 0);
        mgpu::diag::info("[MGPU][R488] smoke: releasing - proxy/back buffers first, then the context");
        efg_release(s);
        snprintf(line, sizeof line,
                 "[MGPU][R488] smoke: END - context destroyed, proxy released, window gone. The game was "
                 "never touched by this path.");
        mgpu::diag::info(line);
    }
}

// R488: the thread body. Every failure is a logged stop, never a silent one,
// and nothing it allocates is reachable from any other subsystem.
void mgpu_xefg_smoke_start(ID3D12Device *device, const char *where)
{
    static bool tried = false;   // one shot per process
    if (tried) return;

    wchar_t dir[MAX_PATH * 2] = {};
    if (!addon_dir_w(dir, MAX_PATH * 2)) return;
    wchar_t flag[MAX_PATH * 2] = {};
    _snwprintf_s(flag, MAX_PATH * 2, _TRUNCATE, L"%smgpu_xefg_smoke.on", dir);
    if (!file_exists_w(flag)) return;
    tried = true;

    wchar_t flag_api[MAX_PATH * 2] = {};
    _snwprintf_s(flag_api, MAX_PATH * 2, _TRUNCATE, L"%smgpu_xefg_smoke.api.on", dir);
    const bool api_on = file_exists_w(flag_api);

    if (device == nullptr)
    {
        mgpu::diag::error("[MGPU][R488] smoke: NOT STARTED - no GPU 1 device at the call site, so there is "
                          "no adapter LUID to build the throwaway device on.");
        return;
    }
    const LUID luid = device->GetAdapterLuid();   // read on the caller's thread, before detaching

    // `where` is a string literal at the only call site, so the pointer stays
    // valid for the process; the LUID is copied by value.
    std::thread([](LUID l, const char *w, bool api)
    {
        // The chain is armed during the game's start-up. Delaying the smoke
        // keeps XeFG's pipeline build out of that load spike. The delay is its
        // own log line so that a launch which dies before the window appears
        // cannot be mistaken for "the smoke never armed".
        char arm[220];
        snprintf(arm, sizeof arm,
                 "[MGPU][R488] smoke: ARMED - thread created at create_present_chain, arm=%s, sleeping "
                 "6000 ms before the first device call. If this is the last R488 line, the launch ended "
                 "during that sleep and nothing of ours had run.",
                 api ? "XEFG(libxess_fg WILL be loaded)" : "HARNESS-ONLY(libxess_fg NOT loaded)");
        mgpu::diag::info(arm);
        Sleep(6000);

        efg_smoke s;
        if (!efg_setup(s, l, w, api)) { efg_cleanup(s); return; }

        if (!api)
        {
            // The harness arm: no vendor code has been entered at this point,
            // and none will be. 900 presents at vsync is ~15 s on a 60 Hz
            // output - long enough to watch, and long enough that a repeat of
            // the "dies ~7 s in" pattern lands inside it.
            mgpu::diag::info("[MGPU][R488] smoke: H[harness] begins - our own chain, our own Present, "
                             "no XeFG, no tags, no status call");
            efg_arm(s, "H[harness]", 900, false, 120);
            efg_cleanup(s);
            return;
        }

        mgpu::diag::info("[MGPU][R488] smoke: phase A begins - FG enabled, NO tags (does XeFG interpolate "
                         "without inputs?)");
        efg_arm(s, "A[no-tags]", 60, false, 20);
        mgpu::diag::info("[MGPU][R488] smoke: phase B begins - same scene, same enabled state, tags added");
        efg_arm(s, "B[tags]", 1800, true, 120);
        efg_cleanup(s);
    }, luid, where, api_on).detach();
}
