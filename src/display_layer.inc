// R448 cut 3: the display layer, un-blockable AND non-blocking for the game.
//
// Cut 1 froze the game. Cut 2 fixed the freeze but cost the game 20 fps, because
// the present hook waited for the GPU twice per frame:
//   - the marquee patch had its own submit + Signal + WaitForSingleObject(500),
//     which drains the whole bridge queue;
//   - disp_push copied into the ring and then waited 2000 ms for its fence.
// Those waits made the game's present thread synchronous with the neural job.
// Measured: [R300l] 59.99 -> 41.99 -> 38.73 FPS once the display window appeared,
// while [R300e2] had GPU1 at 65-78% - the second card was idle, the game was not.
// Cut 3 removes every CPU wait from the caller's thread:
//   - the ring copy runs in a per-slot command list (4 allocators, one per slot,
//     reset only when the slot is handed back, never mid-flight);
//   - the bridge queue signals a shared fence, and the DISPLAY queue waits on it
//     on the GPU (ID3D12CommandQueue::Wait) before reading the ring;
//   - the marquee patch and tag are recorded into the same list, no extra submit.
// The four rules from cut 2 still hold:
//   (a) the caller NEVER waits for the display layer. Ring full -> drop.
//   (b) no swapchain touch while our window is hidden, minimised or occluded.
//   (c) every present is non-blocking (Present(0, DO_NOT_WAIT)); every wait has a
//       timeout; a frame whose deadline is far in the past is dropped.
//   (d) the display thread never raises somebody else's window.
// Watchdog: no progress for 1.5 s while frames are pushed -> hide, mark dead, the
// caller falls back to the old DComp chain ([R455]).

// =====================================================================
// R490 (2b-2): the frame generator's inputs, travelling with the frame
// =====================================================================
//
// 2b-1' proved this layer can PRESENT through the XeFG proxy. What it could not
// answer is whether XeFG will GENERATE here, because generation needs three
// things nobody had wired to this path: depth, motion vectors, and the camera
// constants of the frame being presented.
//
// The hard part is TIME. A frame is pushed on the game's present thread and
// presented a few milliseconds later on the display thread - and by then the
// bridge has already started writing the NEXT frame's depth and vectors. Tagging
// the live textures would describe a frame that no longer exists, and the
// intermittent "one frame of wrong motion" that produces is exactly the kind of
// bug that is blamed on the generator for a week.
//
// So the two inputs are COPIED into the ring, in the same command list as the
// colour copy, under the same slot and the same fence. The tag points at the
// copy. Same frame, by construction rather than by timing.
//
// Sizes measured in this title ([R474]): depth 3840x2160 fmt=41 (D32_FLOAT),
// vectors 1920x1080 fmt=34 (R16G16_FLOAT). Six slots of both is ~250 MB, which
// is why the copies only exist while generation is armed.
struct disp_fg_in
{
    ID3D12Resource *depth = nullptr;
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource *mvec = nullptr;
    D3D12_RESOURCE_STATES mvec_state = D3D12_RESOURCE_STATE_COMMON;
};

// Everything the generator needs to know about this frame that is not in the
// two textures. Filled on the bridge side, consumed on the display side.
struct disp_fg_params
{
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 1.0f;   // same convention the bridge hands DLSS
    float mv_scale_y = 1.0f;
    float frame_ms = 0.0f;     // how long the source frame took
    unsigned reset = 0;        // 1 on the frames after a start or a discontinuity
};

// Defined in xefg_display.inc, later in this translation unit. Declared here so
// the display thread can stay free of vendor types.
bool mgpu_xefg_gen_active();
int  mgpu_xefg_gen_tag(ID3D12CommandList *cl, unsigned present_id, const disp_fg_params *fc,
                       ID3D12Resource *depth, ID3D12Resource *mvec);
void mgpu_xefg_gen_set_present_id(unsigned present_id);
void mgpu_xefg_gen_after_present(unsigned *frames_presented, int *gen_result, unsigned *gen_enabled);
// R493: the vendor pacer's hooks. No-ops unless XeFGPace is on and generation is
// armed, so the display thread never has to ask whether to call them.
void mgpu_xefg_pace_present_begin(unsigned frame_id);
void mgpu_xefg_pace_present_end(unsigned frame_id);
// R509: the other four of XeLL's six required markers. Declared here so the display
// thread stays free of vendor types; the mapping lives in xefg_display.inc.
void mgpu_xefg_pace_sim_begin(unsigned frame_id);
void mgpu_xefg_pace_sim_end(unsigned frame_id);
void mgpu_xefg_pace_submit_begin(unsigned frame_id);
void mgpu_xefg_pace_submit_end(unsigned frame_id);
void mgpu_xefg_pace_report();
// R497: applies the panel's backend/multiplier to the provider. Two integer
// compares when nothing changed, so it is safe to call every iteration.
void mgpu_xefg_apply_mode();

// R490a: the one shader this layer owns - a point-sampled resample of the guide
// depth into the vector resolution. See the long note at disp_fg_scale_init.
struct disp_fg_scale
{
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    UINT inc = 0;
    bool ok = false;
    void *src_seen[6] = {};   // the depth each slot's SRV currently points at
};

namespace
{
    struct disp_frame
    {
        unsigned slot = 0;
        double t_ms = 0.0;
        double max_late_ms = 25.0;   // R471 / C2: how late this frame is still worth showing
        UINT64 xfv = 0;      // value the display queue must wait for before reading this slot
        bool has_fg = false;         // R490: this frame carries generator inputs in its slot
        disp_fg_params fg{};         // R490: and the constants that go with them
        // R507: which BURST of frames this item belongs to. One source frame that
        // produces N+1 frames gives all of them this one id, and the display
        // thread treats an id as indivisible. It never presents a burst with a
        // hole in it, and it never drops one sibling out of a burst - that is the
        // whole defect this field exists to fix.
        unsigned grp = 0;
    };

    struct disp_pipe
    {
        HWND hwnd = nullptr;
        IDXGISwapChain1 *sc = nullptr;
        IDXGISwapChain3 *sc3 = nullptr;
        ID3D12Device *dev = nullptr;
        ID3D12CommandQueue *q = nullptr;          // display's own queue
        ID3D12DescriptorHeap *rtv = nullptr;
        UINT rtv_inc = 0;
        UINT bb_n = 3;                            // R489: back buffers we actually own (the proxy may report another count)
        ID3D12Resource *bb[3] = {};
        ID3D12CommandAllocator *al = nullptr;
        ID3D12GraphicsCommandList *cl = nullptr;
        ID3D12Fence *fe = nullptr;
        HANDLE ev = nullptr;
        UINT64 fv = 0;

        ID3D12Resource *ring[6] = {};
        ID3D12CommandAllocator *cal[6] = {};      // one per slot: reset only when the slot comes back
        ID3D12GraphicsCommandList *ccl[6] = {};
        UINT64 slot_fv[6] = {};                   // last fence value the bridge queue signalled for that slot
        ID3D12Fence *xfe = nullptr;               // bridge queue -> display queue handoff
        UINT64 xfv = 0;
        HANDLE cev = nullptr;                     // only used by the (rare) slot-reuse guard

        // R490 (2b-2): the generator's inputs, one copy per slot, resting in
        // ALL_SHADER_RESOURCE - the state XeFG is told to expect and the state it
        // is expected to hand back.
        ID3D12Resource *fg_d[6] = {};
        ID3D12Resource *fg_m[6] = {};
        UINT fg_dw = 0, fg_dh = 0, fg_mw = 0, fg_mh = 0;
        bool fg_d_scaled = false;                 // R490a: the depth slot copies are resampled, not copied
        disp_fg_scale fgsc;
        unsigned fg_id = 0;                       // XeFG present id: 1, 2, 3 ... one per presented frame
        unsigned long long fg_n1 = 0, fg_n2 = 0, fg_n0 = 0;   // how many frames XeFG actually put up
        unsigned long long fg_n3 = 0, fg_n4 = 0;              // R492b: the real buckets
        int fg_res = 0;                           // last frame_gen_result
        unsigned fg_en = 0;                       // last is_frame_gen_enabled
        int fg_warned = 0;

