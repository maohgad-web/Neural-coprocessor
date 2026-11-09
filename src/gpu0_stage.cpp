// MGPU Bridge - R333 StageCopy (GPU 0 async staging). See gpu0_stage.hpp for
// what it is for and the contract it keeps. Design: Dave (davidm71), OPT5.

#include "gpu0_stage.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::stage
{
namespace
{
const unsigned MAX_RING = 16u;

std::atomic<bool> g_up{false};

unsigned           g_ring = 0u;
unsigned long long g_slot_bytes = 0ull;
ID3D12Resource    *g_gxfer = nullptr;                 // borrowed, the caller owns it
ID3D12Resource    *g_stage = nullptr;                 // DEFAULT heap, game device, VRAM
ID3D12CommandQueue *g_cq = nullptr;                   // COPY queue, game device
ID3D12CommandAllocator *g_ca = nullptr;
ID3D12GraphicsCommandList *g_cl[MAX_RING] = {};       // prerecorded: stage slot k -> gxfer slot k
ID3D12Fence       *g_sf = nullptr;                    // game queue -> copy queue hand-off
unsigned long long g_last = 0ull;                     // last frame handed to the copy queue
unsigned long long g_submits = 0ull, g_fail = 0ull, g_bp_waits = 0ull;

// DMA timing (optional): copy-queue timestamps, two per slot, resolved into a
// READBACK buffer by the slot's own list.
ID3D12QueryHeap *g_cts = nullptr;
ID3D12Resource  *g_tsrb = nullptr;
const UINT64    *g_tsrb_cpu = nullptr;
UINT64           g_cts_freq = 0ull;
unsigned long long g_sampled = 0ull;                  // last fence value sampled
double   g_dma_sum = 0.0, g_dma_max = 0.0;
unsigned g_dma_n = 0u;
unsigned long long g_report_at = 1200ull;

bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

// StageCopy=1 and nothing else turns it on (the residual reader's rules).
bool read_key()
{
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return false;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return false;   // whole-document rule
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "StageCopy");
    return v != nullptr && atoi(v) == 1;
}

HRESULT make_buf(ID3D12Device *dev, UINT64 bytes, D3D12_HEAP_TYPE type, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_STATES st = (type == D3D12_HEAP_TYPE_READBACK)
                                       ? D3D12_RESOURCE_STATE_COPY_DEST
                                       : D3D12_RESOURCE_STATE_COMMON;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(out));
}

void free_all()
{
    g_up.store(false, std::memory_order_relaxed);
    for (unsigned k = 0; k < MAX_RING; ++k)
        if (g_cl[k] != nullptr) { g_cl[k]->Release(); g_cl[k] = nullptr; }
    if (g_cts != nullptr) { g_cts->Release(); g_cts = nullptr; }
    if (g_tsrb != nullptr)
    {
        if (g_tsrb_cpu != nullptr) g_tsrb->Unmap(0, nullptr);
        g_tsrb->Release(); g_tsrb = nullptr;
    }
    g_tsrb_cpu = nullptr;
    if (g_sf != nullptr) { g_sf->Release(); g_sf = nullptr; }
    if (g_ca != nullptr) { g_ca->Release(); g_ca = nullptr; }
    if (g_cq != nullptr) { g_cq->Release(); g_cq = nullptr; }
    if (g_stage != nullptr) { g_stage->Release(); g_stage = nullptr; }
    g_gxfer = nullptr;
    g_ring = 0u; g_slot_bytes = 0ull; g_last = 0ull;
    g_cts_freq = 0ull; g_sampled = 0ull;
    g_dma_sum = g_dma_max = 0.0; g_dma_n = 0u;
    g_report_at = 1200ull;
}

// Fold the DMA time of the newest frame the copy queue has completed into the
// running mean. gfence's completed value is the frame whose slot has landed, so
// that slot's two stamps have resolved. Each fence value is sampled once.
void dma_sample(ID3D12Fence *gfence)
{
    if (g_tsrb_cpu == nullptr || gfence == nullptr) return;
    const unsigned long long done = (unsigned long long)gfence->GetCompletedValue();
    if (done == 0ull || done <= g_sampled) return;
    g_sampled = done;
    const unsigned k = (unsigned)((done - 1ull) % g_ring);
    const UINT64 a = g_tsrb_cpu[k * 2u], b = g_tsrb_cpu[k * 2u + 1u];
    if (g_cts_freq == 0ull || b <= a) return;
    const double ms = (double)(b - a) * 1000.0 / (double)g_cts_freq;
    if (ms > 100.0) return;   // a stale or unwritten pair
    g_dma_sum += ms; ++g_dma_n;
    if (ms > g_dma_max) g_dma_max = ms;
}
} // namespace

