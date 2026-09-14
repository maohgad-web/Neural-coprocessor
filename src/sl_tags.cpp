// MGPU Bridge - SLT1: the Streamline tag tap. See sl_tags.hpp.
//
// Self-contained like probe.cpp, calibrator.cpp, screen.cpp and sl_probe.cpp:
// no ReShade type, no gpu1_context type, no Streamline header and no link
// library. Every Streamline structure is read by byte offset, and every one
// of those offsets is quoted from the public header beside it.
#include "sl_tags.hpp"
#include "diag.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>   // _wcsnicmp, for skipping sl.* modules by name
#include <atomic>
#include <mutex>

namespace mgpu::sltags
{
namespace
{
    // ---- THE ABI, QUOTED ----
    //
    // sl_struct.h:
    //   struct BaseStructure {
    //       BaseStructure* next{};          // +0   8 bytes
    //       StructType     structType{};    // +8  16 bytes (u32,u16,u16,u8[8])
    //       size_t         structVersion;   // +24  8 bytes
    //   };                                  // = 32 bytes, no virtuals
    //
    //   SL_STRUCT_BEGIN inserts only a constructor and a CONSTEXPR STATIC
    //   s_structType, so a tagged struct is BaseStructure followed by its own
    //   members and nothing else. That is why these offsets are just sums.
    const unsigned BASE_SIZE   = 32u;
    const unsigned OFF_TYPE    = 8u;    // StructType, within BaseStructure

    // sl_core_types.h:
    //   SL_STRUCT_BEGIN(ResourceTag, {4c6a5aad-b445-496c-87ff-1af3845be653}, 1)
    //       Resource*         resource{};   // +32
    //       BufferType        type{};       // +40  (using BufferType = uint32_t)
    //       ResourceLifecycle lifecycle{};  // +44  (plain enum -> int)
    //       Extent            extent{};     // +48  SIZE NOT PINNED - see below
    const unsigned TAG_RESOURCE  = 32u;
    const unsigned TAG_TYPE      = 40u;
    const unsigned TAG_LIFECYCLE = 44u;
    const unsigned TAG_MIN_SIZE  = 48u;   // everything we read lies below this

    // sl_core_types.h:
    //   SL_STRUCT_BEGIN(Resource, {3a9d70cf-2418-4b72-8391-13f8721c7261}, 1)
    //       ResourceType type;            // +32  enum class : char  -> 1 byte
    //       void*        native{};        // +40  (7 bytes of padding above)
    //       void*        memory{};        // +48
    //       void*        view{};          // +56
    //       uint32_t     state;           // +64
    //       uint32_t     width{};         // +68
    //       uint32_t     height{};        // +72
    //       uint32_t     nativeFormat{};  // +76
    //       uint32_t     mipLevels{};     // +80
    //       uint32_t     arrayLayers{};   // +84
    //       uint64_t     gpuVirtualAddress{}; // +88
    //       uint32_t     flags;           // +96
    //       uint32_t     usage{};         // +100
    //       uint32_t     reserved{};      // +104
    const unsigned RES_NATIVE = 40u;
    const unsigned RES_WIDTH  = 68u;
    const unsigned RES_HEIGHT = 72u;
    const unsigned RES_FORMAT = 76u;
    const unsigned RES_MIN_SIZE = 80u;

    // The two GUIDs, as 16 raw bytes each, in the layout StructType stores
    // them: uint32 then uint16 then uint16 then 8 bytes, all little-endian on
    // x64. Comparing 16 bytes is how an element is validated before it is
    // read, and how the array stride is measured instead of assumed.
    const unsigned char GUID_RESOURCE_TAG[16] = {
        0xAD,0x5A,0x6A,0x4C, 0x45,0xB4, 0x6C,0x49,
        0x87,0xFF,0x1A,0xF3,0x84,0x5B,0xE6,0x53 };

    // sl_core_types.h constants. Only the ones this instrument names.
    const unsigned BT_DEPTH          = 0u;
    const unsigned BT_MVEC           = 1u;
    const unsigned BT_HUDLESS        = 2u;
    const unsigned BT_SCALING_IN     = 3u;
    const unsigned BT_SCALING_OUT    = 4u;
    const unsigned BT_UI_COLOR_ALPHA = 23u;