        std::mutex m;
        std::condition_variable cv;
        std::deque<disp_frame> items;
        unsigned free_slots = 6;   // R472: 2 frames per source exactly filled 4 slots; 6 leaves headroom
        unsigned next_slot = 0;
        bool stop = false;
        bool ready = false;
        HWND game = nullptr;
        std::atomic<bool> shown{true};
        bool manual = false;
        std::atomic<bool> panel_hide{false};
        std::atomic<bool> dead{false};
        std::atomic<unsigned long long> frames_seen{0};
        unsigned long long last_seen = 0;         // bridge-side copy of frames_seen
        double last_progress = 0.0;               // when frames_seen last moved (bridge side)
        volatile bool occluded = false;
        double last_occl_test = 0.0;              // R494: when DXGI was last asked whether the output is free
        HWND last_fg = nullptr;
        std::thread th;
        UINT w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        unsigned long long presented = 0;
        unsigned long long skipped = 0;           // not attempted: window hidden
        unsigned long long stale = 0;             // deadline far in the past (>100 ms)
        unsigned long long late  = 0;             // R471 / C2: past its own allowance (half a source period)
        unsigned long long refused = 0;           // Present said WAS_STILL_DRAWING
        unsigned long long dropped = 0;           // ring full at disp_push
        unsigned long long waited = 0;            // slot-reuse guard actually had to wait
        unsigned long long copy_behind = 0;        // R495b: the display copy was not done when Present was due
        unsigned long long superseded = 0;         // R501: queued frames skipped because a newer one existed
        // R507: the two ways a BURST (not a sibling) may be dropped. Kept apart
        // from superseded because they say different things: late_group means the
        // source was too slow to show us, cap_drop means we were too slow to show
        // it. A run where cap_drop climbs and late_group does not is the display
        // thread failing; the opposite is the source stalling. Reading them as one
        // number is how "it stutters" becomes unattributable.
        unsigned long long late_group = 0;
        unsigned long long cap_drop   = 0;
        unsigned grp_seq = 0;                      // R507: last burst id handed out
        unsigned last_grp = 0;                     // R507: burst id of the last item presented
        double last_grp_t0 = 0.0;                  // R507: its first target, for the period estimate
        double period_est = 0.0;                   // R507: MEASURED source period, ms (0 = not yet)
        double pint[120] = {};                    // present-to-present intervals, ms
        int pint_n = 0;
        LONGLONG last_pqpc = 0;
        UINT last_rc = 0;
        LONGLONG last_qpct = 0;
        UINT sync_count = 0;
        double composite_hz = 0.0;                // refresh our chain is actually synced to (DXGI counters)
        // R491: how many vblanks each Present actually consumed, and what mode the
        // panel is in. Both are needed because "composite=80 Hz" CANNOT be read as
        // "the panel runs at 80 Hz": SyncRefreshCount advances once per retired
        // frame, so 40 presents/s x 2 frames each reads 80 Hz on a 160 Hz panel and
        // 80 Hz on an 80 Hz panel alike. The two are told apart by the mode line and
        // by the delta histogram, not by that number.
        UINT refresh_hz = 0;
        unsigned long long vbd[9] = {};           // vblanks per Present: index 1..7, 8 = "8 or more"
        unsigned long long vbd_n = 0;
        // R491b: frames actually put on screen, summed from GetLastPresentStatus
        // (1 for a Present nobody tagged). This is the numerator the pacing question
        // needs: with the panel mode known, vblanks/frame = refresh / (frames/s).
        // 2.00 means "every other vblank", which is exactly even 80 fps on a 160 Hz
        // panel - the target for 2x, and the thing that does NOT exist at 4x, where
        // 40 fps x 4 = 160 = every single vblank with zero headroom.
        unsigned long long frames_total = 0;
        // R491c: the report needs a WINDOWED rate, not the cumulative average.
        // Measured 2026-10-30 23:45: 1917 frames over 108.94 s printed "17.6/s",
        // because the denominator included the 3360 presents skipped while the
        // window was hidden during loading. The frame count was right; the rate
        // was a session average wearing a rate's clothes. Two deltas, one window.
        unsigned long long last_rep_frames = 0;
        double last_rep_ms = 0.0;
        // R495: where does a display-thread frame actually go? Measured
        // 2026-10-31 00:22 at 4X: the game pushed every 25.0 ms ([R300l] 40.0 fps)
        // while this thread consumed one frame every 32.4 ms, so every fourth frame
        // was dropped as late. Three phases, three numbers, so the answer stops
        // being a guess: wait for an item, copy into the back buffer (including the
        // CPU fence wait), and the Present call itself - which above 2X contains the
        // provider's whole burst and is the prime suspect.
        double ph_item[120] = {};
        double ph_copy[120] = {};
        double ph_pres[120] = {};
        int ph_n = 0;
        int vsync = 1;                            // 1 = Present(1,0); flag mgpu_disp_novsync.on -> Present(0,DO_NOT_WAIT)
        double t0 = 0.0;
    };

    disp_pipe &dp()
    {
        static disp_pipe p;
        return p;
    }

    double disp_now_ms()
    {
        static LARGE_INTEGER f{};
        if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
    }

    // ---- R510: THE MULTIPLIER BUDGET -------------------------------------
    //
    // A generated frame costs one vblank. N of them plus the real one cost N+1
    // vblanks, and a source frame that arrives faster than that makes the delivery
    // rate a FRACTION of what was asked - measured three times now:
    //   2026-10-31 03:09  DLSS-G 4X, 40 fps source, 165 Hz  -> 157.7/s of 160
    //   2026-10-31 03:5x  XeSS   4X, 40 fps source, 165 Hz  -> 160/152 bouncing
    //   2026-10-31 02:4x  DLSS-G 3X, 65 fps source, 165 Hz  -> 62/s of 120
    //
    // The operator's own reading of the rule is the one that holds: at 38 fps a 4X
    // feels clean, at 40 it does not. Turning that into arithmetic needs one honest
    // constant - the HEADROOM - because 4X at 40 fps on 165 Hz is 4 x 6.06 = 24.24 ms
    // inside a 25.00 ms period, i.e. 96.97% of the panel, and the missing 3% has to
    // absorb every jitter there is (GPU scheduling, DWM, the game's own presents).
    //
    // 5% reproduces all three measurements and the operator's three verdicts:
    //   usable vblanks = T_src * 0.95 / T_vb
    //   40 fps -> 3.92 -> 3 vblanks -> 3X (and 4X is exactly the case that fails)
    //   38 fps -> 4.13 -> 4 vblanks -> 4X (matches "38 fps 4X feels clean")
    //   60 fps -> 2.61 -> 2 vblanks -> 2X (matches "锁 60 用 4X 也大概率出问题")
    //   65 fps -> 2.41 -> 2 vblanks -> 2X
    //
    // The SAME rule then covers 3X at 65 fps, which is the arm that produced 62/s of
    // 120 earlier tonight. It is one rule for every backend and every source rate,
    // because it is expressed in vblanks rather than in a constant somebody tuned.
    //
    // Returns 0 for "cannot say yet" (no measured source period, or the panel mode
    // was unreadable), and every caller must treat 0 as "do not clamp". FgBudget=0
    // disables it outright.
    double disp_source_period_ms()
    {
        disp_pipe &p = dp();
        return (p.period_est > 1.0) ? p.period_est : 0.0;
    }

    double disp_vblank_ms()
    {
        disp_pipe &p = dp();
        return (p.refresh_hz != 0u) ? (1000.0 / (double)p.refresh_hz) : 0.0;
    }

    // Generated frames per source frame that this panel and this source rate can
    // actually deliver: 1 = 2X, 3 = 4X. 0 = no opinion.
    unsigned disp_budget_gen()
    {
        static const int on = mgpu::gpu1::ui_ini_read("FgBudget", 1);
        if (on == 0) return 0u;
        const double t_src = disp_source_period_ms();
        const double t_vb = disp_vblank_ms();
        if (t_src <= 0.0 || t_vb <= 0.0) return 0u;
        const double vbl = (t_src * 0.95) / t_vb;
        if (vbl < 2.0)
        {
            // R510b: EVEN 2X DOES NOT FIT. This is the 60 Hz player with a 60 fps
            // source: 2 frames per source frame is 120 frames/s on a 60 Hz panel, and
            // no clamp can make that work - the second frame of every pair has no
            // vblank to land in. 2X stays as the floor because turning generation off
            // under the operator is worse than showing them the truth, but the run
            // gets told exactly what to do about it, once.
            static int said_slow = 0;
            if (said_slow++ == 0)
            {
                char sl[320];
                snprintf(sl, sizeof sl,
                         "[MGPU][R510] source %.2f ms is too fast for ANY frame generation on this panel "
                         "(2 frames need 2 vblanks = %.2f ms). 2X is the floor and will be delivered as about one "
                         "frame per source frame. Fix: lower the game's frame cap to %.0f fps or below, or turn "
                         "frame generation off. Refresh %.1f Hz.",
                         t_src, 2.0 * t_vb, 1000.0 / (2.0 * t_vb), 1000.0 / t_vb);
                mgpu::diag::warn(sl);
            }
            return 1u;
        }
        unsigned n = (unsigned)((int)vbl - 1);
        if (n < 1u) n = 1u;
        if (n > 3u) n = 3u;
        return n;
    }

    LRESULT CALLBACK disp_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        // R465: this window covers the whole screen, and Windows draws the hardware
        // cursor over whatever window it believes the pointer is in. With the game
        // capturing the mouse the pointer parks at the centre, so an arrow froze in
        // the middle of the picture (it moved when Home hid this window). Hide it.
        if (m == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
        return DefWindowProcW(h, m, w, l);
    }

