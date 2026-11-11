// =====================================================================
// R489 - 2b-1: THE DISPLAY LAYER'S OWN CHAIN, HANDED TO XeFG
// =====================================================================
//
// The smoke (R488, 2a) proved the whole XeFG sequence on a throwaway chain:
// 1798 presents with framesPresented=2, 12.5 ms per present on a 160 Hz
// output. This file is the first slice that touches the REAL chain - the
// display layer's 4K flip chain that puts the picture on the monitor.
//
// 2b-1 IS DELIBERATELY BORING: frame generation stays OFF (SetEnabled(0)).
// The only question is "does the display layer survive the handover?" - and
// the only honest answer is behavioural:
//
//     the picture looks exactly like the DLSS-G path does today, and the
//     [R448] present rate is still one frame per present.
//
// If that holds, 2b-2 can switch generation on with real depth/velocity. If
// it does not, we have spent one flag file instead of a session.
//
// WHY THIS IS THE ONE IRREVERSIBLE STEP. After a successful
// InitFromSwapChain, XeFG has RE-CREATED the chain: the object the display
// layer created is gone (2a recorded appChain=NULL from
// GetInitializationParameters), and the only way to present is the proxy. So
// the failure we must never paper over is "init succeeded, GetSwapChainPtr
// failed" - then there is no present path at all and the layer must stop
// rather than pretend. That is why both are checked here before anything is
// replaced, and why the caller is told to keep its own chain on failure.
//
// WHAT DOES NOT CHANGE: the window, the display queue, the 6-slot ring, the
// GPU-side cross-queue fence, the non-blocking push, the watchdog. XeFG only
// replaces the chain object and its back buffers. (2a measured exactly that:
// proxy desc 480x270 fmt=24 buffers=2 swapEffect=4, same shape as handed in.)
//
// THE CONTEXT IS NEVER DESTROYED. It has to outlive every present on this
// chain, and the display layer lives for the process. The handles are kept in
// function-static cells on purpose: freeing 22 MB of vendor runtime during
// shutdown, while the compositor may still hold the chain, is the worse risk.
//
// GATED BY A FILE: <exe dir>\mgpu_xefg_handover.on. Absent => one
// GetFileAttributesW, and the display layer takes the exact path it takes
// today.
//
// DEPLOYMENT PREREQUISITES, all measured in 2a (PLAN 搂11.7):
//   * <exe dir>\libxell.dll must be >= 1.3.x  - XeFG resolves libxell by plain
//     name and gets the exe-directory copy; 1.1.0 cannot create a context on
//     this GPU at all, and a context from any other copy is rejected (-15).
//   * xellSetSleepMode MUST enable bLowLatencyMode (bit0), or SetEnabled is
//     refused with -15 "Latency reduction is not enabled".
//   * <add-on dir>\mgpu\libxess_fg.dll 1.3.1.78 is the module used here.
#include <dxgi1_5.h>

namespace
{
    // R490b: THE DOOR IS THE PANEL, NOT A FLAG FILE.
    //
    // 2a/2b were armed by <exe dir>\mgpu_xefg_handover.on and mgpu_xefg_gen.on
    // because a file can be created without a rebuild. Both facts are now known,
    // so the flags retire: the backend chosen in the panel (FgBackend=3 in the
    // ini) is the one state, and "XeSS-FG selected" means both "hand it the
    // chain" and "let it generate". One state, one door - the same rule R463 used
    // when it retired the Ctrl+Alt+G hotkey.
    bool hfg_flag_on()
    {
        return mgpu::fgmode::get() == mgpu::fgmode::XEFG;
    }

    // R490b: was <exe dir>\mgpu_xefg_gen.on. Same door as the handover now: if the
    // operator asked for XeSS-FG, they asked for interpolated frames. The flag
    // existed only to keep 2b-1 ("chain works, generation off") reachable while the
    // generation half was still unproven; that half is measured, so the two-flag
    // arrangement has no remaining job. Selecting another backend is the rollback.
    bool hfg_gen_flag_on()
    {
        return mgpu::fgmode::get() == mgpu::fgmode::XEFG;
    }

    bool hfg_exists(const wchar_t *p)
    {
        return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    }

