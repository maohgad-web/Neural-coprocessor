// MGPU Bridge - R379 (0.5.1 discovery): the game's own camera for frame generation. See sl_camera.hpp.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

#include "sl_camera.hpp"
#include "sl_tags.hpp"      // swap_import_external: the same import-slot swap the tag tap uses
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu
{
namespace slcam
{
namespace
{
// ---- sl::Constants, from the public headers (sl_struct.h, sl_consts.h), x64 ----
// BaseStructure: next (8) | structType GUID (16) | structVersion size_t (8) = 32 bytes, then:
const size_t OFF_GUID = 8;
const size_t OFF_VIEW_TO_CLIP = 32;    // float4x4 cameraViewToClip
const size_t OFF_CLIP_TO_VIEW = 96;    // float4x4 clipToCameraView
const size_t OFF_CLIP_TO_PREV = 224;   // float4x4 clipToPrevClip   (clipToLensClip sits at 160)
const size_t OFF_PREV_TO_CLIP = 288;   // float4x4 prevClipToClip
const size_t OFF_POS = 376;            // float3 cameraPos (jitterOffset 352, mvecScale 360, pinhole 368)
const size_t OFF_UP = 388, OFF_RIGHT = 400, OFF_FWD = 412;
const size_t OFF_NEAR = 424, OFF_FAR = 428, OFF_FOV = 432, OFF_ASPECT = 436;
const size_t BLOCK = 440;              // bytes copied: everything up to and including cameraAspectRatio
// StructType of sl::Constants: { 0xdcd35ad7, 0x4e4a, 0x4bad, { 0xa9, 0xc, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe } }
const unsigned char GUID_CONSTANTS[16] = { 0xd7, 0x5a, 0xd3, 0xdc, 0x4a, 0x4e, 0xad, 0x4b,
                                           0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe };
const float SL_INVALID = 3.40282346638528859811704183484516925440e38f;

typedef int (*pf_set_constants)(const void *values, const void *frame, const void *viewport);

std::atomic<bool> g_want{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_done{false};
std::atomic<unsigned> g_ticks{0};
pf_set_constants g_real = nullptr;

struct rec
{
    unsigned long long n;      // call number, 0 = empty
    bool ok;                   // the four matrices are finite
    float c2p[16], p2c[16], v2c[16], c2v[16];
    float pos[3], up[3], right[3], fwd[3];
    float cam_near, cam_far, fov, aspect;
};
const unsigned CALLS = 32u;    // constants calls kept
const unsigned SEALS = 64u;    // seal stamps kept
std::mutex g_mx;               // game threads only ever TRY it
rec g_rec[CALLS];
unsigned long long g_n = 0ull;                 // newest call number (under g_mx)
struct stamp { unsigned long long fi, n; };   // n: the GAME FRAME (g_t) the seal belongs to (R383; was the call number)
stamp g_stamp[SEALS];
// R383: the frame boundary is the PRESENT. A title may call slSetConstants more than once per frame (per viewport,
// per feature); the camera step of a frame is ONE record - the last call before that frame's present - and the
// chain across a pair runs over game frames, not over calls.
const unsigned FRAMES = 64u;
struct fclose_t { unsigned long long t, n; };  // game frame number, newest call number when it closed (0 = not known)
fclose_t g_frame[FRAMES];
unsigned long long g_t = 0ull;                 // game frames closed (under g_mx)
std::atomic<unsigned> g_close_missed{0};       // presents whose close lost the try-lock: closed later as "not known"
std::atomic<unsigned long long> g_frames_closed{0}, g_close_busy{0};

std::atomic<unsigned long long> g_calls{0}, g_stored{0}, g_busy{0}, g_bad_guid{0}, g_fault{0}, g_not_finite{0};
std::atomic<unsigned long long> g_seals{0}, g_seal_busy{0};
// bridge thread only
unsigned long long g_pairs = 0ull, g_pairs_ok = 0ull, g_no_stamp = 0ull, g_gap = 0ull, g_too_long = 0ull, g_bad_rec = 0ull;
unsigned long long g_no_call = 0ull;           // R383: a game frame inside the pair had no new constants call
// R393: the newest camera handed to the generator, kept for the totals line (bridge thread only)
float g_smp_row3[4] = {}, g_smp_diag[2] = {}, g_smp_pos[3] = {}, g_smp_lens[4] = {};
unsigned g_smp_frames = 0u; bool g_smp_have = false, g_smp_lens_ok = false;
std::atomic<bool> g_said_first{false}, g_said_guid{false};

bool key_is_one()
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm)) return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", mod);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "FGCamera");
    return v != nullptr && atoi(v) == 1;
}

// The only dereference of the game's pointer. Alone in a function with no C++ object, as sl_tags does it.
bool copy_block(const void *src, unsigned char *dst)
{
    __try
    {
        memcpy(dst, src, BLOCK);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool finite16(const float *m)
{
    for (int i = 0; i < 16; ++i)
        if (!(m[i] == m[i]) || m[i] >= SL_INVALID || m[i] <= -SL_INVALID) return false;
    return true;
}

void take(const void *values)
{
    g_calls.fetch_add(1ull, std::memory_order_relaxed);
    if (values == nullptr) return;
    unsigned char b[BLOCK];
    if (!copy_block(values, b)) { g_fault.fetch_add(1ull, std::memory_order_relaxed); return; }
    if (memcmp(b + OFF_GUID, GUID_CONSTANTS, 16) != 0)
    {
        g_bad_guid.fetch_add(1ull, std::memory_order_relaxed);
        if (!g_said_guid.exchange(true))
            mgpu::diag::warn("[MGPU][R379] slSetConstants was called with a struct that does not carry the documented "
                             "sl::Constants structType GUID. Nothing was read; the call went through untouched. The layout "
                             "this build quotes does not match the Streamline in this process. Said once.");
        return;
    }
    rec r{};
    memcpy(r.v2c, b + OFF_VIEW_TO_CLIP, 64); memcpy(r.c2v, b + OFF_CLIP_TO_VIEW, 64);
    memcpy(r.c2p, b + OFF_CLIP_TO_PREV, 64); memcpy(r.p2c, b + OFF_PREV_TO_CLIP, 64);
    memcpy(r.pos, b + OFF_POS, 12); memcpy(r.up, b + OFF_UP, 12); memcpy(r.right, b + OFF_RIGHT, 12); memcpy(r.fwd, b + OFF_FWD, 12);
    memcpy(&r.cam_near, b + OFF_NEAR, 4); memcpy(&r.cam_far, b + OFF_FAR, 4);
    memcpy(&r.fov, b + OFF_FOV, 4); memcpy(&r.aspect, b + OFF_ASPECT, 4);
    r.ok = finite16(r.c2p) && finite16(r.p2c) && finite16(r.v2c) && finite16(r.c2v);
    if (!r.ok) g_not_finite.fetch_add(1ull, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock()) { g_busy.fetch_add(1ull, std::memory_order_relaxed); return; }
    r.n = ++g_n;
    g_rec[(unsigned)(r.n % CALLS)] = r;
    lk.unlock();
    g_stored.fetch_add(1ull, std::memory_order_relaxed);
    if (!g_said_first.exchange(true))
    {
        char l[520];
        snprintf(l, sizeof l, "[MGPU][R379] FIRST sl::Constants read: matrices %s | clipToPrevClip row 3 = %.5f %.5f %.5f %.5f | "
                 "near %.4g far %.4g fov %.4f rad aspect %.4f | camera position %.2f %.2f %.2f. This is the game's own "
                 "statement, per frame.",
                 r.ok ? "finite" : "NOT finite (not used)", (double)r.c2p[12], (double)r.c2p[13], (double)r.c2p[14], (double)r.c2p[15],
                 (double)r.cam_near, (double)r.cam_far, (double)r.fov, (double)r.aspect,
                 (double)r.pos[0], (double)r.pos[1], (double)r.pos[2]);
        mgpu::diag::info(l);
    }
}

int hook_set_constants(const void *values, const void *frame, const void *viewport)
{
    take(values);
    return (g_real != nullptr) ? g_real(values, frame, viewport) : 0;
}

void mul(const float *a, const float *b, float *o)   // o = a * b, row-major
{
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                           a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    memcpy(o, t, sizeof t);
}
}   // namespace

void init()
{
    if (!key_is_one()) return;
    g_want.store(true, std::memory_order_release);
    mgpu::diag::info("[MGPU][R379] FGCamera=1: the game's own camera block for frame generation, read from Streamline's "
                     "slSetConstants (a read-only import-slot tap, installed once the title has been presenting for a "
                     "while, like the tag tap). Without Streamline in the process nothing is installed.");
}

bool on() { return g_want.load(std::memory_order_relaxed); }

// R383: once per present, on the game's present thread, BEFORE the seal of the same present. Never waits: a close
// that loses the try-lock is made up at the next one as a frame whose camera is not known (a pair across it gets
// no camera, never a wrong one).
static void close_frame()
{
    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock())
    {
        g_close_missed.fetch_add(1u, std::memory_order_relaxed);
        g_close_busy.fetch_add(1ull, std::memory_order_relaxed);
        return;
    }
    unsigned missed = g_close_missed.exchange(0u, std::memory_order_relaxed);
    if (missed > FRAMES) missed = FRAMES;
    for (unsigned i = 0; i < missed; ++i) { ++g_t; g_frame[(unsigned)(g_t % FRAMES)] = fclose_t{ g_t, 0ull }; }
    ++g_t;
    g_frame[(unsigned)(g_t % FRAMES)] = fclose_t{ g_t, g_n };
    lk.unlock();
    g_frames_closed.fetch_add(1ull, std::memory_order_relaxed);
}

void tick()
{
    if (!g_want.load(std::memory_order_relaxed)) return;
    if (g_installed.load(std::memory_order_relaxed)) { close_frame(); return; }
    if (g_done.load(std::memory_order_relaxed)) return;
    if (g_ticks.fetch_add(1u, std::memory_order_relaxed) < 900u) return;   // the tag tap's threshold: nothing of this exists during startup
    if (g_done.exchange(true, std::memory_order_acq_rel)) return;
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (sl == nullptr)
    {
        mgpu::diag::info("[MGPU][R379] FGCamera=1: sl.interposer.dll is not loaded - this title does not use Streamline. "
                         "Nothing was touched; frame generation gets the camera it gets today.");
        return;
    }
    g_real = (pf_set_constants)GetProcAddress(sl, "slSetConstants");
    if (g_real == nullptr)
    {
        mgpu::diag::warn("[MGPU][R379] FGCamera=1: sl.interposer.dll exports no slSetConstants. Nothing was touched.");
        return;
    }
    mgpu::diag::info("[MGPU][R379] TAP BEGIN: swapping the game's import slots for slSetConstants. One pointer store per "
                     "slot, in the game's modules only. IF THIS IS THE LAST R379 LINE, IT DIED HERE.");
    const unsigned hits = mgpu::sltags::swap_import_external((void *)g_real, (void *)&hook_set_constants);
    g_installed.store(hits != 0u, std::memory_order_release);
    char l[420];
    snprintf(l, sizeof l, "[MGPU][R379] TAP INSTALLED: %u slot(s) for slSetConstants. ZERO IS A RESULT: this title resolves "
             "Streamline dynamically, so no import slot leads to it; the camera stays what it is today and the "
             "summary will show 0 calls.", hits);
    if (hits != 0u) mgpu::diag::info(l); else mgpu::diag::warn(l);
}

void on_seal(unsigned long long fi)
{
    if (!g_installed.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock()) { g_seal_busy.fetch_add(1ull, std::memory_order_relaxed); return; }
    // R383: the game frame just closed by tick(). If that close lost its try-lock the frame is not closed yet:
    // no stamp (0), so a pair ending here gets no camera instead of one that is a frame short.
    g_stamp[(unsigned)(fi % SEALS)] = stamp{ fi, (g_close_missed.load(std::memory_order_relaxed) != 0u) ? 0ull : g_t };
    lk.unlock();
    g_seals.fetch_add(1ull, std::memory_order_relaxed);
}

bool pair(unsigned long long f_prev, unsigned long long f_cur, camera &out)
{
    if (!g_installed.load(std::memory_order_relaxed)) return false;
    ++g_pairs;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const stamp a = g_stamp[(unsigned)(f_prev % SEALS)], b = g_stamp[(unsigned)(f_cur % SEALS)];
        if (f_prev == 0ull || a.fi != f_prev || b.fi != f_cur || a.n == 0ull || b.n == 0ull) ++g_no_stamp;
        else if (b.n <= a.n) ++g_gap;                       // the two seals are in the same game frame
        else if (b.n - a.n > 8ull) ++g_too_long;
        else
        {
            // R383: one record per GAME FRAME T in (a.n, b.n] - the last constants call before that frame's present.
            // A frame counts only if its close is known and it saw a call the frame before it had not seen.
            auto frame_rec = [&](unsigned long long T, const rec *&out_r) -> int   // 1 ok, 0 no new call / not known, -1 record gone
            {
                const fclose_t fc = g_frame[(unsigned)(T % FRAMES)], fp = g_frame[(unsigned)((T - 1ull) % FRAMES)];
                if (fc.t != T || fp.t != T - 1ull || fc.n == 0ull || fp.n == 0ull || fc.n <= fp.n) return 0;
                const rec &r = g_rec[(unsigned)(fc.n % CALLS)];
                if (r.n != fc.n || !r.ok) return -1;
                out_r = &r;
                return 1;
            };
            const rec *curp = nullptr;
            int st = frame_rec(b.n, curp);
            bool good = (st == 1);
            bool no_call = (st == 0);
            float c2p[16], p2c[16];
            if (good) { memcpy(c2p, curp->c2p, 64); memcpy(p2c, curp->p2c, 64); }
            // current clip -> clip of frame T-1 -> ... : v_prev = v_cur * C[T] * C[T-1] * ...
            for (unsigned long long T = b.n - 1ull; good && T > a.n; --T)
            {
                const rec *rp = nullptr;
                st = frame_rec(T, rp);
                if (st != 1) { good = false; no_call = (st == 0); break; }
                mul(c2p, rp->c2p, c2p);          // one more step back
                mul(rp->p2c, p2c, p2c);          // and the way forward again, in the opposite order
            }
            if (!good && no_call) ++g_no_call;
            else if (!good) ++g_bad_rec;
            else
            {
                memcpy(out.clip_to_prev, c2p, 64); memcpy(out.prev_to_clip, p2c, 64);
                memcpy(out.view_to_clip, curp->v2c, 64); memcpy(out.clip_to_view, curp->c2v, 64);
                memcpy(out.pos, curp->pos, 12); memcpy(out.up, curp->up, 12); memcpy(out.right, curp->right, 12); memcpy(out.fwd, curp->fwd, 12);
                out.cam_near = curp->cam_near; out.cam_far = curp->cam_far; out.fov = curp->fov; out.aspect = curp->aspect;
                out.have_lens = curp->cam_near < SL_INVALID && curp->cam_far < SL_INVALID && curp->fov < SL_INVALID && curp->aspect < SL_INVALID &&
                                curp->cam_near == curp->cam_near && curp->cam_far == curp->cam_far && curp->fov == curp->fov && curp->aspect == curp->aspect;
                out.frames = (unsigned)(b.n - a.n);
                ok = true;
                ++g_pairs_ok;
                // R393: what was handed, for the totals line
                memcpy(g_smp_row3, c2p + 12, 16); g_smp_diag[0] = c2p[0]; g_smp_diag[1] = c2p[5];
                memcpy(g_smp_pos, curp->pos, 12);
                g_smp_lens[0] = curp->cam_near; g_smp_lens[1] = curp->cam_far; g_smp_lens[2] = curp->fov; g_smp_lens[3] = curp->aspect;
                g_smp_frames = out.frames; g_smp_have = true; g_smp_lens_ok = out.have_lens;
            }
        }
    }
    if (g_pairs % 1200ull == 0ull)
    {
        char l[900];
        const unsigned long long fcl = g_frames_closed.load(std::memory_order_relaxed);
        snprintf(l, sizeof l, "[MGPU][R379] FGCamera totals: slSetConstants calls %llu, stored %llu (lock busy %llu, struct not "
                 "recognised %llu, read fault %llu, matrices not finite %llu) | game frames closed %llu (busy %llu) = %.2f "
                 "call(s) per frame; the LAST call of a frame is that frame's camera (R383) | seals stamped %llu (busy %llu) | "
                 "pairs asked %llu: camera handed to the generator %llu, no stamp for a frame %llu, both seals in one frame "
                 "%llu, span over 8 frames %llu, a frame without a new constants call %llu, a record missing or not finite "
                 "%llu. A pair without the camera gets what it gets today.",
                 g_calls.load(std::memory_order_relaxed), g_stored.load(std::memory_order_relaxed), g_busy.load(std::memory_order_relaxed),
                 g_bad_guid.load(std::memory_order_relaxed), g_fault.load(std::memory_order_relaxed), g_not_finite.load(std::memory_order_relaxed),
                 fcl, g_close_busy.load(std::memory_order_relaxed),
                 fcl ? (double)g_calls.load(std::memory_order_relaxed) / (double)fcl : 0.0,
                 g_seals.load(std::memory_order_relaxed), g_seal_busy.load(std::memory_order_relaxed),
                 g_pairs, g_pairs_ok, g_no_stamp, g_gap, g_too_long, g_no_call, g_bad_rec);
        mgpu::diag::info(l);
        if (g_smp_have)
        {   // R393: the first-read line is taken wherever the tap comes up (a menu); this one is from the pairs
            char sl[520];
            snprintf(sl, sizeof sl, "[MGPU][R379] SAMPLE of the newest camera handed to the generator (across %u game frame(s)): "
                     "clipToPrevClip [0][0] %.6f [1][1] %.6f, row 3 = %.6f %.6f %.6f %.6f (identity = 1, 1, 0 0 0 1: the game "
                     "says the camera did not move) | position %.3f %.3f %.3f | near %.4g far %.4g fov %.4f aspect %.4f (%s).",
                     g_smp_frames, (double)g_smp_diag[0], (double)g_smp_diag[1], (double)g_smp_row3[0], (double)g_smp_row3[1],
                     (double)g_smp_row3[2], (double)g_smp_row3[3], (double)g_smp_pos[0], (double)g_smp_pos[1], (double)g_smp_pos[2],
                     (double)g_smp_lens[0], (double)g_smp_lens[1], (double)g_smp_lens[2], (double)g_smp_lens[3],
                     g_smp_lens_ok ? "stated by the game, handed over" : "not stated: the keys' values are used");
            mgpu::diag::info(sl);
        }
    }
    return ok;
}

void uninstall()
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    (void)mgpu::sltags::swap_import_external((void *)&hook_set_constants, (void *)g_real);
    mgpu::diag::info("[MGPU][R379] TAP REMOVED. Import slots restored.");
}
}   // namespace slcam
}   // namespace mgpu