    const char *buffer_name(unsigned t)
    {
        switch (t)
        {
        case BT_DEPTH:          return "Depth";
        case BT_MVEC:           return "MotionVectors";
        case BT_HUDLESS:        return "HUDLessColor";
        case BT_SCALING_IN:     return "ScalingInputColor";
        case BT_SCALING_OUT:    return "ScalingOutputColor";
        case BT_UI_COLOR_ALPHA: return "UIColorAndAlpha";
        default:                return "other";
        }
    }

    // sl_core_api.h, verbatim:
    //   SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame,
    //                                      const sl::ViewportHandle& viewport,
    //                                      const sl::ResourceTag* resources,
    //                                      uint32_t numResources,
    //                                      sl::CommandBuffer* cmdBuffer);
    //   SL_API sl::Result slSetTag(const sl::ViewportHandle& viewport,
    //                              const sl::ResourceTag* tags,
    //                              uint32_t numTags,
    //                              sl::CommandBuffer* cmdBuffer);
    //
    // SL_API is `extern "C"`, sl::Result is an enum, and a C++ reference is a
    // pointer at the ABI. x64 has one calling convention, so these two
    // typedefs are the call exactly. Nothing is inferred.
    typedef int (*pf_set_tag_for_frame)(const void *, const void *,
                                        const void *, unsigned, void *);
    typedef int (*pf_set_tag)(const void *, const void *, unsigned, void *);

    // ---- STATE ----
    std::mutex g_cs;
    std::atomic<int>  g_mode{0};
    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_done{false};      // install attempted, win or lose

    pf_set_tag_for_frame g_real_stff = nullptr;
    pf_set_tag           g_real_st   = nullptr;
    HMODULE g_self = nullptr;

    std::atomic<unsigned long long> g_calls{0};
    std::atomic<unsigned long long> g_tags_read{0};
    std::atomic<unsigned long long> g_slots{0};
    std::atomic<unsigned long long> g_stride{0};   // measured, 0 until it is
    std::atomic<unsigned long long> g_skipped{0};  // elements not validated

    std::atomic<unsigned long long> g_h_mvec{0};
    std::atomic<unsigned long long> g_h_hudless{0};
    std::atomic<unsigned long long> g_h_scale_in{0};

    // One "said" bit per buffer type we name, plus a catch-all.
    bool g_said[24] = {};
    unsigned long long g_last[24] = {};