    void disp_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                      D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        if (a == b || r == nullptr) return;
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &br);
    }

    // (d) housekeeping: only our own window, never the peer's. Runs on the display
    // thread every four frames seen - not every four presented, because while hidden
    // we still have to notice that we may show it again.
    void disp_housekeeping(disp_pipe *p)
    {
        HWND fg = GetForegroundWindow();
        DWORD fpid = 0; if (fg != nullptr) GetWindowThreadProcessId(fg, &fpid);
        if (p->game == nullptr && fg != nullptr && fpid == GetCurrentProcessId())
            p->game = fg;
        const bool game_fg = (fg != nullptr) && ((fg == p->game) || (fpid == GetCurrentProcessId()));
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F6) & 1))
            p->manual = !p->manual;
        const bool panel = p->panel_hide.load(std::memory_order_relaxed) || g_panel_live.load(std::memory_order_relaxed);
        const bool want = game_fg && !p->manual && !panel
            && !p->dead.load(std::memory_order_relaxed) && !p->occluded;
        if (want != p->shown.load(std::memory_order_relaxed))
        {
            p->shown.store(want, std::memory_order_relaxed);
            ShowWindow(p->hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
            char l[240];
            snprintf(l, sizeof l, "[MGPU][R454] output %s (fg pid=%lu manual=%d panel=%d)", want ? "shown" : "hidden",
                     (unsigned long)fpid, (int)p->manual, (int)panel);
            mgpu::diag::info(l);
        }
        if (want && fg != p->last_fg)
            SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Re-assert the z-order periodically too: a game window that is itself
        // topmost can end up in front of us without the foreground changing, and
        // then the eye alternately sees the game's own frame and ours - which reads
        // as flicker, not as a frame rate.
        else if (want && (p->presented % 32ull) == 0ull)
            SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        p->last_fg = fg;
    }

    void disp_thread(disp_pipe *p)
    {
        for (;;)
        {
            disp_frame f{};
            unsigned long long superseded = 0;
            unsigned long long late_grp = 0;
            unsigned long long cap_drop = 0;
            const double t_item = disp_now_ms();
            {
                std::unique_lock<std::mutex> lk(p->m);
                p->cv.wait(lk, [&] { return p->stop || !p->items.empty(); });
                if (p->stop) return;
                // R501 + R506, RETRACTED 2026-10-31 03:0x. Both said "take the
                // newest", one at a time and then again as the fix for the other's
                // hang. The reasoning was sound for a producer that pushes ONE frame
                // per source frame and wrong for this one, which pushes N+1 at once:
                // "keep the newest" is a per-ITEM rule, and per-item rules cannot
                // express "these four frames are one picture". R501's own words below
                // are kept because the sentence "at 4X one push is four frames" was
                // correct and the conclusion drawn from it was backwards - it is
                // exactly why the newest is NOT the right thing to keep.
                //
                //   "A frame that has been waiting behind another is, by definition,
                //    older news... at 4X one push is four frames, so presenting a
                //    superseded push costs a whole stride."
                //
                // What is still true from R506: R505's never-skip rule, applied to a
                // queue consumed strictly in order, fell further behind every frame
                // and never caught up - Present blocked, the picture stopped, and the
                // panel could not be opened (measured 2026-10-31 02:0x; the game had
                // to be killed from Task Manager). A finite lag is survivable and an
                // unbounded one is a hang. R507 keeps that lesson and fixes the unit.
                // R507: DROP WHOLE BURSTS, NEVER SIBLINGS.
                //
                // The rule this replaces kept only the newest queued item and threw
                // the rest away. That reads as correct for a game presenting one
                // frame per source frame, and it is catastrophic here, because one
                // source frame arrives at this queue as TWO OR FOUR items pushed
                // back to back. At queue=[generated, real], "keep the newest" throws
                // the GENERATED frame away every time - the multiplier silently
                // collapses and the panel reads a fraction where an integer belongs.
                // Measured 2026-10-31 02:46, 40 fps source at x3: 120 frames/s were
                // pushed, 62 reached the screen, and superseded climbed 50/s. The
                // operator saw "40/60" and reported a frame mismatch; the mismatch
                // was this loop.
                //
                // So the unit is the BURST. A burst is dropped whole or not at all,
                // and both bounds are expressed in the MEASURED source period so
                // that they scale with the source rate, the refresh rate and the
                // multiplier instead of being a constant someone has to re-tune per
                // configuration - which is how the previous two attempts at this
                // (a 25 ms deadline, then a one-period late tolerance) both failed.
                //
                //   late_group  the head burst is older than one measured period AND
                //               a newer burst is already queued. Showing it now would
                //               put stale news on the screen; dropping it costs a
                //               whole stride, which is the price of being late.
                //   cap_drop    the queue spans more than two periods. THIS is what
                //               keeps R505 from coming back: lag can never grow
                //               without bound, because past two periods the oldest
                //               bursts are discarded until it does. It also cannot
                //               deadlock - the producer is never asked to wait.
                //
                // A queue holding ONE burst is always presented in full, however
                // late it is. That is the case a slow source produces, and dropping
                // frames there would remove content for no benefit.
                const double per = (p->period_est > 1.0) ? p->period_est : 25.0;
                const double now0 = disp_now_ms();
                while (p->items.size() > 1)
                {
                    const disp_frame &head = p->items.front();
                    bool newer_burst = false;
                    for (const disp_frame &it : p->items)
                        if (it.grp != head.grp) { newer_burst = true; break; }
                    if (!newer_burst) break;
                    const bool late_now = (now0 - head.t_ms) > per;
                    const bool over_cap = (p->items.back().t_ms - head.t_ms) > (2.0 * per);
                    if (!late_now && !over_cap) break;
                    const unsigned g = head.grp;
                    do
                    {
                        p->items.pop_front();
                        ++p->free_slots;
                        ++superseded;
                    } while (!p->items.empty() && p->items.front().grp == g);
                    if (over_cap) ++cap_drop; else ++late_grp;
                }
                f = p->items.front();
                p->items.pop_front();
            }
            const double t_got = disp_now_ms();
            if (superseded > 0) p->superseded += superseded;
            if (late_grp > 0) p->late_group += late_grp;
            if (cap_drop > 0) p->cap_drop += cap_drop;

            // R507: the source period, MEASURED from the spacing of burst starts.
            // Both bounds above are expressed in it, so it has to be observed
            // rather than configured: a 40 fps source and a 165 fps one then need
            // no edit anywhere. Before the first two bursts it reads 25 ms, which
            // only has to be sane, not exact - it is a bound, not a schedule.
            if (f.grp != p->last_grp)
            {
                p->last_grp = f.grp;
                if (p->last_grp_t0 > 0.0)
                {
                    const double d = f.t_ms - p->last_grp_t0;
                    if (d > 3.0 && d < 60.0)
                        p->period_est = (p->period_est > 1.0) ? (p->period_est * 0.75 + d * 0.25) : d;
                    if (p->period_est > 0.0 && p->period_est < 4.0) p->period_est = 4.0;
                    if (p->period_est > 40.0) p->period_est = 40.0;
                }
                p->last_grp_t0 = f.t_ms;
            }
            const unsigned long long seen = p->frames_seen.fetch_add(1, std::memory_order_relaxed) + 1;

            // R497: the panel is the door, and this is where its answer reaches the
            // provider - before the frame that will use it, never in the middle of a
            // burst. Changing the multiplier or switching generation off takes effect
            // on the next Present instead of the next restart.
            mgpu_xefg_apply_mode();

            // (b) the window decides whether we may touch the swapchain at all.
            const bool may_present = p->shown.load(std::memory_order_relaxed)
                && !p->panel_hide.load(std::memory_order_relaxed)
                && !p->dead.load(std::memory_order_relaxed) && !p->occluded
                && p->hwnd != nullptr && IsWindowVisible(p->hwnd) && !IsIconic(p->hwnd);

            if (may_present)
            {
                double now = disp_now_ms();
                // R509: NEVER SLEEP FOR LESS THAN ONE VBLANK.
                //
                // The target times this layer computes are evenly spaced inside one
                // source period: at 4X on a 40 fps source they are 6.25 ms apart. The
                // panel's vblanks are 6.0606 ms apart (165 Hz). Two lattices with
                // different periods drift against each other by 25.0 - 4*6.0606 =
                // 0.76 ms per source frame, and every time the accumulated drift
                // crosses a vblank the Present has to wait one extra one - which is a
                // whole STRIDE, because a stride here is what the operator sees as the
                // picture jumping forward.
                //
                // Measured 2026-10-31 03:09 at 4X / 40 fps / 165 Hz: on screen 157.7/s
                // against 160 wanted, dropped +1.4/s, vblanks/frame 1.05, and the panel
                // read 40/157.7 - a non-integer ratio for the third time, this time
                // with the drop policy already fixed.
                //
                // The fix is not to drop less, it is to stop competing with the chain:
                // a sleep SHORTER THAN A VBLANK buys nothing at all, because the
                // Present that follows blocks until the next vblank anyway. Sleeping
                // 6.25 ms to hit an arithmetic target and then letting Present wait for
                // a physical one is how the +0.19 ms phase error per present gets in.
                // So when the remaining wait is under one vblank + 10%, skip the sleep
                // and let the flip queue set the cadence - which is exactly what Intel's
                // own frame generation sample does: no application-side target times at
                // all, the chain and XeLL pace the frame.
                //
                // 2X and 3X are unaffected by construction: their steps (12.5 and
                // 8.33 ms) are above one vblank, so they keep their even spacing. That
                // is the property to preserve - measured 80.0/s and 120.0/s exactly.
                const double t_vb = (p->refresh_hz != 0u) ? (1000.0 / (double)p->refresh_hz) : 0.0;
                const double wait_ms = f.t_ms - now;
                if (wait_ms > 0.0 && wait_ms <= 20.0 && (t_vb <= 0.0 || wait_ms > t_vb * 1.10))
                {
                    Sleep((DWORD)wait_ms);
                    now = disp_now_ms();
                }
                else if (wait_ms > 0.0 && wait_ms <= 20.0)
                {
                    static int said_skip = 0;
                    if (said_skip++ == 0)
                    {
                        char sl[260];
                        snprintf(sl, sizeof sl,
                                 "[MGPU][R509] target-time sleep skipped: %.2f ms is under one vblank (%.2f ms) - "
                                 "the flip queue paces this frame. 4X on a 165 Hz panel is exactly this case.",
                                 wait_ms, t_vb);
                        mgpu::diag::info(sl);
                    }
                }
                // R501: THE DEADLINE IS GONE. It used to drop anything later than one
                // source period, which at 4X is 25.0 ms against a measured 26.05 ms
                // burst - so every frame was "late" by construction and one drop cost
                // four frames. Measured 01:01: age=23.81 against allowance=23.33.
                //
                // What is left is not a schedule, it is a sanity bound: a frame older
                // than 200 ms belongs to a loading screen, not to this moment. Being a
                // little late is invisible; being replaced by four-frame jumps is not.
                if (f.t_ms > 0.0 && now - f.t_ms > 200.0)
                {
                    ++p->stale;
                }
                else
                {
                    // Cross-queue handoff: the display queue waits on the GPU for the
                    // bridge's copy of this slot. No CPU wait, no game-side stall.
                    p->q->Wait(p->xfe, f.xfv);
                    // R489: the index comes from the chain we present to, which
                    // after a handover is the proxy and may report a different
                    // buffer count than the three this layer was built for.
                    UINT idx = p->sc3 ? p->sc3->GetCurrentBackBufferIndex() : 0u;
                    if (idx >= p->bb_n)
                    {
                        static bool idx_logged = false;
                        if (!idx_logged)
                        {
                            idx_logged = true;
                            char il[220];
                            snprintf(il, sizeof il, "[MGPU][R489] back buffer index %u is outside the %u we hold - "
                                     "clamping to 0 for this and every later frame", idx, p->bb_n);
                            mgpu::diag::warn(il);
                        }
                        idx = 0u;
                    }
                    disp_barrier(p->cl, p->ring[f.slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    disp_barrier(p->cl, p->bb[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                    p->cl->CopyResource(p->bb[idx], p->ring[f.slot]);
                    disp_barrier(p->cl, p->bb[idx], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                    disp_barrier(p->cl, p->ring[f.slot], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    // R490: tag THIS frame for the generator, with the copies that
                    // belong to it. They are already resting in ALL_SHADER_RESOURCE -
                    // the state XeFG is told to expect - so the tag needs no barrier
                    // of its own. If the tag fails the frame still presents, just
                    // without an interpolated sibling.
                    unsigned pid = 0;
                    if (f.has_fg && mgpu_xefg_gen_active())
                    {
                        const unsigned id = ++p->fg_id;
                        if (mgpu_xefg_gen_tag(p->cl, id, &f.fg, p->fg_d[f.slot], p->fg_m[f.slot]) == 0)
                            pid = id;
                        else
                            --p->fg_id;   // a tag that did not land is not a frame the generator saw
                    }
                    // R509: XeLL's six required markers, in the only order this
                    // layer can honestly produce them. sim_* brackets the recording
                    // of this frame's copy, submit_* its execution and fence wait,
                    // present_* the Present itself (below). The id is the same one
                    // xefgSwapChainSetPresentId gets - markers and present id are one
                    // timeline for the provider. Sent only for frames that actually
                    // carry generator inputs: a frame the generator never saw must not
                    // appear in its record.
                    if (pid != 0) mgpu_xefg_pace_sim_begin(pid);
                    p->cl->Close();
                    if (pid != 0) { mgpu_xefg_pace_sim_end(pid); mgpu_xefg_pace_submit_begin(pid); }
                    p->q->ExecuteCommandLists(1, (ID3D12CommandList **)&p->cl);
                    if (pid != 0) mgpu_xefg_gen_set_present_id(pid);
                    p->q->Signal(p->fe, ++p->fv);
                    p->fe->SetEventOnCompletion(p->fv, p->ev);
                    // R495b: 100 ms -> 2 ms, and a timeout is counted rather than
                    // fatal. The copy and the Present that follows are on the SAME
                    // queue, so the flip cannot overtake the copy - waiting on the CPU
                    // adds the GPU's remaining time to our frame cost and buys no
                    // ordering. Measured 2026-10-31 00:33 at 4X: copy=7-9 ms against a
                    // 4K copy that is ~0.1 ms of bandwidth, sum 26.0 ms against a
                    // 25.0 ms source period. Whether that 7-9 ms is our copy or the
                    // back buffer still being scanned out is what the next report's
                    // present= number will settle: if present grows by the same amount,
                    // the cost is the burst and not the wait.
                    if (WaitForSingleObject(p->ev, 2) != WAIT_OBJECT_0) ++p->copy_behind;
                    if (pid != 0) mgpu_xefg_pace_submit_end(pid);
                    {
                        // R493: hand the pacer the present's boundaries. Before
                        // the call it may also wait (its own sleep primitive); after
                        // it, it records what happened, which is the timestamp its
                        // frame report later hands back to us.
                        if (pid != 0) mgpu_xefg_pace_present_begin(pid);
                        const double t_copy1 = disp_now_ms();
                        const HRESULT hr = p->vsync ? p->sc->Present(1, 0)
                                                                  : p->sc->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
                        const double t_pres1 = disp_now_ms();
                        if (p->ph_n < 120)
                        {
                            p->ph_item[p->ph_n] = t_got - t_item;
                            p->ph_copy[p->ph_n] = t_copy1 - t_got;
                            p->ph_pres[p->ph_n] = t_pres1 - t_copy1;
                            ++p->ph_n;
                        }
                        if (pid != 0) mgpu_xefg_pace_present_end(pid);
                        if (hr == DXGI_STATUS_OCCLUDED)
                        {
                            p->occluded = true;
                            mgpu::diag::warn("[MGPU][R454] output occluded - presents suspended");
                        }
                        else if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
                        {
                            ++p->refused;
                        }
                        else if (SUCCEEDED(hr))
                        {
                            ++p->presented;
                            // R490: the only measure of whether generation happened that
                            // does not depend on anyone's eyes - DXGI's own count of
                            // frames this chain put up for that one Present.
                            unsigned fpn = 1;
                            if (pid != 0)
                            {
                                int fres = 0; unsigned fen = 0;
                                mgpu_xefg_gen_after_present(&fpn, &fres, &fen);
                                // R492b: bucket the ACTUAL count. The old form put
                                // everything at 2 or more into one counter, so a
                                // 4-frame present and a 2-frame present read the
                                // same - measured 2026-10-31: "fg frames 2:1917"
                                // was really 4 frames every time, and the number
                                // that gave it away was frames_total/presents.
                                if (fpn == 0u) ++p->fg_n0;
                                else if (fpn == 1u) ++p->fg_n1;
                                else if (fpn == 2u) ++p->fg_n2;
                                else if (fpn == 3u) ++p->fg_n3;
                                else ++p->fg_n4;
                                p->fg_res = fres;
                                p->fg_en = fen;
                            }
                            p->frames_total += fpn;   // R491b: what actually reached the screen
                            // R498: and the same number feeds the overlay, so "源帧 /
                            // 上屏" reports what left the GPU rather than how many
                            // times we called into the layer.
                            mgpu::fgmode::out_count().fetch_add((unsigned long long)fpn, std::memory_order_relaxed);
                            // R460: how fast does this chain actually retire presents, and
                            // what refresh is it synced to? GetFrameStatistics answers both
                            // from DXGI's own counters instead of from our assumptions.
                            LARGE_INTEGER pn{};
                            QueryPerformanceCounter(&pn);
                            if (p->last_pqpc != 0)
                            {
                                static LARGE_INTEGER pq{};
                                if (pq.QuadPart == 0) QueryPerformanceFrequency(&pq);
                                p->pint[p->pint_n++ % 120] = (double)(pn.QuadPart - p->last_pqpc) * 1000.0 / (double)pq.QuadPart;
                                if (p->pint_n > 120) p->pint_n = 120;
                            }
                            p->last_pqpc = pn.QuadPart;
                            DXGI_FRAME_STATISTICS fs{};
                            if (SUCCEEDED(p->sc->GetFrameStatistics(&fs)))
                            {
                                // R491: the delta BEFORE last_rc is overwritten.
                                // One entry per Present, i.e. per window between two
                                // of our own presents - with generation on that
                                // window should hold 2 retired frames and, on a
                                // 160 Hz panel driven at 40 fps, 4 vblanks.
                                if (p->last_rc != 0 && fs.SyncRefreshCount > p->last_rc)
                                {
                                    const UINT d = fs.SyncRefreshCount - p->last_rc;
                                    ++p->vbd[(d < 8u) ? d : 8u];
                                    ++p->vbd_n;
                                }
                                if (p->last_rc != 0 && fs.SyncQPCTime.QuadPart > p->last_qpct)
                                {
                                    LARGE_INTEGER fq2{}; QueryPerformanceFrequency(&fq2);
                                    p->composite_hz = (double)(fs.SyncRefreshCount - p->last_rc) * (double)fq2.QuadPart
                                                      / (double)(fs.SyncQPCTime.QuadPart - p->last_qpct);
                                }
                                p->last_rc = fs.SyncRefreshCount;
                                p->last_qpct = fs.SyncQPCTime.QuadPart;
                                p->sync_count = fs.SyncRefreshCount;
                            }
                        }
                        p->al->Reset();
                        p->cl->Reset(p->al, nullptr);
                    }
                }
            }
            else
            {
                ++p->skipped;
                // R494: OCCLUSION MUST NOT BE A ONE-WAY LATCH.
                //
                // Measured 2026-10-31 00:2x: an always-on-top Task Manager made
                // Present return DXGI_STATUS_OCCLUDED once, and nothing in this file
                // ever cleared `occluded` - so the layer hid its window and stopped
                // presenting for the rest of the session. Reopening the panel showed
                // whatever frame was left in the chain, which reads as "the picture
                // is frozen but the game is running".
                //
                // Rule (b) ("do not touch the swapchain while hidden/occluded") is
                // still right - it is there because presenting into an occluded output
                // is what used to stall for half a second. DXGI has the one call that
                // respects it: Present(0, DXGI_PRESENT_TEST) presents NOTHING and only
                // reports whether the output is free again. Polling it every 250 ms
                // turns a permanent freeze into a quarter-second recovery.
                if (p->occluded)
                {
                    const double nowtest = disp_now_ms();
                    if (nowtest - p->last_occl_test > 250.0)
                    {
                        p->last_occl_test = nowtest;
                        const HRESULT th = p->sc->Present(0, DXGI_PRESENT_TEST);
                        // Anything that is not "still occluded" means the output is
                        // usable again. Being told something unexpected is not a
                        // reason to keep the picture frozen.
                        if (th != DXGI_STATUS_OCCLUDED)
                        {
                            p->occluded = false;
                            char l2[200];
                            snprintf(l2, sizeof l2, "[MGPU][R454] output free again (PRESENT_TEST hr=0x%08X) - "
                                     "presents resume", (unsigned)th);
                            mgpu::diag::warn(l2);
                        }
                    }
                }
            }

            if ((seen % 4ull) == 0ull) disp_housekeeping(p);

            if (p->presented != 0ull && (p->presented % 120ull) == 0ull)
            {
                const double sec = (disp_now_ms() - p->t0) / 1000.0;
                double ip50 = 0.0, ip99 = 0.0;
                if (p->pint_n >= 8)
                {
                    double sp[120];
                    memcpy(sp, p->pint, sizeof(double) * (size_t)p->pint_n);
                    std::sort(sp, sp + p->pint_n);
                    ip50 = sp[p->pint_n / 2];
                    ip99 = sp[(p->pint_n * 99) / 100];
                }
                char l[860];
                // R491b/c: the two derived numbers that answer the pacing question
                // without guessing, over the LAST REPORT WINDOW ONLY. `frames/s` is
                // measured, `vblanks/frame` divides the panel's own refresh by it:
                // 2.00 = every other vblank = even spacing at that budget.
                const double now_ms = disp_now_ms();
                const double dsec = (p->last_rep_ms > 0.0) ? (now_ms - p->last_rep_ms) / 1000.0 : 0.0;
                const unsigned long long dframes = p->frames_total - p->last_rep_frames;
                const double f_fps = (dsec > 0.05) ? (double)dframes / dsec : 0.0;
                p->last_rep_ms = now_ms;
                p->last_rep_frames = p->frames_total;
                const double vb_per_frame = (f_fps > 0.5 && p->refresh_hz != 0u)
                                          ? (double)p->refresh_hz / f_fps : 0.0;
                snprintf(l, sizeof l,
                         "[MGPU][R448] display: %llu presents in %.2fs = %.1f fps (skipped=%llu stale=%llu late=%llu refused=%llu dropped=%llu reuse_wait=%llu superseded=%llu late_grp=%llu cap=%llu) | interval p50=%.2f p99=%.2f ms | dxgi sync=%u composite=%.1f Hz | fg frames per present 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu id=%u res=%d en=%u | vblanks/present 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7:%llu 8+:%llu mode=%uHz | on screen: %llu frames = %.1f/s (window), %.2f vblanks/frame",
                         (unsigned long long)p->presented, sec, sec > 0.0 ? (double)p->presented / sec : 0.0,
                         (unsigned long long)p->skipped, (unsigned long long)p->stale,
                         (unsigned long long)p->late,
                         (unsigned long long)p->refused, (unsigned long long)p->dropped,
                         (unsigned long long)p->waited, (unsigned long long)p->superseded,
                         (unsigned long long)p->late_group, (unsigned long long)p->cap_drop,
                         ip50, ip99, p->sync_count, p->composite_hz,
                         p->fg_n0, p->fg_n1, p->fg_n2, p->fg_n3, p->fg_n4, p->fg_id, p->fg_res, p->fg_en,
                         p->vbd[1], p->vbd[2], p->vbd[3], p->vbd[4], p->vbd[5], p->vbd[6], p->vbd[7], p->vbd[8],
                         p->refresh_hz,
                         p->frames_total, f_fps, vb_per_frame);
                mgpu::diag::info(l);
                if (p->presented >= 120ull) p->pint_n = 0;
                // R493: what the vendor pacer measured, if it is armed. One line
                // per report window, and it is the only per-frame timing we have:
                // counters cannot tell two frames from four.
                mgpu_xefg_pace_report();
                // R495: the three phase medians. A frame budget is only worth
                // arguing about once it is decomposed: 40 fps of source is 25.0 ms,
                // and whichever of these three crosses it is the one to fix.
                if (p->ph_n >= 16)
                {
                    double a[120], b[120], c[120];
                    memcpy(a, p->ph_item, sizeof(double) * (size_t)p->ph_n);
                    memcpy(b, p->ph_copy, sizeof(double) * (size_t)p->ph_n);
                    memcpy(c, p->ph_pres, sizeof(double) * (size_t)p->ph_n);
                    std::sort(a, a + p->ph_n);
                    std::sort(b, b + p->ph_n);
                    std::sort(c, c + p->ph_n);
                    char pl[300];
                    snprintf(pl, sizeof pl,
                             "[MGPU][R495] display thread p50: wait_item=%.2f copy=%.2f present=%.2f ms "
                             "(sum %.2f vs source period; n=%d)",
                             a[p->ph_n / 2], b[p->ph_n / 2], c[p->ph_n / 2],
                             a[p->ph_n / 2] + b[p->ph_n / 2] + c[p->ph_n / 2], p->ph_n);
                    mgpu::diag::info(pl);
                    p->ph_n = 0;
                }
            }

            {
                std::lock_guard<std::mutex> lk(p->m);
                ++p->free_slots;
            }
            p->cv.notify_all();
        }
    }

    bool disp_init(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        disp_pipe &p = dp();
        if (p.ready) return (p.w == w && p.h == h && p.fmt == fmt);
        if (p.dead.load(std::memory_order_relaxed)) return false;
        if (p.dev == nullptr) p.dev = st().device;
        if (p.dev == nullptr || w == 0 || h == 0) return false;

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HWND fg_before = GetForegroundWindow();

        static bool cls = false;
        if (!cls)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = disp_wndproc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MgpuDisp";
            RegisterClassExW(&wc);
            cls = true;
        }
        // Cut 3 drops WS_EX_LAYERED. A layered window forces DWM down a different
        // composition path and buys us nothing: alpha was fixed at 255.
        p.hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"MgpuDisp", L"mgpu disp", WS_POPUP, 0, 0, (int)w, (int)h,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (p.hwnd == nullptr) return false;
        // R489: the handover arm creates the chain while this window is still
        // HIDDEN. XeFG has to create its own chain on this same HWND, and a
        // window that already covers the whole screen sits in DWM's fullscreen
        // path - exactly the state that can refuse a second flip chain with
        // E_ACCESSDENIED, which is 2b-1's failure (0x80070005 at 3 buffers and
        // again at 2). Showing it a few milliseconds later removes the variable.
        const bool ho_stage = mgpu_xefg_handover_wanted();
        if (!ho_stage)
        {
            ShowWindow(p.hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(p.hwnd, HWND_TOPMOST, 0, 0, (int)w, (int)h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        {
            char l[260];
            RECT wr{}; GetWindowRect(p.hwnd, &wr);
            DWORD fpid = 0; GetWindowThreadProcessId(fg_before, &fpid);
            wchar_t ftitle[128]{}; if (fg_before) GetWindowTextW(fg_before, ftitle, 128);
            snprintf(l, sizeof l, "[MGPU][R451] display win=%p rect=(%ld,%ld)-(%ld,%ld) fg_before=%p pid=%lu title=%ls fg_after=%p visible=%d",
                     (void *)p.hwnd, wr.left, wr.top, wr.right, wr.bottom,
                     (void *)fg_before, (unsigned long)fpid, ftitle,
                     (void *)GetForegroundWindow(), (int)IsWindowVisible(p.hwnd));
            mgpu::diag::info(l);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(p.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&p.q)))) return false;

        IDXGIFactory2 *fac = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = w; scd.Height = h; scd.Format = fmt;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // R489: when the XeFG handover is armed the chain is created with TWO
        // buffers, matching the throwaway chain 2a proved init on. With three,
        // XeFG's own CreateSwapChainForHwnd came back E_ACCESSDENIED (0x80070005)
        // and InitFromSwapChain failed -17 in 52 ms, before any pipeline work.
        // Buffer count is the one shape difference that a re-created chain on the
        // same HWND can plausibly trip over, so it is the first variable.
        // R504: the buffer count is a PERFORMANCE knob now, not a workaround.
        //
        // 2 was chosen during 2b-1, while the chain was still created by us - and the
        // four failures there were E_ACCESSDENIED for a SECOND chain on the same HWND,
        // never about the count (3 buffers failed and 2 buffers failed identically).
        // With InitFromSwapChainDesc nobody creates a chain but XeFG, so this number
        // is simply what the provider builds for us.
        //
        // Why it might matter: one Present delivers N+1 frames, and with only two back
        // buffers the flip queue has no room to absorb the provider's own overhead -
        // measured 2026-10-31 01:34 at 4X: 4 vblanks (24.24 ms) + ~1 ms = 25.26 ms
        // against a 25.00 ms source period, i.e. every frame a quarter of a
        // millisecond short, which is what makes 4X at 40 fps marginal on 165 Hz.
        // A third buffer gives that millisecond somewhere to go.
        // Capped at 3 because this layer's RTV heap and back-buffer array are sized 3;
        // 4 would need both widened, which is a separate change.
        int bufs = mgpu::gpu1::ui_ini_read("DispBuffers", 3);
        if (bufs < 2) bufs = 2;
        if (bufs > 3) bufs = 3;
        scd.BufferCount = (UINT)bufs;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // R489 (2b-1'): in the handover arm NOBODY creates a chain here - XeFG
        // creates the window's only one from `scd`. Four 2b-1 runs proved why:
        // with a chain already on this HWND, XeFG's own CreateSwapChainForHwnd
        // returns E_ACCESSDENIED (0x80070005), and so does OUR second attempt,
        // so the rule is DXGI's and not XeFG's.
        HRESULT hr = S_OK;

        // ---- R489 (2b-1): hand THIS chain to XeFG, present to the proxy ----
        //
        // Placed here on purpose: after the chain exists, and BEFORE the RTV
        // heap and the back-buffer references are created, so that every one of
        // them is taken from the chain that will actually present. After a
        // successful init XeFG has re-created the chain (2a recorded
        // appChain=NULL), so p.sc/p.sc3 are replaced by the proxy and the
        // objects created above are released. Frame generation stays OFF in
        // this slice: the question is whether the layer survives the handover.
        if (ho_stage)
        {
            IDXGISwapChain1 *h_sc = nullptr;
            IDXGISwapChain3 *h_sc3 = nullptr;
            if (mgpu_xefg_display_handover(p.dev, p.q, p.hwnd, &scd, fac, &h_sc, &h_sc3, "disp_init"))
            {
                if (h_sc != nullptr && h_sc3 != nullptr)
                {
                    if (p.sc != nullptr) p.sc->Release();
                    if (p.sc3 != nullptr) p.sc3->Release();
                    p.sc = h_sc;
                    p.sc3 = h_sc3;
                    DXGI_SWAP_CHAIN_DESC1 hd1{};
                    if (SUCCEEDED(p.sc->GetDesc1(&hd1)) && hd1.BufferCount >= 1 && hd1.BufferCount <= 3)
                        p.bb_n = hd1.BufferCount;
                    { IDXGISwapChain2 *sc2 = nullptr; if (SUCCEEDED(p.sc->QueryInterface(IID_PPV_ARGS(&sc2)))) { sc2->SetMaximumFrameLatency((UINT)mgpu::gpu1::ui_ini_read("DispLatency", 2)); sc2->Release(); } }
                    char hr_l[300];
                    snprintf(hr_l, sizeof hr_l,
                             "[MGPU][R489] display chain IS the XeFG proxy now: %ux%u fmt=%u buffers=%u swapEffect=%u (generation OFF, 2b-1)",
                             hd1.Width, hd1.Height, (unsigned)hd1.Format, p.bb_n, (unsigned)hd1.SwapEffect);
                    mgpu::diag::info(hr_l);
                }
                else
                {
                    mgpu::diag::error("[MGPU][R489] handover said success but handed back no proxy - the display layer "
                                      "falls through to its own chain (R508) and this session runs without XeSS-FG");
                }
            }
        }

        // R508: WHOEVER CREATED IT, WE MUST HOLD A CHAIN WHEN WE LEAVE THIS BLOCK.
        //
        // The handover arm used to skip creating a chain here on purpose, because
        // the window's second flip chain is refused (2b-1: 0x80070005, four runs).
        // That was correct while the handover only happened for an operator who had
        // asked for XeSS. Now that it is attempted EVERY session, "no chain of our
        // own" would turn an absent libxell.dll into a black screen instead of a
        // session that runs exactly as it did before R508.
        //
        // A FAILED handover leaves no chain on the HWND - 2b-1 measured the failed
        // InitFromSwapChain returning in 52 ms with the display layer intact and
        // presenting normally - so ours is still allowed here. A SUCCESSFUL one has
        // already put the proxy in p.sc, so this block is skipped.
        if (p.sc == nullptr)
        {
            const bool came_from_handover = ho_stage;
            hr = fac->CreateSwapChainForHwnd(p.q, p.hwnd, &scd, nullptr, nullptr, &p.sc);
            if (SUCCEEDED(hr)) fac->MakeWindowAssociation(p.hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (FAILED(hr) || p.sc == nullptr)
            {
                char l[260];
                snprintf(l, sizeof l, "[MGPU][R489] no chain to present to: the handover %s and creating our own "
                         "returned hr=0x%08X - the display layer cannot come up this session",
                         came_from_handover ? "did not produce one" : "was not attempted", (unsigned)hr);
                mgpu::diag::error(l);
                if (fac != nullptr) { fac->Release(); fac = nullptr; }
                return false;
            }
            p.sc->QueryInterface(IID_PPV_ARGS(&p.sc3));
            p.bb_n = scd.BufferCount;
            { IDXGISwapChain2 *sc2 = nullptr; if (SUCCEEDED(p.sc->QueryInterface(IID_PPV_ARGS(&sc2)))) { sc2->SetMaximumFrameLatency((UINT)mgpu::gpu1::ui_ini_read("DispLatency", 2)); sc2->Release(); } }
            if (came_from_handover)
            {
                char l[240];
                snprintf(l, sizeof l, "[MGPU][R489] handover did not run - the display layer created its own chain "
                         "(%ux%u fmt=%u buffers=%u) and this session runs as before R508",
                         w, h, (unsigned)fmt, scd.BufferCount);
                mgpu::diag::warn(l);
            }
        }

        // R489: in the handover arm the window is shown HERE - after the chain
        // exists. A window that already covers the whole screen sits in DWM's
        // fullscreen path, and nothing good comes of asking for a flip chain
        // from inside that state.
        if (fac != nullptr) { fac->Release(); fac = nullptr; }
        if (ho_stage)
        {
            ShowWindow(p.hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(p.hwnd, HWND_TOPMOST, 0, 0, (int)w, (int)h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        if (p.sc == nullptr || p.sc3 == nullptr)
        {
            // R508: only the half-built case reaches here now - a chain exists but
            // IDXGISwapChain3 does not, which would mean a DXGI older than the one
            // this layer is built against. A display layer with nothing to present
            // to would just freeze the picture, so fail the init instead.
            mgpu::diag::error("[MGPU][R489] a chain exists but not as IDXGISwapChain3 - the display layer cannot "
                              "come up this session");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        if (FAILED(p.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtv)))) return false;
        p.rtv_inc = p.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < p.bb_n; ++i)
        {
            if (FAILED(p.sc->GetBuffer(i, IID_PPV_ARGS(&p.bb[i])))) return false;
            D3D12_CPU_DESCRIPTOR_HANDLE dh = p.rtv->GetCPUDescriptorHandleForHeapStart();
            dh.ptr += (UINT64)i * p.rtv_inc;
            p.dev->CreateRenderTargetView(p.bb[i], nullptr, dh);
        }
        if (FAILED(p.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.al)))) return false;
        if (FAILED(p.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.al, nullptr, IID_PPV_ARGS(&p.cl)))) return false;
        if (FAILED(p.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fe)))) return false;
        p.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (p.ev == nullptr) return false;

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        for (int i = 0; i < 6; ++i)
            if (FAILED(p.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.ring[i])))) return false;
        for (int i = 0; i < 6; ++i)
        {
            if (FAILED(p.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.cal[i])))) return false;
            if (FAILED(p.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.cal[i], nullptr, IID_PPV_ARGS(&p.ccl[i])))) return false;
            p.ccl[i]->Close();
        }
        if (FAILED(p.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.xfe)))) return false;
        p.cev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (p.cev == nullptr) return false;

        p.w = w; p.h = h; p.fmt = fmt;
        {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            wchar_t *slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
            wchar_t flag[MAX_PATH]{};
            swprintf_s(flag, L"%s\\mgpu_disp_novsync.on", exe);
            p.vsync = (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) ? 0 : 1;
        }
        // R491: the panel's current mode, read from Windows rather than inferred
        // from DXGI's retire counters. 0 or 1 from DM_DISPLAYFREQUENCY means "the
        // hardware default", which is worth logging as unknown rather than as 1 Hz.
        {
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                p.refresh_hz = dm.dmDisplayFrequency;
            char ml[220];
            snprintf(ml, sizeof ml, "[MGPU][R491] panel mode %ux%u @ %u Hz (EnumDisplaySettings), present sync=%d",
                     (unsigned)dm.dmPelsWidth, (unsigned)dm.dmPelsHeight, p.refresh_hz, p.vsync);
            mgpu::diag::info(ml);
        }
        { DWORD gp = 0; if (fg_before) GetWindowThreadProcessId(fg_before, &gp); if (gp == GetCurrentProcessId()) p.game = fg_before; }
        p.t0 = disp_now_ms();
        p.last_progress = p.t0;
        p.ready = true;
        p.th = std::thread(disp_thread, &p);

        char l[260];
        snprintf(l, sizeof l, "[MGPU][R448] display layer up (cut 4): %ux%u fmt=%u own queue+swapchain, 3 buffers, 4-slot ring, GPU-side handoff, present(%d,0)",
                 w, h, (unsigned)fmt, p.vsync);
        mgpu::diag::info(l);
        return true;
    }

    bool mq_tag_copy(ID3D12GraphicsCommandList *cl, ID3D12Resource *dst, UINT w, UINT h, int color, int code);
    ID3D12Resource *mq_marker(unsigned phase);

    // R490: a slot-sized twin of `like`, born in the state the tag will name.
    ID3D12Resource *disp_fg_like(ID3D12Device *dev, ID3D12Resource *like)
    {
        if (dev == nullptr || like == nullptr) return nullptr;
        D3D12_RESOURCE_DESC d = like->GetDesc();
        d.Flags = D3D12_RESOURCE_FLAG_NONE;      // a depth-format texture needs no DSV flag to be copied or read
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource *r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    // R490a: same, from a shape instead of a template resource.
    ID3D12Resource *disp_fg_make(ID3D12Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
    {
        if (dev == nullptr || w == 0u || h == 0u) return nullptr;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource *r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    // R490a: the one shader this layer owns - a point-sampled resample of depth.
    //
    // WHY IT HAS TO EXIST. XeFG refuses a pair whose resolutions differ:
    //   "XeFG: Invalid argument. motion vector and depth resource resolutions
    //    must match."  (measured 2026-10-30 22:59, 899 times, res=-4 per present)
    // and the bridge's guide depth is 3840x2160 while the game's real vectors are
    // 1920x1080 - the mismatch R474 had already written down. Only one of the two
    // may move, and it must be the depth: the vectors are the game's native
    // render-resolution field, and XeFG's own contract (like DLSS's) is that both
    // inputs live at the RENDER size. Upscaling the vectors instead would leave
    // "is motionVectorScaleX in render pixels or per texel of a 4K buffer?"
    // ambiguous, and that ambiguity is exactly what shows up later as motion
    // smear. Point sampling (Load, never Sample) on purpose: a bilinear tap
    // across a depth discontinuity invents depth between two surfaces.
    bool disp_fg_scale_init(disp_pipe &p)
    {
        if (p.fgsc.ok) return true;
        const char *src = R"(
Texture2D<float>   Src : register(t0);
RWTexture2D<float> Dst : register(u0);
cbuffer P : register(b0) { uint SrcW; uint SrcH; uint DstW; uint DstH; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) return;
    uint sx = (id.x * SrcW) / DstW;
    uint sy = (id.y * SrcH) / DstH;
    Dst[id.xy] = Src.Load(int3((int)sx, (int)sy, 0));
}
)";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        if (dc == nullptr)
        {
            mgpu::diag::error("[MGPU][R490a] d3dcompiler_47.dll not loadable - cannot resample depth to the vector "
                              "resolution; the generator stays off this session");
            return false;
        }
        typedef HRESULT(WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
                                             UINT, UINT, ID3DBlob **, ID3DBlob **);
        pfn_compile compile = (pfn_compile)GetProcAddress(dc, "D3DCompile");
        if (compile == nullptr)
        {
            mgpu::diag::error("[MGPU][R490a] D3DCompile not exported by d3dcompiler_47.dll");
            return false;
        }
        ID3DBlob *code = nullptr, *err = nullptr;
        // cs_5_0 on purpose: the same choice nr16 / residual / mvec_extract make,
        // because a game can ship a d3dcompiler_47 that rejects cs_5_1.
        // Flags 0: this file does not pull in <d3dcompiler.h> for the D3DCOMPILE_*
        // constants, and a twelve-line resample needs no special mode.
        const HRESULT cr = compile(src, strlen(src), "mgpu_fg_depth_resample", nullptr, nullptr, "main", "cs_5_0",
                                   0, 0, &code, &err);
        if (FAILED(cr) || code == nullptr)
        {
            char l[300];
            snprintf(l, sizeof l, "[MGPU][R490a] depth resample shader failed to compile 0x%08X %s",
                     (unsigned)cr, (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(l);
            if (err != nullptr) err->Release();
            if (code != nullptr) code->Release();
            return false;
        }
        if (err != nullptr) { err->Release(); err = nullptr; }

        D3D12_DESCRIPTOR_RANGE r0{}, r1{};
        r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
        r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r1.NumDescriptors = 1; r1.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER par[3]{};
        par[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        par[0].DescriptorTable.NumDescriptorRanges = 1;
        par[0].DescriptorTable.pDescriptorRanges = &r0;
        par[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        par[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        par[1].DescriptorTable.NumDescriptorRanges = 1;
        par[1].DescriptorTable.pDescriptorRanges = &r1;
        par[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        par[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        par[2].Constants.ShaderRegister = 0;
        par[2].Constants.Num32BitValues = 4;
        par[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = par;
        rsd.NumStaticSamplers = 0;              // Load() only: no sampler, nothing to clamp
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob *rb = nullptr, *rbe = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &rbe);
        if (FAILED(hr) || rb == nullptr)
        {
            char l[260];
            snprintf(l, sizeof l, "[MGPU][R490a] SerializeRootSignature failed 0x%08X %s", (unsigned)hr,
                     (rbe != nullptr) ? (const char *)rbe->GetBufferPointer() : "");
            mgpu::diag::error(l);
            if (rbe != nullptr) rbe->Release();
            code->Release();
            return false;
        }
        if (rbe != nullptr) rbe->Release();
        hr = p.dev->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(),
                                        IID_PPV_ARGS(&p.fgsc.rs));
        rb->Release();
        if (FAILED(hr)) { code->Release(); mgpu::diag::error("[MGPU][R490a] CreateRootSignature failed"); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = p.fgsc.rs;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        pd.NodeMask = 1;
        hr = p.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.fgsc.pso));
        code->Release();
        if (FAILED(hr)) { mgpu::diag::error("[MGPU][R490a] CreateComputePipelineState failed"); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 12;                 // per slot: one SRV, one UAV
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(p.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.fgsc.heap))))
        {
            mgpu::diag::error("[MGPU][R490a] descriptor heap for the depth resample failed");
            return false;
        }
        p.fgsc.inc = p.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        p.fgsc.ok = true;
        mgpu::diag::info("[MGPU][R490a] depth resample ready: point-sampled Scale() of the guide depth into the "
                         "vector resolution, cs_5_0, 12 descriptors");
        return true;
    }

    // R507: one id per source frame, shared by every frame that source frame
    // produces. The producer calls this ONCE before pushing a burst; each
    // disp_push in that burst carries the id through. A push that passes 0 gets
    // an id of its own, which is exactly right for the 1x paths - one frame per
    // source frame is a burst of one, and a burst of one can never be torn.
    unsigned disp_burst_id()
    {
        disp_pipe &p = dp();
        std::lock_guard<std::mutex> lk(p.m);
        return ++p.grp_seq;
    }

    // (a) never waits on the CPU. Ring full means the display side is behind; dropping
    // is the whole point - the game's present thread must keep running no matter what.
    // patch_phase >= 0 stamps the 512x512 phase pattern into the same command list.
    //
    // R490: `fg` (optional) names the depth and vectors THIS frame was made with. When
    // it is given and generation is armed, they are copied into the slot beside the
    // colour, so the display thread's tag describes the frame it is actually about to
    // present rather than whatever the renderer has moved on to.
    bool disp_push(ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, double t_ms,
                   int tag_color = -1, int tag_code = 0, int patch_phase = -1,
                   double max_late_ms = 25.0, const disp_fg_in *fg = nullptr,
                   const disp_fg_params *fpp = nullptr, unsigned grp = 0)
    {
        disp_pipe &p = dp();
        if (!p.ready || src == nullptr) return false;
        if (p.dead.load(std::memory_order_relaxed)) return false;
        if (p.stop) return false;

        // R498: the "on screen" tick used to be counted HERE, per push - which is
        // "frames handed to the layer", not "frames that reached the panel". With
        // XeSS one push becomes four frames, so the overlay read 40/40 at 4X
        // (measured 2026-10-31 00:5x). It is counted where the frames actually go
        // out now, in the display thread, from the provider's own count.

        // A QUIET PRODUCER IS NOT A STALLED CONSUMER.
        //
        // The stall test used to fire on "no pushes for 1.5 s", which is exactly what a
        // backend switch, a load screen, a pause or a menu looks like. So the watchdog
        // was reporting the producer's silence as the display thread's failure - and
        // that is why the failures clustered around switching backends. The test only
        // means anything when there IS work queued that nobody has presented.
        auto frames_are_queued = [&p]() -> bool
        {
            std::lock_guard<std::mutex> lk(p.m);
            return p.free_slots < 6u;
        };

        const double now = disp_now_ms();
        const unsigned long long seen = p.frames_seen.load(std::memory_order_relaxed);
        if (seen != p.last_seen)
        {
            p.last_seen = seen;
            p.last_progress = now;
        }
        else if (seen != 0 && (now - p.last_progress) > 1500.0 && frames_are_queued())
        {
            // A STALL IS NOT A DEATH SENTENCE.
            //
            // This used to disable the layer on the first 1.5 s of silence and fall
            // back to the old DComp chain for the rest of the session. That fallback
            // chain is exactly where the misaligned, smeared picture comes from, so a
            // recoverable hiccup was being converted into a permanently worse session.
            // Measured 2026-10-30 19:09: a 3954 ms stall with dropped=5 (the ring was
            // NOT full) - nothing about it was irrecoverable.
            //
            // So: re-arm, up to three times, keeping the window visible and resetting
            // the progress baseline. Only the fourth stall in one session is treated as
            // a real failure, and it says so out loud.
            // ALWAYS re-arm, never disable. The cap of three was itself a trap: the
            // operator switched backend a few times, the producer went quiet each time,
            // the counter ran out and the fourth pause permanently killed the layer -
            // exactly the "switching to FSR smears, DLSS acts like off" session that
            // followed. A layer that re-arms is wrong for a frame; a layer that dies is
            // wrong for the rest of the session.
            static int rearm = 0;
            const double stalled_ms = now - p.last_progress;
            ++rearm;
            p.last_progress = now;
            p.frames_seen.store(seen, std::memory_order_relaxed);
            if (p.hwnd != nullptr && !p.shown.load(std::memory_order_relaxed))
            {
                ShowWindow(p.hwnd, SW_SHOWNOACTIVATE);
                p.shown.store(true, std::memory_order_relaxed);
            }
            if (rearm <= 5 || (rearm % 50) == 0)
            {
                char l[280];
                snprintf(l, sizeof l,
                         "[MGPU][R455] frames queued but not presented for %.0f ms - RE-ARMING "
                         "(count=%d). The old DComp chain this used to fall back to is where the "
                         "smearing came from.", stalled_ms, rearm);
                mgpu::diag::warn(l);
            }
        }

        unsigned slot = 0;
        {
            std::lock_guard<std::mutex> lk(p.m);
            if (p.free_slots == 0)
            {
                ++p.dropped;
                return false;
            }
            --p.free_slots;
            slot = p.next_slot++ % 6u;
        }

        // The slot is ours, but its previous list may still be in flight (only when
        // the display thread skipped it while hidden). Bounded check, not a wait: with
        // 4 slots it normally sees the fence long past.
        if (p.slot_fv[slot] != 0 && p.xfe->GetCompletedValue() < p.slot_fv[slot])
        {
            ++p.waited;
            p.xfe->SetEventOnCompletion(p.slot_fv[slot], p.cev);
            WaitForSingleObject(p.cev, 50);
        }

        // ---- R490: do the generator's inputs travel with THIS frame? ----
        // A missing resource, a changed extent or a failed allocation all mean
        // "present this frame untagged". That costs one interpolated frame; guessing
        // costs the picture.
        bool fg_ok = (fg != nullptr && !p.stop);
        if (fg_ok && (fg->depth == nullptr || fg->mvec == nullptr || !mgpu_xefg_gen_active())) fg_ok = false;
        if (fg_ok)
        {
            ID3D12Device *dv = st().device;
            if (p.fg_d[0] == nullptr)
            {
                const D3D12_RESOURCE_DESC dd = fg->depth->GetDesc();
                const D3D12_RESOURCE_DESC md = fg->mvec->GetDesc();
                p.fg_mw = (UINT)md.Width; p.fg_mh = md.Height;
                const bool same = ((UINT)dd.Width == p.fg_mw && dd.Height == p.fg_mh);
                p.fg_dw = same ? (UINT)dd.Width : p.fg_mw;
                p.fg_dh = same ? dd.Height : p.fg_mh;
                p.fg_d_scaled = !same;
                if (p.fg_d_scaled && !disp_fg_scale_init(p))
                {
                    fg_ok = false;
                    if (p.fg_warned++ == 0)
                        mgpu::diag::error("[MGPU][R490a] depth and vectors differ in size and the resample is not "
                                          "available - every frame stays untagged");
                }
                if (fg_ok)
                {
                    for (unsigned i = 0; i < 6; ++i)
                    {
                        p.fg_d[i] = disp_fg_make(dv, p.fg_dw, p.fg_dh,
                                                 p.fg_d_scaled ? DXGI_FORMAT_R32_FLOAT : dd.Format,
                                                 p.fg_d_scaled ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                               : D3D12_RESOURCE_FLAG_NONE);
                        p.fg_m[i] = disp_fg_like(dv, fg->mvec);
                        if (p.fg_d[i] == nullptr || p.fg_m[i] == nullptr) { fg_ok = false; break; }
                    }
                }
                if (fg_ok)
                {
                    char l[320];
                    snprintf(l, sizeof l,
                             "[MGPU][R490] generator inputs armed: source depth %ux%u fmt=%u | source mvec %ux%u fmt=%u "
                             "| slot depth %ux%u fmt=%u (%s) | slot mvec %ux%u fmt=%u | 6 slots in ALL_SHADER_RESOURCE",
                             (unsigned)dd.Width, dd.Height, (unsigned)dd.Format,
                             p.fg_mw, p.fg_mh, (unsigned)md.Format,
                             p.fg_dw, p.fg_dh, (unsigned)(p.fg_d_scaled ? DXGI_FORMAT_R32_FLOAT : dd.Format),
                             p.fg_d_scaled ? "RESAMPLED to the vector resolution" : "copied 1:1",
                             p.fg_mw, p.fg_mh, (unsigned)md.Format);
                    mgpu::diag::info(l);
                }
                else if (p.fg_warned++ == 0)
                {
                    mgpu::diag::error("[MGPU][R490] could not allocate the generator's ring copies - every frame will be "
                                      "presented untagged");
                }
            }
            else
            {
                const D3D12_RESOURCE_DESC md = fg->mvec->GetDesc();
                if ((UINT)md.Width != p.fg_mw || md.Height != p.fg_mh)
                {
                    fg_ok = false;
                    if (p.fg_warned++ == 0)
                    {
                        char l[280];
                        snprintf(l, sizeof l, "[MGPU][R490] generator inputs changed extent (%ux%u vs %ux%u) - left "
                                 "untagged rather than reallocating six slots with frames in flight",
                                 (UINT)md.Width, md.Height, p.fg_mw, p.fg_mh);
                        mgpu::diag::warn(l);
                    }
                }
            }
        }
        disp_fg_params fp{};
        if (fpp != nullptr) fp = *fpp;

        ID3D12CommandAllocator *cal = p.cal[slot];
        ID3D12GraphicsCommandList *cl = p.ccl[slot];

        if (FAILED(cal->Reset())) return false;
        if (FAILED(cl->Reset(cal, nullptr))) return false;

        disp_barrier(cl, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        disp_barrier(cl, p.ring[slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(p.ring[slot], src);
        if (patch_phase >= 0)
        {
            ID3D12Resource *mk = mq_marker((unsigned)patch_phase);
            if (mk != nullptr)
            {
                disp_barrier(cl, mk, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION md{}, ms{};
                md.pResource = p.ring[slot]; md.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; md.SubresourceIndex = 0;
                ms.pResource = mk; ms.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ms.SubresourceIndex = 0;
                D3D12_BOX box{}; box.right = 512; box.bottom = 512; box.back = 1;
                cl->CopyTextureRegion(&md, 0, 0, 0, &ms, &box);
                disp_barrier(cl, mk, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
        }
        if (tag_color >= 0) mq_tag_copy(cl, p.ring[slot], p.w, p.h, tag_color, tag_code);
        // R490: the generator's two inputs, copied in this same list so that the
        // slot, the fence and the frame are one thing. The textures come back to
        // exactly the state they arrived in - this list must not own them.
        if (fg_ok)
        {
            if (p.fg_d_scaled && p.fgsc.ok)
            {
                // R490a: point-sampled resample of the guide depth into this slot.
                // The SRV is written only when a slot first sees a given depth
                // texture: a slot is never re-used while its list is in flight (the
                // fence above saw to that), so writing it here cannot race the GPU.
                D3D12_CPU_DESCRIPTOR_HANDLE base = p.fgsc.heap->GetCPUDescriptorHandleForHeapStart();
                D3D12_GPU_DESCRIPTOR_HANDLE gbase = p.fgsc.heap->GetGPUDescriptorHandleForHeapStart();
                const UINT inc = p.fgsc.inc;
                if (p.fgsc.src_seen[slot] != (void *)fg->depth)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                    sd.Format = DXGI_FORMAT_R32_FLOAT;      // legal view of a D32 resource
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sd.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE s = base;
                    s.ptr += (UINT64)(2u * slot) * inc;
                    p.dev->CreateShaderResourceView(fg->depth, &sd, s);

                    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = DXGI_FORMAT_R32_FLOAT;
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    D3D12_CPU_DESCRIPTOR_HANDLE u = base;
                    u.ptr += (UINT64)(2u * slot + 1u) * inc;
                    p.dev->CreateUnorderedAccessView(p.fg_d[slot], nullptr, &ud, u);
                    p.fgsc.src_seen[slot] = (void *)fg->depth;
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gs = gbase, gu = gbase;
                gs.ptr += (UINT64)(2u * slot) * inc;
                gu.ptr += (UINT64)(2u * slot + 1u) * inc;

                const D3D12_RESOURCE_DESC dd = fg->depth->GetDesc();
                disp_barrier(cl, fg->depth, fg->depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                disp_barrier(cl, p.fg_d[slot], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12DescriptorHeap *heaps[1] = { p.fgsc.heap };
                cl->SetDescriptorHeaps(1, heaps);
                cl->SetComputeRootSignature(p.fgsc.rs);
                cl->SetPipelineState(p.fgsc.pso);
                cl->SetComputeRootDescriptorTable(0, gs);
                cl->SetComputeRootDescriptorTable(1, gu);
                const UINT rc[4] = { (UINT)dd.Width, dd.Height, p.fg_dw, p.fg_dh };
                cl->SetComputeRoot32BitConstants(2, 4, rc, 0);
                cl->Dispatch((p.fg_dw + 7u) / 8u, (p.fg_dh + 7u) / 8u, 1u);
                disp_barrier(cl, p.fg_d[slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                disp_barrier(cl, fg->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, fg->depth_state);
            }
            else
            {
                disp_barrier(cl, fg->depth, fg->depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                disp_barrier(cl, p.fg_d[slot], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(p.fg_d[slot], fg->depth);
                disp_barrier(cl, p.fg_d[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                disp_barrier(cl, fg->depth, D3D12_RESOURCE_STATE_COPY_SOURCE, fg->depth_state);
            }

            disp_barrier(cl, fg->mvec, fg->mvec_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            disp_barrier(cl, p.fg_m[slot], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(p.fg_m[slot], fg->mvec);
            disp_barrier(cl, p.fg_m[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            disp_barrier(cl, fg->mvec, D3D12_RESOURCE_STATE_COPY_SOURCE, fg->mvec_state);
        }
        disp_barrier(cl, p.ring[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        disp_barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
        if (FAILED(cl->Close())) return false;

        ID3D12CommandQueue *bq = st().queue;
        if (bq == nullptr)
        {
            std::lock_guard<std::mutex> lk(p.m);
            ++p.free_slots;
            return false;
        }
        bq->ExecuteCommandLists(1, (ID3D12CommandList **)&cl);
        const UINT64 v = ++p.xfv;
        bq->Signal(p.xfe, v);
        p.slot_fv[slot] = v;

        {
            std::lock_guard<std::mutex> lk(p.m);
            disp_frame fr{};
            fr.slot = slot;
            fr.t_ms = t_ms;
            fr.max_late_ms = max_late_ms;
            // R507: a push with no burst id is its own burst. Assigned under the
            // lock so the counter is only ever touched by one thread at a time.
            fr.grp = (grp != 0u) ? grp : ++p.grp_seq;
            fr.xfv = v;
            fr.has_fg = fg_ok;
            fr.fg = fp;
            p.items.push_back(fr);
        }
        p.cv.notify_all();
        return true;
    }

    bool disp_alive()
    {
        return dp().ready && !dp().dead.load(std::memory_order_relaxed);
    }

    void disp_set_panel(bool hide)
    {
        disp_pipe &p = dp();
        p.panel_hide.store(hide, std::memory_order_relaxed);
        if (!p.ready) return;
        // R494: closing the panel is also the moment the output may have become free
        // again (the panel covered it, or something else did). Do not assume; let the
        // next pass ask DXGI. Clearing here only means "try", and the first Present
        // that still says occluded sets it straight back.
        if (!hide && p.occluded)
        {
            p.occluded = false;
            p.last_occl_test = 0.0;
            mgpu::diag::warn("[MGPU][R454] panel closed - re-testing the output instead of staying suspended");
        }
        if (hide)
        {
            std::deque<disp_frame> throwaway;
            {
                std::lock_guard<std::mutex> lk(p.m);
                throwaway.swap(p.items);
                p.free_slots += (unsigned)throwaway.size();
            }
            if (p.hwnd != nullptr) ShowWindow(p.hwnd, SW_HIDE);
            p.shown.store(false, std::memory_order_relaxed);
        }
    }

    void disp_stop()
    {
        disp_pipe &p = dp();
        if (!p.ready) return;
        {
            std::lock_guard<std::mutex> lk(p.m);
            p.stop = true;
        }
        p.cv.notify_all();
        if (p.th.joinable()) p.th.join();
        p.ready = false;
    }
}