bool arm(ID3D12Device *gdev, ID3D12CommandQueue *gq, ID3D12Resource *gxfer,
         unsigned long long heap_bytes, unsigned long long slot_bytes, unsigned ring)
{
    free_all();
    if (!read_key()) return false;   // key absent / 0: the module does not exist
    char line[700];
    if (gdev == nullptr || gq == nullptr || gxfer == nullptr || ring == 0u || ring > MAX_RING ||
        slot_bytes == 0ull || heap_bytes < slot_bytes * ring)
    {
        mgpu::diag::warn("[MGPU][R333] StageCopy=1 but the arm handed no usable heap/ring - OFF for this run.");
        return false;
    }
    g_ring = ring; g_slot_bytes = slot_bytes; g_gxfer = gxfer;

    HRESULT h = S_OK;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;   // GPU 0 VRAM: the fast write the game's list makes
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = heap_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        h = gdev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&g_stage));
    }
    if (SUCCEEDED(h))
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        h = gdev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_cq));
    }
    if (SUCCEEDED(h)) h = gdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_ca));
    if (SUCCEEDED(h)) h = gdev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_sf));

    // Copy-queue timestamps are optional; their absence only loses the DMA ms.
    if (SUCCEEDED(h))
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
        HRESULT th = gdev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3);
        if (SUCCEEDED(th) && !o3.CopyQueueTimestampQueriesSupported) th = E_NOTIMPL;
        if (SUCCEEDED(th))
        {
            D3D12_QUERY_HEAP_DESC qh{};
            qh.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
            qh.Count = ring * 2u;
            th = gdev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_cts));
        }
        if (SUCCEEDED(th)) th = make_buf(gdev, (UINT64)ring * 16ull, D3D12_HEAP_TYPE_READBACK, &g_tsrb);
        if (SUCCEEDED(th))
        {
            void *p = nullptr;
            th = g_tsrb->Map(0, nullptr, &p);
            if (SUCCEEDED(th) && p != nullptr) { memset(p, 0, (size_t)ring * 16u); g_tsrb_cpu = (const UINT64 *)p; }
            else th = E_FAIL;
        }
        if (SUCCEEDED(th)) th = g_cq->GetTimestampFrequency(&g_cts_freq);
        if (FAILED(th) || g_cts_freq == 0ull)
        {
            if (g_cts != nullptr) { g_cts->Release(); g_cts = nullptr; }
            if (g_tsrb != nullptr) { if (g_tsrb_cpu != nullptr) g_tsrb->Unmap(0, nullptr); g_tsrb->Release(); g_tsrb = nullptr; }
            g_tsrb_cpu = nullptr; g_cts_freq = 0ull;
        }
    }

    // The RING prerecorded lists: stage slot k -> gxfer slot k, the whole slot.
    for (unsigned k = 0; k < ring && SUCCEEDED(h); ++k)
    {
        h = gdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_ca, nullptr, IID_PPV_ARGS(&g_cl[k]));
        if (FAILED(h)) break;
        const UINT64 off = (UINT64)k * slot_bytes;
        if (g_cts != nullptr) g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u);
        g_cl[k]->CopyBufferRegion(gxfer, off, g_stage, off, slot_bytes);
        if (g_cts != nullptr)
        {
            g_cl[k]->EndQuery(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u + 1u);
            g_cl[k]->ResolveQueryData(g_cts, D3D12_QUERY_TYPE_TIMESTAMP, k * 2u, 2, g_tsrb, (UINT64)k * 16ull);
        }
        h = g_cl[k]->Close();
    }

    if (FAILED(h))
    {
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy FAILED TO COME UP hr=0x%08X - OFF for this run; the game's queue "
                 "copies straight to the shared heap as the shipped build does.", (unsigned)h);
        mgpu::diag::error(line);
        free_all();
        return false;
    }
    snprintf(line, sizeof line,
             "[MGPU][R333] StageCopy=1 UP (Dave's OPT5 design): %llu-byte VRAM staging ring on GPU 0 (%u slots of "
             "%llu bytes), a GPU 0 COPY queue with %u prerecorded slot copies, DMA timing %s. The game's list now "
             "copies VRAM to VRAM and the PCIe transfer runs on the DMA engine; gfence is signalled by the copy "
             "queue. Compare the in-frame GPU 0 copy ms (Gpu0Timers=1) and the game fps against a StageCopy=0 run.",
             heap_bytes, ring, slot_bytes, ring, (g_cts != nullptr) ? "ON" : "OFF (no copy-queue timestamps)");
    mgpu::diag::info(line);
    g_up.store(true, std::memory_order_release);
    return true;
}