    // R508: WHO OWNS THE CHAIN IS NOT THE BACKEND'S DECISION.
    //
    // Until now the handover was gated on `fgmode::get() == XEFG`, which reads as
    // "the operator chose XeSS, so hand it the chain" and is wrong in one specific
    // way that cost a whole session on 2026-10-31 03:0x: a session started on
    // DLSS-G can never switch to XeSS-FG, because XeFG only creates the window's
    // chain at disp_init. The panel offered the cell, the cell wrote the ini, and
    // the picture never changed - XeSS "did not work", and it was not XeSS.
    //
    // What the layer actually wants is the shape 2b-1' measured: the proxy owns
    // the chain PERMANENTLY, and with SetEnabled(0) it presents our frames
    // untouched (28.7 fps, one frame per Present, p50 25.07 ms). Generation is a
    // runtime switch on top of that (mgpu_xefg_apply_mode, R497), not a
    // precondition for the chain.
    //
    // So the gate becomes its own key, read once at disp_init:
    //   XeFGChain=1 (default, also when absent)  attempt the handover every session
    //   XeFGChain=0                              only when the session starts on XeSS-FG,
    //                                            which is the behaviour before R508
    // One hard precondition, because it is XeFG's and not ours: libxell.dll has to
    // be beside the executable (XeFG resolves it by bare name and gets that copy).
    // Without it the attempt is a guaranteed failure, so it is not attempted and
    // the session is byte-for-byte what it was before this change.
    bool hfg_chain_wanted()
    {
        static int cached = -1;
        if (cached >= 0) return cached != 0;

        int ok = (mgpu::gpu1::ui_ini_read("XeFGChain", 1) != 0) ? 1 : 0;
        char l[300];
        if (ok)
        {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) ok = 0;
            else
            {
                wchar_t *slash = wcsrchr(exe, L'\\');
                if (slash != nullptr) *(slash + 1) = 0;
                wchar_t p[MAX_PATH * 2]{};
                _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\libxell.dll", exe);
                if (!hfg_exists(p))
                {
                    ok = 0;
                    snprintf(l, sizeof l, "[MGPU][R508] XeFGChain is on but there is no libxell.dll beside the "
                             "executable - XeFG cannot create a context without it, so this session keeps the "
                             "display layer's own chain (XeSS-FG unavailable, everything else unchanged)");
                    mgpu::diag::info(l);
                }
            }
        }
        if (ok && cached < 0)
        {
            snprintf(l, sizeof l, "[MGPU][R508] XeFGChain=on: the display chain will be created by XeFG this session "
                     "whatever backend starts. Generation stays OFF until the panel asks for XeSS-FG; the multiplier "
                     "and the on/off are live either way. This is the only way XeSS-FG can be switched to mid-session.");
            mgpu::diag::info(l);
        }
        else if (!ok)
        {
            snprintf(l, sizeof l, "[MGPU][R508] XeFGChain=off (or refused): the handover happens only when the session "
                     "starts on XeSS-FG, as before R508");
            mgpu::diag::info(l);
        }
        cached = ok;
        return cached != 0;
    }

    // Same tail-trimming rule the rest of this file uses for a log line.
    void hfg_tail(const wchar_t *p, wchar_t *out, size_t out_n)
    {
        const size_t len = (p != nullptr) ? wcslen(p) : 0;
        if (len == 0) { wcscpy_s(out, out_n, L"?"); return; }
        if (len + 1 <= out_n) { wcscpy_s(out, out_n, p); return; }
        wcscpy_s(out, out_n, L"...");
        wcscat_s(out, out_n, p + (len - (out_n - 4)));
    }

    // XeFG's own log, forwarded. 2b-1's first run did NOT install this and paid
    // for it: InitFromSwapChain returned -17 (ERROR_HRESULT_FAILURE), whose
    // documentation is literally "see the logs" - and there was no log. The
    // result was a run that produced one number and no reason.
    void hfg_log_cb(const char *msg, int level, void * /*user*/)
    {
        if (msg == nullptr) return;
        char l[512];
        const char *lv = (level == 0) ? "debug" : (level == 1) ? "info" : (level == 2) ? "warn" : "error";
        snprintf(l, sizeof l, "[MGPU][R489] xefg says [%s] %s", lv, msg);
        mgpu::diag::info(l);
    }

    // The export table this slice needs. Optional ones are marked: the shipped
    // 1.3.1.78 does not export GetInitializationParameters, so its absence is
    // a note, not a failure.
    bool hfg_load_xefg(efg_api &api, HMODULE mod, char *missing, size_t missing_n)
    {
        struct { const char *name; void **slot; bool required; } want[] = {
            { "xefgSwapChainD3D12CreateContext",     (void **)&api.create_context,        true  },
            { "xefgSwapChainGetProperties",          (void **)&api.get_properties,        true  },
            { "xefgSwapChainSetLatencyReduction",    (void **)&api.set_latency_reduction, true  },
            { "xefgSwapChainD3D12InitFromSwapChain", (void **)&api.init_from_swapchain,   true  },
            { "xefgSwapChainD3D12InitFromSwapChainDesc", (void **)&api.init_from_desc,    true  },
            { "xefgSwapChainD3D12GetSwapChainPtr",   (void **)&api.get_swapchain_ptr,     true  },
            { "xefgSwapChainSetEnabled",             (void **)&api.set_enabled,           true  },
            { "xefgSwapChainSetNumInterpolatedFrames", (void **)&api.set_num_frames,      false },
            // R490: the four entry points 2b-2 cannot work without. 2a proved all
            // four present in 1.3.1.78, so their absence here is a deployment
            // problem worth refusing over rather than a code path to guess at.
            { "xefgSwapChainSetPresentId",           (void **)&api.set_present_id,        true  },
            { "xefgSwapChainTagFrameConstants",      (void **)&api.tag_frame_constants,   true  },
            { "xefgSwapChainD3D12TagFrameResource",  (void **)&api.tag_frame_resource,    true  },
            { "xefgSwapChainGetLastPresentStatus",   (void **)&api.get_last_present_status, true },
            { "xefgSwapChainSetLoggingCallback",     (void **)&api.set_logging_callback,  false },
            { "xefgSwapChainD3D12GetInitializationParameters", (void **)&api.get_init_params, false },
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

    bool hfg_load_xell(xell_api &xa, HMODULE mod, char *missing, size_t missing_n)
    {
        struct { const char *name; void **slot; bool required; } want[] = {
            { "xellD3D12CreateContext", (void **)&xa.create_context,  true  },
            { "xellDestroyContext",     (void **)&xa.destroy_context, true  },
            { "xellSetSleepMode",       (void **)&xa.set_sleep_mode,  false },
            { "xellGetVersion",         (void **)&xa.get_version,     false },
            // R493: XeLL is the vendor's own frame pacer, and we had only ever
            // used SetSleepMode. All three below are declared in the shipped
            // xell.h, so the ABI is read from the header rather than guessed -
            // which is the whole reason this is the first pacing attempt and the
            // provider-thunk rewrite is not.
            { "xellAddMarkerData",      (void **)&xa.add_marker,      false },
            { "xellSleep",              (void **)&xa.sleep,           false },
            { "xellGetFramesReports",   (void **)&xa.frames_reports,  false },
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

    // ------------------------------------------------------------------
    // R492 (slice 4): the provider's MFG gate is five bytes.
    //
    // Intel gates multi-frame generation behind "am I the igxess_fg.dll build?"
    // rather than behind a capability query, so there is nothing to ask and
    // nothing to configure: the only lever is the provider's own code. The five
    // sites and their expected bytes come from Coldwood1026/OptiScaler
    // `proxies/XeFGUnlock.h` @70676c5f, and every one of them was re-verified
    // against OUR copy on disk before this was written:
    //
    //   build identity   TimeDateStamp 0x69CB0F4D  SizeOfImage 0x015ED000
    //   sha256           EC5E0C65E075570C...        (mgpu\libxess_fg.dll)
    //   U1 0x20DA4F  0F 85 CC 00 00 00  jne -> jmp   frame-count resolver stops
    //                                                falling back to 2X on the
    //                                                "XeLL too old" verdict
    //   U2 0x1A5DE4  74 09              je  -> jmp   mfgAllowed() always true, so
    //                                                init stops downgrading model
    //   U3 0x1A517D  BB 03 00 00 00     mov ebx,3    default ceiling
    //   U4 0x1A45C2  C7 87 6C 01 00 00 01 00 00 00   the "MFG unavailable" path
    //                                                pins the override to ONE
    //                                                interpolated frame; pin it to
    //                                                the configured count
    //   U5 0x20973B  B8 01 00 00 00     mov eax,1    GetProperties reports
    //                                                maxSupportedInterpolations
    //
    // ARMS, so that each step is one ini edit and no rebuild:
    //   XeFGUnlock=0  off (default: the binary behaves exactly as before)
    //   XeFGUnlock=1  VERIFY ONLY - read the five sites, log every byte and
    //                 whether it matches, write nothing. This is the step that
    //                 must pass before anything is written.
    //   XeFGUnlock=2  U5 only - changes only what is REPORTED. Generation
    //                 cannot change, so this arm answers one question: does the
    //                 number the provider reports follow the bytes?
    //   XeFGUnlock=3  all five, with XeFGMaxInterp (1..3) baked into the
    //                 immediates. This is the arm that can actually generate
    //                 more than one interpolated frame.
    //
    // SAFETY, in this order: refused outside .text; every site compared against
    // the expected bytes before any write; every write verified by read-back;
    // any failure rolls the whole set back and reports it. A failed unlock is
    // not fatal - the provider simply behaves as it shipped.
    //
    // STILL MISSING ON PURPOSE: pacing. Above 2X the provider hands the whole
    // burst to the swapchain back to back and paces only the burst, so 3X/4X
    // lands bunched until the present path is paced per frame. That is the NEXT
    // step, and it is why XeFGMaxInterp exists separately from "does 4 frames
    // per present happen at all".
    struct hfg_patch
    {
        unsigned rva;
        const unsigned char *expect;
        const unsigned char *replace;
        unsigned size;
        int imm_off;           // -1 = no immediate to stamp
        const char *name;
    };

    // Byte strings are written as arrays rather than strings on purpose: these
    // are machine code, and a "string" invites someone to edit a ' character.
    static const unsigned char U1O[] = { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 };
    static const unsigned char U1N[] = { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 };
    static const unsigned char U2O[] = { 0x74, 0x09 };
    static const unsigned char U2N[] = { 0xEB, 0x06 };
    static const unsigned char U3O[] = { 0xBB, 0x03, 0x00, 0x00, 0x00 };
    static const unsigned char U3N[] = { 0xBB, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char U4O[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    static const unsigned char U4N[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char U5O[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
    static const unsigned char U5N[] = { 0xB8, 0x00, 0x00, 0x00, 0x00 };

    void hfg_hex(const unsigned char *p, unsigned n, char *out, size_t out_n)
    {
        static const char *d = "0123456789ABCDEF";
        size_t o = 0;
        for (unsigned i = 0; i < n && o + 4 < out_n; ++i)
        {
            if (i != 0) out[o++] = ' ';
            out[o++] = d[p[i] >> 4];
            out[o++] = d[p[i] & 0xF];
        }
        out[o] = 0;
    }

    // Returns how many patches are in effect. Never throws, never leaves a
    // half-applied set behind.
    int hfg_unlock(HMODULE mod, int mode)
    {
        char l[420];
        if (mode <= 0)
        {
            mgpu::diag::info("[MGPU][R492] XeFG unlock OFF (XeFGUnlock=0) - the provider runs exactly as shipped");
            return 0;
        }
        if (mod == nullptr) return 0;

        unsigned char *base = (unsigned char *)mod;
        if (base[0] != 'M' || base[1] != 'Z')
        {
            mgpu::diag::error("[MGPU][R492] XeFG unlock: module has no MZ header - skipping");
            return 0;
        }
        const unsigned ptr32 = *(const unsigned *)(base + 0x3C);
        const unsigned char *nt = base + ptr32;
        if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0)
        {
            mgpu::diag::error("[MGPU][R492] XeFG unlock: no PE signature - skipping");
            return 0;
        }
        const unsigned stamp = *(const unsigned *)(nt + 8);
        const unsigned char *fh = nt + 4;
        const unsigned nsec = *(const unsigned short *)(fh + 2);
        const unsigned optsz = *(const unsigned short *)(fh + 16);
        const unsigned char *opt = fh + 20;
        const unsigned soi = *(const unsigned *)(opt + 56);
        snprintf(l, sizeof l, "[MGPU][R492] XeFG provider: stamp=0x%08X sizeofimage=0x%08X (patch table expects "
                 "0x69CB0F4D/0x015ED000) - %s", stamp, soi,
                 (stamp == 0x69CB0F4Du && soi == 0x015ED000u) ? "KNOWN BUILD" : "UNKNOWN BUILD, per-byte checks only");
        mgpu::diag::info(l);

        // .text, because a patch outside it means the table belongs to another build.
        const unsigned char *sec = opt + optsz;
        unsigned text_va = 0, text_vs = 0;
        bool have_text = false;
        for (unsigned i = 0; i < nsec; ++i, sec += 40)
        {
            if (sec[0] == '.' && sec[1] == 't' && sec[2] == 'e' && sec[3] == 'x' && sec[4] == 't')
            {
                text_vs = *(const unsigned *)(sec + 8);
                text_va = *(const unsigned *)(sec + 12);
                have_text = true;
                break;
            }
        }
        if (!have_text)
        {
            mgpu::diag::error("[MGPU][R492] XeFG unlock: no .text section - skipping");
            return 0;
        }

        // R492b: the count baked into the provider's bytes is now the CEILING, not
        // the setting. It has to be: the immediates in U3/U4/U5 are written once at
        // handover, so a count of 1 baked there would make a runtime request for 4
        // an invalid argument. The live count is SetNumInterpolatedFrames' job
        // (R497), and it is the panel that decides it.
        int n = 5;
        if (n < 1) n = 1;
        if (n > 5) n = 5;
        const bool want_n = (mode >= 3);

        const hfg_patch tab[5] = {
            { 0x20DA4Fu, U1O, U1N, sizeof U1O, -1, "U1/frame-count-fallback" },
            { 0x1A5DE4u, U2O, U2N, sizeof U2O, -1, "U2/model-downgrade" },
            { 0x1A517Du, U3O, U3N, sizeof U3O,  1, "U3/default-ceiling" },
            { 0x1A45C2u, U4O, U4N, sizeof U4O,  6, "U4/override-clamp" },
            { 0x20973Bu, U5O, U5N, sizeof U5O,  1, "U5/reported-maximum" },
        };
        // mode 2 touches only U5 (what is REPORTED); arms 1 and 3 walk all five.
        //
        // ARM 1 CHECKS ALL FIVE AND WRITES NONE, and that is not a repeat of the
        // offline check run on the file: this is the MAPPED image, after the loader
        // applied base relocations. A table verified only on disk can still be
        // wrong here, and the first arm exists to catch exactly that before
        // anything is written. (Its first run checked U5 only, because `sel` was
        // derived from "which arm applies" instead of "which arm inspects" - one
        // variable conflated with another, caught by the log saying
        // "1 applied, 4 skipped".)
        const bool all_five = (mode != 2);
        const bool sel[5] = { all_five, all_five, all_five, all_five, true };

        int ok = 0, skipped = 0, failed = 0;

        // EVERY SITE STANDS ALONE. A site whose bytes do not match is a finding,
        // not a reason to abandon the other four: the run that teaches us "U1 did
        // not land but U5 did" is worth more than the run that refuses to start.
        // The one thing never done is leaving a half-written site behind.
        for (int i = 0; i < 5; ++i)
        {
            const hfg_patch &p = tab[i];
            if (!sel[i]) { ++skipped; continue; }
            if (p.rva < text_va || p.rva + p.size > text_va + text_vs)
            {
                snprintf(l, sizeof l, "[MGPU][R492] %s at 0x%X is outside .text - skipped", p.name, p.rva);
                mgpu::diag::warn(l);
                ++failed;
                continue;
            }
            unsigned char *dst = base + p.rva;
            char before[48];
            hfg_hex(dst, p.size, before, sizeof before);
            if (memcmp(dst, p.expect, p.size) != 0)
            {
                char want[48];
                hfg_hex(p.expect, p.size, want, sizeof want);
                snprintf(l, sizeof l, "[MGPU][R492] %s at 0x%X has UNEXPECTED bytes (%s, expected %s) - skipped, the "
                         "other sites still apply", p.name, p.rva, before, want);
                mgpu::diag::warn(l);
                ++failed;
                continue;
            }
            if (mode == 1)
            {
                snprintf(l, sizeof l, "[MGPU][R492] %s at 0x%X verified (%s) - VERIFY ONLY, nothing written",
                         p.name, p.rva, before);
                mgpu::diag::info(l);
                ++ok;
                continue;
            }

            unsigned char buf[16] = {};
            memcpy(buf, p.replace, p.size);
            if (p.imm_off >= 0)
            {
                buf[p.imm_off + 0] = (unsigned char)(n & 0xFF);
                buf[p.imm_off + 1] = (unsigned char)((n >> 8) & 0xFF);
                buf[p.imm_off + 2] = (unsigned char)((n >> 16) & 0xFF);
                buf[p.imm_off + 3] = (unsigned char)((n >> 24) & 0xFF);
            }
            DWORD oldp = 0;
            if (!VirtualProtect(dst, p.size, PAGE_EXECUTE_READWRITE, &oldp))
            {
                snprintf(l, sizeof l, "[MGPU][R492] %s: VirtualProtect failed (err=%lu) - this site skipped",
                         p.name, (unsigned long)GetLastError());
                mgpu::diag::warn(l);
                ++failed;
                continue;
            }
            memcpy(dst, buf, p.size);
            FlushInstructionCache(GetCurrentProcess(), dst, p.size);
            DWORD ign = 0;
            VirtualProtect(dst, p.size, oldp, &ign);
            if (memcmp(dst, buf, p.size) != 0)
            {
                // Only THIS site goes back; the earlier ones did verify and stay.
                if (VirtualProtect(dst, p.size, PAGE_EXECUTE_READWRITE, &oldp))
                {
                    memcpy(dst, p.expect, p.size);
                    FlushInstructionCache(GetCurrentProcess(), dst, p.size);
                    DWORD ign2 = 0;
                    VirtualProtect(dst, p.size, oldp, &ign2);
                }
                snprintf(l, sizeof l, "[MGPU][R492] %s: write verification FAILED - this site restored, others kept",
                         p.name);
                mgpu::diag::warn(l);
                ++failed;
                continue;
            }
            ++ok;
            char after[48];
            hfg_hex(dst, p.size, after, sizeof after);
            snprintf(l, sizeof l, "[MGPU][R492] %s at 0x%X patched: %s -> %s", p.name, p.rva, before, after);
            mgpu::diag::info(l);
        }

        snprintf(l, sizeof l, "[MGPU][R492] XeFG unlock arm=%d: %d ok, %d failed, %d skipped, interpolated=%d -> %s",
                 mode, ok, failed, skipped, n,
                 (mode == 1) ? "verified only"
                             : (want_n ? (failed == 0 ? "multi-frame requested" : "PARTIAL unlock, provider decides")
                                       : "reported maximum only, generation unchanged"));
        if (failed != 0) mgpu::diag::warn(l);
        else mgpu::diag::info(l);

        // What the init call should ask for. Only arm 3 asks for more than one
        // interpolated frame; arm 2 changes the REPORTED number alone so that its
        // single variable stays readable. If some sites were refused the provider
        // clamps whatever we ask, and the frame count in [R448] tells the truth -
        // so there is nothing to guard here.
        return want_n ? n : 0;
    }

    // ------------------------------------------------------------------
    // R499 (slice 5, step 1): LOOK BEFORE TOUCHING.
    //
    // Only 4X is wrong, and the published analysis says why: above count > 2 the
    // provider hands every generated frame of the burst straight to the swapchain
    // and computes a presentation time for the LAST one only, so a 4X burst is
    // delivered as 0, i, 2i, 2i instead of 0, i, 2i, 3i - the fourth frame reuses
    // the third's slot and the picture appears to jump backwards. 3X has count 2
    // and never reaches that block, which is exactly the pattern the operator
    // reported (4X only, DLSS-G at 4X fine).
    //
    // The fix is to schedule every frame of the burst through the provider's own
    // scheduler. Before writing that, this step only INSTALLS A PASSTHROUGH on the
    // present thunk and logs which call sites actually arrive, with which burst
    // pointer and flag. Nothing is dereferenced - a wrong pointer arithmetic inside
    // someone else's present is a crash, and a log line is not worth one.
    //
    // Provider internals, libxess_fg.dll 1.3.1.78 (stamp 0x69CB0F4D):
    //   thunk 0x25C0 -> 0x21F730   present one frame (16-byte slot: E9 rel32 + int3)
    //   call site 0x2202ED         the burst loop's present (1..count-1)
    //   call site 0x220467         the post-loop present of the burst's last frame
    typedef long long (*hfg_present_fn)(void *, unsigned, unsigned, unsigned long long, void *, void *, unsigned long long);

    unsigned char *g_hfg_base = nullptr;
    hfg_present_fn g_hfg_native_present = nullptr;
    int g_hfg_obs_left = 0;

    long long hfg_present_detour(void *ctx, unsigned a2, unsigned a3, unsigned long long a4, void *arg5, void *arg6,
                                 unsigned long long arg7)
    {
        if (g_hfg_obs_left > 0 && g_hfg_base != nullptr)
        {
            --g_hfg_obs_left;
            const unsigned long long rva = (unsigned long long)((unsigned char *)_ReturnAddress() - g_hfg_base);
            char l[240];
            snprintf(l, sizeof l, "[MGPU][R499] present via thunk: caller_rva=0x%llX arg5=%p arg6=%p arg7=%llu",
                     rva, arg5, arg6, arg7);
            mgpu::diag::info(l);
        }
        return g_hfg_native_present(ctx, a2, a3, a4, arg5, arg6, arg7);
    }

    // One 16-byte thunk slot -> an absolute jump anywhere. Every entry of the
    // provider's thunk table has this same shape (5-byte E9 rel32 + 11 int3), so a
    // 14-byte `jmp qword ptr [rip+0]` fits with no instruction to relocate and no
    // code cave. Verified before writing and verified again after.
    bool hfg_hook_thunk(unsigned char *base, unsigned rva, unsigned target_rva, const unsigned char *expect,
                        void *detour, void **native_out)
    {
        static const unsigned char exp[16] = { 0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC,
                                               0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
        if (expect == nullptr) expect = exp;
        unsigned char *thunk = base + rva;
        if (memcmp(thunk, expect, 16) != 0)
        {
            char l[200];
            snprintf(l, sizeof l, "[MGPU][R499] thunk at 0x%X has unexpected bytes - not hooking", rva);
            mgpu::diag::warn(l);
            return false;
        }
        unsigned char repl[16] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC,
                                   0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
        const unsigned long long target = (unsigned long long)detour;
        memcpy(repl + 6, &target, sizeof target);
        DWORD oldp = 0;
        if (!VirtualProtect(thunk, 16, PAGE_EXECUTE_READWRITE, &oldp)) return false;
        memcpy(thunk, repl, 16);
        FlushInstructionCache(GetCurrentProcess(), thunk, 16);
        DWORD ign = 0;
        VirtualProtect(thunk, 16, oldp, &ign);
        if (memcmp(thunk, repl, 16) != 0) return false;
        *native_out = base + target_rva;
        return true;
    }

    // Returns true when the observation hook is live. XeFGObserve = 0 leaves the
    // provider exactly as shipped.
    bool hfg_observe(HMODULE mod, int quota)
    {
        if (quota <= 0 || mod == nullptr) return false;
        if (g_hfg_native_present != nullptr) return true;
        unsigned char *base = (unsigned char *)mod;
        void *native = nullptr;
        if (!hfg_hook_thunk(base, 0x25C0u, 0x21F730u, nullptr, (void *)&hfg_present_detour, &native))
        {
            mgpu::diag::warn("[MGPU][R499] present thunk not hookable - observation skipped, provider untouched");
            return false;
        }
        g_hfg_base = base;
        g_hfg_native_present = (hfg_present_fn)native;
        g_hfg_obs_left = quota;
        char l[220];
        snprintf(l, sizeof l, "[MGPU][R499] present thunk 0x25C0 hooked -> passthrough, observing %d call(s)", quota);
        mgpu::diag::info(l);
        return true;
    }

    // ------------------------------------------------------------------
    // R500 (slice 5, step 2): schedule every frame of a 4X burst.
    //
    // R499 confirmed the call pattern in OUR chain, byte for byte against the
    // published analysis: the burst loop's presents arrive from 0x2202ED with
    // arg7=1 and arg6 stepping 8 bytes per frame, and the burst's last frame from
    // 0x220467 with arg7=0. That is the shape the reconstruction needs.
    //
    // Above count > 2 the provider computes a presentation time for the last frame
    // only, so a 4X burst is delivered as 0, i, 2i, 2i - the fourth frame reuses the
    // third's slot, which is exactly the "picture jumps backwards while panning"
    // the operator reported at 4X and nowhere else. The provider has a scheduler of
    // its own (0x21EE30) that computes the right time for a frame given its index;
    // it is simply never called for the frames the loop presents. This calls it.
    //
    // Everything is validated before it is used, and the whole body is inside an SEH
    // guard: a wrong pointer in someone else's present must disable this arm, not
    // kill the game. If any check fails the frame is forwarded untouched, i.e. the
    // provider behaves exactly as it did before this was written.
    typedef bool (*hfg_sched_fn)(void *, void *, unsigned char, void *, unsigned);
    typedef void *(*hfg_ring_fn)(void *, void *);

    hfg_sched_fn g_hfg_sched = nullptr;
    hfg_ring_fn g_hfg_ring_snap = nullptr;
    int g_hfg_sched_on = 0;
    int g_hfg_sched_log = 0;
    int g_hfg_sched_calls = 0;
    int g_hfg_sched_refused = 0;
    int g_hfg_sched_bad = 0;
    unsigned char *g_hfg_ring = nullptr;      // ctx + 0x168, captured when we schedule (R502 uses it)

    // The provider's own gate, transcribed: the scheduler only runs when the
    // limiter byte is clear and the enable byte is set.
    bool hfg_sched_usable(const void *ctx)
    {
        const unsigned char *b = (const unsigned char *)ctx;
        return b[0x340] == 0 && b[0x341] != 0;
    }

    // arg5 is present arg5 - 0x38, arg6 is frames + index*8 (R499 measured the
    // 8-byte step), arg7 is 1 for the loop's presents and 0 for the last one.
    void hfg_try_pace(void *ctx, void *arg5, void *arg6)
    {
        if (g_hfg_sched_on == 0 || g_hfg_sched == nullptr || arg5 == nullptr) return;
        unsigned char *burst = (unsigned char *)arg5 - 0x38;
        const unsigned long long count = *(unsigned long long *)(burst + 8);
        if (count < 1ull || count > 5ull) { ++g_hfg_sched_bad; return; }
        unsigned char *frames = *(unsigned char **)burst;
        if (frames == nullptr || (unsigned char *)arg6 < frames) { ++g_hfg_sched_bad; return; }
        const unsigned long long off = (unsigned long long)((unsigned char *)arg6 - frames);
        if ((off % 8ull) != 0ull) { ++g_hfg_sched_bad; return; }
        const unsigned long long index = off / 8ull;
        if (index < 1ull || index >= count) { ++g_hfg_sched_bad; return; }
        if (!hfg_sched_usable(ctx)) return;      // the provider would refuse: not an error

        unsigned char timing[0x20] = {};
        // The provider snapshots its own ring before scheduling; the block is 16
        // bytes {sample count, median} and the call writes, never reads it back.
        g_hfg_ring_snap((unsigned char *)ctx + 0x168, timing);
        g_hfg_ring = (unsigned char *)ctx + 0x168;   // R502's deadline detour reads +0x1B8
        const unsigned char gate = burst[0xC0] & 1;
        const bool ok = g_hfg_sched(ctx, burst, gate, timing, (unsigned)index);
        ++g_hfg_sched_calls;
        if (!ok) ++g_hfg_sched_refused;
        if (g_hfg_sched_log < 6)
        {
            ++g_hfg_sched_log;
            char l[200];
            snprintf(l, sizeof l, "[MGPU][R500] scheduled frame %llu of %llu -> %s", index, count,
                     ok ? "ok" : "REFUSED");
            mgpu::diag::info(l);
        }
    }

    long long hfg_present_paced(void *ctx, unsigned a2, unsigned a3, unsigned long long a4, void *arg5, void *arg6,
                                unsigned long long arg7)
    {
        if (g_hfg_sched_on != 0 && g_hfg_base != nullptr && (unsigned char)arg7 == 1)
        {
            const unsigned long long rva = (unsigned long long)((unsigned char *)_ReturnAddress() - g_hfg_base);
            if (rva == 0x2202EDull)   // the burst loop, and only it
            {
                __try { hfg_try_pace(ctx, arg5, arg6); }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    g_hfg_sched_on = 0;
                    mgpu::diag::error("[MGPU][R500] fault while scheduling a burst frame - per-frame pacing OFF for "
                                      "this session, the provider presents as it always did");
                }
            }
        }
        return g_hfg_native_present(ctx, a2, a3, a4, arg5, arg6, arg7);
    }
    // ------------------------------------------------------------------
    // R502 (slice 5, step 3): give the scheduler a deadline it can believe.
    //
    // R501's A/B settled who loses frames: with the per-frame scheduling OFF the
    // provider delivered 4 frames in 3117 of 3117 presents (3:0 4+:3117); with it ON
    // a third of the bursts delivered 3. So the provider never drops - our calls did,
    // because the deadline it computed for those frames was wrong.
    //
    // Why it is wrong (0x224B30, the single place a prediction time is produced):
    //     deadline = base + min(index * median / (count+1), f * index / (count+1))
    //                - recv
    // `base` is only refreshed when index == 1; for every other index it is whatever
    // that stack slot held. And the min() clamps the step down to `f`, a float smaller
    // than a real frame, so the whole burst is compressed. Both are repaired here at
    // the one choke point, which the provider calls once per scheduled frame.
    //
    // The rule, in one sentence: anchor the burst on its index-1 frame (the only one
    // whose base is real), then step every later frame by the interval the burst
    // actually wants - so the stale base and the clamp stop mattering.
    typedef void *(*hfg_ts_fn)(void *, long long *, void *, void *, unsigned, unsigned);

    hfg_ts_fn g_hfg_ts = nullptr;
    long long g_hfg_next_deadline = 0;
    long long g_hfg_step = 0;
    unsigned g_hfg_last_index = 0;
    unsigned g_hfg_last_count = 0;
    long long g_hfg_ts_calls = 0;
    long long g_hfg_ts_rebased = 0;
    int g_hfg_ts_log = 0;
    int g_hfg_ts_on = 0;

    void *hfg_ts_detour(void *a1, long long *out, void *lookup, void *timing, unsigned index, unsigned count_plus_1)
    {
        void *result = g_hfg_ts(a1, out, lookup, timing, index, count_plus_1);
        if (g_hfg_ts_on == 0 || out == nullptr || timing == nullptr) return result;
        if (index == 0u || index > 5u || count_plus_1 < 2u) return result;

        const long long median = *(const long long *)((const unsigned char *)timing + 8);
        long long unit = median / (long long)count_plus_1;
        if (unit <= 0) return result;
        ++g_hfg_ts_calls;

        // A fresh burst starts at index 1. Anything else that does not look like a
        // continuation (a jump back, a changed multiplier) re-anchors instead of
        // stepping from a stale schedule.
        const bool fresh = (index == 1u) || (index <= g_hfg_last_index) || (count_plus_1 != g_hfg_last_count);
        if (fresh)
        {
            // How much the provider's min() removed, recomputed the way it does it.
            long long native_unit = unit;
            if (g_hfg_ring != nullptr)
            {
                float f = *(const float *)(g_hfg_ring + 0x1B8);
                if (!(f >= 0.125f) || f > 500.0f) f = 500.0f;
                const long long f_ns = (long long)((double)f * 1000000.0);
                const long long clamped = f_ns / (long long)count_plus_1;
                if (clamped < native_unit) native_unit = clamped;
            }
            g_hfg_next_deadline = *out + (long long)index * (unit - native_unit);
            g_hfg_step = unit;
            ++g_hfg_ts_rebased;
        }
        else
        {
            g_hfg_next_deadline += g_hfg_step;
        }
        g_hfg_last_index = index;
        g_hfg_last_count = count_plus_1;
        *out = g_hfg_next_deadline;
        if (g_hfg_ts_log < 4)
        {
            ++g_hfg_ts_log;
            char l[220];
            snprintf(l, sizeof l, "[MGPU][R502] deadline frame %u/%u -> %lld ns (step %lld ns, %s)", index,
                     count_plus_1, g_hfg_next_deadline, g_hfg_step, fresh ? "anchored" : "stepped");
            mgpu::diag::info(l);
        }
        return result;
    }
    // Installs the scheduling detour. Returns false (and leaves the provider
    // untouched) when the thunk is not the expected shape.
    bool hfg_enable_sched(HMODULE mod, int on)
    {
        if (on == 0 || mod == nullptr) return false;
        if (g_hfg_sched != nullptr) { g_hfg_sched_on = 1; return true; }
        unsigned char *base = (unsigned char *)mod;
        void *native = nullptr;
        if (!hfg_hook_thunk(base, 0x25C0u, 0x21F730u, nullptr, (void *)&hfg_present_paced, &native))
            return false;
        g_hfg_base = base;
        g_hfg_native_present = (hfg_present_fn)native;
        g_hfg_sched = (hfg_sched_fn)(base + 0x21EE30u);
        g_hfg_ring_snap = (hfg_ring_fn)(base + 0x224CF0u);
        g_hfg_sched_on = 1;
        g_hfg_sched_log = 0;
        g_hfg_sched_calls = 0;
        g_hfg_sched_refused = 0;
        g_hfg_sched_bad = 0;

        // R502: the deadline half. Same thunk table, same 16-byte shape; its expected
        // bytes are the provider's own E9 rel32 plus int3 padding.
        {
            static const unsigned char ts_exp[16] = { 0xE9, 0xFB, 0x16, 0x22, 0x00, 0xCC, 0xCC, 0xCC,
                                                      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
            void *ts_native = nullptr;
            g_hfg_ts = (hfg_ts_fn)(base + 0x224B30u);
            if (hfg_hook_thunk(base, 0x3430u, 0x224B30u, ts_exp, (void *)&hfg_ts_detour, &ts_native))
            {
                g_hfg_ts = (hfg_ts_fn)ts_native;
                g_hfg_ts_on = 1;
                mgpu::diag::info("[MGPU][R502] deadline detour ON: each burst is anchored on its index-1 frame and "
                                 "stepped by the interval it asks for (thunk 0x3430)");
            }
            else
            {
                // Scheduling without a corrected deadline is what threw frames away
                // in R501, so refuse the whole arm rather than run it half.
                g_hfg_ts_on = 0;
                g_hfg_sched_on = 0;
                mgpu::diag::warn("[MGPU][R502] deadline thunk not hookable - per-frame scheduling stays OFF rather "
                                 "than running without the deadline correction");
            }
        }
        if (g_hfg_sched_on == 0) return false;
        mgpu::diag::info("[MGPU][R500] per-frame burst scheduling ON: every generated frame of a burst is handed to "
                         "the provider's own scheduler (thunk 0x25C0, scheduler 0x21EE30)");
        return true;
    }


    // ------------------------------------------------------------------
    // R503 (slice 5, step 4): WALL-CLOCK PACING - nothing in the provider is written.
    //
    // R501/R502 showed the two intrusive halves are not worth their risk here: the
    // scheduler call threw away a third of the frames (its deadline disagreed with
    // ours) and the deadline write froze the layer before the game even started.
    // This does the same job from the outside, with a counter and a QPC.
    //
    // R499 already measured everything needed:
    //     0x2202ED  arg7=1   one call per generated frame of the burst, in order
    //     0x220467  arg7=0   one call closing the burst
    // so a counter gives the frame's index and the previous burst's length - no burst
    // struct, no ring, no dereference of anything. Each frame is then held until its
    // own slot (burst start + index * period / (count+1)). That is exactly the piece
    // a 4X burst is missing: instead of handing four frames over at once with one
    // timestamp, they leave 6 ms apart, so the swapchain sees the frame that belongs
    // to now.
    //
    // The period is measured between burst starts and median-smoothed, not assumed.
    static const int HFG_CLK_SAMPLES = 9;
    LARGE_INTEGER g_hfg_qpf{};
    long long g_hfg_clk_last_start = 0;
    long long g_hfg_clk_period = 0;
    long long g_hfg_clk_target = 0;
    long long g_hfg_clk_samples[HFG_CLK_SAMPLES] = {};
    int g_hfg_clk_n = 0;
    int g_hfg_clk_pos = 0;
    unsigned g_hfg_clk_index = 0;
    unsigned g_hfg_clk_count = 3;          // learned from the closing call of the previous burst
    int g_hfg_clk_on = 0;
    long long g_hfg_clk_waits = 0;
    long long g_hfg_clk_skips = 0;
    int g_hfg_clk_log = 0;

    long long hfg_qpc_now()
    {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    void hfg_clk_wait(long long target)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // Busy-wait near the target, yield far from it: Sleep(1) rounds up to a whole
        // timer tick, which is coarser than the interval being hit.
        const long long near_ticks = g_hfg_qpf.QuadPart / 5000;   // 200 us
        while (now.QuadPart < target)
        {
            if ((target - now.QuadPart) > near_ticks) Sleep(0);
            else YieldProcessor();
            QueryPerformanceCounter(&now);
        }
    }

    void hfg_clk_note_period(long long ticks)
    {
        if (ticks <= 0) return;
        g_hfg_clk_samples[g_hfg_clk_pos] = ticks;
        g_hfg_clk_pos = (g_hfg_clk_pos + 1) % HFG_CLK_SAMPLES;
        if (g_hfg_clk_n < HFG_CLK_SAMPLES) ++g_hfg_clk_n;
        long long s[HFG_CLK_SAMPLES];
        memcpy(s, g_hfg_clk_samples, sizeof(long long) * (size_t)g_hfg_clk_n);
        for (int i = 1; i < g_hfg_clk_n; ++i)
        {
            const long long key = s[i];
            int j = i - 1;
            while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; --j; }
            s[j + 1] = key;
        }
        g_hfg_clk_period = s[g_hfg_clk_n / 2];
    }

    void hfg_clk_frame(bool is_last)
    {
        if (is_last)
        {
            // The closing call is the LAST GENERATED frame of the burst; the loop before it
            // presented 1..count-1, so count = last index + 1 = the GENERATED count (mfc).
            // Total frames of the burst = count generated + 1 real, hence count+1 slots and
            // step = period/(count+1). The real frame is presented by the app's Present after
            // this thunk returns, i.e. at the last slot.
            //
            // [FIX R503c, 2026-11-11] R503b divided by count and was measured wrong at 3X:
            // "paced closing frame of 2: step 17.15 ms, burst period 34.31 ms" -> 34.31/2,
            // while three frames need 34.31/3 = 11.44. The log line "of 3" at 4X proved the
            // count is the GENERATED count, not the total. Back to count+1; the closing
            // frame is still held one step, which is what R503b added correctly.
            const unsigned idx = g_hfg_clk_index;
            g_hfg_clk_count = idx + 1u;
            g_hfg_clk_index = 0;
            if (g_hfg_clk_period > 0 && idx >= 1u && g_hfg_clk_count >= 1u)
            {
                const long long step = g_hfg_clk_period / (long long)(g_hfg_clk_count + 1u);
                const long long now = hfg_qpc_now();
                if (step > 0)
                {
                    g_hfg_clk_target += step;
                    if (g_hfg_clk_target > now + g_hfg_clk_period) { ++g_hfg_clk_skips; }
                    else
                    {
                        hfg_clk_wait(g_hfg_clk_target);
                        ++g_hfg_clk_waits;
                        if (g_hfg_clk_log < 6)
                        {
                            ++g_hfg_clk_log;
                            char l[220];
                            snprintf(l, sizeof l,
                                     "[MGPU][R503] paced closing frame of %u: step %.2f ms, burst period %.2f ms",
                                     g_hfg_clk_count + 1u,
                                     1000.0 * (double)step / (double)g_hfg_qpf.QuadPart,
                                     1000.0 * (double)g_hfg_clk_period / (double)g_hfg_qpf.QuadPart);
                            mgpu::diag::info(l);
                        }
                    }
                }
            }
            return;
        }
        ++g_hfg_clk_index;
        const long long now = hfg_qpc_now();
        if (g_hfg_clk_index == 1u)
        {
            if (g_hfg_clk_last_start != 0) hfg_clk_note_period(now - g_hfg_clk_last_start);
            g_hfg_clk_last_start = now;
            g_hfg_clk_target = now;
            return;
        }
        if (g_hfg_clk_period <= 0) return;              // first burst: nothing to pace against
        const long long step = g_hfg_clk_period / (long long)(g_hfg_clk_count + 1u);
        if (step <= 0) return;
        g_hfg_clk_target += step;
        // A hitch that already put us a whole frame behind skips the wait for this
        // burst instead of stalling the thread trying to catch up.
        if (g_hfg_clk_target > now + g_hfg_clk_period) { ++g_hfg_clk_skips; return; }
        hfg_clk_wait(g_hfg_clk_target);
        ++g_hfg_clk_waits;
        if (g_hfg_clk_log < 4)
        {
            ++g_hfg_clk_log;
            char l[220];
            snprintf(l, sizeof l, "[MGPU][R503] paced frame %u of %u: step %.2f ms, burst period %.2f ms",
                     g_hfg_clk_index, g_hfg_clk_count + 1u,
                     1000.0 * (double)step / (double)g_hfg_qpf.QuadPart,
                     1000.0 * (double)g_hfg_clk_period / (double)g_hfg_qpf.QuadPart);
            mgpu::diag::info(l);
        }
    }

    long long hfg_present_clock(void *ctx, unsigned a2, unsigned a3, unsigned long long a4, void *arg5, void *arg6,
                                unsigned long long arg7)
    {
        if (g_hfg_clk_on != 0 && g_hfg_base != nullptr)
        {
            const unsigned long long rva = (unsigned long long)((unsigned char *)_ReturnAddress() - g_hfg_base);
            const bool flag = ((unsigned char)arg7) == 1;
            if (rva == 0x2202EDull && flag) hfg_clk_frame(false);
            else if (rva == 0x220467ull && !flag) hfg_clk_frame(true);
        }
        return g_hfg_native_present(ctx, a2, a3, a4, arg5, arg6, arg7);
    }

    bool hfg_enable_clock(HMODULE mod, int on)
    {
        if (on == 0 || mod == nullptr) return false;
        if (g_hfg_clk_on != 0) return true;
        unsigned char *base = (unsigned char *)mod;
        void *native = nullptr;
        if (!hfg_hook_thunk(base, 0x25C0u, 0x21F730u, nullptr, (void *)&hfg_present_clock, &native)) return false;
        g_hfg_base = base;
        g_hfg_native_present = (hfg_present_fn)native;
        QueryPerformanceFrequency(&g_hfg_qpf);
        if (g_hfg_qpf.QuadPart <= 0) return false;
        g_hfg_clk_last_start = 0;
        g_hfg_clk_period = 0;
        g_hfg_clk_target = 0;
        g_hfg_clk_n = 0;
        g_hfg_clk_pos = 0;
        g_hfg_clk_index = 0;
        g_hfg_clk_count = 3;    // [FIX R503c] GENERATED frames of a burst; 3 = 4X, learned from the first one
        g_hfg_clk_waits = 0;
        g_hfg_clk_skips = 0;
        g_hfg_clk_log = 0;
        g_hfg_clk_on = 1;
        mgpu::diag::info("[MGPU][R503] wall-clock burst pacing ON: frames of a burst leave one slot apart, measured "
                         "from burst starts (present thunk 0x25C0; nothing in the provider is written)");
        return true;
    }

    // The handover proper. One SEH guard around the whole vendor sequence, on
    // the display layer's present thread: an access violation inside 22 MB of
    // someone else's code becomes one log line instead of a dead game.
    struct hfg_state
    {
        efg_api api{};
        xell_api xa{};
        void *ctx = nullptr;
        void *xell = nullptr;
        bool ok = false;
        bool gen = false;      // R490: SetEnabled(1) was accepted AND nobody has faulted since
        unsigned gen_ids = 0;  // R490: how many frames have been tagged
        int gen_ni = 0;        // R492: interpolated frames per real frame the unlock arm reached (0 = 2X)
        int pace = 0;          // R493: XeFGPace - XeLL gets the sleep primitive, the present markers and is asked
                               //       for its own frame report; 0 = the provider paces as it always did
        int applied_on = -1;   // R497: last value handed to SetEnabled (-1 = never called)
        int applied_ni = 0;    // R497: last accepted SetNumInterpolatedFrames
        int need_reset = 0;    // R497: one frame of resetHistory after a multiplier or on/off change
        bool warned_no_num_frames = false;
    };

    // R490: the context now has two users - the handover at init and the tag path
    // on every present - so it lives in one cell instead of inside the handover.
    hfg_state &hfg_st()
    {
        static hfg_state h;
        return h;
    }

    bool hfg_body(hfg_state &h, ID3D12Device *dev, ID3D12CommandQueue *q, HWND hwnd,
                  const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIFactory2 *fac,
                  IDXGISwapChain1 **out_sc, IDXGISwapChain3 **out_sc3)
    {
        char l[512];
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return false;
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash != nullptr) *slash = 0;
        wchar_t p[MAX_PATH * 2] = {};

        // ---- 1. libxess_fg. mgpu\ first: on a game whose own copy predates
        //         non-Intel support (Cyberpunk 2077's does: API 1.1.0) the
        //         game's copy cannot answer on GPU 1. ----
        _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\mgpu\\libxess_fg.dll", exe);
        HMODULE mod = hfg_exists(p) ? LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (mod == nullptr)
        {
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\libxess_fg.dll", exe);
            if (hfg_exists(p)) mod = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (mod == nullptr) mod = LoadLibraryW(L"libxess_fg.dll");
        if (mod == nullptr)
        {
            snprintf(l, sizeof l, "[MGPU][R489] handover NOT attempted - libxess_fg.dll not loadable (err=%lu). "
                     "The display layer keeps its own chain.", (unsigned long)GetLastError());
            mgpu::diag::error(l);
            return false;
        }
        {
            char miss[96] = {};
            wchar_t mp[MAX_PATH * 2] = {}; wchar_t shown[192];
            if (GetModuleFileNameW(mod, mp, MAX_PATH * 2) == 0) wcscpy_s(mp, L"?");
            hfg_tail(mp, shown, 192);
            if (!hfg_load_xefg(h.api, mod, miss, sizeof miss))
            {
                snprintf(l, sizeof l, "[MGPU][R489] handover NOT attempted - %s missing from module=%ls", miss, shown);
                mgpu::diag::error(l);
                return false;
            }
            snprintf(l, sizeof l, "[MGPU][R489] xefg module=%ls", shown);
            mgpu::diag::info(l);
        }

        // ---- 1b. R492: the MFG gate, before ANYTHING asks the provider a question ----
        // Placed here on purpose: U3/U4 shape the init path and U5 shapes what
        // GetProperties reports, so patching after CreateContext would mean the
        // two halves disagree - exactly the mismatch that makes an unlock look
        // like it did nothing.
        h.gen_ni = hfg_unlock(mod, mgpu::gpu1::ui_ini_read("XeFGUnlock", 0));

        // R499: observe the present thunk. Pure passthrough - the provider behaves
        // exactly as shipped, and the log says which call sites arrive. This is the
        // prerequisite for scheduling every frame of the 4X burst, and it is the
        // step that must be right before any of that is written.
        //
        // R500: the two are mutually exclusive by construction - both replace the
        // same 16-byte thunk, so the second install would refuse. XeFGSched wins
        // when it is set, because it is the arm that can actually fix 4X.
        {
            const int clock_on = mgpu::gpu1::ui_ini_read("XeFGClock", 0);
            const int sched_on = mgpu::gpu1::ui_ini_read("XeFGSched", 0);
            if (clock_on != 0)
            {
                // [FIX attribution, 2026-11-11] A REFUSED install used to be silent:
                // hfg_enable_clock logs only on success, so "the key is set but nothing
                // happened" and "the key was not read" looked identical in the log. The
                // operator judges this arm by feel; the log has to say whether it ran.
                if (!hfg_enable_clock(mod, clock_on))
                    mgpu::diag::warn("[MGPU][R503] XeFGClock=1 requested but the present thunk 0x25C0 was NOT hooked "
                                     "- burst pacing is OFF this session (provider bytes differ from the known build?)");
            }
            else if (sched_on != 0)
            {
                if (!hfg_enable_sched(mod, sched_on))
                    mgpu::diag::warn("[MGPU][R502] XeFGSched=1 requested but the scheduler/deadline hook was NOT "
                                     "installed - the provider runs exactly as shipped this session");
            }
            else
            {
                const int obs_on = mgpu::gpu1::ui_ini_read("XeFGObserve", 0);
                mgpu::diag::info(obs_on != 0
                                     ? "[MGPU][R499] XeFGClock and XeFGSched are both off - no burst pacing this "
                                       "session (observer only)"
                                     : "[MGPU][R499] XeFGClock and XeFGSched are both off - no burst pacing this session");
                hfg_observe(mod, obs_on);
            }
        }

        // ---- 2. XeLL. It must be the instance XeFG itself resolves by plain
        //         name, which is the copy the host already loaded from the exe
        //         directory. Building a context from any other copy is rejected
        //         with -15 (2a measured exactly that for 1.3.0 and 1.3.2 under
        //         other paths). ----
        HMODULE xmod = GetModuleHandleW(L"libxell.dll");
        if (xmod == nullptr)
        {
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\libxell.dll", exe);
            if (hfg_exists(p)) xmod = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (xmod == nullptr)
        {
            mgpu::diag::error("[MGPU][R489] handover NOT attempted - no libxell.dll in the process or beside the "
                              "exe. XeFG cannot initialise without one (-15).");
            return false;
        }
        {
            char miss[96] = {};
            wchar_t mp[MAX_PATH * 2] = {}; wchar_t shown[192];
            if (GetModuleFileNameW(xmod, mp, MAX_PATH * 2) == 0) wcscpy_s(mp, L"?");
            hfg_tail(mp, shown, 192);
            if (!hfg_load_xell(h.xa, xmod, miss, sizeof miss))
            {
                snprintf(l, sizeof l, "[MGPU][R489] handover NOT attempted - %s missing from module=%ls", miss, shown);
                mgpu::diag::error(l);
                return false;
            }
            char vs[40];
            snprintf(vs, sizeof vs, "n/a");
            if (h.xa.get_version != nullptr) { unsigned short v[4] = {}; if (h.xa.get_version(v) == 0) snprintf(vs, sizeof vs, "%u.%u.%u", (unsigned)v[0], (unsigned)v[1], (unsigned)v[2]); }
            snprintf(l, sizeof l, "[MGPU][R489] xell module=%ls version=%s", shown, vs);
            mgpu::diag::info(l);
        }

        // ---- 3. context ----
        void *ctx = nullptr;
        int r = h.api.create_context(dev, &ctx);
        snprintf(l, sizeof l, "[MGPU][R489] CreateContext res=%d (%s) ctx=%p", r, xefg_result_name(r), ctx);
        mgpu::diag::info(l);
        if (r != 0 || ctx == nullptr) return false;
        h.ctx = ctx;
        if (h.api.set_logging_callback != nullptr)
            h.api.set_logging_callback(h.ctx, 1 /*INFO*/, hfg_log_cb, nullptr);

        // ---- 4. XeLL context, with low-latency mode ON: with it off, XeFG
        //         refuses SetEnabled with -15 "Latency reduction is not
        //         enabled" (2a measured both states). ----
        void *xc = nullptr;
        const int zr = h.xa.create_context(dev, &xc);
        snprintf(l, sizeof l, "[MGPU][R489] xellD3D12CreateContext res=%d xell=%p", zr, xc);
        mgpu::diag::info(l);
        if (zr != 0 || xc == nullptr) return false;
        h.xell = xc;
        if (h.xa.set_sleep_mode != nullptr)
        {
            efg_sleep_params sp = {};
            sp.minimum_interval_us = 0;   // no fps cap
            sp.flags = 1u;                // bit0 = bLowLatencyMode, REQUIRED
            const int sr = h.xa.set_sleep_mode(h.xell, &sp);
            snprintf(l, sizeof l, "[MGPU][R489] xellSetSleepMode(cap=0, lowLatency=1) res=%d", sr);
            mgpu::diag::info(l);
        }
        r = h.api.set_latency_reduction(h.ctx, h.xell);
        snprintf(l, sizeof l, "[MGPU][R489] SetLatencyReduction res=%d (%s)", r, xefg_result_name(r));
        mgpu::diag::info(l);

        // R493: arm the vendor pacer. Read once, here, so the display thread
        // never touches the ini file.
        h.pace = mgpu::gpu1::ui_ini_read("XeFGPace", 0);
        if (h.pace != 0)
        {
            snprintf(l, sizeof l, "[MGPU][R493] XeLL pacing arm=%d: sleep=%s markers=%s reports=%s", h.pace,
                     (h.xa.sleep != nullptr) ? "yes" : "MISSING",
                     (h.xa.add_marker != nullptr) ? "yes" : "MISSING",
                     (h.xa.frames_reports != nullptr) ? "yes" : "MISSING");
            mgpu::diag::info(l);
        }
        if (r != 0) return false;

        // ---- 5. properties, then the handover. Announced before it happens:
        //         if the log ends here, this line names the call that took the
        //         process down. ----
        efg_props props = {};
        r = h.api.get_properties(h.ctx, &props);
        snprintf(l, sizeof l, "[MGPU][R489] GetProperties res=%d maxInterp=%u (before init: other fields are 0 by contract)",
                 r, props.max_supported_interpolations);
        mgpu::diag::info(l);
        if (r != 0) return false;

        // What exactly is being asked for. 2a's throwaway chain was 480x270
        // fmt=24 with 2 buffers; this is the display layer's window at 4K
        // fmt=28. Four 2b-1 runs established that the difference is NOT what
        // killed them - the second chain on an HWND is refused whatever its
        // shape - so this line is now the record of what XeFG is being asked to
        // create, not a suspect.
        if (desc != nullptr)
        {
            snprintf(l, sizeof l,
                     "[MGPU][R489] asking XeFG to CREATE the chain: %ux%u fmt=%u buffers=%u swapEffect=%u "
                     "scaling=%u alpha=%u flags=0x%X on hwnd=%p",
                     desc->Width, desc->Height, (unsigned)desc->Format, desc->BufferCount,
                     (unsigned)desc->SwapEffect, (unsigned)desc->Scaling, (unsigned)desc->AlphaMode,
                     desc->Flags, (void *)hwnd);
            mgpu::diag::info(l);
        }

        efg_init_params ip = {};
        ip.pApplicationSwapChain = nullptr;   // 2b-1': XeFG creates the one and only chain
        // R496: THE FLAGS WERE 0, AND THAT WAS A CLAIM ABOUT THE DATA, not a default.
        //
        // 0 means "the depth is ordinary Z" and "the velocity does not carry
        // jitter". This title is reversed-Z, and its vectors do carry the frame's
        // jitter. Telling the generator the opposite does not fail any check - it
        // just makes it compute occlusions with the depth comparison inverted, which
        // shows up as the picture jumping backwards now and then while panning, with
        // zero dropped frames and a perfect 2.00 vblanks/frame. Measured that way on
        // 2026-10-31 00:39 at 2X: late=1 for the whole session, copy=0.09,
        // present=0.87 - the timing was innocent, so the content had to be.
        //
        // One flag at a time: inverted depth first, because a wrong depth SIGN is
        // the single most likely way to misplace a generated frame. XeFGFlags lets
        // it be turned back off without a rebuild.
        {
            const int fl = mgpu::gpu1::ui_ini_read("XeFGFlags", 1);   // bit0 = inverted depth
            ip.initFlags = (fl & 1) ? EFG_INIT_FLAG_INVERTED_DEPTH : 0u;
            snprintf(l, sizeof l, "[MGPU][R496] initFlags=0x%X (XeFGFlags=%d, inverted depth %s)", ip.initFlags, fl,
                     (ip.initFlags & EFG_INIT_FLAG_INVERTED_DEPTH) ? "DECLARED" : "not declared");
            mgpu::diag::info(l);
        }
        // R492: the unlock arm decides which counts are REACHABLE; the panel decides
        // which one is ACTIVE.
        //
        // [COPY OptiScaler, 2026-11-11] THE INIT PARAMETER IS THE WANTED COUNT, NOT THE
        // CEILING. XeFG_Dx12.cpp:329-349 hands `intTarget` (the configured count) to
        // D3D12InitFromSwapChainDesc, and only changes it later through
        // SetNumInterpolatedFrames. We were handing it the unlock maximum (5), and with
        // the patched provider the count it starts at IS that maximum - which is the
        // 2X-start-runs-at-4X bug. Passing the wanted count makes the very first frame
        // right and leaves the maximum to the patched DLL.
        {
            int want_ni = (int)mgpu::fgmode::get_multi();
            if (want_ni < 1) want_ni = 1;
            // No unlock = the provider's own maximum is 1: never ask it for more than it
            // can do at init, or the whole handover is refused instead of running 2X.
            const int ceiling = (h.gen_ni > 0) ? h.gen_ni : 1;
            if (want_ni > ceiling) want_ni = ceiling;
            ip.maxInterpolatedFrames = (unsigned)want_ni;
            snprintf(l, sizeof l,
                     "[MGPU][R489] init interpolation count = %d (panel asks %d generated frame(s), unlock "
                     "ceiling %d) - this is the count the provider STARTS at, not its maximum",
                     want_ni, (int)mgpu::fgmode::get_multi(), h.gen_ni);
            mgpu::diag::info(l);
        }
        ip.creationNodeMask = 1;
        ip.visibleNodeMask = 1;
        ip.uiMode = EFG_UI_MODE_NONE;
        mgpu::diag::info("[MGPU][R489] calling InitFromSwapChainDesc on the display layer's WINDOW - XeFG creates "
                         "the only chain this HWND will ever have, which is the point: DXGI allows exactly one");
        r = h.api.init_from_desc(h.ctx, hwnd, desc, nullptr, q, fac, &ip);
        snprintf(l, sizeof l, "[MGPU][R489] InitFromSwapChainDesc res=%d (%s)", r, xefg_result_name(r));
        mgpu::diag::info(l);
        if (r != 0) return false;

        if (h.api.get_init_params != nullptr)
        {
            efg_init_params out = {};
            const int r2 = h.api.get_init_params(h.ctx, &out);
            snprintf(l, sizeof l, "[MGPU][R489] GetInitializationParameters res=%d appliedMaxInterp=%u appChain=%s "
                     "node=%u/%u uiMode=%d", r2, out.maxInterpolatedFrames,
                     (out.pApplicationSwapChain == nullptr) ? "NULL(chain re-created)" : "kept",
                     out.creationNodeMask, out.visibleNodeMask, out.uiMode);
            mgpu::diag::info(l);
        }

        IDXGISwapChain3 *proxy3 = nullptr;
        r = h.api.get_swapchain_ptr(h.ctx, __uuidof(IDXGISwapChain3), (void **)&proxy3);
        snprintf(l, sizeof l, "[MGPU][R489] GetSwapChainPtr(IDXGISwapChain3) res=%d (%s) proxy=%p",
                 r, xefg_result_name(r), (void *)proxy3);
        mgpu::diag::info(l);
        if (r != 0 || proxy3 == nullptr)
        {
            // The one failure that cannot be papered over: the chain was
            // consumed and nothing came back.
            mgpu::diag::error("[MGPU][R489] INIT SUCCEEDED BUT NO PROXY CAME BACK - there is no present path left "
                              "for the display layer. It will stop presenting rather than pretend.");
            return false;
        }

        IDXGISwapChain1 *proxy1 = nullptr;
        if (FAILED(proxy3->QueryInterface(IID_PPV_ARGS(&proxy1))) || proxy1 == nullptr)
        {
            mgpu::diag::error("[MGPU][R489] proxy does not expose IDXGISwapChain1 - cannot present. Chain consumed.");
            proxy3->Release();
            return false;
        }

        // ---- 6. generation: ON only when the second flag asks for it ----
        //
        // 2b-1 measured SetEnabled(0) - the chain works and the picture is
        // unchanged. 2b-2 asks the next question, and a frame generator that is
        // enabled but never tagged produces nothing but a long log, so the two
        // travel together: enabled here, tagged per frame below.
        const bool want_gen = hfg_gen_flag_on();
        const int er = h.api.set_enabled(h.ctx, want_gen ? 1u : 0u);
        snprintf(l, sizeof l, "[MGPU][R489] SetEnabled(%d) res=%d (%s) - %s", want_gen ? 1 : 0, er,
                 xefg_result_name(er),
                 (er != 0) ? "REFUSED, generation stays off"
                           : (want_gen ? "generation ARMED: every tag made from here interpolates"
                                       : "generation OFF for 2b-1; the picture must be identical to the path without XeFG"));
        mgpu::diag::info(l);
        h.gen = (er == 0 && want_gen);
        // R497: remember what the provider is actually doing, so the first live
        // comparison in the display loop does not send a redundant call.
        //
        // [FIX X13, 2026-11-11] IT STARTS AT THE MAX, NOT AT 1. The old line below
        // said `applied_ni = 1` on the measured claim that "the count after init is 1
        // even though the maximum handed in is the patched ceiling". With the patched
        // DLL that claim is false: a run that asked for 2X at launch (FgMulti=2 ->
        // fgmode multi=1) delivered 4+ frames on 599 of its first 600 presents
        // (mgpu_run.28060_5.log), because want_ni(1) == applied_ni(1) made
        // mgpu_xefg_apply_mode return without ever calling SetNumInterpolatedFrames,
        // and the provider stayed at the 5 it was initialised with. That is the
        // "panel says 2X, screen is 4X, picture is mushy" state.
        //
        // -1 = "unknown, force the first apply", so a 2X start now really is 2X and
        // logs [R497] SetNumInterpolatedFrames(1) -> 2X at handover.
        h.applied_on = want_gen ? 1 : 0;
        h.applied_ni = -1;

        *out_sc = proxy1;
        *out_sc3 = proxy3;
        h.ok = true;
        return true;
    }

    // R490: the per-frame tag. Runs on the DISPLAY thread, on the command list
    // that is about to present, before that list is closed.
    //
    // Nothing here allocates, waits, or inspects the caller's state: the two
    // textures handed in are the slot copies made on the bridge side, already
    // resting in ALL_SHADER_RESOURCE - exactly the state XeFG is told to expect
    // and is expected to hand back.
    int hfg_tag_body(ID3D12CommandList *cl, unsigned pid, const disp_fg_params *fc,
                     ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
                     ID3D12Resource *mvec, D3D12_RESOURCE_STATES mvec_state,
                     unsigned bb_w, unsigned bb_h)
    {
        hfg_state &h = hfg_st();
        if (!h.gen || h.ctx == nullptr || cl == nullptr || fc == nullptr || pid == 0u) return 1;

        efg_frame_constants k = {};
        const mgpu::fgcamera::Params &cam = mgpu::fgcamera::get();
        k.view[0] = k.view[5] = k.view[10] = k.view[15] = 1.0f;
        for (int i = 0; i < 16; ++i) k.projection[i] = cam.viewToClip[i];
        k.jitter_x = fc->jitter_x;
        k.jitter_y = fc->jitter_y;
        k.mv_scale_x = fc->mv_scale_x;
        k.mv_scale_y = fc->mv_scale_y;
        k.reset_history = fc->reset ? 1u : 0u;
        if (h.need_reset != 0)
        {
            // R497: the frame after a multiplier or on/off change must not be blended
            // with history from the old timeline - one flag against a whole burst of
            // smear.
            k.reset_history = 1u;
            h.need_reset = 0;
        }
        k.frame_render_time_ms = fc->frame_ms;
        h.api.tag_frame_constants(h.ctx, (uint32_t)pid, &k);

        // Once, the exact numbers being handed over. 2b-2's whole claim is that
        // these are measurements and not constants, and a claim like that has to
        // be readable in the log or it is folklore.
        if (h.gen_ids == 0u && depth != nullptr && mvec != nullptr)
        {
            const D3D12_RESOURCE_DESC dd = depth->GetDesc();
            const D3D12_RESOURCE_DESC md = mvec->GetDesc();
            char il[340];
            snprintf(il, sizeof il,
                     "[MGPU][R490] first tag: id=%u depth=%ux%u fmt=%u | mvec=%ux%u fmt=%u | jitter=%.4f,%.4f "
                     "mvscale=%.3f,%.3f reset=%u frame=%.2fms view=identity proj=m00 %.4f m11 %.4f",
                     pid, (unsigned)dd.Width, dd.Height, (unsigned)dd.Format,
                     (unsigned)md.Width, md.Height, (unsigned)md.Format,
                     (double)fc->jitter_x, (double)fc->jitter_y,
                     (double)fc->mv_scale_x, (double)fc->mv_scale_y, fc->reset,
                     (double)fc->frame_ms, (double)k.projection[0], (double)k.projection[5]);
            mgpu::diag::info(il);
        }

        ID3D12Resource *res[2] = { depth, mvec };
        const D3D12_RESOURCE_STATES st[2] = { depth_state, mvec_state };
        for (int i = 0; i < 2; ++i)
        {
            if (res[i] == nullptr) return 2;
            const D3D12_RESOURCE_DESC d = res[i]->GetDesc();
            efg_res_data rd = {};
            rd.type = (i == 0) ? EFG_RES_DEPTH : EFG_RES_MVEC;
            rd.validity = EFG_RV_ONLY_NOW;
            rd.size_x = (uint32_t)d.Width;
            rd.size_y = d.Height;
            rd.pResource = res[i];
            // [COPY OptiScaler, 2026-11-11] THE STATE IS THE RESOURCE'S OWN, NOT A CONSTANT.
            // OptiScaler passes `fResource->state` (XeFG_Dx12.cpp:191, and COPY_DEST for a
            // resource it copied at present time, :1792). Hardcoding ALL_SHADER_RESOURCE here
            // declared a state our live textures are not in (they rest in tex_in_rest), which
            // is an illegal barrier for the library and invisible in a release build.
            rd.incomingState = (int)st[i];
            const int r = h.api.tag_frame_resource(h.ctx, cl, (uint32_t)pid, &rd);
            if (r != 0)
            {
                if (h.gen_ids < 4u)
                {
                    char tl[260];
                    snprintf(tl, sizeof tl,
                             "[MGPU][R490] TagFrameResource(%s) res=%d (%s) at present id %u - this frame presents "
                             "without an interpolated sibling",
                             (i == 0) ? "DEPTH" : "MVEC", r, xefg_result_name(r), pid);
                    mgpu::diag::warn(tl);
                }
                return 10 + r;
            }
        }

        // [COPY OptiScaler, 2026-11-11] THE BACKBUFFER REGION. XeFG_Dx12.cpp:957-966 tags
        // XEFG_SWAPCHAIN_RES_BACKBUFFER with XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, a dummy
        // command list and ONLY the region (the SDK says only the region is read), so the
        // generator knows which part of the chain's current backbuffer is this real frame.
        // We never told it; the provider then has to assume the whole backbuffer.
        if (bb_w != 0u && bb_h != 0u)
        {
            efg_res_data bb = {};
            bb.type = EFG_RES_BACKBUFFER;
            bb.validity = EFG_RV_UNTIL_NEXT_PRESENT;
            bb.base_x = 0u;
            bb.base_y = 0u;
            bb.size_x = bb_w;
            bb.size_y = bb_h;
            bb.pResource = nullptr;
            bb.incomingState = 0;
            const int rb = h.api.tag_frame_resource(h.ctx, (ID3D12CommandList *)1, (uint32_t)pid, &bb);
            if (rb != 0)
            {
                if (h.gen_ids < 4u)
                {
                    char bl[220];
                    snprintf(bl, sizeof bl,
                             "[MGPU][R490] TagFrameResource(BACKBUFFER) %ux%u res=%d (%s) at present id %u",
                             bb_w, bb_h, rb, xefg_result_name(rb), pid);
                    mgpu::diag::warn(bl);
                }
                return 20 + rb;
            }
        }
        ++h.gen_ids;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// R490: the three calls the display thread makes per frame. All of them are
// no-ops when generation is not armed, which is what keeps 2b-1' behaviour one
// deleted flag file away.
// ---------------------------------------------------------------------------
bool mgpu_xefg_gen_active()
{
    return hfg_st().gen;
}

int mgpu_xefg_gen_tag(ID3D12CommandList *cl, unsigned present_id, const disp_fg_params *fc,
                      ID3D12Resource *depth, ID3D12Resource *mvec)
{
    __try
    {
        // The display-layer arm hands in its own slot copies, which really do rest in
        // ALL_SHADER_RESOURCE (display_layer.inc:1577-1604). No backbuffer tag: that arm
        // presents through its own chain and never had to describe a region.
        return hfg_tag_body(cl, present_id, fc,
                            depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            mvec, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, 0u, 0u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        char l[240];
        snprintf(l, sizeof l,
                 "[MGPU][R490] SEH FAULT 0x%08X while tagging present id %u - generation switched off for this "
                 "session; the picture keeps presenting", (unsigned)GetExceptionCode(), present_id);
        mgpu::diag::error(l);
        hfg_st().gen = false;
        return -1;
    }
}

// [COPY OptiScaler, 2026-11-11] THE AUTHOR-CHAIN TAG: live textures + their REAL states,
// plus the backbuffer region. Same three calls OptiScaler makes in XeFG_Dx12::Dispatch
// (TagFrameConstants -> depth/velocity with the resource's own state -> backbuffer), and
// the same reason: a tag that names a state the resource is not in is a lie the driver
// inherits on the barrier it records from it.
int mgpu_xefg_gen_tag_ex(ID3D12CommandList *cl, unsigned present_id, const disp_fg_params *fc,
                         ID3D12Resource *depth, unsigned depth_state,
                         ID3D12Resource *mvec, unsigned mvec_state,
                         unsigned bb_w, unsigned bb_h)
{
    __try
    {
        return hfg_tag_body(cl, present_id, fc, depth, (D3D12_RESOURCE_STATES)depth_state,
                            mvec, (D3D12_RESOURCE_STATES)mvec_state, bb_w, bb_h);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        char l[240];
        snprintf(l, sizeof l,
                 "[MGPU][R490] SEH FAULT 0x%08X while tagging present id %u - generation switched off for this "
                 "session; the picture keeps presenting", (unsigned)GetExceptionCode(), present_id);
        mgpu::diag::error(l);
        hfg_st().gen = false;
        return -1;
    }
}

void mgpu_xefg_gen_set_present_id(unsigned present_id)
{
    hfg_state &h = hfg_st();
    if (!h.gen || h.ctx == nullptr || h.api.set_present_id == nullptr) return;
    __try { h.api.set_present_id(h.ctx, (uint32_t)present_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h.gen = false; }
}

void mgpu_xefg_gen_after_present(unsigned *frames_presented, int *gen_result, unsigned *gen_enabled)
{
    if (frames_presented != nullptr) *frames_presented = 1u;
    if (gen_result != nullptr) *gen_result = 0;
    if (gen_enabled != nullptr) *gen_enabled = 0;
    hfg_state &h = hfg_st();
    if (!h.gen || h.ctx == nullptr || h.api.get_last_present_status == nullptr) return;
    efg_present_status ps = {};
    __try { h.api.get_last_present_status(h.ctx, &ps); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h.gen = false; return; }
    if (frames_presented != nullptr) *frames_presented = ps.frames_presented;
    if (gen_result != nullptr) *gen_result = ps.frame_gen_result;
    if (gen_enabled != nullptr) *gen_enabled = ps.is_frame_gen_enabled;
}

// ---------------------------------------------------------------------------
// R493: XeLL as the pacer, and its own frame reports as the ruler.
//
// WHY HERE AND NOT IN THE PROVIDER. Above 2X the provider hands every generated
// frame of a burst to the swapchain back to back and schedules only the burst's
// last one. The published fix rewrites three internal thunks and calls the
// provider's own scheduler per frame with reconstructed arguments (about 700
// lines of reverse engineering, with a recorded C0000005 signature). Before
// paying that, the vendor's own component gets its chance: XeLL is the
// low-latency/pacing library, it is already attached to the provider through
// SetLatencyReduction, and three of its documented calls are unused here - the
// sleep primitive, the frame markers that tell it when a present happens, and
// the per-frame report that says what it measured.
//
// The report is the point of this first step: it carries timestamps for the last
// 64 frames. If four frames of a burst show up there spaced by one refresh, the
// burst is already spread and there is nothing to pace. If they share a
// timestamp, the clump is MEASURED - which is the only thing that justifies the
// thunk rewrite afterwards. Reading it costs one call every few seconds.
//
// GATED BY XeFGPace (0 = off, the default). Every call is reversible with one
// ini edit, a refusal is logged and never fatal, and a fault disables the arm
// for the session instead of taking the picture down.
struct hfg_frame_report
{
    uint32_t frame_id;
    uint32_t pad;
    uint64_t sim_start, sim_end;
    uint64_t rs_start, rs_end;
    uint64_t present_start, present_end;
    uint64_t reserved[5];
};

void hfg_pace_marker(unsigned frame_id, int marker)
{
    hfg_state &h = hfg_st();
    if (h.pace == 0 || !h.gen || h.xell == nullptr || h.xa.add_marker == nullptr) return;
    __try { h.xa.add_marker(h.xell, (uint32_t)frame_id, marker); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h.pace = 0; }
}

// xell_latency_marker_type_t: PRESENT_START = 4, PRESENT_END = 5.
//
// XeFGPace = 1 measures only (markers + the frame report); = 2 also hands each
// present to the vendor's sleep primitive. Two levels on purpose: the sleep call
// is the one that can block the display thread, and it must be reachable on its
// own rather than tangled into the step that merely observes.
void mgpu_xefg_pace_present_begin(unsigned frame_id)
{
    hfg_state &h = hfg_st();
    if (h.pace == 0 || !h.gen) return;
    hfg_pace_marker(frame_id, 4);
    if (h.pace >= 2 && h.xell != nullptr && h.xa.sleep != nullptr)
    {
        __try { h.xa.sleep(h.xell, (uint32_t)frame_id); }
        __except (EXCEPTION_EXECUTE_HANDLER) { h.pace = 0; }
    }
}

void mgpu_xefg_pace_present_end(unsigned frame_id)
{
    hfg_pace_marker(frame_id, 5);
}

// R509: THE OTHER FOUR MARKERS. XeLL's enum (xell_latency_marker_type_t, and our
// local copy of the SDK header agrees byte for byte with Intel's) has SIX entries
// and the SDK marks every one of the first six "required":
//
//   0 XELL_SIMULATION_START    1 XELL_SIMULATION_END
//   2 XELL_RENDERSUBMIT_START  3 XELL_RENDERSUBMIT_END
//   4 XELL_PRESENT_START       5 XELL_PRESENT_END        6 XELL_INPUT_SAMPLE (optional)
//
// Until now we sent only 4 and 5. XeLL builds ONE frame record out of all six
// (see hfg_frame_report above: sim_start/sim_end, rs_start/rs_end, present_start/
// present_end) and a record with half its stamps missing is not a record - which is
// exactly what [R493] has been reporting in every run since pacing was armed:
// "XeLL reports carry no present timestamps". That line was never evidence that
// XeLL does not consume our markers; it was evidence that we only ever fed it half
// of what it asks for.
//
// The phases are OUR phases, not the game's: sim_* brackets the recording of this
// frame's copy into the ring, submit_* brackets its execution and the bounded fence
// wait, present_* brackets the Present itself. That is honest for a display layer
// that is the presenter, and it is what makes the record complete. The frame id is
// the same one xefgSwapChainSetPresentId is given, because XeLL's markers and that
// id are one timeline (XeFGFrameId.h: "XeLL's markers and SetPresentId carry the
// same frame ID").
void mgpu_xefg_pace_sim_begin(unsigned frame_id)
{
    hfg_pace_marker(frame_id, 0);
}

void mgpu_xefg_pace_sim_end(unsigned frame_id)
{
    hfg_pace_marker(frame_id, 1);
}

void mgpu_xefg_pace_submit_begin(unsigned frame_id)
{
    hfg_pace_marker(frame_id, 2);
}

void mgpu_xefg_pace_submit_end(unsigned frame_id)
{
    hfg_pace_marker(frame_id, 3);
}

// ---------------------------------------------------------------------------
// R497: the live multiplier and the live on/off.
//
// Everything the panel can change between two frames is decided here, in one
// place, by comparing what is asked for against what was last applied. Called
// once per display-loop iteration: two integer compares in the common case.
//
//   backend  -> SetEnabled(1) for XeSS, SetEnabled(0) for every other backend.
//               The chain stays XeFG's either way - with generation off the proxy
//               passes our frames through, which is exactly what 2b-1' measured
//               (28.7 fps, one frame per present), so switching generators no
//               longer has anything to do with who owns the chain.
//   multi    -> SetNumInterpolatedFrames(1..5). This is the call that turns
//               "change the multiplier" from a restart into a click.
//
// A refusal is logged and remembered (so it is not retried every frame), never
// fatal: the picture keeps presenting at whatever the provider last accepted.
void mgpu_xefg_apply_mode()
{
    hfg_state &h = hfg_st();
    if (h.ctx == nullptr || !h.ok) return;
    const int want_on = (mgpu::fgmode::get() == mgpu::fgmode::XEFG) ? 1 : 0;
    int want_ni = (int)mgpu::fgmode::get_multi();

    // R510: the panel's multiplier is an UPPER BOUND; the budget is the ceiling.
    // disp_budget_gen() turns "usable vblanks" into generated frames per source
    // frame and returns 0 when it has no measured source period yet. Clamping here
    // rather than in the panel means every path that asks for a multiplier - the
    // click, the ini at startup, a future dynamic controller - gets the same answer.
    {
        const unsigned budget = disp_budget_gen();
        if (budget != 0u && want_ni > (int)budget)
        {
            static int said_budget = -1;
            if (said_budget != (int)budget)
            {
                said_budget = (int)budget;
                char bl[260];
                snprintf(bl, sizeof bl,
                         "[MGPU][R510] XeSS multiplier clamped %dX -> %dX: source %.2f ms, vblank %.2f ms, "
                         "usable vblanks %.2f (5%% headroom). Asking for more is what makes the delivery rate a "
                         "fraction instead of a multiple.",
                         want_ni + 1, (int)budget + 1, disp_source_period_ms(), disp_vblank_ms(),
                         (disp_vblank_ms() > 0.0) ? (disp_source_period_ms() * 0.95 / disp_vblank_ms()) : 0.0);
                mgpu::diag::warn(bl);
            }
            want_ni = (int)budget;
        }
    }

    if (want_on != h.applied_on)
    {
        if (h.api.set_enabled == nullptr) return;
        int r = -1;
        __try { r = h.api.set_enabled(h.ctx, (uint32_t)want_on); }
        __except (EXCEPTION_EXECUTE_HANDLER) { h.ok = false; return; }
        char l[200];
        snprintf(l, sizeof l, "[MGPU][R497] SetEnabled(%d) res=%d (%s) - %s", want_on, r, xefg_result_name(r),
                 (r == 0) ? (want_on ? "XeSS generation ON" : "XeSS generation OFF, frames pass through")
                          : "refused, keeping the previous state");
        mgpu::diag::info(l);
        if (r != 0) return;
        h.applied_on = want_on;
        h.gen = (want_on != 0);
        // Turning generation back on after a gap must not interpolate across it.
        h.need_reset = 1;
    }

    if (!want_on) return;
    if (want_ni == h.applied_ni) return;
    if (h.api.set_num_frames == nullptr)
    {
        if (!h.warned_no_num_frames)
        {
            h.warned_no_num_frames = true;
            mgpu::diag::warn("[MGPU][R497] xefgSwapChainSetNumInterpolatedFrames is not exported - the multiplier "
                             "still needs a restart");
        }
        return;
    }
    int r = -1;
    __try { r = h.api.set_num_frames(h.ctx, (uint32_t)want_ni); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h.ok = false; return; }
    char l[220];
    snprintf(l, sizeof l, "[MGPU][R497] SetNumInterpolatedFrames(%d) res=%d (%s) -> %dX", want_ni, r,
             xefg_result_name(r), (r == 0) ? want_ni + 1 : 0);
    mgpu::diag::info(l);
    if (r == 0)
    {
        h.applied_ni = want_ni;
        h.need_reset = 1;     // a new timeline: do not blend across the change
    }
}

// Called from the [R448] report. Prints the present timestamps XeLL holds, so
// the burst spacing is read off the GPU timeline instead of inferred from
// counters that cannot tell 2 frames apart from 4.
void mgpu_xefg_pace_report()
{
    hfg_state &h = hfg_st();
    if (h.pace == 0 || !h.gen || h.xell == nullptr || h.xa.frames_reports == nullptr) return;
    static unsigned last_id = 0;
    static int said = 0;
    hfg_frame_report rep[64] = {};
    int r = -1;
    __try { r = h.xa.frames_reports(h.xell, rep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h.pace = 0; return; }
    if (r != 0)
    {
        if (said++ < 3)
        {
            char l[180];
            snprintf(l, sizeof l, "[MGPU][R493] xellGetFramesReports res=%d - XeLL has no reports to give", r);
            mgpu::diag::warn(l);
        }
        return;
    }
    int newest = -1;
    for (int i = 0; i < 64; ++i)
        if (rep[i].present_start != 0 && (newest < 0 || rep[i].frame_id > rep[newest].frame_id)) newest = i;
    if (newest < 0)
    {
        if (said++ < 3)
            mgpu::diag::warn("[MGPU][R493] XeLL reports exist but carry no present timestamps - XeFG is not feeding "
                             "the pacer, so it cannot space what it cannot see");
        return;
    }
    if (rep[newest].frame_id == last_id) return;
    last_id = rep[newest].frame_id;

    // The newest nine present timestamps, ordered by frame id. The gap between
    // consecutive ones IS the burst spacing: at 4X on a 160 Hz panel the even
    // pattern is 6.25 / 6.25 / 6.25, and a clump reads as 0 / 0 / 18.75.
    struct hfg_ts { unsigned id; unsigned long long ts; } best[9];
    int np = 0;
    for (int i = 0; i < 64; ++i)
    {
        if (rep[i].present_start == 0) continue;
        const unsigned id = rep[i].frame_id;
        const unsigned long long ts = rep[i].present_start;
        if (np < 9) { best[np].id = id; best[np].ts = ts; ++np; continue; }
        int mn = 0;
        for (int j = 1; j < np; ++j) if (best[j].id < best[mn].id) mn = j;
        if (id == 0 || id <= best[mn].id) continue;
        best[mn].id = id;
        best[mn].ts = ts;
    }
    for (int i = 1; i < np; ++i)
    {
        hfg_ts key = best[i];
        int j = i - 1;
        while (j >= 0 && best[j].id > key.id) { best[j + 1] = best[j]; --j; }
        best[j + 1] = key;
    }

    char l[460];
    int o = snprintf(l, sizeof l, "[MGPU][R493] XeLL present timestamps (%d entries, newest id=%u):", np,
                     rep[newest].frame_id);
    for (int j = 0; j < np && o > 0 && o < (int)sizeof l - 40; ++j)
    {
        if (j == 0)
            o += snprintf(l + o, sizeof l - (size_t)o, " %u:-", best[j].id);
        else
            o += snprintf(l + o, sizeof l - (size_t)o, " %u:%+.2f", best[j].id,
                          (double)(best[j].ts - best[j - 1].ts) / 1e6);
    }
    mgpu::diag::info(l);
}

// R489. Called by disp_init() BEFORE the RTVs and back buffers are taken, so
// that every one of them is created from the chain we will actually present
// to. On false the caller keeps its own chain and behaves exactly as before.
bool mgpu_xefg_handover_wanted()
{
    // R508: not the backend any more - see hfg_chain_wanted() (xefg_display.inc).
    return hfg_chain_wanted();
}

bool mgpu_xefg_display_handover(ID3D12Device *dev, ID3D12CommandQueue *q, HWND hwnd,
                                const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIFactory2 *fac,
                                IDXGISwapChain1 **out_sc, IDXGISwapChain3 **out_sc3, const char *where)
{
    static bool tried = false;
    if (tried) return false;
    if (out_sc == nullptr || out_sc3 == nullptr) return false;
    *out_sc = nullptr;
    *out_sc3 = nullptr;
    if (!hfg_chain_wanted()) return false;
    tried = true;
    if (dev == nullptr || q == nullptr || hwnd == nullptr || desc == nullptr || fac == nullptr)
    {
        mgpu::diag::error("[MGPU][R489] handover NOT attempted - device, queue, window, description or factory is null");
        return false;
    }

    // The context must outlive every present on this chain, and the display
    // layer lives for the process. Deliberately never destroyed.
    hfg_state &h = hfg_st();
    char l[256];
    snprintf(l, sizeof l, "[MGPU][R489] handover BEGIN where=%s - XeFG will create the display window's ONLY "
             "swap chain; if this is the last R489 line, it died inside the guarded sequence",
             (where != nullptr) ? where : "?");
    mgpu::diag::info(l);

    __try
    {
        return hfg_body(h, dev, q, hwnd, desc, fac, out_sc, out_sc3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        snprintf(l, sizeof l, "[MGPU][R489] SEH FAULT 0x%08X inside the display handover - caught, the chain may be "
                 "consumed. ctx=%p", (unsigned)GetExceptionCode(), h.ctx);
        mgpu::diag::error(l);
        return false;
    }
}