    // ---- READING, UNDER A HANDLER, ALWAYS ----
    //
    // Every dereference below is of memory another module owns. The layout is
    // quoted rather than guessed, but a quoted layout against a future SDK is
    // still a bet, and a bet taken on the game's own render thread must not be
    // able to take the game down. The handler is what makes the bet bounded:
    // the worst outcome is that the tap reads nothing and says so.
    bool bytes_equal(const void *p, const unsigned char *want, unsigned n)
    {
        __try { return memcmp(p, want, n) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool is_tag(const void *p)
    {
        if (p == nullptr) return false;
        return bytes_equal((const unsigned char *)p + OFF_TYPE,
                           GUID_RESOURCE_TAG, 16u);
    }

    // Measure sizeof(ResourceTag) from the data, once. The array is
    // contiguous by definition - it is a C array passed by pointer with a
    // count - so the distance to the next element's structType GUID IS the
    // stride. Bounded to a window that cannot be a struct, and 8-aligned
    // because BaseStructure begins with a pointer.
    unsigned measure_stride(const void *first)
    {
        const unsigned char *p = (const unsigned char *)first;
        __try
        {
            for (unsigned off = TAG_MIN_SIZE; off <= 512u; off += 8u)
                if (memcmp(p + off + OFF_TYPE, GUID_RESOURCE_TAG, 16u) == 0)
                    return off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
        return 0u;
    }

    // POD only, and no object with a destructor anywhere near the handler:
    // a function that mixes SEH with C++ unwinding does not compile (C2712),
    // and the lock below is exactly such an object. So the dereferences live
    // alone in here and the bookkeeping lives in the caller.
    struct tag_fields
    {
        unsigned type;
        int      life;
        unsigned long long native;
        unsigned w, h, fmt;
    };

    bool read_fields(const void *tag, tag_fields &out)
    {
        out.type = 0xFFFFFFFFu; out.life = -1; out.native = 0;
        out.w = 0; out.h = 0; out.fmt = 0;
        __try
        {
            const unsigned char *t = (const unsigned char *)tag;
            out.type = *(const unsigned *)(t + TAG_TYPE);
            out.life = *(const int *)(t + TAG_LIFECYCLE);
            const void *res = *(const void *const *)(t + TAG_RESOURCE);
            if (res != nullptr)
            {
                const unsigned char *r = (const unsigned char *)res;
                out.native = (unsigned long long)(uintptr_t)
                             *(const void *const *)(r + RES_NATIVE);
                out.w   = *(const unsigned *)(r + RES_WIDTH);
                out.h   = *(const unsigned *)(r + RES_HEIGHT);
                out.fmt = *(const unsigned *)(r + RES_FORMAT);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void read_one(const void *tag)
    {
        tag_fields f{};
        if (!read_fields(tag, f))
        {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const unsigned type = f.type;
        const int life = f.life;
        const unsigned long long native = f.native;
        const unsigned w = f.w, h = f.h, fmt = f.fmt;

        g_tags_read.fetch_add(1, std::memory_order_relaxed);

        switch (type)
        {
        case BT_MVEC:       g_h_mvec.store(native, std::memory_order_relaxed); break;
        case BT_HUDLESS:    g_h_hudless.store(native, std::memory_order_relaxed); break;
        case BT_SCALING_IN: g_h_scale_in.store(native, std::memory_order_relaxed); break;
        default: break;
        }

        const unsigned slot = (type < 24u) ? type : 23u;
        bool say = false;
        {
            std::lock_guard<std::mutex> lk(g_cs);
            if (!g_said[slot]) { g_said[slot] = true; say = true; }
            else if (g_mode.load(std::memory_order_relaxed) >= 2 &&
                     g_last[slot] != native) say = true;
            g_last[slot] = native;
        }
        if (!say) return;

        char line[900];
        snprintf(line, sizeof line,
            "[MGPU][SLT1] TAG %s (BufferType=%u) native=0x%llx %ux%u nativeFormat=%u "
            "lifecycle=%d. THIS IS THE GAME'S OWN DECLARATION, not a ranking. For "
            "MotionVectors it is the address every round from R70 to R103 tried to "
            "infer, stated by the title itself with its extent and format attached; "
            "compare it against the transport source on the R71 line, and where they "
            "differ THIS one is right. For HUDLessColor and ScalingInputColor it is a "
            "colour buffer NAMED rather than deduced from a format, which is what an "
            "HDR path needs before it can be built. lifecycle 0 is valid only now, 1 "
            "until present, 2 until evaluate - anything above 0 is a resource that "
            "can be read outside the tagging call.",
            buffer_name(type), type, native, w, h, fmt, life);
        mgpu::diag::info(line);
    }

    void read_array(const void *tags, unsigned n)
    {
        if (tags == nullptr || n == 0u) return;
        g_calls.fetch_add(1, std::memory_order_relaxed);

        if (!is_tag(tags))
        {
            // The first element does not carry the ResourceTag GUID. Either
            // the SDK's layout moved or this is not what we think it is.
            // Either way: read nothing, say it once, and leave the call alone.
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                mgpu::diag::warn(
                    "[MGPU][SLT1] TAG ARRAY NOT RECOGNISED: the first element does not carry "
                    "the documented ResourceTag structType GUID, so the layout this build "
                    "quotes does not match the Streamline in this process. NOTHING WAS READ "
                    "and the call was passed through untouched. This is the guard working, "
                    "not a fault - and it is the finding: the ABI moved, and the offsets in "
                    "sl_tags.cpp need re-reading against the SDK this title ships.");
            return;
        }

        read_one(tags);
        if (n == 1u) return;

        unsigned stride = (unsigned)g_stride.load(std::memory_order_relaxed);
        if (stride == 0u)
        {
            stride = measure_stride(tags);
            if (stride == 0u)
            {
                // One tag read, the rest skipped. Information lost, nothing
                // risked - which is the right way round.
                g_skipped.fetch_add(n - 1u, std::memory_order_relaxed);
                return;
            }
            g_stride.store(stride, std::memory_order_relaxed);
            char sl[420];
            snprintf(sl, sizeof sl,
                "[MGPU][SLT1] TAG STRIDE MEASURED: sizeof(sl::ResourceTag) = %u bytes, found "
                "by locating the next element's structType GUID rather than by assuming "
                "sizeof(sl::Extent). Every element is still validated against that GUID "
                "before it is read, so a wrong stride skips tags instead of reading memory "
                "that is not a tag.", stride);
            mgpu::diag::info(sl);
        }

        const unsigned char *p = (const unsigned char *)tags;
        for (unsigned i = 1u; i < n && i < 64u; ++i)
        {
            const void *e = p + (size_t)i * stride;
            if (!is_tag(e)) { g_skipped.fetch_add(1, std::memory_order_relaxed); break; }
            read_one(e);
        }
    }

    // ---- THE THUNKS ----
    //
    // Read, then call the real function with every argument exactly as it
    // arrived. No argument is copied, rewritten or held.
    int hook_stff(const void *frame, const void *viewport,
                  const void *tags, unsigned n, void *cmd)
    {
        read_array(tags, n);
        return (g_real_stff != nullptr)
                   ? g_real_stff(frame, viewport, tags, n, cmd) : 0;
    }

    int hook_st(const void *viewport, const void *tags, unsigned n, void *cmd)
    {
        read_array(tags, n);
        return (g_real_st != nullptr) ? g_real_st(viewport, tags, n, cmd) : 0;
    }

    // ---- THE IMPORT SWAP ----
    //
    // The same mechanism as the calibrator's rung A, and deliberately NOT its
    // rung B: no data section is written, so no module's memory is modified
    // while that module may be running. If the game resolved these entry
    // points dynamically rather than importing them, this finds nothing, and
    // finding nothing is reported as the measurement it is.
    unsigned patch_module(HMODULE mod, void *find, void *repl)
    {
        if (mod == nullptr || mod == g_self) return 0;
        unsigned char *base = (unsigned char *)mod;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        const IMAGE_DATA_DIRECTORY dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;

        volatile unsigned hits = 0;
        IMAGE_IMPORT_DESCRIPTOR *imp =
            (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
        __try
        {
            for (; imp->Name != 0; ++imp)
            {
                if (imp->FirstThunk == 0) continue;
                IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
                for (; t->u1.Function != 0; ++t)
                {
                    if ((void *)(uintptr_t)t->u1.Function != find) continue;
                    DWORD old = 0;
                    if (!VirtualProtect(&t->u1.Function, sizeof(void *),
                                        PAGE_READWRITE, &old))
                        continue;
                    t->u1.Function = (ULONGLONG)(uintptr_t)repl;
                    VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                    ++hits;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return (unsigned)hits; }
        return (unsigned)hits;
    }

    unsigned swap_everywhere(void *find, void *repl)
    {
        if (find == nullptr) return 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        unsigned hits = 0;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                // Skip Streamline's own modules. Their internal calls are not
                // the game declaring anything, and a thunk in that path would
                // put us inside the very interposer we are staying out of.
                if (_wcsnicmp(me.szModule, L"sl.", 3) == 0) continue;
                hits += patch_module(me.hModule, find, repl);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return hits;
    }

    void install_now()
    {
        HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
        if (sl == nullptr)
        {
            mgpu::diag::info("[MGPU][SLT1] TAP NOT INSTALLED: sl.interposer.dll is not "
                             "loaded. Nothing was touched.");
            return;
        }

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&install_now, &g_self);

        g_real_stff = (pf_set_tag_for_frame)GetProcAddress(sl, "slSetTagForFrame");
        g_real_st   = (pf_set_tag)GetProcAddress(sl, "slSetTag");

        mgpu::diag::info(
            "[MGPU][SLT1] TAP step 1/2 BEGIN: swapping import slots for slSetTagForFrame. "
            "One pointer store per slot, in the GAME's modules only - sl.* modules are "
            "skipped - and no data section is written anywhere. IF THIS IS THE LAST SLT1 "
            "LINE, IT DIED HERE.");
        const unsigned a = swap_everywhere((void *)g_real_stff, (void *)&hook_stff);

        mgpu::diag::info(
            "[MGPU][SLT1] TAP step 2/2 BEGIN: the same for the deprecated slSetTag. IF THIS "
            "IS THE LAST SLT1 LINE, IT DIED HERE.");
        const unsigned b = swap_everywhere((void *)g_real_st, (void *)&hook_st);

        g_slots.store(a + b, std::memory_order_relaxed);
        g_installed.store((a + b) != 0u, std::memory_order_relaxed);

        char line[1100];
        snprintf(line, sizeof line,
            "[MGPU][SLT1] TAP INSTALLED: %u slot(s) for slSetTagForFrame + %u for slSetTag. "
            "HOW TO READ IT. A non-zero count means the game imports the tagging call and "
            "the next frame will start naming buffers. ZERO IS A RESULT, NOT A FAILURE: it "
            "says this title resolves Streamline dynamically - the static interposer library "
            "does LoadLibrary and GetProcAddress and keeps the pointer in its own data - so "
            "an import table was never going to reach it. Reaching THAT would mean writing "
            "into another module's data section while it runs, which is the calibrator's "
            "R102 rung and is exactly the thing this round is trying not to do. If this "
            "reads zero, the decision is a deliberate one and belongs in the ledger, not in "
            "a reflex.",
            a, b);
        if ((a + b) != 0u) mgpu::diag::info(line);
        else               mgpu::diag::warn(line);
    }
} // namespace

void tick(unsigned long long frames, int mode)
{
    if (mode == 0) return;
    g_mode.store(mode, std::memory_order_relaxed);
    if (g_done.load(std::memory_order_relaxed)) return;

    // ---- THE DEFERRAL ----
    //
    // 900 frames, not zero. The one rig that still faults is the one whose
    // startup we cannot see, and the standing theory is that it reaches a
    // window our rig stops reaching once shaders are warm. Nothing in this
    // file may exist inside that window. By 900 presented frames the title
    // has compiled, resized, settled and is tagging every frame, and an
    // instrument that arrives then cannot be part of a startup race.
    if (frames < 900ull) return;

    {
        std::lock_guard<std::mutex> lk(g_cs);
        if (g_done.load(std::memory_order_relaxed)) return;
        g_done.store(true, std::memory_order_relaxed);
    }

    char line[600];
    snprintf(line, sizeof line,
        "[MGPU][SLT1] TAP INSTALL BEGIN mode=%d at frame %llu. DEFERRED ON PURPOSE: nothing "
        "in this instrument exists during startup, because the fault we are avoiding has "
        "only ever been seen during startup. Nothing has been touched yet.",
        mode, frames);
    mgpu::diag::info(line);
    install_now();
}

void report()
{
    if (!g_installed.load(std::memory_order_relaxed)) return;

    char line[1000];
    snprintf(line, sizeof line,
        "[MGPU][SLT1] TAGS mode=%d slots=%llu | tagging calls seen=%llu | tags read=%llu | "
        "elements skipped=%llu | measured stride=%llu | MVEC=0x%llx HUDLessColor=0x%llx "
        "ScalingInputColor=0x%llx. HOW TO READ IT. calls=0 with slots non-zero means the "
        "slots we swapped are not the ones this title calls through - the same shape as the "
        "calibrator's slots=44 resolved=0 on Plague Tale, and it means the caller cached the "
        "pointer before we arrived. MVEC non-zero is the authoritative motion vector address "
        "and should be compared against the R71 TRANSPORT SOURCE: if they differ, the ranked "
        "pick was wrong and this is the correction. skipped counts elements that failed the "
        "structType check or faulted while being read - a non-zero number there is the guard "
        "doing its job and a reason to re-read the offsets, never a reason to widen them.",
        g_mode.load(std::memory_order_relaxed),
        g_slots.load(std::memory_order_relaxed),
        g_calls.load(std::memory_order_relaxed),
        g_tags_read.load(std::memory_order_relaxed),
        g_skipped.load(std::memory_order_relaxed),
        g_stride.load(std::memory_order_relaxed),
        g_h_mvec.load(std::memory_order_relaxed),
        g_h_hudless.load(std::memory_order_relaxed),
        g_h_scale_in.load(std::memory_order_relaxed));
    mgpu::diag::info(line);
}

unsigned long long mvec_handle()          { return g_h_mvec.load(std::memory_order_relaxed); }
unsigned long long hudless_handle()       { return g_h_hudless.load(std::memory_order_relaxed); }
unsigned long long scaling_input_handle() { return g_h_scale_in.load(std::memory_order_relaxed); }

void uninstall()
{
    std::lock_guard<std::mutex> lk(g_cs);
    if (!g_installed.load(std::memory_order_relaxed)) return;
    swap_everywhere((void *)&hook_stff, (void *)g_real_stff);
    swap_everywhere((void *)&hook_st,   (void *)g_real_st);
    g_installed.store(false, std::memory_order_relaxed);
    mgpu::diag::info("[MGPU][SLT1] TAP REMOVED. Import slots restored.");
}

} // namespace mgpu::sltags