bool active()
{
    return g_up.load(std::memory_order_relaxed);
}

ID3D12Resource *dst(ID3D12Resource *gxfer)
{
    return (g_up.load(std::memory_order_relaxed) && g_stage != nullptr) ? g_stage : gxfer;
}

// StageCopy off: exactly the old call.
// StageCopy on: the game's queue signals g_sf (its copies into the staging ring are
// done), the copy queue waits on that, moves every slot not yet moved up to f into
// gxfer, and signals gfence. Then the game's queue is told to wait - ON THE GPU -
// for gfence f+2-RING, so the slot its next frame writes has been moved out.
bool signal(ID3D12CommandQueue *gq, ID3D12Fence *gfence, unsigned long long f)
{
    if (!active() || g_cq == nullptr || g_sf == nullptr)
        return SUCCEEDED(gq->Signal(gfence, f));
    if (f <= g_last) return false;   // already handed over

    unsigned long long from = g_last + 1ull;
    if (f - from + 1ull > (unsigned long long)g_ring) from = f + 1ull - g_ring;   // older slots already overwritten

    if (FAILED(gq->Signal(g_sf, f)))
    {
        // Nothing reaches gxfer for these frames. Signalling gfence anyway keeps GPU 1
        // from waiting forever; the seal checker will see stale slots and say so.
        ++g_fail;
        g_last = f;
        return SUCCEEDED(gq->Signal(gfence, f));
    }
    if (FAILED(g_cq->Wait(g_sf, f))) ++g_fail;
    ID3D12CommandList *ls[MAX_RING];
    UINT n = 0;
    for (unsigned long long k = from; k <= f; ++k) ls[n++] = g_cl[(unsigned)((k - 1ull) % g_ring)];
    g_cq->ExecuteCommandLists(n, ls);
    const bool ok = SUCCEEDED(g_cq->Signal(gfence, f));
    if (!ok) ++g_fail;
    g_last = f;
    g_submits += n;

    if (f + 2ull > (unsigned long long)g_ring)
    {
        // Back-pressure, Dave's: a queue wait, never a thread wait. Counted when the
        // fence has not already passed the value (the DMA is RING-2 frames behind).
        const unsigned long long need = f + 2ull - g_ring;
        if ((unsigned long long)gfence->GetCompletedValue() < need) ++g_bp_waits;
        if (FAILED(gq->Wait(gfence, need))) ++g_fail;
    }

    dma_sample(gfence);
    if (f >= g_report_at)
    {
        g_report_at = f + 1200ull;
        char line[520];
        snprintf(line, sizeof line,
                 "[MGPU][R333] StageCopy f=%llu: slots staged %llu | failures %llu | back-pressure waits armed "
                 "behind the DMA %llu | staging DMA %s%.2f ms mean, %.2f ms max over %u sampled slots (off the "
                 "game's queue; -1 = not measured). In-frame GPU 0 copy ms: Gpu0Timers=1.",
                 f, g_submits, g_fail, g_bp_waits,
                 (g_dma_n != 0u) ? "" : "n/a ", (g_dma_n != 0u) ? g_dma_sum / (double)g_dma_n : -1.0,
                 (g_dma_n != 0u) ? g_dma_max : -1.0, g_dma_n);
        mgpu::diag::info(line);
        g_dma_sum = g_dma_max = 0.0; g_dma_n = 0u;
    }
    return ok;
}

void release()
{
    if (g_cq != nullptr)
    {
        // Drain: the copy queue writes gxfer, which the caller frees next. Bounded.
        ID3D12Fence *df = nullptr;
        ID3D12Device *dev = nullptr;
        if (SUCCEEDED(g_cq->GetDevice(IID_PPV_ARGS(&dev))) && dev != nullptr &&
            SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&df))) &&
            SUCCEEDED(g_cq->Signal(df, 1)) && df->GetCompletedValue() < 1)
        {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev != nullptr)
            {
                if (SUCCEEDED(df->SetEventOnCompletion(1, ev)))
                {
                    if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0)
                        mgpu::diag::warn("[MGPU][R333] release: the GPU 0 copy queue did not drain in 2 s; "
                                         "freeing anyway (the stream's own drain already passed gfence).");
                }
                CloseHandle(ev);
            }
        }
        if (df != nullptr) df->Release();
        if (dev != nullptr) dev->Release();
        char line[300];
        snprintf(line, sizeof line, "[MGPU][R333] StageCopy released: %llu slots staged, %llu failures, %llu back-pressure waits.",
                 g_submits, g_fail, g_bp_waits);
        mgpu::diag::info(line);
    }
    free_all();
    g_submits = g_fail = g_bp_waits = 0ull;
}
}
