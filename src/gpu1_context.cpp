// MGPU Bridge - the private D3D12 device on the selected adapter (T3;
// the device-removal poll accessor arrives with T4)
#include <windows.h>
#include <combaseapi.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>   // P1.3: ID3D12InfoQueue only. NOT ID3D12Debug -
                              // see the comment above transit_drain_info_queue.
#include <dxgi1_4.h>   // T5: IDXGISwapChain3 (GetCurrentBackBufferIndex),
                      // IDXGIFactory2 (CreateSwapChainForHwnd,
                      // MakeWindowAssociation) and DXGI_SWAP_CHAIN_DESC1.
                      // Cumulative include: also brings in dxgi1_2/
                      // dxgi1_3/dxgi.h.
#include <cstdio>
#include <cstdlib>   // atoll, malloc/free - used throughout; made explicit for P5.0
#include <cstring>
#include <cwchar>    // V45: wcsrchr, for splitting our own module path. Pulled in
                     // transitively by windows.h on MSVC, made explicit because a
                     // load path that fails to compile is not a fix.
#include <mutex>
#include <atomic>    // R78: one counter, written from the game's render thread
                     // without this file's mutex - it counts the times that
                     // mutex was NOT taken, so it cannot be guarded by it.

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "calibrator.hpp"   // R104: jitter compensation
#include "mgpu_ini_parser.hpp"
#include "screen.hpp"   // R108: the idle screen. Compiled since R108, called since V26.
#include "sl_probe.hpp"   // SL1: is our own device an SL proxy

// R111. DXGI_STATUS_OCCLUDED comes from dxgi.h by way of <dxgi1_4.h> above.
// Guarded because it is a SUCCESS code and a build where it went missing
// would fail silently in the worst way: the occlusion branch would simply
// never be taken, which is the state R111 exists to end.
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#endif

// P1.0: the NGX headers, fetched by CI into ext/ngx/ and never committed
// (THIRD_PARTY.md, "NVIDIA NGX headers"). CMakeLists.txt is closed and
// gains no include directory, so this is a quote include resolved relative
// to this file's own directory. It must stay BELOW <d3d12.h> above: the NGX
// header forward-declares ID3D12Device and ID3D12GraphicsCommandList, and
// the real definitions have to be in scope first.
//
// nvsdk_ngx_d3d12.h does not exist in this tree - the D3D12 entry points
// are declared in nvsdk_ngx.h itself, which pulls in nvsdk_ngx_defs.h and
// nvsdk_ngx_params.h by quote include from the same directory.
#include "../ext/ngx/nvsdk_ngx.h"

// P1.0: the add-on's own module handle, defined in dllmain.cpp and captured
// at DLL_PROCESS_ATTACH. worker.cpp reaches it the same way. The probe needs
// it to derive NGX's application data path - our own deploy directory, which
// ReShade has already demonstrated is writable by writing its log there.
// GetModuleHandle(nullptr) would return the game's module, not ours.
namespace mgpu { HMODULE module_handle(); }

namespace mgpu::gpu1
{
namespace
{
    struct state
    {
        std::mutex cs;
        ID3D12Device *device = nullptr;
        LUID device_luid{};

        // P1.3: the GAME's adapter LUID, copied out of the T2 selection.
        // Transit needs a device on the other adapter, and this is how it
        // is found again without reaching into adapter.cpp.
        LUID game_luid{};
        bool game_luid_known = false;

        // T5: the present chain. Created by create_present_chain (bridge
        // thread only) and released by shutdown() in reverse creation
        // order after the GPU has been drained. The raw pointers never
        // leave this translation unit - the same guarantee as the device,
        // which is why they sit behind the same mutex. The backbuffer
        // references are held for the chain's lifetime because the
        // PRESENT/RENDER_TARGET barriers name the resource (the RTV alone
        // cannot express them). present_failed_logged is the one-shot
        // guard for the first-failure line (brief T5: "log the first
        // Present failure and stop presenting; do not log every frame's
        // failure").
        ID3D12CommandQueue *queue = nullptr;
        IDXGISwapChain3 *swapchain = nullptr;
        ID3D12DescriptorHeap *rtv_heap = nullptr;
        ID3D12Resource *backbuffer[2] = {};
        // ---- D6: an allocator RING, not a single allocator ----
        //
        // This used to be one allocator and one list, and the thing that made
        // that legal was a WaitForSingleObject(INFINITE) at the end of every
        // present_frame - a full GPU round trip, on the bridge thread, which is
        // also the CONSUME thread. It was never about ordering and never about
        // which frame is displayed; it existed so the next frame's
        // allocator->Reset() could not land under the GPU.
        //
        // It is the same defect as the consumer's (see stream_poll), in the
        // other loop, and it was found by the same arithmetic: run A against
        // run B on 2026-09-11 put the whole present path at about 1.5 ms of
        // consumer-thread CPU, and this is the part of it that is a stall
        // rather than work.
        //
        // THREE SLOTS. Two would work with a swapchain of two buffers; three
        // leaves a frame of slack so the wait below is normally already
        // satisfied rather than merely brief. slot_value[i] is the fence value
        // of the last submission that used slot i, and 0 means never used.
        //
        // WHAT THIS DOES NOT CHANGE: the thread, the order, and the point at
        // which nr_final is read. The screen shows the same frame it showed
        // before. That is the whole reason this is separable from A3, which
        // moves the output's ownership and does change it.
        static const unsigned PRESENT_SLOTS = 3;
        ID3D12CommandAllocator *allocator[PRESENT_SLOTS] = {};
        ID3D12GraphicsCommandList *command_list[PRESENT_SLOTS] = {};
        UINT64 slot_value[PRESENT_SLOTS] = {};
        ID3D12Fence *fence = nullptr;
        HANDLE fence_event = nullptr;
        UINT64 fence_value = 0;
        bool present_failed_logged = false;
        // ---- R111: THE PRESENT STALL, COUNTED INSTEAD OF FATAL ----
        //
        // MEASURED, Battlefield 6, 2026-09-14 07:34:49.495: the slot-reuse
        // wait timed out after exactly 5000 ms, GetDeviceRemovedReason said
        // S_OK, and the bridge tore itself down while the game carried on
        // rendering for another 24 seconds. One transient stall of GPU 1's
        // present queue ended the session permanently, because present_frame
        // returning false makes the worker loop break into the ordered
        // teardown and there is no way back from it.
        //
        // A stall is not a fault. A flip-model swapchain that nobody is
        // compositing does not retire frames, and a fence value that has not
        // been reached yet is the correct behaviour of a queue whose presents
        // are still parked. The ONE thing that makes it fatal is a removed
        // device, and that is already a separate question with its own answer.
        //
        // So: count it, say it, skip the frame, and come back next time.
        unsigned long long present_stalls = 0;      // slot-reuse waits that timed out
        unsigned present_stall_run = 0;             // consecutive; any good frame clears it
        // R114b. ATOMIC, AND THAT IS THE WHOLE POINT OF THE CHANGE.
        //
        // These two are written from the present path, which runs every
        // presented frame. R111 wrote them under S.cs, which put a NEW MUTEX
        // ACQUISITION on the frame path to maintain a diagnostic counter.
        // That is exactly the kind of incidental change this round is meant
        // not to make: the bug being chased is a stall on this thread, and
        // adding a lock to the thread that stalls is how a diagnostic becomes
        // a cause. As atomics they are a relaxed store and nothing else.
        std::atomic<unsigned long long> present_occluded{0};  // Present returned DXGI_STATUS_OCCLUDED
        std::atomic<unsigned> last_present_status{0};         // last non-zero Present return, success codes included
        bool present_stall_logged = false;          // one-shot for the first stall
        bool neural_shown = false;   // P5.0: one-shot "the window is live" log
        // P7.0: the chain is resizable now. `hwnd` is kept because a resize has
        // to move the window as well as the buffers, and `chain_w/h` because
        // present_frame's crop maths needs the CURRENT size rather than the
        // 1280x720 T4 fixed it at.
        HWND hwnd = nullptr;
        UINT chain_w = 0, chain_h = 0;
        // P7.3: the backbuffer's ACTUAL format, tracked rather than assumed.
        // See DEFECT H at present_resize. Set at creation, updated at resize.
        DXGI_FORMAT chain_fmt = DXGI_FORMAT_UNKNOWN;
        bool borderless = false;
        bool sized_to_source = false;   // one resize per stream, not per frame
    };

    state &st()
    {
        static state s;
        return s;
    }
}

bool create_device(const adapter::selection_result &sel)
{
    auto &S = st();

    {
        std::lock_guard<std::mutex> lk(S.cs);
        if (S.device != nullptr)
        {
            mgpu::diag::info("[MGPU][T3] create_device: device already exists - no-op");
            return true;
        }
    }

    if (!sel.valid)
    {
        mgpu::diag::error("[MGPU][T3] no valid T2 selection - refusing D3D12CreateDevice; P0 cannot "
                          "proceed without a second adapter (see the [MGPU][T2] lines)");
        return false;
    }

    char line[400];
    snprintf(line, sizeof line,
             "[MGPU][T3] D3D12CreateDevice begin: selected luid=0x%08X-0x%08X desc=\"%s\" "
             "(game luid=%s)",
             (unsigned)sel.selected_luid.HighPart, (unsigned)sel.selected_luid.LowPart,
             sel.selected_desc,
             sel.game_luid_known ? "known, see [MGPU][T2] final line" : "unknown");
    mgpu::diag::info(line);

    ID3D12Device *dev = nullptr;
    HRESULT hr = D3D12CreateDevice(static_cast<IUnknown *>(sel.selected_adapter),
                                   D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr))
    {
        snprintf(line, sizeof line,
                 "[MGPU][T3] D3D12CreateDevice hr=0x%08X luid=0x%08X-0x%08X - STOP: no fallback to the "
                 "game's adapter; report the HRESULT and the [MGPU][T2] adapter table",
                 (unsigned)hr,
                 (unsigned)sel.selected_luid.HighPart, (unsigned)sel.selected_luid.LowPart);
        mgpu::diag::error(line);
        return false;
    }

    const LUID luid = dev->GetAdapterLuid();

    // The binding must be exactly what T2 selected - anything else is the
    // silent re-bind that makes every downstream result meaningless.
    if (luid.LowPart != sel.selected_luid.LowPart || luid.HighPart != sel.selected_luid.HighPart)
    {
        snprintf(line, sizeof line,
                 "[MGPU][T3] MISMATCH: device luid=0x%08X-0x%08X != selected luid=0x%08X-0x%08X - "
                 "releasing device, stopping (downstream would prove nothing)",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart,
                 (unsigned)sel.selected_luid.HighPart, (unsigned)sel.selected_luid.LowPart);
        mgpu::diag::error(line);
        dev->Release();
        return false;
    }
    if (sel.game_luid_known &&
        luid.LowPart == sel.game_luid.LowPart && luid.HighPart == sel.game_luid.HighPart)
    {
        snprintf(line, sizeof line,
                 "[MGPU][T3] FATAL: device bound to the GAME's luid=0x%08X-0x%08X - releasing, stopping",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart);
        mgpu::diag::error(line);
        dev->Release();
        return false;
    }

    // For the record only; P0 uses nothing cross-adapter. Whether this
    // driver advertises cross-adapter row-major texture support: a BOOL
    // member of D3D12_FEATURE_DATA_D3D12_OPTIONS, queried via
    // D3D12_FEATURE_D3D12_OPTIONS. There is no dedicated feature enum or
    // result struct for it.
    D3D12_FEATURE_DATA_D3D12_OPTIONS fx{};
    HRESULT fxhr = dev->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &fx, sizeof(fx));
    if (SUCCEEDED(fxhr))
        snprintf(line, sizeof line,
                 "[MGPU][T3] CrossAdapterRowMajorTextureSupported hr=0x00000000 supported=%d "
                 "(record only)",
                 fx.CrossAdapterRowMajorTextureSupported ? 1 : 0);
    else
        snprintf(line, sizeof line,
                 "[MGPU][T3] CrossAdapterRowMajorTextureSupported hr=0x%08X (record only; "
                 "CheckFeatureSupport failed)",
                 (unsigned)fxhr);
    mgpu::diag::info(line);

    {
        std::lock_guard<std::mutex> lk(S.cs);
        S.device = dev;
        S.device_luid = luid;
        S.game_luid = sel.game_luid;
        S.game_luid_known = sel.game_luid_known;
    }
    snprintf(line, sizeof line,
             "[MGPU][T3] D3D12CreateDevice hr=0x00000000 luid=0x%08X-0x%08X - device live on the "
             "second adapter; game rendering unaffected",
             (unsigned)luid.HighPart, (unsigned)luid.LowPart);
    mgpu::diag::info(line);

    // SL1. The device exists and nothing has been built on it yet, which
    // is the only moment where the answer is still free. If sl.interposer
    // wrapped it, every call we make from here re-enters Streamline.
    mgpu::slprobe::report(dev, nullptr);

    return true;
}

// T5: the present chain (brief section 06). Bridge thread only.
//
// Creation order (shutdown() releases the reverse): command queue, DXGI
// factory, swapchain, RTV heap, backbuffer references + RTVs, command
// allocator, command list, fence, fence event. The factory is local -
// CreateSwapChainForHwnd and MakeWindowAssociation are the only things
// that need it, and the swapchain holds its own reference to it.
bool create_present_chain(HWND hwnd)
{
    auto &S = st();

    if (hwnd == nullptr)
    {
        mgpu::diag::error("[MGPU][T5] create_present_chain: null hwnd - refusing (no window to "
                          "present against)");
        return false;
    }

    char line[400];

    // The chain is created on the T3 device - the swapchain lands on the
    // queue's adapter (gotcha 2), so the T2/T3 binding is what makes this
    // a GPU 1 swapchain. There is no adapter parameter anywhere to get
    // wrong.
    ID3D12Device *dev = nullptr;
    {
        std::lock_guard<std::mutex> lk(S.cs);
        if (S.device == nullptr)
        {
            mgpu::diag::error("[MGPU][T5] create_present_chain: no device - refusing (the chain is "
                              "created on the T3 device; there is no fallback path)");
            return false;
        }
        dev = S.device;
    }

    // Query the window's client rect itself (brief T5). The T5 window is
    // non-resizable (WS_THICKFRAME/WS_MAXIMIZEBOX removed), so this size
    // is fixed for the chain's lifetime - ResizeBuffers never happens at
    // P0.
    RECT rc{};
    if (GetClientRect(hwnd, &rc) == FALSE)
    {
        snprintf(line, sizeof line,
                 "[MGPU][T5] GetClientRect failed (GetLastError=%lu) hwnd=0x%p - refusing",
                 (unsigned long)GetLastError(), (void *)hwnd);
        mgpu::diag::error(line);
        return false;
    }
    const UINT width = static_cast<UINT>(rc.right - rc.left);
    const UINT height = static_cast<UINT>(rc.bottom - rc.top);

    ID3D12CommandQueue *queue = nullptr;
    IDXGIFactory2 *factory = nullptr;
    IDXGISwapChain1 *sc1 = nullptr;
    IDXGISwapChain3 *sc3 = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *back0 = nullptr;
    ID3D12Resource *back1 = nullptr;
    // D6: one per ring slot. Created together, released together, published
    // together - so a partial failure cannot leave the state holding two of
    // three and nothing saying so.
    ID3D12CommandAllocator *allocator[state::PRESENT_SLOTS] = {};
    ID3D12GraphicsCommandList *cl[state::PRESENT_SLOTS] = {};
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    HRESULT swapchain_hr = E_FAIL;

    // Failure cleanup: release everything created so far, in reverse
    // creation order. The device is not released here - it is T3's, and
    // this cycle's thread releases it in its own teardown.
    auto release_all = [&]()
    {
        if (event != nullptr)     { CloseHandle(event); event = nullptr; }
        if (fence != nullptr)     { fence->Release(); fence = nullptr; }
        for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)
        {
            if (cl[i] != nullptr)        { cl[i]->Release();        cl[i] = nullptr; }
            if (allocator[i] != nullptr) { allocator[i]->Release(); allocator[i] = nullptr; }
        }
        if (back1 != nullptr)     { back1->Release(); back1 = nullptr; }
        if (back0 != nullptr)     { back0->Release(); back0 = nullptr; }
        if (heap != nullptr)      { heap->Release(); heap = nullptr; }
        if (sc3 != nullptr)       { sc3->Release(); sc3 = nullptr; }
        if (sc1 != nullptr)       { sc1->Release(); sc1 = nullptr; }
        if (factory != nullptr)   { factory->Release(); factory = nullptr; }
        if (queue != nullptr)     { queue->Release(); queue = nullptr; }
    };

    // One error line per failure, with the failing call and its HRESULT -
    // the inputs to the decision, not just the outcome.
    auto fail = [&](const char *call, HRESULT hr) -> bool
    {
        snprintf(line, sizeof line,
                 "[MGPU][T5] create_present_chain failed at %s hr=0x%08X hwnd=0x%p client=%ux%u - "
                 "releasing everything created, returning false",
                 call, (unsigned)hr, (void *)hwnd, width, height);
        mgpu::diag::error(line);
        release_all();
        return false;
    };

    // 1. Command queue (DIRECT). The swapchain created against it lands
    // on this queue's adapter - the T3 device's adapter (GPU 1).
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = 0;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qd.NodeMask = 0;
        const HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr))
            return fail("CreateCommandQueue", hr);
    }

    // 2. The swapchain, against the window.
    {
        // The in-tree precedent (adapter.cpp, T2): CreateDXGIFactory2 with
        // the requested interface. IDXGIFactory2 is the surface that has
        // CreateSwapChainForHwnd and MakeWindowAssociation.
        // SL4. SAY IT BEFORE IT HAPPENS. The 2026-09-14 dump faulted on this
        // exact call: add-on -> ReShade's dxgi proxy -> sl.interposer ->
        // sl.dlss_g -> sl.common+0x611AA, a null read, while the bridge was
        // building its present chain on the SECOND adapter. If this is the
        // last line in a log, that is where it died and the stack is already
        // written down in STARTUP_CRASH_LEDGER.
        mgpu::diag::info(
            "[MGPU][SL4] creating the bridge's DXGI factory. IF THIS IS THE LAST LINE, IT DIED "
            "INSIDE CreateDXGIFactory2 - which on a Streamline title means the interposer, not "
            "DXGI. Nothing is bypassed here; this build only measures.");

        const HRESULT fhr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2),
                                               reinterpret_cast<void **>(&factory));
        if (FAILED(fhr))
            return fail("CreateDXGIFactory2", fhr);

        // SL4. Read-only: one documented QueryInterface, reference released.
        mgpu::slprobe::report_object("bridge DXGI factory", factory);

        // The desc is flat (gotcha 4): no BufferDesc, no OutputWindow, no
        // Windowed member - the HWND is a parameter of
        // CreateSwapChainForHwnd and pFullscreenDesc == nullptr is what
        // makes it windowed. Flip model (gotcha 3): FLIP_DISCARD,
        // BufferCount 2, SampleDesc.Count 1, R8G8B8A8_UNORM (no SRGB, no
        // MSAA - the older DISCARD/SEQUENTIAL effects fail outright).
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = width;
        scd.Height = height;
        // P5.0: R10G10B10A2_UNORM, not R8G8B8A8. The bridge's backbuffer is now
        // a COPY DESTINATION for the neural output, and CopyTextureRegion
        // requires the two formats to match exactly - there is no conversion in
        // a copy, and a converting blit would need a shader, which would need
        // d3dcompiler, which would need a new link library. CMakeLists.txt is
        // closed, so matching the game's format is not a shortcut here: it is
        // the only route. The game renders R10G10B10A2 (DXGI 24) and P3.1
        // established NR consumes and produces it unconverted, so the whole
        // chain is now one format end to end.
        scd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scd.Flags = 0;

        // Gotcha 1: the first parameter is the COMMAND QUEUE, not the
        // device. It is named pDevice and typed IUnknown *, so passing
        // the device compiles, runs, and fails at runtime with an
        // unhelpful E_INVALIDARG.
        swapchain_hr = factory->CreateSwapChainForHwnd(
            static_cast<IUnknown *>(queue), hwnd, &scd, nullptr, nullptr, &sc1);
        if (FAILED(swapchain_hr))
            return fail("CreateSwapChainForHwnd", swapchain_hr);

        // SL4. The object a possibly-wrapped factory just handed us. A factory
        // hands out the objects it makes, so if the factory is a proxy this
        // one is too - and the swapchain is what DLSS Frame Generation wraps.
        mgpu::slprobe::report_object("bridge swapchain", sc1);

        // Gotcha 5: without this, DXGI installs its own message hook on
        // our window and Alt+Enter toggles it to fullscreen - on the
        // display the game is using. A failure here does not break the
        // swapchain; the loss is the Alt+Enter protection. Log the input,
        // continue.
        const HRESULT mhr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(mhr))
        {
            snprintf(line, sizeof line,
                     "[MGPU][T5] MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER) hr=0x%08X "
                     "hwnd=0x%p - Alt+Enter protection degraded, continuing",
                     (unsigned)mhr, (void *)hwnd);
            mgpu::diag::error(line);
        }

        // Gotcha 6: CreateSwapChainForHwnd yields IDXGISwapChain1;
        // GetCurrentBackBufferIndex() is on IDXGISwapChain3. Without it
        // the index would be tracked by hand and drift.
        const HRESULT qhr = sc1->QueryInterface(__uuidof(IDXGISwapChain3),
                                                reinterpret_cast<void **>(&sc3));
        if (FAILED(qhr))
            return fail("QueryInterface(IDXGISwapChain3)", qhr);
        sc1->Release();
        sc1 = nullptr;
    }

    // 3. RTV heap: one descriptor per backbuffer - the frame picks the
    // current backbuffer by index.
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        const HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        if (FAILED(hr))
            return fail("CreateDescriptorHeap", hr);
    }

    // 3b. The backbuffer references and their RTVs. The references are
    // held for the chain's lifetime: gotcha 7 makes the
    // PRESENT/RENDER_TARGET barriers mandatory, and a barrier names the
    // resource - it cannot be expressed through the RTV alone.
    {
        const UINT rtv_size =
            dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const HRESULT hr0 = sc3->GetBuffer(0, __uuidof(ID3D12Resource),
                                           reinterpret_cast<void **>(&back0));
        if (FAILED(hr0))
            return fail("GetBuffer(0)", hr0);
        // CreateRenderTargetView returns void - there is no HRESULT to
        // check. A bad argument surfaces on the debug layer, not here.
        dev->CreateRenderTargetView(
            back0, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        const HRESULT hr1 = sc3->GetBuffer(1, __uuidof(ID3D12Resource),
                                           reinterpret_cast<void **>(&back1));
        if (FAILED(hr1))
            return fail("GetBuffer(1)", hr1);
        D3D12_CPU_DESCRIPTOR_HANDLE h1{};
        h1.ptr = heap->GetCPUDescriptorHandleForHeapStart().ptr + rtv_size;
        dev->CreateRenderTargetView(back1, nullptr, h1);
    }

    // 4. Command allocators + command lists, PRESENT_SLOTS of each (D6).
    // The old comment here said "the allocator is single: present_frame
    // waits on the fence before returning, which is what makes the next
    // frame's Reset safe." That wait is gone; the ring is what makes the
    // Reset safe now, and present_frame waits only on the slot it is about
    // to reuse - which in the steady state has long since completed.
    for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)
    {
        // CreateCommandAllocator takes the list type directly - there is
        // no D3D12_COMMAND_ALLOCATION_DESC and no allocator flags enum.
        const HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator[i]));
        if (FAILED(hr))
            return fail("CreateCommandAllocator", hr);
        const HRESULT clr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   allocator[i], nullptr,
                                                   IID_PPV_ARGS(&cl[i]));
        if (FAILED(clr))
            return fail("CreateCommandList", clr);
        // CreateCommandList returns the list in the RECORDING state. The
        // first frame calls Reset on it, and Reset on a recording list is
        // invalid - so close it once here, immediately after creation.
        const HRESULT cchr = cl[i]->Close();
        if (FAILED(cchr))
            return fail("Close (initial)", cchr);
    }

    // 5. Fence + event. The event is auto-reset: SetEventOnCompletion
    // sets it and the per-frame wait consumes it.
    {
        const HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr))
            return fail("CreateFence", hr);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
            return fail("CreateEventW", static_cast<HRESULT>(GetLastError()));
    }

    // Everything succeeded: publish under the lock (the game thread's
    // readers take the same lock), then log the creation line - one line
    // with the swapchain format, buffer count, client size, and the
    // HRESULT of CreateSwapChainForHwnd (brief T5 "Logging").
    {
        std::lock_guard<std::mutex> lk(S.cs);
        S.queue = queue;
        S.hwnd = hwnd;
        S.chain_w = width;
        S.chain_h = height;
        S.chain_fmt = DXGI_FORMAT_R10G10B10A2_UNORM;   // must match scd.Format above
        S.swapchain = sc3;
        S.rtv_heap = heap;
        S.backbuffer[0] = back0;
        S.backbuffer[1] = back1;
        for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)
        {
            S.allocator[i] = allocator[i];
            S.command_list[i] = cl[i];
            S.slot_value[i] = 0;          // never used: the reuse wait is a no-op
        }
        S.fence = fence;
        S.fence_event = event;
        S.fence_value = 0;
        S.present_failed_logged = false;
        // R111. A new chain starts with a clean stall history: the counters
        // describe THIS chain, and carrying them across a rebuild would make
        // the consecutive-stall limit fire on a chain that never stalled.
        S.present_stalls = 0;
        S.present_stall_run = 0;
        S.present_occluded.store(0, std::memory_order_relaxed);
        S.last_present_status.store(0, std::memory_order_relaxed);
        S.present_stall_logged = false;
        // The state owns them now; the locals must not release them twice.
        queue = nullptr;
        sc3 = nullptr;
        heap = nullptr;
        back0 = nullptr;
        back1 = nullptr;
        for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)
        { allocator[i] = nullptr; cl[i] = nullptr; }
        fence = nullptr;
        event = nullptr;
        factory->Release();
        factory = nullptr;
    }

    snprintf(line, sizeof line,
             // P7.3: this said R8G8B8A8 for four milestones while the chain was
             // created R10G10B10A2 - the string was never updated when P5.0
             // changed scd.Format. A log line that contradicts the code is worse
             // than no log line: it is evidence pointing the wrong way.
             "[MGPU][T5] present chain created: format=DXGI_FORMAT_R10G10B10A2_UNORM buffers=2 "
             "swapeffect=FLIP_DISCARD queue=DIRECT client=%ux%u CreateSwapChainForHwnd "
             "hr=0x%08X hwnd=0x%p (vsync present, non-resizable window - no ResizeBuffers at P0)",
             width, height, (unsigned)swapchain_hr, (void *)hwnd);
    mgpu::diag::info(line);
    return true;
}

// T5: one frame (brief section 06). Bridge thread only.
//
// The whole chain (and the device) is copied under the lock and the GPU
// work happens outside it: the lock is never held across a wait (the
// fence wait below is a wait), and the raw pointers never leave this
// translation unit, so detaching them from the state cannot expose a
// dangling pointer to any other caller (has_present_chain and
// device_removed_reason both read under the same lock).
// L5: read on the present thread, written on the game's render thread at arm.
// An atomic rather than a field on either state struct, because the present
// path must not take the stream lock - the lock order in this file is one of
// the two things that can reach the game.
static std::atomic<unsigned> g_present_vsync{1u};

// R142. The GAME runtime's depth-tap state, pushed in from dllmain. See the
// header. Relaxed on both ends: it is read once per present to pick an idle
// screen, and a frame of staleness on a condition that lasts for the whole
// session cannot matter.
static std::atomic<int> g_tap_state_game{-2};
void ui_set_tap_state(int state)
{
    g_tap_state_game.store(state, std::memory_order_relaxed);
}

// R143. The GAME runtime has presented past AutoArm's threshold without ever
// running an effect pass. Set from on_present, which is the only path that
// still arrives when that is true - see the header, and R138/R140 in dllmain
// for the log side of the same correction. A latch: set once, never cleared.
static std::atomic<bool> g_game_fx_absent{false};
void ui_set_game_fx_absent(bool absent)
{
    // R149. IT IS NOT A LATCH, AND CALLING IT ONE WAS WRONG.
    //
    // R143 justified never clearing this with "the condition it reports cannot
    // un-happen within a process". That is false. The counter behind it stops
    // the moment an effect pass is seen, so the THRESHOLD can only be crossed
    // by a runtime that has stayed silent past it - but a runtime that has been
    // silent for 600 presents can still start. A cold shader cache is the
    // ordinary way: ReShade compiles on load, 600 presents is ten seconds at
    // 60 fps, and a first launch with a dozen effects and no cache can spend
    // longer than that before the first pass runs. With the store one-way that
    // run got a red ERROR 204 for the rest of the session on a game that then
    // worked - a fault report manufactured by the fault reporter.
    //
    // So it stores what it is told. The game path clears it on any finish
    // effects event, because one effect pass is proof the claim is wrong.
    g_game_fx_absent.store(absent, std::memory_order_relaxed);
}

namespace
{
    // P5.0. Defined with the stream, below. Returns the neural output texture
    // when one is live, or nullptr. Takes the stream's own lock briefly and
    // never while holding this file's - stream_poll nests them the other way
    // round, and the two orders together would be a cycle.
    ID3D12Resource *stream_present_source(UINT &w, UINT &h, DXGI_FORMAT &fmt,
                                          D3D12_RESOURCE_STATES &rest);

    // P7.4. Same arrangement, for the LEFT half of a split present: the frame
    // as it arrived from the game, before the model. Returns nullptr unless
    // the mode is split AND a neural output exists to put beside it - half a
    // comparison would read as the model producing nothing.
    ID3D12Resource *stream_present_split_left(D3D12_RESOURCE_STATES &rest, float &pos);
}

// R108 / V26. Defined further down, beside the stream state: present_frame
// sits above stream_state in this file and cannot reach str() from here.
void present_screen_state(int &st_out, const char *&l1, const char *&l2);

// V43. The recovery launch. Defined beside present_screen_state, declared
// here because the idle-screen present path and ngx_probe both sit above it.
bool recovery_begin();
bool in_recovery();
void recovery_tick_presented();
void arm_sentinel_set();
void arm_sentinel_clear();
// arm_fault_recover is NOT declared here: it takes a stream_state &, a type
// that is not declared this early. It is defined in the anonymous namespace
// immediately above stream_nr_create, which is its only caller.

bool present_frame(float r, float g, float b)
{
    auto &S = st();

    ID3D12Device *dev = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *sc = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *backbuffer[2] = {};
    // D6: the slot this call will use, and the fence value that slot last
    // carried. slot is derived from fence_value, so it advances with the
    // submissions rather than with a counter that could drift from them.
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    UINT64 slot_prev = 0;
    unsigned slot = 0;

    {
        std::lock_guard<std::mutex> lk(S.cs);
        if (S.swapchain == nullptr || S.device == nullptr)
            return false;   // no chain: the caller already knows (its create call said so)
        dev = S.device;
        queue = S.queue;
        sc = S.swapchain;
        heap = S.rtv_heap;
        backbuffer[0] = S.backbuffer[0];
        backbuffer[1] = S.backbuffer[1];
        fence = S.fence;
        event = S.fence_event;
        fence_value = S.fence_value;
        slot = (unsigned)(fence_value % state::PRESENT_SLOTS);
        allocator = S.allocator[slot];
        cl = S.command_list[slot];
        slot_prev = S.slot_value[slot];
    }

    char line[800];   // P5.3 widened: the Present=in banner is longer than 320 and a
                      // truncated banner is a wrong label on a whole run

    // One-shot first-failure log (brief T5: "log the first Present
    // failure and stop presenting; do not log every frame's failure").
    // The step and its HRESULT are the decision inputs; the removal
    // reason says whether the device is what broke.
    auto fail = [&](const char *step, unsigned hr) -> bool
    {
        bool log_it = false;
        {
            std::lock_guard<std::mutex> lk(S.cs);
            if (!S.present_failed_logged)
            {
                S.present_failed_logged = true;
                log_it = true;
            }
        }
        if (log_it)
        {
            const HRESULT reason = dev->GetDeviceRemovedReason();
            snprintf(line, sizeof line,
                     "[MGPU][T5] present failed (logged once): %s hr=0x%08X "
                     "GetDeviceRemovedReason=0x%08X - the loop stops presenting",
                     step, hr, (unsigned)reason);
            mgpu::diag::error(line);
        }
        return false;
    };

    // The current backbuffer by index (gotcha 6) - no hand tracking.
    // GetCurrentBackBufferIndex takes NO parameters and returns UINT
    // directly; it is not an HRESULT call.
    const UINT index = sc->GetCurrentBackBufferIndex();
    if (index >= 2)
    {
        // Cannot happen with BufferCount 2. Guarded because the index
        // feeds both a descriptor offset and backbuffer[], and a wrong
        // one would be a silent out-of-bounds read.
        return fail("GetCurrentBackBufferIndex (index out of range)", index);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    rtv.ptr = heap->GetCPUDescriptorHandleForHeapStart().ptr +
              static_cast<UINT64>(index) *
              dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // ---- D6: WAIT ON THIS SLOT ONLY, AND ONLY IF IT IS STILL IN FLIGHT ----
    //
    // What replaces the old unconditional round trip. slot_prev is the fence
    // value of the last submission that used THIS allocator; anything at or
    // below the fence's completed value is done with. With three slots and a
    // swapchain of two, the steady state makes this a single GetCompletedValue
    // read and no wait at all.
    //
    // The timeout is finite where the old wait was INFINITE. A present chain
    // that never completes is a removed device, and hanging the bridge thread
    // forever is how that turns into a process nobody can diagnose. The
    // existing failure path already handles a false return.
    if (slot_prev != 0 && fence->GetCompletedValue() < slot_prev)
    {
        const HRESULT ehr = fence->SetEventOnCompletion(slot_prev, event);
        if (FAILED(ehr))
            return fail("SetEventOnCompletion (slot reuse)", static_cast<unsigned>(ehr));

        const DWORD wr = WaitForSingleObject(event, 5000);
        if (wr != WAIT_OBJECT_0)
        {
            // ---- R111. WHAT THIS USED TO SAY, AND WHY IT WAS WRONG ----
            //
            // It reported GetLastError() on a path whose normal outcome is
            // WAIT_TIMEOUT, where GetLastError carries nothing, and it
            // printed that nothing through the fail() lambda's hr= field.
            // The log therefore read "present failed ... hr=0x00000000",
            // which is the code for SUCCESS. A line that says a thing failed
            // with no error is the instrument failing, not the bridge.
            //
            // Now: the wait's own return, the two fence values that decide
            // the wait, the occlusion count and the last Present status -
            // the four numbers that separate a parked queue from a dead one.
            const HRESULT removed = dev->GetDeviceRemovedReason();
            const UINT64 done = fence->GetCompletedValue();

            unsigned long long stalls = 0, occ = 0;
            unsigned lps = 0;
            HWND wnd = nullptr;
            {
                std::lock_guard<std::mutex> lk(S.cs);
                stalls = ++S.present_stalls;
                occ = S.present_occluded.load(std::memory_order_relaxed);
                lps = S.last_present_status.load(std::memory_order_relaxed);
                wnd = S.hwnd;
                // R114. present_stall_run and present_stall_logged survive as
                // state because the counters are read by the line below and
                // reset by a good frame, but nothing branches on them any
                // more: the first stall is the last one, so a consecutive
                // count and a one-shot say-it-once bit have nothing left to
                // decide.
                S.present_stall_run = 1;
                S.present_stall_logged = true;
            }

            // Its own buffer: `line` is 800 and the reading below is longer
            // than that. A truncated diagnostic on the one path that used to
            // end the session is not a saving.
            char r111[1600];

            // A removed device is the one reading that is genuinely fatal,
            // and it keeps the old behaviour exactly.
            if (FAILED(removed))
            {
                snprintf(r111, sizeof r111,
                         "[MGPU][R111] PRESENT STALL WITH A REMOVED DEVICE - this one IS fatal. "
                         "wait=0x%08X slot_prev=%llu completed=%llu stalls=%llu occluded=%llu "
                         "last Present status=0x%08X.",
                         (unsigned)wr, (unsigned long long)slot_prev,
                         (unsigned long long)done, stalls, occ, lps);
                mgpu::diag::error(r111);
                return fail("slot reuse wait (device removed)", (unsigned)removed);
            }

            // ---- R114: STOP PRETENDING THIS RECOVERS ----
            //
            // R111 skipped the frame and came back. MEASURED, 2026-09-14
            // 08:34: five stalls, 5.01 seconds apart to the millisecond, and
            // then STREAM FAILED anyway. Skipping cannot unjam a fence that is
            // stuck - nothing about waiting again changes what the queue is
            // waiting for - so all the retry bought was twenty seconds of a
            // frozen window before the same ending, with the producer still
            // filling the ring and throwing DROPPED bursts into the log.
            //
            // The old fast teardown was better for the person playing. What
            // was missing was never the retry: it was TELLING THEM. A bridge
            // that vanishes with a developer's HRESULT in a log file is a
            // blind failure, and this one has a workaround a player can
            // actually perform.
            //
            // So: fail on the FIRST stall, the way it did before R111, and say
            // two things - the numbers, for us, and one plain sentence, for
            // them.
            //
            // WHAT WE HONESTLY KNOW, and the wording below is held to it:
            // this has only ever been seen with the bridge window not the top
            // window on its own display. That is an OBSERVED CORRELATION and
            // not a mechanism - DXGI reported zero occluded presents on the
            // run that produced it, the window was visible and not minimised,
            // and Present returned S_OK. So the message says what to try, and
            // does not claim to know why it works.
            //
            // NOT WIRED, deliberately: the R108 idle screen's error codes.
            // Drawing one needs a present, and a jammed present chain is the
            // thing being reported. A code nobody can see is worse than none.
            {
                const int iconic  = (wnd != nullptr) ? (IsIconic(wnd) ? 1 : 0) : -1;
                const int visible = (wnd != nullptr) ? (IsWindowVisible(wnd) ? 1 : 0) : -1;
                snprintf(r111, sizeof r111,
                         "[MGPU][R111] PRESENT STALL: the fence for this present slot did not "
                         "advance within 5000 ms. wait=0x%08X slot_prev=%llu completed=%llu | "
                         "stalls=%llu occluded-presents=%llu last Present status=0x%08X | "
                         "window minimised=%d visible=%d | DeviceRemovedReason=0x%08X. HOW TO "
                         "READ IT. The device is healthy and exactly one submission is "
                         "outstanding, so this is our own queue holding work that never "
                         "completes - not a removed device and not composition. R111's "
                         "skip-and-retry was measured on 2026-09-14 and did not recover: five "
                         "stalls at 5.01 s and then STREAM FAILED, so the bridge now stops at "
                         "the first one instead of freezing for twenty seconds first. The "
                         "cause is open; the copy queue's GPU gate is where it is being "
                         "chased, because it fired on exactly the frame count the neural "
                         "stage stopped at.",
                         (unsigned)wr, (unsigned long long)slot_prev,
                         (unsigned long long)done, stalls, occ, lps,
                         iconic, visible, (unsigned)removed);
                mgpu::diag::error(r111);

                mgpu::diag::error(
                    "[MGPU][R114] THE BRIDGE HAS STOPPED, AND THE GAME IS FINE - close nothing "
                    "in a hurry. WHAT TO DO: restart the game, and before arming, put the "
                    "bridge window on the second display and make sure it is the TOP window "
                    "there, not covered by the game or anything else. Every time this has been "
                    "seen, the bridge window was not the top window on its own display. WE DO "
                    "NOT YET KNOW WHY THAT MATTERS - Windows did not report the window as "
                    "covered or minimised on the run we caught, so this is something we have "
                    "observed and not something we can explain, and it may not be the whole "
                    "story. It is being worked on. Nothing on your machine has been left in a "
                    "bad state: the add-on shuts down in order from here, and the game keeps "
                    "running.");
            }
            return fail("slot reuse wait (stalled, not recoverable)", (unsigned)wr);
        }
    }

    // The allocator must be reset explicitly - resetting the command list
    // does not reclaim the allocator's memory, so omitting this grows it
    // without bound for as long as the loop runs.
    HRESULT hr = allocator->Reset();
    if (FAILED(hr))
        return fail("CommandAllocator::Reset", static_cast<unsigned>(hr));
    hr = cl->Reset(allocator, nullptr);
    if (FAILED(hr))
        return fail("CommandList::Reset", static_cast<unsigned>(hr));

    // Gotcha 7: barriers are mandatory. PRESENT -> RENDER_TARGET before
    // the clear, RENDER_TARGET -> PRESENT after it. Omitting them is a
    // debug-layer error and undefined behaviour in release.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backbuffer[index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &barrier);

    // ---- P5.0: SHOW THE NEURAL OUTPUT ----
    //
    // Every verdict this project has produced is a byte comparison. Nothing has
    // ever been looked at. That is a real gap: temporal ghosting from
    // DLSSNR.Reset=0 accumulating history, an inverted channel order, a
    // half-updated region - none of them moves a counter, and all of them are
    // obvious in one glance.
    //
    // A 1:1 CENTRED CROP, NOT A SCALED VIEW. The backbuffer is 1280x720 and the
    // neural output is the game's full frame, and there is no way to downscale
    // without a shader (see the format note at the swapchain). A crop is
    // therefore not a compromise on the way to something better - it is the
    // only honest option available, and it happens to be the right one: every
    // pixel shown is exactly a pixel NR produced, with no resampling standing
    // between the model's output and the eye.
    //
    // When no neural output exists - before the stream is armed, or after it
    // has run to its bound and released - this falls back to the cycling clear
    // colour. That fallback is also the signal that the stream has ended.
    UINT nw = 0, nh = 0;
    DXGI_FORMAT nfmt = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES nrest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource *nsrc = stream_present_source(nw, nh, nfmt, nrest);
    // The format check is not paranoia: the copy is silent about a mismatch at
    // record time and would fail at execute, taking the device with it.
    //
    // P7.3, DEFECT H's SECOND HALF - AND THE ACTUAL CAUSE OF THE BLINKING. This
    // compared against the CONSTANT R10G10B10A2, so on a game that renders
    // anything else the guard refused the copy every frame and the present fell
    // through to the cycling clear colour below. That is what "colours blinking
    // from start to finish, never the game" was: not corrupted pixels, but the
    // no-output fallback, running for the whole session because the guard was
    // measuring the neural output against one title's format instead of against
    // the backbuffer it is actually copying into.
    //
    // The guard was RIGHT to refuse - a mismatched CopyTextureRegion would have
    // taken the device down. It was simply asking the wrong question. The right
    // question is whether the source matches THIS CHAIN, whatever the chain is
    // now, and P7.3 makes the chain follow the game, so the two agree.
    DXGI_FORMAT bbfmt = DXGI_FORMAT_UNKNOWN;
    {
        std::lock_guard<std::mutex> lk(S.cs);
        bbfmt = S.chain_fmt;
    }
    if (nsrc != nullptr && nfmt == bbfmt && bbfmt != DXGI_FORMAT_UNKNOWN &&
        nw >= 1 && nh >= 1)
    {
        // P7.0: the CHAIN's size, not the 1280x720 T4 fixed. When the chain
        // has been resized to the source these are equal and the "crop" is the
        // whole frame - which is the point: still one CopyTextureRegion, still
        // no resampling anywhere in our code.
        UINT bw = 1280u, bh = 720u;
        {
            std::lock_guard<std::mutex> lk(S.cs);
            if (S.chain_w != 0 && S.chain_h != 0) { bw = S.chain_w; bh = S.chain_h; }
        }
        const UINT cw = (nw < bw) ? nw : bw;
        const UINT ch = (nh < bh) ? nh : bh;
        const UINT left = (nw - cw) / 2u;
        const UINT top  = (nh - ch) / 2u;

        // The backbuffer went PRESENT -> RENDER_TARGET above for the clear.
        // Take it on to COPY_DEST. The clear still happens first, so a crop
        // smaller than the backbuffer leaves the cycling colour as a border
        // rather than undefined pixels.
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        // R108 / V26. The border behind a crop smaller than the backbuffer.
        // This used to be the cycling colour, so a crop sat inside a band of
        // saturated hue that changed every frame and read as a fault. Same
        // dark neutral the idle screen uses, so the two never disagree.
        const float color_pre[4] = { 0.030f, 0.032f, 0.035f, 1.0f };
        cl->ClearRenderTargetView(rtv, color_pre, 0, nullptr);
        cl->ResourceBarrier(1, &barrier);

        D3D12_RESOURCE_BARRIER nb{};
        nb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        nb.Transition.pResource = nsrc;
        nb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        nb.Transition.StateBefore = nrest;
        nb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &nb);

        // P7.4. SPLIT. The left half is the INPUT, the right half is the
        // OUTPUT, and they are the SAME FRAME - not two captures aligned after
        // the fact, which is the thing that cannot be done in a game at all.
        // Both textures are the same size and format by construction, so the
        // seam falls on the same column of the same picture and every pixel
        // either side is at its true position: this is a split, not a blend
        // and not a resample.
        D3D12_RESOURCE_STATES lrest = D3D12_RESOURCE_STATE_COPY_DEST;
        float spos = 0.5f;
        ID3D12Resource *lsrc = stream_present_split_left(lrest, spos);
        // P7.5: the seam column, from the live position. Clamped one pixel in
        // from each edge so that a seam pushed all the way over never produces
        // a zero-width CopyTextureRegion, which is invalid rather than empty.
        UINT halfw = cw;
        if (lsrc != nullptr)
        {
            long sx = (long)((float)cw * spos + 0.5f);
            if (sx < 1) sx = 1;
            if (sx > (long)cw - 1) sx = (long)cw - 1;
            halfw = (UINT)sx;
        }

        D3D12_TEXTURE_COPY_LOCATION ps{}, pd{};
        ps.pResource = nsrc;
        ps.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ps.SubresourceIndex = 0;
        pd.pResource = backbuffer[index];
        pd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        pd.SubresourceIndex = 0;
        // Right half in split, whole frame otherwise. The destination x
        // matches the source x, so nothing shifts sideways when the mode
        // changes - the seam appears, the picture does not move.
        D3D12_BOX pbox{ left + halfw, top, 0, left + cw, top + ch, 1 };
        if (lsrc == nullptr) pbox.left = left;
        cl->CopyTextureRegion(&pd, (lsrc != nullptr) ? halfw : 0u, 0, 0, &ps, &pbox);

        nb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        nb.Transition.StateAfter = nrest;
        cl->ResourceBarrier(1, &nb);

        if (lsrc != nullptr)
        {
            D3D12_RESOURCE_BARRIER lb{};
            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            lb.Transition.pResource = lsrc;
            lb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            lb.Transition.StateBefore = lrest;
            lb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &lb);

            D3D12_TEXTURE_COPY_LOCATION ls{};
            ls.pResource = lsrc;
            ls.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ls.SubresourceIndex = 0;
            D3D12_BOX lbox{ left, top, 0, left + halfw, top + ch, 1 };
            cl->CopyTextureRegion(&pd, 0, 0, 0, &ls, &lbox);

            lb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            lb.Transition.StateAfter = lrest;
            cl->ResourceBarrier(1, &lb);
        }

        if (lsrc != nullptr)
        {
            // The seam, drawn last. Without it a viewer has to guess where the
            // boundary is, and on a frame where the two halves happen to look
            // similar they will guess wrong and conclude the split is not
            // working. Back to RENDER_TARGET for one 3px clear, then PRESENT.
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cl->ResourceBarrier(1, &barrier);

            const float seam[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            D3D12_RECT sr{};
            sr.left   = (LONG)((halfw >= 2u) ? (halfw - 2u) : 0u);
            sr.top    = 0;
            sr.right  = (LONG)(halfw + 1u);
            sr.bottom = (LONG)ch;
            cl->ClearRenderTargetView(rtv, seam, 1, &sr);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cl->ResourceBarrier(1, &barrier);
        }
        else
        {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cl->ResourceBarrier(1, &barrier);
        }

        bool say = false;
        {
            std::lock_guard<std::mutex> lk(S.cs);
            if (!S.neural_shown) { S.neural_shown = true; say = true; }
        }
        if (say)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P5.0] the bridge window is now showing the %s - %s of the %ux%u "
                     "frame at %ux%u, one CopyTextureRegion, no resampling in this add-on. The "
                     "cycling colour returns when the stream ends.",
                     (lsrc != nullptr)
                         ? "SPLIT (Present=split) - LEFT half is the frame handed TO DLSS-NR, "
                           "RIGHT half is what DLSS-NR produced, and BOTH ARE THE SAME FRAME. "
                           "Same instant, same camera, same lighting, one white seam between "
                           "them. Any difference across that seam is the model and nothing else"
                     : (nrest == D3D12_RESOURCE_STATE_COPY_DEST)
                         ? "NEURAL INPUT (mgpu.ini Present=in) - the transited game frame as it "
                           "was handed to DLSS-NR, BEFORE the neural stage. NR still runs and is "
                           "still timed; only what is on screen changed. If THIS looks wrong, the "
                           "fault is on our side of the handover and NR is innocent"
                         : "NEURAL OUTPUT - every pixel on screen is a pixel DLSS-NR produced on "
                           "the second adapter",
                     (cw == nw && ch == nh) ? "THE WHOLE"
                                            : "a 1:1 centre crop",
                     nw, nh, cw, ch);
            mgpu::diag::info(line);
        }
    }
    else
    {
        // P7.3. SAY WHY THE CYCLING COLOUR IS ON SCREEN. "No neural output yet"
        // and "there IS output but this function refused to copy it" look
        // identical from the outside - both are the blinking clear - and the
        // second one cost a night. Once, when output exists and was rejected.
        if (nsrc != nullptr && nfmt != bbfmt)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                snprintf(line, sizeof line,
                         "[MGPU][P7.3] THE CYCLING COLOUR IS A REFUSED COPY, NOT AN ABSENT FRAME. "
                         "Neural output exists (%ux%u, DXGI format %d) but the backbuffer is "
                         "format %d, and CopyTextureRegion does not convert - copying anyway would "
                         "fail at execute and take the device with it. The stream, the transport "
                         "and the neural stage are all running normally and their figures are "
                         "valid; only the display is blocked. The P7.3 resize should have made "
                         "these agree, so if you are reading this the chain did not get the "
                         "source format - check for a RESIZE FAILED line above.",
                         nw, nh, (int)nfmt, (int)bbfmt);
                mgpu::diag::error(line);
            }
        }

        // ---- R108 / V26: THE IDLE SCREEN, FINALLY CALLED ----
        //
        // This was the cycling saturated clear. screen.cpp has been in the
        // build since R108 and nothing ever called it, so every user who
        // reached this path got a full-screen flat hue changing every frame -
        // the harshest thing a display can do, indistinguishable from a fault,
        // and silent about what is actually missing.
        //
        // THE SWEEP BAR IS THE NEW LIVENESS PROOF. The cycling colour existed
        // to prove the loop was still presenting rather than hung on one
        // frame; the bar moves every frame and carries the same guarantee
        // without shouting. r, g and b are now unused here and are kept in the
        // signature only so worker.cpp does not have to change in the same
        // build as this.
        //
        // Error codes 201/202/203/301/302 are declared in screen.hpp and are
        // NOT wired up yet - nothing here detects those conditions, and a code
        // that can never appear is worse than no code. That is its own change.
        // V43. FIRST, and the order matters. The recovery launch counts its
        // clearing frames here, on the one path that only runs when a frame
        // actually reached the screen - a timer would tick through a loader
        // stall, this cannot. It also runs the one-shot recovery init, so
        // present_screen_state below is guaranteed to see the right state
        // even on the very first present, without depending on ngx_probe
        // having been reached first.
        recovery_tick_presented();

        int sst = mgpu::screen::st_idle;
        const char *sl1 = MGPU_IDLE_L1;
        const char *sl2 = MGPU_IDLE_L2;
        present_screen_state(sst, sl1, sl2);

        UINT sw = 0, sh = 0;
        {
            std::lock_guard<std::mutex> lk(S.cs);
            sw = S.chain_w; sh = S.chain_h;
        }
        if (sw == 0u || sh == 0u) { sw = 1280u; sh = 720u; }

        // Bridge thread only, and this is the only writer.
        static unsigned long long idle_frame = 0;
        ++idle_frame;

        mgpu::screen::draw(cl, rtv, sw, sh, idle_frame, sst, sl1, sl2);
        (void)r; (void)g; (void)b;

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &barrier);
    }

    hr = cl->Close();
    if (FAILED(hr))
        return fail("Close", static_cast<unsigned>(hr));

    ID3D12CommandList *const lists[1] = { cl };
    queue->ExecuteCommandLists(1, lists);

    // Signal the fence for this frame, present with vsync - Present(1, 0),
    // never Present(0, 0): an uncapped loop would spin GPU 1 at full rate
    // for a clear - and only then wait for the GPU to finish this frame.
    fence_value++;
    hr = queue->Signal(fence, fence_value);
    if (FAILED(hr))
        return fail("Signal", static_cast<unsigned>(hr));

    // ---- L5: THE PRESENT INTERVAL IS A LATENCY DECISION, NOT A STYLE ONE ----
    //
    // Present(1, 0) waits for vblank. With D6's three present slots and the
    // blocking wait removed, up to three presents can be queued, each gated to
    // a refresh interval - so the OUTPUT side can hold frames that nothing in
    // this project has ever measured. L2 stops at consume and P2.2 stops at the
    // evaluate; everything after them was dark.
    //
    // The original comment here said Present(0, 0) must never be used because
    // an uncapped loop would spin GPU 1 at full rate for a clear. That is true
    // of a loop that presents unconditionally. It is NOT true here: this path
    // only runs when there is a frame to show, so the producer paces it at its
    // own rate and there is nothing to spin on.
    //
    // PresentVsync=0 asks for the immediate present. Expect tearing - that is
    // the trade, and it is the user's to make, not ours to make silently.
    const UINT pi_interval = (g_present_vsync.load(std::memory_order_relaxed) != 0u) ? 1u : 0u;
    hr = sc->Present(pi_interval, 0);
    if (FAILED(hr))
        return fail("Present", static_cast<unsigned>(hr));

    // ---- R111: THE SUCCESS CODES PRESENT RETURNS, WHICH WERE INVISIBLE ----
    //
    // DXGI_STATUS_OCCLUDED is 0x087A0001 - the high bit is clear, so FAILED()
    // is false and the check above steps straight over it. It is DXGI saying
    // the frame was accepted and will not be shown, which is exactly the
    // condition under which the queue stops retiring and the slot-reuse wait
    // above times out. It has been happening in silence.
    if (hr != S_OK)
    {
        // R114b. NO LOCK HERE. Two relaxed atomic stores on the frame path,
        // where R111 took S.cs to do the same bookkeeping. Nothing reads
        // these except the stall path, which runs once and then tears down,
        // so relaxed ordering is all the guarantee they need.
        bool say = false;
        unsigned long long occ = 0;
        S.last_present_status.store((unsigned)hr, std::memory_order_relaxed);
        if (hr == DXGI_STATUS_OCCLUDED)
        {
            occ = S.present_occluded.fetch_add(1, std::memory_order_relaxed) + 1ull;
            say = (occ == 1ull) || ((occ % 600ull) == 0ull);
        }
        if (say)
        {
            char occl[800];
            snprintf(occl, sizeof occl,
                     "[MGPU][R111] PRESENT OCCLUDED x%llu (0x%08X). The frame was accepted and "
                     "will not be shown: nothing is compositing this window. This is not an "
                     "error and the loop keeps running, but it is the condition that parks the "
                     "present queue, and a parked queue is what R111's stall counter above is "
                     "counting. If stalls and this number move together, the window being "
                     "covered or off-screen is the whole story.",
                     occ, (unsigned)hr);
            mgpu::diag::warn(occl);
        }
    }

    // D6: NO WAIT HERE. This is the stall the ring exists to remove - a full
    // GPU round trip taken on the consume thread, once per presented frame,
    // for no reason but allocator reuse. The reuse wait above is where that
    // obligation now lives, and it is paid against a slot that is three
    // submissions old instead of the one just issued.

    // Publish the advanced fence value (bridge thread only: no other
    // writer). A failure before this line leaves it un-advanced; the
    // final drain in shutdown() re-signals and the wait still completes
    // (a fence value already reached fires its completion immediately,
    // and device removal signals all fences to UINT64_MAX - brief
    // section 09).
    {
        std::lock_guard<std::mutex> lk(S.cs);
        S.fence_value = fence_value;
        S.slot_value[slot] = fence_value;   // D6: this slot is busy until here
        // R111. A frame that reached Present clears the consecutive count.
        // The total is left alone: it is the session's history and the thing
        // the occlusion reading is checked against.
        S.present_stall_run = 0;
    }
    return true;
}

// T5: existence gate for the present chain. Any thread (takes this
// file's lock). T6 will need the native device / queue / swapchain
// pointers for create_effect_runtime; the accessor for those is written
// with T6, not now.
bool has_present_chain()
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    return S.swapchain != nullptr;
}

// T5 extends shutdown(), it does not replace it: the present chain is
// released first - after the GPU has been drained - and then the device
// exactly as T3 did it. The chain goes first because the swapchain holds
// a reference to the window it was created against: in T4's order
// (DestroyWindow first) it would outlive the window.
void shutdown()
{
    auto &S = st();

    // Detach the chain under the lock, release it outside: the lock is
    // never held across the fence wait below, and the raw pointers never
    // leave this translation unit (the same guarantee as the device), so
    // no other caller can observe the in-between state.
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *sc = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *backbuffer[2] = {};
    ID3D12CommandAllocator *allocator[state::PRESENT_SLOTS] = {};   // D6
    ID3D12GraphicsCommandList *cl[state::PRESENT_SLOTS] = {};
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    {
        std::lock_guard<std::mutex> lk(S.cs);
        queue = S.queue;
        sc = S.swapchain;
        heap = S.rtv_heap;
        backbuffer[0] = S.backbuffer[0];
        backbuffer[1] = S.backbuffer[1];
        for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)
        {
            allocator[i] = S.allocator[i];
            cl[i] = S.command_list[i];
            S.allocator[i] = nullptr;
            S.command_list[i] = nullptr;
            S.slot_value[i] = 0;
        }
        fence = S.fence;
        event = S.fence_event;
        fence_value = S.fence_value;
        S.queue = nullptr;
        S.swapchain = nullptr;
        S.rtv_heap = nullptr;
        S.backbuffer[0] = nullptr;
        S.backbuffer[1] = nullptr;
        S.fence = nullptr;
        S.fence_event = nullptr;
        S.fence_value = 0;
        S.present_failed_logged = false;
    }

    if (sc != nullptr)
    {
        // Before releasing anything, wait for the GPU to finish. Present
        // is asynchronous: returning from it does not mean the queue has
        // drained, and releasing the swapchain, queue or command list
        // while work is in flight is undefined behaviour - the failure
        // mode T4 spent a rig cycle eliminating. One final fence signal,
        // one wait. On a removed device this cannot hang: device removal
        // signals all fences to UINT64_MAX (brief section 09), so the
        // completion event fires even if the final Signal reports an
        // error.
        char line[256];
        fence_value++;
        const HRESULT shr = queue->Signal(fence, fence_value);
        if (FAILED(shr))
        {
            snprintf(line, sizeof line,
                     "[MGPU][T5] shutdown: final fence Signal hr=0x%08X - the device was likely "
                     "removed (removal signals all fences to UINT64_MAX); waiting anyway",
                     (unsigned)shr);
            mgpu::diag::info(line);
        }
        fence->SetEventOnCompletion(fence_value, event);
        const DWORD wr = WaitForSingleObject(event, INFINITE);
        if (wr != WAIT_OBJECT_0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][T5] shutdown: final fence wait failed (GetLastError=%lu) - releasing "
                     "the chain anyway",
                     (unsigned long)GetLastError());
            mgpu::diag::error(line);
        }

        // Reverse creation order (created: queue, swapchain, RTV heap,
        // backbuffers, allocator, command list, fence, event).
        CloseHandle(event);
        fence->Release();
        for (unsigned i = 0; i < state::PRESENT_SLOTS; ++i)   // D6
        {
            if (cl[i] != nullptr)        cl[i]->Release();
            if (allocator[i] != nullptr) allocator[i]->Release();
        }
        if (backbuffer[1] != nullptr)
            backbuffer[1]->Release();
        if (backbuffer[0] != nullptr)
            backbuffer[0]->Release();
        heap->Release();
        sc->Release();
        queue->Release();
        mgpu::diag::info("[MGPU][T5] present chain released (GPU drained, reverse creation order)");
    }

    // The device, as T3 did it - last: everything that references it
    // (the chain, above) is gone by now.
    {
        std::lock_guard<std::mutex> lk(S.cs);
        if (S.device != nullptr)
        {
            S.device->Release();
            S.device = nullptr;
            mgpu::diag::info("[MGPU][T3] device released");
        }
    }
}

bool has_device()
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    return S.device != nullptr;
}

bool device_removed_reason(HRESULT &out)
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    if (S.device == nullptr)
    {
        out = S_OK;
        return false;   // no device exists; nothing to poll
    }
    // GetDeviceRemovedReason() takes no parameters and returns S_OK when
    // healthy, otherwise the removal reason. It is sticky once removed
    // (brief section 09) - the caller guards the log with a one-shot
    // transition flag, so a healthy device is silent by construction.
    out = S.device->GetDeviceRemovedReason();
    return true;
}

// ---------------------------------------------------------------------
// P1.0b - make CreateFeature(Reserved18) succeed on the second GPU
//
// P1.0 answered the question that gated the milestone: NGX DOES stand up
// on a headless, non-game adapter. Init_Ext returned Success and
// GetCapabilityParameters returned Success against luid 00000000-0001382B,
// outputs=0. That result is recorded in P0_RECORD.md section 09 and
// VENDOR_LOCK.md and is not re-litigated here.
//
// What P1.0 left open, and what everything below exists to close:
//
//   1. CreateFeature(Reserved18) returned 0xBAD0000B in 0 ms - the NGX
//      core rejecting a feature it has no snippet mapping for. The feature
//      calls are therefore routed to nvngx_dlssnr.dll, which requires this
//      module's file name to contain "nvngx.dll" (hence the deploy name
//      nvngx.dll_mgpu_bridge.addon64).
//   2. We were blind to NGX's own diagnostics. An application-form Init
//      with a log callback fixes that; the driver's lines arrive prefixed
//      [MGPU][P1.0c][NGX].
//   3. The teardown crashed. The probe released a command list
//      CreateFeature had recorded into and that had never executed. It is
//      now closed, executed and fence-waited before anything is released,
//      and every teardown step announces itself before it runs.
// ---------------------------------------------------------------------
namespace
{
    // P1.0b. The first rig run settled what P1.0 could only infer: ALL
    // SEVEN entry points resolved from the core. _nvngx.dll exports
    // Init_Ext despite the header declaring it under NGX_SNIPPET_BUILD - a
    // header's preprocessor gating describes what a compiler sees, not what
    // a DLL exports. The earlier comment here said the opposite; it was
    // wrong and VENDOR_LOCK.md records why.
    //
    // What the run did NOT settle is where CreateFeature should be called.
    // The core accepted the call and rejected the feature:
    // 0xBAD0000B FAIL_UnableToInitializeFeature in 0 ms, which is the core
    // saying it has no feature->snippet mapping for Reserved18 and never
    // opening a library. So P1.0b routes the FEATURE calls to the snippet
    // and leaves the SESSION calls on the core:
    //
    //   core     Init / Init_Ext, GetCapabilityParameters,
    //            DestroyParameters, Shutdown1
    //   snippet  CreateFeature, ReleaseFeature, EvaluateFeature
    //
    // The snippet inspects its caller's return address and requires the
    // owning module's file name to contain the substring "nvngx.dll" -
    // which is why this add-on now deploys as
    // nvngx.dll_mgpu_bridge.addon64. Every snippet call therefore has to
    // originate from inside this module, and none of them may be forwarded
    // through a helper in another DLL. A caller that fails that test gets
    // 0xBAD00002 FAIL_PlatformError, which is a distinct code from the one
    // above and tells us the routing changed something.
    //
    // CreateFeature, ReleaseFeature and EvaluateFeature are ABI-identical
    // between the core-facing and snippet-facing builds of the header -
    // only the Init* family differs - so the typedefs below serve both.
    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_init)(
        unsigned long long InApplicationId,
        const wchar_t *InApplicationDataPath,
        ID3D12Device *InDevice,
        const NVSDK_NGX_FeatureCommonInfo *InFeatureInfo,
        NVSDK_NGX_Version InSDKVersion);

    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_init_ext)(
        unsigned long long InApplicationId,
        const wchar_t *InApplicationDataPath,
        ID3D12Device *InDevice,
        NVSDK_NGX_Version InSDKVersion,
        const NVSDK_NGX_Parameter *InParameters);

    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_get_cap_params)(
        NVSDK_NGX_Parameter **OutParameters);

    // The core declares InParameters non-const and the snippet declares it
    // const. That is a compile-time distinction only - the binary
    // signature is identical - so one typedef serves both modules.
    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_create_feature)(
        ID3D12GraphicsCommandList *InCmdList,
        NVSDK_NGX_Feature InFeatureID,
        NVSDK_NGX_Parameter *InParameters,
        NVSDK_NGX_Handle **OutHandle);

    // ---- V44: THE ONLY __try IN THIS PROJECT, AND IT STAYS THAT WAY ----
    //
    // CreateFeature(Reserved18) faults inside NVIDIA's cubin allocator when
    // the snippet's process-global arch binding belongs to the other adapter.
    // We cannot stop it faulting. We can stop it taking the user's game down
    // with it, and that is worth more than it sounds: the difference between
    // "Cyberpunk closed to desktop in the middle of a quest" and "the neural
    // stage did not start, here is why".
    //
    // WHY ITS OWN FUNCTION: MSVC rejects __try in any function that requires
    // object unwinding (C2712), and the arm path holds a lock_guard and
    // several RAII objects. Nothing in here has a destructor - every
    // parameter is a pointer or a POD - so this one compiles where an inline
    // __try around the same call would not.
    //
    // WHAT WE DO NOT DO: go back into NGX afterwards. After a fault inside a
    // vendor allocator the snippet's internal state is unknown, and treating
    // a caught access violation as "carry on" would be the single most
    // reckless thing in this file. The caller closes the session and stops.
    // Catching an AV is defensible exactly once, at the boundary of a vendor
    // call we cannot inspect, and nowhere else.
    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_create_seh_fn)(
        ID3D12GraphicsCommandList *InCmdList,
        NVSDK_NGX_Feature InFeatureID,
        NVSDK_NGX_Parameter *InParameters,
        NVSDK_NGX_Handle **OutHandle);

    // 0xDEAD0001 is not an NGX code. It is ours, it is checked by identity at
    // the one call site, and it exists so a fault is distinguishable from
    // every result NVIDIA can return.
    const unsigned NGX_RESULT_MGPU_SEH_FAULT = 0xDEAD0001u;

    NVSDK_NGX_Result ngx_create_guarded(ngx_pf_create_seh_fn fn,
                                        ID3D12GraphicsCommandList *cl,
                                        NVSDK_NGX_Feature feat,
                                        NVSDK_NGX_Parameter *params,
                                        NVSDK_NGX_Handle **out,
                                        unsigned long *seh_code_out)
    {
        __try
        {
            return fn(cl, feat, params, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (seh_code_out != nullptr) *seh_code_out = (unsigned long)GetExceptionCode();
            return (NVSDK_NGX_Result)NGX_RESULT_MGPU_SEH_FAULT;
        }
    }

    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_release_feature)(
        NVSDK_NGX_Handle *InHandle);

    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_destroy_params)(
        NVSDK_NGX_Parameter *InParameters);

    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_shutdown1)(
        ID3D12Device *InDevice);

    // P1.0c. NOT IN ANY PUBLIC HEADER. The snippet exports a routine the
    // core normally calls to have the feature register its own private
    // parameter keys - the DLSSNR.* set - into a parameter block. Reaching
    // it by hand is what lets the block arrive at CreateFeature configured
    // by the feature itself rather than by us transcribing key names from
    // a document.
    //
    // The signature is INFERRED, not read: the entry point appears in no
    // header we have. Two things make that acceptable here rather than
    // reckless. It plausibly takes exactly the block it populates, and on
    // x64 Windows there is one calling convention with arguments in
    // registers and the caller cleaning up - so a wrong ARITY does not
    // corrupt the stack the way the guide's x86-era warning implies. If it
    // returns something absurd, the result code says so and nothing has
    // been handed a bad pointer.
    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_populate_params)(
        NVSDK_NGX_Parameter *InParameters);

    // P1.1. The fourth argument is a progress callback. It is declared as
    // void * rather than PFN_NVSDK_NGX_ProgressCallback on purpose: we
    // always pass nullptr, the header's spelling of that typedef is one
    // more thing to get wrong, and on x64 Windows a pointer parameter is a
    // pointer parameter - same register, same ABI. If a real callback is
    // ever wanted, the type comes from the header at that point.
    typedef NVSDK_NGX_Result (NVSDK_CONV *ngx_pf_evaluate_feature)(
        ID3D12GraphicsCommandList *InCmdList,
        const NVSDK_NGX_Handle *InFeatureHandle,
        const NVSDK_NGX_Parameter *InParameters,
        void *InCallback);

    // ---- P1.1 helpers: local resources for the evaluate probe ----

    // One committed texture on the GPU 1 device. Every field is spelled out
    // rather than using a d3dx12 helper - CMakeLists.txt is closed and
    // d3dx12.h is not in the include path.
    HRESULT make_tex(ID3D12Device *dev, UINT w, UINT h, DXGI_FORMAT fmt,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                     ID3D12Resource **out)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hp.CreationNodeMask = 0;
        hp.VisibleNodeMask = 0;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Alignment = 0;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;

        return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            state, nullptr, IID_PPV_ARGS(out));
    }


    // P7.8: THE NEURAL TEXTURES MUST NOT BE _SRGB, AND THE PRESENT CHAIN MUST.
    //
    // Cyberpunk 2077 renders to DXGI 28, R8G8B8A8_UNORM_SRGB. The Blood of
    // Dawnwalker renders to 24, R10G10B10A2_UNORM, which has no sRGB sibling -
    // so until a second title was tested this path had never seen an _SRGB
    // format at all, and the code below passed the game's format straight into
    // every texture it creates.
    //
    // Two things are wrong with that, and only the first is certain:
    //
    //   1. tex_out and tex_pong are created ALLOW_UNORDERED_ACCESS. D3D12 does
    //      not support typed UAVs on _SRGB formats. That is an invalid
    //      combination whatever it appeared to do on this driver, and the debug
    //      layer would have said so - see section 09 for why this project
    //      cannot run it.
    //   2. An _SRGB view linearises on read and encodes on write. Anywhere the
    //      bytes are meant to pass through untouched, that is one conversion
    //      too many, and one extra linearisation looks exactly like the washed
    //      output observed on Cyberpunk with Present=in - which is BEFORE the
    //      model, so the neural stage cannot be the cause.
    //
    // The fix is to carry the bytes as UNORM through the whole pipeline and let
    // only the swapchain be _SRGB, because the game's own swapchain is _SRGB
    // and the compositor treats both the same way. Copies between an _SRGB
    // texture and its UNORM sibling are legal - they are the same typeless
    // family - so nothing else has to change.
    //
    // Formats without an sRGB variant are returned unchanged, so the Dawnwalker
    // path is byte-identical to what it was.
    DXGI_FORMAT nr_linear_format(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:                              return f;
        }
    }

    // A buffer on an UPLOAD or READBACK heap. Buffers must be created in
    // GENERIC_READ (upload) or COPY_DEST (readback); anything else is a
    // debug-layer error.
    HRESULT make_buf(ID3D12Device *dev, UINT64 bytes, D3D12_HEAP_TYPE type,
                     ID3D12Resource **out)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;

        const D3D12_RESOURCE_STATES state = (type == D3D12_HEAP_TYPE_UPLOAD)
            ? D3D12_RESOURCE_STATE_GENERIC_READ
            : D3D12_RESOURCE_STATE_COPY_DEST;

        return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            state, nullptr, IID_PPV_ARGS(out));
    }

    void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *res,
                 D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cl->ResourceBarrier(1, &b);
    }

    // The test image. This is not decoration.
    //
    // DLSS-NR reconstructs organic sub-pixel detail; on a flat or
    // low-frequency field the honest output is nearly identical to the
    // input, so a uniform clear colour would make "the model ran" and "the
    // model did nothing" indistinguishable. The bridge window currently
    // presents exactly such a flat field, which is why the probe uploads
    // its own image instead of sampling the backbuffer.
    //
    // Four ingredients, each of which NR has a documented reason to touch:
    // a smooth luminance ramp (local tone), hard edges (local structure),
    // a fine checker near the Nyquist limit (detail synthesis), and
    // deterministic pseudo-noise (texture). Deterministic on purpose - the
    // same bytes every run, so two runs are comparable.
    void fill_pattern(unsigned char *px, UINT w, UINT h, UINT row_pitch)
    {
        for (UINT y = 0; y < h; ++y)
        {
            unsigned char *row = px + (size_t)y * row_pitch;
            for (UINT x = 0; x < w; ++x)
            {
                const float fx = (float)x / (float)(w ? w : 1);
                const float fy = (float)y / (float)(h ? h : 1);

                float v = 0.25f + 0.5f * (fx * 0.5f + fy * 0.5f);          // ramp
                if (((x / 64) + (y / 64)) % 2 == 0) v += 0.12f;            // blocks
                if (((x / 2) + (y / 2)) % 2 == 0)   v += 0.06f;            // fine checker
                if (x % 128 < 3 || y % 128 < 3)     v = 0.95f;             // hard edges

                // xorshift-ish hash on the pixel index: deterministic,
                // no <random>, no state.
                unsigned int hsh = (unsigned int)(x * 1973u + y * 9277u + 26699u);
                hsh ^= hsh << 13; hsh ^= hsh >> 17; hsh ^= hsh << 5;
                v += ((float)(hsh & 0xFF) / 255.0f - 0.5f) * 0.08f;        // noise

                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                const unsigned char c = (unsigned char)(v * 255.0f + 0.5f);

                unsigned char *p = row + (size_t)x * 4;
                p[0] = c;                                   // R
                p[1] = (unsigned char)(c * 0.85f);          // G
                p[2] = (unsigned char)(c * 0.70f);          // B  (warm, skin-ish)
                p[3] = 255;
            }
        }
    }

    struct ngx_modules
    {
        HMODULE core = nullptr;      // _nvngx.dll       - the driver's NGX core
        HMODULE snippet = nullptr;   // nvngx_dlssnr.dll - the DLSS-NR feature
    };

    // =======================================================================
    // V45: LOAD THE NR SNIPPET FROM OUR OWN SUBFOLDER, NOT FROM BESIDE THE EXE
    // =======================================================================
    //
    // PROVEN 2026-09-13, and it is the cause of the crash this project spent a
    // night on - not a workaround for it.
    //
    // Streamline scans for nvngx_*.dll beside the executable and initialises
    // what it finds. Cyberpunk does not use neural rendering and has no reason
    // to touch this DLL, but because WE told people to drop it next to the exe,
    // the game loads it and NGX's cubin layer binds itself to the GAME's
    // adapter before our bridge thread exists:
    //
    //   22:10:02:547 [26996]  our add-on is loaded          (game main thread)
    //   22:10:04     [26996]  SetGPUArch:: luid: 0x13fe8    (GPU 0, same thread)
    //   22:10:05:548 [ 8024]  our bridge thread starts      (1.5 s too late)
    //
    // That run had our NGX code provably disabled - the log carries "[P1.0c]
    // SKIPPED - recovery launch" and "[P4.0] arm REFUSED" - and the snippet
    // initialised anyway. It was never us.
    //
    // The binding is per LOADED MODULE, and Windows keys modules by resolved
    // path. A copy of the same DLL under a different path is a SEPARATE module
    // with its own data section and its own globals. So we load ours from
    // <add-on dir>\mgpu\nvngx_dlssnr.dll and get a snippet nobody else in the
    // process has touched: our Init is the first one for that instance, and it
    // binds to GPU 1.
    //
    // THIS IS TRIED FIRST, ALWAYS, even when a copy is already resident beside
    // the exe. Deliberately: a user upgrading from 0.1.0 will still have the
    // old file there, the game will still latch it to GPU 0, and the private
    // instance is what saves them anyway. Requiring people to delete the old
    // file correctly is not a fix, it is a support queue.
    //
    // Returns null when the subfolder copy is absent - the caller then falls
    // back to the 0.1.0 layout and says loudly that it is the broken one.
    // Our own directory, with a trailing backslash. This is also the game's
    // exe directory - a ReShade add-on sits beside dxgi.dll, which sits beside
    // the executable - so "beside us" and "beside the exe" are the same place.
    bool addon_dir_w(wchar_t *out, size_t out_n)
    {
        if (out == nullptr || out_n < 8) return false;
        out[0] = L'\0';
        const DWORD n = GetModuleFileNameW(mgpu::module_handle(), out, (DWORD)out_n);
        if (n == 0 || n >= out_n) return false;
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash == nullptr) return false;
        *(slash + 1) = L'\0';
        return true;
    }

    bool file_exists_w(const wchar_t *p)
    {
        const DWORD a = GetFileAttributesW(p);
        return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) == 0);
    }

    // ---- V46: FIND A PRIVATE SNIPPET UNDER *ANY* SUBFOLDER NAME ----
    //
    // V45 looked only in "mgpu\". The folder on the development rig was named
    // "mpgu" - a transposition of the project's own name, made by the person
    // who named the project - and V45 silently found nothing, reported "not
    // found", and cost twenty minutes of reading logs that could not possibly
    // say anything.
    //
    // A user who makes that typo gets a bug report we cannot reproduce and an
    // evening they will not get back. So: try "mgpu\" first because that is
    // what the README will say, then walk the immediate subdirectories and
    // take the first one that has the file. One FindFirstFile pass over a
    // handful of folders, once per launch, at startup.
    //
    // NOT recursive, deliberately. The game directory has thousands of files
    // under it and a deep walk at startup is a stall nobody asked for. One
    // level is enough for "put it in a folder next to the add-on".
    bool find_private_snippet(const wchar_t *name, wchar_t *out, size_t out_n)
    {
        if (name == nullptr || out == nullptr || out_n < 8) return false;
        out[0] = L'\0';

        wchar_t dir[MAX_PATH * 2] = {};
        if (!addon_dir_w(dir, MAX_PATH * 2)) return false;

        // 1. the documented location
        _snwprintf_s(out, out_n, _TRUNCATE, L"%smgpu\\%s", dir, name);
        if (file_exists_w(out)) return true;

        // 2. any immediate subdirectory
        wchar_t glob[MAX_PATH * 2] = {};
        _snwprintf_s(glob, MAX_PATH * 2, _TRUNCATE, L"%s*", dir);

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) { out[0] = L'\0'; return false; }
        bool found = false;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (fd.cFileName[0] == L'.') continue;   // covers "." and ".."
            _snwprintf_s(out, out_n, _TRUNCATE, L"%s%s\\%s", dir, fd.cFileName, name);
            if (file_exists_w(out)) { found = true; break; }
        } while (FindNextFileW(h, &fd) != FALSE);
        FindClose(h);

        if (!found) out[0] = L'\0';
        return found;
    }

    HMODULE load_private_snippet(const wchar_t *name, char *where, size_t where_n)
    {
        if (where != nullptr && where_n > 0) where[0] = '\0';

        wchar_t p[MAX_PATH * 2] = {};
        if (!find_private_snippet(name, p, MAX_PATH * 2)) return nullptr;

        HMODULE m = LoadLibraryW(p);
        if (m != nullptr && where != nullptr && where_n > 0)
            snprintf(where, where_n, "%ls", p);
        return m;
    }

    HMODULE nr_load_private_snippet(char *where, size_t where_n)
    {
        return load_private_snippet(L"nvngx_dlssnr.dll", where, where_n);
    }

    // Is a copy sitting beside the exe where Streamline will find it? Purely
    // diagnostic - we do not delete or rename the user's files - but it is the
    // single most useful line we can put in a bug report from 0.2.0 onwards.
    //
    // V46: asks the FILESYSTEM, not the loader. GetModuleHandleW only answers
    // once somebody has already loaded it, which on a fast startup can be
    // after we look - and the panel needs this answer before anything arms.
    bool nr_snippet_is_beside_exe()
    {
        wchar_t dir[MAX_PATH * 2] = {};
        if (!addon_dir_w(dir, MAX_PATH * 2)) return false;
        wchar_t p[MAX_PATH * 2] = {};
        _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%snvngx_dlssnr.dll", dir);
        return file_exists_w(p);
    }

    // P1.0 stopped at the first module that answered, which is why its log
    // said "CreateFeature=core" and could not say whether the snippet also
    // exported it. That ambiguity is exactly what this task needs resolved,
    // so both modules are always queried and both are reported.
    //
    // `prefer` picks which module's pointer is USED when both have the
    // name. A preference that cannot be honoured falls back to the other
    // module rather than failing - and says so, because a silent fallback
    // would make the run unreadable.
    //
    // `where` receives e.g. "core+snippet(used:snippet)" or "core+-(used:core)".
    enum class ngx_prefer { core, snippet };

    FARPROC ngx_resolve(const ngx_modules &m, const char *name, ngx_prefer prefer,
                        char *where, size_t where_n)
    {
        FARPROC in_core = (m.core != nullptr) ? GetProcAddress(m.core, name) : nullptr;
        FARPROC in_snip = (m.snippet != nullptr) ? GetProcAddress(m.snippet, name) : nullptr;

        FARPROC pick = nullptr;
        const char *used = "missing";
        if (prefer == ngx_prefer::snippet)
        {
            if (in_snip != nullptr)      { pick = in_snip; used = "snippet"; }
            else if (in_core != nullptr) { pick = in_core; used = "core!FALLBACK"; }
        }
        else
        {
            if (in_core != nullptr)      { pick = in_core; used = "core"; }
            else if (in_snip != nullptr) { pick = in_snip; used = "snippet!FALLBACK"; }
        }

        snprintf(where, where_n, "%s+%s(used:%s)",
                 in_core != nullptr ? "core" : "-",
                 in_snip != nullptr ? "snippet" : "-",
                 used);
        return pick;
    }

    // P1.0c: resolve strictly from one module, with no fallback. The
    // snippet's own session is what FAIL_NotInitialized was complaining
    // about, so "initialise the snippet" must not be silently satisfiable
    // by the core's export of the same name - which both modules carry.
    FARPROC ngx_resolve_strict(HMODULE m, const char *name, char *where, size_t where_n)
    {
        FARPROC p = (m != nullptr) ? GetProcAddress(m, name) : nullptr;
        snprintf(where, where_n, "%s", p != nullptr ? "yes" : "no");
        return p;
    }

    // P1.0b: NGX's own diagnostics. The registry's
    // HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\LogLevel opens the
    // tap; this callback is the only thing that catches what comes out.
    //
    // It fires on driver threads we do not own, at times we do not choose.
    // So: no lock (st().cs above all - the driver may call this from inside
    // a call we are making while holding it), no NGX call, no D3D12 object,
    // no allocation beyond one stack buffer. Format and hand to diag, which
    // is the same sink every other line in this file uses.
    //
    // The typedef comes from the header rather than being written out by
    // hand, so a signature that does not match is a compile error here
    // instead of stack corruption on the rig.
    void NVSDK_CONV ngx_log_callback(const char *message,
                                     NVSDK_NGX_Logging_Level level,
                                     NVSDK_NGX_Feature source)
    {
        if (message == nullptr)
            return;

        // The driver's lines carry their own trailing newline; diag adds
        // one. Copy and trim rather than logging a blank line per message.
        char buf[900];
        size_t n = 0;
        for (; n + 1 < sizeof buf && message[n] != '\0'; ++n)
            buf[n] = message[n];
        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        if (n == 0)
            return;

        char line[1000];
        snprintf(line, sizeof line, "[MGPU][P1.0c][NGX] level=%d feature=%d %s",
                 (int)level, (int)source, buf);
        mgpu::diag::info(line);
    }

    // Symbolic names for the result codes so a log line carries both the
    // raw value and what the header calls it. Anything unmapped logs as a
    // bare number rather than as a guess.
    const char *ngx_result_name(NVSDK_NGX_Result r)
    {
        switch (r)
        {
        case NVSDK_NGX_Result_Success:                         return "Success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:        return "FAIL_FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:              return "FAIL_PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:       return "FAIL_FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:            return "FAIL_FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:           return "FAIL_InvalidParameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:      return "FAIL_ScratchBufferTooSmall";
        case NVSDK_NGX_Result_FAIL_NotInitialized:             return "FAIL_NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:     return "FAIL_UnsupportedInputFormat";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:              return "FAIL_RWFlagMissing";
        case NVSDK_NGX_Result_FAIL_MissingInput:               return "FAIL_MissingInput";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:  return "FAIL_UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                  return "FAIL_OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:             return "FAIL_OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:          return "FAIL_UnsupportedFormat";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "FAIL_UnableToWriteToAppDataPath";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter:       return "FAIL_UnsupportedParameter";
        case NVSDK_NGX_Result_FAIL_Denied:                     return "FAIL_Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:             return "FAIL_NotImplemented";
        default:                                               return "unmapped";
        }
    }
}

namespace
{
    // P1.4. One forward declaration. It is defined with the P1.3 transit
    // helpers further down, and ngx_probe needs it because P1.4 lives INSIDE
    // ngx_probe rather than beside it. That placement is deliberate: the NGX
    // session, the parameter block and the feature handle are locals of this
    // function, and opening a SECOND NGX session on the same device to reach
    // them from outside is exactly the thing P1.0b showed is delicate. P1.4
    // reaches the session where it already is. The GPU 0 side builds its own
    // command objects inline rather than borrowing transit_side, so nothing
    // else has to move.
    HRESULT transit_make_device(LUID want, ID3D12Device **out);

    // P5.2. DEFECT C, found on the rig 2026-09-05. The one-shot probe chain
    // and the P4.1 persistent neural stage BOTH take their parameter block
    // from NVSDK_NGX_D3D12_GetCapabilityParameters, and that call does not
    // hand out a fresh block per caller - it hands out the core's capability
    // block. stream_release has said so in a comment since P4.1 ("nr_params
    // is NOT destroyed: it is the core's capability block") and acted on it.
    // The probe's teardown did not: it called DestroyParameters
    // unconditionally, which destroyed the block the LIVE stream still held
    // and still wrote DLSSNR.Color / DLSSNR.Output into every frame.
    //
    // The symptom was a neural image with the colours wrong and NOTHING else
    // out of place - every seal counter zero, gap=1 throughout. That is the
    // section 00 failure shape exactly: the transport was healthy and the
    // instrument had no way to say the consumer was broken.
    //
    // Latent, not new. In the P5.0 run the probe chain finished (P3.1 at
    // :280) before the stream's NR came up (P5.0 at :735) and the destroy
    // landed on a block nobody else held. P5.1 moved stream_poll() ahead of
    // present_frame() in the worker loop, which reversed that order and put
    // the destroy in the middle of a live stream.
    //
    // Returns true when the stream's neural stage holds the block. Takes the
    // stream's own lock; defined with the stream further down.
    bool stream_nr_live();
}

bool ngx_probe(UINT width, UINT height, const ngx_input_frame *ext)
{
    // ---- V43: THE RECOVERY GATE, AND IT IS FIRST ON PURPOSE ----
    //
    // The whole value of a recovery launch is that this module opens NO NGX
    // session. This probe opens one - it is the earliest NGX Init in the
    // process on our side - so if it runs, the launch is not a clearing
    // launch and the residue survives. Everything below this line is
    // unreachable while ArmCrashed is set, and that is the point.
    if (in_recovery())
    {
        mgpu::diag::warn("[MGPU][P1.0c] SKIPPED - recovery launch. NGX is not touched at all "
                         "this run. See the [MGPU][V43] line above.");
        return false;
    }

    auto &S = st();
    // P3.2 widened this from 600: its verdict has to state the discipline,
    // the counts and what the result retracts, and a truncated retraction is
    // worse than none.
    char line[1200];

    // P3.0. `ext` is the entire difference between this being a probe and
    // this being a pipeline stage. When it is null every path below is
    // byte-identical to the P1 build that has been passing since P1.0 -
    // that is deliberate, and it is why this milestone is a parameter
    // rather than a fork: the four probes that already work must not be
    // able to regress because P3 was added.
    //
    // When it is non-null, the colour input is the GAME'S OWN FRAME, already
    // carried across the adapter boundary by the P1.5 capture path and
    // waited on by P2.0's shared fence, and this call is the last stage of
    // the chain the whole project exists to build:
    //
    //   game frame -> cross the boundary -> DLSS-NR on GPU 1
    //
    // Two things change with it and nothing else does. The upload writes the
    // supplied pixels instead of fill_pattern, and P1.4's transit block is
    // skipped - P1.4 compares against a control that only exists for the
    // synthetic path, and running it here would compare a real frame against
    // a pattern's control and call the difference a finding.
    const bool P3 = (ext != nullptr);
    // P3.1 needs the RETURN VALUE to mean "the neural stage produced a real
    // result", not merely "nothing threw". Without this, a run where every
    // NGX call succeeded but the model changed nothing would report true, the
    // caller would take the native format as accepted, and a null result
    // would be recorded as the answer. Set only in the P3 verdict below.
    bool p3_ok = false;
    if (P3)
    {
        snprintf(line, sizeof line,
                 "[MGPU][P3.0] ngx_probe entered with an EXTERNAL frame: %ux%u src_pitch=%u "
                 "src_dxgi_fmt=%u. This is the game's own transited frame, not a pattern of "
                 "ours. The P1.2 intensity comparison below now runs against real rendered "
                 "content; the P1.4 transit block is skipped by design.",
                 width, height, ext->row_pitch, ext->dxgi_format);
        mgpu::diag::info(line);
    }

    // The device and its LUID are copied out under the lock and every NGX
    // call happens outside it. CreateFeature took 1.16 s on the reference
    // run, and the game thread's has_present_chain / device_removed_reason
    // readers take this same lock - holding it across a call that long
    // would stall them. Safe because the probe runs on the bridge thread,
    // which is also the only thread that releases these objects.
    //
    // P1.0b also copies the present chain's QUEUE. CreateFeature records
    // initialisation work into the command list it is handed, and that work
    // has to execute before anything it touched is released - so the probe
    // needs somewhere to submit. Taking the existing DIRECT queue is safe
    // here and only here: the probe runs on the bridge thread between
    // create_present_chain returning and the present loop starting, so no
    // frame is in flight and nothing else is submitting.
    ID3D12Device *dev = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    LUID luid{};
    {
        std::lock_guard<std::mutex> lk(S.cs);
        dev = S.device;
        queue = S.queue;
        luid = S.device_luid;
    }
    if (dev == nullptr)
    {
        mgpu::diag::warn("[MGPU][P1.0c] no GPU 1 device - probe skipped");
        return false;
    }
    if (queue == nullptr)
    {
        // Not fatal to the bridge, but fatal to this probe: without a queue
        // the command list cannot be drained, and releasing an undrained
        // list is the fault this task exists to remove. Refusing is the
        // correct outcome, and it is loud.
        mgpu::diag::warn("[MGPU][P1.0c] no GPU 1 command queue - probe skipped (the probe must be "
                         "able to execute the list CreateFeature records into; without that it "
                         "would have to release an undrained list, which is the exact fault P1.0b "
                         "removes)");
        return false;
    }

    // ---- 1. the modules ----
    //
    // P1.0 expected _nvngx.dll to be unreachable without a third-party
    // add-on force-loading it, because it lives in the driver store and not
    // on the loader search path. The rig disproved that: it was ALREADY
    // RESIDENT with only this add-on in the process, no DLSS add-on
    // anywhere in the log. So GetModuleHandleW is the normal path here, not
    // the lucky one, and the reference add-on is no longer a deployment
    // requirement for this probe.
    //
    // A locator remains a portability question rather than a blocker. If it
    // is ever needed, P0_RECORD.md section 09 records the registry route -
    // HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath - which
    // needs no new link library and no SetupAPI.
    //
    // nvngx_dlssnr.dll sits beside dxgi.dll in the application directory,
    // so the ordinary search finds it with no locator at all.
    //
    // Neither module is ever freed. The core may be in use by the game, and
    // the snippet is NGX's to manage; dropping a reference we did not
    // establish would be worse than keeping one we did.
    ngx_modules mods;
    const char *core_how = "not found";
    const char *snip_how = "not found";

    mods.core = GetModuleHandleW(L"_nvngx.dll");
    if (mods.core != nullptr)
    {
        core_how = "already resident";
    }
    else
    {
        mods.core = LoadLibraryW(L"_nvngx.dll");
        core_how = (mods.core != nullptr) ? "loaded" : "not found";
    }

    // ---- V45: THE PRIVATE COPY FIRST. SEE nr_load_private_snippet. ----
    char snip_where[MAX_PATH * 2] = {};
    const bool beside_exe = nr_snippet_is_beside_exe();
    mods.snippet = nr_load_private_snippet(snip_where, sizeof snip_where);
    if (mods.snippet != nullptr)
    {
        snip_how = "PRIVATE INSTANCE from mgpu\\ - nothing else in this process has it";
    }
    else
    {
        // The 0.1.0 layout. It works, right up until the title's own
        // Streamline finds the same file and binds NGX to the game's adapter
        // first - which is the crash, not a risk of one.
        mods.snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
        if (mods.snippet != nullptr) snip_how = "already resident (SHARED with the game)";
        else
        {
            mods.snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
            snip_how = (mods.snippet != nullptr) ? "loaded beside the exe (SHARED)" : "not found";
        }
    }

    snprintf(line, sizeof line,
             "[MGPU][P1.0c] ngx modules: _nvngx.dll=0x%p (%s) nvngx_dlssnr.dll=0x%p (%s)%s%s",
             (void *)mods.core, core_how, (void *)mods.snippet, snip_how,
             (snip_where[0] != '\0') ? " path=" : "", snip_where);
    mgpu::diag::info(line);

    // THE LINE TO READ FIRST IN ANY 0.2.0 BUG REPORT.
    if (beside_exe)
        mgpu::diag::warn(
            "[MGPU][P1.0c] nvngx_dlssnr.dll IS ALSO LOADED BESIDE THE EXECUTABLE. The title's "
            "Streamline scans for nvngx_*.dll next to the exe and initialises what it finds, and "
            "that binds NGX's cubin layer to the GAME's adapter before this add-on's thread even "
            "exists - which is what makes CreateFeature fault on the second adapter. MOVE "
            "nvngx_dlssnr.dll INTO THE mgpu FOLDER beside the add-on and delete the copy next to "
            "the exe. This is the 0.1.0 layout and it is the bug.");
    else if (mods.snippet != nullptr)
        mgpu::diag::info(
            "[MGPU][P1.0c] no copy of nvngx_dlssnr.dll beside the executable - the title cannot "
            "load it, so this add-on holds the only instance and NGX binds to the adapter WE "
            "pass. This is the correct layout.");

    if (mods.core == nullptr && mods.snippet == nullptr)
    {
        mgpu::diag::warn("[MGPU][P1.0c] PROBE FAILED at module - neither _nvngx.dll nor "
                         "nvngx_dlssnr.dll is reachable. On this rig _nvngx.dll has always been "
                         "resident already, so this line means something changed in the driver or "
                         "the process, not that a DLSS add-on is missing. Bridge continues.");
        return false;
    }

    // The snippet is what P1.0b routes CreateFeature to. Without it there is
    // nothing to route to, and calling the core again would just reproduce
    // P1.0's 0xBAD0000B - a run that costs a rig cycle and answers nothing.
    if (mods.snippet == nullptr)
    {
        // V45. THE INSTRUCTION CHANGED, AND THE OLD ONE IS WHAT CAUSED THE
        // CRASH. "Copy it into the deploy directory" is exactly how the title
        // ends up loading it first.
        mgpu::diag::warn("[MGPU][P1.0c] PROBE FAILED at module - nvngx_dlssnr.dll was not found. "
                         "PUT IT IN THE mgpu FOLDER BESIDE THE ADD-ON, i.e. "
                         "<game>\\bin\\x64\\mgpu\\nvngx_dlssnr.dll, and NOT beside the "
                         "executable. Next to the exe the title's own Streamline loads it first "
                         "and binds NGX to the game's GPU, which is what makes the neural stage "
                         "fault on the second card. Bridge continues.");
        return false;
    }

    // ---- 2. the entry points ----
    //
    // Resolved and logged before any of them is called: a missing export is
    // reported by name, not discovered as a crash.
    char w_init[48]{}, w_init_ext[48]{}, w_caps[48]{}, w_create[48]{};
    char w_release[48]{}, w_destroy[48]{}, w_shutdown[48]{}, w_evaluate[48]{};

    ngx_pf_init            p_init     = (ngx_pf_init)           ngx_resolve(mods, "NVSDK_NGX_D3D12_Init",                    ngx_prefer::core,    w_init,     sizeof w_init);
    ngx_pf_init_ext        p_init_ext = (ngx_pf_init_ext)       ngx_resolve(mods, "NVSDK_NGX_D3D12_Init_Ext",                ngx_prefer::core,    w_init_ext, sizeof w_init_ext);
    ngx_pf_get_cap_params  p_caps     = (ngx_pf_get_cap_params) ngx_resolve(mods, "NVSDK_NGX_D3D12_GetCapabilityParameters", ngx_prefer::core,    w_caps,     sizeof w_caps);
    ngx_pf_destroy_params  p_destroy  = (ngx_pf_destroy_params) ngx_resolve(mods, "NVSDK_NGX_D3D12_DestroyParameters",       ngx_prefer::core,    w_destroy,  sizeof w_destroy);
    ngx_pf_shutdown1       p_shutdown = (ngx_pf_shutdown1)      ngx_resolve(mods, "NVSDK_NGX_D3D12_Shutdown1",               ngx_prefer::core,    w_shutdown, sizeof w_shutdown);

    // The two that P1.0b moves. Preferring the snippet is the whole change.
    ngx_pf_create_feature  p_create   = (ngx_pf_create_feature) ngx_resolve(mods, "NVSDK_NGX_D3D12_CreateFeature",           ngx_prefer::snippet, w_create,   sizeof w_create);
    ngx_pf_release_feature p_release  = (ngx_pf_release_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_ReleaseFeature",          ngx_prefer::snippet, w_release,  sizeof w_release);

    // P1.1 calls this one. Snippet-preferred like create and release - the
    // three feature entry points must come from the same module.
    ngx_pf_evaluate_feature p_evaluate = (ngx_pf_evaluate_feature)
        ngx_resolve(mods, "NVSDK_NGX_D3D12_EvaluateFeature", ngx_prefer::snippet,
                    w_evaluate, sizeof w_evaluate);

    snprintf(line, sizeof line,
             "[MGPU][P1.0c] ngx exports: Init=%s Init_Ext=%s GetCapabilityParameters=%s "
             "DestroyParameters=%s Shutdown1=%s CreateFeature=%s ReleaseFeature=%s "
             "EvaluateFeature=%s",
             w_init, w_init_ext, w_caps, w_destroy, w_shutdown, w_create, w_release, w_evaluate);
    mgpu::diag::info(line);
    (void)p_shutdown;   // P1.0c no longer calls it - see the teardown comment

    // ---- 2b. the SNIPPET's own entry points ----
    //
    // P1.0b initialised the core and then asked the snippet to create a
    // feature. The snippet answered FAIL_NotInitialized, about itself. So
    // the snippet has a session of its own and nobody had opened it.
    //
    // Resolved strictly from nvngx_dlssnr.dll: both modules export these
    // names, and a fallback to the core here would re-run P1.0b while
    // looking like progress. Three init spellings are probed because the
    // guide names Init_Ext2 among the Vulkan snippet exports and we do not
    // know which of them the D3D12 snippet carries - the log answers that
    // rather than a guess doing so.
    char s_init[8]{}, s_init_ext[8]{}, s_init_ext2[8]{}, s_populate[8]{};

    ngx_pf_init     ps_init      = (ngx_pf_init)     ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init",      s_init,      sizeof s_init);
    ngx_pf_init_ext ps_init_ext  = (ngx_pf_init_ext) ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init_Ext",  s_init_ext,  sizeof s_init_ext);
    FARPROC         ps_init_ext2 =                   ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init_Ext2", s_init_ext2, sizeof s_init_ext2);

    ngx_pf_populate_params ps_populate = (ngx_pf_populate_params)
        ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl",
                           s_populate, sizeof s_populate);

    snprintf(line, sizeof line,
             "[MGPU][P1.0c] snippet-only exports: Init=%s Init_Ext=%s Init_Ext2=%s "
             "PopulateParameters_Impl=%s",
             s_init, s_init_ext, s_init_ext2, s_populate);
    mgpu::diag::info(line);
    (void)ps_init_ext2;   // presence is the finding; not called this task

    if (ps_init == nullptr && ps_init_ext == nullptr)
    {
        mgpu::diag::warn("[MGPU][P1.0c] PROBE FAILED at snippet exports - nvngx_dlssnr.dll offers "
                         "no Init or Init_Ext to open its own session with. The Init_Ext2 line "
                         "above says whether a third spelling exists; if it does, that is the next "
                         "thing to call and its signature has to come from the header, not from "
                         "this one. Bridge continues.");
        return false;
    }

    if ((p_init == nullptr && p_init_ext == nullptr) ||
        p_caps == nullptr || p_create == nullptr || p_evaluate == nullptr ||
        p_release == nullptr || p_destroy == nullptr)
    {
        mgpu::diag::warn("[MGPU][P1.0c] PROBE FAILED at exports - see the line above for which "
                         "names were missing. Bridge continues.");
        return false;
    }

    // ---- 3. the application data path ----
    //
    // The add-on's own directory: it is the deploy directory, and ReShade
    // writes its log and ini there, so it is known writable - which is what
    // NGX asks of this path. GetModuleFileNameW on our own HMODULE, with
    // the filename cut off at the last backslash (the trailing separator is
    // kept).
    wchar_t data_path[MAX_PATH] = {};
    {
        wchar_t mod_path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(mgpu::module_handle(), mod_path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.0c] GetModuleFileNameW failed (n=%lu GetLastError=%lu) - "
                     "InApplicationDataPath will be empty; the init result code says what the "
                     "driver makes of that",
                     (unsigned long)n, (unsigned long)GetLastError());
            mgpu::diag::warn(line);
        }
        else
        {
            size_t cut = 0;
            for (size_t i = 0; i + 1 < (size_t)n; ++i)
                if (mod_path[i] == L'\\')
                    cut = i + 1;
            for (size_t i = 0; i < cut; ++i)
                data_path[i] = mod_path[i];
        }
    }
    char data_path_n[400] = "";
    WideCharToMultiByte(CP_UTF8, 0, data_path, -1, data_path_n,
                        (int)sizeof data_path_n, nullptr, nullptr);

    // ---- state the teardown has to unwind ----
    bool init_ok = false;
    NVSDK_NGX_Parameter *params = nullptr;
    ID3D12CommandAllocator *palloc = nullptr;
    ID3D12GraphicsCommandList *pcmd = nullptr;
    ID3D12Fence *pfence = nullptr;
    HANDLE pevent = nullptr;
    NVSDK_NGX_Handle *handle = nullptr;
    bool list_open = false;   // pcmd exists and has not been Closed yet

    // P1.1: the local evaluate set. All on the GPU 1 device, nothing shared,
    // nothing crossing the bus - the whole point of doing evaluation before
    // transit is that this milestone needs no bus at all.
    ID3D12Resource *tex_color = nullptr;    // NR input   (the uploaded pattern)
    ID3D12Resource *tex_depth = nullptr;    // cleared depth, only if NR demands one
    ID3D12Resource *tex_mvec = nullptr;     // zero motion
    ID3D12Resource *buf_upload = nullptr;
    ID3D12Resource *buf_read_in = nullptr;

    // P1.2: THREE outputs, not one. A and C are evaluated at the same low
    // intensity and B at a high one, in the order A -> B -> C. Two outputs
    // would only show that something changed between evaluates; the third
    // is what separates "intensity changed the image" from "the second
    // evaluate differs because the first one ran". See the verdict block.
    // P1.4: a CPU copy of the local NR result for output A, taken before the
    // readback buffers are unmapped. Freed at the end of this function.
    unsigned char *ref_local = nullptr;

    static const int NOUT = 3;
    ID3D12Resource *tex_out[NOUT] = {};     // RT|UAV - never the input
    ID3D12Resource *buf_read_out[NOUT] = {};

    // Unwinds in reverse order of construction and logs every NGX result it
    // produces - a teardown call is an NGX call too, and acceptance item 2
    // ("every NGX call's result reaches the log") holds all the way down.
    //
    // Every step announces itself BEFORE it runs. A call that faults cannot
    // log its own result, so the last line in the log is the only evidence
    // of where the process died - P1.0's teardown crashed between
    // DestroyParameters and the next NGX line and left us guessing between
    // four candidates. That is what these lines cost one string each to
    // avoid, and they are not tidying-up material.
    auto teardown = [&](const char *failed_step)
    {
        // ---- 1. DRAIN FIRST, RELEASE NOTHING BEFORE IT IS DONE ----
        //
        // CreateFeature records initialisation work into the list it is
        // handed. P1.0 closed and released that list without ever executing
        // it, which leaves the GPU referencing freed resources - the
        // documented device-lost pattern, and the leading explanation for
        // P1.0's teardown crash. One code path, not branched on whether
        // CreateFeature succeeded: executing a list NGX recorded nothing
        // into is harmless, and executing one it half-recorded and then
        // abandoned is precisely the case that must not be skipped.
        bool drained = true;
        if (list_open)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: Close(probe command list) ...");
            const HRESULT hr = pcmd->Close();
            list_open = false;
            snprintf(line, sizeof line, "[MGPU][P1.0c] teardown: Close hr=0x%08X", (unsigned)hr);
            mgpu::diag::info(line);
            if (FAILED(hr))
            {
                drained = false;
            }
            else
            {
                mgpu::diag::info("[MGPU][P1.0c] teardown: ExecuteCommandLists(probe list) ...");
                ID3D12CommandList *const lists[1] = { pcmd };
                queue->ExecuteCommandLists(1, lists);

                mgpu::diag::info("[MGPU][P1.0c] teardown: Signal + wait for the probe list ...");
                const HRESULT shr = queue->Signal(pfence, 1);
                if (FAILED(shr))
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P1.0c] teardown: Signal hr=0x%08X - cannot confirm the list "
                             "executed", (unsigned)shr);
                    mgpu::diag::error(line);
                    drained = false;
                }
                else
                {
                    pfence->SetEventOnCompletion(1, pevent);
                    // Bounded, unlike the present loop's INFINITE: this wait
                    // is on work a driver recorded, and a hang here would
                    // take the bridge thread with it. Ten seconds is far
                    // beyond the 1.16 s CreateFeature took on the reference
                    // run.
                    const DWORD wr = WaitForSingleObject(pevent, 10000);
                    if (wr == WAIT_OBJECT_0)
                    {
                        mgpu::diag::info("[MGPU][P1.0c] teardown: probe list executed and drained");
                    }
                    else
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][P1.0c] teardown: fence wait returned 0x%08X "
                                 "(GetLastError=%lu) - the probe list may still be in flight",
                                 (unsigned)wr, (unsigned long)GetLastError());
                        mgpu::diag::error(line);
                        drained = false;
                    }
                }
            }
        }

        if (!drained)
        {
            // Deliberate leak, and the only safe choice. Releasing an NGX
            // feature or a command list whose work is still in flight is the
            // fault this whole function was rewritten to avoid; leaking a
            // list, an allocator, a fence and one NGX session once per
            // launch is bounded and harmless by comparison. Say so plainly,
            // because a silent leak here would be indistinguishable from a
            // clean teardown in the log.
            mgpu::diag::error("[MGPU][P1.0c] teardown: ABANDONED - the probe's GPU work could not "
                              "be confirmed complete, so nothing is released. The command list, "
                              "allocator, fence, NGX parameters and NGX session are leaked on "
                              "purpose; releasing them now is the exact device-lost fault this "
                              "path exists to prevent. Bridge continues.");
            if (failed_step != nullptr)
            {
                snprintf(line, sizeof line,
                         "[MGPU][P1.0c] PROBE FAILED at %s - see the result code above. "
                         "Bridge continues.", failed_step);
                mgpu::diag::warn(line);
            }
            return;
        }

        // ---- 2. the NGX objects, now that the GPU is idle ----
        if (handle != nullptr)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: ReleaseFeature ...");
            const NVSDK_NGX_Result r = p_release(handle);
            snprintf(line, sizeof line, "[MGPU][P1.0c] teardown: ReleaseFeature result=0x%08X (%s)",
                     (unsigned)r, ngx_result_name(r));
            mgpu::diag::info(line);
            handle = nullptr;
        }
        if (params != nullptr)
        {
            // P5.2 / DEFECT C. GetCapabilityParameters returns the CORE's
            // block, not a per-caller one, so destroying it here destroys it
            // for every other holder in the process. When the P4.1 stream is
            // live it is such a holder, and this call is what turned its
            // output into wrong colours while leaving every transport counter
            // clean. The block is deliberately never freed in that case - the
            // NGX session is already kept open for the process lifetime for
            // the same reason (see the teardown note above), so this leaks
            // nothing that was not already held on purpose.
            if (stream_nr_live())
            {
                mgpu::diag::warn("[MGPU][P1.0c] teardown: DestroyParameters SKIPPED - the P4.1 "
                                 "stream holds the same capability block (GetCapabilityParameters "
                                 "returns the core's block, not a per-caller one). Destroying it "
                                 "here would pull the parameter map out from under a running "
                                 "neural stage. See P1_INSTRUMENT defect C.");
                params = nullptr;
            }
            else
            {
                // The capability map is driver-allocated and the header states
                // it must be freed this way - never with delete or free.
                mgpu::diag::info("[MGPU][P1.0c] teardown: DestroyParameters ...");
                const NVSDK_NGX_Result r = p_destroy(params);
                snprintf(line, sizeof line,
                         "[MGPU][P1.0c] teardown: DestroyParameters result=0x%08X (%s)",
                         (unsigned)r, ngx_result_name(r));
                mgpu::diag::info(line);
                params = nullptr;
            }
        }

        // ---- 2b. the P1.1 local resources ----
        //
        // After ReleaseFeature and after the drain: NR held these while the
        // feature existed, and the drain above is what guarantees the GPU
        // is no longer reading them.
        for (int i = NOUT - 1; i >= 0; --i)
        {
            if (buf_read_out[i] != nullptr) { buf_read_out[i]->Release(); buf_read_out[i] = nullptr; }
            if (tex_out[i]      != nullptr) { tex_out[i]->Release();      tex_out[i]      = nullptr; }
        }
        if (buf_read_in  != nullptr) { buf_read_in->Release();  buf_read_in  = nullptr; }
        if (buf_upload   != nullptr) { buf_upload->Release();   buf_upload   = nullptr; }
        if (tex_mvec     != nullptr) { tex_mvec->Release();     tex_mvec     = nullptr; }
        if (tex_depth    != nullptr) { tex_depth->Release();    tex_depth    = nullptr; }
        if (tex_color    != nullptr) { tex_color->Release();    tex_color    = nullptr; }

        // ---- 3. the D3D12 objects, reverse creation order ----
        if (pevent != nullptr)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: CloseHandle(probe fence event) ...");
            CloseHandle(pevent);
            pevent = nullptr;
        }
        if (pfence != nullptr)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: Release(probe fence) ...");
            pfence->Release();
            pfence = nullptr;
        }
        if (pcmd != nullptr)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: Release(probe command list) ...");
            pcmd->Release();
            pcmd = nullptr;
        }
        if (palloc != nullptr)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: Release(probe allocator) ...");
            palloc->Release();
            palloc = nullptr;
        }

        // ---- 4. the NGX session is KEPT ----
        //
        // P1.0b's teardown crashed inside NVSDK_NGX_D3D12_Shutdown1. It was
        // localised by announcing each step before making the call: every
        // other step logged its own completion, and Shutdown1 logged its
        // announcement and nothing after.
        //
        // The fix is not to make that call work. P1.1 keeps NGX
        // initialised for the process lifetime anyway, so a probe that
        // opens a session and closes it is satisfying a P1.0-shaped
        // requirement that the next milestone deletes. One NGX session per
        // launch, reclaimed by the OS at process exit, is the intended end
        // state - and every crash observed in this project so far has been
        // in teardown, none in use.
        //
        // The device is still ours and is still released by shutdown()
        // below in the ordinary way; only NGX's session outlives the probe.
        if (init_ok)
        {
            mgpu::diag::info("[MGPU][P1.0c] teardown: NGX session KEPT OPEN on purpose - "
                             "Shutdown1 is not called (it is where P1.0b faulted, and P1.1 needs "
                             "the session anyway). One session per launch, reclaimed at process "
                             "exit.");
            init_ok = false;
        }

        mgpu::diag::info("[MGPU][P1.0c] teardown: complete - D3D12 objects released, NGX session "
                         "intentionally retained");

        if (failed_step != nullptr)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.0c] PROBE FAILED at %s - see the result code above. "
                     "Bridge continues.",
                     failed_step);
            mgpu::diag::warn(line);
        }
    };

    // ---- 4. init, and the log sink ----
    //
    // P1.0 preferred Init_Ext. P1.0b prefers the APPLICATION form, Init,
    // for one reason: it is the only one that takes an
    // NVSDK_NGX_FeatureCommonInfo, and that struct is the only place a log
    // callback can be handed to NGX. Init_Ext takes a parameter block in
    // that argument position and cannot carry one.
    //
    // The fallback to Init_Ext is kept because P1.0 proved it works, but a
    // run that takes it has NO callback installed - so the absence of
    // [MGPU][P1.0c][NGX] lines would mean "we never asked" rather than "NGX
    // had nothing to say". The log says which form ran, so the two are
    // never confused.
    {
        NVSDK_NGX_FeatureCommonInfo common{};
        common.LoggingInfo.LoggingCallback = ngx_log_callback;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        // false, not true: the driver's own sinks stay enabled. The registry
        // LogLevel under HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore is
        // what opens them, and suppressing them would throw away the second
        // copy for no gain.
        common.LoggingInfo.DisableOtherLoggingSinks = false;

        NVSDK_NGX_Result r;
        const char *which;
        bool callback_installed;
        if (p_init != nullptr)
        {
            which = "Init";
            callback_installed = true;
            r = p_init(0ULL, data_path, dev, &common, NVSDK_NGX_Version_API);
        }
        else
        {
            which = "Init_Ext (FALLBACK - no log callback installed)";
            callback_installed = false;
            r = p_init_ext(0ULL, data_path, dev, NVSDK_NGX_Version_API, nullptr);
        }
        // The inputs, not just the verdict: app id and data path are the
        // two values most likely to be what the driver objects to, and they
        // were chosen rather than derived.
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] %s: result=0x%08X (%s) device=0x%p luid=%08lX-%08lX "
                 "app_id=0 sdk_version=0x%08X log_callback=%s data_path=\"%s\"",
                 which, (unsigned)r, ngx_result_name(r), (void *)dev,
                 (unsigned long)luid.HighPart, (unsigned long)luid.LowPart,
                 (unsigned)NVSDK_NGX_Version_API,
                 callback_installed ? "installed(VERBOSE)" : "NOT INSTALLED",
                 data_path_n);
        mgpu::diag::info(line);

        // P3.0. THE SECOND INIT IS EXPECTED NOT TO BE A FIRST INIT. The
        // session opened by the startup call is deliberately never shut down
        // (see the teardown note - Shutdown1 is where P1.0b faulted), so by
        // the time a captured frame arrives NGX has been initialised in this
        // process for minutes. What a re-Init returns in that state is not
        // documented anywhere we trust, so this does not guess: it logs the
        // result and CONTINUES, because the session it would be establishing
        // is known to be open already. If the code turns out to matter, it is
        // in the line above and the failure will surface at CreateFeature
        // with its own result - which is a better place to read it than an
        // abort here on an assumption.
        if (P3 && r != NVSDK_NGX_Result_Success)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P3.0] re-Init returned 0x%08X (%s) and is being IGNORED: this is "
                     "the second Init of a session that was never shut down. Continuing to "
                     "CreateFeature, which is where a genuinely broken session will say so.",
                     (unsigned)r, ngx_result_name(r));
            mgpu::diag::warn(line);
        }
        else if (r != NVSDK_NGX_Result_Success)
        {
            // FAIL_OutOfDate here is a KNOWN INTERMITTENT and almost
            // certainly not a defect in this add-on. Seen twice: once with
            // Generic Depth disabled and once with it enabled, which rules
            // out the .ini theory. Both times NGX's own log carried
            // "NGXInitValidateSnippets: installed NGX API is older than the
            // one used by client application" as its FIRST and ONLY line -
            // no telemetry blocks at all - so snippet validation gates
            // everything downstream of it.
            //
            // The suspect is OTA. nvngx_update.exe runs on EVERY launch,
            // failed ones included, and rewrites
            // C:\ProgramData\NVIDIA\NGX\models. On 2026-09-03 the failure
            // went PERSISTENT: 14:07 pass, 14:17 pass, 14:36 fail, 14:40
            // fail, with nothing changed between the last two. An earlier
            // version of this message said to relaunch because it had
            // recovered once; that was wrong and it is retracted here.
            // Streamline is eliminated - 2.14.0.0 was live in the passing
            // 14:17 run.
            //
            // Say all of this here rather than leaving a bare code, because
            // the correct response is to fix the environment, not to debug
            // this add-on.
            if (r == NVSDK_NGX_Result_FAIL_OutOfDate)
                mgpu::diag::warn(
                    "[MGPU][P1.0c] FAIL_OutOfDate at init is an ENVIRONMENT fault, not a defect "
                    "in this add-on - the same call succeeded repeatedly on this rig with this "
                    "binary. Confirm in nvngx.log: its first line will be "
                    "\"NGXInitValidateSnippets: installed NGX API is older than the one used by "
                    "client application\" with NO telemetry blocks after it. "
                    "FIX, reproduced on the rig 2026-09-03: OPEN THE NVIDIA APP, then relaunch "
                    "the game. Every earlier clearing event was an instance of this - an app "
                    "update, a reboot, a settings change - each of which restarted or refreshed "
                    "that app. Relaunching the game ALONE does not reliably clear it. "
                    "The OTA model store is NOT the cause and is exonerated: failing runs still "
                    "parse C:\\ProgramData\\NVIDIA\\NGX\\models and MapProjectId succeeds in them. "
                    "THIS RUN ANSWERS NOTHING ABOUT NGX and no NGX result from it may be "
                    "recorded - but it does NOT void probes that never touch NGX. P1.3 builds "
                    "its own devices and produced a valid cross-adapter transit result on a "
                    "launch that failed here. Void the NGX portion, not the launch.");
            teardown(which);
            return false;
        }
        init_ok = true;
    }

    // ---- 5. the capability parameter map ----
    {
        const NVSDK_NGX_Result r = p_caps(&params);
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] GetCapabilityParameters: result=0x%08X (%s) params=0x%p",
                 (unsigned)r, ngx_result_name(r), (void *)params);
        mgpu::diag::info(line);
        if (r != NVSDK_NGX_Result_Success || params == nullptr)
        {
            params = nullptr;
            teardown("GetCapabilityParameters");
            return false;
        }
    }

    // ---- 5b. open the SNIPPET's session ----
    //
    // This is P1.0c's whole point. The block from the core is passed in
    // because the snippet-build init takes a parameter block where the
    // application form takes an NVSDK_NGX_FeatureCommonInfo * - a
    // difference in TYPE at the same argument position, which is why
    // Init_Ext's typedef is written out by hand in this file and not
    // borrowed from the application-facing declaration.
    //
    // Init_Ext is preferred over Init for exactly that reason: it is the
    // form that carries the parameter block. A snippet that only exports
    // Init gets Init with a null common-info, and the log says which ran.
    {
        NVSDK_NGX_Result r;
        const char *which;
        if (ps_init_ext != nullptr)
        {
            which = "snippet Init_Ext";
            r = ps_init_ext(0ULL, data_path, dev, NVSDK_NGX_Version_API, params);
        }
        else
        {
            which = "snippet Init";
            r = ps_init(0ULL, data_path, dev, nullptr, NVSDK_NGX_Version_API);
        }
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] %s: result=0x%08X (%s) device=0x%p params=0x%p",
                 which, (unsigned)r, ngx_result_name(r), (void *)dev, (void *)params);
        mgpu::diag::info(line);

        // A failure here is NOT fatal to the probe, deliberately. The point
        // of this run is to learn what the snippet says at each step, and
        // stopping now would throw away the two results below - which are
        // the ones nobody has ever seen. CreateFeature will repeat
        // FAIL_NotInitialized if this did not take, and that is a clean
        // answer rather than a lost run.
        if (r != NVSDK_NGX_Result_Success)
            mgpu::diag::warn("[MGPU][P1.0c] snippet init did not succeed - continuing anyway so "
                             "PopulateParameters_Impl and CreateFeature still report. Expect "
                             "FAIL_NotInitialized below if the session really is closed.");
    }

    // ---- 5c. let the snippet register its own parameter keys ----
    //
    // The alternative is transcribing DLSSNR.* key names by hand, which
    // fails SILENTLY when a name is wrong: the parameter is set, nothing
    // reads it, and the feature runs on defaults. Asking the feature to
    // name its own keys removes that entire class of error.
    //
    // Called AFTER the snippet init above, on the theory that a routine
    // belonging to an uninitialised session will refuse like CreateFeature
    // did. If this returns FAIL_NotInitialized while the init above
    // returned Success, that ordering assumption is wrong and the two
    // blocks swap - which is a one-line change and a settled question
    // rather than a guess either way.
    if (ps_populate != nullptr)
    {
        const NVSDK_NGX_Result r = ps_populate(params);
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] PopulateParameters_Impl: result=0x%08X (%s) params=0x%p",
                 (unsigned)r, ngx_result_name(r), (void *)params);
        mgpu::diag::info(line);
    }
    else
    {
        mgpu::diag::warn("[MGPU][P1.0c] PopulateParameters_Impl not exported under that D3D12 name "
                         "- the parameter block reaches CreateFeature carrying only what we set. "
                         "That is a finding, not a fault; see the snippet-only exports line.");
    }

    // ---- 5d. the size, under BOTH key namespaces ----
    //
    // P1.0c's first run got all the way through: snippet init Success,
    // PopulateParameters_Impl Success, CreateFeature 185 ms - long enough
    // that the snippet's own log shows it building the network on GPU 1,
    // 153 tensors, a 140.9 MB weight heap and a 296.9 MB working set,
    // before unwinding with FAIL_InvalidParameter. The reason is in that
    // log, in one line:
    //
    //     DLSSNR: CreateFeature begin requested resolution 0x0 (network 0x0)
    //
    // We had set NVSDK_NGX_Parameter_Width / _Height, which are the generic
    // keys "Width" and "Height". The feature reads its OWN namespaced keys,
    // DLSSNR.Width and DLSSNR.Height, saw nothing, and built a 0x0 network.
    //
    // Both namespaces are set, in this order, for a reason: the generic
    // pair is what the header documents as required for every feature and
    // may still be read by the core, and the namespaced pair is what this
    // particular feature actually looks up. Setting both costs nothing and
    // removes the question.
    //
    // The names are string literals rather than header constants because
    // the DLSSNR.* keys appear in no public header - they belong to the
    // snippet. A wrong name here fails SILENTLY: the parameter is set,
    // nothing reads it, and the feature runs on defaults. That is why the
    // resolution shows up in the snippet's log, and why that log is the
    // thing to check first if this still comes back 0x0.
    params->Set(NVSDK_NGX_Parameter_Width, (unsigned int)width);
    params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)height);
    params->Set("DLSSNR.Width", (unsigned int)width);
    params->Set("DLSSNR.Height", (unsigned int)height);

    snprintf(line, sizeof line,
             "[MGPU][P1.0c] size set: Width/Height and DLSSNR.Width/DLSSNR.Height = %ux%u "
             "(check the snippet log's \"requested resolution\" line - it is the only place "
             "that says whether the namespaced keys were read)",
             (unsigned)width, (unsigned)height);
    mgpu::diag::info(line);

    // ---- 6. a private command list, allocator and fence ----
    //
    // Private rather than the present chain's: CreateFeature wants a list
    // that is open and recording, the chain's list is closed between
    // frames, and this path never Resets anything - a list straight from
    // CreateCommandList is already open.
    //
    // The FENCE is private too, and that is deliberate. The chain's fence
    // belongs to present_frame, which advances it every frame; borrowing it
    // for a one-shot wait would leave the loop's own accounting to be
    // reasoned about. A fence of our own starts at 0, is signalled to 1
    // exactly once, and is released with everything else.
    {
        HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&palloc));
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] probe CreateCommandAllocator hr=0x%08X", (unsigned)hr);
        mgpu::diag::info(line);
        if (FAILED(hr))
        {
            palloc = nullptr;
            teardown("CreateCommandAllocator");
            return false;
        }

        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, palloc,
                                    nullptr, IID_PPV_ARGS(&pcmd));
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] probe CreateCommandList hr=0x%08X (open and recording, not reset)",
                 (unsigned)hr);
        mgpu::diag::info(line);
        if (FAILED(hr))
        {
            pcmd = nullptr;
            teardown("CreateCommandList");
            return false;
        }
        // From here on the list exists and is recording, so every exit runs
        // the drain path in teardown.
        list_open = true;

        hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pfence));
        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] probe CreateFence hr=0x%08X", (unsigned)hr);
        mgpu::diag::info(line);
        if (FAILED(hr))
        {
            pfence = nullptr;
            teardown("CreateFence");
            return false;
        }

        pevent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (pevent == nullptr)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.0c] probe CreateEventW failed (GetLastError=%lu)",
                     (unsigned long)GetLastError());
            mgpu::diag::error(line);
            teardown("CreateEventW");
            return false;
        }
    }

    // ---- 7. the feature ----
    //
    // This is the question. Everything above is plumbing.
    //
    // p_create points at the SNIPPET when nvngx_dlssnr.dll exports the name
    // (the export line above says which module answered). P1.0 called the
    // core here and got 0xBAD0000B in 0 ms - the core rejecting a feature it
    // has no snippet mapping for. Three outcomes are worth telling apart in
    // the log, and none of them is a defect in this code:
    //
    //   Success        the feature exists on the non-game adapter.
    //   0xBAD0000B     unchanged - the routing was not the difference.
    //   0xBAD00002     FAIL_PlatformError: the snippet's caller check
    //                  rejected us, so the module rename did not satisfy it.
    //   0xBAD0000C     FAIL_OutOfDate: a version gate - which also proves
    //                  the feature id is right.
    {
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const NVSDK_NGX_Result r =
            p_create(pcmd, NVSDK_NGX_Feature_Reserved18, params, &handle);
        QueryPerformanceCounter(&t1);

        const double ms = (f.QuadPart > 0)
            ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart)
            : 0.0;

        snprintf(line, sizeof line,
                 "[MGPU][P1.0c] CreateFeature(Reserved18) via %s: result=0x%08X (%s) handle=0x%p "
                 "%ux%u elapsed=%.0fms",
                 w_create, (unsigned)r, ngx_result_name(r), (void *)handle,
                 (unsigned)width, (unsigned)height, ms);
        mgpu::diag::info(line);

        if (r != NVSDK_NGX_Result_Success)
        {
            handle = nullptr;
            teardown("CreateFeature");
            return false;
        }
    }

    // =================================================================
    // P1.2 - are the tuning parameters LIVE per evaluate, or baked at
    //        CreateFeature?
    //
    // P1.1 closed the execution question: the model runs on GPU 1 and
    // rewrites every pixel. It left one thing open, and the snippet named
    // it itself:
    //
    //     DLSSNR: PollRuntimeParams - callback is NULL (core did not set it)
    //
    // NR expects a runtime-parameter callback that the core normally
    // installs. On our path it is absent. So the echoed intensity=0.84
    // proves the value was READ; it does not prove it was APPLIED. If
    // tuning is frozen at CreateFeature, every quality change costs a
    // ~220 ms feature rebuild, and that is an architectural constraint
    // rather than a detail.
    //
    // THE CONTROL. Two evaluates would only show that something changed
    // between them - NR keeps a temporal history (dlssnr_prev_output), so
    // the second evaluate could differ from the first for reasons that have
    // nothing to do with intensity. Three evaluates settle it:
    //
    //     A  intensity LOW    C  intensity LOW  (same as A, run last)
    //     B  intensity HIGH
    //
    //     A == B            -> parameters are frozen at create
    //     A != B, A == C    -> intensity is live per evaluate
    //     A != B, A != C    -> history contamination; inconclusive
    //
    // Without C, that third case reads as a pass. DLSSNR.Reset is set on
    // every evaluate to ask NR to discard history, so A == C is what we
    // expect if Reset does what it says - and if it does not, the log says
    // so instead of us believing a wrong answer.
    //
    // Still entirely local to GPU 1. No shared handles, nothing on the bus.
    // =================================================================
    {
        // P3.1: the colour and output textures follow the frame when the
        // caller asks for the native format. The byte comparisons below stay
        // valid either way - both candidate formats are 4 bytes per pixel and
        // every test here is byte-exactness, not colour arithmetic. The
        // per-channel MEAN and MAX figures are the exception: they are only
        // meaningful for R8G8B8A8, because on a packed 10:10:10:2 format they
        // difference bit fields that straddle byte boundaries. On a
        // native-format run, read `differing` and ignore mean/max.
        const bool native_fmt = P3 && ext->native_format;
        const DXGI_FORMAT fmt_color = native_fmt ? (DXGI_FORMAT)ext->dxgi_format
                                                 : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (native_fmt)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P3.1] NATIVE-FORMAT ATTEMPT: building the NR colour and output "
                     "textures as DXGI %u - the game's own format - and feeding the frame "
                     "UNCONVERTED. If every stage below succeeds, the CPU conversion P3.0 paid "
                     "for is unnecessary. If one fails, its result code is the answer, and the "
                     "converted run that follows still delivers this launch's P3.0 result.",
                     ext->dxgi_format);
            mgpu::diag::info(line);
        }
        const DXGI_FORMAT fmt_mvec  = DXGI_FORMAT_R16G16_FLOAT;
        // R32_FLOAT rather than a real depth format: NR reads depth as a
        // plain texture, and a D32_FLOAT resource would need
        // ALLOW_DEPTH_STENCIL, which conflicts with the copy path used to
        // initialise it. P1.1 established depth may be null anyway; this is
        // kept only as the retry path.
        const DXGI_FORMAT fmt_depth = DXGI_FORMAT_R32_FLOAT;

        // The two intensities. Far apart on purpose: if a wide separation
        // produces no difference, a narrow one certainly would not, and a
        // null result is only informative when the input range was generous.
        const float INTENSITY_LO = 0.0f;
        const float INTENSITY_HI = 1.6f;
        const float intensities[NOUT] = { INTENSITY_LO, INTENSITY_HI, INTENSITY_LO };
        const char *labels[NOUT]      = { "A(lo)",      "B(hi)",      "C(lo)" };

        HRESULT hr = make_tex(dev, width, height, fmt_color,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, &tex_color);
        for (int i = 0; i < NOUT && SUCCEEDED(hr); ++i)
            hr = make_tex(dev, width, height, fmt_color,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_out[i]);
        if (SUCCEEDED(hr))
            hr = make_tex(dev, width, height, fmt_mvec,
                          D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_mvec);
        if (SUCCEEDED(hr))
            hr = make_tex(dev, width, height, fmt_depth,
                          D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_depth);

        snprintf(line, sizeof line,
                 "[MGPU][P1.2] resources hr=0x%08X color=0x%p outA=0x%p outB=0x%p outC=0x%p "
                 "mvec=0x%p depth=0x%p %ux%u",
                 (unsigned)hr, (void *)tex_color, (void *)tex_out[0], (void *)tex_out[1],
                 (void *)tex_out[2], (void *)tex_mvec, (void *)tex_depth, width, height);
        mgpu::diag::info(line);
        if (FAILED(hr))
        {
            teardown("P1.2 resource creation");
            return false;
        }

        // ---- footprints: colour, mvec, depth, then one sentinel per output ----
        D3D12_RESOURCE_DESC d_color = tex_color->GetDesc();
        D3D12_RESOURCE_DESC d_mvec  = tex_mvec->GetDesc();
        D3D12_RESOURCE_DESC d_depth = tex_depth->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_color{}, fp_mvec{}, fp_depth{}, fp_out[NOUT]{};
        UINT64 sz_color = 0, sz_mvec = 0, sz_depth = 0, sz_out = 0;
        UINT rows = 0; UINT64 rowb = 0;

        dev->GetCopyableFootprints(&d_color, 0, 1, 0, &fp_color, &rows, &rowb, &sz_color);
        const UINT64 off_mvec = (sz_color + 511) & ~(UINT64)511;
        dev->GetCopyableFootprints(&d_mvec, 0, 1, off_mvec, &fp_mvec, &rows, &rowb, &sz_mvec);
        const UINT64 off_depth = (off_mvec + sz_mvec + 511) & ~(UINT64)511;
        dev->GetCopyableFootprints(&d_depth, 0, 1, off_depth, &fp_depth, &rows, &rowb, &sz_depth);

        // The SENTINEL, one region per output.
        //
        // Without it "the output differs from the input" is not proof of
        // anything: a texture NR never wrote is not black, it is
        // UNINITIALISED, and uninitialised memory differs from the input
        // too. With it, each output is unambiguous - still sentinel means
        // nothing was written there.
        UINT64 off_out[NOUT] = {};
        UINT64 cursor = off_depth + sz_depth;
        for (int i = 0; i < NOUT; ++i)
        {
            off_out[i] = (cursor + 511) & ~(UINT64)511;
            dev->GetCopyableFootprints(&d_color, 0, 1, off_out[i], &fp_out[i], &rows, &rowb, &sz_out);
            cursor = off_out[i] + sz_out;
        }
        const UINT64 upload_bytes = cursor;

        // Chosen so it cannot occur in the pattern: fill_pattern always
        // writes B = 0.70*R, so any pixel with B far above R is ours.
        const unsigned char SENT_R = 0x10, SENT_G = 0x20, SENT_B = 0xF0;

        hr = make_buf(dev, upload_bytes, D3D12_HEAP_TYPE_UPLOAD, &buf_upload);
        if (SUCCEEDED(hr))
            hr = make_buf(dev, sz_color, D3D12_HEAP_TYPE_READBACK, &buf_read_in);
        for (int i = 0; i < NOUT && SUCCEEDED(hr); ++i)
            hr = make_buf(dev, sz_color, D3D12_HEAP_TYPE_READBACK, &buf_read_out[i]);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][P1.2] staging buffers hr=0x%08X", (unsigned)hr);
            mgpu::diag::error(line);
            teardown("P1.2 staging buffers");
            return false;
        }

        {
            unsigned char *mapped = nullptr;
            D3D12_RANGE none{0, 0};   // write-only mapping
            hr = buf_upload->Map(0, &none, reinterpret_cast<void **>(&mapped));
            if (FAILED(hr))
            {
                snprintf(line, sizeof line, "[MGPU][P1.2] upload Map hr=0x%08X", (unsigned)hr);
                mgpu::diag::error(line);
                teardown("P1.2 upload Map");
                return false;
            }
            // Motion and depth are ZERO. Zero flow is the correct "nothing
            // moved" input and it keeps this test about intensity alone.
            // QuantMotion's real 320x180 flow arrives in P1.5, with the
            // subrect and MVecScale values a lower-resolution motion buffer
            // needs.
            memset(mapped + off_mvec, 0, (size_t)(off_out[0] - off_mvec));
            if (P3)
            {
                // The game's frame, row by row. Two pitches are in play and
                // they are not the same number: the source is whatever the
                // capture produced on the game's device, the destination is
                // whatever GetCopyableFootprints chose for GPU 1's texture.
                // Copying by bytes rather than by rows is the classic way to
                // get a sheared image that still "transfers correctly".
                const UINT copy = (ext->row_pitch < fp_color.Footprint.RowPitch)
                                    ? ext->row_pitch : fp_color.Footprint.RowPitch;
                for (UINT y = 0; y < height; ++y)
                    memcpy(mapped + fp_color.Offset + (size_t)y * fp_color.Footprint.RowPitch,
                           ext->pixels + (size_t)y * ext->row_pitch, copy);
            }
            else
                fill_pattern(mapped + fp_color.Offset, width, height,
                             fp_color.Footprint.RowPitch);
            for (int i = 0; i < NOUT; ++i)
                for (UINT y = 0; y < height; ++y)
                {
                    unsigned char *row = mapped + fp_out[i].Offset
                                       + (size_t)y * fp_out[i].Footprint.RowPitch;
                    for (UINT x = 0; x < width; ++x)
                    {
                        unsigned char *p = row + (size_t)x * 4;
                        p[0] = SENT_R; p[1] = SENT_G; p[2] = SENT_B; p[3] = 255;
                    }
                }
            buf_upload->Unmap(0, nullptr);
        }

        // ---- record: uploads -> barriers -> three evaluates -> readbacks ----
        auto copy_in = [&](ID3D12Resource *dst, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp)
        {
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = buf_upload;
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = dst;
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = 0;
            pcmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        };
        copy_in(tex_color, fp_color);
        copy_in(tex_mvec,  fp_mvec);
        copy_in(tex_depth, fp_depth);
        for (int i = 0; i < NOUT; ++i)
            copy_in(tex_out[i], fp_out[i]);

        const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(pcmd, tex_color, D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        barrier(pcmd, tex_mvec,  D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        barrier(pcmd, tex_depth, D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        for (int i = 0; i < NOUT; ++i)
            barrier(pcmd, tex_out[i], D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // ---- the parameters that do not change between evaluates ----
        //
        // Namespaced keys. PopulateParameters_Impl registers these but does
        // not fill them, and per the guide it does NOT register the subrect
        // keys at all - those are set here by hand. The subrect naming has
        // no separator: DLSSNR.ColorSubrectWidth, NOT
        // DLSSNR.Color.SubrectWidth. P1.1 confirmed every spelling below by
        // seeing them echoed in the snippet's own log.
        params->Set("DLSSNR.Color", tex_color);
        params->Set("DLSSNR.MVec",  tex_mvec);

        params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)width);
        params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)height);
        params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)width);
        params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)height);
        params->Set("DLSSNR.MVecSubrectBaseX", 0u);
        params->Set("DLSSNR.MVecSubrectBaseY", 0u);
        params->Set("DLSSNR.MVecSubrectWidth", (unsigned int)width);
        params->Set("DLSSNR.MVecSubrectHeight", (unsigned int)height);

        // Motion is at colour resolution here, so the scale genuinely is
        // 1.0 - not a safe default, a true one. When QuantMotion's 320x180
        // grid arrives this becomes the ratio between the two extents, and
        // leaving it at 1.0 would silently misread every vector.
        params->Set("DLSSNR.MVecScaleX", 1.0f);
        params->Set("DLSSNR.MVecScaleY", 1.0f);
        params->Set("DLSSNR.DepthInverted", 0u);

        // Held CONSTANT across all three evaluates. Only Intensity varies,
        // so that a difference has exactly one possible cause.
        params->Set("DLSSNR.LocalToneStrength", 1.142f);
        params->Set("DLSSNR.LocalStructureStrength", 1.092f);
        params->Set("DLSSNR.SkinStructureStrength", 1.025f);
        params->Set("DLSSNR.UseAutoMask", 1u);

        // P1.1 established depth may be null - it was accepted on the first
        // attempt and the snippet logged Depth=0000000000000000. Kept as a
        // retry path only.
        params->Set("DLSSNR.Depth", (ID3D12Resource *)nullptr);

        // ---- what the machine was doing while this ran ----
        //
        // The probe fires within milliseconds of the present chain being
        // created - which is during game STARTUP. The game is sitting in a
        // menu with almost nothing on screen and is still compiling shaders,
        // and which window owns the foreground at that instant varies from
        // launch to launch: the game, our bridge window, or the desktop.
        //
        // None of that can move this milestone's VERDICT, and that is a
        // property of the design rather than luck: the answer is a byte
        // comparison between three images produced from one deterministic
        // input, on a GPU the game is not using. Focus, occlusion and
        // shader compilation cannot change whether two buffers are equal.
        //
        // It absolutely does contaminate every TIMING here. The flush
        // number below, and the per-evaluate record_cpu values, are taken
        // while another GPU and most of the CPU are busy with startup work.
        // They are logged so a run can be described, NOT so they can be
        // compared - against the reference tool's 14.2 ms, against each
        // other, or against a later build. A real cost figure needs
        // timestamp queries and a settled scene, which is P1_INSTRUMENT.md's
        // job. This line exists so nobody reads these numbers as
        // performance data later without seeing the caveat next to them.
        {
            const HWND fg = GetForegroundWindow();
            DWORD fg_pid = 0;
            if (fg != nullptr)
                GetWindowThreadProcessId(fg, &fg_pid);
            wchar_t cls_w[64] = {};
            if (fg != nullptr)
                GetClassNameW(fg, cls_w, 64);
            char cls[128] = "";
            WideCharToMultiByte(CP_UTF8, 0, cls_w, -1, cls, (int)sizeof cls, nullptr, nullptr);

            const char *owner = "none";
            if (fg != nullptr)
                owner = (fg_pid == GetCurrentProcessId()) ? "this process"
                                                          : "another process (desktop/shell/other)";

            snprintf(line, sizeof line,
                     "[MGPU][P1.2] context: foreground hwnd=0x%p owner=%s class=\"%s\" - "
                     "STARTUP PHASE (menu, shaders still compiling). Timings below are "
                     "contaminated by construction and are NOT comparable; the verdict is a byte "
                     "comparison and is unaffected.",
                     (void *)fg, owner, cls);
            mgpu::diag::info(line);
        }

        LARGE_INTEGER f{}, e0{}, e1{};
        QueryPerformanceFrequency(&f);
        NVSDK_NGX_Result er[NOUT] = {};
        bool used_depth = false;

        for (int i = 0; i < NOUT; ++i)
        {
            params->Set("DLSSNR.Output", tex_out[i]);
            params->Set("DLSSNR.Intensity", intensities[i]);
            // Reset on EVERY evaluate: ask NR to discard dlssnr_prev_output
            // so the three runs are independent. Whether it honours that is
            // exactly what the A/C comparison measures.
            params->Set("DLSSNR.Reset", 1u);

            QueryPerformanceCounter(&e0);
            er[i] = p_evaluate(pcmd, handle, params, nullptr);
            QueryPerformanceCounter(&e1);
            const double ems = (f.QuadPart > 0)
                ? ((double)(e1.QuadPart - e0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
            snprintf(line, sizeof line,
                     "[MGPU][P1.2] EvaluateFeature %s intensity=%.2f depth=%s: result=0x%08X (%s) "
                     "record_cpu=%.2fms",
                     labels[i], intensities[i], used_depth ? "bound" : "null",
                     (unsigned)er[i], ngx_result_name(er[i]), ems);
            mgpu::diag::info(line);

            // One retry, on the first failure only, with a cleared depth.
            if (er[i] != NVSDK_NGX_Result_Success && !used_depth)
            {
                mgpu::diag::warn("[MGPU][P1.2] retrying WITH a cleared depth texture - the "
                                 "depth-free path held in P1.1, so this is a regression worth "
                                 "reporting whichever way it goes");
                params->Set("DLSSNR.Depth", tex_depth);
                params->Set("DLSSNR.DepthSubrectBaseX", 0u);
                params->Set("DLSSNR.DepthSubrectBaseY", 0u);
                params->Set("DLSSNR.DepthSubrectWidth", (unsigned int)width);
                params->Set("DLSSNR.DepthSubrectHeight", (unsigned int)height);
                used_depth = true;
                er[i] = p_evaluate(pcmd, handle, params, nullptr);
                snprintf(line, sizeof line,
                         "[MGPU][P1.2] EvaluateFeature %s (with depth): result=0x%08X (%s)",
                         labels[i], (unsigned)er[i], ngx_result_name(er[i]));
                mgpu::diag::info(line);
            }
        }

        // Readbacks are recorded whatever the result codes said. An evaluate
        // that returned Success and wrote nothing is a real and very quiet
        // failure mode, and it is what the sentinel exists to catch.
        for (int i = 0; i < NOUT; ++i)
            barrier(pcmd, tex_out[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(pcmd, tex_color, read_state, D3D12_RESOURCE_STATE_COPY_SOURCE);

        auto copy_out = [&](ID3D12Resource *src, ID3D12Resource *dst)
        {
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = src;
            s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            s.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = dst;
            d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d.PlacedFootprint = fp_color;
            d.PlacedFootprint.Offset = 0;
            pcmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        };
        copy_out(tex_color, buf_read_in);
        for (int i = 0; i < NOUT; ++i)
            copy_out(tex_out[i], buf_read_out[i]);

        // ---- flush here rather than in teardown ----
        //
        // The comparison needs the GPU to have finished, and teardown
        // releases the buffers it would read. So the drain happens now;
        // list_open goes false and teardown skips its own.
        mgpu::diag::info("[MGPU][P1.2] flush: Close -> Execute -> fence wait ...");
        LARGE_INTEGER g0{}, g1{};
        QueryPerformanceCounter(&g0);
        hr = pcmd->Close();
        list_open = false;
        if (SUCCEEDED(hr))
        {
            ID3D12CommandList *const lists[1] = { pcmd };
            queue->ExecuteCommandLists(1, lists);
            hr = queue->Signal(pfence, 1);
        }
        DWORD wr = WAIT_FAILED;
        if (SUCCEEDED(hr))
        {
            pfence->SetEventOnCompletion(1, pevent);
            wr = WaitForSingleObject(pevent, 20000);
        }
        if (FAILED(hr) || wr != WAIT_OBJECT_0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.2] flush FAILED hr=0x%08X wait=0x%08X - not reading back, and not "
                     "releasing anything (the GPU may still hold these resources)",
                     (unsigned)hr, (unsigned)wr);
            mgpu::diag::error(line);
            mgpu::diag::warn("[MGPU][P1.2] PROBE FAILED at flush. Bridge continues.");
            return false;
        }
        QueryPerformanceCounter(&g1);
        const double gms = (f.QuadPart > 0)
            ? ((double)(g1.QuadPart - g0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
        // WHOLE LIST, and now THREE evaluates plus seven copies. An upper
        // bound and nothing more. A per-pass GPU figure needs timestamp
        // queries, which belong with P1_INSTRUMENT.md - do NOT compare this
        // against the reference tool's 14.2 ms evaluateGPU.
        snprintf(line, sizeof line,
                 "[MGPU][P1.2] flush: GPU idle after %.2f ms (WHOLE LIST: 4 uploads + 3 evaluates "
                 "+ 4 readbacks, taken during game startup - an upper bound on a contaminated "
                 "sample. NOT a performance figure. See the context line above.)", gms);
        mgpu::diag::info(line);

        // ---- the comparison ----
        {
            const unsigned char *pin = nullptr;
            const unsigned char *po[NOUT] = {};
            D3D12_RANGE all{0, (SIZE_T)sz_color};
            HRESULT hm = buf_read_in->Map(0, &all, (void **)&pin);
            for (int i = 0; i < NOUT && SUCCEEDED(hm); ++i)
                hm = buf_read_out[i]->Map(0, &all, (void **)&po[i]);

            if (FAILED(hm) || pin == nullptr || po[0] == nullptr ||
                po[1] == nullptr || po[2] == nullptr)
            {
                snprintf(line, sizeof line,
                         "[MGPU][P1.2] readback Map hr=0x%08X - cannot compare", (unsigned)hm);
                mgpu::diag::error(line);
            }
            else
            {
                const UINT pitch = fp_color.Footprint.RowPitch;

                // Counts one output against a reference image.
                auto compare = [&](const unsigned char *a, const unsigned char *b,
                                   unsigned long long &differing, double &mean_abs,
                                   unsigned int &max_abs)
                {
                    unsigned long long sum = 0, n = 0;
                    differing = 0; max_abs = 0;
                    for (UINT y = 0; y < height; ++y)
                    {
                        const unsigned char *ra = a + (size_t)y * pitch;
                        const unsigned char *rb = b + (size_t)y * pitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            const unsigned char *pa = ra + (size_t)x * 4;
                            const unsigned char *pb = rb + (size_t)x * 4;
                            bool diff = false;
                            for (int c = 0; c < 3; ++c)
                            {
                                const int d = (int)pb[c] - (int)pa[c];
                                const unsigned int ad = (unsigned int)(d < 0 ? -d : d);
                                if (ad != 0) diff = true;
                                sum += ad;
                                if (ad > max_abs) max_abs = ad;
                            }
                            if (diff) ++differing;
                            ++n;
                        }
                    }
                    mean_abs = n ? ((double)sum / (double)(n * 3)) : 0.0;
                };

                // Sentinel survivors per output: the "NR wrote nothing here"
                // detector.
                unsigned long long sentinel[NOUT] = {};
                const unsigned long long total = (unsigned long long)width * height;
                for (int i = 0; i < NOUT; ++i)
                    for (UINT y = 0; y < height; ++y)
                    {
                        const unsigned char *r = po[i] + (size_t)y * pitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            const unsigned char *p = r + (size_t)x * 4;
                            if (p[0] == SENT_R && p[1] == SENT_G && p[2] == SENT_B)
                                ++sentinel[i];
                        }
                    }

                unsigned long long d_in = 0, d_ab = 0, d_ac = 0;
                double m_in = 0, m_ab = 0, m_ac = 0;
                unsigned int x_in = 0, x_ab = 0, x_ac = 0;
                compare(pin,   po[0], d_in, m_in, x_in);   // A vs input  - did it process?
                compare(po[0], po[1], d_ab, m_ab, x_ab);   // A vs B      - did intensity matter?
                compare(po[0], po[2], d_ac, m_ac, x_ac);   // A vs C      - THE CONTROL

                snprintf(line, sizeof line,
                         "[MGPU][P1.2] sentinel survivors: A=%llu B=%llu C=%llu of %llu pixels",
                         sentinel[0], sentinel[1], sentinel[2], total);
                mgpu::diag::info(line);
                snprintf(line, sizeof line,
                         "[MGPU][P1.2] A vs input: differing=%llu (%.2f%%) mean=%.3f max=%u",
                         d_in, total ? 100.0 * (double)d_in / (double)total : 0.0, m_in, x_in);
                mgpu::diag::info(line);
                snprintf(line, sizeof line,
                         "[MGPU][P1.2] A(%.2f) vs B(%.2f): differing=%llu (%.2f%%) mean=%.3f max=%u",
                         INTENSITY_LO, INTENSITY_HI, d_ab,
                         total ? 100.0 * (double)d_ab / (double)total : 0.0, m_ab, x_ab);
                mgpu::diag::info(line);
                snprintf(line, sizeof line,
                         "[MGPU][P1.2] A(%.2f) vs C(%.2f) [CONTROL]: differing=%llu (%.2f%%) "
                         "mean=%.3f max=%u",
                         INTENSITY_LO, INTENSITY_LO, d_ac,
                         total ? 100.0 * (double)d_ac / (double)total : 0.0, m_ac, x_ac);
                mgpu::diag::info(line);

                const bool all_ok = (er[0] == NVSDK_NGX_Result_Success &&
                                     er[1] == NVSDK_NGX_Result_Success &&
                                     er[2] == NVSDK_NGX_Result_Success);
                const bool wrote  = (sentinel[0] == 0 && sentinel[1] == 0 && sentinel[2] == 0);

                if (!all_ok)
                {
                    mgpu::diag::warn("[MGPU][P1.2] ONE OR MORE EVALUATES FAILED - the numbers "
                                     "above describe outputs NR may not have written. Read them "
                                     "as a control, not a result.");
                }
                else if (!wrote)
                {
                    mgpu::diag::error("[MGPU][P1.2] AN OUTPUT STILL HOLDS SENTINEL PIXELS after a "
                                      "Success - NR did not write everything it claimed. Suspect "
                                      "the DLSSNR.Output rebind between evaluates, or a UAV "
                                      "barrier this path is missing.");
                }
                else if (d_in == 0)
                {
                    mgpu::diag::error("[MGPU][P1.2] OUTPUT A == INPUT byte for byte - NR copied "
                                      "rather than processed. The intensity comparison below is "
                                      "meaningless until that is fixed.");
                }
                else if (d_ac != 0)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P1.2] INCONCLUSIVE - the control failed. A and C ran at the "
                             "SAME intensity (%.2f) and still differ in %llu pixels (mean %.3f). "
                             "DLSSNR.Reset did not fully discard dlssnr_prev_output, so evaluate "
                             "order contaminates the comparison and A-vs-B cannot be attributed "
                             "to intensity. Next: one feature per setting, rebuilt between runs.",
                             INTENSITY_LO, d_ac, m_ac);
                    mgpu::diag::error(line);
                }
                else if (d_ab == 0)
                {
                    mgpu::diag::warn("[MGPU][P1.2] PARAMETERS ARE FROZEN AT CREATE. Intensity 0.00 "
                                     "and 1.60 produced byte-identical output, with the control "
                                     "clean. PollRuntimeParams reporting a NULL callback is the "
                                     "explanation. CONSEQUENCE: every tuning change costs a "
                                     "~220 ms CreateFeature rebuild, and runtime quality control "
                                     "is not available on this path.");
                }
                else
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P1.2] PROBE PASSED - PARAMETERS ARE LIVE PER EVALUATE. "
                             "Intensity %.2f vs %.2f changed %llu pixels (%.2f%%, mean %.3f, "
                             "max %u) while the same-intensity control A vs C was byte-identical. "
                             "DLSS-NR on GPU 1 is tunable at runtime without a rebuild.",
                             INTENSITY_LO, INTENSITY_HI, d_ab,
                             total ? 100.0 * (double)d_ab / (double)total : 0.0, m_ab, x_ab);
                    mgpu::diag::info(line);
                }

                // ---- the P3.0 verdict ----
                // Same evidence, different question. P1.2 asked whether the
                // parameters were live. P3.0 asks whether the thing NR just
                // processed was the application's frame - and the answer is
                // carried by d_ab being non-zero on content we did not
                // generate, with the A/C control still clean. A pattern and a
                // real frame are not distinguishable from the numbers alone,
                // which is why this line states the provenance rather than
                // inferring it: the pixels came from the capture path, which
                // had already proved them byte-identical to what the game
                // rendered.
                if (P3)
                {
                    if (d_ab == 0)
                        mgpu::diag::error("[MGPU][P3.0] PROBE FAILED - DLSS-NR produced "
                                          "byte-identical output at both intensities on the "
                                          "game's own frame. Either the model did not run or it "
                                          "did nothing to this content. Read the CreateFeature "
                                          "and Evaluate results above before assuming the "
                                          "former.");
                    else
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][P3.%s] PROBE PASSED - DLSS-NR RAN ON THE GAME'S OWN "
                                 "FRAME, ON THE SECOND ADAPTER, IN %s. %ux%u of real rendered "
                                 "content crossed the adapter boundary and was processed by the "
                                 "neural stage: intensity %.2f vs %.2f changed %llu pixels "
                                 "(%.2f%%) with the same-intensity control byte-identical. The "
                                 "chain is closed end to end - game frame, boundary, model - "
                                 "and no stage of it is synthetic any more.",
                                 native_fmt ? "1" : "0",
                                 native_fmt ? "THE GAME'S NATIVE FORMAT, UNCONVERTED - the CPU "
                                              "conversion stage is NOT needed"
                                            : "R8G8B8A8 after a CPU conversion",
                                 width, height, INTENSITY_LO, INTENSITY_HI, d_ab,
                                 total ? 100.0 * (double)d_ab / (double)total : 0.0);
                        mgpu::diag::info(line);
                        p3_ok = true;
                    }
                }
            }

            // P1.4 needs the LOCAL NR output as its control, and the mapping
            // it lives in is about to go away. One memcpy now is cheaper than
            // a second evaluate later, and - more to the point - it is the
            // SAME evaluate rather than a repeat of it, so a difference later
            // cannot be blamed on the model having been run twice.
            if (po[0] != nullptr)
            {
                ref_local = (unsigned char *)malloc((size_t)sz_color);
                if (ref_local != nullptr) memcpy(ref_local, po[0], (size_t)sz_color);
            }

            D3D12_RANGE nothing{0, 0};
            if (pin != nullptr) buf_read_in->Unmap(0, &nothing);
            for (int i = 0; i < NOUT; ++i)
                if (po[i] != nullptr) buf_read_out[i]->Unmap(0, &nothing);
        }

        // =================================================================
        // P3.2 - A PERSISTENT FEATURE, DRIVEN REPEATEDLY, AND A SOUND TEST
        //        OF WHETHER NR WRITES EVERY PIXEL
        // =================================================================
        //
        // Two questions, one batch, no new allocations - every buffer below is
        // one P1.2 already owns.
        //
        // QUESTION 1: IS THE FEATURE DRIVABLE, OR ONLY CREATABLE? Everything
        // up to here evaluates a freshly created feature two or three times
        // and destroys it. A pipeline evaluates one feature for the lifetime
        // of a session. This runs the SAME handle REPEATS times per batch,
        // twice, on one command list - and if the feature degrades, stops
        // writing, or starts failing after n uses, this is where it shows.
        //
        // QUESTION 2: THE SENTINEL SURVIVORS, AND WHY THE OLD TEST CANNOT
        // ANSWER THEM. P1.2 pre-fills each output with a fixed COLOUR and
        // counts pixels that still hold it afterwards. That test has the
        // same flaw P1.5e had: it is an ABSOLUTE match with no control, so a
        // pixel NR legitimately wrote to the sentinel value is counted as one
        // NR failed to write. The native-format run made that concrete - 1
        // survivor in A and C, 0 in B, where the converted runs had 0
        // throughout. On a packed 10:10:10:2 format the sentinel is being
        // matched against bit fields that straddle byte boundaries, so an
        // accidental match is far more likely than it is in R8G8B8A8. One
        // pixel in 3.69 million is exactly the scale of coincidence.
        //
        // THE DIFFERENTIAL TEST HAS NO ABSOLUTE COLOUR IN IT. Evaluate twice
        // at the SAME intensity, from the SAME input, into an output
        // pre-filled with 0x00 the first time and 0xFF the second. A pixel NR
        // wrote holds the model's value both times and matches. A pixel NR did
        // not write holds 0x00 once and 0xFF once and cannot match. The count
        // of differing pixels is therefore the EXACT number of unwritten
        // pixels, with no false positives possible, in any format. If it is
        // zero, the survivors were coincidence and the DLSSNR.Output rebind is
        // exonerated.
        //
        // UAV BARRIERS BETWEEN EVALUATES, WHICH P1.2 DOES NOT ISSUE. P1.2's
        // warning named this as a suspect and it was right to: its three
        // evaluates happen to target three DIFFERENT textures, so there is no
        // hazard to guard. Here every evaluate in a batch writes the SAME
        // texture, and without a UAV barrier they may overlap. That barrier is
        // issued below, which also means that if the survivor count changes
        // between P1.2's discipline and this one, the barrier is the variable.
        if (P3)
        {
            const int REPEATS = 8;
            const unsigned char FILL_A = 0x00, FILL_B = 0xFF;

            // The two fills go into the upload regions P1.2 used for its
            // output sentinels. Those readbacks have been compared and
            // unmapped, so the space is free and no new memory is needed at
            // full resolution.
            unsigned char *um = nullptr;
            D3D12_RANGE nonein{0, 0};
            HRESULT ph = buf_upload->Map(0, &nonein, reinterpret_cast<void **>(&um));
            if (SUCCEEDED(ph) && um != nullptr)
            {
                memset(um + off_out[1], FILL_A, (size_t)sz_color);
                memset(um + off_out[2], FILL_B, (size_t)sz_color);
                buf_upload->Unmap(0, nullptr);
            }

            if (FAILED(ph))
                mgpu::diag::warn("[MGPU][P3.2] could not map the upload buffer - skipped");
            else
            {
                ph = palloc->Reset();
                if (SUCCEEDED(ph)) ph = pcmd->Reset(palloc, nullptr);
                if (SUCCEEDED(ph)) list_open = true;

                // tex_color was left in COPY_SOURCE by P1.2's readback. NR
                // reads it, so it goes back to the state the evaluates used.
                if (SUCCEEDED(ph))
                    barrier(pcmd, tex_color, D3D12_RESOURCE_STATE_COPY_SOURCE, read_state);

                auto uav_barrier = [&](ID3D12Resource *r)
                {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    b.UAV.pResource = r;
                    pcmd->ResourceBarrier(1, &b);
                };

                // One batch: pre-fill tex_out[0] from `src_off`, evaluate
                // REPEATS times with a UAV barrier between each, copy the
                // result to `dst`. tex_out[0] arrives in COPY_SOURCE (P1.2's
                // readback left it there) and is returned to COPY_SOURCE.
                auto batch = [&](UINT64 src_off, ID3D12Resource *dst)
                {
                    barrier(pcmd, tex_out[0], D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION s{}, d{};
                    s.pResource = buf_upload;
                    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    s.PlacedFootprint = fp_color;
                    s.PlacedFootprint.Offset = src_off;
                    d.pResource = tex_out[0];
                    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    pcmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                    barrier(pcmd, tex_out[0], D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    params->Set("DLSSNR.Output", tex_out[0]);
                    params->Set("DLSSNR.Intensity", INTENSITY_LO);
                    int fails = 0;
                    NVSDK_NGX_Result last = NVSDK_NGX_Result_Success;
                    for (int k = 0; k < REPEATS; ++k)
                    {
                        params->Set("DLSSNR.Reset", 1u);
                        last = p_evaluate(pcmd, handle, params, nullptr);
                        if (last != NVSDK_NGX_Result_Success) ++fails;
                        if (k + 1 < REPEATS) uav_barrier(tex_out[0]);
                    }
                    barrier(pcmd, tex_out[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
                    s2.pResource = tex_out[0];
                    s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    d2.pResource = dst;
                    d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    d2.PlacedFootprint = fp_color;
                    d2.PlacedFootprint.Offset = 0;
                    pcmd->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);

                    snprintf(line, sizeof line,
                             "[MGPU][P3.2] batch of %d evaluates on ONE persistent feature "
                             "handle: failures=%d last_result=0x%08X (%s)",
                             REPEATS, fails, (unsigned)last, ngx_result_name(last));
                    mgpu::diag::info(line);
                    return fails;
                };

                int f1 = 0, f2 = 0;
                if (SUCCEEDED(ph))
                {
                    f1 = batch(off_out[1], buf_read_out[1]);
                    f2 = batch(off_out[2], buf_read_out[2]);
                    ph = pcmd->Close();
                    list_open = false;
                }
                if (SUCCEEDED(ph))
                {
                    ID3D12CommandList *const ls[1] = { pcmd };
                    queue->ExecuteCommandLists(1, ls);
                    // 2, not 1: value 1 was signalled by the P1.2 flush and a
                    // fence value already passed completes instantly.
                    ph = queue->Signal(pfence, 2);
                    if (SUCCEEDED(ph))
                    {
                        pfence->SetEventOnCompletion(2, pevent);
                        if (WaitForSingleObject(pevent, 20000) != WAIT_OBJECT_0) ph = E_FAIL;
                    }
                }

                if (FAILED(ph))
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P3.2] batch submission failed hr=0x%08X - no verdict",
                             (unsigned)ph);
                    mgpu::diag::error(line);
                }
                else
                {
                    const unsigned char *r1 = nullptr, *r2 = nullptr;
                    D3D12_RANGE allr{0, (SIZE_T)sz_color};
                    const bool m1 = SUCCEEDED(buf_read_out[1]->Map(0, &allr, (void **)&r1))
                                    && r1 != nullptr;
                    const bool m2 = SUCCEEDED(buf_read_out[2]->Map(0, &allr, (void **)&r2))
                                    && r2 != nullptr;
                    if (m1 && m2)
                    {
                        unsigned long long unwritten = 0;
                        const unsigned long long tot =
                            (unsigned long long)width * height;
                        for (UINT y = 0; y < height; ++y)
                        {
                            const size_t ro = (size_t)y * fp_color.Footprint.RowPitch;
                            for (UINT x = 0; x < width; ++x)
                            {
                                const unsigned char *a = r1 + ro + (size_t)x * 4;
                                const unsigned char *b = r2 + ro + (size_t)x * 4;
                                if (a[0] != b[0] || a[1] != b[1] ||
                                    a[2] != b[2] || a[3] != b[3]) ++unwritten;
                            }
                        }
                        if (f1 == 0 && f2 == 0 && unwritten == 0)
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][P3.2] PROBE PASSED - ONE FEATURE, %d EVALUATES, "
                                     "EVERY PIXEL WRITTEN. The same handle was evaluated %d "
                                     "times across two batches with no failure, and the two "
                                     "runs - identical input and intensity, opposite pre-fills "
                                     "(0x%02X and 0x%02X) - produced byte-identical output over "
                                     "all %llu pixels. A pixel NR had not written could not have "
                                     "matched, so THE SENTINEL SURVIVORS WERE COINCIDENCE, not "
                                     "unwritten output: the absolute-colour test was matching "
                                     "real content that happened to equal the fill. "
                                     "DLSSNR.Output rebinding is exonerated. The feature is a "
                                     "pipeline object, not a one-shot.",
                                     REPEATS * 2, REPEATS * 2, FILL_A, FILL_B, tot);
                            mgpu::diag::info(line);
                        }
                        else if (unwritten > 0)
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][P3.2] PROBE FAILED - %llu of %llu pixels DIFFER "
                                     "between the two pre-fills, which is the exact count NR "
                                     "left unwritten (evaluate failures: %d and %d). This is a "
                                     "real gap in the model's output coverage and it is not a "
                                     "measurement artefact - no absolute colour is involved in "
                                     "this test. Map where they are before theorising: a border, "
                                     "a tile edge and a scatter are three different causes.",
                                     unwritten, tot, f1, f2);
                            mgpu::diag::error(line);
                        }
                        else
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][P3.2] PROBE FAILED - the output is fully written "
                                     "but %d and %d evaluates returned a failure. The feature "
                                     "does not survive repeated use, which is the one thing a "
                                     "persistent pipeline requires of it.", f1, f2);
                            mgpu::diag::error(line);
                        }
                    }
                    else mgpu::diag::error("[MGPU][P3.2] readback Map failed - no verdict");
                    D3D12_RANGE none2{0, 0};
                    if (m1) buf_read_out[1]->Unmap(0, &none2);
                    if (m2) buf_read_out[2]->Unmap(0, &none2);
                }
            }
        }

        // =================================================================
        // P4.2 - DOES DLSS-NR READ DEPTH? THE PAYLOAD QUESTION.
        // =================================================================
        //
        // Every evaluate this project has ever run passed depth as NULL and
        // motion vectors as ZERO, and NR accepted all of them. "Accepted" is
        // not "unaffected", and the difference sets the payload for the entire
        // architecture:
        //
        //   depth not read  -> colour only crosses. The design is what we built.
        //   depth read      -> depth must cross too. R32_FLOAT at 1440p is
        //                      ~14.7 MB against ~14.06 MB of colour, so the
        //                      per-frame payload roughly DOUBLES on a link that
        //                      is already the binding constraint.
        //
        // Motion vectors are deliberately not part of this question. P0_RECORD
        // records QuantMotion deriving flow from colour ON GPU 1 at 0.13-0.17
        // ms, and the reference tool already runs that substitution with zero
        // failures over 3000 evaluates - so flow is produced where it is
        // consumed and never crosses. Depth cannot be derived that way, which
        // is why it is the only open half.
        //
        // THE TEST IS A CONTROL, NOT AN OBSERVATION. Two evaluates, identical
        // in every respect - same real frame, same intensity, same Reset - with
        // exactly one variable: whether a depth texture is bound. Byte-compare
        // the outputs.
        //
        //   differing == 0  -> the feature did not read depth AT ALL on this
        //                      path. Not "depth is optional": not read.
        //   differing > 0   -> it read it, and depth joins the payload.
        //
        // THE DEPTH IS A GRADIENT, NOT A CONSTANT, AND THAT MATTERS. A cleared
        // depth carries no more information than no depth, so identical output
        // would be ambiguous between "ignores depth" and "a flat depth happens
        // to mean the same as none". A varying field removes that reading: if
        // NR looks at depth at all, a plane sweeping front-to-back cannot
        // produce the same bytes as no depth.
        if (P3 && !used_depth)
        {
            unsigned long long d_diff = 0;
            bool ok42 = true;
            const unsigned long long total42 = (unsigned long long)width * height;

            // A front-to-back gradient in R32_FLOAT, written into the upload
            // region P1.2 already reserved for depth.
            {
                unsigned char *um = nullptr;
                D3D12_RANGE none{0, 0};
                if (SUCCEEDED(buf_upload->Map(0, &none, reinterpret_cast<void **>(&um))) &&
                    um != nullptr)
                {
                    for (UINT y = 0; y < height; ++y)
                    {
                        unsigned char *row = um + fp_depth.Offset
                                           + (size_t)y * fp_depth.Footprint.RowPitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            const float d = (height > 1)
                                ? ((float)y / (float)(height - 1)) : 0.5f;
                            memcpy(row + (size_t)x * 4, &d, 4);
                        }
                    }
                    buf_upload->Unmap(0, nullptr);
                }
                else ok42 = false;
            }

            // Pass 1: NO depth key has ever been set in this session (the
            // `!used_depth` guard above is what guarantees that - P1.2's retry
            // path binds depth, and if it fired there is no null-depth arm to
            // compare against and this probe correctly does not run).
            auto run42 = [&](bool bind_depth, ID3D12Resource *dst, UINT64 fence_v) -> bool
            {
                HRESULT h = palloc->Reset();
                if (SUCCEEDED(h)) h = pcmd->Reset(palloc, nullptr);
                if (!SUCCEEDED(h)) return false;
                list_open = true;

                if (bind_depth)
                {
                    barrier(pcmd, tex_depth, read_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION ds{}, dd{};
                    ds.pResource = buf_upload;
                    ds.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    ds.PlacedFootprint = fp_depth;
                    dd.pResource = tex_depth;
                    dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    pcmd->CopyTextureRegion(&dd, 0, 0, 0, &ds, nullptr);
                    barrier(pcmd, tex_depth, D3D12_RESOURCE_STATE_COPY_DEST, read_state);

                    params->Set("DLSSNR.Depth", tex_depth);
                    params->Set("DLSSNR.DepthSubrectBaseX", 0u);
                    params->Set("DLSSNR.DepthSubrectBaseY", 0u);
                    params->Set("DLSSNR.DepthSubrectWidth",  (unsigned int)width);
                    params->Set("DLSSNR.DepthSubrectHeight", (unsigned int)height);
                }

                params->Set("DLSSNR.Output", tex_out[0]);
                params->Set("DLSSNR.Intensity", INTENSITY_LO);
                params->Set("DLSSNR.Reset", 1u);
                const NVSDK_NGX_Result er = p_evaluate(pcmd, handle, params, nullptr);

                snprintf(line, sizeof line,
                         "[MGPU][P4.2] EvaluateFeature depth=%s: result=0x%08X (%s)",
                         bind_depth ? "GRADIENT (bound)" : "null",
                         (unsigned)er, ngx_result_name(er));
                mgpu::diag::info(line);

                barrier(pcmd, tex_out[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION os{}, od{};
                os.pResource = tex_out[0];
                os.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                od.pResource = dst;
                od.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                od.PlacedFootprint = fp_color;
                od.PlacedFootprint.Offset = 0;
                pcmd->CopyTextureRegion(&od, 0, 0, 0, &os, nullptr);
                barrier(pcmd, tex_out[0], D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                h = pcmd->Close();
                list_open = false;
                if (FAILED(h) || er != NVSDK_NGX_Result_Success) return false;
                ID3D12CommandList *const ls[1] = { pcmd };
                queue->ExecuteCommandLists(1, ls);
                if (FAILED(queue->Signal(pfence, fence_v))) return false;
                pfence->SetEventOnCompletion(fence_v, pevent);
                return WaitForSingleObject(pevent, 20000) == WAIT_OBJECT_0;
            };

            if (ok42) ok42 = run42(false, buf_read_out[1], 3);
            if (ok42) ok42 = run42(true,  buf_read_out[2], 4);

            if (ok42)
            {
                const unsigned char *a = nullptr, *b = nullptr;
                D3D12_RANGE all{0, (SIZE_T)sz_color};
                const bool ma = SUCCEEDED(buf_read_out[1]->Map(0, &all, (void **)&a)) && a != nullptr;
                const bool mb = SUCCEEDED(buf_read_out[2]->Map(0, &all, (void **)&b)) && b != nullptr;
                if (ma && mb)
                {
                    for (UINT y = 0; y < height; ++y)
                    {
                        const size_t ro = (size_t)y * fp_color.Footprint.RowPitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            const unsigned char *pa = a + ro + (size_t)x * 4;
                            const unsigned char *pb = b + ro + (size_t)x * 4;
                            if (pa[0] != pb[0] || pa[1] != pb[1] ||
                                pa[2] != pb[2] || pa[3] != pb[3]) ++d_diff;
                        }
                    }
                }
                else ok42 = false;
                D3D12_RANGE none{0, 0};
                if (ma) buf_read_out[1]->Unmap(0, &none);
                if (mb) buf_read_out[2]->Unmap(0, &none);
            }

            if (!ok42)
                mgpu::diag::error("[MGPU][P4.2] PROBE INCOMPLETE - one arm did not run to "
                                  "completion. No comparison is available; the depth question "
                                  "stays open rather than being answered by a partial run.");
            else if (d_diff == 0)
            {
                snprintf(line, sizeof line,
                         "[MGPU][P4.2] DEPTH IS NOT READ - %ux%u, all %llu pixels byte-identical "
                         "with a null depth and with a front-to-back gradient bound. Two "
                         "evaluates, one variable. A feature that read depth could not return "
                         "the same bytes for no depth and for a sweeping plane, so this is not "
                         "'depth is optional' - it is not being sampled on this path at all. "
                         "CONSEQUENCE: the per-frame payload is COLOUR ONLY. Depth never crosses "
                         "the link, and the ~2x payload the architecture was budgeting for does "
                         "not exist. SCOPE: this is preset=0 with the parameters this add-on "
                         "sets; a different preset or guidance mode may read it, and that is a "
                         "separate question from whether THIS configuration does.",
                         width, height, total42);
                mgpu::diag::info(line);
            }
            else
            {
                snprintf(line, sizeof line,
                         "[MGPU][P4.2] DEPTH IS READ - %llu of %llu pixels (%.2f%%) differ "
                         "between a null depth and a bound gradient. The feature samples it, so "
                         "the game's real depth buffer has to reach GPU 1 and the per-frame "
                         "payload grows by an R32_FLOAT frame - roughly DOUBLE at this "
                         "resolution, on the link that is already the constraint. Next question "
                         "is not whether to carry it but whether a reduced-precision or "
                         "lower-cadence depth is enough; P0_RECORD's reference tool runs "
                         "depthInterval=4, which is exactly that idea.",
                         d_diff, total42, total42 ? 100.0 * (double)d_diff / (double)total42 : 0.0);
                mgpu::diag::info(line);
            }
        }
        else if (P3)
            mgpu::diag::warn("[MGPU][P4.2] skipped - P1.2's retry path already bound a depth "
                             "texture this session, so there is no null-depth arm left to "
                             "compare against. The result would be a comparison of two "
                             "depth-bound runs, which answers nothing.");

        // =================================================================
        // P1.4 - DLSS-NR INSIDE THE LOOP, ACROSS THE BUS
        // =================================================================
        //
        // Everything before this ran NR against textures that were already on
        // GPU 1. P1.3 crossed a payload but never fed it to anything. P1.4 joins
        // them and asks the milestone's actual question:
        //
        //   pattern on GPU 0 -> cross -> NR on GPU 1 -> cross back -> GPU 0
        //
        // THE VERDICT IS A COMPARISON AGAINST A CONTROL, NOT AN OBSERVATION.
        // `ref_local` holds output A from the P1.2 evaluate above - the same
        // model, the same deterministic input, the same intensity, run entirely
        // on GPU 1 with no bus involved. P1.4 passes only if the bytes that come
        // back across the bus are BYTE-IDENTICAL to it. Anything less and the
        // difference is either transit corrupting the payload or NR behaving
        // differently on transited input, and both are failures.
        //
        // "It looks processed" is not the test, and neither is "it differs from
        // the input" - an uninitialised buffer satisfies both. The sentinel fill
        // separates "came back wrong" from "never came back".
        //
        // Deliberately NOT here: the game's own frame. That needs the ReShade
        // effect-runtime finish hook, which stays in the containment guard until
        // the milestone that legitimately uses it. A synthetic deterministic
        // pattern
        // is what makes the byte comparison possible at all; real content would
        // trade the verdict for a screenshot.
        // !P3: P1.4's control is the LOCAL NR output for the SYNTHETIC
        // pattern. Running it against a real frame would compare two
        // different inputs and report the difference as a transit fault.
        if (!P3 && ref_local != nullptr && st().game_luid_known)
        {
            ID3D12Device *dev0 = nullptr;
            ID3D12CommandQueue *q0 = nullptr;
            ID3D12CommandAllocator *a0 = nullptr;
            ID3D12GraphicsCommandList *l0 = nullptr;
            ID3D12Fence *fen0 = nullptr;
            HANDLE ev0 = nullptr;
            UINT64 fv0 = 0;

            ID3D12Heap *hp0 = nullptr, *hp1 = nullptr;
            HANDLE shh = nullptr;
            ID3D12Resource *xfer0 = nullptr, *xfer1 = nullptr;
            ID3D12Resource *up0 = nullptr, *rb0 = nullptr;
            ID3D12Resource *t_in = nullptr, *t_out = nullptr;

            auto p14_cleanup = [&]()
            {
                if (t_out != nullptr) t_out->Release();
                if (t_in  != nullptr) t_in->Release();
                if (rb0   != nullptr) rb0->Release();
                if (up0   != nullptr) up0->Release();
                if (xfer1 != nullptr) xfer1->Release();
                if (xfer0 != nullptr) xfer0->Release();
                if (hp1   != nullptr) hp1->Release();
                if (hp0   != nullptr) hp0->Release();
                if (shh   != nullptr) CloseHandle(shh);
                if (ev0   != nullptr) CloseHandle(ev0);
                if (fen0  != nullptr) fen0->Release();
                if (l0    != nullptr) l0->Release();
                if (a0    != nullptr) a0->Release();
                if (q0    != nullptr) q0->Release();
                if (dev0  != nullptr) dev0->Release();
            };

            auto flush0 = [&](DWORD timeout_ms) -> HRESULT
            {
                HRESULT h = l0->Close();
                if (FAILED(h)) return h;
                ID3D12CommandList *const ls[1] = { l0 };
                q0->ExecuteCommandLists(1, ls);
                h = q0->Signal(fen0, ++fv0);
                if (FAILED(h)) return h;
                fen0->SetEventOnCompletion(fv0, ev0);
                return (WaitForSingleObject(ev0, timeout_ms) == WAIT_OBJECT_0) ? S_OK : E_FAIL;
            };

            HRESULT h = transit_make_device(st().game_luid, &dev0);
            if (SUCCEEDED(h))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                h = dev0->CreateCommandQueue(&qd, IID_PPV_ARGS(&q0));
            }
            if (SUCCEEDED(h))
                h = dev0->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a0));
            if (SUCCEEDED(h))
                h = dev0->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a0, nullptr,
                                            IID_PPV_ARGS(&l0));
            if (SUCCEEDED(h))
                h = dev0->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fen0));
            if (SUCCEEDED(h))
            {
                ev0 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev0 == nullptr) h = E_FAIL;
            }
            snprintf(line, sizeof line,
                     "[MGPU][P1.4] GPU 0 side: own device + command objects hr=0x%08X "
                     "(the game's device is not touched; the GPU 1 side is the NGX device, "
                     "because that is where the session and the feature handle live)",
                     (unsigned)h);
            mgpu::diag::info(line);

            // ---- the shared cross-adapter heap, path A as P1.3 settled it ----
            // CreateHeap + CreatePlacedResource, share the HEAP not the resource.
            const UINT64 ALIGN = 65536;
            const UINT64 xbytes = ((sz_color + ALIGN - 1) / ALIGN) * ALIGN;
            if (SUCCEEDED(h))
            {
                D3D12_HEAP_PROPERTIES xhp{};
                xhp.Type = D3D12_HEAP_TYPE_DEFAULT;
                xhp.CreationNodeMask = 1; xhp.VisibleNodeMask = 1;
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = xbytes;
                hd.Properties = xhp;
                hd.Alignment = ALIGN;
                hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED |
                                              D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
                h = dev0->CreateHeap(&hd, IID_PPV_ARGS(&hp0));
                if (SUCCEEDED(h))
                    h = dev0->CreateSharedHandle(hp0, nullptr, GENERIC_ALL, nullptr, &shh);
                if (SUCCEEDED(h))
                    h = dev->OpenSharedHandle(shh, IID_PPV_ARGS(&hp1));
                snprintf(line, sizeof line,
                         "[MGPU][P1.4] shared cross-adapter heap: %llu bytes, CreateHeap ->"
                         " CreateSharedHandle(HEAP) -> OpenSharedHandle on the NGX device: "
                         "hr=0x%08X", (unsigned long long)xbytes, (unsigned)h);
                mgpu::diag::info(line);
            }

            D3D12_RESOURCE_DESC xd{};
            xd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xd.Alignment = 0;
            xd.Width = xbytes; xd.Height = 1; xd.DepthOrArraySize = 1; xd.MipLevels = 1;
            xd.Format = DXGI_FORMAT_UNKNOWN; xd.SampleDesc.Count = 1;
            xd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            xd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            if (SUCCEEDED(h))
                h = dev0->CreatePlacedResource(hp0, 0, &xd, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&xfer0));
            if (SUCCEEDED(h))
                h = dev->CreatePlacedResource(hp1, 0, &xd, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&xfer1));

            // GPU 0 staging, and the two GPU 1 textures NR will work on.
            if (SUCCEEDED(h)) h = make_buf(dev0, sz_color, D3D12_HEAP_TYPE_UPLOAD, &up0);
            if (SUCCEEDED(h)) h = make_buf(dev0, sz_color, D3D12_HEAP_TYPE_READBACK, &rb0);
            if (SUCCEEDED(h))
                h = make_tex(dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &t_in);
            if (SUCCEEDED(h))
                h = make_tex(dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_DEST, &t_out);
            snprintf(line, sizeof line,
                     "[MGPU][P1.4] resources hr=0x%08X xfer0=0x%p xfer1=0x%p in=0x%p out=0x%p",
                     (unsigned)h, (void *)xfer0, (void *)xfer1, (void *)t_in, (void *)t_out);
            mgpu::diag::info(line);

            // ---- fill the GPU 0 upload with the SAME deterministic pattern ----
            if (SUCCEEDED(h))
            {
                unsigned char *mp = nullptr;
                D3D12_RANGE none{0, 0};
                h = up0->Map(0, &none, (void **)&mp);
                if (SUCCEEDED(h) && mp != nullptr)
                {
                    memset(mp, 0, (size_t)sz_color);
                    fill_pattern(mp, width, height, fp_color.Footprint.RowPitch);
                    up0->Unmap(0, nullptr);
                }
                else h = E_FAIL;
            }

            LARGE_INTEGER pf{}, p0{}, p1{};
            QueryPerformanceFrequency(&pf);
            QueryPerformanceCounter(&p0);

            // ---- leg 1: GPU 0 -> shared ----
            if (SUCCEEDED(h))
            {
                // Buffer to buffer, so CopyBufferRegion - no footprint needed on
                // this leg. The footprint matters only where a texture is one end
                // of the copy.
                l0->CopyBufferRegion(xfer0, 0, up0, 0, sz_color);
                h = flush0(20000);
                snprintf(line, sizeof line, "[MGPU][P1.4] leg 1 GPU0 -> shared: hr=0x%08X",
                         (unsigned)h);
                mgpu::diag::info(line);
            }

            // ---- leg 2: shared -> GPU 1 -> NR -> shared ----
            NVSDK_NGX_Result p14r = NVSDK_NGX_Result_Fail;
            if (SUCCEEDED(h))
            {
                h = palloc->Reset();
                if (SUCCEEDED(h)) h = pcmd->Reset(palloc, nullptr);
            }
            if (SUCCEEDED(h))
            {
                // shared buffer -> the NR input texture
                D3D12_TEXTURE_COPY_LOCATION s{}, d{};
                s.pResource = xfer1; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                s.PlacedFootprint = fp_color; s.PlacedFootprint.Offset = 0;
                d.pResource = t_in; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                pcmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                barrier(pcmd, t_in, D3D12_RESOURCE_STATE_COPY_DEST, read_state);
                barrier(pcmd, t_out, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                // Same parameters as the P1.2 A evaluate, restated rather than
                // assumed: the control is only a control if the two runs differ
                // in exactly one thing, which is whether the input crossed a bus.
                params->Set("DLSSNR.Color", t_in);
                params->Set("DLSSNR.Output", t_out);
                params->Set("DLSSNR.MVec", tex_mvec);
                params->Set("DLSSNR.ColorSubrectBaseX", 0u);
                params->Set("DLSSNR.ColorSubrectBaseY", 0u);
                params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)width);
                params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)height);
                params->Set("DLSSNR.Intensity", INTENSITY_LO);
                params->Set("DLSSNR.Reset", 1u);

                p14r = p_evaluate(pcmd, handle, params, nullptr);
                snprintf(line, sizeof line,
                         "[MGPU][P1.4] EvaluateFeature on TRANSITED input: result=0x%08X (%s) "
                         "intensity=%.2f", (unsigned)p14r, ngx_result_name(p14r), INTENSITY_LO);
                mgpu::diag::info(line);

                barrier(pcmd, t_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
                s2.pResource = t_out; s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                d2.pResource = xfer1; d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                d2.PlacedFootprint = fp_color; d2.PlacedFootprint.Offset = 0;
                pcmd->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);

                h = pcmd->Close();
                if (SUCCEEDED(h))
                {
                    ID3D12CommandList *const ls[1] = { pcmd };
                    queue->ExecuteCommandLists(1, ls);
                    h = queue->Signal(pfence, 1000);
                    if (SUCCEEDED(h))
                    {
                        pfence->SetEventOnCompletion(1000, pevent);
                        if (WaitForSingleObject(pevent, 20000) != WAIT_OBJECT_0) h = E_FAIL;
                    }
                }
                snprintf(line, sizeof line, "[MGPU][P1.4] leg 2 shared -> GPU1 -> NR -> shared: "
                         "hr=0x%08X", (unsigned)h);
                mgpu::diag::info(line);
            }

            // ---- leg 3: shared -> GPU 0 readback ----
            if (SUCCEEDED(h))
            {
                h = a0->Reset();
                if (SUCCEEDED(h)) h = l0->Reset(a0, nullptr);
                if (SUCCEEDED(h))
                {
                    l0->CopyBufferRegion(rb0, 0, xfer0, 0, sz_color);
                    h = flush0(20000);
                }
                snprintf(line, sizeof line, "[MGPU][P1.4] leg 3 shared -> GPU0: hr=0x%08X",
                         (unsigned)h);
                mgpu::diag::info(line);
            }
            QueryPerformanceCounter(&p1);
            const double p14ms = (pf.QuadPart > 0)
                ? ((double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)pf.QuadPart) : 0.0;

            // ---- the verdict ----
            if (SUCCEEDED(h))
            {
                const unsigned char *pb = nullptr;
                D3D12_RANGE all{0, (SIZE_T)sz_color};
                if (SUCCEEDED(rb0->Map(0, &all, (void **)&pb)) && pb != nullptr)
                {
                    unsigned long long diff_ctrl = 0, sent = 0, diff_in = 0;
                    const unsigned long long total = (unsigned long long)width * height;
                    unsigned char *inref = (unsigned char *)malloc((size_t)sz_color);
                    if (inref != nullptr)
                    {
                        memset(inref, 0, (size_t)sz_color);
                        fill_pattern(inref, width, height, fp_color.Footprint.RowPitch);
                    }
                    for (UINT y = 0; y < height; ++y)
                    {
                        const size_t ro = (size_t)y * fp_color.Footprint.RowPitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            const unsigned char *a = pb + ro + (size_t)x * 4;
                            const unsigned char *c = ref_local + ro + (size_t)x * 4;
                            if (a[0] != c[0] || a[1] != c[1] || a[2] != c[2]) ++diff_ctrl;
                            if (a[0] == SENT_R && a[1] == SENT_G && a[2] == SENT_B) ++sent;
                            if (inref != nullptr)
                            {
                                const unsigned char *i0 = inref + ro + (size_t)x * 4;
                                if (a[0] != i0[0] || a[1] != i0[1] || a[2] != i0[2]) ++diff_in;
                            }
                        }
                    }
                    if (inref != nullptr) free(inref);
                    D3D12_RANGE nothing{0, 0};
                    rb0->Unmap(0, &nothing);

                    snprintf(line, sizeof line,
                             "[MGPU][P1.4] round trip %.2f ms | vs LOCAL NR control: differing=%llu "
                             "of %llu | vs raw input: differing=%llu | sentinel survivors=%llu",
                             p14ms, diff_ctrl, total, diff_in, sent);
                    mgpu::diag::info(line);

                    if (p14r != NVSDK_NGX_Result_Success)
                    {
                        mgpu::diag::error("[MGPU][P1.4] PROBE FAILED - EvaluateFeature refused the "
                                          "transited input. The result code above names the reason; "
                                          "the bytes below it describe a buffer NR never wrote.");
                    }
                    else if (diff_ctrl == 0 && sent == 0 && diff_in > 0)
                    {
                        mgpu::diag::info(
                            "[MGPU][P1.4] PROBE PASSED - DLSS-NR RAN ON A PAYLOAD THAT CROSSED THE "
                            "BUS AND THE RESULT CAME BACK. Byte-identical to the local NR control "
                            "at the same intensity on the same input, no sentinel survivors, and "
                            "different from the raw input - so the model processed transited data "
                            "and produced exactly what it produces without a bus. The neural stage "
                            "is decoupled from the render device end to end.");
                    }
                    else if (sent > 0)
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][P1.4] PROBE FAILED - %llu sentinel pixels survived. Part of "
                                 "the output never arrived; this is a transit or synchronisation "
                                 "fault, not a model one.", sent);
                        mgpu::diag::error(line);
                    }
                    else if (diff_in == 0)
                    {
                        mgpu::diag::error("[MGPU][P1.4] PROBE FAILED - the result is identical to the "
                                          "raw input. NR returned Success and changed nothing, or a "
                                          "copy overwrote its output.");
                    }
                    else
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][P1.4] PROBE INCONCLUSIVE - %llu of %llu pixels differ from "
                                 "the local control. NR ran on both, so this is not a transit "
                                 "corruption question alone: either the payload changed crossing the "
                                 "bus, or the model is not deterministic across these two runs. "
                                 "Re-run before interpreting; do NOT record either reading yet.",
                                 diff_ctrl, total);
                        mgpu::diag::error(line);
                    }
                }
                else mgpu::diag::error("[MGPU][P1.4] readback Map failed - no verdict possible");
            }
            else
            {
                mgpu::diag::error("[MGPU][P1.4] PROBE DID NOT COMPLETE - see the failing leg above. "
                                  "No conclusion about the architecture follows from a setup failure.");
            }

            p14_cleanup();
        }
        else
        {
            mgpu::diag::warn("[MGPU][P1.4] skipped - no local NR control was captured, or the game "
                             "adapter LUID is unknown. P1.4 without its control is not worth running.");
        }

        if (ref_local != nullptr) { free(ref_local); ref_local = nullptr; }

    }

    // ---- 8. leave nothing behind ----
    teardown(nullptr);

    mgpu::diag::info("[MGPU][P1.0c] create/teardown cycle complete - see the [MGPU][P1.2] lines "
                     "above for the parameter-liveness verdict");
    // P1 keeps its old contract (reaching here is a pass). P3 returns whether
    // the neural stage actually produced a result, because its caller branches
    // on the answer rather than just logging it.
    return P3 ? p3_ok : true;
}

// =====================================================================
// P1.3 - can a buffer cross between these two adapters, intact?
//
// This is the first milestone that involves the bus at all. Everything
// before it lived entirely on GPU 1.
//
// WHAT IT DELIBERATELY DOES NOT DO: touch the game's device. P0_RECORD
// section 01 records that the accessor reaching the game's D3D12 objects
// was never written, and this probe does not write it. It creates its OWN
// device on the game's ADAPTER instead - two D3D12 devices on one adapter
// is ordinary - so the only thing at risk is our own state. Reading the
// game's actual frame needs the game's device and belongs to P1.4; the
// question here is narrower and can be answered without it.
//
// THE GATING QUESTION. P0 measured CrossAdapterRowMajorTextureSupported = 0
// on this rig, so cross-adapter TEXTURES are unavailable. Shared BUFFERS
// with placed footprints are the documented fallback, and nobody has tested
// whether they work here. If they do not, the architecture changes shape.
//
// TWO PATHS, ONE RUN:
//   A   D3D12 shared cross-adapter buffer - CreateSharedHandle on the game
//       adapter, OpenSharedHandle on ours. The real architecture.
//   A'  A host-pinned heap: one VirtualAlloc opened as a D3D12 heap on BOTH
//       devices via OpenExistingHeapFromAddress, with a placed buffer on
//       each side. No CPU copy either - both GPUs DMA the same pages.
//
// A' runs only if A fails, and the log says which produced the result.
// They are worth comparing rather than merely ranking: cross-adapter heaps
// between unlinked adapters are widely believed to be host-memory backed
// anyway, in which case A and A' are two APIs onto the same hardware path.
// If their timings match, that is a finding about consumer multi-GPU, not
// just a fallback that happened to work.
//
// NO SHARED FENCE. Synchronisation here is a CPU wait between the two
// submissions - serialised on purpose. Pipelining is P2, and the
// cross-adapter shared-fence flag stays inside build.yml's containment
// pattern until then. This probe moves exactly four symbols out of that
// pattern: SHARED_CROSS_ADAPTER, HEAP_FLAG_SHARED, CreateSharedHandle,
// OpenSharedHandle.
//
// Do not name the still-guarded symbols in this file, even in a comment
// saying they are unused - the containment grep matches text, not code,
// and it is right to. It caught exactly that mistake on build #1 of this
// milestone.
// =====================================================================
namespace
{
    struct transit_side
    {
        ID3D12Device *dev = nullptr;
        ID3D12CommandQueue *queue = nullptr;
        ID3D12CommandAllocator *alloc = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        ID3D12Fence *fence = nullptr;
        HANDLE event = nullptr;
        UINT64 fence_value = 0;
    };

    // P1.3c. THERE IS DELIBERATELY NO EnableDebugLayer HERE. The P1.3b build
    // called ID3D12Debug::EnableDebugLayer() from inside the bridge thread,
    // long after the process had live D3D12 devices, and the run at 16:06:56
    // went: debug layer ENABLED at :750 -> both D3D12CreateDevice calls fail
    // with DXGI_ERROR_DEVICE_RESET at :751 and :752 -> the ALREADY-RUNNING
    // GPU 1 present-chain device is reported removed at :759, reason
    // DXGI_ERROR_DEVICE_RESET. Nine milliseconds, one reason code, every
    // device in the process.
    //
    // That is out of contract and it was my mistake: the debug layer is
    // documented as something you enable BEFORE any device exists in the
    // process. The comment I wrote claiming it "affects ONLY devices created
    // after this call" was an assumption stated as a fact, and the rig
    // disproved it. Do not reintroduce this call. If validation messages are
    // ever genuinely needed, they must come from a process that enabled the
    // layer at startup - not from a hook that switches it on mid-flight.

    // P1.3e. Four rows: resource flag NONE / ALLOW_CROSS_ADAPTER, crossed with
    // initial state COMMON / GENERIC_READ. Every row is logged with its own hr,
    // and the first success wins - here that IS safe, because unlike the A.1
    // matrix none of these rows produces a resource that is unfit for purpose;
    // they are four spellings of the same intent.
    HRESULT transit_place_matrix(ID3D12Device *dev, ID3D12Heap *heap,
                                 D3D12_RESOURCE_DESC desc, ID3D12Resource **out,
                                 UINT width, UINT height, const char *step, const char *side)
    {
        struct row { const char *name; D3D12_RESOURCE_FLAGS rf; D3D12_RESOURCE_STATES st; };
        const row ROWS[] = {
            {"flags=NONE state=COMMON",
             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON},
            {"flags=ALLOW_CROSS_ADAPTER state=COMMON",
             D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON},
            {"flags=NONE state=GENERIC_READ",
             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ},
            {"flags=ALLOW_CROSS_ADAPTER state=GENERIC_READ",
             D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_GENERIC_READ},
        };
        char line[1000];
        HRESULT last = E_FAIL;
        for (int i = 0; i < 4; ++i)
        {
            desc.Flags = ROWS[i].rf;
            last = dev->CreatePlacedResource(heap, 0, &desc, ROWS[i].st, nullptr,
                                             IID_PPV_ARGS(out));
            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u %s CreatePlacedResource on GPU %s, %s: hr=0x%08X%s",
                     width, height, step, side, ROWS[i].name, (unsigned)last,
                     SUCCEEDED(last) ? "  <- ACCEPTED" : "");
            mgpu::diag::info(line);
            if (SUCCEEDED(last) && *out != nullptr) return last;
            if (*out != nullptr) { (*out)->Release(); *out = nullptr; }
        }
        return last;
    }

    // Kept, but expected to do nothing. ID3D12InfoQueue only exists on a
    // device created while the debug layer was active, and P1.3c does not
    // activate it (see above), so the QueryInterface below normally fails and
    // this returns silently. It stays in the source because it costs one
    // failed QI per failure path and it is the correct reader if this probe
    // is ever run under an externally-enabled layer - PIX, or a launcher that
    // sets the layer before the process starts a device.
    void transit_drain_info_queue(ID3D12Device *dev, const char *tag)
    {
        if (dev == nullptr) return;
        ID3D12InfoQueue *iq = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(ID3D12InfoQueue),
                                       reinterpret_cast<void **>(&iq))) || iq == nullptr)
            return;
        const UINT64 n = iq->GetNumStoredMessages();
        char line[1000];
        for (UINT64 i = 0; i < n; ++i)
        {
            SIZE_T len = 0;
            if (FAILED(iq->GetMessage(i, nullptr, &len)) || len == 0) continue;
            D3D12_MESSAGE *m = static_cast<D3D12_MESSAGE *>(malloc(len));
            if (m == nullptr) continue;
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription != nullptr)
            {
                snprintf(line, sizeof line, "[MGPU][P1.3][D3D12:%s] sev=%d id=%d %s",
                         tag, (int)m->Severity, (int)m->ID, m->pDescription);
                mgpu::diag::info(line);
            }
            free(m);
        }
        iq->ClearStoredMessages();
        iq->Release();
    }

    // A device of our own on the adapter with this LUID.
    HRESULT transit_make_device(LUID want, ID3D12Device **out)
    {
        IDXGIFactory2 *factory = nullptr;
        HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2),
                                        reinterpret_cast<void **>(&factory));
        if (FAILED(hr)) return hr;
        IDXGIAdapter1 *found = nullptr;
        for (UINT i = 0; ; ++i)
        {
            IDXGIAdapter1 *a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d)) &&
                d.AdapterLuid.LowPart == want.LowPart &&
                d.AdapterLuid.HighPart == want.HighPart)
            { found = a; break; }
            a->Release();
        }
        if (found == nullptr) { factory->Release(); return DXGI_ERROR_NOT_FOUND; }
        hr = D3D12CreateDevice(static_cast<IUnknown *>(found), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(out));
        found->Release();
        factory->Release();
        return hr;
    }

    void transit_side_release(transit_side &s, bool release_device)
    {
        if (s.event != nullptr) { CloseHandle(s.event); s.event = nullptr; }
        if (s.fence != nullptr) { s.fence->Release(); s.fence = nullptr; }
        if (s.list  != nullptr) { s.list->Release();  s.list  = nullptr; }
        if (s.alloc != nullptr) { s.alloc->Release(); s.alloc = nullptr; }
        if (s.queue != nullptr) { s.queue->Release(); s.queue = nullptr; }
        if (release_device && s.dev != nullptr) { s.dev->Release(); s.dev = nullptr; }
    }

    // Queue, allocator, list, fence, event on an existing device. The list
    // comes back CLOSED so every use is Reset -> record -> Close -> submit.
    HRESULT transit_side_init(transit_side &s)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = s.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue));
        if (SUCCEEDED(hr))
            hr = s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&s.alloc));
        if (SUCCEEDED(hr))
            hr = s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc,
                                          nullptr, IID_PPV_ARGS(&s.list));
        if (SUCCEEDED(hr)) hr = s.list->Close();
        if (SUCCEEDED(hr))
            hr = s.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
        if (SUCCEEDED(hr))
        {
            s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (s.event == nullptr) hr = E_FAIL;
        }
        return hr;
    }

    static double qpc_ms(const LARGE_INTEGER &a, const LARGE_INTEGER &b,
                         const LARGE_INTEGER &freq)
    {
        return (freq.QuadPart > 0)
            ? ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart) : 0.0;
    }

    // Close, submit, wait. Bounded - a hang here would take the bridge
    // thread with it, and a timeout is a result we can print.
    //
    // P1.4a. `submit_ms` and `wait_ms` split this in two, and that split is
    // the whole point of this build. 36 samples put the round trip at a
    // ~6.6 ms fixed cost plus ~2.29 ms/MiB, and the fixed part decides the
    // architecture: if it lives in the WAIT it is our synchronisation and
    // pipelining deletes it; if it lives in the SUBMIT or the copies it is
    // real work and it does not.
    //
    // What these two numbers are, precisely: `submit_ms` is Close +
    // ExecuteCommandLists + Signal - CPU time spent handing work to the
    // driver, not GPU time. `wait_ms` is wall-clock from the signal being
    // queued to the fence event firing, so it contains the GPU's execution
    // AND any queue latency in front of it. Neither is a GPU timestamp;
    // separating execution from queueing needs timestamp queries, which stay
    // in P2. State that when quoting either number.
    HRESULT transit_flush(transit_side &s, DWORD timeout_ms,
                          double *submit_ms = nullptr, double *wait_ms = nullptr)
    {
        LARGE_INTEGER f{}, a{}, b{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);

        HRESULT hr = s.list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList *const lists[1] = { s.list };
        s.queue->ExecuteCommandLists(1, lists);
        s.fence_value++;
        hr = s.queue->Signal(s.fence, s.fence_value);
        if (FAILED(hr)) return hr;
        s.fence->SetEventOnCompletion(s.fence_value, s.event);

        QueryPerformanceCounter(&b);
        const bool ok = (WaitForSingleObject(s.event, timeout_ms) == WAIT_OBJECT_0);
        QueryPerformanceCounter(&c);

        if (submit_ms != nullptr) *submit_ms = qpc_ms(a, b, f);
        if (wait_ms   != nullptr) *wait_ms   = qpc_ms(b, c, f);
        return ok ? S_OK : E_FAIL;
    }
}

bool transit_probe(const char *tag)
{
    if (tag == nullptr) tag = "unlabelled";

    // P1.4b. ALTERNATE THE PATH BETWEEN INVOCATIONS. Until now A' only ran
    // when A failed, so once A started working A' stopped being sampled at
    // all and the two were never compared with n>1 on the same link. Odd
    // invocations force A', even ones take A first. Press the hotkey an even
    // number of times and the samples are paired.
    //
    // Why this is the question now rather than "is transit viable": on the
    // FIRST development machine the link was PCIe 3.0 x2 on chipset lanes,
    // ~1.6 GB/s usable (D3: stated as history, because it is false on the
    // second machine - CPU lanes at PCIe 5.0 x8 - and unknowable for anyone
    // else). GPU 0 moves ~28 MiB
    // over that link per 1440p run - about 17.5 ms of pure link time - so the
    // ~35 ms observed is within 2x of the theoretical floor and is a property
    // of THIS rig's interconnect, not of the architecture. Absolute numbers
    // here do not decide whether the feature is worth building. What they can
    // decide is which pipeline to build around, because A and A' are being
    // measured over the identical link and the comparison between them
    // survives the link being slow.
    static unsigned s_invocation = 0;
    const bool force_a_prime = ((++s_invocation) & 1u) != 0;
    // P1.3g. Every transit line from here on is preceded by this, so a log
    // holding several runs can never have two of them confused. The foreground
    // window is part of the record because it is a variable we do not control
    // and cannot recover after the fact: a run taken with the game focused, the
    // bridge window focused, or the desktop focused are three different
    // measurements, and only this line says which one happened.
    {
        char ctx[900];
        const HWND fg_w = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg_w != nullptr) GetWindowThreadProcessId(fg_w, &fg_pid);
        wchar_t cls_w[64] = {};
        if (fg_w != nullptr) GetClassNameW(fg_w, cls_w, 64);
        char cls[128] = "";
        WideCharToMultiByte(CP_UTF8, 0, cls_w, -1, cls, (int)sizeof cls, nullptr, nullptr);
        const char *owner = "none";
        if (fg_w != nullptr)
            owner = (fg_pid == GetCurrentProcessId()) ? "this process"
                                                      : "another process (game/desktop/shell)";
        snprintf(ctx, sizeof ctx,
                 "[MGPU][P1.3] ===== RUN \"%s\" ===== foreground hwnd=0x%p owner=%s class=\"%s\" "
                 "uptime=%lu ms. A \"startup\" run is contaminated by construction (menu, shaders "
                 "compiling) and its timings are an upper bound only. A manual run is quieter but "
                 "is STILL not a performance figure: no shared fence, no pipelining, CPU-serialised "
                 "round trip. What manual runs buy is repetition and a controlled foreground - n>1 "
                 "in one launch, and a named focus condition per sample.",
                 tag, (void *)fg_w, owner, cls, (unsigned long)GetTickCount());
        mgpu::diag::info(ctx);
    }

    auto &S = st();
    // P2.1 widened this from 700: the ring's comparison line carries two
    // arms, four counts and a ratio, and a silently truncated verdict is
    // worse than no verdict.
    char line[1400];

    LUID luid1{}, luid0{};
    bool have_game_luid = false;
    {
        std::lock_guard<std::mutex> lk(S.cs);
        luid1 = S.device_luid;
        luid0 = S.game_luid;
        have_game_luid = S.game_luid_known && S.device != nullptr;
    }
    if (!have_game_luid)
    {
        mgpu::diag::warn("[MGPU][P1.3] no GPU 1 device or no known game luid - transit probe "
                         "skipped");
        return false;
    }

    transit_side g0;   // our own device on the GAME's adapter
    transit_side g1;   // our own device on OUR adapter

    // ---- 1. two devices of our own, one per adapter ----
    //
    // BOTH are created here, including the one on our own adapter. P1.3's
    // first version borrowed the present chain's GPU 1 device; this one does
    // not. It isolates the probe completely: a transit failure cannot disturb
    // P0's device, and a capability answer from two fresh devices holds for
    // the real ones.
    mgpu::diag::info("[MGPU][P1.3] no debug layer (see comment above transit_drain_info_queue) - "
                     "the split per-call HRESULTs below are the instrument.");

    {
        HRESULT hr0 = transit_make_device(luid0, &g0.dev);
        HRESULT hr1 = transit_make_device(luid1, &g1.dev);
        snprintf(line, sizeof line,
                 "[MGPU][P1.3] own devices: gpu0(game adapter) hr=0x%08X luid=%08lX-%08lX | "
                 "gpu1 hr=0x%08X luid=%08lX-%08lX (the game's own device is NOT touched)",
                 (unsigned)hr0, (unsigned long)luid0.HighPart, (unsigned long)luid0.LowPart,
                 (unsigned)hr1, (unsigned long)luid1.HighPart, (unsigned long)luid1.LowPart);
        mgpu::diag::info(line);
        if (FAILED(hr0) || FAILED(hr1))
        {
            transit_side_release(g1, true);
            transit_side_release(g0, true);
            return false;
        }

        HRESULT hr = transit_side_init(g0);
        if (SUCCEEDED(hr)) hr = transit_side_init(g1);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][P1.3] command objects hr=0x%08X", (unsigned)hr);
            mgpu::diag::error(line);
            transit_side_release(g1, true);
            transit_side_release(g0, true);
            return false;
        }
    }

    // Existing-heaps support decides whether A' is even available. Report it
    // up front rather than discovering it in a failure path.
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh0{}, eh1{};
    g0.dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh0, sizeof eh0);
    g1.dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh1, sizeof eh1);
    snprintf(line, sizeof line,
             "[MGPU][P1.3] ExistingHeaps support: gpu0=%d gpu1=%d (path A' needs both)",
             eh0.Supported ? 1 : 0, eh1.Supported ? 1 : 0);
    mgpu::diag::info(line);

    // ---- 2. two payload sizes ----
    //
    // Not gold-plating: one size gives a number that startup contamination
    // makes unquotable. TWO sizes in the same run give a SLOPE, and the
    // slope survives contamination that the intercept does not. 1280x720 is
    // what every earlier milestone used; 2560x1440 is the game's real
    // swapchain extent, which is what P1.4 will actually move.
    const UINT sizes[2][2] = { {1280, 720}, {2560, 1440} };
    bool all_ok = true;

    for (int si = 0; si < 2; ++si)
    {
        const UINT width = sizes[si][0], height = sizes[si][1];

        ID3D12Resource *tex0 = nullptr, *tex1 = nullptr;
        ID3D12Resource *upload0 = nullptr, *read1 = nullptr;
        ID3D12Resource *shared0 = nullptr, *shared1 = nullptr;
        ID3D12Heap *heap0 = nullptr, *heap1 = nullptr;
        HANDLE sh = nullptr;
        void *pinned = nullptr;
        const char *path = "none";

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes = 0, rowb = 0; UINT rows = 0;

        auto cleanup = [&]()
        {
            if (read1   != nullptr) read1->Release();
            if (upload0 != nullptr) upload0->Release();
            if (tex1    != nullptr) tex1->Release();
            if (tex0    != nullptr) tex0->Release();
            if (shared1 != nullptr) shared1->Release();
            if (shared0 != nullptr) shared0->Release();
            if (heap1   != nullptr) heap1->Release();
            if (heap0   != nullptr) heap0->Release();
            if (sh      != nullptr) CloseHandle(sh);
            if (pinned  != nullptr) VirtualFree(pinned, 0, MEM_RELEASE);
        };

        HRESULT hr = make_tex(g0.dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, &tex0);
        if (SUCCEEDED(hr))
            hr = make_tex(g1.dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex1);
        if (FAILED(hr)) { cleanup(); all_ok = false; continue; }

        D3D12_RESOURCE_DESC td = tex0->GetDesc();
        g0.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowb, &bytes);

        // ---- PATH A: shared cross-adapter buffer ----
        //
        // Cross-adapter resources must be buffers, ROW_MAJOR, and flagged
        // ALLOW_CROSS_ADAPTER. They are implicitly simultaneous-access, so
        // no barriers are recorded against them anywhere below - a
        // transition on a cross-adapter resource is a debug-layer error,
        // not an optimisation.
        //
        // EVERY CALL IS LOGGED SEPARATELY. P1.3's first version chained
        // three calls into one HRESULT and reported 0x80070057 without
        // saying which produced it - and the distinction is the whole
        // question. A rejection at CreateCommittedResource is a parameter
        // mistake we can fix; a rejection at OpenSharedHandle is the
        // RECEIVING adapter refusing the handle, which is a capability
        // answer. Collapsing them made those indistinguishable.
        //
        // Sizes are rounded up to 64 KB. Cross-adapter placement alignment
        // is 64 KB, and 1280x720x4 = 3,686,400 is 56.25 of those - not an
        // integer multiple. 2560x1440x4 = 14,745,600 IS exactly 225, and it
        // failed identically, so alignment cannot be the whole story. It is
        // corrected because it is cheap and correct, not because it is the
        // diagnosis.
        if (!force_a_prime)
        {
            const UINT64 ALIGN = 65536;
            const UINT64 shared_bytes = ((bytes + ALIGN - 1) / ALIGN) * ALIGN;

            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            // Documented as equivalent to 1 when left at 0. Set explicitly so
            // the echo below reports a value we chose rather than a default we
            // are assuming the meaning of.
            hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Alignment = 0;
            bd.Width = shared_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.SampleDesc.Quality = 0;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            // Echo the descriptor we are about to submit, field by field, as
            // NUMBERS rather than as a claim in a comment. Every cross-adapter
            // buffer requirement is visible in this one line, so a future
            // reading of the log can settle "did we set that" without reading
            // this source, and without anyone having to be believed.
            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u A.0 desc echo: dim=%d(BUFFER=1) w=%llu (%llu x 64KB, "
                     "exact=%s) h=%u depth=%u mips=%u fmt=%d(UNKNOWN=0) samples=%u layout=%d"
                     "(ROW_MAJOR=1) resFlags=0x%X(ALLOW_CROSS_ADAPTER=0x%X) heapType=%d(DEFAULT=1) "
                     "nodeMask=%u/%u heapFlags=0x%X(SHARED=0x%X|SHARED_CROSS_ADAPTER=0x%X)",
                     width, height, (int)bd.Dimension, (unsigned long long)bd.Width,
                     (unsigned long long)(shared_bytes / 65536),
                     (shared_bytes % 65536) == 0 ? "yes" : "NO",
                     (unsigned)bd.Height, (unsigned)bd.DepthOrArraySize, (unsigned)bd.MipLevels,
                     (int)bd.Format, (unsigned)bd.SampleDesc.Count, (int)bd.Layout,
                     (unsigned)bd.Flags, (unsigned)D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER,
                     (int)hp.Type, (unsigned)hp.CreationNodeMask, (unsigned)hp.VisibleNodeMask,
                     (unsigned)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
                     (unsigned)D3D12_HEAP_FLAG_SHARED,
                     (unsigned)D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
            mgpu::diag::info(line);

            // P1.3d. The P1.3c run answered the question P1.3b could not: A.1
            // itself fails with E_INVALIDARG, BEFORE any sharing is attempted.
            // Nothing in that is a statement about the adapters - the runtime
            // rejected our creation parameters, and every one of the five
            // "cross-adapter validation requirements" was already satisfied
            // (see the A.0 echo on the line above; it prints them as numbers).
            //
            // So stop reasoning about which flag is wrong and ask the runtime,
            // one flag at a time. Five variants, each logged with its own
            // hr; the first that succeeds carries on into A.2/A.3. The
            // DIFFERENCE between two adjacent rows is the finding - if V1
            // fails and V2 succeeds, ALLOW_CROSS_ADAPTER on a buffer is what
            // the runtime dislikes, and we will know it rather than suspect it.
            //
            // V5 is the one I would bet on, and it is a structural difference
            // rather than a flag tweak: Microsoft's own cross-adapter sample
            // creates the heap explicitly with CreateHeap and then places the
            // resource, and does NOT use CreateCommittedResource. If V1-V4 all
            // fail and V5 succeeds, then SHARED_CROSS_ADAPTER is simply not
            // supported on the committed path and the architecture uses an
            // explicit heap. That is a real possibility, not a certainty, and
            // it is exactly what this matrix is for.
            struct a1_variant
            {
                const char *name;
                D3D12_HEAP_FLAGS heap_flags;
                D3D12_RESOURCE_FLAGS res_flags;
                bool placed;   // true = CreateHeap + CreatePlacedResource
            };
            const a1_variant VARIANTS[] = {
                {"V1 committed SHARED|SHARED_CROSS_ADAPTER + ALLOW_CROSS_ADAPTER",
                 (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
                 D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, false},
                {"V2 committed SHARED|SHARED_CROSS_ADAPTER + no resource flag",
                 (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
                 D3D12_RESOURCE_FLAG_NONE, false},
                {"V3 committed SHARED only + ALLOW_CROSS_ADAPTER",
                 D3D12_HEAP_FLAG_SHARED,
                 D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, false},
                {"V4 committed SHARED only + no resource flag",
                 D3D12_HEAP_FLAG_SHARED,
                 D3D12_RESOURCE_FLAG_NONE, false},
                {"V5 CreateHeap(SHARED|SHARED_CROSS_ADAPTER) + CreatePlacedResource",
                 (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
                 D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, true},
            };

            // P1.3e. The P1.3d loop stopped at the FIRST variant that returned
            // S_OK, and that was a design error of mine. V4 succeeded - and V4
            // is the row with no cross-adapter tokens at all, so it handed the
            // rest of path A a resource that was never eligible to cross an
            // adapter in the first place. A.3 then failed, on a log line that
            // calls itself "the call that answers whether the adapters can
            // share". It answered nothing of the kind. A plain SHARED handle
            // crosses DEVICES, not ADAPTERS; refusing it on the second adapter
            // is the specified behaviour, not a capability verdict.
            //
            // So: run ALL five rows, log all five, and then prefer a success
            // that is actually cross-adapter eligible over one that merely
            // returned S_OK. A row that cannot cross is not a fallback, it is
            // a different experiment.
            HRESULT a1 = E_FAIL;
            const char *a1_won = "none";
            bool a1_eligible = false;
            ID3D12Resource *shared0_keep = nullptr;
            // P1.3f. V5's heap must OUTLIVE the loop now. A placed resource is
            // not itself shareable: for a resource in a shared heap, D3D12
            // shares the HEAP and the other adapter places its own resource
            // into it. P1.3e released the heap immediately after placing, so
            // A.2 had nothing correct to share and was handed the resource.
            ID3D12Heap *heapA = nullptr, *heapA_keep = nullptr;
            for (int v = 0; v < 5; ++v)
            {
                if (shared0 != nullptr && a1_eligible) break;   // best possible already held

                const a1_variant &V = VARIANTS[v];
                bd.Flags = V.res_flags;
                HRESULT hv = E_FAIL;

                if (!V.placed)
                {
                    hv = g0.dev->CreateCommittedResource(&hp, V.heap_flags, &bd,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&shared0));
                }
                else
                {
                    D3D12_HEAP_DESC hd{};
                    hd.SizeInBytes = shared_bytes;
                    hd.Properties = hp;
                    hd.Alignment = 65536;
                    hd.Flags = V.heap_flags;

                    ID3D12Heap *ha = nullptr;
                    hv = g0.dev->CreateHeap(&hd, IID_PPV_ARGS(&ha));
                    heapA = ha;
                    snprintf(line, sizeof line,
                             "[MGPU][P1.3] %ux%u   A.1 %s -> CreateHeap hr=0x%08X",
                             width, height, V.name, (unsigned)hv);
                    mgpu::diag::info(line);
                    if (SUCCEEDED(hv) && ha != nullptr)
                    {
                        hv = g0.dev->CreatePlacedResource(ha, 0, &bd,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&shared0));
                        // Our reference is handed to heapA_keep below if this
                        // row wins; otherwise it is dropped with the row.
                    }
                }

                snprintf(line, sizeof line,
                         "[MGPU][P1.3] %ux%u A.1 %s: hr=0x%08X%s",
                         width, height, V.name, (unsigned)hv,
                         SUCCEEDED(hv) ? "  <- ACCEPTED" : "");
                mgpu::diag::info(line);

                const bool eligible =
                    (V.heap_flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) != 0;

                if (SUCCEEDED(hv) && shared0 != nullptr)
                {
                    if (!a1_eligible || eligible)
                    {
                        if (shared0_keep != nullptr) shared0_keep->Release();
                        if (heapA_keep != nullptr) heapA_keep->Release();
                        shared0_keep = shared0;
                        heapA_keep = heapA; heapA = nullptr;
                        a1 = hv; a1_won = V.name; a1_eligible = eligible;
                    }
                    else { shared0->Release(); }
                    shared0 = nullptr;
                }
                else if (shared0 != nullptr) { shared0->Release(); shared0 = nullptr; }

                if (heapA != nullptr) { heapA->Release(); heapA = nullptr; }
            }
            shared0 = shared0_keep; shared0_keep = nullptr;

            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u A.1 verdict: %s | cross-adapter eligible=%s "
                     "(payload=%llu padded=%llu rowPitch=%u). READ THIS BEFORE READING A.3: if "
                     "eligible=NO, then whatever A.3 returns says nothing about the adapters - "
                     "the resource was never created able to cross one. Only an eligible=YES "
                     "winner makes A.3 a capability answer.",
                     width, height, a1_won, a1_eligible ? "YES" : "NO",
                     (unsigned long long)bytes,
                     (unsigned long long)shared_bytes, (unsigned)fp.Footprint.RowPitch);
            mgpu::diag::info(line);
            if (FAILED(a1)) transit_drain_info_queue(g0.dev, "gpu0 after A.1");

            // Restore the descriptor the rest of path A expects.
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            // P1.3f. SHARE THE HEAP, NOT THE RESOURCE. The 17:01 run reached
            // A.2 with an eligible=YES winner for the first time and got
            // E_INVALIDARG - and that is our mistake once more, not a verdict.
            // A committed resource carries its own implicit heap and can be
            // shared directly; a PLACED resource cannot, because the thing that
            // owns the memory is the heap. The documented cross-adapter shape is
            // share the heap, open it on the second adapter, and place a
            // matching resource into it there.
            //
            // So A.2/A.3 now operate on heapA_keep when V5 won, and fall back to
            // the resource only when a committed row won. The log says which,
            // because "which object did we hand it" is exactly the kind of
            // detail that turns into a wrong conclusion three days later.
            HRESULT a2 = a1, a3 = a1;
            ID3D12Heap *heap1_opened = nullptr;
            const bool share_heap = (heapA_keep != nullptr);
            if (SUCCEEDED(a1))
            {
                ID3D12DeviceChild *to_share = share_heap
                    ? static_cast<ID3D12DeviceChild *>(heapA_keep)
                    : static_cast<ID3D12DeviceChild *>(shared0);
                a2 = g0.dev->CreateSharedHandle(to_share, nullptr, GENERIC_ALL, nullptr, &sh);
                snprintf(line, sizeof line,
                         "[MGPU][P1.3] %ux%u A.2 CreateSharedHandle(%s): hr=0x%08X handle=0x%p",
                         width, height, share_heap ? "HEAP" : "resource",
                         (unsigned)a2, (void *)sh);
                mgpu::diag::info(line);
                if (FAILED(a2)) transit_drain_info_queue(g0.dev, "gpu0 after A.2");
            }
            if (SUCCEEDED(a2))
            {
                if (share_heap)
                {
                    a3 = g1.dev->OpenSharedHandle(sh, IID_PPV_ARGS(&heap1_opened));
                    snprintf(line, sizeof line,
                             "[MGPU][P1.3] %ux%u A.3 OpenSharedHandle as HEAP on GPU 1: "
                             "hr=0x%08X heap=0x%p <- THE capability answer: an eligible "
                             "cross-adapter heap presented to the second adapter",
                             width, height, (unsigned)a3, (void *)heap1_opened);
                    mgpu::diag::info(line);
                    if (SUCCEEDED(a3) && heap1_opened != nullptr)
                    {
                        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
                        a3 = g1.dev->CreatePlacedResource(heap1_opened, 0, &bd,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&shared1));
                        snprintf(line, sizeof line,
                                 "[MGPU][P1.3] %ux%u A.3b CreatePlacedResource in the opened "
                                 "heap on GPU 1: hr=0x%08X res=0x%p",
                                 width, height, (unsigned)a3, (void *)shared1);
                        mgpu::diag::info(line);
                    }
                }
                else
                {
                    a3 = g1.dev->OpenSharedHandle(sh, IID_PPV_ARGS(&shared1));
                    snprintf(line, sizeof line,
                             "[MGPU][P1.3] %ux%u A.3 OpenSharedHandle as resource on GPU 1: "
                             "hr=0x%08X res=0x%p <- a capability answer ONLY if the A.1 verdict "
                             "above says eligible=YES", width, height, (unsigned)a3,
                             (void *)shared1);
                    mgpu::diag::info(line);
                }
                if (FAILED(a3)) transit_drain_info_queue(g1.dev, "gpu1 after A.3");
            }

            if (heap1_opened != nullptr) heap1_opened->Release();
            if (heapA_keep != nullptr) heapA_keep->Release();

            if (SUCCEEDED(a3) && shared1 != nullptr) { path = "A(shared cross-adapter)"; }
            else
            {
                if (shared1 != nullptr) { shared1->Release(); shared1 = nullptr; }
                if (sh != nullptr) { CloseHandle(sh); sh = nullptr; }
                if (shared0 != nullptr) { shared0->Release(); shared0 = nullptr; }
                mgpu::diag::warn("[MGPU][P1.3] path A did not complete - trying A' (host-pinned "
                                 "heap opened on both devices)");
            }
        }

        // ---- PATH A': one VirtualAlloc, opened as a heap on both devices ----
        // Reached either because A failed, or because this invocation forced it.
        if (shared1 == nullptr && eh0.Supported && eh1.Supported)
        {
            // Five calls, five log lines, same reasoning as path A. The
            // VirtualAlloc region is rounded to 64 KB rather than to the
            // page size: a D3D12 heap's size must be a multiple of 64 KB,
            // and Windows reserves at 64 KB granularity anyway, so the
            // earlier page rounding could hand OpenExistingHeapFromAddress
            // a region it was never allowed to accept.
            const UINT64 ALIGN = 65536;
            const SIZE_T alloc_bytes = (SIZE_T)(((bytes + ALIGN - 1) / ALIGN) * ALIGN);
            pinned = VirtualAlloc(nullptr, alloc_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Alignment = 0;
            bd.Width = alloc_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.SampleDesc.Quality = 0;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u A'.0 VirtualAlloc: addr=0x%p bytes=%llu (payload %llu, "
                     "rounded to 64 KB)",
                     width, height, pinned, (unsigned long long)alloc_bytes,
                     (unsigned long long)bytes);
            mgpu::diag::info(line);

            ID3D12Device3 *dev0_3 = nullptr, *dev1_3 = nullptr;
            HRESULT b = (pinned != nullptr) ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(b))
                b = g0.dev->QueryInterface(__uuidof(ID3D12Device3),
                                           reinterpret_cast<void **>(&dev0_3));
            if (SUCCEEDED(b))
                b = g1.dev->QueryInterface(__uuidof(ID3D12Device3),
                                           reinterpret_cast<void **>(&dev1_3));
            snprintf(line, sizeof line, "[MGPU][P1.3] %ux%u A'.1 QueryInterface(ID3D12Device3): "
                     "hr=0x%08X", width, height, (unsigned)b);
            mgpu::diag::info(line);

            if (SUCCEEDED(b))
            {
                b = dev0_3->OpenExistingHeapFromAddress(pinned, IID_PPV_ARGS(&heap0));
                snprintf(line, sizeof line,
                         "[MGPU][P1.3] %ux%u A'.2 OpenExistingHeapFromAddress on GPU 0: hr=0x%08X",
                         width, height, (unsigned)b);
                mgpu::diag::info(line);
                if (FAILED(b)) transit_drain_info_queue(g0.dev, "gpu0 after A'.2");
            }
            if (SUCCEEDED(b))
            {
                b = dev1_3->OpenExistingHeapFromAddress(pinned, IID_PPV_ARGS(&heap1));
                snprintf(line, sizeof line,
                         "[MGPU][P1.3] %ux%u A'.3 OpenExistingHeapFromAddress on GPU 1: hr=0x%08X "
                         "<- same host pages, second adapter",
                         width, height, (unsigned)b);
                mgpu::diag::info(line);
                if (FAILED(b)) transit_drain_info_queue(g1.dev, "gpu1 after A'.3");

                // P1.3d. We do not get to choose this heap's properties - the
                // runtime derived them from the host allocation - so we were
                // placing a resource into a heap whose type, CPU page property
                // and memory pool we had never looked at. Ask it. This is free,
                // needs no debug layer, and it is the difference between
                // knowing what we are placing into and assuming.
                if (SUCCEEDED(b) && heap0 != nullptr && heap1 != nullptr)
                {
                    const D3D12_HEAP_DESC h0 = heap0->GetDesc();
                    const D3D12_HEAP_DESC h1 = heap1->GetDesc();
                    snprintf(line, sizeof line,
                             "[MGPU][P1.3] %ux%u A'.3b heap desc: gpu0 size=%llu align=%llu "
                             "type=%d(DEFAULT=1,UPLOAD=2,READBACK=3,CUSTOM=4) cpuPage=%d "
                             "pool=%d(L0=1,L1=2) flags=0x%X | gpu1 size=%llu align=%llu type=%d "
                             "cpuPage=%d pool=%d flags=0x%X",
                             width, height,
                             (unsigned long long)h0.SizeInBytes, (unsigned long long)h0.Alignment,
                             (int)h0.Properties.Type, (int)h0.Properties.CPUPageProperty,
                             (int)h0.Properties.MemoryPoolPreference, (unsigned)h0.Flags,
                             (unsigned long long)h1.SizeInBytes, (unsigned long long)h1.Alignment,
                             (int)h1.Properties.Type, (int)h1.Properties.CPUPageProperty,
                             (int)h1.Properties.MemoryPoolPreference, (unsigned)h1.Flags);
                    mgpu::diag::info(line);
                }
            }
            if (SUCCEEDED(b))
            {
                // P1.3d. A'.4 was the ONLY failing call left on this path, and
                // it failed with COMMON. A heap opened from host memory is
                // CPU-accessible, and D3D12 constrains the initial state of a
                // resource placed in a CPU-accessible heap. So try COMMON, then
                // GENERIC_READ, and log both - if the second succeeds where the
                // first failed, the rule is named by the log rather than by me.
                // P1.3e. A'.3b earned its keep: the heap the RUNTIME built for us
                // came back flags=0x421 - SHARED(0x1) | SHARED_CROSS_ADAPTER(0x20)
                // | ALLOW_SHADER_ATOMICS(0x400). We never asked for
                // SHARED_CROSS_ADAPTER; OpenExistingHeapFromAddress set it
                // itself. And D3D12 pairs those: a resource placed in a heap
                // carrying SHARED_CROSS_ADAPTER is expected to carry
                // ALLOW_CROSS_ADAPTER. Our descriptor had Flags = NONE, which
                // is very likely the whole of A'.4's E_INVALIDARG.
                //
                // Note what this means for the A.1 result. The runtime refuses
                // SHARED_CROSS_ADAPTER when WE ask for it on a committed
                // resource, and sets it itself on a heap it builds. That is not
                // "cross-adapter is unsupported on this rig" - it is narrower
                // and more interesting, and it is why the matrix below varies
                // the flag rather than assuming either answer.
                b = transit_place_matrix(g0.dev, heap0, bd, &shared0, width, height, "A'.4", "0");
                if (FAILED(b)) transit_drain_info_queue(g0.dev, "gpu0 after A'.4");
            }
            if (SUCCEEDED(b))
            {
                b = transit_place_matrix(g1.dev, heap1, bd, &shared1, width, height, "A'.5", "1");
                if (FAILED(b)) transit_drain_info_queue(g1.dev, "gpu1 after A'.5");
            }

            if (dev1_3 != nullptr) dev1_3->Release();
            if (dev0_3 != nullptr) dev0_3->Release();
            if (SUCCEEDED(b)) path = "A'(host-pinned)";
        }

        if (shared0 == nullptr || shared1 == nullptr)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u NEITHER PATH PRODUCED A SHARED RESOURCE. Read the "
                     "per-call hr lines above before drawing any conclusion: E_INVALIDARG "
                     "(0x80070057) is the runtime rejecting a parameter WE supplied, not the "
                     "adapters refusing to share. As of the 16:16 run, A'.2 and A'.3 both "
                     "SUCCEEDED - both adapters opened the same host pages as a heap - so the "
                     "only call on this path that could have been a hardware or driver refusal "
                     "has already passed. Everything failing below that line is a parameter "
                     "mistake of ours and is fixable. Path A has produced no evidence about the "
                     "adapters at all: it dies at creation, before sharing is attempted.",
                     width, height);
            mgpu::diag::error(line);
            cleanup(); all_ok = false; continue;
        }

        // ---- upload the pattern on GPU 0, and sentinel GPU 1's texture ----
        const unsigned char SENT_R = 0x10, SENT_G = 0x20, SENT_B = 0xF0;
        {
            hr = make_buf(g0.dev, bytes, D3D12_HEAP_TYPE_UPLOAD, &upload0);
            if (SUCCEEDED(hr)) hr = make_buf(g1.dev, bytes, D3D12_HEAP_TYPE_READBACK, &read1);
            if (FAILED(hr)) { cleanup(); all_ok = false; continue; }

            unsigned char *m = nullptr;
            D3D12_RANGE none{0, 0};
            if (FAILED(upload0->Map(0, &none, reinterpret_cast<void **>(&m))) || m == nullptr)
            { cleanup(); all_ok = false; continue; }
            fill_pattern(m, width, height, fp.Footprint.RowPitch);
            upload0->Unmap(0, nullptr);
        }

        // ---- P1.4b: the same-adapter control ----------------------------
        // The discriminator for the 15x asymmetry between wait0 (~35 ms) and
        // wait1 (~2.25 ms) at 1440p. GPU 0 does EXACTLY the two copies it does
        // in the real measurement - upload -> tex0 -> buffer - except the
        // destination buffer is GPU-0-LOCAL. Same adapter, same contention
        // from the game, same bytes, no adapter crossing.
        //
        //   local fast, shared slow  -> the crossing is the cost
        //   both slow                -> GPU 0 is busy and it is contention
        //
        // On ANY link the crossing is expected to cost something, and on the
        // narrow chipset-fed one the first machine had it cost a great deal
        // (D3: the width is a property of the machine, not of this code);
        // this says HOW MUCH of the 35 ms is the link rather than
        // the adapter being busy, which is the number a bifurcated x8 rig
        // would change and the contention number is not.
        {
            ID3D12Resource *local0 = nullptr;
            LARGE_INTEGER lf{}, la{}, lb{};
            QueryPerformanceFrequency(&lf);
            double lsub = 0.0, lwait = 0.0;
            if (SUCCEEDED(make_buf(g0.dev, bytes, D3D12_HEAP_TYPE_DEFAULT, &local0)) &&
                local0 != nullptr)
            {
                g0.alloc->Reset(); g0.list->Reset(g0.alloc, nullptr);
                D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
                cs.pResource = upload0; cs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cs.PlacedFootprint = fp;
                cd.pResource = tex0; cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                g0.list->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                barrier(g0.list, tex0, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cs2{}, cd2{};
                cs2.pResource = tex0; cs2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cd2.pResource = local0; cd2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cd2.PlacedFootprint = fp; cd2.PlacedFootprint.Offset = 0;
                g0.list->CopyTextureRegion(&cd2, 0, 0, 0, &cs2, nullptr);

                QueryPerformanceCounter(&la);
                const HRESULT lhr = transit_flush(g0, 20000, &lsub, &lwait);
                QueryPerformanceCounter(&lb);
                snprintf(line, sizeof line,
                         "[MGPU][P1.4b] %ux%u SAME-ADAPTER CONTROL (GPU 0, identical two copies, "
                         "destination LOCAL to GPU 0): hr=0x%08X sub=%.2f wait=%.2f total=%.2f ms "
                         "for %.2f MiB. Compare against wait0 in the breakdown below: the "
                         "DIFFERENCE is what crossing the adapter costs on this link; what is "
                         "left is GPU 0 being busy with the game.",
                         width, height, (unsigned)lhr, lsub, lwait, qpc_ms(la, lb, lf),
                         (double)bytes / (1024.0 * 1024.0));
                mgpu::diag::info(line);

                // The texture goes back to COPY_DEST for the real run below.
                g0.alloc->Reset(); g0.list->Reset(g0.alloc, nullptr);
                barrier(g0.list, tex0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                (void)transit_flush(g0, 20000);
                local0->Release();
            }
            else
            {
                mgpu::diag::warn("[MGPU][P1.4b] same-adapter control buffer allocation failed - "
                                 "control skipped, the breakdown below stands alone");
            }
        }

        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        // P1.4a stage boundaries. r0/r1 bracket GPU 0's command recording,
        // r2/r3 GPU 1's; the two flushes report their own submit/wait split.
        LARGE_INTEGER r0{}, r1{}, r2{}, r3{}, c0{}, c1{};
        double sub0 = 0.0, wait0 = 0.0, sub1 = 0.0, wait1 = 0.0;

        // GPU 0: upload -> tex0 -> shared buffer.
        QueryPerformanceCounter(&r0);
        g0.alloc->Reset(); g0.list->Reset(g0.alloc, nullptr);
        {
            D3D12_TEXTURE_COPY_LOCATION s{}, d{};
            s.pResource = upload0; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp;
            d.pResource = tex0; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g0.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(g0.list, tex0, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
            s2.pResource = tex0; s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d2.pResource = shared0; d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d2.PlacedFootprint = fp; d2.PlacedFootprint.Offset = 0;
            g0.list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
        }
        QueryPerformanceCounter(&r1);
        t0 = r1;
        hr = transit_flush(g0, 20000, &sub0, &wait0);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][P1.3] %ux%u GPU0 submit/wait failed hr=0x%08X",
                     width, height, (unsigned)hr);
            mgpu::diag::error(line);
            cleanup(); all_ok = false; continue;
        }

        // GPU 1: shared buffer -> tex1 -> readback.
        QueryPerformanceCounter(&r2);
        g1.alloc->Reset(); g1.list->Reset(g1.alloc, nullptr);
        {
            D3D12_TEXTURE_COPY_LOCATION s{}, d{};
            s.pResource = shared1; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp; s.PlacedFootprint.Offset = 0;
            d.pResource = tex1; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g1.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(g1.list, tex1, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
            s2.pResource = tex1; s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d2.pResource = read1; d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d2.PlacedFootprint = fp; d2.PlacedFootprint.Offset = 0;
            g1.list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
        }
        QueryPerformanceCounter(&r3);
        hr = transit_flush(g1, 20000, &sub1, &wait1);
        QueryPerformanceCounter(&t1);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][P1.3] %ux%u GPU1 submit/wait failed hr=0x%08X",
                     width, height, (unsigned)hr);
            mgpu::diag::error(line);
            cleanup(); all_ok = false; continue;
        }

        const double ms = (f.QuadPart > 0)
            ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;

        // ---- did the bytes survive the crossing? ----
        {
            QueryPerformanceCounter(&c0);
            const unsigned char *p = nullptr;
            D3D12_RANGE all{0, (SIZE_T)bytes};
            if (FAILED(read1->Map(0, &all, (void **)&p)) || p == nullptr)
            {
                mgpu::diag::error("[MGPU][P1.3] readback Map failed - cannot verify");
                cleanup(); all_ok = false; continue;
            }
            // Regenerate the pattern on the CPU and compare. The reference
            // is deterministic, so this is an exact test, not a similarity
            // one - and a sentinel count separates "arrived wrong" from
            // "never arrived".
            unsigned char *ref = (unsigned char *)malloc((size_t)bytes);
            unsigned long long differing = 0, sentinel = 0;
            const unsigned long long total = (unsigned long long)width * height;
            if (ref != nullptr)
            {
                memset(ref, 0, (size_t)bytes);
                fill_pattern(ref, width, height, fp.Footprint.RowPitch);
                for (UINT y = 0; y < height; ++y)
                {
                    const unsigned char *ra = ref + (size_t)y * fp.Footprint.RowPitch;
                    const unsigned char *rb = p   + (size_t)y * fp.Footprint.RowPitch;
                    for (UINT x = 0; x < width; ++x)
                    {
                        const unsigned char *a = ra + (size_t)x * 4;
                        const unsigned char *b = rb + (size_t)x * 4;
                        if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) ++differing;
                        if (b[0] == SENT_R && b[1] == SENT_G && b[2] == SENT_B) ++sentinel;
                    }
                }
                free(ref);
            }
            D3D12_RANGE nothing{0, 0};
            read1->Unmap(0, &nothing);
            QueryPerformanceCounter(&c1);

            const double mib = (double)bytes / (1024.0 * 1024.0);
            snprintf(line, sizeof line,
                     "[MGPU][P1.3] %ux%u via %s: %.2f MiB round trip in %.2f ms "
                     "(%.0f MiB/s apparent) differing=%llu of %llu sentinel=%llu",
                     width, height, path, mib, ms, ms > 0.0 ? (mib / (ms / 1000.0)) : 0.0,
                     differing, total, sentinel);
            mgpu::diag::info(line);

            // P1.4a. The same round trip, decomposed. rec0/rec1 are CPU
            // command recording; sub0/sub1 are Close+Execute+Signal; wait0/
            // wait1 are fence wall-clock (GPU execution plus queue latency).
            // verify is CPU-only readback and comparison and is NOT part of
            // the round trip above - it is printed so the run's total cost
            // is accounted for rather than partly invisible.
            //
            // How to read it: rec+sub is CPU-side driver overhead that a
            // pipelined design still pays but can overlap. wait0+wait1 is the
            // part that pipelining and a shared fence are meant to remove,
            // because in the real design GPU 1 does not block on a CPU event
            // between the two halves. If wait0+wait1 dominates, the ~6.6 ms
            // fixed cost is an artefact of this probe's structure and the
            // architecture survives. If rec+sub dominates, it does not, and
            // no amount of pipelining fixes it.
            const double rec0 = qpc_ms(r0, r1, f);
            const double rec1 = qpc_ms(r2, r3, f);
            const double verify = qpc_ms(c0, c1, f);
            const double cpu_side = rec0 + rec1 + sub0 + sub1;
            const double waits = wait0 + wait1;
            snprintf(line, sizeof line,
                     "[MGPU][P1.4a] %ux%u path=%s forced=%s breakdown: rec0=%.2f sub0=%.2f "
                     "wait0=%.2f | rec1=%.2f sub1=%.2f wait1=%.2f | cpu(rec+sub)=%.2f "
                     "waits=%.2f (%.0f%% of the trip) round_trip=%.2f verify=%.2f ms. "
                     // D3. This used to assert "this rig is PCIe 3.0 x2 on "
                     // "chipset lanes" as a fact. That was true of the FIRST
                     // development machine and is false on the second (both
                     // cards on CPU lanes at PCIe 5.0 x8), and it is unknowable
                     // for whoever is reading their own log. The claim is gone;
                     // what is left is the part that is true on any link.
                     "LINK: these absolute numbers are a property of WHATEVER INTERCONNECT THIS "
                     "MACHINE HAS, which this line does not know and does not guess - check the "
                     "slot topology against your board's manual before quoting them. They do not "
                     "decide whether the architecture is viable. What they do decide is A versus "
                     "A-prime, because both are measured over the same link.",
                     width, height, path, force_a_prime ? "A-prime" : "A-first",
                     rec0, sub0, wait0, rec1, sub1, wait1,
                     cpu_side, waits, ms > 0.0 ? (waits / ms * 100.0) : 0.0, ms, verify);
            mgpu::diag::info(line);

            if (differing == 0)
                mgpu::diag::info("[MGPU][P1.3] PAYLOAD INTACT - every pixel crossed the bus "
                                 "unchanged.");
            else
            {
                all_ok = false;
                mgpu::diag::error("[MGPU][P1.3] PAYLOAD CORRUPTED - bytes crossed but do not match "
                                  "the source. Suspect the placed footprint (row pitch / offset) "
                                  "rather than the sharing mechanism.");
            }
        }

        // ================= P2.1: RING + COPY QUEUES ======================
        //
        // Everything above this line is deliberately serial: GPU 0 submits,
        // the CPU blocks on a fence event, GPU 1 then submits, the CPU blocks
        // again. That discipline was correct for P1 - it makes a wrong answer
        // impossible to mistake for a slow one - and it is also the single
        // largest artefact in every number P1 produced.
        //
        // P2.1 runs THE SAME BYTES OVER THE SAME HEAP TWICE, changing only the
        // discipline:
        //
        //   SERIAL     one band, DIRECT-equivalent ordering, a CPU fence wait
        //              between the two sides. P1.3's structure, reduced to
        //              buffer copies.
        //   PIPELINED  RING_DEPTH bands on dedicated COPY queues. GPU 0 signals
        //              a cross-adapter shared fence after each band; GPU 1's
        //              queue WAITS on that fence value on the GPU and consumes
        //              the band. The CPU issues every submission without
        //              blocking and waits exactly once, at the end.
        //
        // WHY BUFFER-TO-BUFFER AND NOT THE TEXTURE ROUND TRIP. The P1.3 path
        // above stages upload -> tex0 -> shared -> tex1 -> readback. Two of
        // those five stages are texture copies that have nothing to do with the
        // link, and a ring cannot overlap them band-by-band without a second
        // pass. Including them would make the A/B compare two different amounts
        // of work and attribute the difference to pipelining. So P2.1 measures
        // upload0 -> shared -> read2: the traversal of the shared heap and
        // nothing else, in both arms. This is a NARROWER measurement than
        // P1.3's, not a faster version of it, and the two numbers are not
        // interchangeable. Say so whenever either is quoted.
        //
        // RESULT ON THE RIG, RECORDED HERE SO THE CODE DOES NOT READ AS A
        // PROMISE IT DID NOT KEEP: eight runs, both paths, both resolutions,
        // all eight byte-exact in both arms. The ring is correct. It is also
        // not faster - at 1440p the pipelined arm averaged 16.25 ms against
        // serial's 11.48 ms over four settled runs, and never won at that
        // size. The 720p readings that looked like 2x were slow serial runs.
        // The block is kept for the correctness result and for the ordering
        // primitive it exercises, not as an optimisation.
        //
        // WHAT THIS DOES NOT MEASURE. Neither arm uses a GPU timestamp. Both
        // are QPC wall-clock around a CPU-visible completion, so both contain
        // queue latency and driver overhead as well as execution. Separating
        // those needs a calibrated cross-adapter clock, which is P2.2 and is
        // the one symbol still behind the containment guard. (The guard greps
        // this source tree, so the API's name is deliberately not written
        // here - naming it in a comment would fail the build as loudly as
        // calling it, which is the guard working, not a bug.)
        {
            const unsigned RING_DEPTH = 4;

            ID3D12CommandQueue *cq0 = nullptr, *cq1 = nullptr;
            ID3D12CommandAllocator *ca0[RING_DEPTH] = {}, *ca1[RING_DEPTH] = {};
            ID3D12GraphicsCommandList *cl0[RING_DEPTH] = {}, *cl1[RING_DEPTH] = {};
            ID3D12Fence *prod0 = nullptr, *prod1 = nullptr, *donef = nullptr;
            HANDLE prod_share = nullptr, done_ev = nullptr;
            ID3D12Resource *read2 = nullptr;

            auto p21_cleanup = [&]()
            {
                for (unsigned i = 0; i < RING_DEPTH; ++i)
                {
                    if (cl1[i] != nullptr) { cl1[i]->Release(); cl1[i] = nullptr; }
                    if (cl0[i] != nullptr) { cl0[i]->Release(); cl0[i] = nullptr; }
                    if (ca1[i] != nullptr) { ca1[i]->Release(); ca1[i] = nullptr; }
                    if (ca0[i] != nullptr) { ca0[i]->Release(); ca0[i] = nullptr; }
                }
                if (done_ev    != nullptr) { CloseHandle(done_ev); done_ev = nullptr; }
                if (donef      != nullptr) { donef->Release(); donef = nullptr; }
                if (prod1      != nullptr) { prod1->Release(); prod1 = nullptr; }
                if (prod_share != nullptr) { CloseHandle(prod_share); prod_share = nullptr; }
                if (prod0      != nullptr) { prod0->Release(); prod0 = nullptr; }
                if (cq1        != nullptr) { cq1->Release(); cq1 = nullptr; }
                if (cq0        != nullptr) { cq0->Release(); cq0 = nullptr; }
                if (read2      != nullptr) { read2->Release(); read2 = nullptr; }
            };

            // ---- the copy queues ----
            // A COPY queue is the DMA engine, not the 3D engine. On GPU 0 that
            // matters for a reason no benchmark shows: the game owns the 3D
            // engine, and every P1 measurement of GPU 0 was taken in a queue
            // behind the game's frame. A copy queue does not stand in that
            // line. Whether that is where wait0's 15x asymmetry against wait1
            // lives is exactly what the two arms below decide.
            D3D12_COMMAND_QUEUE_DESC cqd{};
            cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            HRESULT rh = g0.dev->CreateCommandQueue(&cqd, IID_PPV_ARGS(&cq0));
            if (SUCCEEDED(rh)) rh = g1.dev->CreateCommandQueue(&cqd, IID_PPV_ARGS(&cq1));
            for (unsigned i = 0; i < RING_DEPTH && SUCCEEDED(rh); ++i)
            {
                rh = g0.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                    IID_PPV_ARGS(&ca0[i]));
                if (SUCCEEDED(rh))
                    rh = g0.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, ca0[i],
                                                   nullptr, IID_PPV_ARGS(&cl0[i]));
                if (SUCCEEDED(rh)) rh = cl0[i]->Close();
                if (SUCCEEDED(rh))
                    rh = g1.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                        IID_PPV_ARGS(&ca1[i]));
                if (SUCCEEDED(rh))
                    rh = g1.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, ca1[i],
                                                   nullptr, IID_PPV_ARGS(&cl1[i]));
                if (SUCCEEDED(rh)) rh = cl1[i]->Close();
            }

            // ---- the cross-adapter producer fence ----
            // Created on GPU 0's device, opened on GPU 1's. This is the same
            // mechanism P2.0 proved against the game's device; here both
            // devices are ours, so a failure is ours to fix and not the
            // application's to blame.
            if (SUCCEEDED(rh))
                rh = g0.dev->CreateFence(0, (D3D12_FENCE_FLAGS)(D3D12_FENCE_FLAG_SHARED |
                                                                D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER),
                                         IID_PPV_ARGS(&prod0));
            if (SUCCEEDED(rh))
                rh = g0.dev->CreateSharedHandle(prod0, nullptr, GENERIC_ALL, nullptr, &prod_share);
            if (SUCCEEDED(rh)) rh = g1.dev->OpenSharedHandle(prod_share, IID_PPV_ARGS(&prod1));
            if (SUCCEEDED(rh))
                rh = g1.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&donef));
            if (SUCCEEDED(rh))
            {
                done_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (done_ev == nullptr) rh = E_FAIL;
            }
            if (SUCCEEDED(rh)) rh = make_buf(g1.dev, bytes, D3D12_HEAP_TYPE_READBACK, &read2);

            snprintf(line, sizeof line,
                     "[MGPU][P2.1] %ux%u setup: COPY queues on both adapters, %u-deep ring, "
                     "cross-adapter producer fence (CreateFence(SHARED|SHARED_CROSS_ADAPTER) on "
                     "GPU 0 -> OpenSharedHandle on GPU 1): hr=0x%08X",
                     width, height, RING_DEPTH, (unsigned)rh);
            mgpu::diag::info(line);

            if (FAILED(rh))
            {
                mgpu::diag::warn("[MGPU][P2.1] setup failed - the ring is skipped and the P1.3 "
                                 "serial numbers above stand alone for this resolution. This is "
                                 "not a transit finding: nothing was transported.");
                p21_cleanup();
            }
            else
            {
                // Band geometry. Bands are whole rows, so every offset is a
                // multiple of RowPitch and inherits its 256-byte alignment -
                // there is no sub-row arithmetic anywhere in this block, which
                // is the only reason the footprint cannot be got wrong here the
                // way it could in P1.3.
                const UINT pitch = fp.Footprint.RowPitch;
                const UINT rows_per = (height + RING_DEPTH - 1) / RING_DEPTH;

                // The sentinel again, and for the same reason as P1.5: a band
                // that never arrives has to look different from a band that
                // arrived wrong. read2 is a READBACK buffer - WRITE_BACK, L0,
                // CPU-writable - so the fill is a memset through Map. That is
                // the one CPU write to a readback resource in this codebase and
                // it happens only before the GPU has been asked for anything.
                auto sentinel_fill = [&]() -> bool
                {
                    unsigned char *m = nullptr;
                    D3D12_RANGE none{0, 0};
                    if (FAILED(read2->Map(0, &none, reinterpret_cast<void **>(&m))) ||
                        m == nullptr) return false;
                    for (UINT y = 0; y < height; ++y)
                    {
                        unsigned char *r = m + (size_t)y * pitch;
                        for (UINT x = 0; x < width; ++x)
                        {
                            r[(size_t)x * 4 + 0] = SENT_R;
                            r[(size_t)x * 4 + 1] = SENT_G;
                            r[(size_t)x * 4 + 2] = SENT_B;
                            r[(size_t)x * 4 + 3] = 0xFF;
                        }
                    }
                    D3D12_RANGE allw{0, (SIZE_T)bytes};
                    read2->Unmap(0, &allw);
                    return true;
                };

                // Compare read2 against the pattern. Returns false only on a
                // Map failure; the counts come back through the out params.
                auto verify2 = [&](unsigned long long *differing,
                                   unsigned long long *sentinel) -> bool
                {
                    *differing = 0; *sentinel = 0;
                    const unsigned char *p2 = nullptr;
                    D3D12_RANGE all2{0, (SIZE_T)bytes};
                    if (FAILED(read2->Map(0, &all2, (void **)&p2)) || p2 == nullptr) return false;
                    unsigned char *ref2 = (unsigned char *)malloc((size_t)bytes);
                    if (ref2 != nullptr)
                    {
                        memset(ref2, 0, (size_t)bytes);
                        fill_pattern(ref2, width, height, pitch);
                        for (UINT y = 0; y < height; ++y)
                        {
                            const unsigned char *ra = ref2 + (size_t)y * pitch;
                            const unsigned char *rb = p2   + (size_t)y * pitch;
                            for (UINT x = 0; x < width; ++x)
                            {
                                const unsigned char *a2 = ra + (size_t)x * 4;
                                const unsigned char *b2 = rb + (size_t)x * 4;
                                if (a2[0] != b2[0] || a2[1] != b2[1] || a2[2] != b2[2])
                                    ++*differing;
                                if (b2[0] == SENT_R && b2[1] == SENT_G && b2[2] == SENT_B)
                                    ++*sentinel;
                            }
                        }
                        free(ref2);
                    }
                    D3D12_RANGE nothing2{0, 0};
                    read2->Unmap(0, &nothing2);
                    return true;
                };

                // Fence values never restart. Both arms draw from one rising
                // sequence, because a value the fence has already passed
                // completes instantly and a Wait on it is not a wait at all -
                // the exact trap P2.0 hit with the sentinel signal.
                UINT64 fv = 0;
                LARGE_INTEGER pf{}; QueryPerformanceFrequency(&pf);

                // ---- ARM 1: SERIAL. One band, CPU wait between the sides ----
                double serial_ms = 0.0;
                unsigned long long ser_diff = 0, ser_sent = 0;
                bool ser_ok = sentinel_fill();
                if (ser_ok)
                {
                    LARGE_INTEGER a1{}, b1{};
                    QueryPerformanceCounter(&a1);

                    ca0[0]->Reset(); cl0[0]->Reset(ca0[0], nullptr);
                    cl0[0]->CopyBufferRegion(shared0, 0, upload0, 0, bytes);
                    cl0[0]->Close();
                    { ID3D12CommandList *ls[1] = { cl0[0] }; cq0->ExecuteCommandLists(1, ls); }
                    const UINT64 v_ser0 = ++fv;
                    cq0->Signal(prod0, v_ser0);
                    // THE CPU BLOCKS HERE. This is the line P2.1 exists to
                    // delete, kept in the control arm so the deletion has a
                    // measured value rather than an asserted one.
                    prod0->SetEventOnCompletion(v_ser0, done_ev);
                    if (WaitForSingleObject(done_ev, 20000) != WAIT_OBJECT_0) ser_ok = false;

                    if (ser_ok)
                    {
                        ca1[0]->Reset(); cl1[0]->Reset(ca1[0], nullptr);
                        cl1[0]->CopyBufferRegion(read2, 0, shared1, 0, bytes);
                        cl1[0]->Close();
                        { ID3D12CommandList *ls[1] = { cl1[0] }; cq1->ExecuteCommandLists(1, ls); }
                        const UINT64 v_ser1 = ++fv;
                        cq1->Signal(donef, v_ser1);
                        donef->SetEventOnCompletion(v_ser1, done_ev);
                        if (WaitForSingleObject(done_ev, 20000) != WAIT_OBJECT_0) ser_ok = false;
                    }
                    QueryPerformanceCounter(&b1);
                    serial_ms = qpc_ms(a1, b1, pf);
                    if (ser_ok) ser_ok = verify2(&ser_diff, &ser_sent);
                }

                // ---- ARM 2: PIPELINED. RING_DEPTH bands, GPU-side ordering ----
                double ring_ms = 0.0;
                unsigned long long ring_diff = 0, ring_sent = 0;
                bool ring_ok = sentinel_fill();
                if (ring_ok)
                {
                    // Record every list first, so the submission burst below
                    // contains no CPU work between Execute calls. Recording is
                    // CPU time either way; putting it here keeps it out of the
                    // window we are timing on both arms equally.
                    for (unsigned i = 0; i < RING_DEPTH && ring_ok; ++i)
                    {
                        const UINT r_start = i * rows_per;
                        if (r_start >= height) break;
                        const UINT r_count = (r_start + rows_per > height)
                                               ? (height - r_start) : rows_per;
                        const UINT64 off = (UINT64)r_start * pitch;
                        const UINT64 len = (UINT64)r_count * pitch;

                        if (FAILED(ca0[i]->Reset()) ||
                            FAILED(cl0[i]->Reset(ca0[i], nullptr))) { ring_ok = false; break; }
                        cl0[i]->CopyBufferRegion(shared0, off, upload0, off, len);
                        if (FAILED(cl0[i]->Close())) { ring_ok = false; break; }

                        if (FAILED(ca1[i]->Reset()) ||
                            FAILED(cl1[i]->Reset(ca1[i], nullptr))) { ring_ok = false; break; }
                        cl1[i]->CopyBufferRegion(read2, off, shared1, off, len);
                        if (FAILED(cl1[i]->Close())) { ring_ok = false; break; }
                    }
                }
                if (ring_ok)
                {
                    const UINT64 base = fv;
                    LARGE_INTEGER a2{}, b2{};
                    QueryPerformanceCounter(&a2);

                    // Producer: every band submitted back to back, each
                    // followed by its own fence value. No CPU wait anywhere in
                    // this loop.
                    unsigned bands = 0;
                    for (unsigned i = 0; i < RING_DEPTH; ++i)
                    {
                        if (i * rows_per >= height) break;
                        ID3D12CommandList *ls[1] = { cl0[i] };
                        cq0->ExecuteCommandLists(1, ls);
                        cq0->Signal(prod0, base + i + 1);
                        ++bands;
                    }
                    // Consumer: a GPU-side Wait per band. cq1 does not run
                    // band i until prod reaches i+1, and the CPU is not
                    // involved in that decision. Band 0 can be crossing while
                    // band 1 is still being produced - which is the entire
                    // claim P2.1 makes.
                    for (unsigned i = 0; i < bands; ++i)
                    {
                        cq1->Wait(prod1, base + i + 1);
                        ID3D12CommandList *ls[1] = { cl1[i] };
                        cq1->ExecuteCommandLists(1, ls);
                    }
                    fv = base + bands;
                    const UINT64 vdone = ++fv;
                    cq1->Signal(donef, vdone);
                    donef->SetEventOnCompletion(vdone, done_ev);
                    if (WaitForSingleObject(done_ev, 20000) != WAIT_OBJECT_0) ring_ok = false;

                    QueryPerformanceCounter(&b2);
                    ring_ms = qpc_ms(a2, b2, pf);
                    if (ring_ok) ring_ok = verify2(&ring_diff, &ring_sent);

                    snprintf(line, sizeof line,
                             "[MGPU][P2.1] %ux%u bands=%u rows_per_band=%u pitch=%u",
                             width, height, bands, rows_per, pitch);
                    mgpu::diag::info(line);
                }

                const double mib2 = (double)bytes / (1024.0 * 1024.0);
                // NO DERIVED RATIO IS PRINTED HERE, DELIBERATELY. The first
                // build of this block ended the line with "speedup=%.2fx" and
                // that single field did more damage than every other number in
                // the probe: across eight runs it read 1.10, 0.67, 0.69, 0.94,
                // 0.61 at 1440p and 1.08, 0.50, 1.15, 2.24, 2.00 at 720p, and
                // every one of those figures was the ratio of two noisy
                // wall-clock samples of size one. The two 2x readings at 720p
                // were slow SERIAL runs, not fast rings. A ratio invites a
                // claim; the raw pair does not. Both durations are still
                // logged, because they are data - they are just not a
                // comparison anyone should act on. See the verdict below.
                snprintf(line, sizeof line,
                         "[MGPU][P2.1] %ux%u path=%s buffer-to-buffer over the shared heap, "
                         "%.2f MiB | SERIAL(1 band, CPU wait between sides): ok=%s %.2f ms "
                         "(%.0f MiB/s) differing=%llu sentinel=%llu | PIPELINED(%u bands, COPY "
                         "queues, GPU-side fence wait): ok=%s %.2f ms (%.0f MiB/s) differing=%llu "
                         "sentinel=%llu",
                         width, height, path, mib2,
                         ser_ok ? "yes" : "no", serial_ms,
                         serial_ms > 0.0 ? (mib2 / (serial_ms / 1000.0)) : 0.0,
                         ser_diff, ser_sent,
                         RING_DEPTH,
                         ring_ok ? "yes" : "no", ring_ms,
                         ring_ms > 0.0 ? (mib2 / (ring_ms / 1000.0)) : 0.0,
                         ring_diff, ring_sent);
                mgpu::diag::info(line);

                // The verdict is about CORRECTNESS FIRST and speed second, in
                // that order and never merged. A ring that is faster and wrong
                // is not a result.
                if (!ser_ok || !ring_ok)
                    mgpu::diag::error("[MGPU][P2.1] PROBE INCOMPLETE - one arm did not run to "
                                      "completion (see ok= above). No comparison is available; "
                                      "do not read the timings.");
                else if (ring_diff != 0 || ser_diff != 0)
                {
                    all_ok = false;
                    snprintf(line, sizeof line,
                             "[MGPU][P2.1] PROBE FAILED - payload wrong (serial differing=%llu, "
                             "pipelined differing=%llu). If ONLY the pipelined arm differs, the "
                             "GPU-side ordering is the suspect and the band boundaries are where "
                             "to look: a band consumed before its producer signal would show as "
                             "a contiguous wrong region, not scattered pixels. If BOTH differ, "
                             "the fault is in the buffer copies and predates the ring.",
                             ser_diff, ring_diff);
                    mgpu::diag::error(line);
                }
                else
                {
                    // THIS IS A CORRECTNESS RESULT AND NOTHING ELSE.
                    //
                    // The previous wording of this line claimed "GPU 1 consumed
                    // band 0 while GPU 0 was still producing band 1". Nothing
                    // in this probe measures that. It is the mechanism the code
                    // was written to produce, asserted in a PASSED line as
                    // though it had been observed - the same mistake as the
                    // P1.3b failure message and the EnableDebugLayer comment,
                    // and it is removed for the same reason.
                    //
                    // What IS established: a %u-band ring, with dedicated COPY
                    // queues on both adapters and GPU-side fence ordering
                    // between them, moves the payload byte-exact. Every band
                    // boundary held; no band was consumed before its producer
                    // signal, because that would have left a contiguous wrong
                    // region and differing is 0.
                    //
                    // WHAT IS NOT ESTABLISHED, AND WHY WE STOPPED ASKING: that
                    // this discipline is FASTER. On the rig it was not - the
                    // pipelined arm ran ~1.4x SLOWER than serial at 1440p, in
                    // four settled runs out of four, while the serial arm held
                    // to +/-2%. That is consistent with both halves crossing
                    // the SAME link: producer and consumer contend for one
                    // path whatever its width, the total bytes over it are
                    // unchanged,
                    // so overlap cannot add bandwidth and the extra
                    // submissions and cross-adapter waits are pure cost.
                    // Pipelining pays when the overlapped stages use DIFFERENT
                    // resources - transfer against neural execution on GPU 1,
                    // which P1.4 already showed is possible - and that is not
                    // what this block overlaps. The ring is kept for its
                    // correctness and its ordering primitive; its timings are
                    // logged but are not a case for it.
                    snprintf(line, sizeof line,
                             "[MGPU][P2.1] PROBE PASSED (CORRECTNESS ONLY) - both arms delivered "
                             "the payload byte-exact with no sentinel survivors, so the %u-band "
                             "ring transports correctly and the GPU-side fence ordering between "
                             "the two adapters holds at every band boundary. NO SPEED CLAIM IS "
                             "MADE OR IMPLIED. The two durations above are single noisy "
                             "wall-clock samples of a NARROWER path than P1.3's (shared-heap "
                             "traversal only, no texture stages), neither is a GPU timestamp, "
                             "and on this rig the pipelined arm has been the SLOWER of the two - "
                             "both halves cross the same link, so overlapping them adds "
                             "contention, not bandwidth. The overlap that could pay is transfer "
                             "against neural execution, which this block does not test.",
                             RING_DEPTH);
                    mgpu::diag::info(line);
                }

                p21_cleanup();
            }
        }
        cleanup();
    }

    mgpu::diag::info("[MGPU][P1.3] NOTE: the timings above are a CPU-serialised round trip "
                     "measured during game startup, with no shared fence and no pipelining. "
                     "Treat the RATIO between the two sizes as the signal and the absolute "
                     "numbers as an upper bound; a real figure needs cross-adapter GPU clock "
                     "calibration and a settled scene (P2).");

    // P1.3b: the probe now creates BOTH devices itself (one per adapter), so
    // both are ours to destroy. The NGX session on create_device's GPU 1
    // device is untouched by this - we never borrowed it.
    transit_side_release(g1, true);
    transit_side_release(g0, true);
    mgpu::diag::info("[MGPU][P1.3] transit probe torn down - both probe-owned devices released");
    return all_ok;
}

// =====================================================================
// P1.5 - THE HOST'S REAL FRAME
// =====================================================================
//
// P1.4 closed the loop on a pattern we generated: deterministic, well formed,
// ours. That is what made a byte-exact verdict possible, and it is also the
// last thing about the payload that was convenient. P1.5 replaces it with the
// game's finished colour buffer - 2560x1440 R10G10B10A2_UNORM here - a
// resource this add-on does not own and did not create.
//
// THE VERDICT CHANGES SHAPE, AND THAT IS THE INTERESTING PART. Real content is
// not deterministic, so P1.4's "identical to the local control" test cannot
// survive the move. Rather than weaken it to something softer ("it looks like
// a frame"), the question is split in two:
//
//   1. TRANSIT INTEGRITY, still exact. The same command list records TWO
//      copies of the same source: one into the cross-adapter buffer, one into
//      an ordinary readback buffer on the game's own device. They are the same
//      bytes by construction. What GPU 1 receives is compared against what the
//      readback holds. Byte-identical, or not.
//   2. NR CORRECTNESS on that payload - already established by P1.4 and NOT
//      re-argued here. The model is deterministic within a session and
//      produces identical output on transited input. Nothing about real
//      content changes that, so P1.5 does not re-prove it.
//
// The heap is created on the GAME'S DEVICE, not on one of ours. The game's
// command list can only reference resources from the device that created it,
// and the ReShade event hands us that list. This is the most intrusive thing
// this project does, so it is one-shot: one frame, then disarmed forever.
namespace
{
    struct capture_state
    {
        std::mutex cs;
        bool requested = false;     // the operator asked for a capture
        // P1.5b. One-shot diagnostic latches. The 21:23 run requested a capture
        // and nothing followed: no arm line, no failure line, nothing. Every
        // early return in the handler was silent, so the log could not say
        // whether the event never fired, fired on the wrong adapter, or fired
        // with a handle we could not use. An instrument that cannot report its
        // own failure is the failure mode this project keeps rediscovering.
        bool said_seen = false;     // the event reached us at least once
        bool said_bad = false;      // null list or resource
        bool said_other = false;    // fired, but not on the game's adapter
        bool said_wrongq = false;   // armed, but the list belonged elsewhere
        unsigned ev_game = 0;       // post-arm events whose list is on the game adapter
        unsigned ev_other = 0;      // post-arm events from anywhere else

        // P2.0: the shared fence. Created on the GAME's device, signalled on the
        // GAME's queue after the copies have been submitted, waited on from the
        // NGX device. This replaces counting bridge presents and hoping - the
        // one deliberately weak thing left in P1.5.
        ID3D12Fence *gfence = nullptr;      // game side
        HANDLE gfence_share = nullptr;
        ID3D12Fence *nfence = nullptr;      // the same fence, opened on GPU 1
        bool fence_ok = false;              // the cross-adapter pair exists
        bool signalled = false;             // Signal has been issued on the game queue
        const UINT64 SIG = 1;
        bool tried = false;         // allocation attempted (success or not)
        bool armed = false;         // resources exist, waiting to record
        bool recorded = false;      // the copies are in a submitted list
        bool done = false;          // verdict printed; never act again
        unsigned polls = 0;         // present-loop polls since recording

        ID3D12Device *gdev = nullptr;      // the GAME's device - borrowed, not owned
        ID3D12Heap *gheap = nullptr;       // cross-adapter heap, on the game's device
        ID3D12Resource *gxfer = nullptr;   // placed buffer in it, game side
        ID3D12Resource *gread = nullptr;   // plain readback, game side - the reference
        HANDLE gshare = nullptr;

        ID3D12Heap *nheap = nullptr;       // the same heap, opened on the NGX device
        ID3D12Resource *nxfer = nullptr;
        ID3D12Resource *nread = nullptr;   // readback on the NGX device

        ID3D12CommandQueue *nq = nullptr;  // our own queue on the NGX device
        ID3D12CommandAllocator *na = nullptr;
        ID3D12GraphicsCommandList *nl = nullptr;
        ID3D12Fence *nf = nullptr;
        HANDLE nev = nullptr;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes = 0;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    capture_state &cap()
    {
        static capture_state s;
        return s;
    }

    // Sentinel for the cross-adapter buffer. Reading before the game's queue
    // has retired the copy shows up as surviving sentinel bytes rather than as
    // a plausible frame, which is the whole reason it is here.
    const unsigned char CAP_SENT = 0xA5;

    void capture_release()
    {
        capture_state &c = cap();
        if (c.nev   != nullptr) { CloseHandle(c.nev); c.nev = nullptr; }
        if (c.nf    != nullptr) { c.nf->Release();    c.nf = nullptr; }
        if (c.nl    != nullptr) { c.nl->Release();    c.nl = nullptr; }
        if (c.na    != nullptr) { c.na->Release();    c.na = nullptr; }
        if (c.nq    != nullptr) { c.nq->Release();    c.nq = nullptr; }
        if (c.nread != nullptr) { c.nread->Release(); c.nread = nullptr; }
        if (c.nxfer != nullptr) { c.nxfer->Release(); c.nxfer = nullptr; }
        if (c.nheap != nullptr) { c.nheap->Release(); c.nheap = nullptr; }
        if (c.nfence != nullptr) { c.nfence->Release(); c.nfence = nullptr; }
        if (c.gfence_share != nullptr) { CloseHandle(c.gfence_share); c.gfence_share = nullptr; }
        if (c.gfence != nullptr) { c.gfence->Release(); c.gfence = nullptr; }
        if (c.gshare!= nullptr) { CloseHandle(c.gshare); c.gshare = nullptr; }
        if (c.gread != nullptr) { c.gread->Release(); c.gread = nullptr; }
        if (c.gxfer != nullptr) { c.gxfer->Release(); c.gxfer = nullptr; }
        if (c.gheap != nullptr) { c.gheap->Release(); c.gheap = nullptr; }
        c.gdev = nullptr;   // borrowed; never released here
        c.armed = false;
    }
}

void capture_request()
{
    capture_state &c = cap();
    std::lock_guard<std::mutex> lk(c.cs);
    if (c.done) { mgpu::diag::info("[MGPU][P1.5] capture already spent this launch - one shot "
                                   "per process, by design"); return; }
    if (c.requested)
    {
        // The old wording here said "already armed", which was wrong and
        // actively misleading: it fires on the REQUEST flag, not on resources
        // existing, so a run where arming never happened still reported armed.
        char l2[400];
        snprintf(l2, sizeof l2,
                 "[MGPU][P1.5] already requested. event_seen=%s armed=%s recorded=%s | "
                 "post-arm events: game_adapter=%u other=%u. If armed=yes and game_adapter=0, "
                 "the game's runtime raised the event once and then stopped - which is a fact "
                 "about ReShade worth recording, not a fault here.",
                 c.said_seen ? "yes" : "NO", c.armed ? "yes" : "no",
                 c.recorded ? "yes" : "no", c.ev_game, c.ev_other);
        mgpu::diag::info(l2);
        return;
    }
    c.requested = true;
    mgpu::diag::info("[MGPU][P1.5] capture REQUESTED - the next game frame allocates and arms, "
                     "the one after it is captured. Stay in gameplay; a black source frame "
                     "makes the verdict inconclusive and the shot is not repeatable.");
}

void capture_on_finish_effects(void *runtime_v, void *cmd_list_v, void *cmd_queue_v,
                               unsigned long long rtv_handle)
{
    (void)runtime_v;
    capture_state &c = cap();
    std::lock_guard<std::mutex> lk(c.cs);
    // Inert until requested. The operator picks the frame, because the probe
    // cannot tell a loading screen from gameplay and only gets one.
    // P2.0. SIGNAL ON THE FRAME AFTER RECORDING, NOT THE SAME ONE. Queue
    // operations happen in submission order, and ReShade executes the list we
    // recorded into AFTER this event returns. Signalling here would place the
    // signal ahead of our own copies and the wait would clear before the data
    // existed - a race that produces a plausible frame most of the time and a
    // torn one occasionally, which is the worst possible failure shape.
    //
    // By the next event on this adapter, the previous frame's list has been
    // submitted (it had to be, to present), so a Signal now lands behind it.
    if (c.recorded && !c.signalled && c.fence_ok && !c.done)
    {
        ID3D12CommandQueue *gq = reinterpret_cast<ID3D12CommandQueue *>(cmd_queue_v);
        if (gq != nullptr)
        {
            ID3D12Device *qd = nullptr;
            LUID ql{};
            const bool got = SUCCEEDED(gq->GetDevice(IID_PPV_ARGS(&qd))) && qd != nullptr;
            if (got) { ql = qd->GetAdapterLuid(); qd->Release(); }
            LUID want{};
            {
                std::lock_guard<std::mutex> g(st().cs);
                want = st().game_luid;
            }
            if (got && ql.LowPart == want.LowPart && ql.HighPart == want.HighPart)
            {
                const HRESULT sh2 = gq->Signal(c.gfence, c.SIG);
                c.signalled = SUCCEEDED(sh2);
                char sl[400];
                snprintf(sl, sizeof sl,
                         "[MGPU][P2.0] Signal(%llu) issued on the GAME's queue, one frame after "
                         "the copies were recorded so queue order puts it behind them: hr=0x%08X",
                         (unsigned long long)c.SIG, (unsigned)sh2);
                mgpu::diag::info(sl);
            }
        }
        return;   // nothing else to do on this event
    }

    if (!c.requested || c.done || c.recorded) return;

    char line[900];

    // The event reached us. Said once, because it fires every frame.
    if (!c.said_seen)
    {
        c.said_seen = true;
        mgpu::diag::info("[MGPU][P1.5] finish_effects event RECEIVED after the request - the "
                         "hook is live. If nothing follows this line, the reason is below and "
                         "not silence.");
    }

    ID3D12GraphicsCommandList *gl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list_v);
    ID3D12Resource *src = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)rtv_handle);
    if (gl == nullptr || src == nullptr)
    {
        if (!c.said_bad)
        {
            c.said_bad = true;
            snprintf(line, sizeof line,
                     "[MGPU][P1.5] event carries an unusable pair: cmd_list=0x%p resource=0x%p. "
                     "One of them is null, so there is nothing to record into or copy from.",
                     (void *)gl, (void *)src);
            mgpu::diag::warn(line);
        }
        return;
    }

    // ---- first call: allocate, arm, and record nothing ----
    //
    // Allocation and recording are deliberately different frames. Creating a
    // heap, sharing it, opening it on another device and building command
    // objects is not work to do inside a hook on the game's render thread with
    // its command list open.
    if (!c.armed)
    {
        if (c.tried)
        {
            // Arming was attempted once and did not complete. Saying so beats
            // the previous behaviour, which was to fall silent forever.
            return;
        }
        c.tried = true;

        ID3D12Device *gdev = nullptr;
        if (FAILED(src->GetDevice(IID_PPV_ARGS(&gdev))) || gdev == nullptr)
        {
            mgpu::diag::warn("[MGPU][P1.5] the event's resource has no device - cannot identify "
                             "which adapter raised it, so nothing is armed");
            c.done = true;
            return;
        }

        // FILTER. The bridge's own effect runtime raises this event too, and
        // acting on it would capture our own 1280x720 window and call it the
        // game's frame - a false positive that would look entirely correct.
        const LUID l = gdev->GetAdapterLuid();
        bool is_game = false;
        {
            std::lock_guard<std::mutex> g(st().cs);
            is_game = st().game_luid_known &&
                      l.LowPart == st().game_luid.LowPart &&
                      l.HighPart == st().game_luid.HighPart;
        }
        if (!is_game)
        {
            if (!c.said_other)
            {
                c.said_other = true;
                LUID want{}; bool known = false;
                {
                    std::lock_guard<std::mutex> g(st().cs);
                    want = st().game_luid; known = st().game_luid_known;
                }
                snprintf(line, sizeof line,
                         "[MGPU][P1.5] event fired on adapter %08lX-%08lX, which is NOT the "
                         "game's (%08lX-%08lX, known=%s) - this is the bridge's own effect "
                         "runtime and is correctly ignored. If ONLY this line appears, the "
                         "game's runtime is not raising the event: it has no effects to run. "
                         "Enable one effect on the game's runtime and request again.",
                         (unsigned long)l.HighPart, (unsigned long)l.LowPart,
                         (unsigned long)want.HighPart, (unsigned long)want.LowPart,
                         known ? "yes" : "NO");
                mgpu::diag::warn(line);
            }
            gdev->Release();
            c.tried = false;   // this was not our adapter; stay armable
            return;
        }

        const D3D12_RESOURCE_DESC rd = src->GetDesc();
        c.gdev = gdev;   // borrowed reference kept for the lifetime of the probe
        c.width = (UINT)rd.Width; c.height = rd.Height; c.format = rd.Format;

        UINT rows = 0; UINT64 rowb = 0;
        gdev->GetCopyableFootprints(&rd, 0, 1, 0, &c.fp, &rows, &rowb, &c.bytes);

        const UINT64 ALIGN = 65536;
        const UINT64 heap_bytes = ((c.bytes + ALIGN - 1) / ALIGN) * ALIGN;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = heap_bytes; hd.Properties = hp; hd.Alignment = ALIGN;
        hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED |
                                      D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = heap_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        HRESULT h = gdev->CreateHeap(&hd, IID_PPV_ARGS(&c.gheap));
        if (SUCCEEDED(h))
            h = gdev->CreatePlacedResource(c.gheap, 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                                           nullptr, IID_PPV_ARGS(&c.gxfer));
        if (SUCCEEDED(h))
            h = gdev->CreateSharedHandle(c.gheap, nullptr, GENERIC_ALL, nullptr, &c.gshare);
        if (SUCCEEDED(h)) h = make_buf(gdev, c.bytes, D3D12_HEAP_TYPE_READBACK, &c.gread);

        ID3D12Device *ndev = nullptr;
        {
            std::lock_guard<std::mutex> g(st().cs);
            ndev = st().device;
        }
        if (SUCCEEDED(h) && ndev != nullptr)
        {
            h = ndev->OpenSharedHandle(c.gshare, IID_PPV_ARGS(&c.nheap));
            if (SUCCEEDED(h))
                h = ndev->CreatePlacedResource(c.nheap, 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&c.nxfer));
            if (SUCCEEDED(h)) h = make_buf(ndev, c.bytes, D3D12_HEAP_TYPE_READBACK, &c.nread);
            if (SUCCEEDED(h))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                h = ndev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.nq));
            }
            if (SUCCEEDED(h))
                h = ndev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&c.na));
            if (SUCCEEDED(h))
                h = ndev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.na, nullptr,
                                            IID_PPV_ARGS(&c.nl));
            if (SUCCEEDED(h)) { h = c.nl->Close(); }
            if (SUCCEEDED(h))
                h = ndev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.nf));
            if (SUCCEEDED(h))
            {
                c.nev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (c.nev == nullptr) h = E_FAIL;
            }
        }
        else if (ndev == nullptr) h = E_FAIL;

        snprintf(line, sizeof line,
                 "[MGPU][P1.5] arm hr=0x%08X source=%ux%u fmt=%d rowPitch=%u bytes=%llu "
                 "(the game's finished colour buffer, on the game's own device). Heap is "
                 "created on the GAME's device because its command list can only reference "
                 "resources from the device that made it.",
                 (unsigned)h, c.width, c.height, (int)c.format,
                 (unsigned)c.fp.Footprint.RowPitch, (unsigned long long)c.bytes);
        mgpu::diag::info(line);

        // ---- P2.0: the cross-adapter shared fence ----
        //
        // A fence created SHARED | SHARED_CROSS_ADAPTER on the game's device and
        // opened on the NGX device is the only way to know the game's queue has
        // retired our copies. We cannot ask that queue anything - but we can be
        // told by it. Capability is logged rather than assumed: cross-adapter
        // fences are a separate support question from cross-adapter heaps, and
        // this rig has answered only the second.
        if (SUCCEEDED(h) && ndev != nullptr)
        {
            HRESULT fh = gdev->CreateFence(0, (D3D12_FENCE_FLAGS)(D3D12_FENCE_FLAG_SHARED |
                                                                  D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER),
                                           IID_PPV_ARGS(&c.gfence));
            if (SUCCEEDED(fh))
                fh = gdev->CreateSharedHandle(c.gfence, nullptr, GENERIC_ALL, nullptr,
                                              &c.gfence_share);
            if (SUCCEEDED(fh))
                fh = ndev->OpenSharedHandle(c.gfence_share, IID_PPV_ARGS(&c.nfence));
            c.fence_ok = SUCCEEDED(fh) && c.nfence != nullptr;
            snprintf(line, sizeof line,
                     "[MGPU][P2.0] cross-adapter shared fence: CreateFence(SHARED|"
                     "SHARED_CROSS_ADAPTER) on the game's device -> CreateSharedHandle -> "
                     "OpenSharedHandle on the NGX device: hr=0x%08X. %s",
                     (unsigned)fh,
                     c.fence_ok
                       ? "The read below waits on this instead of counting frames - the guess is gone."
                       : "UNAVAILABLE on this rig; falling back to the frame-count wait, which is "
                         "a guess and is labelled as one in the verdict.");
            mgpu::diag::info(line);
            // A missing fence is not fatal: the frame-count path still works and
            // the sentinel still catches a premature read.
        }

        // ---- WRITE THE SENTINEL, or the control cannot fire ----
        //
        // A DEFAULT heap comes back zeroed, not poisoned, so a "sentinel
        // survivors" count against a buffer nobody filled would be zero on
        // every run including the broken ones - a control that always passes,
        // which is worse than no control at all. Fill the cross-adapter buffer
        // with a value that cannot be mistaken for content, from GPU 1's side,
        // before the game's copy is ever recorded.
        if (SUCCEEDED(h))
        {
            ID3D12Resource *poison = nullptr;
            h = make_buf(ndev, c.bytes, D3D12_HEAP_TYPE_UPLOAD, &poison);
            if (SUCCEEDED(h))
            {
                unsigned char *m = nullptr;
                D3D12_RANGE none{0, 0};
                h = poison->Map(0, &none, (void **)&m);
                if (SUCCEEDED(h) && m != nullptr)
                {
                    memset(m, CAP_SENT, (size_t)c.bytes);
                    poison->Unmap(0, nullptr);
                }
                else h = E_FAIL;
            }
            if (SUCCEEDED(h)) h = c.na->Reset();
            if (SUCCEEDED(h)) h = c.nl->Reset(c.na, nullptr);
            if (SUCCEEDED(h))
            {
                c.nl->CopyBufferRegion(c.nxfer, 0, poison, 0, c.bytes);
                h = c.nl->Close();
            }
            if (SUCCEEDED(h))
            {
                ID3D12CommandList *const ls[1] = { c.nl };
                c.nq->ExecuteCommandLists(1, ls);
                h = c.nq->Signal(c.nf, 99);
                if (SUCCEEDED(h))
                {
                    c.nf->SetEventOnCompletion(99, c.nev);
                    if (WaitForSingleObject(c.nev, 20000) != WAIT_OBJECT_0) h = E_FAIL;
                }
            }
            if (poison != nullptr) poison->Release();
            snprintf(line, sizeof line,
                     "[MGPU][P1.5] sentinel 0x%02X written across %llu bytes of the "
                     "cross-adapter buffer from GPU 1: hr=0x%08X. Reading before the game's "
                     "queue retires its copy now shows as surviving sentinel rather than as a "
                     "plausible frame.",
                     (unsigned)CAP_SENT, (unsigned long long)c.bytes, (unsigned)h);
            mgpu::diag::info(line);
        }

        if (FAILED(h)) { capture_release(); c.done = true; return; }
        c.armed = true;
        return;   // record on a later frame, not this one
    }

    // ---- second call: record two copies of the same source ----
    //
    // P1.5c. THE ARM PATH FILTERED BY ADAPTER AND THIS PATH DID NOT, which is
    // the defect that produced the 21:5x run's all-sentinel result. Both effect
    // runtimes raise this event every frame. Once armed, the next event to
    // arrive recorded the copies - and if that was the BRIDGE's runtime, we
    // recorded a copy of a game-device resource into a command list belonging
    // to the GPU 1 device. A command list cannot reference resources from
    // another device: the work is invalid, nothing executes, and both
    // destinations stay exactly as they were.
    //
    // That is precisely what the log showed: the cross-adapter buffer still
    // held its sentinel AND the game-side readback was still zero. The readback
    // needs no bus and no crossing, so both being untouched could never have
    // been a synchronisation race - and the probe said "read too early"
    // anyway, because that was the only failure mode I had given it words for.
    // A diagnosis is only as good as the alternatives the instrument can name.
    {
        // P1.5d. COMPARE BY ADAPTER LUID, NOT BY DEVICE POINTER. The previous
        // build compared ID3D12Device pointers, and pointer identity is the
        // wrong criterion here: ReShade wraps D3D12 objects, so the device
        // behind a command list it hands us need not be the same COM object as
        // the device behind a resource, even when both sit on the same
        // adapter. The arm path already filtered by LUID and worked first try;
        // the record path invented a second, stricter criterion for the same
        // question and rejected everything. Use one criterion.
        ID3D12Device *ld = nullptr;
        const bool got = SUCCEEDED(gl->GetDevice(IID_PPV_ARGS(&ld))) && ld != nullptr;
        LUID ll{};
        if (got) ll = ld->GetAdapterLuid();
        if (got) ld->Release();

        LUID want{};
        {
            std::lock_guard<std::mutex> g(st().cs);
            want = st().game_luid;
        }
        const bool same = got && ll.LowPart == want.LowPart && ll.HighPart == want.HighPart;

        if (same) ++c.ev_game; else ++c.ev_other;

        if (!same)
        {
            // Silent after the first: both runtimes raise this every frame.
            if (!c.said_wrongq)
            {
                c.said_wrongq = true;
                snprintf(line, sizeof line,
                         "[MGPU][P1.5] armed, but this event's command list is on adapter "
                         "%08lX-%08lX, not the game's %08lX-%08lX (got_device=%s). Recording "
                         "here would build a list referencing another device's resources, which "
                         "executes nothing and looks exactly like a transit failure.",
                         (unsigned long)ll.HighPart, (unsigned long)ll.LowPart,
                         (unsigned long)want.HighPart, (unsigned long)want.LowPart,
                         got ? "yes" : "NO");
                mgpu::diag::warn(line);
            }
            return;
        }
    }

    // Same list, same source, same instant. That is what makes the readback a
    // valid reference for what crossed: not "a frame", THE frame, byte for
    // byte, with no opportunity for the two to diverge.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION s{}, d1{}, d2{};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    d1.pResource = c.gxfer; d1.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d1.PlacedFootprint = c.fp; d1.PlacedFootprint.Offset = 0;
    d2.pResource = c.gread; d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d2.PlacedFootprint = c.fp; d2.PlacedFootprint.Offset = 0;
    gl->CopyTextureRegion(&d1, 0, 0, 0, &s, nullptr);
    gl->CopyTextureRegion(&d2, 0, 0, 0, &s, nullptr);

    // Restore EXACTLY. The game did not ask us to change its resource state and
    // must not be able to tell that we did.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gl->ResourceBarrier(1, &b);

    c.recorded = true;
    c.polls = 0;
    mgpu::diag::info("[MGPU][P1.5] capture recorded into the game's command list: one copy to "
                     "the cross-adapter buffer, one to a local readback. Same list, same "
                     "source, same instant - the readback is the reference for what crossed. "
                     "The add-on will not touch the game's list again.");
}

void capture_poll()
{
    capture_state &c = cap();
    std::lock_guard<std::mutex> lk(c.cs);
    if (c.done || !c.recorded) return;

    // ---- P2.0: wait on the shared fence, not on a frame count ----
    //
    // P1.5 counted bridge presents because we could not signal on a queue we
    // do not own. We can: ReShade hands us the game's immediate queue, and a
    // fence created SHARED | SHARED_CROSS_ADAPTER on the game's device and
    // opened on ours is visible to both. The signal was issued on the frame
    // AFTER the copies were recorded, so queue order puts it behind them; when
    // it lands, the crossing is complete by definition rather than by guess.
    //
    // POLLED, NOT BLOCKED. GetCompletedValue is a read, and capture_poll runs
    // under the same mutex the game-thread event handler takes. Blocking here
    // on SetEventOnCompletion would hold that lock across a wait on work owned
    // by another process's queue - the one place in this add-on where a stall
    // could reach into the game's render thread. Once per present is frequent
    // enough; the fence is either past SIG or it is not.
    //
    // WAIT_POLLS survives as the bound and as the fallback. When the shared
    // fence could not be created (fence_ok false) or could not be signalled
    // (signalled false), this is exactly P1.5's frames-elapsed guess and the
    // verdict below says which mode produced it. When the fence exists but
    // never reaches SIG within the bound, that is itself the finding - the
    // handoff did not complete - and it is reported as such rather than read
    // early and blamed on transit.
    const unsigned WAIT_POLLS = 240;
    ++c.polls;

    const bool fence_mode = c.fence_ok && c.signalled && c.nfence != nullptr;
    bool fence_landed = false;

    if (fence_mode)
    {
        fence_landed = (c.nfence->GetCompletedValue() >= c.SIG);
        if (!fence_landed && c.polls < WAIT_POLLS) return;
    }
    else
    {
        if (c.polls < WAIT_POLLS) return;
    }

    c.done = true;

    {
        char mline[400];
        if (fence_mode && fence_landed)
            snprintf(mline, sizeof mline,
                     "[MGPU][P2.0] handoff CONFIRMED by shared fence: the game's queue passed "
                     "value %llu after %u bridge presents. The read below is ordered behind the "
                     "copies, not merely later than them.",
                     (unsigned long long)c.SIG, c.polls);
        else if (fence_mode)
            snprintf(mline, sizeof mline,
                     "[MGPU][P2.0] shared fence NEVER REACHED %llu in %u bridge presents "
                     "(completed=%llu). Reading anyway so the buffers can be described, but any "
                     "sentinel survivors below are the unfinished handoff, not transit.",
                     (unsigned long long)c.SIG, c.polls,
                     (unsigned long long)c.nfence->GetCompletedValue());
        else
            snprintf(mline, sizeof mline,
                     "[MGPU][P2.0] FALLBACK MODE - no shared fence (created=%s signalled=%s). "
                     "Completion is inferred from %u elapsed bridge presents, exactly as P1.5 "
                     "did. Treat the verdict as timing-dependent.",
                     c.fence_ok ? "yes" : "no", c.signalled ? "yes" : "no", c.polls);
        mgpu::diag::info(mline);
    }

    char line[1000];
    HRESULT h = c.na->Reset();
    if (SUCCEEDED(h)) h = c.nl->Reset(c.na, nullptr);
    if (SUCCEEDED(h))
    {
        c.nl->CopyBufferRegion(c.nread, 0, c.nxfer, 0, c.bytes);
        h = c.nl->Close();
    }
    if (SUCCEEDED(h))
    {
        ID3D12CommandList *const ls[1] = { c.nl };
        c.nq->ExecuteCommandLists(1, ls);
        // 100, not 1: the sentinel fill already signalled 99 on this fence, and
        // a fence value that has already been passed completes instantly - the
        // wait would return before the copy had run.
        h = c.nq->Signal(c.nf, 100);
        if (SUCCEEDED(h))
        {
            c.nf->SetEventOnCompletion(100, c.nev);
            if (WaitForSingleObject(c.nev, 20000) != WAIT_OBJECT_0) h = E_FAIL;
        }
    }
    if (FAILED(h))
    {
        snprintf(line, sizeof line, "[MGPU][P1.5] GPU 1 read failed hr=0x%08X - no verdict",
                 (unsigned)h);
        mgpu::diag::error(line);
        capture_release();
        return;
    }

    // P3.0: the converted copy of the arrived frame, built while the readback
    // is still mapped and consumed after it is not. Declared out here because
    // ngx_probe must NOT be called with a D3D12 resource mapped - it creates
    // its own resources, submits its own work and blocks for over a second on
    // CreateFeature, all on this thread.
    unsigned char *nr_in = nullptr;
    UINT nr_pitch = 0;
    unsigned char *nr_raw = nullptr;
    UINT nr_raw_pitch = 0;

    const unsigned char *pn = nullptr, *pg = nullptr;
    D3D12_RANGE all{0, (SIZE_T)c.bytes};
    const bool mn = SUCCEEDED(c.nread->Map(0, &all, (void **)&pn)) && pn != nullptr;
    const bool mg = SUCCEEDED(c.gread->Map(0, &all, (void **)&pg)) && pg != nullptr;
    if (mn && mg)
    {
        unsigned long long diff = 0, sent = 0, nonzero = 0, sent_ref = 0;
        const unsigned long long total = (unsigned long long)c.width * c.height;
        for (UINT y = 0; y < c.height; ++y)
        {
            const size_t ro = (size_t)y * c.fp.Footprint.RowPitch;
            for (UINT x = 0; x < c.width; ++x)
            {
                const unsigned char *a = pn + ro + (size_t)x * 4;
                const unsigned char *b2 = pg + ro + (size_t)x * 4;
                if (a[0] != b2[0] || a[1] != b2[1] || a[2] != b2[2] || a[3] != b2[3]) ++diff;
                if (a[0] == CAP_SENT && a[1] == CAP_SENT && a[2] == CAP_SENT) ++sent;
                // P1.5e: count the sentinel colour in the REFERENCE too. A real
                // frame contains mid-greys and 0xA5A5A5 is one, so counting it
                // only on the received side turns ordinary content into a
                // "survivor" - which reported failure at 21:50:44 on a run
                // where differing was 0.
                if (b2[0] == CAP_SENT && b2[1] == CAP_SENT && b2[2] == CAP_SENT) ++sent_ref;
                if (b2[0] || b2[1] || b2[2]) ++nonzero;
            }
        }
        // A survivor is only a survivor if the REFERENCE does not also hold it.
        // Subtracting is what turns an absolute test into a comparison against
        // the control, which is the rule everywhere else in this project.
        const unsigned long long survivors = (sent > sent_ref) ? (sent - sent_ref) : 0;
        snprintf(line, sizeof line,
                 "[MGPU][P1.5] %ux%u fmt=%d %.2f MiB | received vs sent: differing=%llu of %llu "
                 "| sentinel-coloured: received=%llu reference=%llu -> true survivors=%llu "
                 "| source non-black pixels=%llu",
                 c.width, c.height, (int)c.format,
                 (double)c.bytes / (1024.0 * 1024.0), diff, total, sent, sent_ref, survivors,
                 nonzero);
        mgpu::diag::info(line);

        // ORDER MATTERS, AND IT WAS WRONG. `differing == 0` means every byte
        // received equals the reference taken from the same command list at the
        // same instant. A buffer that was never written would differ from that
        // reference in essentially every pixel, so a byte-exact match cannot be
        // stale sentinel data whatever colours it happens to contain. The
        // comparison against the control outranks every absolute test - putting
        // an absolute first is what made a passing run report failure over one
        // grey pixel out of 3.69 million.
        if (diff == 0 && nonzero > 0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.5] PROBE PASSED - THE GAME'S OWN FRAME CROSSED THE ADAPTER "
                     "BOUNDARY INTACT. All %llu pixels received on the second adapter match the "
                     "reference taken from the same command list at the same instant, and the "
                     "source carried %llu non-black pixels of real rendered content at the "
                     "game's native %ux%u fmt=%d. This is the application's finished colour "
                     "buffer, not a pattern of ours. (%llu pixels were the sentinel colour in "
                     "BOTH buffers - content, not survival.)",
                     total, nonzero, c.width, c.height, (int)c.format, sent_ref);
            mgpu::diag::info(line);

            // ---- P3.0: hand the arrived frame to the neural stage ----
            //
            // THE CONVERSION IS A KNOWN COST AND IT IS NOT A PRODUCTION PATH.
            // The game renders R10G10B10A2 and ngx_probe builds its colour
            // texture as R8G8B8A8, so this drops two bits per channel on the
            // CPU, at 3.7 million pixels, once. That is acceptable for a
            // one-shot probe and unacceptable per frame. Whether DLSS-NR will
            // accept R10G10B10A2 directly - which would delete this stage
            // entirely - is NOT answered here and is the first thing to test
            // once the pipeline runs at all. It is called out rather than
            // buried because a silent conversion is exactly the kind of cost
            // that ends up in a performance number later with no name on it.
            // P3.1: the frame's ORIGINAL bytes, kept alongside the converted
            // copy. Both attempts run from CPU memory, so the capture's GPU
            // resources can still be released before either one starts.
            nr_raw_pitch = c.fp.Footprint.RowPitch;
            nr_raw = (unsigned char *)malloc((size_t)nr_raw_pitch * c.height);
            if (nr_raw != nullptr)
                memcpy(nr_raw, pn, (size_t)nr_raw_pitch * c.height);

            nr_pitch = c.width * 4;
            nr_in = (unsigned char *)malloc((size_t)nr_pitch * c.height);
            if (nr_in == nullptr)
                mgpu::diag::warn("[MGPU][P3.0] out of memory converting the frame - the neural "
                                 "stage is skipped, the P1.5 verdict above still stands");
            else
            {
                const int f = (int)c.format;
                bool ok_fmt = true;
                for (UINT y = 0; y < c.height && ok_fmt; ++y)
                {
                    const unsigned char *srow = pn + (size_t)y * c.fp.Footprint.RowPitch;
                    unsigned char *drow = nr_in + (size_t)y * nr_pitch;
                    for (UINT x = 0; x < c.width; ++x)
                    {
                        const unsigned char *sp = srow + (size_t)x * 4;
                        unsigned char *dp = drow + (size_t)x * 4;
                        if (f == 24)   // R10G10B10A2_UNORM - the game's format on this rig
                        {
                            unsigned v = (unsigned)sp[0] | ((unsigned)sp[1] << 8) |
                                         ((unsigned)sp[2] << 16) | ((unsigned)sp[3] << 24);
                            dp[0] = (unsigned char)(( v        & 0x3FF) >> 2);
                            dp[1] = (unsigned char)(((v >> 10) & 0x3FF) >> 2);
                            dp[2] = (unsigned char)(((v >> 20) & 0x3FF) >> 2);
                            dp[3] = 0xFF;
                        }
                        else if (f == 28 || f == 29)        // R8G8B8A8_UNORM / _SRGB
                        { dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=0xFF; }
                        else if (f == 87 || f == 91)        // B8G8R8A8_UNORM / _SRGB
                        { dp[0]=sp[2]; dp[1]=sp[1]; dp[2]=sp[0]; dp[3]=0xFF; }
                        else { ok_fmt = false; break; }
                    }
                }
                if (!ok_fmt)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P3.0] the captured frame is DXGI format %d, which this "
                             "converter does not handle. The neural stage is skipped rather "
                             "than fed bytes it would misread - a wrong conversion here would "
                             "produce a plausible NR result on a corrupted image, which is the "
                             "worst outcome available. Add the case and rerun.", f);
                    mgpu::diag::error(line);
                    free(nr_in); nr_in = nullptr;
                }
                else
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P3.0] frame converted for the neural stage: DXGI %d -> "
                             "R8G8B8A8_UNORM, %ux%u, %u bytes/row. Two bits per channel were "
                             "discarded; see the note in the source before quoting any quality "
                             "result from this run.",
                             f, c.width, c.height, nr_pitch);
                    mgpu::diag::info(line);
                }
            }
        }
        else if (nonzero == 0)
        {
            mgpu::diag::error("[MGPU][P1.5] PROBE INCONCLUSIVE - the SOURCE frame is entirely "
                              "black, so an all-black arrival proves nothing. Capture during "
                              "gameplay, not on a loading screen or a faded menu.");
        }
        else if (survivors > 0)
        {
            // Two causes, told apart by the reference buffer rather than by the
            // survivor count alone.
            if (diff >= total / 2)
                mgpu::diag::error("[MGPU][P1.5] PROBE FAILED - most of the buffer is still "
                                  "sentinel and the reference has content, so the copies ran but "
                                  "the crossing had not completed when we read. In fallback mode "
                                  "this is the frames-elapsed guess being wrong and the bound "
                                  "should rise; in fence mode it is a real finding - the fence "
                                  "reports the copies retired and the bytes did not arrive, "
                                  "which means the ordering assumption itself is wrong. The "
                                  "[MGPU][P2.0] line above says which mode this was.");
            else
            {
                snprintf(line, sizeof line,
                         "[MGPU][P1.5] PROBE FAILED - %llu true sentinel survivors (received "
                         "%llu, reference %llu) with %llu of %llu pixels differing. A PARTIAL "
                         "arrival, which is neither a clean timing miss nor a clean corruption "
                         "- record the counts before theorising.",
                         survivors, sent, sent_ref, diff, total);
                mgpu::diag::error(line);
            }
        }
        else
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.5] PROBE FAILED - %llu of %llu pixels differ from the reference "
                     "with no sentinel left. Both copies came from one source in one list, so "
                     "they cannot have diverged before transit: the payload changed on the way "
                     "across. Check the footprint arithmetic first - row pitch is %u for a "
                     "%u-pixel row.",
                     diff, total, (unsigned)c.fp.Footprint.RowPitch, c.width);
            mgpu::diag::error(line);
        }
    }
    else mgpu::diag::error("[MGPU][P1.5] readback Map failed - no verdict possible");

    D3D12_RANGE nothing{0, 0};
    if (mn) c.nread->Unmap(0, &nothing);
    if (mg) c.gread->Unmap(0, &nothing);

    // ---- P3.0: the last stage ----
    //
    // Released FIRST, then evaluated. capture_release frees the cross-adapter
    // heap, the shared fence and both readbacks; ngx_probe is about to create
    // several full-resolution textures plus its own upload and readback
    // buffers on the same adapter, and holding the capture's ~28 MiB of
    // mappable allocations across that is free memory pressure for no reason.
    // The frame is already a CPU copy by this point and does not depend on any
    // of it.
    //
    // This runs on the BRIDGE THREAD, the same thread the startup ngx_probe
    // ran on, which is what makes reusing the retained NGX session legitimate.
    // It will block for roughly a second in CreateFeature at native
    // resolution; the bridge's present loop pauses for that long and the game
    // is unaffected, because nothing here touches the game's device.
    // Snapshotted before the release, not read through `c` after it.
    // capture_release does not currently clear these three, but depending on
    // that is depending on the internals of another function to stay the way
    // they are - and a stale width here would be a wrongly-shaped NR run with
    // no obvious symptom.
    const UINT nr_w = c.width, nr_h = c.height;
    const unsigned nr_srcfmt = (unsigned)c.format;

    capture_release();

    // P3.1: NATIVE FIRST, CONVERTED SECOND, BOTH IN ONE LAUNCH.
    //
    // The capture is one shot per process, so a native-format attempt that
    // fails must not cost the launch its P3.0 result - the operator would
    // have to relaunch, get back into gameplay and press the hotkey again to
    // learn one boolean. Running the converted path afterwards makes the
    // native attempt free: worst case the log gains a named failure and the
    // launch still ends with NR having run on the game's frame.
    //
    // The cost of the extra attempt is one CreateFeature, measured at 179 ms
    // at this resolution on a warm session. That is the whole price of the
    // answer.
    bool native_ok = false;
    if (nr_raw != nullptr)
    {
        ngx_input_frame ext{};
        ext.pixels = nr_raw;
        ext.row_pitch = nr_raw_pitch;
        ext.dxgi_format = nr_srcfmt;
        ext.native_format = true;
        native_ok = ngx_probe(nr_w, nr_h, &ext);
        free(nr_raw);
    }

    if (native_ok)
        mgpu::diag::info("[MGPU][P3.1] NATIVE FORMAT ACCEPTED - the converted run is skipped. "
                         "DLSS-NR consumed the game's buffer in the format the game rendered "
                         "it, so no conversion stage belongs in this pipeline. Every earlier "
                         "quality figure taken through the R8G8B8A8 path was measured two bits "
                         "per channel short of what the model can actually see.");
    else if (nr_in != nullptr)
    {
        mgpu::diag::warn("[MGPU][P3.1] native-format attempt did not complete - falling back to "
                         "the converted path so this launch still produces a result. The reason "
                         "is in the [MGPU][P1.0c] / [MGPU][P1.2] lines above, and it is the "
                         "answer: a conversion stage is structural on this format and has to be "
                         "budgeted, ideally as a GPU pass rather than the CPU one used here.");
        ngx_input_frame ext{};
        ext.pixels = nr_in;
        ext.row_pitch = nr_pitch;
        ext.dxgi_format = nr_srcfmt;
        (void)ngx_probe(nr_w, nr_h, &ext);
    }
    if (nr_in != nullptr) free(nr_in);
}

// =====================================================================
// P4.0 - THE STREAM: continuous per-frame capture into a ring, with the seal
// =====================================================================
//
// Everything before this was a probe: one frame, one question, one answer, one
// shot per process. This is the first stage that RUNS - every game frame, into
// a ring of slots, for as long as it is armed.
//
// That changes what can go wrong, completely. P1_INSTRUMENT.md section 00
// lists thirteen transit failures and calls the second half of them QUIET:
// torn, stale, dropped, duplicated, reordered, slot-aliased. Not one of them
// is reachable by a one-shot probe, and not one of them is visible to a person
// watching the bridge window - a stream that consistently delivers frame N-4
// looks perfect on static content. The seal is the instrument that makes them
// nameable, and section 06 committed to shipping it with the first task that
// transits a stream. This is that task.
//
// WHAT THIS DOES NOT DO, stated up front so the log is not over-read:
//
//   - It does NOT verify pixels per frame. The seal proves IDENTITY, ORDER and
//     AGE. P1.5 already proved the payload crosses byte-exact, and re-proving
//     that every frame would cost a full-resolution readback per frame and
//     measure the instrument instead of the transit.
//   - The `barcode` field is written as 0 and NOT CHECKED. It needs a shader
//     that renders the frame index into the pixels (P1_INSTRUMENT section 03),
//     which does not exist yet. Writing frame_index into it here would produce
//     a check that compares a value against itself and always passes - the
//     exact shape of the failures section 00a records. Zero and unchecked is
//     honest; self-comparison would not be.
//   - It does NOT run the neural stage. Feeding this stream to a persistent
//     DLSS-NR feature is the next step and is deliberately separate: if both
//     landed in one commit, a failure would not say which half.
//
// SELF-LIMITING BY DESIGN. This is the first code in the project that adds
// per-frame work to the GAME'S command list, so it stops on its own after
// its frame bound (Frames= in mgpu.ini, default 600) and prints a summary. A
// build that misbehaves costs a
// bounded number of frames rather than the rest of the session.
namespace
{
    // The seal. Layout is fixed and shared by both ends; the static_assert is
    // the requirement and the comment is a courtesy (P1_INSTRUMENT section 01
    // records an earlier draft that asserted 64 while listing 56 bytes).
    struct MgpuSeal
    {
        unsigned int  magic;          // 'MGPU'
        unsigned int  seal_version;
        unsigned long long frame_index;
        unsigned long long qpc_submit;
        unsigned long long payload_bytes;
        unsigned int  width;
        unsigned int  height;
        unsigned int  dxgi_format;
        unsigned int  row_pitch;      // the FOOTPRINT pitch, not width * bpp
        unsigned int  slot_index;
        unsigned int  barcode;        // 0 = not implemented; see the note above
        unsigned int  reserved[2];

        // ---- SEAL v2, R30's spec plus R61's addition ----
        //
        // R30 established this needs 24 bytes to describe a second buffer and
        // that reserved[2] is 8, so the seal grows 64 -> 128. FREE IN SPACE:
        // SEAL_STRIDE has always been 512, so the seal lives in padding the
        // payload alignment already required, and no slot arithmetic moves.
        //
        // NOT free in contract, deliberately. A v1 producer against a v2
        // consumer must fail LOUDLY on the first frame rather than read a
        // colour-only slot as though it had depth in it. The bad_magic path
        // already rejects an unknown version; the bump is what makes it fire.
        unsigned long long depth_offset;   // from the START OF THE SLOT
        unsigned long long depth_bytes;
        unsigned int  depth_width;
        unsigned int  depth_height;
        unsigned int  depth_format;
        unsigned int  depth_pitch;        // FOOTPRINT pitch, not width * bpp

        // R61. THE FIELD NOBODY SPECIFIED AND THE DATA DEMANDED.
        //
        // ReShade drops its depth binding during menus and scene changes. The
        // tap then samples an unbound texture and writes a plane of ZEROS -
        // byte-identical to a legitimate far-plane reading, because far IS 0.0
        // in reversed-Z. Identical bytes, opposite meanings. Without this flag
        // the model is handed false zero depth every time the player opens a
        // menu and nothing in any log would say so.
        //
        // The producer gets the answer for free: get_texture_binding returns
        // nothing exactly when ReShade has nothing bound.
        unsigned int  depth_valid;

        unsigned int  depth_mode;         // 0 off, 1 bound, 2 on the bus unbound
        unsigned int  reserved2[6];

        // ---- SEAL v3, R78: THE MOTION VECTOR REGION ----
        //
        // Same shape as depth's, field for field, because it is the same
        // problem: a second buffer in the slot the consumer must be able to
        // describe without asking the producer. Every thing the depth half
        // needed - its own offset, its own pitch, its own validity - the MVec
        // half needs for the same reason, and a different shape here would
        // only be a second thing to get wrong.
        //
        // 128 -> 192, AND FREE IN SPACE EXACTLY - not approximately. slot_end
        // starts at SEAL_STRIDE + payload_bytes and never at sizeof(MgpuSeal),
        // so the seal's region is 512 whatever this struct weighs: no slot
        // offset, no alignment and no heap size moves. The cost is 128 bytes
        // per frame on the bus (sizeof is copied twice - producer to gxfer,
        // nxfer to readback), which is one part in 195,000 of the payload.
        //
        // WHAT THE VERSION BUMP DOES NOT DO, corrected in place. v2's comment
        // said a v2 producer against a v3 consumer must fail loudly. THAT
        // CANNOT HAPPEN AND THE CLAIM WAS WRONG: the producer and the consumer
        // are the same binary in the same process - stream_on_finish_effects
        // writes this and stream_poll reads it, both in this file - and the
        // heap is created per-arm, so no seal outlives the build that wrote
        // it. The seal crosses an ADAPTER, not a wire.
        //
        // What actually catches a partial source copy is the COMPILER, via
        // stream_on_finish_effects's changed signature. The version field is
        // cheap insurance against a case the compiler already forecloses, and
        // it is not a hard stop either: a mismatch increments bad_magic, logs,
        // and resyncs.
        //
        // THE REASON THESE FIELDS EXIST IS THE SECOND ROUTE. mvec_slot_valid
        // is process memory; mvec_valid crosses the adapter. Two independent
        // routes to one fact means a disagreement is COUNTABLE rather than a
        // tie to break - which is the diagnostic that made depth trustworthy,
        // and the one R71-R77 did not have when the instrument was the thing
        // lying. That is what the 64 bytes buy.
        //
        // AND THE COST OF THE SHAPE: three regions now means three sets of
        // hand-named fields and three copies of the contract check. A fourth
        // input needs v4. A region TABLE - count plus an array of descriptors,
        // looked up by kind - would pay that off once. Deferred option 14,
        // deferred for scheduling and not for design.
        //
        // mvec_valid IS NOT DECORATION, and it is a stronger requirement than
        // depth's. The velocity buffer is a MID-FRAME RENDER TARGET: it is
        // written while the game draws the scene and is a render target again
        // by the time effects run. A frame where our bind hook did not fire -
        // a menu, a load screen, a frame the engine skipped the pass on -
        // leaves the slot's MVec region holding THE PREVIOUS FRAME'S vectors.
        // That is worse than none: the model would reproject confidently
        // against a motion that did not happen. This flag is what makes such
        // a frame ship as colour-plus-depth instead of as a lie.
        unsigned long long mvec_offset;   // from the START OF THE SLOT
        unsigned long long mvec_bytes;
        unsigned int  mvec_width;
        unsigned int  mvec_height;
        unsigned int  mvec_format;
        unsigned int  mvec_pitch;         // FOOTPRINT pitch, not width * bpp
        unsigned int  mvec_valid;
        unsigned int  mvec_mode;          // 0 off, 1 synthetic, 2 estimated, 3 real
        unsigned int  reserved3[6];
    };
    static_assert(sizeof(MgpuSeal) == 192, "seal v3 layout changed");

    const unsigned int SEAL_MAGIC   = 0x5550474Du;   // 'MGPU' little-endian
    const unsigned int SEAL_VERSION = 3u;   // R78: 128 -> 192, MVec described

    // 512, not 64: D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT. The payload in each
    // slot must start on that boundary, so the seal lives in space that would
    // have been padding anyway and costs nothing.
    const UINT64 SEAL_STRIDE = 512;

    struct stream_state
    {
        std::mutex cs;

        // RING DEPTH IS A NAMED CONSTANT, NEVER A HARDCODED 2. P0_RECORD's
        // multi-pass note requires this: depth is entangled with the fence and
        // ownership logic, and changing it later means reopening the
        // synchronisation design.
        //
        // RAISED 3 -> 6 on 2026-09-05, as a MITIGATION and not a fix. The
        // consumer is driven from the bridge's present loop, so its poll
        // cadence is whatever the bridge swapchain's vsync is. Measured: 3.23x
        // the producer's rate on the 210 Hz display, and 1.01x once the bridge
        // card drove a 60 Hz monitor - one poll per produced frame, with the
        // whole safety margin gone. Nothing overran, because each poll drains
        // the entire backlog, but the margin is what protects against a hitch
        // and against a game running faster than the bridge's refresh.
        //
        // Six slots costs ~50 MB at 1080p and buys back the margin the display
        // took away. THE ACTUAL FIX IS TO STOP PACING THE CONSUMER WITH THE
        // PRESENTER - see the note in stream_poll.
        static const unsigned RING = 6;

        // The bound. See the header comment. Overridable with Frames= in
        // mgpu.ini so the window can be watched for longer than fifteen
        // seconds; the default is unchanged.
        unsigned long long max_frames = 600;

        bool requested = false, tried = false, armed = false;
        bool finished = false, summarised = false;
        bool said_other = false, said_overrun = false;

        // ---- D4: the fault selector, resolved ONCE at arm ----
        //
        // This used to be strcmp() against s.fault on every frame, in two
        // places. One of them - the "drop" test - ran on the GAME'S RENDER
        // THREAD, every frame, forever, in service of an injection that fires
        // once at f=40 in runs nobody ships. Microscopic, and exactly what the
        // debt list's selection rule exists to catch: nobody was ever going to
        // measure it while it was still there.
        //
        // Resolved at arm because s.fault cannot change after arm. The strings
        // stay the interface; only the per-frame comparison goes.
        enum fault_kind { FK_NONE = 0, FK_DROP, FK_STALE, FK_PITCH, FK_ALIAS, FK_MAGIC };
        int fault_id = FK_NONE;

        // ---- D2: a fault storm must not cost the bridge thread its frame ----
        //
        // The earlier colour-only run wrote 1543 REORDERED and 541 DROPPED
        // ERROR lines in 141 seconds. Formatting and file I/O for those happens
        // on the bridge thread - the CONSUME thread - during the exact window
        // in which it has no time to spare. The failure mode was spending its
        // scarcest resource on reporting itself, which is a positive feedback
        // loop rather than an inconvenience.
        //
        // THE COUNTERS ARE NOT CAPPED. dropped, reordered and overrun still
        // count every occurrence and the summary still reports the true
        // totals. Only the per-occurrence LINES are bounded, and the periodic
        // line below says how many were suppressed, so a storm is still
        // visible without the storm being made worse by looking at it.
        // ---- B1: THE PRODUCER STRIDE ----
        //
        // The consumer has never been able to tell the producer anything, so
        // the producer seals and copies EVERY game frame whether or not one
        // will ever be read. Measured waste: 14.6% of all transport on the
        // first bounded run, and on the 4080 SUPER pairing in discussion #3
        // roughly 60% - bytes copied out of the render card, across the link,
        // into a ring slot, and discarded on arrival.
        //
        // THE FENCE THE ROUTE DOCUMENT ASKED FOR IS NOT NEEDED. That design
        // called for a second shared cross-adapter fence carrying `consumed`
        // back to the game thread. It was over-engineered: producer and
        // consumer are in the SAME PROCESS, `s.consumed` is written by the
        // consumer under `s.cs`, and stream_on_finish_effects ALREADY HOLDS
        // `s.cs` for its whole body. The lead is a subtraction of two members
        // the producer can already see. No new shared state, no new fence, no
        // new failure mode.
        //
        // A STRIDED FRAME IS NOT A DROPPED FRAME. It is never sealed, never
        // copied, and `produced` does not advance for it, so the sequence the
        // consumer sees stays contiguous and its gap check stays quiet. That
        // is the difference between this and the `drop` fault injection, which
        // advances the counter and leaves a hole on purpose.
        //
        // OFF BY DEFAULT. This changes what crosses the bus, so it is opt-in
        // until it is proven, and FULL stays byte-identical to every published
        // figure while it is off.
        // THE SIGNAL IS A RATE, NOT A QUEUE DEPTH, AND THE FIRST VERSION OF
        // THIS GOT IT WRONG.
        //
        // The obvious signal is `produced - consumed`. It is useless here, and
        // the run that would have proved it was already configured before the
        // error was caught. The consumer CONSUMES every frame - it reads every
        // seal, cheaply, and only skips the EVALUATE - so `consumed` tracks
        // `produced` within a frame or two no matter how far behind the neural
        // stage actually is. Measured: produced=15000, consumed=15000,
        // evaluates=13265. A queue-depth trigger would never have fired, the
        // run would have reported zero strided frames, and that would have read
        // as "B1 does nothing" rather than "B1 was asked the wrong question".
        //
        // What matters is how many game frames pass per EVALUATE, measured over
        // a window: `game_frames / nr_evals`. At 1.17 (this rig today) the
        // honest answer is stride 1 - there is nothing to save, because an
        // integer stride can only cut 0% or 50% and the waste is 14%. At 2.6
        // (the 4080 SUPER pairing in discussion #3) stride 2 halves the
        // transport and the consumer does not notice, because it was already
        // discarding those frames.
        //
        // The window counts GAME frames, including strided ones, so the ratio
        // is stride-invariant and the controller cannot chase its own tail.
        static const unsigned long long STRIDE_WINDOW = 120;
        unsigned stride_max = 0;          // 0 = off; 2..8 = enabled, this is the cap
        unsigned cur_stride = 1;
        unsigned stride_phase = 0;
        unsigned long long game_frames = 0;      // events seen, including strided ones
        unsigned long long stride_skipped = 0;
        unsigned long long stride_hist[9] = {};  // how long we sat at each stride
        unsigned long long ratio_hist[9] = {};   // frames-per-evaluate at each decision
        unsigned long long win_frames_at = 0, win_evals_at = 0;
        // R87 stays honest under striding: the alignment metric counts GAME
        // frames between evaluates, and once `produced` stops counting them
        // the seal index is the wrong unit. The producer records the game-frame
        // number into the slot it is about to seal; the consumer reads it back.
        // Process-local memory, written and read under s.cs, so it needs none
        // of the seal's cross-adapter machinery.
        unsigned long long slot_game_frame[RING] = {};

        static const unsigned long long FAULT_LOG_CAP = 20;
        unsigned long long logged_stale = 0, logged_reord = 0, logged_drop = 0;
        unsigned long long faults_muted = 0, faults_muted_total = 0;

        // ---- R102: Subrect= 25..100, PERCENT OF EACH AXIS. Default 100. ----
        //
        // THE ONLY LEVER LEFT THAT CAN MOVE THE MODEL'S COST, AND IT IS A
        // PARAMETER THIS FILE ALREADY WRITES EVERY FRAME.
        //
        // Four runs on 2026-09-11 established that EvaluateFeature costs
        // 15.05 ms mean / 12.83 ms floor at 2560x1440 on a 5060 Ti, and that
        // the figure is INVARIANT TO ITS INPUTS: 15.049 with colour, depth and
        // real motion vectors bound, 15.054 with colour alone. Five
        // microseconds apart. Binding more information does not buy speed, so
        // nothing that rides the slot can reduce this number.
        //
        // What has never been measured is whether the cost scales with the
        // number of OUTPUT PIXELS. The whole C2 argument - NR at render
        // resolution rather than display resolution, ~0.42 of the area on this
        // title - rests on that scaling law and the law has never been tested.
        //
        // This key tests it without a new tap point. DLSSNR.ColorSubrect* and
        // DLSSNR.OutputSubrect* are set to the full source size on every
        // evaluate today, unconditionally. Setting them smaller asks the model
        // to process a region. If the P2.2 evaluate bracket tracks the area,
        // the law holds and this becomes a shipping lever rather than a
        // research direction; if it does not move, the law is false and C2's
        // projection dies here for the cost of one run.
        //
        // THIS IS A MEASUREMENT KEY, NOT A FEATURE. Below 100 only that region
        // of the output is written and the rest of the window holds whatever
        // was there before, so it is meant to be run with Profile=1. The arm
        // path says so in a warning rather than leaving it to be discovered on
        // screen.
        //
        // BASE IS 0,0 - the top-left region, not the centre. The model's cost
        // should not depend on WHICH region, and if a later run shows that it
        // does, that is a finding rather than a reason to have picked the
        // centre.
        unsigned subrect_pct = 100;
        bool said_subrect = false;

        // ================= C2-SR: DLSS SUPER RESOLUTION ON GPU 1 =================
        //
        // NR is not an upscaler. It is an image-space post-process and it runs
        // at whatever size it is handed. So the only way to make it cheap and
        // still fill the screen is to shrink the image, run NR, and enlarge the
        // result with something that enlarges well.
        //
        // WHY THE SMALL IMAGE MUST BE MADE HERE. NR wants a DISPLAY-REFERRED
        // image - tone mapped. The game's own render-resolution colour is
        // linear HDR and pre-tonemap, so it cannot be the source however
        // convenient its size is. We sit after ReShade's tap, which is tone
        // mapped and at display extent, so the reduction is ours to do and
        // D3D12 has no filtered blit. Hence a compute pass.
        //
        // WHY R IS NOT A FREE PARAMETER. R is set to the GAME'S OWN DLSS render
        // extent, mvec_w x mvec_h - 1485x835 at 2560x1440. That is the one size
        // at which the motion vectors we already transport are native, so no
        // vector is ever resampled. Picking any other R would mean rescaling
        // motion, which is a category of wrong this project has avoided so far.
        //
        // THE CHAIN:  tap (D, tone mapped) -> downscale -> NR at R -> SR R->D
        //
        // SR IS CREATED THROUGH THE CORE, NOT THE SNIPPET. Everything P1.0b and
        // P1.0c fought - the shim, the nvngx.dll substring gate, routing
        // CreateFeature to the snippet - was because NR is an unsupported
        // feature with no core mapping. SuperSampling is feature 1, supported,
        // and takes the ordinary path.
        //
        // DEFECT C APPLIES. Two NGX consumers must not share one parameter
        // block. SR gets its own.
        unsigned sr_on = 0;                 // ini SRUpscale, default 0
        int      sr_preset = 0;             // ini SRPreset, 0 = leave title default
        int      sr_quality = 1;            // ini SRQuality, 1 = Balanced
        unsigned sr_scale_pct = 0;          // ini SRScale, 0 = use the game's extent
        bool     sr_mv_lowres = false;      // set when MVec extent == R
        unsigned sr_w = 0, sr_h = 0;        // R
        NVSDK_NGX_Handle    *sr_handle = nullptr;
        NVSDK_NGX_Parameter *sr_params = nullptr;
        ngx_pf_evaluate_feature sr_eval    = nullptr;
        ngx_pf_release_feature  sr_release = nullptr;
        ID3D12Resource *sr_color[2] = {};   // R, tone-mapped, NR input
        ID3D12Resource *sr_depth    = nullptr;   // R, point-reduced
        ID3D12Resource *sr_nrout    = nullptr;   // R, NR output = SR input
        ID3D12Resource *sr_expose   = nullptr;   // 1x1 R32_FLOAT = 1.0
        ID3D12RootSignature *ds_rs  = nullptr;
        ID3D12PipelineState *ds_pso = nullptr;
        ID3D12DescriptorHeap *ds_heap = nullptr;
        UINT ds_inc = 0;
        unsigned long long sr_evals = 0, sr_fails = 0;
        bool sr_first = true;
        bool sr_ready = false;

        // ---- INNER LOOP, PHASE 1: measure F and k ON THIS CARD ----
        //
        // NAMED CostCurve, NOT Calibrate, AND THE DISTINCTION IS LOAD-BEARING.
        // `Calib` already exists and is the NGX CALIBRATOR (R101/R104/R105) -
        // it patches import slots to read the GAME's own DLSS telemetry, which
        // is where MVecScale, DepthInverted, MVLowRes and the render/display
        // extents come from. It has nothing to do with this.
        //
        // Two keys five characters apart doing unrelated things is DEFECT D's
        // shape: the parser would tell them apart and a person would not. This
        // measures the MODEL'S COST CURVE against area on the card that is
        // present. Log tag [MGPU][CURVE], not [MGPU][CAL].
        //
        // The control law `eval = F + k x area` was fitted on three subrect
        // points taken on one 5060 Ti at 2560x1440. Shipping a loop that solves
        // against those constants would put a 5060 Ti inside the source, which
        // is the one thing this architecture has avoided everywhere else.
        //
        // So it measures them instead: a handful of evaluates at full frame, a
        // handful at half area, solve the line. On whatever card is present, at
        // whatever resolution the game is running.
        //
        // THE FLOOR, NOT THE MEAN, and D5b is what earns that choice. Over four
        // minutes and 13,265 evaluates the floor moved 4 microseconds while the
        // mean wandered 0.4 ms with the scene. The floor is the card's answer;
        // the mean is the card's answer plus whatever happened to be on screen
        // during calibration. Calibrating on the mean would bake the arm-time
        // scene into every solve for the rest of the run.
        //
        // PHASE 1 CHANGES NO BEHAVIOUR. It measures, it logs, and the area
        // stays at 1.0. Off by default. Phase 2 is the solve that actually
        // moves the area, and it is deliberately not written until this has
        // said whether F and k transfer across resolutions.
        static const unsigned CC_WARMUP  = 3;    // discarded: first evaluates after
                                                  // a subrect change are not settled
        static const unsigned CC_SAMPLES = 12;   // kept, per point
        static const unsigned CC_PCT_B   = 71;   // 71% per axis = 50.1% of the area
        unsigned cc_on = 0;          // ini, default 0
        unsigned cc_state = 0;       // 0 idle/off, 1 point A, 2 point B, 3 done
        unsigned cc_n = 0;
        double   cc_floor[2] = {};
        double   cc_area[2] = {};
        float    sr_mv_fix_x = 1.0f, sr_mv_fix_y = 1.0f;
        unsigned sr_mv_mode = 0;           // ini SRMvLowRes: 0 off, 1 on, 2 derive

        // ---- PHASE 1b: THE REFIT. Why the arm-time solve is not the answer.
        //
        // The solve takes the MINIMUM of CC_SAMPLES evaluates as the floor at
        // each area. Min-of-12 drawn from a broad content-dependent
        // distribution is not the floor - it is roughly its 8th percentile -
        // and D5 had already shown this distribution is broad. The 2026-09-12
        // run at 2560x1440 measured the bias directly against its own
        // uncontaminated quarters:
        //
        //   area 1.0   curve said 14.769   run floor 12.92   +14.3%
        //   area 0.5   curve said  7.999   run floor  7.06   +13.3%
        //
        // Both inflated by nearly the same MULTIPLICATIVE factor, which is the
        // whole reason this is fixable cheaply. The RATIO survives it: 1.846
        // against a true 1.830. The shape of the line is right and only its
        // scale is wrong, so the correction is one number applied to both
        // constants rather than a different fit.
        //
        // Taking more samples does not fix a bias, it only narrows the noise
        // around it, and it costs visible half-area frames at arm. So the shape
        // is measured at arm as before and the SCALE is corrected once the run
        // has seen a real floor at area 1.0 - which the run produces for free,
        // because ts_min[2] is exactly that once LEDGER 6b stops the
        // calibration frames from landing in it. Nothing is changed by the
        // refit either; Phase 1 still only measures and logs.
        static const unsigned CC_REFIT_N = 400;  // evaluates after the solve
                                                  // before the floor is trusted
        double   cc_F = 0.0, cc_k = 0.0;         // the arm-time solve, kept
        unsigned long long cc_refit_at = 0;      // ts_n when the solve landed
        bool     cc_refit_done = false;

        ID3D12Device *gdev = nullptr;          // borrowed
        // R28. GPU 1's device, borrowed at arm. Held so the D3D12 info queue
        // can be drained from stream_poll WITHOUT reaching into st() for it -
        // that would mean taking st().cs while holding s.cs, and the lock
        // order in this file is one of the two things that can reach the game.
        ID3D12Device *ndev_b = nullptr;        // borrowed
        ID3D12Heap *gheap = nullptr, *nheap = nullptr;
        ID3D12Resource *gxfer = nullptr, *nxfer = nullptr;
        ID3D12Resource *gup = nullptr;         // UPLOAD, RING seals, game side
        unsigned char *gup_cpu = nullptr;      // persistently mapped
        HANDLE gshare = nullptr;

        ID3D12Fence *gfence = nullptr;         // produced-count, game side
        HANDLE gfence_share = nullptr;
        ID3D12Fence *nfence = nullptr;         // the same fence on GPU 1

        ID3D12Resource *nseal = nullptr;       // READBACK, RING * SEAL_STRIDE

        // ---- P2.2a: GPU timestamps on GPU 1's consume list ----
        //
        // The first numbers in this project that are GPU TIME rather than
        // wall-clock. Everything before this was QPC around a CPU-visible
        // completion, so it carried queue latency and driver overhead mixed in
        // with execution and could not tell them apart - which is why every one
        // of those figures is filed as perishable.
        //
        // Five marks per consumed frame, all on the one list:
        //   0  list start
        //   1  after the seal copy
        //   2  after the payload unpack (cross-adapter buffer -> NR input)
        //   3  after EvaluateFeature
        //   4  after the output sample copy
        //
        // The interesting one is 2->3: what DLSS-NR actually costs on GPU 1,
        // on our path, against the 14.2 ms the reference tool measures on a
        // comparable single-GPU one. That comparison is the whole architecture
        // argument and it has never had our side of it.
        //
        // NOTE THIS NEEDS NO CROSS-ADAPTER CLOCK. These are all GPU 1's own
        // timestamps on GPU 1's own queue, so GetTimestampFrequency is enough
        // and the one symbol still behind the containment guard stays there.
        // (Its name is deliberately not written here - the guard greps this
        // tree, so naming it in a comment would fail the build exactly as
        // calling it would. That is the guard working, not a bug.)
        // Correlating GPU 0's timeline with GPU 1's - which is what a true
        // transit time in GPU time would need - is a separate step and is
        // deliberately not taken here.
        ID3D12QueryHeap *tsheap = nullptr;
        ID3D12Resource *tsread = nullptr;      // READBACK, TS_MARKS * 8 bytes
        UINT64 ts_freq = 0;
        bool ts_ok = false;
        static const UINT TS_MARKS = 5;

        // ---- D5: the SHAPE of the evaluate, not just min/mean/max ----
        //
        // 2.31 ms of the model's cost does not scale with pixels (R102's three
        // subrect points), and the 2.2 ms between the floor and the mean has
        // survived every explanation tried: not the present chain (run B), not
        // the copy-queue DMA (run C), not the motion vectors (run D, which had
        // none and the same spread). Three numbers cannot separate a bimodal
        // split from a long tail from a drift, and those have three different
        // causes.
        //
        // Buckets are PERCENT OVER THE OBSERVED FLOOR, not absolute
        // milliseconds, so the same instrument reads correctly at any subrect,
        // any resolution and on any card - the thing an absolute ladder tuned
        // to 2560x1440 on a 5060 Ti could not do.
        //
        // The floor moves during a run, so the earliest samples are bucketed
        // against a floor that was not yet the floor. That biases the first
        // handful HIGH and nothing else; it is a distribution-shape
        // instrument, not a measurement.
        static const UINT TS_BUCKETS = 8;
        unsigned long long ts_hist[TS_BUCKETS] = {};

        // ---- D5b: the run in QUARTERS, because the aggregate cannot see time ----
        //
        // D5's histogram rules out two clock states and two code paths - there
        // is no second hump - but it is summed over the whole run, so a broad
        // spread and a slow drift produce the SAME picture. Those have opposite
        // causes and opposite conclusions.
        //
        // The sharpest discriminator is not the shape, it is THE FLOOR OVER
        // TIME. Content-dependent work does not move the floor: the cheapest
        // frame in the scene stays cheap however hard the other frames get.
        // Thermal or power behaviour moves it, because at a lower clock even
        // the cheapest frame costs more.
        //
        //   min rises across quarters              -> the card. No code fixes it.
        //   min flat, mean rises                   -> the scene got harder.
        //   both flat, spread wide                 -> content, and D5's shape is
        //                                             the honest description.
        //
        // Four segments rather than two halves: a drift that only starts in the
        // last third is invisible to a split at the midpoint, and four costs the
        // same three arrays two would.
        static const UINT TS_SEGS = 4;
        double             ts_seg_min[TS_SEGS] = {};
        double             ts_seg_max[TS_SEGS] = {};
        double             ts_seg_sum[TS_SEGS] = {};
        unsigned long long ts_seg_n[TS_SEGS] = {};
        double ts_sum[TS_MARKS - 1] = {};
        double ts_min[TS_MARKS - 1] = {};
        double ts_max[TS_MARKS - 1] = {};
        unsigned long long ts_n = 0;
        ID3D12CommandQueue *nq = nullptr;
        ID3D12CommandAllocator *na = nullptr;
        ID3D12GraphicsCommandList *nl = nullptr;
        ID3D12Fence *nf = nullptr;
        HANDLE nev = nullptr;
        UINT64 nf_value = 0;

        // ---- R26: a dedicated COPY queue on GPU 1 ----
        //
        // Everything in this project's live path has been TYPE_DIRECT since
        // P2.0; the P2.1 lateral probe's copy queues are the only ones that
        // have ever existed here and they tore themselves down at the end of
        // the experiment. So "GPU 1's own queue", singular, in the P2.2 line
        // was literally true: unpack and evaluate serialise on one engine.
        //
        // This queue is the DMA engine rather than the 3D engine. It is
        // created beside the DIRECT set, not instead of it, and it is OFF by
        // default - CopyQueue=1 in mgpu.ini - because every published figure
        // was measured without it and the control has to stay reachable.
        //
        // READ THE cq_ahead COUNTER BEFORE BELIEVING ANY RESULT FROM THIS. A
        // separate queue does not by itself overlap anything: see the long
        // note in stream_poll for what the loop's own structure permits.
        // 0 = shipped, 1 = queue split with a CPU-tested arrival, 2 = queue
        // split with a GPU-SIDE arrival gate (R28-B). copy_queue is "the copy
        // queue is in use at all" and cq_gated is "mode 2", so every path
        // written for mode 1 stays literally unchanged in mode 2 except where
        // it asks cq_gated.
        int copy_queue_mode = 0;
        bool copy_queue = false;
        bool cq_gated = false;
        ID3D12CommandQueue *cq = nullptr;
        // Two of each, because a prefetched copy for frame N+1 can still be in
        // flight while frame N's is recorded, and an allocator may not be
        // Reset under the GPU. Two is also the bound - tex_in has two halves,
        // so frame N+2 would land in the one frame N is reading.
        ID3D12CommandAllocator *cqa[2] = {};
        ID3D12GraphicsCommandList *cql[2] = {};
        ID3D12Fence *cqf = nullptr;
        UINT64 cqf_value = 0;                  // the frame index last signalled
        // The copy-fence value each allocator was last submitted with. NEVER
        // FREE - OR RESET - UNDER THE GPU is the existing rule and an
        // allocator is the easiest place to break it: a frame whose neural
        // work was SKIPPED submits no Wait on cqf, so a copy prefetched for it
        // is never fenced behind anything the CPU waited on, and two frames
        // later its allocator would be Reset on nothing better than a
        // probability. Checked, not assumed, and checked WITHOUT BLOCKING -
        // the answer to a busy allocator is to fall back to the inline unpack
        // for that frame, not to wait for the GPU with the stream mutex held.
        // That mutex is taken by the GAME's render thread; DEFECT E is what
        // blocking under it costs.
        UINT64 cqa_value[2] = {};
        unsigned long long cq_issued = 0;      // highest frame whose copy is submitted
        unsigned long long cq_ahead = 0;       // copies that ran ahead of the previous evaluate
        unsigned long long cq_fails = 0;       // frames that fell back to the inline unpack
        // Armed for a frame THAT HAD NOT ARRIVED YET - only possible in mode 2,
        // and the whole point of it. In mode 1 this is always 0 by
        // construction, which is the difference between the two arms stated as
        // a number rather than as a claim.
        unsigned long long cq_spec = 0;
        // R28. The D3D12 info queue has never been drained anywhere in the
        // stream path - transit_drain_info_queue is called only on P1.3 probe
        // failures - so a validation message raised by a STREAM submission has
        // had nowhere to appear. That is the only instrument that can answer
        // whether the COMMON-rather-than-COPY_DEST barrier reasoning in
        // stream_cq_submit is right, which is the riskiest line in R26.
        // Drained twice: once after the first consumed frame that ran the
        // neural stage, so a state error sits in the log next to the frame
        // that caused it, and once at the summary for everything after.
        // Costs nothing with no debug layer - the QueryInterface simply fails.
        bool iq_first_done = false;

        // ---- R26 gotcha, and it is two gotchas, not one ----
        //
        // A COPY queue's timestamp clock is NOT the direct queue's, so these
        // ticks are divided by cq_ts_freq and never by ts_freq - the other way
        // round is a confidently formatted wrong number, section 00 again.
        //
        // The part R26 did not name: a copy queue also needs its own query
        // heap TYPE (COPY_QUEUE_TIMESTAMP, not TIMESTAMP) and the adapter has
        // to claim CopyQueueTimestampQueriesSupported at all. When it does
        // not, the transport still works and only the measurement is absent,
        // which the summary says out loud rather than reporting the direct
        // queue's now-empty unpack bracket as though it were the cost.
        ID3D12QueryHeap *cq_tsheap = nullptr;
        ID3D12Resource *cq_tsread = nullptr;    // READBACK, 2 marks x 2 parities x 8 bytes
        UINT64 cq_ts_freq = 0;
        bool cq_ts_ok = false;
        double cq_ts_sum = 0.0, cq_ts_min = 1e30, cq_ts_max = 0.0;
        unsigned long long cq_ts_n = 0;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 payload_bytes = 0, slot_bytes = 0;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

        unsigned signal_at = 0;            // ini SignalAt: 0 effects, 1 present
        unsigned present_vsync = 1;        // ini PresentVsync: 1 = Present(1,0)
        unsigned sr_snippet = 1;           // ini SRSnippet: 1 driver, 0 game's

        // ---- V27: LIVE PRESET / QUALITY, STAGE AND COMMIT ----
        //
        // Quality and preset are baked into the NGX feature handle at create
        // time, so changing either means releasing that handle and building a
        // new one. That is why these are a REQUEST rather than a setter: the
        // panel stages, and stream_poll commits at the top of a poll, which is
        // the only moment on the bridge thread when nothing is in flight.
        //
        // NOTHING ELSE IS REBUILT. R does not change with quality or preset,
        // so the R-sized textures, the reduce PSO and the whole transport are
        // untouched. A change that moves R - SRScale, SRMvLowRes - is a bigger
        // teardown and stays in the ini for now (roadmap D1).
        unsigned sr_rebuild_req = 0;
        int      sr_req_quality = -1;      // -1 = leave as is
        int      sr_req_preset  = -1;
        int      sr_req_scale   = -1;      // V28: R as a % of display, -1 = leave
        unsigned sr_rebuild_n   = 0;       // how many have been committed

        // ---- V28: THE INNER LOOP. "AUTO". ----
        //
        // WHAT IT IS: the consumer sizing its own work so that GPU 1 finishes
        // inside the frame budget, instead of the operator picking a quality
        // and hoping. The area it can afford IS the quality it can deliver,
        // and that gap is continuous rather than a cliff.
        //
        // WHY IT IS A FEEDBACK CONTROLLER AND NOT THE SOLVE THE ROUTE
        // DOCUMENT DESCRIBES. That document says
        //     area = (target_interval - cpu_residual - F) / k
        // and ledger 6e/6g WITHDREW that model: F was curvature fitted as an
        // intercept over a narrow range, there is no fixed term, and what
        // transfers is one curve per card in evaluated megapixels. Solving
        // open-loop from two coefficients we no longer believe in would be
        // building on a retracted result.
        //
        // So this measures what the evaluate ACTUALLY costs on this card, in
        // this scene, at the R it is currently running, and steps. No model,
        // no constants naming a GPU, nothing that needs to transfer. The cost
        // is one rebuild per step and the ladder is short.
        //
        // HYSTERESIS AND COOLDOWN ARE THE WHOLE DESIGN. The route document
        // warns that the failure mode is a depth that oscillates rather than
        // settles, and a controller that acts on every window will do exactly
        // that at the boundary. Step DOWN at 105% of budget, step UP only
        // below 80%, and do nothing at all for a cooldown after either.
        unsigned auto_on = 0;              // ini Auto: 0 off, 1 on
        unsigned auto_target_fps = 60;     // ini AutoTargetFps
        double   auto_budget_ms = 0.0;     // derived at arm
        unsigned auto_rung = 0;            // index into AUTO_LADDER
        double   auto_win_sum = 0.0;
        unsigned auto_win_n = 0;
        unsigned long long auto_hold_until = 0;   // consumed count
        unsigned auto_changes = 0;
        double   auto_last_mean = 0.0;
        // R as a percentage of the display extent. Coarse on purpose: every
        // rung costs a rebuild, and a ladder fine enough to be smooth would
        // spend more time rebuilding than rendering.
        static const unsigned AUTO_LADDER[5];
        static const unsigned AUTO_RUNGS = 5;
        int      sr_rebuild_last_ok = -1;  // -1 none yet, 0 failed, 1 ok
        // Which snippet ACTUALLY answered, as opposed to which one was asked
        // for. sr_snippet is the request; this is the outcome, and the panel
        // must show the outcome - a user whose driver copy was not found needs
        // to see the fallback, not the intention.
        bool     sr_snippet_driver = false;

        // ---- PANEL TELEMETRY ----
        // Rates are derived in ui_read from the overlay's own cadence rather
        // than accumulated per frame: the panel is the only consumer, it is
        // drawn at a sane rate, and a counter the stream has to maintain every
        // frame for a window nobody has open is cost for nothing. The 0.25 s
        // gate is what keeps the number readable - the overlay is drawn on both
        // runtimes, so ui_read can be called more than once per frame.
        LARGE_INTEGER      ui_tlast{};
        unsigned long long ui_plast = 0, ui_clast = 0;
        double             ui_fps_prod = 0.0, ui_fps_cons = 0.0;

        // ---- RING WINDOW: allocate 6, use as few as the run needs ----
        //
        // RING is 6 and stays 6 - reallocating mid-stream is how the teardown
        // crashes of P1.0 come back. What adapts is how far behind the newest
        // frame the consumer is willing to start, which costs nothing to change
        // and can change every 120 frames.
        //
        // WHY IT MATTERS NOW AND DID NOT BEFORE. With the game's low-latency
        // mode off the producer ran ~58-71 fps and the consumer idled on three
        // quarters of its polls - six slots of headroom nobody needed. With it
        // ON the producer reaches ~138 fps, which is 7.2 ms per frame against
        // GPU 1's 7.0 ms of work. The consumer stops being 4x idle and lands at
        // about 1.0x. Depth starts being spent rather than wasted.
        //
        // ASYMMETRIC ON PURPOSE. An overrun is a visible fault; a frame of
        // latency is not. So grow on the first overrun and shrink only after
        // three clean, mostly-idle windows.
        static const unsigned RW_MIN = 2, RW_MAX = RING, RW_WINDOW = 120;
        // OFF BY DEFAULT. The window is a MEMORY and tidiness argument, not a
        // latency fix - 6i settled that the latency lives elsewhere. And until
        // its skips are fully separated from the fault counters it can make a
        // clean transport look broken, which is a worse trade than carrying
        // four unused slots. RingWindow=1 turns it on.
        unsigned rw_on = 0;
        unsigned rw = RING;                 // current usable depth
        unsigned rw_clean = 0;              // consecutive clean windows
        unsigned long long rw_at = 0;       // frame the window started
        unsigned long long rw_over_at = 0;  // overrun count at window start
        unsigned long long rw_idle_at = 0, rw_polls_at = 0;
        unsigned long long rw_skipped = 0;  // frames skipped BY the window
        unsigned rw_hist[7] = {};           // how long each depth was held
        unsigned long long produced = 0;   // frames recorded into the game's list

        // ---- L1: WHERE THE 62 ms ACTUALLY IS ----
        //
        // `lat` runs from the seal's qpc_submit to the poll that consumes it,
        // and it has been blamed on two things that turned out to be innocent:
        // ring depth (the consumer idles on 75% of its polls, so the ring is
        // nearly EMPTY) and the game's queue depth (ReflexMode=Enabled moved it
        // 62.68 -> 64.14, i.e. not at all).
        //
        // This measures the remaining candidate directly: how far behind GPU 0
        // ALREADY IS at the instant we record a copy. Sampled on the game's
        // render thread, but GetCompletedValue does not block and does not
        // wait - DEFECT E stands.
        //
        // ONE FRAME OF THE ANSWER IS ALREADY KNOWN AND IS STRUCTURAL. The
        // signal for frame N is issued on frame N+1's event, deliberately,
        // because ReShade executes our recorded list after the handler returns
        // and a signal issued in the same event would clear before the data
        // existed. So a backlog of 1 is the floor and is not a defect.
        //
        //   histogram mass at 1        the fence lag IS the latency, and the
        //                              only lever is where the signal is issued
        //   mass at 3-4                GPU 0 is genuinely that far behind and
        //                              the copy is waiting on the game's work
        //   mass at 0 with high lat    the copy itself is slow, which would be
        //                              the most surprising answer and the most
        //                              interesting one
        unsigned long long q_hist[8] = {};
        unsigned long long q_sum = 0, q_n = 0, q_max = 0;
        unsigned long long consumed = 0;   // frames whose seal has been checked
        unsigned long long last_seen = 0;

        // Counters. A quiet failure is a RATE, not an event, which is why the
        // summary matters more than any single line.
        unsigned long long dropped = 0, reordered = 0;
        unsigned long long bad_magic = 0, contract = 0, alias = 0, overrun = 0;

        // P4.0a. THE REUSE COUNTER IS GONE AND THIS REPLACES IT.
        //
        // P1_INSTRUMENT section 01 specifies `reuse` for a SAMPLING consumer -
        // one that re-reads the newest seal every poll and therefore sees the
        // same frame_index four or five times in a row. This consumer is
        // EVENT-DRIVEN: it reads each new frame exactly once and never revisits
        // one. `reuse` could therefore only ever be zero, and the first run
        // duly printed `reuse=0` - a number with one possible value, reported
        // as though it were a measurement. That is the failure family in
        // section 00a, committed by the instrument that documents it.
        //
        // What the rate ratio actually is here: polls that found nothing new,
        // over polls in total. It measures the same underlying thing - how much
        // faster the consumer runs than the producer - out of quantities this
        // design can actually observe.
        unsigned long long polls = 0, idle_polls = 0;

        // Every logged sample of the first build read `slot=2`, because the
        // sample stride was 60 and the ring is 3. All 600 frames were checked
        // across all three slots, but nothing in the log said so. The histogram
        // says so, and the stride below is now coprime with the depth.
        unsigned long long slot_hits[RING] = {};

        LARGE_INTEGER freq{};
        double lat_min = 1e30, lat_max = 0.0, lat_sum = 0.0;

        // ---- L2: READY-TO-CONSUME, the number a player actually experiences ----
        //
        // `lat` runs from qpc_submit, which is stamped on the game's render
        // thread when the copy is RECORDED. L1 then measured that GPU 0 is
        // three frames behind at that instant: 1424 of 1500 samples at exactly
        // 3, never 4. The CPU is three frames ahead of the GPU, so WHEN WE
        // STAMP THAT TIMESTAMP THE FRAME HAS NOT BEEN RENDERED YET.
        //
        // Our copy is appended to frame N's own list, so it executes the moment
        // frame N's graphics work finishes. It is not queued behind anything.
        // It is as prompt as it can physically be. So `lat` has been measuring
        // CPU LEAD TIME, not transport delay, since it was written - and three
        // separate fixes were proposed against it (ring depth, Reflex, a GPU 0
        // copy queue) before anyone checked what it was measuring.
        //
        // ready_* starts the clock where the data starts existing: the first
        // poll that observes the produced fence reach this frame. Everything
        // after that really is ours - poll granularity, transit, evaluate,
        // present.
        //
        //   near 10 ms   the 3-frame floor is an artifact of where a timestamp
        //                was taken, the GPU 0 copy queue is unnecessary, and
        //                ledger 6h closes because there was nothing there.
        //   near 60 ms   the ordering argument above is wrong and the copy is
        //                genuinely waiting on something. Build the queue.
        double ready_min = 1e30, ready_max = 0.0, ready_sum = 0.0;
        unsigned long long ready_n = 0;
        unsigned long long seen_fence_at = 0;    // QPC of the poll that saw it
        unsigned long long seen_fence_val = 0;   // what it had reached
        unsigned long long lat_n = 0;
        // An outlier with no name is not a measurement either: the first run
        // reported max=672.06 ms nine times above the mean and could not say
        // which frame it was. The first consumed frame is also separated out -
        // its age includes everything between arming and the first poll, which
        // is not transit.
        unsigned long long lat_max_frame = 0;
        double first_lat = 0.0;

        // Fault injection (P1_INSTRUMENT section 04). Absent file = no fault,
        // so the shipped default is a clean run and a missing file is never an
        // error.
        char fault[32] = "none";
        bool fault_unimpl = false;   // a name was given that this build cannot inject

        // ---- P5.1: pipeline cleanup ----
        // `presented` is the value `consumed` had when the bridge last put a
        // frame on screen. The two being equal means there is nothing new to
        // show, and the present is skipped entirely.
        unsigned long long presented = 0;
        HANDLE gate_ev = nullptr;
        unsigned long long gate_waits = 0, gate_presents = 0, gate_idle = 0;
        // Profile=1 in mgpu.ini strips the instrument down to what a shipping
        // build would carry: no on-screen output, no liveness sample. The seal
        // stays - it is the correctness check, it is 64 bytes, and a
        // measurement run that silently stops checking identity is how a
        // corrupted stream gets recorded as a fast one.
        bool profile = false;
        // P5.3: Present=in shows the NR INPUT in the bridge window instead of
        // the NR output. Display only - the neural stage still runs and is
        // still measured, so a Present=in run and a Present=nr run are
        // otherwise the same run.
        // P7.4: present_mode replaces the present_in bool. 0 = the neural
        // output, 1 = the frame handed TO the model, 2 = SPLIT - the left half
        // of the input beside the right half of the output, THE SAME FRAME,
        // in one backbuffer.
        //
        // Split exists because the quality comparison was impossible without
        // it. Two runs cannot be aligned: no game replays a frame, and an
        // exterior with a sky changes underneath you, so a difference between
        // two captures is never cleanly attributable to the setting that
        // changed. Splitting one frame removes the question entirely - both
        // halves are the same instant, the same camera, the same weather.
        //
        // It is also the only honest way to film this. Cutting between two
        // recordings proves nothing to a sceptical viewer; a live seam does.
        int present_mode = 0;

        // P7.4: the intensity SHAPE, held as a mode rather than written once.
        // 0 = manual, 1 = front-loaded (pass 1 at FULL, every other pass at
        // FLOOR), 2 = back-loaded (last pass at FULL, every other at FLOOR).
        //
        // A preset that only wrote numbers would silently stop describing the
        // experiment the moment the pass count moved: set "max on the last
        // pass" at 4 passes, go to 5, and the max is now on the second to last
        // one while the panel still says back-loaded. Since the demo changes
        // the pass count live, on camera, the shape has to FOLLOW the count.
        // Touching any single slider drops back to manual, so nothing is
        // trapped - re-picking a preset re-applies it.
        int preset = 0;

        // P7.5: where the split seam sits, 0.0 (all output) .. 1.0 (all input).
        // A fixed centre seam is the wrong instrument for a face: the thing
        // worth comparing is rarely in the middle of the frame, and asking the
        // viewer to move the CAMERA to bring it to the seam changes the frame
        // being compared. Moving the seam instead leaves the frame alone.
        float split_pos = 0.5f;

        static constexpr float PRESET_FLOOR = 0.10f;
        static constexpr float PRESET_FULL  = 2.00f;
        int window_mode = 2;   // P7.0: 0=crop 1=match 2=fit

        // ---- P4.1: the persistent neural stage ----
        // Opt-OUT (mgpu.ini Neural=0), because running without it is now the
        // control rather than the default: the pace of the consumer with and
        // without NR is the comparison that answers whether it keeps up.
        bool neural = true;
        bool nr_tried = false, nr_ok = false;
        // ---- V43: THE RECOVERY LAUNCH ----
        //
        // Set at startup when mgpu.ini still carries ArmCrashed=1, which means
        // the PREVIOUS launch entered CreateFeature and never came back out.
        // While this is true the add-on does not touch NGX at all: no probe
        // Init, no arm, no AutoArm. That is not a safety pause - it IS the
        // fix. A launch in which our module leaves NGX alone is exactly what
        // renaming the add-on to .bak and running the game once does, and that
        // procedure is the only thing that has ever cleared this residue.
        // Confirmed 2026-09-13: Neural=0, launch, quit, Neural=1, and the
        // next launch arms.
        bool recovery = false;
        bool recovery_cleared = false;
        unsigned recovery_frames = 0;
        // V44. recovery can now be entered two ways and they behave
        // differently at the end of the launch:
        //   from the sentinel (ArmCrashed=1) - this launch IS the clearing
        //     launch, so the key is cleared after 600 presented frames;
        //   from a caught fault in THIS launch - the key was just rewritten
        //     deliberately and must survive, so it is not cleared.
        // Without the distinction the second case would wipe the note it had
        // only just written.
        bool recovery_clears = false;
        // V29. NGX said its session was stale and we stopped before entering
        // CreateFeature. Not a failure yet - a reason to wait and ask again.
        // V31: the V29/V30 stale-session retry is gone - see stream_nr_create.
        // nr_attempts is kept only so the log can say the arm was attempted.
        unsigned nr_attempts = 0;
        NVSDK_NGX_Parameter *nr_params = nullptr;
        // ---- P6.0: N neural passes per frame ----
        //
        // The architectural claim this project has argued from the start is
        // that offloading pays once the neural work is more than one pass:
        // transport is paid ONCE per frame whatever N is, while doing the same
        // work locally costs GPU 0 the full per-pass time out of its own frame
        // budget. Nothing has ever tested it, because until now there was only
        // ever one pass.
        //
        // ONE FEATURE HANDLE PER PASS, not one handle evaluated N times. The
        // feature carries temporal history (dlssnr_prev_output), so a single
        // handle run twice in a frame would have its history be "the previous
        // PASS" rather than "the previous FRAME". The GPU cost would be the
        // same and the measurement would still be valid - but the picture would
        // ghost, and an artefact of the test rig that looks exactly like a real
        // fault is the thing this instrument exists to avoid.
        //
        // Passes=1 is the default and is byte-identical to the P5 behaviour:
        // one handle, one evaluate, tex_out is the output. Nothing about the
        // shipped path changes unless the ini asks for it.
        // P7.7: 2, DOWN from 6, and this is a POWER decision rather than a
    // performance one.
    //
    // Measured on the development rig at 1080p, with a ~16.7 ms frame period:
    //
    //   1 pass    8.3 ms of GPU 1 work per frame  ~50% duty   ~49 W
    //   2 passes  17.5 ms - about the whole frame  ~100% duty  ~145 W
    //   3 passes  26.0 ms - OVER the frame period  saturated   ~180 W = the cap
    //
    // Two passes fill the card. Three exceed the frame period, so the card
    // clamps at its power limit and every further pass is served on throttled
    // clocks - 3, 4, 5 and 6 all drew an identical ~180 W, which is the limiter
    // and not a coincidence. Past two, a pass buys latency rather than picture.
    //
    // The reason it is enforced in code rather than documented is the failure
    // mode at the other end of the range. This add-on ships with Frames=0, so a
    // run is unbounded: it holds the second GPU at whatever load it reaches for
    // as long as the game is open, in a window the user has very likely
    // minimised. On a larger card at a higher resolution those watt figures
    // scale with the hardware, not with the numbers above. Sustained maximum
    // board power is the condition under which a marginally seated power
    // connector fails, and "they can set it back down in the ini" is not a
    // safety argument for a setting whose cost is invisible until something
    // melts.
    //
    // The ghosting experiment that motivated six passes is NOT settled by any
    // of this - it was never run, because no ghosting scene was ever captured.
    // If it is run later, raise this constant in a local build. It does not
    // ship raised.
    static const unsigned MAX_PASSES = 2;
        unsigned passes = 1;
        NVSDK_NGX_Handle *nr_handle[MAX_PASSES] = {};
        ngx_pf_evaluate_feature nr_eval = nullptr;
        ngx_pf_release_feature nr_release = nullptr;
        // V40. The parameter blocks' destructor. NEVER CALLED UNTIL NOW: the
        // stream creates nr_params and sr_params and leaks both on every run,
        // because DestroyParameters only ever ran in the P1.0c probe path.
        ngx_pf_destroy_params ngx_destroy_params = nullptr;
        // V36. The NGX SESSION's closer and the device it was opened on.
        ngx_pf_shutdown1 ngx_shutdown = nullptr;
        ID3D12Device    *ngx_dev = nullptr;
        // V38. stream_shutdown() is now called from TWO places - the bridge
        // thread's ordered teardown and ReShade's destroy_device event -
        // because relying on one of them is what left the session open on any
        // exit that did not unwind cleanly. Whichever arrives first does the
        // work; this makes the second a no-op rather than a double release.
        bool ngx_shut_done = false;
        // Ping-pong. tex_in has no UAV flag - it is a copy destination and can
        // never be a neural OUTPUT - so passes alternate between tex_out and
        // tex_pong, both of which are UAV. `nr_final` is whichever one the last
        // pass wrote, and it is what the window presents.
        // R26. TWO INPUT TEXTURES, indexed by frame parity - but tex_in[1] is
        // created ONLY when the copy queue is on, so a CopyQueue=0 run
        // allocates exactly what every published run allocated.
        //
        // The second one is not decoration. For frame N+1's unpack to run
        // while frame N's evaluate is still reading its input, the copy cannot
        // land in the texture the model is reading. Without the pair there is
        // no overlap available at all and the queue is plumbing with nothing
        // flowing through it.
        ID3D12Resource *tex_in[2] = {};
        // The state the pair RESTS in between frames, and what the presenter
        // must transition from. COPY_DEST on the shipped path. COMMON with the
        // copy queue on, because D3D12 decays every resource a COPY queue
        // touched back to COMMON when that submission completes - see the note
        // above stream_cq_submit.
        D3D12_RESOURCE_STATES tex_in_rest = D3D12_RESOURCE_STATE_COPY_DEST;
        // The half the last consumed frame actually fed to the model. Present=in
        // and the split's left half must show THAT one, not whichever half the
        // copy queue is filling next.
        ID3D12Resource *tex_in_show = nullptr;
        ID3D12Resource *tex_out = nullptr, *tex_pong = nullptr,
                       *nr_final = nullptr, *nr_read = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT nr_fp{};   // the 64x4 liveness sample
        unsigned char nr_prev[1024] = {};
        bool nr_have_prev = false;
        unsigned long long nr_evals = 0, nr_fails = 0, nr_same = 0;
        // P6.0: nr_evals counts PASSES now. The liveness sample is taken once
        // per frame, so its rate needs a frame count of its own - dividing by
        // evaluates would report a 2-pass run's identical-rate at half its
        // true value.
        unsigned long long nr_frames = 0;

        // ---- P6.2: the knobs ----
        //
        // Until now exactly one quality parameter was set, hardcoded:
        // DLSSNR.Intensity = 0.84. No model, no encoding, no white point. The
        // reference addon's own panel exposes several we never touch, and the
        // observation that drove this - four passes fixed ghosting, noise and
        // blur but washed the colour out - is a cumulative effect that a
        // per-pass strength is the natural lever against.
        //
        // intensity[] is per pass. IntensityN in the ini overrides pass N;
        // absent, every pass uses Intensity. A strong first pass with gentle
        // later ones is the configuration that observation suggests, and it
        // could not be expressed before.
        // One initialiser per pass. This list is sized by MAX_PASSES and an
        // over-long list is a compile error, which is the intended behaviour:
        // it forces this line to be revisited whenever the bound moves.
        float intensity[MAX_PASSES] = { 0.84f, 0.84f };

        // ---- P8.0: DLSSNR.MVec - the half of the model's contract that has
        // never been fed ----
        //
        // Every evaluate this project has ever run bound Color and Output and
        // nothing else. DLSS-NR carries temporal history (dlssnr_prev_output)
        // and REPROJECTS it with motion vectors; with none, that history is
        // misaligned on every frame the camera moves, so the model cannot
        // reuse what it synthesised and re-synthesises instead. Detail that is
        // re-invented per frame is in the same spatial band as the detail -
        // which is why no spatial filter separated the two - and it worsens
        // with strength because there is more synthesis to be unstable.
        //
        // MODE:
        //   0  off. The control arm, byte-identical to every published run.
        //   1  SYNTHETIC: a constant field. A CONTROL in the P4.2 depth sense,
        //      not a feature - two evaluates identical but for whether a
        //      NON-ZERO MVec is bound. Same output means NR does not read MVec
        //      on this path and no flow estimator is worth writing.
        //   2  ESTIMATED - flow from colour on GPU 1. Not in this build.
        //   3  REAL - R78. THE GAME'S OWN velocity buffer, identified by the
        //      P12 probe, copied across the bus into its own slot region and
        //      bound. This is the only mode that can answer the question the
        //      depth arm could not: the model reprojects its history with
        //      MVec, so a MVec that is genuinely the scene's motion is the
        //      first input that can make temporal reuse work.
        //
        // SIGN AND SCALE ARE LIVE ON PURPOSE. Which way NVIDIA's vectors point
        // and in what units cannot be read back, only observed, and P6.3
        // exists because finding a value used to cost one launch per value.
        //
        // R32G32_FLOAT rather than R16G16_FLOAT: 16.6 MB at 1080p on a card
        // that is half idle, and it keeps a float-to-half conversion out of a
        // probe whose purpose is to not be wrong.
        ID3D12Resource *tex_mvec = nullptr;
        ID3D12Resource *mvec_up  = nullptr;      // UPLOAD, one full frame
        unsigned char  *mvec_cpu = nullptr;      // persistently mapped
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT mvec_fp{};
        UINT64 mvec_bytes = 0;
        int   mvec_mode = 0;

        // ---- R78: THE REAL MOTION VECTOR TRANSPORT, MVec=3 ----
        //
        // MODE 3 IS A DIFFERENT THING FROM MODES 1 AND 2 and shares almost
        // nothing with them. 1 and 2 synthesise a field ON GPU 1 and upload it
        // there; 3 copies THE GAME'S OWN velocity buffer across the bus. So it
        // needs its own footprint - sized from the game's resource, on the
        // game's device - its own slot region and its own validity, all of
        // which the depth half already needed and none of which the synthetic
        // path has. mvec_fp / mvec_bytes above describe the GPU 1 upload and
        // are left exactly alone; these describe the bus.
        //
        // THE SOURCE IS MID-FRAME AND THAT IS THE WHOLE DIFFICULTY. Depth
        // arrives as a handle at finish-effects time because ReShade holds it
        // for us. NOTHING holds the velocity buffer. So the copy is issued
        // FROM THE BIND EVENT, mid-frame, by stream_mvec_copy, into the slot
        // this frame is about to seal - see the note on that function for why
        // that slot is knowable there.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT mvec_fp2{};
        UINT64 mvec_bytes2 = 0;
        UINT64 mvec_off    = 0;      // offset WITHIN the slot, 512-aligned
        unsigned mvec_w = 0, mvec_h = 0, mvec_format = 0;

        // depth_slot_valid's twin, for the same reason and read under the same
        // mutex: the consumer unpacks before the seal readback exists, so it
        // cannot ask the seal whether this slot carries vectors.
        // ---- R88: ATOMIC, so the consumer never has to be waited on ----
        //
        // R87 measured lock-skipped=13 at two passes - the first time that
        // counter moved. R78 said in its own comment that more than a handful
        // means the consumer holds the mutex too long and THAT is the finding.
        // It is. At two passes the bridge thread's sections are twice as long
        // and the game's render thread was bailing mid-scene rather than wait.
        //
        // These are the only things stream_mvec_copy shared with the consumer.
        // Made atomic, they need no lock at all, and the small mutex below is
        // left with exactly one job.
        std::atomic<unsigned char> mvec_slot_valid[RING] = {};

        std::atomic<unsigned long long> mvec_copies{0}, mvec_missing{0};
        // R118. One-shot: the fallback arms once per armed stream and never
        // disarms itself. A route that was dead for 300 frames and then
        // flickers is not a reason to start toggling the copy trigger
        // mid-session.
        bool mvec_auto_armed = false;
        // Written from the game's render thread WITHOUT this struct's mutex -
        // it counts the times that mutex was not taken. Atomic for that reason
        // and no other.
        std::atomic<unsigned long long> mvec_lock_skips{0};
        std::atomic<unsigned long long> mvec_size_rejects{0};
        unsigned long long mvec_arm_waits = 0;
        bool   mvec_arm_logged = false;
        // ---- R99: the arm waits for the probe to STOP CHANGING ITS MIND ----
        //
        // Dawnwalker armed at 2560x1440 on the first handle offered, then the
        // probe settled on 1485x836 and published two different sources during
        // the run. The region was sized for a resource that no longer exists,
        // the hook never fired, and the title read copies=0 with
        // size-rejected=0 - a silent 0% that no counter explained.
        //
        // Dragon Sword and Plague Tale published their real buffer first and
        // never showed this. Nothing required the answer to be STABLE.
        unsigned long long mvec_seen_handle = 0;
        unsigned long long mvec_stable = 0;
        unsigned long long mvec_unpacks = 0;
        unsigned long long mvec_bound = 0, mvec_skipped = 0;
        unsigned long long mvec_seen_valid = 0, mvec_seen_invalid = 0;
        unsigned long long mvec_contract = 0;
        unsigned long long mvec_flag_disagree = 0;
        unsigned long long mvec_report_at = 0;

        // ---- R87: FRAME-ELAPSED ALIGNMENT ----
        //
        // A motion vector describes exactly ONE GAME FRAME of motion. NR
        // reprojects its history by whatever vector it is handed. If the frame
        // NR is blending against is not one game frame old - because a frame's
        // neural work was skipped, or the consumer fell behind - history lands
        // SHORT by exactly the factor of frames that actually elapsed.
        //
        // That error is zero at rest, grows with speed, cannot be corrected by
        // any constant MVecScale, and compounds per pass because each pass
        // keeps its own history. It is the shape of the residual artefact.
        //
        // The correction is exact and free: the seal already carries
        // frame_index, so elapsed = f - nr_last_eval_fi is a subtraction, and
        // the scale is multiplied by it.
        //
        // BUILT AS AN INSTRUMENT FIRST. P4.1 counts 3 skipped frames in 597,
        // so elapsed should be 1 on about 99.5% of evaluates and this should
        // change almost nothing. The histogram is what makes that checkable:
        // if it is flat at 1 and the artefact is unchanged, PACING IS
        // ELIMINATED and the remaining answer is units or sign. If it is not
        // flat, there is a desync no counter has shown us.
        unsigned long long nr_last_eval_fi = 0;
        unsigned long long mvec_elapsed_hist[8] = {};   // [0]=1 frame .. [7]=8+
        unsigned long long mvec_elapsed_max = 0;

        // ---- R88: ONE JOB - the gxfer lifetime ----
        //
        // stream_mvec_copy records into gxfer from the GAME'S render thread,
        // mid-scene. stream_release frees gxfer from the bridge thread. That
        // is the only genuine race left once the flags above are atomic, and
        // it is a use-after-free rather than a torn counter, so it keeps a
        // lock - a SMALL one, held for a GetDesc and a CopyTextureRegion
        // record and nothing else. The consumer's long sections are on cs and
        // never touch this, so the game thread has nothing to wait behind.
        //
        // LOCK ORDER, and it is never violated: cs first, then mvec_cs. Only
        // stream_release takes both.
        std::mutex mvec_cs;
        std::atomic<bool> mvec_live{false};
        std::atomic<unsigned long long> produced_a{0};

        // The GPU 1 landing pair. tex_mvec stays the SYNTHETIC field's texture
        // and is untouched, so the mode-1 control arm is byte-identical to
        // every published run. Mode 3 lands here instead: at the game's
        // velocity size and format, and A PAIR for the same reason tex_in and
        // tex_depth are pairs - with the copy queue one frame ahead, frame
        // f+1's vectors would otherwise land in the texture frame f is being
        // evaluated from.
        ID3D12Resource *tex_mvec_r[2] = {};

        // ---- R30/R63: depth transport ----
        //
        // No ReShade types here, exactly as before: the source arrives as an
        // unsigned long long handle from dllmain, which is the boundary this
        // file has always used. A handle of 0 means ReShade had nothing bound
        // this frame, and that IS the seal's depth_valid.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_fp{};
        UINT64 depth_bytes = 0;
        UINT64 depth_off   = 0;      // offset WITHIN the slot, 512-aligned
        int    depth_mode  = 0;      // 0 off, 1 bound, 2 transported unbound
        unsigned depth_w = 0, depth_h = 0, depth_format = 0;
        unsigned long long depth_valid_frames = 0, depth_invalid_frames = 0;
        unsigned long long depth_size_rejects = 0;
        unsigned long long depth_arm_waits = 0;
        // R146. WHICH LANE IS HOLDING THE ARM *RIGHT NOW*.
        //
        // depth_arm_waits and mvec_arm_waits above are CUMULATIVE and are never
        // reset, so they answer "did this lane ever hold", not "is it holding".
        // R145 read them as if they were the second question and got the first:
        // measured on Resonance, depth clears at frame 252 and mvec runs to
        // 1305, so for the last ~1050 frames of every arm the screen would have
        // said WAITING FOR THE GAMES DEPTH BUFFER while the actual hold was the
        // velocity lane. A counter that only goes up cannot express a state
        // that goes away.
        //
        // 0 nothing holding   1 depth   2 velocity
        // Written every frame by the two hold paths and cleared when the arm
        // gets past them, so it is the live answer by construction.
        int hold_lane = 0;
        bool   depth_arm_logged = false;
        // consumer side, read from the seal rather than from our own state, so
        // a producer/consumer disagreement shows up as a mismatch instead of
        // as agreement between two copies of the same variable.
        unsigned long long depth_contract = 0;
        unsigned long long depth_seen_valid = 0, depth_seen_invalid = 0;
        unsigned long long depth_report_at = 0;

        // ---- T1b: the GPU 1 side ----
        //
        // A PAIR, for the same reason tex_in is a pair: with the copy queue one
        // frame ahead, frame f+1's depth would land in the texture frame f is
        // being evaluated from. Both are allocated unconditionally rather than
        // conditionally like tex_in, because 5.76 MB on a 16 GB card is not
        // worth a mode-dependent allocation path that can only ever be wrong.
        ID3D12Resource *tex_depth[2] = {};

        // Producer to consumer, same process, under the same mutex. The SEAL
        // remains the authority - it is what crosses the adapter - and the
        // verifier cross-checks the two, so a disagreement is counted rather
        // than silently resolved in favour of whichever was read first.
        unsigned char depth_slot_valid[RING] = {};

        unsigned long long depth_unpacks = 0;
        unsigned long long depth_bound = 0, depth_skipped = 0;
        unsigned long long depth_flag_disagree = 0;

        // R58 risk B. The shipped code sets DLSSNR.DepthInverted to 0 and this
        // title is UE5, which renders reversed-Z: R56 measured depth clustered
        // near 0.001, and 0.0 IS the far plane in reversed-Z. So 1 is almost
        // certainly right here - but NGX's convention for the word "inverted"
        // is not something this project has read, and it is one ini key to make
        // it testable instead of arguable.
        int depth_inverted = 1;
        float mvec_dx = 0.0f, mvec_dy = 0.0f;
        float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;
        bool  mvec_dirty = true;
        bool  mvec_in_read = false;
        unsigned long long mvec_fills = 0;

        // ---- P8.1: per-pass Reset ----
        //
        // Until now every pass got the same flag: Reset=1 on the first frame,
        // 0 afterwards. With Passes=2 that means TWO CASCADED TEMPORAL MODELS,
        // each with its own dlssnr_prev_output, one consuming the other's
        // output - and with no motion vectors BOTH histories are misaligned on
        // every frame the camera moves. Pass 2 is then reprojecting a history
        // of a signal that was itself built from a misaligned history. That
        // compounds rather than adds, which is a candidate mechanism for
        // Passes=2 being violently worse rather than merely stronger.
        //
        //   0  every pass keeps history (the shipped behaviour)
        //   1  passes 2..N reset EVERY frame - the cascade is broken and the
        //      later passes become stateless spatial refinement of pass 1
        //   2  EVERY pass resets every frame - no temporal history anywhere.
        //      This is the control for the whole hypothesis: if the artifact
        //      largely goes with mode 2, it IS NR's own misaligned history,
        //      and that is established without writing a flow estimator.
        int pass_reset = 0;

        // ---- P8.1: frames whose final pass did not write ----
        //
        // nr_final used to advance unconditionally. tex_out and tex_pong are
        // never cleared, so a failed EvaluateFeature left it pointing at a
        // texture nothing had written - undefined memory, presented. On a
        // fresh default heap that reads as black, which is indistinguishable
        // from a dark scene, a dead stream or a failed copy.
        unsigned long long nr_unwritten = 0;

        // ---- P7.9: the tuning parameters, from the DLSS-NR programming guide ----
        //
        // Names and types are READ FROM THE GUIDE's parameter reference, not
        // inferred: LocalToneStrength, LocalStructureStrength,
        // SkinStructureStrength and Style are floats (Set overload slot 1);
        // UseAutoMask is unsigned int 0/1 (slot 3). Choosing the wrong overload
        // writes a value the snippet never reads back, silently - which is the
        // same failure shape as the subrect keys.
        //
        // The guide describes LocalToneStrength as driving local contrast and
        // reading as ambient-occlusion-like shading. Until now this add-on set
        // exactly one quality parameter - Intensity - and left every one of
        // these at whatever the feature defaults to, while the reference
        // implementation this project measures against sets them all. That is a
        // recorded, unmatchable difference between the two arms in RESULTS.md,
        // and it is the first thing to reach for on a title whose output looks
        // wrong at strength.
        //
        // DEFAULT OFF. tuning_on is false unless the ini or the panel turns it
        // on, and while it is false NOTHING here is set - the evaluate path is
        // byte-identical to every published measurement. A knob that changes a
        // published number the moment it is compiled in is not a knob, it is a
        // silent regression.
        bool  tuning_on = false;
        float tone_strength      = 1.0f;
        float structure_strength = 1.0f;
        float skin_strength      = 1.0f;
        float style              = 0.0f;
        bool  auto_mask          = false;
        bool intensity_set[MAX_PASSES] = {};   // was it named per pass in the ini?
        // P6.3: 0 = every pass, 1..MAX_PASSES = that pass alone. Bridge thread
        // only - the hotkey is delivered to the bridge thread's message queue
        // and stream_poll reads intensity[] on that same thread, which is why
        // this needs no synchronisation of its own.
        unsigned intensity_target = 0;
        unsigned long long intensity_edits = 0;
        // P6.4: frames whose seal was checked but whose neural work was
        // deliberately skipped because a newer frame was already waiting.
        unsigned long long nr_skipped = 0;

        // GENERIC KEYS. Every other parameter is reachable without this file
        // knowing its name: Set.<key>=<value> in the ini is applied verbatim
        // before every evaluate. That is deliberate - guessing NGX key names
        // from memory is a mistake this project has already paid for, and this
        // way the names come from whoever actually knows them rather than from
        // me. A value containing '.' is sent as a float, otherwise as an
        // unsigned int, and the arm line says which so a mistyped value is
        // visible rather than silently coerced.
        static const unsigned MAX_SETS = 12;
        char set_key[MAX_SETS][96] = {};
        float set_f[MAX_SETS] = {};
        unsigned set_u[MAX_SETS] = {};
        bool set_is_float[MAX_SETS] = {};
        unsigned set_n = 0;
        bool nr_first = true;
        unsigned long long resync = 0;   // seals rejected, next gap check suppressed
        bool skip_next_gap = false;
    };

    // The rungs, largest R first. 67% is roughly DLSS Quality, 50% Performance,
    // 33% Ultra Performance - named by what they RESEMBLE, not taken from
    // NVIDIA's table: ours is a fraction of the DISPLAY extent and theirs is a
    // render-resolution recommendation, and conflating the two is how a number
    // ends up meaning something different from what its name says.
    const unsigned stream_state::AUTO_LADDER[5] = { 67u, 58u, 50u, 42u, 33u };

    stream_state &str()
    {
        static stream_state s;
        return s;
    }

    // P5.0. Forward-declared above present_frame. Copies out the pointer and
    // geometry under the stream's lock and returns immediately - the caller
    // does GPU work with it, and holding a lock the GAME'S render thread takes
    // every frame across that work is the one thing this add-on must never do.
    //
    // Safe to hand out a raw pointer here only because both the caller and the
    // only code that releases it (stream_release, from stream_poll) run on the
    // bridge thread, sequentially. If a consumer thread is ever added - see the
    // note in stream_poll - this becomes a lifetime bug and must be revisited
    // with it.

    // P5.3. `rest` is the state the returned texture sits in between frames,
    // which the caller must barrier away from and back to. It is an OUT
    // PARAMETER rather than a constant because the two things this can now
    // return rest in different states - tex_out in UNORDERED_ACCESS (NR writes
    // it), tex_in in COPY_DEST (we copy into it) - and a present path that
    // assumed one of them would silently record an invalid transition on the
    // other. The debug layer would catch it; a release build would not.
    ID3D12Resource *stream_present_source(UINT &w, UINT &h, DXGI_FORMAT &fmt,
                                          D3D12_RESOURCE_STATES &rest)
    {
        stream_state &s = str();
        std::lock_guard<std::mutex> lk(s.cs);
        if (!s.nr_ok || s.profile) return nullptr;
        w = s.width; h = s.height; fmt = s.format;

        // P5.3, the discriminator. Present=in shows the frame we HANDED to
        // DLSS-NR instead of the frame it produced - same adapter, same
        // texture format, same crop, same present path, one resource
        // different. It is the only thing that separates "the neural stage
        // produced wrong colours" from "we handed the neural stage a wrong
        // frame and it faithfully denoised it", and no amount of looking at
        // the output alone can tell those apart.
        if (s.present_mode == 1)
        {
            // R26: the half the last consumed frame was actually built from,
            // and the state THAT mode rests it in - COPY_DEST on the shipped
            // path, COMMON with the copy queue on. The comment above this
            // function is about exactly this hazard: a present path that
            // assumes one resting state and gets the other records an invalid
            // transition that only the debug layer would catch.
            if (s.tex_in_show == nullptr) return nullptr;
            rest = s.tex_in_rest;
            return s.tex_in_show;
        }
        // P6.0: the LAST pass's output, not tex_out unconditionally. With
        // Passes=1 nr_final is tex_out and this is the P5 behaviour exactly.
        if (s.nr_final == nullptr) return nullptr;
        rest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return s.nr_final;
    }

    // P7.4. THE LEFT HALF OF A SPLIT PRESENT.
    //
    // Returns tex_in - the frame as it arrived from the game, before the model
    // - but ONLY in split mode, and only when a neural output also exists to
    // put beside it. Half a comparison is worse than none: a viewer looking at
    // the input on the left and the cycling clear colour on the right would
    // read it as the model producing nothing.
    //
    // Same texture, same size, same format as the right half by construction:
    // tex_in and the pass outputs are all created from s.width/height/format in
    // stream_nr_create, so the two halves cannot disagree about geometry.
    ID3D12Resource *stream_present_split_left(D3D12_RESOURCE_STATES &rest, float &pos)
    {
        stream_state &s = str();
        std::lock_guard<std::mutex> lk(s.cs);
        if (s.present_mode != 2) return nullptr;
        if (!s.nr_ok || s.profile) return nullptr;
        if (s.tex_in_show == nullptr || s.nr_final == nullptr) return nullptr;
        rest = s.tex_in_rest;   // R26, see stream_present_source
        pos = s.split_pos;
        return s.tex_in_show;
    }

    // P5.2 / DEFECT C. Declared up beside ngx_probe; defined here, where the
    // stream state it reads already is. True only while the persistent neural
    // stage actually holds the capability block - `nr_ok` alone is not the
    // test, because the block is taken before the feature is created and must
    // be protected from that moment on.
    bool stream_nr_live()
    {
        stream_state &s = str();
        std::lock_guard<std::mutex> lk(s.cs);
        return s.nr_params != nullptr;
    }

    // Read Fault= out of mgpu.ini beside the add-on. Deliberately tiny and
    // deliberately failure-tolerant: this must never be a reason a run does not
    // happen.
    // P5.0: Frames=<n> raises or lowers the stream's self-imposed bound. The
    // default of 600 is about fifteen seconds, which was right while the only
    // output was a log line and is too short to look at anything. Clamped at
    // both ends: below 60 there is nothing to measure, and the bound exists to
    // stop a misbehaving build costing the whole session.
    // ---- P5.2: the ini reader, fixed ----
    //
    // Every reader below used strstr on the whole file. strstr does not know
    // what a comment is, so a line reading "; Set Neural to 0 for the control"
    // would have been found BEFORE the real Neural= key and silently disabled
    // the neural stage - a config file whose own documentation changes its
    // meaning. That was caught by reading a draft ini rather than by any
    // check, which is the same class of miss as section 00a.
    //
    // ini_find returns a pointer just past "<key>=" for the first occurrence
    // that starts a line (leading spaces and tabs allowed) and is not preceded
    // on that line by ';' or '#'. Returns nullptr when there is no such line.
    // The bounded, BOM-aware implementation is shared with the portable
    // regression test; this wrapper preserves the existing call sites.
    const char *ini_find(const char *buf, const char *key)
    {
        return mgpu::config::find(buf, strlen(buf), key);
    }

    // Reads the file once into `buf`. False when there is nothing to read or
    // the document is malformed. Callers then keep their documented defaults;
    // a malformed document is rejected as a whole, never partially applied.
    // The ini is read whole into a bounded stack buffer. 64 KiB accommodates
    // the shipped comment-heavy config while keeping malformed/unbounded
    // input from driving allocation or a partial parse.
    static const size_t INI_BYTES = mgpu::config::MAX_BYTES + 1;

    // DEFECT D, found on the rig 2026-09-05 and fixed here. This read 1023
    // bytes into a 1024-byte buffer AND SAID NOTHING when the file was longer.
    // The shipped mgpu.ini - whose comments I wrote - is 1419 bytes, and
    // Passes= sits at byte 1410. Frames (82), Neural (145), Profile (395),
    // Probes (625) and Present (896) all fall inside 1023 and worked; Passes
    // fell outside and read as ABSENT, so two rig launches ran Passes=1 while
    // the ini said 4 and every log line agreed with itself.
    //
    // The size was the trigger. The DEFECT is that truncation was silent: a
    // key past the cutoff is indistinguishable from a key that was never
    // written, and "absent" is a legitimate value here, so the wrong answer
    // was perfectly well-formed. Section 00 again.
    //
    // Two changes, and the second one matters more than the first: the buffer
    // is now bounded at 64 KiB, and a file that does not fit is rejected
    // rather than quietly clipped or partially applied.
    // P7.2. DEFECT G. mgpu.ini was opened as a BARE RELATIVE PATH, so it
    // resolved against the process's CURRENT WORKING DIRECTORY - which is not
    // the add-on's folder, is not something the add-on controls, and is not
    // even stable per game. It happened to be the game folder for every title
    // tested up to now, which is exactly why this survived: the bug and the
    // working case are indistinguishable until the CWD moves.
    //
    // When it does move, fopen returns null, ini_slurp returns false, and EVERY
    // reader falls back to its default while the log reports those defaults as
    // though they had been read. That is section 00's failure again in its
    // purest form - a confident, correctly-formatted, wrong answer - and it is
    // worse here than defect D was, because defect D lost one key and this
    // loses the whole file at once. Frames=600, Passes=1, Window=fit,
    // Neural=ON, Profile=off is the signature: all defaults, simultaneously.
    //
    // The add-on's own directory is the right anchor. mgpu.ini ships beside the
    // .addon64 and the .addon64's path is knowable from inside it - ask the
    // loader where this code is, take the directory, put mgpu.ini in it. The
    // CWD is kept as a SECOND attempt so an existing rig that relies on it does
    // not change behaviour, and the log says which one answered.
    const wchar_t *ini_path()
    {
        static wchar_t path[1024];
        static bool done = false;
        if (done) return path;
        done = true;
        path[0] = L'\0';

        HMODULE h = nullptr;
        // FROM_ADDRESS with our own code as the address: this is the module
        // that contains this function, whatever it was named or renamed to on
        // disk. UNCHANGED_REFCOUNT so we are not pinning ourselves loaded.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ini_path), &h) != FALSE && h != nullptr)
        {
            wchar_t mod[1024];
            const DWORD got = GetModuleFileNameW(h, mod, 1024);
            if (got > 0 && got < 1024)
            {
                size_t cut = 0;
                for (size_t i = 0; mod[i] != L'\0'; ++i)
                    if (mod[i] == L'\\' || mod[i] == L'/') cut = i + 1;
                if (cut > 0 && cut + 10 < 1024)
                {
                    for (size_t i = 0; i < cut; ++i) path[i] = mod[i];
                    const wchar_t *nm = L"mgpu.ini";
                    size_t j = cut;
                    for (size_t i = 0; nm[i] != L'\0'; ++i) path[j++] = nm[i];
                    path[j] = L'\0';
                }
            }
        }
        return path;   // empty means "could not work it out - use the CWD"
    }

    // =================== V28: WRITING mgpu.ini ===================
    //
    // Until now this add-on only ever READ its config. The panel could show a
    // setting and not change it, which is why Reflex and the DLAA lever were
    // ini-only: they cannot be applied live, so with no writer there was
    // nothing the panel could usefully do with them at all.
    //
    // The compromise this implements: THE PANEL WRITES THE FILE, THE SETTING
    // TAKES EFFECT ON THE NEXT LAUNCH. That is honest about the constraint
    // rather than pretending a driver mode or an R change is a live toggle,
    // and it means a user never has to open a text editor to try something.
    //
    // THREE RULES, EACH LEARNED THE HARD WAY IN THIS FILE.
    //
    //  1. LINE-ANCHORED, LIKE THE READER. A key is only a key if it is the
    //     first token on its line. A commented-out ";SRScale=50" must not be
    //     what we rewrite, or turning a toggle on would edit a comment and
    //     change nothing - which is the P1.6 stale-preset failure again, in a
    //     new place.
    //  2. ONE LINE CHANGES, NOTHING ELSE MOVES. The shipped mgpu.ini is mostly
    //     comments and those comments are the documentation. A writer that
    //     rewrites the file from parsed values destroys them.
    //  3. WRITE A TEMP AND RENAME. A half-written mgpu.ini is a game that
    //     launches with every key at its default and no way to tell (P7.2).
    //     MoveFileExW with REPLACE_EXISTING is atomic enough for this.
    bool ini_write_int(const char *key, int value)
    {
        if (key == nullptr || key[0] == '\0') return false;
        const wchar_t *wp = ini_path();
        if (wp == nullptr || wp[0] == L'\0')
        {
            mgpu::diag::warn("[MGPU][P7.2] cannot write mgpu.ini: the add-on's own directory "
                             "could not be resolved, so there is no file to edit. Set the key "
                             "by hand.");
            return false;
        }

        static const size_t CAP = INI_BYTES;
        char *buf = (char *)calloc(1, CAP);
        char *out = (char *)calloc(1, CAP + 256);
        if (buf == nullptr || out == nullptr) { free(buf); free(out); return false; }

        size_t len = 0;
        {
            FILE *f = _wfopen(wp, L"rb");
            if (f != nullptr)
            {
                len = fread(buf, 1, CAP - 1, f);
                fclose(f);
            }
        }
        buf[len] = '\0';

        char line_new[160];
        snprintf(line_new, sizeof line_new, "%s=%d", key, value);

        const size_t klen = strlen(key);
        size_t oi = 0;
        bool replaced = false;

        size_t i = 0;
        while (i <= len)
        {
            size_t e = i;
            while (e < len && buf[e] != '\n') ++e;
            // [i, e) is the line without its newline; e is '\n' or end.
            size_t p = i;
            while (p < e && (buf[p] == ' ' || buf[p] == '\t')) ++p;

            bool is_key = false;
            // Rule 1: a comment is never a key, whatever it contains.
            if (p < e && buf[p] != ';' && buf[p] != '#')
            {
                if ((size_t)(e - p) > klen && _strnicmp(buf + p, key, klen) == 0)
                {
                    size_t q = p + klen;
                    while (q < e && (buf[q] == ' ' || buf[q] == '\t')) ++q;
                    is_key = (q < e && buf[q] == '=');
                }
            }

            if (is_key && !replaced)
            {
                const size_t nl = strlen(line_new);
                if (oi + nl + 2 >= CAP + 256) { free(buf); free(out); return false; }
                memcpy(out + oi, line_new, nl); oi += nl;
                replaced = true;
            }
            else if (!is_key)
            {
                const size_t ll = e - i;
                if (oi + ll + 2 >= CAP + 256) { free(buf); free(out); return false; }
                memcpy(out + oi, buf + i, ll); oi += ll;
            }
            // is_key && replaced -> a duplicate of the key later in the file.
            // Dropped on purpose: two lines for one key is exactly the trap
            // where the file says one thing and the run does another.

            if (e < len) { out[oi++] = '\n'; i = e + 1; }
            else break;
        }

        if (!replaced)
        {
            if (oi > 0 && out[oi - 1] != '\n') out[oi++] = '\n';
            const size_t nl = strlen(line_new);
            memcpy(out + oi, line_new, nl); oi += nl;
            out[oi++] = '\n';
        }
        out[oi] = '\0';

        wchar_t tmp[MAX_PATH * 2];
        _snwprintf_s(tmp, MAX_PATH * 2, _TRUNCATE, L"%s.tmp", wp);

        bool ok = false;
        {
            FILE *f = _wfopen(tmp, L"wb");
            if (f != nullptr)
            {
                ok = (fwrite(out, 1, oi, f) == oi);
                fclose(f);
            }
        }
        if (ok) ok = (MoveFileExW(tmp, wp, MOVEFILE_REPLACE_EXISTING) != FALSE);
        else    (void)DeleteFileW(tmp);

        char l[420];
        snprintf(l, sizeof l,
                 "[MGPU][P7.2] mgpu.ini %s: %s (%s). THIS DOES NOT CHANGE THE RUNNING SESSION - "
                 "the key is read at arm, so it takes effect on the next launch.",
                 ok ? "written" : "WRITE FAILED", line_new,
                 replaced ? "replaced in place" : "appended, the key was absent");
        if (ok) mgpu::diag::info(l); else mgpu::diag::error(l);

        free(buf); free(out);
        return ok;
    }

    // Defined below, with the rest of the readers. Declared here because the
    // mirror sits beside the writer and both are above it.
    int ini_read_sr_int(const char *key, int dflt, int lo, int hi);

    // A tiny mirror so the panel can show the FILE's current value without
    // reading it every time it draws. Filled on first use, updated by any
    // write we make ourselves. Eight slots is more keys than the panel edits.
    struct ini_mirror_ent { const char *key; int val; bool have; };
    // V43: 8 -> 12. ArmCrashed joined the panel keys and the old size
    // was one short of comfortable; a full mirror silently stops caching.
    ini_mirror_ent g_ini_mirror[12] = {};

    int ini_mirror_get(const char *key, int def)
    {
        for (unsigned i = 0; i < 12u; ++i)
            if (g_ini_mirror[i].have && g_ini_mirror[i].key != nullptr
                && _stricmp(g_ini_mirror[i].key, key) == 0)
                return g_ini_mirror[i].val;
        const int v = ini_read_sr_int(key, def, -1000000, 1000000);
        for (unsigned i = 0; i < 12u; ++i)
            if (!g_ini_mirror[i].have)
            { g_ini_mirror[i].key = key; g_ini_mirror[i].val = v; g_ini_mirror[i].have = true; break; }
        return v;
    }

    void ini_mirror_set(const char *key, int v)
    {
        for (unsigned i = 0; i < 12u; ++i)
            if (g_ini_mirror[i].have && g_ini_mirror[i].key != nullptr
                && _stricmp(g_ini_mirror[i].key, key) == 0)
            { g_ini_mirror[i].val = v; return; }
        for (unsigned i = 0; i < 12u; ++i)
            if (!g_ini_mirror[i].have)
            { g_ini_mirror[i].key = key; g_ini_mirror[i].val = v; g_ini_mirror[i].have = true; return; }
    }

    bool ini_slurp(char *buf, size_t n)
    {
        if (buf == nullptr || n < 2)
        {
            mgpu::diag::error("[MGPU][P6.1] internal mgpu.ini buffer is too small; rejecting config");
            return false;
        }
        buf[0] = '\0';
        FILE *f = nullptr;
        bool beside = false;
        const wchar_t *wp = ini_path();
        if (wp[0] != L'\0') { f = _wfopen(wp, L"rb"); beside = (f != nullptr); }
        if (f == nullptr) f = fopen("mgpu.ini", "rb");   // legacy CWD fallback

        // Say once, out loud, WHICH file is in force - or that none is. Every
        // value on the arm line downstream of this is either the file's or a
        // default, and until now there was no way to tell those apart.
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                // R144. THIS LINE USED TO NAME THE FOUR DEFAULTS THAT DO NOT
                // MATTER AND NEITHER OF THE TWO THAT DO.
                //
                // Measured 2026-09-16 on Resonance and again on Cyberpunk
                // 2077: no mgpu.ini in the game folder, and the add-on did
                // nothing at all for a full session. Not a fault, not an
                // error, not a held arm - NOTHING, because with no file
                // ini_read_autoarm_frames returns 0 and stream_request is
                // never called, so stream_on_finish_effects returns at its
                // first line every frame and every diagnostic downstream of
                // the arm is silent by construction. The log then said "EVERY
                // KEY IS AT ITS DEFAULT: 600 frames, Passes=1, Window=fit,
                // Neural=ON, Profile=off", which reads as a working
                // configuration and cost two test sessions.
                //
                // The shipped assets/mgpu.ini sets AutoArm=1 and Depth=1. The
                // code defaults are 0 and 0. Those two keys are the entire
                // difference between a run and an inert add-on, and they are
                // the two the line omitted. Frames and Passes are not
                // interesting and are gone.
                //
                // SEVERITY, not politeness: this is the one condition where
                // the add-on is loaded, healthy, and deliberately doing
                // nothing. It says so in the imperative.
                char pl[1700];
                if (f == nullptr)
                    snprintf(pl, sizeof pl,
                             "[MGPU][P7.2] NO mgpu.ini FOUND - not beside the add-on (\"%ls\") and "
                             "not in the working directory. THE ADD-ON WILL DO NOTHING THIS RUN, "
                             "AND THAT IS NOT A FAULT YOU WILL SEE REPORTED ANYWHERE ELSE IN THIS "
                             "LOG. Two code defaults decide it and both are OFF: AutoArm=0, so "
                             "the stream is never requested and never arms - no [R63], no [R78], "
                             "no arm line, because all of them live behind the request - and "
                             "Depth=0, so the depth tap is not consulted and the idle screen's "
                             "ERROR 203/204 cannot fire either. The shipped assets/mgpu.ini sets "
                             "AutoArm=1 and Depth=1; the file is what turns this add-on on. "
                             "COPY assets/mgpu.ini FROM THE RELEASE ARCHIVE TO BESIDE THE "
                             ".addon64 AND RELAUNCH. Everything below this line describes a "
                             "process that is loaded, healthy and idle on purpose.",
                             (wp[0] != L'\0') ? wp : L"<path unknown>");
                else if (beside)
                    snprintf(pl, sizeof pl,
                             "[MGPU][P7.2] mgpu.ini read from beside the add-on: \"%ls\". This is "
                             "the file whose values appear on the arm line.", wp);
                else
                    snprintf(pl, sizeof pl,
                             "[MGPU][P7.2] mgpu.ini read from the WORKING DIRECTORY, not from "
                             "beside the add-on (nothing at \"%ls\"). It works, but the CWD is the "
                             "game's to change and a launcher that changes it silently reverts "
                             "every key to its default. Move mgpu.ini next to the .addon64.",
                             (wp[0] != L'\0') ? wp : L"<path unknown>");
                if (f == nullptr) mgpu::diag::error(pl);
                else              mgpu::diag::info(pl);
            }
        }

        if (f == nullptr) return false;
        const size_t got = fread(buf, 1, n - 1, f);
        // Is there anything left? One byte past what we took is enough to know.
        const bool truncated = (fgetc(f) != EOF);
        const bool read_error = ferror(f) != 0;
        fclose(f);
        buf[got] = '\0';
        if (truncated)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                char tl[400];
                snprintf(tl, sizeof tl,
                         "[MGPU][P6.1] mgpu.ini is larger than the %zu-byte limit (%zu bytes read, "
                         "more follow). The complete config is REJECTED; no partial keys are applied "
                         "and documented built-in defaults are used. Shorten comments or the file.",
                         mgpu::config::MAX_BYTES, got);
                mgpu::diag::error(tl);
            }
            return false;
        }
        if (read_error)
        {
            mgpu::diag::error("[MGPU][P6.1] mgpu.ini read error; complete config rejected and built-in defaults used");
            return false;
        }
        const mgpu::config::validation result = mgpu::config::validate(buf, got);
        if (result != mgpu::config::validation::ok)
        {
            const char *reason = "invalid text";
            switch (result)
            {
            case mgpu::config::validation::invalid_input: reason = "invalid input pointer"; break;
            case mgpu::config::validation::empty: reason = "empty file"; break;
            case mgpu::config::validation::utf16_bom: reason = "UTF-16 BOM (only UTF-8 is supported)"; break;
            case mgpu::config::validation::embedded_nul: reason = "embedded NUL byte"; break;
            case mgpu::config::validation::invalid_control: reason = "unsupported control byte"; break;
            case mgpu::config::validation::invalid_utf8: reason = "malformed UTF-8"; break;
            case mgpu::config::validation::too_large: reason = "size limit exceeded"; break;
            case mgpu::config::validation::ok: break;
            }
            char ml[320];
            snprintf(ml, sizeof ml,
                     "[MGPU][P6.1] mgpu.ini rejected (%s); no partial keys are applied and documented "
                     "built-in defaults are used.", reason);
            mgpu::diag::error(ml);
            return false;
        }
        // UTF-8 BOM is valid and common on Windows. Strip it before the
        // existing line reader sees the first key; UTF-16 is rejected above.
        size_t usable = got;
        mgpu::config::strip_utf8_bom(buf, usable);
        return usable != 0;
    }

    // P5.2. DEFECT C's second half. The one-shot probe chain (P1.3 transit,
    // P1.5 capture, and the P3.x ngx_probe it leads into) and the P4.1
    // persistent stream both armed from the SAME hotkey press, so every
    // stream run also ran a second, independent NGX consumer against the same
    // shared parameter block - setting DLSSNR.Color, DLSSNR.Output and the
    // subrect keys for its own textures in between the stream's frames.
    // Skipping the destroy (above) stops the block being pulled away, but two
    // writers on one block is not a thing to leave running under a
    // measurement.
    //
    // So the probes are now OPT-IN and default OFF: the hotkey arms the
    // stream alone unless mgpu.ini says Probes=1. The probes are answered
    // questions - P1.3, P1.5, P3.0-P3.2 are all closed - and re-running them
    // under a live stream can only cost.
    bool ini_read_probes()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return false;
        const char *k = ini_find(buf, "Probes");
        return (k != nullptr) && (*k == '1');
    }

    // P7.8b: EXPERIMENT, not a setting. Default 0 = the shipped behaviour.
    //
    // Cyberpunk 2077 (_SRGB backbuffer) returns a washed frame from the model
    // while the frame handed TO it is correct - established by same-frame split,
    // so transport and presentation are both exonerated and the model is doing
    // colour maths on gamma-encoded values.
    //
    // The free fix, IF it works, is to let the hardware linearise on read: an
    // SRV typed _SRGB decodes during the texture fetch at no ALU cost. That only
    // helps if NGX builds its SRV from the resource's own format rather than
    // overriding it, and NOTHING ESTABLISHES THAT. This key exists to find out
    // in one launch instead of one build.
    //
    // SrgbInput=1 creates tex_in (and its copy footprint) in the game's actual
    // format, which on such a title is _SRGB. tex_out and tex_pong stay UNORM
    // regardless - they are UAVs and D3D12 does not permit typed UAVs on _SRGB.
    //
    // Reading the result: run Present=split on an _SRGB title.
    //   both halves match      -> NGX honours the format, the decode is free,
    //                             and the remaining work is the encode on the
    //                             way out (an _SRGB RTV blit, also free).
    //   output still washed    -> NGX overrides the view format; the free path
    //                             does not exist and a real conversion pass is
    //                             required. That is worth knowing before one is
    //                             written.
    //   output changed but wrong in a NEW way -> the decode happened and the
    //                             missing encode is now visible. Also progress.
    // P7.9. Tuning=1 enables the guide's quality parameters; absent or 0 leaves
    // every one of them unset, which is what every published figure was measured
    // under. Tone/Structure/Skin/Style are floats, AutoMask is 0/1.
    float ini_read_float(const char *key, float dflt)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return dflt;
        const char *k = ini_find(buf, key);
        if (k == nullptr) return dflt;
        return (float)atof(k);
    }

    bool ini_read_flag(const char *key)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return false;
        const char *k = ini_find(buf, key);
        return (k != nullptr) && (*k == '1');
    }

    bool ini_read_srgb_input()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return false;
        const char *k = ini_find(buf, "SrgbInput");
        return (k != nullptr) && (*k == '1');
    }

    // P5.3: Present=in | nr. Default nr - the output, which is what every run
    // so far has shown.
    // P7.4: nr | in | split. 's' is unambiguous against the other two.
    int ini_read_present_mode()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "Present");
        if (k == nullptr) return 0;
        if (k[0] == 'i' && k[1] == 'n') return 1;
        if (k[0] == 's') return 2;
        return 0;
    }

    // P7.4: Preset= manual | front | back. The shape, not four numbers.
    int ini_read_preset()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "Preset");
        if (k == nullptr) return 0;
        if (k[0] == 'f') return 1;
        if (k[0] == 'b') return 2;
        return 0;
    }

    // P6.0: Passes=<n>, clamped to 1..MAX_PASSES. Out-of-range is CLAMPED AND
    // SAID, not silently accepted: a run that quietly did one pass when the ini
    // asked for eight would produce a perfectly clean summary describing the
    // wrong experiment.
    //
    // P7.7 makes the saying real. The clamp existed and this comment claimed it
    // spoke, but nothing printed the value that was asked for - so a file
    // reading Passes=6 produced a run identical in every log line to one reading
    // Passes=2, and the only way to know which you had was to open the ini. Now
    // the requested value is named, with the reason for the bound.
    unsigned ini_read_passes()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 1u;
        const char *k = ini_find(buf, "Passes");
        if (k == nullptr) return 1u;
        const long long v = atoll(k);
        if (v < 1)
        {
            if (v != 0)
            {
                char l[220];
                snprintf(l, sizeof l,
                         "[MGPU][P7.7] mgpu.ini asked for Passes=%lld - RAISED to 1. "
                         "The minimum is one pass.", v);
                mgpu::diag::warn(l);
            }
            return 1u;
        }
        if (v > (long long)stream_state::MAX_PASSES)
        {
            char l[420];
            snprintf(l, sizeof l,
                     "[MGPU][P7.7] mgpu.ini asked for Passes=%lld - CLAMPED to %u, the build "
                     "maximum. Three or more passes exceed the frame period on the hardware "
                     "this was measured on, so the second GPU clamps at its power limit and "
                     "each further pass is served on throttled clocks - it buys latency, not "
                     "picture. The bound is a power decision and is enforced here, not in the "
                     "settings file.",
                     v, stream_state::MAX_PASSES);
            mgpu::diag::warn(l);
            return stream_state::MAX_PASSES;
        }
        return (unsigned)v;
    }

    // P6.2. Reads Intensity and Intensity1..IntensityN. Returns the count of
    // per-pass overrides found, for the log.
    unsigned ini_read_intensity(float *out, bool *named, unsigned n_passes)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        float base = 0.84f;
        const char *k = ini_find(buf, "Intensity");
        if (k != nullptr) base = (float)atof(k);
        for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i) out[i] = base;

        unsigned named_n = 0;
        for (unsigned i = 0; i < n_passes && i < stream_state::MAX_PASSES; ++i)
        {
            char key[32];
            snprintf(key, sizeof key, "Intensity%u", i + 1);
            const char *p2 = ini_find(buf, key);
            if (p2 != nullptr) { out[i] = (float)atof(p2); named[i] = true; ++named_n; }
        }
        return named_n;
    }

    // P8.0. MVec= off | 0 | 1 | synthetic | 2 | estimated, and the synthetic
    // field plus the convention knobs. Read through THIS file's reader: the
    // buffer and ini_find live in this anonymous namespace, and a second
    // parser in a second file is how two files start disagreeing.
    // R30's key, in the same shape as MVec so the two compose and neither
    // needs new surface: 0 off, 1 acquired-transported-and-bound, 2 acquired
    // and transported but NOT bound.
    //
    // Mode 2 is not padding. It is the arm that separates TRANSPORT COST from
    // MODEL EFFECT: the bus carries exactly the same bytes as mode 1 while
    // DLSSNR.Depth stays unbound, so the evaluate is byte-identical to a
    // colour-only run and any frametime difference is transport and nothing
    // else. R62 made it load-bearing - it is the only thing that can measure
    // the producer-side cost on GPU 0, which no run has ever isolated.
    int ini_read_depth_mode()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "Depth");
        if (k == nullptr) return 0;
        const int v = atoi(k);
        return (v >= 0 && v <= 2) ? v : 0;
    }

    int ini_read_depth_inverted()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 1;
        const char *k = ini_find(buf, "DepthInverted");
        if (k == nullptr) return 1;          // reversed-Z is the default here
        const int v = atoi(k);
        return (v == 0) ? 0 : 1;
    }

    int ini_read_mvec_mode()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "MVec");
        if (k == nullptr) return 0;
        if (*k == 's' || *k == 'S') return 1;
        if (*k == 'e' || *k == 'E') return 2;
        if (*k == 'r' || *k == 'R') return 3;   // R78: REAL, the game's own
        if (*k == 'o' || *k == 'O') return 0;
        const int v = atoi(k);
        return (v < 0) ? 0 : ((v > 3) ? 3 : v);
    }

    // P8.1. PassReset= 0 | 1 | 2. See stream_state::pass_reset.
    int ini_read_pass_reset()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "PassReset");
        if (k == nullptr) return 0;
        const int v = atoi(k);
        return (v < 0) ? 0 : ((v > 2) ? 2 : v);
    }

    // R26. CopyQueue= 0 | 1, default 0. Read ONCE at arm and never live: the
    // resting state of tex_in differs between the two modes, so a toggle in
    // the middle of a run would leave the pair in a state the other path does
    // not expect. The control is a relaunch, which is what a control should be.
    // Returns 0, 1 or 2 - or -1 for a key that IS PRESENT and names something
    // else. That distinction is not pedantry. The first version of this
    // returned a bool and tested (*k == '1'), so CopyQueue=2 read as false and
    // ran the shipped path while the ini said otherwise and no line in the log
    // disagreed - a key that is present, well-formed, and silently not applied.
    // That is DEFECT D's shape exactly, and DEFECT D cost two rig launches.
    int ini_read_copy_queue()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "CopyQueue");
        if (k == nullptr) return 0;
        if (*k < '0' || *k > '9') return -1;
        const int v = atoi(k);
        return (v < 0 || v > 2) ? -1 : v;
    }

    // R102. Subrect= 25..100, percent of each axis, default 100 (the shipped
    // behaviour, byte-identical to every published run). Read ONCE at arm.
    //
    // Returns 100 when the key is absent, and -1 for a key that IS PRESENT and
    // names something else - the same distinction CopyQueue makes above, for
    // the same reason. A key that is present, well-formed and silently not
    // applied is DEFECT D's shape, and DEFECT D cost two rig launches.
    //
    // The floor is 25 rather than 1 because this is a scaling measurement, not
    // a quality control: a subrect small enough to fall off the model's own
    // internal tiling would measure the tiling rather than the law.
    // B1. Stride= 0 | 2..8. 0 is OFF and is the default, so a file that does
    // not mention it produces the shipped behaviour exactly. The value is the
    // CAP, not the stride: the loop picks its own between 1 and this.
    //
    // Same present-but-invalid discipline as CopyQueue and Subrect - DEFECT D
    // cost two rig launches and a silently unapplied setting is its shape.
    int ini_read_stride()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "Stride");
        if (k == nullptr) return 0;
        if (*k < '0' || *k > '9') return -1;
        const int v = atoi(k);
        if (v == 0) return 0;
        return (v < 2 || v > 8) ? -1 : v;
    }

    // Phase 1. CostCurve= 0 | 1, default 0. Absent means the run is byte-identical
    // to every published one, which is the whole point of the phase split.
    int ini_read_costcurve()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "CostCurve");
        if (k == nullptr) return 0;
        if (*k < '0' || *k > '9') return -1;
        const int v = atoi(k);
        return (v < 0 || v > 1) ? -1 : v;
    }

    int ini_read_subrect()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 100;
        const char *k = ini_find(buf, "Subrect");
        if (k == nullptr) return 100;
        if (*k < '0' || *k > '9') return -1;
        const int v = atoi(k);
        return (v < 25 || v > 100) ? -1 : v;
    }

    int ini_read_signal_at()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "SignalAt");
        if (k == nullptr) return 0;
        if (*k < '0' || *k > '9') return 0;
        const int v = atoi(k);
        return (v == 0 || v == 1) ? v : 0;
    }

    int ini_read_sr_int(const char *key, int dflt, int lo, int hi)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return dflt;
        const char *k = ini_find(buf, key);
        if (k == nullptr) return dflt;
        if ((*k < '0' || *k > '9') && *k != '-') return dflt;
        const int v = atoi(k);
        return (v < lo || v > hi) ? dflt : v;
    }

    int ini_read_sr()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "SRUpscale");
        if (k == nullptr) return 0;
        if (*k < '0' || *k > '9') return -1;
        const int v = atoi(k);
        return (v == 0 || v == 1) ? v : -1;
    }

    float ini_read_named_float(const char *key, float dflt)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return dflt;
        const char *k = ini_find(buf, key);
        return (k != nullptr) ? (float)atof(k) : dflt;
    }

    // P6.2. Set.<key>=<value>, up to MAX_SETS of them. Scans line by line
    // rather than by key name, because the whole point is not to know the names.
    unsigned ini_read_sets(char (*keys)[96], float *fv, unsigned *uv, bool *isf)
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        unsigned n = 0;
        const char *p2 = buf;
        while (*p2 != '\0' && n < stream_state::MAX_SETS)
        {
            const char *q = p2;
            while (*q == ' ' || *q == '\t') ++q;
            if (*q != ';' && *q != '#' && strncmp(q, "Set.", 4) == 0)
            {
                const char *ks = q + 4;
                const char *eq = ks;
                while (*eq != '\0' && *eq != '=' && *eq != '\r' && *eq != '\n') ++eq;
                if (*eq == '=' && eq > ks && (size_t)(eq - ks) < 95)
                {
                    memcpy(keys[n], ks, (size_t)(eq - ks));
                    keys[n][eq - ks] = '\0';
                    const char *vs = eq + 1;
                    bool dot = false;
                    for (const char *c = vs; *c != '\0' && *c != '\r' && *c != '\n'; ++c)
                        if (*c == '.') { dot = true; break; }
                    isf[n] = dot;
                    if (dot) fv[n] = (float)atof(vs);
                    else     uv[n] = (unsigned)strtoul(vs, nullptr, 10);
                    ++n;
                }
            }
            while (*p2 != '\0' && *p2 != '\n') ++p2;
            if (*p2 == '\n') ++p2;
        }
        return n;
    }

    // P7.0: Window= crop | match | fit. Default fit - the window is the product
    // now, and a 1280x720 letterbox out of a 2048x1152 frame is not it. crop
    // restores the P5 behaviour exactly.
    int ini_read_window_mode()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 2;
        const char *k = ini_find(buf, "Window");
        if (k == nullptr) return 2;
        if (k[0] == 'c') return 0;
        if (k[0] == 'm') return 1;
        return 2;
    }

    // P7.10: Monitor= auto | <index>. Which of the BRIDGE adapter's own outputs
    // the window is created on.
    //
    // The defect this exists to close: the window was created at CW_USEDEFAULT
    // and the fit code then asked MonitorFromWindow which panel it had landed
    // on. That is an inference from where Windows happened to put it, so on
    // every launch it sized itself correctly to the wrong display - GPU 1's
    // output presented on GPU 0's panel, which is the exact cross-adapter
    // present this topology exists to avoid. The adapter knows its own outputs;
    // asking it is a statement rather than an inference.
    //
    // auto (the default) = the bridge adapter's first output attached to the
    // desktop. An index selects among that adapter's outputs, for a rig with
    // more than one panel on the second card. Note it indexes THAT ADAPTER's
    // outputs, not Windows' display numbering, because the whole point is to
    // stay on the card that did the neural work.
    int ini_read_monitor_index()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return -1;
        const char *k = ini_find(buf, "Monitor");
        if (k == nullptr) return -1;
        if (k[0] == 'a' || k[0] == 'A') return -1;   // auto
        if (k[0] < '0' || k[0] > '9')    return -1;  // anything unparseable = auto
        return atoi(k);
    }

    // P7.10: AutoArm= 0 | 1 | <frames>. 0 or absent = off, which is what every
    // published measurement ran under and stays the default for measuring.
    //
    // 1 means "on, at the built-in delay". A number above 1 is that delay in
    // presented bridge frames. There IS a delay rather than arming at frame one,
    // and the reason is not politeness: the stream is armed once, against the
    // game's swapchain as it exists at that moment - source size, format and row
    // pitch are all fixed then. A game still building its swapchain during
    // startup, or a user still in the graphics menu, will rebuild it, and the
    // stream then consumes against an arrangement that no longer exists. The
    // caller pairs this count with a swapchain-quiet check for the same reason.
    unsigned ini_read_autoarm_frames()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 0;
        const char *k = ini_find(buf, "AutoArm");
        if (k == nullptr) return 0;
        if (k[0] < '0' || k[0] > '9') return 0;
        const int v = atoi(k);
        if (v <= 0) return 0;
        if (v == 1) return 600;   // ~10 s at vblank pace
        return (unsigned)v;
    }

    // P7.6: Frames=0 means NO BOUND - the stream runs until the game closes.
    //
    // Deliberately implemented as "a bound nothing will ever reach" rather than
    // as a stop-and-restart control. Stopping and re-arming would mean
    // releasing the NGX features and the ping-pong textures and rebuilding them
    // while the game is live, and that path has never executed once; it is the
    // P1.0 teardown crash in waiting. Raising the comparison bound changes no
    // lifecycle at all - the same single stream simply never satisfies the
    // condition that ends it. Same behaviour for the player, none of the
    // untested code.
    static const unsigned long long FRAMES_UNBOUNDED = ~0ull;

    unsigned long long stream_read_frames()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return 600ull;
        const char *k = ini_find(buf, "Frames");
        if (k == nullptr) return 600ull;
        const long long v = atoll(k);
        if (v == 0) return FRAMES_UNBOUNDED;
        if (v < 60) return 60ull;
        if (v > 100000) return 100000ull;
        return (unsigned long long)v;
    }

    // P5.1: Profile=1 strips display and liveness sampling for measurement runs.
    bool stream_read_profile()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return false;
        const char *k = ini_find(buf, "Profile");
        return (k != nullptr) && (*k == '1');
    }

    // Returns false when the file explicitly says Neural=0.
    bool stream_read_neural()
    {
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return true;
        const char *k = ini_find(buf, "Neural");
        return (k == nullptr) || (*k != '0');
    }

    void stream_read_fault(char *out, size_t n)
    {
        snprintf(out, n, "none");
        char buf[INI_BYTES];
        if (!ini_slurp(buf, sizeof buf)) return;
        const char *k = ini_find(buf, "Fault");
        if (k == nullptr) return;
        size_t i = 0;
        while (i + 1 < n && k[i] != '\0' && k[i] != '\r' && k[i] != '\n' && k[i] != ' ')
        { out[i] = k[i]; ++i; }
        out[i] = '\0';
        if (i == 0) snprintf(out, n, "none");
    }

    void stream_release()
    {
        stream_state &s = str();

        // ---- R26: DRAIN THE COPY QUEUE BEFORE ANYTHING IT REFERENCES IS
        // FREED ----
        //
        // In the normal case there is nothing to drain: the direct queue Waits
        // on cqf and the CPU waits on the direct fence, so every copy is long
        // finished by the time a frame is counted. But a run that ended on a
        // failed Close, and a prefetched copy for a frame the loop never
        // reached, both leave one in flight - and "never free under the GPU"
        // is not a rule with a normal case. The wait is bounded: a hang here
        // would be a worse failure than a leak.
        // ---- R28-B: RELEASE ANY SPECULATIVE GPU-SIDE WAIT FIRST ----
        //
        // In CopyQueue=2 the copy queue is armed for a frame that has not
        // arrived: cq->Wait(nfence, N). If the run ends before the game ever
        // produces N - the bound is reached, the swapchain is rebuilt, the
        // player alt-tabs - THAT WAIT NEVER SATISFIES. The copy queue stays
        // blocked forever while holding references to nxfer and tex_in[], and
        // releasing those under it is freeing under the GPU, which is the
        // oldest rule in this file.
        //
        // The fence is our object on both sides, so ID3D12Fence::Signal - the
        // fence's own CPU-side method, NOT the queue's - can advance it past
        // the pending value and let the queue drain.
        //
        // SAFE HERE AND NOWHERE ELSE. By the time this runs the producer has
        // already stopped: `finished` is set on the GAME thread the moment
        // produced reaches the bound, stream_on_finish_effects returns
        // immediately on it, and the bridge thread only reaches the summary
        // afterwards. So nothing will signal this fence from GPU 0's queue
        // again and the value written here is the last it ever holds. Writing
        // it while the producer was live would corrupt a counter whose value
        // IS the frame index.
        if (s.cq_gated && s.nfence != nullptr && s.cq_issued > 0 &&
            s.nfence->GetCompletedValue() < (UINT64)s.cq_issued)
        {
            (void)s.nfence->Signal((UINT64)s.cq_issued);
        }

        // A TRAILING SIGNAL, not a wait on the last per-frame value. A queue
        // Signal is ordered behind everything already submitted on that queue,
        // so this drains the copy queue whether or not the last per-frame
        // Signal was the one that succeeded - which the cqf_value bookkeeping
        // alone cannot promise.
        if (s.cq != nullptr && s.cqf != nullptr)
        {
            const UINT64 fin = s.cqf_value + 1;
            if (SUCCEEDED(s.cq->Signal(s.cqf, fin)) && s.cqf->GetCompletedValue() < fin)
            {
                HANDLE drain_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (drain_ev != nullptr)
                {
                    if (SUCCEEDED(s.cqf->SetEventOnCompletion(fin, drain_ev)))
                        WaitForSingleObject(drain_ev, 5000);
                    CloseHandle(drain_ev);
                }
            }
        }

        // The NGX features first: they hold references to the textures.
        // Released in reverse creation order, every one of them - a partial
        // release on a failed create is how a handle leaks past the summary.
        if (s.nr_release != nullptr)
            for (int i = (int)stream_state::MAX_PASSES - 1; i >= 0; --i)
                if (s.nr_handle[i] != nullptr)
                { (void)s.nr_release(s.nr_handle[i]); s.nr_handle[i] = nullptr; }
        for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i) s.nr_handle[i] = nullptr;

        // C2-SR teardown, in the same order and for the same reason as R26 and
        // R28-B: the feature goes before the resources it was created against,
        // and nothing is freed while the GPU may still be reading it.
        if (s.sr_handle != nullptr && s.sr_release != nullptr)
        { (void)s.sr_release(s.sr_handle); }
        s.sr_handle = nullptr;
        s.sr_ready = false;
        if (s.ds_pso  != nullptr) { s.ds_pso->Release();  s.ds_pso  = nullptr; }
        if (s.ds_rs   != nullptr) { s.ds_rs->Release();   s.ds_rs   = nullptr; }
        if (s.ds_heap != nullptr) { s.ds_heap->Release(); s.ds_heap = nullptr; }
        for (unsigned i = 0; i < 2u; ++i)
            if (s.sr_color[i] != nullptr) { s.sr_color[i]->Release(); s.sr_color[i] = nullptr; }
        if (s.sr_depth  != nullptr) { s.sr_depth->Release();  s.sr_depth  = nullptr; }
        if (s.sr_nrout  != nullptr) { s.sr_nrout->Release();  s.sr_nrout  = nullptr; }
        if (s.sr_expose != nullptr) { s.sr_expose->Release(); s.sr_expose = nullptr; }
        s.nr_final = nullptr;   // borrowed; released with tex_out / tex_pong
        if (s.nr_read != nullptr) { s.nr_read->Release(); s.nr_read = nullptr; }
        // P8.0, before the textures it describes.
        if (s.mvec_up != nullptr && s.mvec_cpu != nullptr) { s.mvec_up->Unmap(0, nullptr); }
        s.mvec_cpu = nullptr;
        if (s.mvec_up  != nullptr) { s.mvec_up->Release();  s.mvec_up = nullptr; }
        if (s.tex_mvec != nullptr) { s.tex_mvec->Release(); s.tex_mvec = nullptr; }
        // R78: the mode-3 pair, released the same way tex_depth is.
        for (unsigned mi = 0; mi < 2; ++mi)
            if (s.tex_mvec_r[mi] != nullptr)
            { s.tex_mvec_r[mi]->Release(); s.tex_mvec_r[mi] = nullptr; }
        for (unsigned di = 0; di < 2; ++di)
            if (s.tex_depth[di] != nullptr)
            { s.tex_depth[di]->Release(); s.tex_depth[di] = nullptr; }
        s.mvec_dirty = true;
        s.mvec_in_read = false;
        if (s.tex_pong != nullptr) { s.tex_pong->Release(); s.tex_pong = nullptr; }
        if (s.tex_out != nullptr) { s.tex_out->Release(); s.tex_out = nullptr; }
        s.tex_in_show = nullptr;   // borrowed; released with the pair below
        for (int ti = 1; ti >= 0; --ti)
            if (s.tex_in[ti] != nullptr) { s.tex_in[ti]->Release(); s.tex_in[ti] = nullptr; }
        // nr_params is NOT destroyed: it is the core's capability block and the
        // NGX session is deliberately kept open for the process lifetime.
        if (s.gup != nullptr && s.gup_cpu != nullptr) { s.gup->Unmap(0, nullptr); }
        s.gup_cpu = nullptr;
        // R26, in reverse creation order and BEFORE the DIRECT set it was
        // created after. Drained above.
        if (s.cq_tsread != nullptr) { s.cq_tsread->Release(); s.cq_tsread = nullptr; }
        if (s.cq_tsheap != nullptr) { s.cq_tsheap->Release(); s.cq_tsheap = nullptr; }
        if (s.cqf != nullptr) { s.cqf->Release(); s.cqf = nullptr; }
        for (int ci = 1; ci >= 0; --ci)
        {
            if (s.cql[ci] != nullptr) { s.cql[ci]->Release(); s.cql[ci] = nullptr; }
            if (s.cqa[ci] != nullptr) { s.cqa[ci]->Release(); s.cqa[ci] = nullptr; }
        }
        if (s.cq  != nullptr) { s.cq->Release();  s.cq = nullptr; }
        s.cqf_value = 0;
        s.cq_gated = false;
        s.cqa_value[0] = 0;
        s.cqa_value[1] = 0;
        s.cq_ts_ok = false;
        if (s.nev != nullptr) { CloseHandle(s.nev); s.nev = nullptr; }
        if (s.nf  != nullptr) { s.nf->Release();  s.nf = nullptr; }
        if (s.nl  != nullptr) { s.nl->Release();  s.nl = nullptr; }
        if (s.na  != nullptr) { s.na->Release();  s.na = nullptr; }
        if (s.nq  != nullptr) { s.nq->Release();  s.nq = nullptr; }
        if (s.gate_ev != nullptr) { CloseHandle(s.gate_ev); s.gate_ev = nullptr; }
        if (s.tsread != nullptr) { s.tsread->Release(); s.tsread = nullptr; }
        if (s.tsheap != nullptr) { s.tsheap->Release(); s.tsheap = nullptr; }
        if (s.nseal != nullptr) { s.nseal->Release(); s.nseal = nullptr; }
        if (s.nfence != nullptr) { s.nfence->Release(); s.nfence = nullptr; }
        // ---- HANGFIX 2026-09-12: DRAIN *GPU 0* BEFORE FREEING WHAT IT READS ----
        //
        // "Never free under the GPU" is enforced twice over for GPU 1 in this
        // function - R26 drains the copy queue, R28-B releases the speculative
        // wait - and was never enforced ONCE for GPU 0. gxfer is a placed
        // buffer on the GAME's device that the GAME's command lists copy into,
        // and it was released with no wait at all.
        //
        // nfence is the produced-count fence, the same object the game signals
        // on its own queue, opened here. Waiting for it to reach `produced`
        // waits for the game's queue to execute past every list that can
        // reference gxfer - given the mvec recorder is stopped at the bound,
        // which is the other half of this fix.
        //
        // BOUNDED, and deliberately: a hang here would be a worse failure than
        // a leak, which is the same reasoning R26 states a few lines up. Two
        // seconds is longer than any frame this project has measured and
        // shorter than the TDR it is trying to avoid.
        if (s.nfence != nullptr && s.produced != 0
            && s.nfence->GetCompletedValue() < (UINT64)s.produced)
        {
            HANDLE dev0 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (dev0 != nullptr)
            {
                if (SUCCEEDED(s.nfence->SetEventOnCompletion((UINT64)s.produced, dev0)))
                {
                    if (WaitForSingleObject(dev0, 2000) != WAIT_OBJECT_0)
                        mgpu::diag::warn(
                            "[MGPU][P4.0] teardown: the GAME's queue did not reach the produced "
                            "count within 2 s. Releasing anyway - a leak is recoverable and a "
                            "hang here is not - but if this line appears, something on GPU 0 is "
                            "stuck and the device removal that may follow is NOT this add-on "
                            "freeing under it.");
                }
                CloseHandle(dev0);
            }
        }

        if (s.gfence_share != nullptr) { CloseHandle(s.gfence_share); s.gfence_share = nullptr; }
        if (s.gfence != nullptr) { s.gfence->Release(); s.gfence = nullptr; }
        if (s.nxfer != nullptr) { s.nxfer->Release(); s.nxfer = nullptr; }
        if (s.nheap != nullptr) { s.nheap->Release(); s.nheap = nullptr; }
        if (s.gup   != nullptr) { s.gup->Release();   s.gup = nullptr; }
        // ---- R88: STOP THE GAME THREAD FIRST, THEN FREE ----
        // stream_mvec_copy records into gxfer from the game's render thread.
        // Clearing the flag under mvec_cs means any copy already inside the
        // lock finishes, and none starts afterwards.
        {
            std::lock_guard<std::mutex> mlk(s.mvec_cs);
            s.mvec_live.store(false, std::memory_order_release);
        }
        if (s.gxfer != nullptr) { s.gxfer->Release(); s.gxfer = nullptr; }
        if (s.gheap != nullptr) { s.gheap->Release(); s.gheap = nullptr; }
        if (s.gshare!= nullptr) { CloseHandle(s.gshare); s.gshare = nullptr; }
        s.gdev = nullptr;
        s.ndev_b = nullptr;   // borrowed
        s.armed = false;
    }
}

namespace
{
    // P4.1. Bring up a PERSISTENT DLSS-NR stage on GPU 1, sized and formatted
    // to the stream. Called once, lazily, on the bridge thread, after the first
    // seal has told us the geometry is real.
    //
    // It re-resolves and re-Inits rather than borrowing anything from
    // ngx_probe. That is legitimate and was proved by P3.0: the NGX session is
    // never shut down, and a second full Init -> GetCapabilityParameters ->
    // snippet Init_Ext -> PopulateParameters_Impl sequence returns Success and
    // yields a NEW parameter block and a NEW feature handle. Independence is
    // worth more here than sharing: this stage outlives every probe, and a
    // probe's teardown must not be able to take it down.
    // ---- C2-SR-c: PREFER THE DRIVER'S SNIPPET OVER THE GAME'S ----
    //
    // NGX searches the APPLICATION DIRECTORY first for feature snippets, so a
    // game that ships its own nvngx_dlss.dll wins over the driver's. Cyberpunk
    // ships 310.1.0, which does not carry preset K: we asked for it, the
    // snippet mapped it to internal id 244 and logged "App hint Preset X244 is
    // not available". Our hint reached the feature - five keys we set said
    // "not available" while UltraQuality, the one key we do NOT set, still said
    // "Unspecified". The mechanism works; the DLL is just old.
    //
    // That is also what NVIDIA's own DLSS override does: make the DRIVER'S copy
    // win. The driver already ships a newer snippet and it is already updated
    // on every machine, so preferring it means nobody downloads an SDK, nothing
    // is replaced per game, and the game's own DLSS keeps using its own copy
    // untouched. Two snippets, two features, one process - which is the
    // arrangement NR and SR already run.
    //
    // Candidates in order. The registry key is NGX's own and is the only one
    // that is not a guess; the rest are where drivers have actually put it.
    static HMODULE sr_load_driver_snippet(char *where, size_t where_n)
    {
        where[0] = 0;

        // THE DRIVER'S SNIPPETS ARE NOT CALLED nvngx_dlss.dll. They live in the
        // NGX model store under obfuscated names, one directory per version:
        //
        //   %PROGRAMDATA%\\NVIDIA\\NGX\\models\\dlss\\versions\\<id>\\files\\*.bin
        //
        // On the 2026-09-12 rig: ids 20316673 (48,971,832 bytes, May 2025)
        // through 20318464 (74,187,376, Sep 2026). The first of those is byte
        // for byte the size of the nvngx_dlss.dll that shipped in the game
        // folder - so the app-local copy was the OLDEST of the modern line
        // while the driver had one 25 MB newer sitting unused.
        //
        // The .bin extension is cosmetic; they are PE modules and LoadLibrary
        // takes them. Highest version id wins, which is why the ids are parsed
        // as numbers rather than compared as strings - 20318464 must beat
        // 131844, and lexically it does not.
        wchar_t root[MAX_PATH] = {};
        {
            wchar_t pd[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) == 0) return nullptr;
            _snwprintf_s(root, MAX_PATH, _TRUNCATE,
                         L"%s\\NVIDIA\\NGX\\models\\dlss\\versions", pd);
        }

        wchar_t glob[MAX_PATH] = {};
        _snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\*", root);

        unsigned long long best = 0ull;
        wchar_t best_dir[MAX_PATH] = {};

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (fd.cFileName[0] < L'0' || fd.cFileName[0] > L'9') continue;
            unsigned long long v = 0ull;
            bool ok = true;
            for (const wchar_t *c = fd.cFileName; *c != 0; ++c)
            {
                if (*c < L'0' || *c > L'9') { ok = false; break; }
                v = v * 10ull + (unsigned long long)(*c - L'0');
            }
            if (ok && v > best) { best = v; wcscpy_s(best_dir, MAX_PATH, fd.cFileName); }
        } while (FindNextFileW(h, &fd) != 0);
        FindClose(h);
        if (best == 0ull) return nullptr;

        wchar_t fglob[MAX_PATH] = {};
        _snwprintf_s(fglob, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\*.bin", root, best_dir);
        h = FindFirstFileW(fglob, &fd);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            wchar_t full[MAX_PATH] = {};
            _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s\\files\\%s",
                         root, best_dir, fd.cFileName);
            HMODULE m = LoadLibraryW(full);
            if (m == nullptr) continue;
            if (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") == nullptr)
            { FreeLibrary(m); continue; }
            FindClose(h);
            WideCharToMultiByte(CP_UTF8, 0, full, -1, where, (int)where_n, nullptr, nullptr);
            return m;
        } while (FindNextFileW(h, &fd) != 0);
        FindClose(h);
        return nullptr;
    }

    // ================= C2-SR: build the reduce pass and the SR feature =========
    //
    // Everything here is off unless SRUpscale=1. A run with it off never calls
    // this and is byte-identical to runs 1 through 6.

    // The reduce shader. Colour is sampled with a linear filter, which for a
    // 1.72x reduction is a two-tap average and not a box filter - good enough
    // for an image the model is about to rewrite, and wrong for depth, which is
    // why depth is LOADED rather than sampled. Averaging depth across an edge
    // invents a surface that is not there and SR reprojects against it.
    static const char *kReduceHLSL =
        "Texture2D<float4> gColor : register(t0);\n"
        "Texture2D<float>  gDepth : register(t1);\n"
        "RWTexture2D<float4> oColor : register(u0);\n"
        "RWTexture2D<float>  oDepth : register(u1);\n"
        "SamplerState gLin : register(s0);\n"
        "cbuffer C : register(b0) { uint2 gSrc; uint2 gDst; };\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "  if (id.x >= gDst.x || id.y >= gDst.y) return;\n"
        "  float2 uv = (float2(id.xy) + 0.5f) / float2(gDst);\n"
        "  oColor[id.xy] = gColor.SampleLevel(gLin, uv, 0);\n"
        "  int2 sp = int2(uv * float2(gSrc));\n"
        "  sp = clamp(sp, int2(0,0), int2(gSrc) - int2(1,1));\n"
        "  oDepth[id.xy] = gDepth.Load(int3(sp, 0));\n"
        "}\n";

    typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR,
                                             const D3D_SHADER_MACRO *, ID3DInclude *,
                                             LPCSTR, LPCSTR, UINT, UINT,
                                             ID3DBlob **, ID3DBlob **);

    static bool sr_build_reduce(stream_state &s, ID3D12Device *ndev)
    {
        char line[700];

        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        if (dc == nullptr)
        {
            mgpu::diag::error("[MGPU][C2-SR] d3dcompiler_47.dll not loadable - the reduce "
                              "pass cannot be built and SRUpscale stays off for this run.");
            return false;
        }
        pfn_d3dcompile p_compile = (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile");
        if (p_compile == nullptr)
        {
            mgpu::diag::error("[MGPU][C2-SR] D3DCompile not exported by d3dcompiler_47.dll.");
            return false;
        }

        ID3DBlob *cs = nullptr, *err = nullptr;
        HRESULT hr = p_compile(kReduceHLSL, strlen(kReduceHLSL), "reduce", nullptr, nullptr,
                               "main", "cs_5_1", 0, 0, &cs, &err);
        if (FAILED(hr) || cs == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][C2-SR] reduce shader failed to compile: 0x%08X %s",
                     (unsigned)hr, (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            return false;
        }
        if (err != nullptr) { err->Release(); err = nullptr; }

        D3D12_DESCRIPTOR_RANGE rng[2] = {};
        rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rng[0].NumDescriptors = 2; rng[0].BaseShaderRegister = 0;
        rng[0].OffsetInDescriptorsFromTableStart = 0;
        rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rng[1].NumDescriptors = 2; rng[1].BaseShaderRegister = 0;
        rng[1].OffsetInDescriptorsFromTableStart = 2;

        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 2;
        rp[0].DescriptorTable.pDescriptorRanges = rng;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.ShaderRegister = 0;
        rp[1].Constants.Num32BitValues = 4;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2; rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;

        ID3DBlob *sig = nullptr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][C2-SR] root signature serialise failed 0x%08X %s",
                     (unsigned)hr, (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            cs->Release();
            return false;
        }
        hr = ndev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&s.ds_rs));
        sig->Release();
        if (FAILED(hr)) { cs->Release(); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = s.ds_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength  = cs->GetBufferSize();
        hr = ndev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.ds_pso));
        cs->Release();
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][C2-SR] compute PSO failed 0x%08X", (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 8;                       // 4 per parity
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = ndev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.ds_heap));
        if (FAILED(hr)) return false;
        s.ds_inc = ndev->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    // Per parity: SRV(colour full) SRV(depth full) UAV(colour small) UAV(depth small)
    static void sr_write_descriptors(stream_state &s, ID3D12Device *ndev, unsigned ii,
                                     ID3D12Resource *src_color, ID3D12Resource *src_depth)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s.ds_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(ii * 4u) * s.ds_inc;

        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = src_color->GetDesc().Format;
        ndev->CreateShaderResourceView(src_color, &sv, h);
        h.ptr += s.ds_inc;

        sv.Format = (src_depth != nullptr) ? src_depth->GetDesc().Format : DXGI_FORMAT_R32_FLOAT;
        ndev->CreateShaderResourceView(src_depth, &sv, h);
        h.ptr += s.ds_inc;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Format = s.sr_color[ii]->GetDesc().Format;
        ndev->CreateUnorderedAccessView(s.sr_color[ii], nullptr, &uv, h);
        h.ptr += s.ds_inc;

        uv.Format = s.sr_depth->GetDesc().Format;
        ndev->CreateUnorderedAccessView(s.sr_depth, nullptr, &uv, h);
    }

    // Feature 1, SuperSampling, through the CORE. Parameter names verified
    // against NVIDIA's own nvsdk_ngx_defs.h rather than transcribed.
    // V27. The SR half of stream_release(), on its own, so a preset or quality
    // change can rebuild the feature without tearing down the run.
    //
    // SAME ORDER AS stream_release AND FOR THE SAME REASON: the feature goes
    // before the resources it was created against. Everything here is
    // recreated by stream_sr_create immediately afterwards; if that fails, SR
    // is off for the rest of the run and the stream carries on without it,
    // which is the behaviour a failed create has always had.
    void stream_sr_release_only(stream_state &s)
    {
        if (s.sr_handle != nullptr && s.sr_release != nullptr)
        { (void)s.sr_release(s.sr_handle); }
        s.sr_handle = nullptr;
        s.sr_ready  = false;
        if (s.ds_pso  != nullptr) { s.ds_pso->Release();  s.ds_pso  = nullptr; }
        if (s.ds_rs   != nullptr) { s.ds_rs->Release();   s.ds_rs   = nullptr; }
        if (s.ds_heap != nullptr) { s.ds_heap->Release(); s.ds_heap = nullptr; }
        for (unsigned i = 0; i < 2u; ++i)
            if (s.sr_color[i] != nullptr) { s.sr_color[i]->Release(); s.sr_color[i] = nullptr; }
        if (s.sr_depth  != nullptr) { s.sr_depth->Release();  s.sr_depth  = nullptr; }
        if (s.sr_nrout  != nullptr) { s.sr_nrout->Release();  s.sr_nrout  = nullptr; }
        if (s.sr_expose != nullptr) { s.sr_expose->Release(); s.sr_expose = nullptr; }
    }

    bool stream_sr_create(stream_state &s, ID3D12Device *ndev)
    {
        char line[1200];
        s.sr_ready = false;

        if (s.mvec_w == 0u || s.mvec_h == 0u)
        {
            mgpu::diag::error("[MGPU][C2-SR] the game's render extent is not known yet "
                              "(mvec_w/mvec_h are 0), so R cannot be chosen. SRUpscale is "
                              "off for this run. R is deliberately tied to the render extent "
                              "so the transported motion vectors are never resampled.");
            return false;
        }
        // R: EXPLICIT IF ASKED, OTHERWISE THE GAME'S RENDER EXTENT.
        //
        // Tying R to mvec_w/mvec_h is the best default because the motion
        // vectors are then native and nothing rescales them. It stops being
        // available the moment the game itself renders at native - DLAA - since
        // there is no smaller extent to inherit. SRScale gives R directly, as a
        // percent per axis, and 67 is DLSS Quality's own ratio.
        if (s.sr_scale_pct != 0u)
        {
            s.sr_w = (UINT)(((unsigned long long)s.width  * s.sr_scale_pct) / 100ull) & ~7u;
            s.sr_h = (UINT)(((unsigned long long)s.height * s.sr_scale_pct) / 100ull) & ~7u;
        }
        else
        {
            s.sr_w = s.mvec_w;
            s.sr_h = s.mvec_h;
        }

        // ---- MVLowRes IS ALWAYS SET, AND THE SCALE CARRIES THE DIFFERENCE ----
        //
        // MVLowRes means "the vectors are at RENDER resolution". Deriving it
        // from whether the extents happened to match made SRScale unsafe: at
        // Balanced the vectors are 1485x835 and R was 1280x720, so the flag
        // came out 0 - telling DLSS the vectors covered the whole 2560x1440
        // display - and it read a 1485x835 resource as if it were more than
        // three times the size. That CRASHED at arm. At Ultra Performance the
        // vectors are 853x480, SMALLER than R, and the same mismatch merely
        // looked wrong instead of running off the end.
        //
        // The fix is not to constrain R. MV_Scale_X/Y is DLSS'S OWN conversion
        // from vector units to pixels, so correcting it by R/mvec tells the
        // feature the truth about vectors that do not match R. NO VECTOR IS
        // EVER RESAMPLED - the texture is handed over untouched and only the
        // scale factor it applies itself changes. That is the line this project
        // has always drawn and it is not being crossed.
        //
        // So the flag is now unconditional and the scale carries the ratio.
        // Which also unlocks DLAA: with R free, a game rendering at native can
        // still be given a smaller R for NR and enlarged back by SR.
        // ---- SRMvLowRes: A TOGGLE, NOT A DERIVATION ----
        //
        // 0  FORCE OFF, and leave MV_Scale exactly as the game reports it.
        //    This is V6 byte for byte - the build that looked best in motion,
        //    no shimmer, no ghosting. By the spec it is "wrong": the flag says
        //    the vectors are at display resolution when they are at render
        //    resolution. Reported as looking BETTER than the correct setting,
        //    which is a fact about the model and not about the documentation.
        // 1  FORCE ON, with MV_Scale corrected by R/mvec.
        // 2  DERIVE from whether the extents match. V7 through V19, and the
        //    configuration where the ghosting was noticed.
        //
        // The default is 0. Correct-by-spec is not the same as better-looking,
        // and the only two people who have watched this in motion both
        // preferred the flag off. It stays a toggle precisely because that
        // preference is one rig and one title.
        if (s.sr_mv_mode == 0u)
        {
            s.sr_mv_lowres = false;
            s.sr_mv_fix_x = 1.0f;
            s.sr_mv_fix_y = 1.0f;
        }
        else if (s.sr_mv_mode == 1u)
        {
            s.sr_mv_lowres = true;
            s.sr_mv_fix_x = (s.mvec_w != 0u) ? ((float)s.sr_w / (float)s.mvec_w) : 1.0f;
            s.sr_mv_fix_y = (s.mvec_h != 0u) ? ((float)s.sr_h / (float)s.mvec_h) : 1.0f;
        }
        else
        {
            s.sr_mv_lowres = (s.mvec_w == s.sr_w && s.mvec_h == s.sr_h);
            s.sr_mv_fix_x = 1.0f;
            s.sr_mv_fix_y = 1.0f;
        }

        // WHATEVER THE MODE, A MISMATCH WITH THE FLAG OFF IS THE CRASH CASE.
        // At Balanced the vectors are 1485x835; with R smaller and the flag
        // off, DLSS reads that resource as if it covered the whole display and
        // runs off the end - it crashed at arm. Mode 0 is only safe while R IS
        // the vector extent, which is SRScale=0. Say so rather than crash.
        // ---- THE GUARD KNEW TWO CASES AND THERE ARE THREE ----
        //
        // Vectors can be at R, at D, or at neither. MVLowRes=1 means "at R",
        // MVLowRes=0 means "at the display extent". BOTH are legitimate and
        // only the third case is dangerous.
        //
        // The old guard forced the flag ON whenever the vector extent was not
        // R - correct when the game is upscaling, WRONG at DLAA or native,
        // where the vectors ARE at D and the flag should stay off. Measured
        // 2026-09-12 at 1080p DLAA with SRScale=50: the guard forced
        // MVLowRes=1 with a 0.5 scale correction, telling DLSS the vectors
        // were 960x536 when the resource was 1920x1080, and the first
        // EvaluateFeature returned 0xBAD00002 FAIL_PlatformError.
        if (!s.sr_mv_lowres && s.mvec_w == s.width && s.mvec_h == s.height)
        {
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] motion vectors are at the DISPLAY extent (%ux%u), which is "
                     "what MVLowRes=0 means, so the flag stays OFF and MV_Scale is untouched. "
                     "This is the DLAA / native-render case: the game is not upscaling, so its "
                     "vectors cover the whole frame. R is ours from SRScale and the vectors are "
                     "read as covering D, which is the truth.",
                     s.mvec_w, s.mvec_h);
            mgpu::diag::info(line);
        }
        else if (!s.sr_mv_lowres && (s.mvec_w != s.sr_w || s.mvec_h != s.sr_h))
        {
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] SRMvLowRes=%u leaves the flag OFF while the vectors "
                     "(%ux%u) are not at R (%ux%u). That combination CRASHED at arm on "
                     "2026-09-12: the feature reads the vector resource as if it covered the "
                     "whole display. Forcing the flag ON with the corrected scale for this "
                     "run. Use SRScale=0 if you want the flag off - then R IS the vector "
                     "extent and there is nothing to disagree about.",
                     s.sr_mv_mode, s.mvec_w, s.mvec_h, s.sr_w, s.sr_h);
            mgpu::diag::warn(line);
            s.sr_mv_lowres = true;
            s.sr_mv_fix_x = (s.mvec_w != 0u) ? ((float)s.sr_w / (float)s.mvec_w) : 1.0f;
            s.sr_mv_fix_y = (s.mvec_h != 0u) ? ((float)s.sr_h / (float)s.mvec_h) : 1.0f;
        }
        if (s.sr_w >= s.width || s.sr_h >= s.height)
        {
            if (s.sr_scale_pct == 0u)
                mgpu::diag::warn(
                    "[MGPU][C2-SR] THIS IS THE DLAA CASE. The game renders at native, so its "
                    "render extent IS the display extent and there is nothing for SR to "
                    "enlarge. SET SRScale - 50 or 67 - and R becomes ours to choose instead of "
                    "inherited, which is what makes DLAA work: NR runs small and SR puts it "
                    "back. That path needs the corrected MV scale and it is newer than the "
                    "rest, so treat it as experimental.");
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] render extent %ux%u is not SMALLER than the display "
                     "extent %ux%u, so there is nothing to upscale and nothing to save. "
                     "SRUpscale off. This is what DLAA or a native-resolution preset looks "
                     "like from here.", s.sr_w, s.sr_h, s.width, s.height);
            mgpu::diag::warn(line);
            return false;
        }

        // ---- C2-SR-b: ROUTE THE FEATURE CALLS TO nvngx_dlss.dll ----
        //
        // The first attempt asked the CORE to create SuperSampling and got
        // 0xBAD0000B in 0 ms, with NGX's own log saying:
        //
        //   NGXInitValidateSnippets: installed NGX API is older than the one
        //                            used by client application
        //   NVSDK_NGX_CreateFeature_Validate: feature is not supported on this
        //                            device
        //
        // That is the SAME shape P1.0b already solved for NR: the core's
        // validator rejecting a feature before it opens any library. P1.0b's
        // answer was to leave the SESSION calls on the core and route the
        // FEATURE calls to the snippet. The snippet for Super Resolution is
        // nvngx_dlss.dll rather than nvngx_dlssnr.dll; everything else about
        // the arrangement is unchanged, including the caller-identity gate,
        // which this module already satisfies by deploying as
        // nvngx.dll_mgpu_bridge.addon64.
        //
        // If this returns 0xBAD00002 FAIL_PlatformError instead, the routing
        // worked and the caller check failed, which is a different problem with
        // a different fix. The two codes are worth telling apart.
        ngx_modules mods;
        mods.core = GetModuleHandleW(L"_nvngx.dll");
        if (mods.core == nullptr) mods.core = LoadLibraryW(L"_nvngx.dll");
        // C2-SR-c. SRSnippet=1 (default) prefers the driver's copy; 0 keeps the
        // game's, which is what every run before 2026-09-12 used.
        char snip_path[MAX_PATH * 2] = {};
        mods.snippet = nullptr;
        if (s.sr_snippet != 0u)
            mods.snippet = sr_load_driver_snippet(snip_path, sizeof snip_path);
        s.sr_snippet_driver = (mods.snippet != nullptr);
        if (mods.snippet != nullptr)
        {
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] using the DRIVER'S Super Resolution snippet: %s. The game "
                     "ships its own beside the executable and NGX would prefer that one - "
                     "Cyberpunk's is 310.1.0, which does not carry preset K. The driver's copy "
                     "is already on every machine and already updated, so this is the override "
                     "without anyone downloading an SDK. THE GAME'S OWN DLSS IS UNTOUCHED: it "
                     "keeps using its own copy. Check which version answered by the filename of "
                     "the nvngx_dlss_*.log it writes.", snip_path);
            mgpu::diag::info(line);
        }
        else
        {
            if (s.sr_snippet != 0u)
                mgpu::diag::warn("[MGPU][C2-SR] no driver-side nvngx_dlss.dll found (registry "
                                 "NGXCore\\FullPath, System32, DriverStore all checked). "
                                 "Falling back to the game's copy - preset availability is "
                                 "then whatever that DLL carries.");

            // ---- V46: A PRIVATE COPY BEFORE THE GAME'S ----
            //
            // The driver's copy (above) is already a separate module from the
            // game's, which is why Super Resolution has been working while
            // Neural Rendering crashed - different path, different instance,
            // our Init is its first. This branch is the hole in that: with
            // SRSnippet=0, or when no driver copy is found, we fell through to
            // GetModuleHandleW("nvngx_dlss.dll") and got THE GAME'S OWN
            // INSTANCE - already initialised on the game's adapter, which is
            // exactly the arrangement that made CreateFeature fault for NR.
            //
            // nvngx_dlss.dll differs from the NR snippet in one way that
            // matters: THE GAME GENUINELY NEEDS ITS COPY beside the exe, so we
            // cannot move it. A second copy under our subfolder is the only
            // option, and it is enough - Windows keys modules by resolved
            // path, so a duplicate file is a duplicate module with its own
            // globals.
            char sr_priv[MAX_PATH * 2] = {};
            mods.snippet = load_private_snippet(L"nvngx_dlss.dll", sr_priv, sizeof sr_priv);
            if (mods.snippet != nullptr)
            {
                snprintf(line, sizeof line,
                         "[MGPU][C2-SR] using a PRIVATE copy of the SR snippet at %s - this "
                         "module instance is ours alone, so NGX binds it to the adapter we "
                         "pass rather than to the one the game already claimed.", sr_priv);
                mgpu::diag::info(line);
            }
            else
            {
                mgpu::diag::warn(
                    "[MGPU][C2-SR] NO PRIVATE SR SNIPPET and no driver copy - falling back to "
                    "the GAME'S OWN nvngx_dlss.dll instance, which it has already initialised "
                    "on its own adapter. This is the same arrangement that made the neural "
                    "stage fault, and Super Resolution may fault the same way. Put a copy of "
                    "nvngx_dlss.dll in the mgpu folder beside the add-on, or leave SRSnippet=1 "
                    "so the driver's copy is used.");
                mods.snippet = GetModuleHandleW(L"nvngx_dlss.dll");
                if (mods.snippet == nullptr) mods.snippet = LoadLibraryW(L"nvngx_dlss.dll");
            }
        }
        if (mods.core == nullptr || mods.snippet == nullptr)
        {
            mgpu::diag::error("[MGPU][C2-SR] _nvngx.dll or nvngx_dlss.dll not reachable. "
                              "nvngx_dlss.dll is the Super Resolution snippet and it must sit "
                              "beside the executable like the NR one does.");
            return false;
        }

        char w[6][160] = {};
        ngx_pf_create_feature p_cre = (ngx_pf_create_feature)
            ngx_resolve(mods, "NVSDK_NGX_D3D12_CreateFeature",  ngx_prefer::snippet, w[0], sizeof w[0]);
        s.sr_eval = (ngx_pf_evaluate_feature)
            ngx_resolve(mods, "NVSDK_NGX_D3D12_EvaluateFeature", ngx_prefer::snippet, w[1], sizeof w[1]);
        s.sr_release = (ngx_pf_release_feature)
            ngx_resolve(mods, "NVSDK_NGX_D3D12_ReleaseFeature",  ngx_prefer::snippet, w[2], sizeof w[2]);
        ngx_pf_get_cap_params p_caps = (ngx_pf_get_cap_params)
            ngx_resolve(mods, "NVSDK_NGX_D3D12_GetCapabilityParameters", ngx_prefer::core, w[3], sizeof w[3]);
        ngx_pf_init_ext p_iext = (ngx_pf_init_ext)
            ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init_Ext", w[4], sizeof w[4]);
        ngx_pf_populate_params p_pop = (ngx_pf_populate_params)
            ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl", w[5], sizeof w[5]);

        snprintf(line, sizeof line,
                 "[MGPU][C2-SR] ngx routing: CreateFeature=%s EvaluateFeature=%s "
                 "ReleaseFeature=%s GetCapabilityParameters=%s Init_Ext=%s "
                 "PopulateParameters_Impl=%s",
                 w[0], w[1], w[2], w[3], w[4], w[5]);
        mgpu::diag::info(line);

        if (p_cre == nullptr || s.sr_eval == nullptr || s.sr_release == nullptr)
        {
            mgpu::diag::error("[MGPU][C2-SR] the feature entry points did not resolve from "
                              "nvngx_dlss.dll.");
            return false;
        }

        // DEFECT C: SR gets its OWN block. It is populated by the SUPER
        // RESOLUTION snippet, so it carries that feature's own keys and
        // callbacks rather than a hand-assembled guess - which is also what
        // makes a freshly allocated block usable at all.
        typedef NVSDK_NGX_Result (NVSDK_CONV *pfn_alloc)(NVSDK_NGX_Parameter **);
        char wa[160] = {};
        pfn_alloc p_alloc = (pfn_alloc)
            ngx_resolve(mods, "NVSDK_NGX_D3D12_AllocateParameters", ngx_prefer::core, wa, sizeof wa);
        if (p_alloc == nullptr)
        {
            mgpu::diag::error("[MGPU][C2-SR] AllocateParameters did not resolve, and sharing "
                              "NR's capability block is DEFECT C. SRUpscale off.");
            return false;
        }
        const NVSDK_NGX_Result ar = p_alloc(&s.sr_params);
        if (ar != NVSDK_NGX_Result_Success || s.sr_params == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][C2-SR] AllocateParameters: 0x%08X (%s)",
                     (unsigned)ar, ngx_result_name(ar));
            mgpu::diag::error(line);
            return false;
        }

        {
            wchar_t data_path[MAX_PATH] = L".";
            (void)GetCurrentDirectoryW(MAX_PATH, data_path);
            if (p_iext != nullptr)
                (void)p_iext(0ULL, data_path, ndev, NVSDK_NGX_Version_API, s.sr_params);
            if (p_pop != nullptr)
                (void)p_pop(s.sr_params);
        }

        // Free, and it turns "not supported" into a reason. These keys are the
        // core's own answer for why SuperSampling is or is not available on
        // THIS adapter, and FeatureInitResult carries the underlying code.
        if (p_caps != nullptr)
        {
            NVSDK_NGX_Parameter *cap = nullptr;
            if (p_caps(&cap) == NVSDK_NGX_Result_Success && cap != nullptr)
            {
                int avail = -1, needs = -1, major = -1, minor = -1;
                unsigned int fir = 0;
                (void)cap->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
                (void)cap->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs);
                (void)cap->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
                (void)cap->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
                (void)cap->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &fir);
                snprintf(line, sizeof line,
                         "[MGPU][C2-SR] core capability for SuperSampling on GPU 1: "
                         "Available=%d NeedsUpdatedDriver=%d MinDriver=%d.%d "
                         "FeatureInitResult=0x%08X (%s). Available=0 with a FeatureInitResult "
                         "is the core telling us WHY, which the bare 0xBAD0000B did not.",
                         avail, needs, major, minor, fir, ngx_result_name((NVSDK_NGX_Result)fir));
                mgpu::diag::info(line);
            }
        }

        // Textures at R, plus the 1x1 identity exposure. IsHDR is OFF and
        // AutoExposure is OFF because the image is already display-referred and
        // already normalised - AutoExposure would divide an already-normalised
        // picture toward black, and supplying nothing at all does the same.
        const DXGI_FORMAT cfmt = s.tex_in[0]->GetDesc().Format;
        HRESULT hr = S_OK;
        for (unsigned i = 0; i < 2u && SUCCEEDED(hr); ++i)
            hr = make_tex(ndev, s.sr_w, s.sr_h, cfmt,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.sr_color[i]);
        if (SUCCEEDED(hr))
            hr = make_tex(ndev, s.sr_w, s.sr_h, DXGI_FORMAT_R32_FLOAT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.sr_depth);
        if (SUCCEEDED(hr))
            hr = make_tex(ndev, s.sr_w, s.sr_h, cfmt,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.sr_nrout);
        if (SUCCEEDED(hr))
            hr = make_tex(ndev, 1, 1, DXGI_FORMAT_R32_FLOAT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.sr_expose);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][C2-SR] texture creation at %ux%u failed 0x%08X",
                     s.sr_w, s.sr_h, (unsigned)hr);
            mgpu::diag::error(line);
            return false;
        }

        if (!sr_build_reduce(s, ndev)) return false;

        int flags = 0;
        if (s.depth_inverted != 0u) flags |= (int)NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        if (s.sr_mv_lowres)         flags |= (int)NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        // MVLowRes is NOT set: the vectors are at R, which IS the colour
        // resolution here, so they are not low-res relative to the input.
        // IsHDR is NOT set: section 7's image is display-referred.
        // AutoExposure is NOT set: identity exposure is supplied instead.

        s.sr_params->Set(NVSDK_NGX_Parameter_Width,          (unsigned int)s.sr_w);
        s.sr_params->Set(NVSDK_NGX_Parameter_Height,         (unsigned int)s.sr_h);
        s.sr_params->Set(NVSDK_NGX_Parameter_OutWidth,       (unsigned int)s.width);
        s.sr_params->Set(NVSDK_NGX_Parameter_OutHeight,      (unsigned int)s.height);
        // INT, NOT UNSIGNED INT. NVIDIA's own NGX_D3D12_CREATE_DLSS_EXT sets
        // these two with the int overload, and the int and unsigned int Set
        // overloads are DIFFERENT vtable slots. Writing the wrong one stores a
        // value the feature never reads back, and it fails silently - which is
        // the same class of mistake as the subrect key spelling.
        s.sr_params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)s.sr_quality);

        // THE PRESET. Left at 0 the feature takes the TITLE default, which is
        // what the 2026-09-12 run got - the log said so in as many words:
        // "App hint Preset Unspecified or Overridden, using title default
        // Preset J". Setting it here is a HINT: the snippet logs which preset
        // it actually used, so a value this DLL does not carry shows up as a
        // fallback in nvngx_dlss_*.log rather than as an error. The number is
        // raw rather than an enum name so a preset newer than this header can
        // still be asked for. J is 10 and K is 11 in the SDK's order.
        if (s.sr_preset != 0)
        {
            s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,            (unsigned int)s.sr_preset);
            s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,         (unsigned int)s.sr_preset);
            s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,        (unsigned int)s.sr_preset);
            s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,     (unsigned int)s.sr_preset);
            s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,(unsigned int)s.sr_preset);

            // READ IT BACK. The 2026-09-12 run asked for K and the snippet
            // logged "App hint Preset Unspecified or Overridden, using title
            // default Preset J" - it looked for the hint and found nothing.
            // Two causes with opposite fixes, and this one line separates them:
            //
            //   comes back as asked  the block holds it and CreateFeature is
            //                        reading hints from a DIFFERENT block
            //   comes back 0/absent  the Set never took - key spelling or the
            //                        wrong overload, the silent-failure class
            //                        this project has hit twice before
            unsigned int rb_q = 0u, rb_b = 0u;
            (void)s.sr_params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,  &rb_q);
            (void)s.sr_params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, &rb_b);
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] preset hint READBACK: asked %d, Quality reads %u, "
                     "Balanced reads %u. If these match what was asked, our block holds it "
                     "and the feature is reading a different one. If they are 0, the Set "
                     "never took. The snippet's own log says which preset it HONOURED - "
                     "these three lines together say WHY.",
                     s.sr_preset, rb_q, rb_b);
            mgpu::diag::info(line);
        }
        s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int)flags);
        s.sr_params->Set(NVSDK_NGX_Parameter_CreationNodeMask,   1u);
        s.sr_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

        // CREATEFEATURE RECORDS INITIALISATION WORK INTO THE LIST IT IS HANDED,
        // and that work must EXECUTE before anything it touched is used or
        // released. P1.0 learned this as a teardown crash and P1.0b as
        // 0xBAD0000B from a list that had never run. Same shape here: open,
        // create, close, execute, wait. Getting this wrong does not fail at
        // CreateFeature - it fails later, as a device removal, somewhere that
        // looks unrelated.
        HRESULT sh = s.na->Reset();
        if (SUCCEEDED(sh)) sh = s.nl->Reset(s.na, nullptr);
        if (FAILED(sh))
        {
            snprintf(line, sizeof line,
                     "[MGPU][C2-SR] could not open a command list for CreateFeature: 0x%08X",
                     (unsigned)sh);
            mgpu::diag::error(line);
            return false;
        }

        mgpu::diag::info("[MGPU][C2-SR] arm step 6: entering CreateFeature(SuperSampling) - "
                         "IF THIS IS THE LAST LINE, IT DIED INSIDE NGX");
        const NVSDK_NGX_Result cr = p_cre(s.nl,
                                          (NVSDK_NGX_Feature)NVSDK_NGX_Feature_SuperSampling,
                                          s.sr_params, &s.sr_handle);

        sh = s.nl->Close();
        if (SUCCEEDED(sh))
        {
            ID3D12CommandList *const sls[1] = { s.nl };
            s.nq->ExecuteCommandLists(1, sls);
            ++s.nf_value;
            sh = s.nq->Signal(s.nf, s.nf_value);
            if (SUCCEEDED(sh))
            {
                s.nf->SetEventOnCompletion(s.nf_value, s.nev);
                if (WaitForSingleObject(s.nev, 20000) != WAIT_OBJECT_0) sh = E_FAIL;
            }
        }
        if (FAILED(sh))
        {
            mgpu::diag::error("[MGPU][C2-SR] the CreateFeature list did not execute. The "
                              "feature handle is not safe to use, so SRUpscale is off for "
                              "this run rather than left armed on unexecuted init work.");
            if (s.sr_handle != nullptr && s.sr_release != nullptr)
            { (void)s.sr_release(s.sr_handle); s.sr_handle = nullptr; }
            return false;
        }
        snprintf(line, sizeof line,
                 "[MGPU][C2-SR] CreateFeature(SuperSampling, id 1) on GPU 1: result=0x%08X (%s) "
                 "handle=0x%p | R=%ux%u -> D=%ux%u, flags=0x%X (IsHDR off, AutoExposure off, "
                 "identity exposure supplied, DepthInverted=%u, MVLowRes=%u). "
                 "R from %s, SRMvLowRes mode %u, MVScale fix %.4f/%.4f. Quality=%d, hint=%d "
                 "(0 = title default). "
                 "The snippet's own log says which preset it HONOURED - read that, not this.",
                 (unsigned)cr, ngx_result_name(cr), (void *)s.sr_handle,
                 s.sr_w, s.sr_h, s.width, s.height, (unsigned)flags, s.depth_inverted,
                 s.sr_mv_lowres ? 1u : 0u,
                 (s.sr_scale_pct != 0u) ? "SRScale" : "the game's render extent",
                 s.sr_mv_mode, s.sr_mv_fix_x, s.sr_mv_fix_y, s.sr_quality, s.sr_preset);
        if (cr != NVSDK_NGX_Result_Success || s.sr_handle == nullptr)
        {
            mgpu::diag::error(line);
            mgpu::diag::error("[MGPU][C2-SR] SR did not create on GPU 1. That closes the "
                              "cheap route: NR cannot be run small and enlarged by a real "
                              "upscaler on this adapter, and what remains is a filtered blit "
                              "with RCAS, or transferring NR's contribution as a difference. "
                              "The run continues with SRUpscale off and full-frame NR.");
            return false;
        }
        mgpu::diag::info(line);
        s.sr_ready = true;
        s.sr_first = true;
        return true;
    }

    // TAKES stream_state & AND LOCKS NOTHING, on purpose. Its only caller is
    // stream_nr_create, which writes s.* unlocked throughout - that is this
    // file's convention for the arm path, which runs on the bridge thread and
    // owns the stream while it builds it. Taking s.cs here would be a
    // self-deadlock waiting for the day someone adds a lock upstream, in the one
    // code path that runs when everything has already gone wrong.
    void arm_fault_recover(stream_state &s, unsigned long seh_code)
    {
        char l[420];
        snprintf(l, sizeof l,
                 "[MGPU][V44] CAUGHT a fault inside NGX CreateFeature (exception 0x%08lX). The game "
                 "is still running. The neural stage is off for this launch and will not be retried "
                 "- after a fault inside a vendor allocator that DLL's state is unknown and going "
                 "back into it would be guesswork with the user's session.",
                 seh_code);
        mgpu::diag::error(l);

        s.nr_tried        = true;
        s.nr_ok           = false;
        s.recovery        = true;
        s.recovery_clears = false;   // we are about to write the key ourselves

        // ---- V48: NO Shutdown1 HERE EITHER, AND THE SOFT PATH GOES WITH IT ----
        //
        // V44 called Shutdown1 here to close the session after a caught fault,
        // on the hope that doing so would make THIS launch count as the
        // clearing launch and save the user a restart. It killed the process
        // one millisecond later - the game had survived the access violation
        // and then died in the cleanup. Dawnwalker later did the same thing on
        // a completely clean teardown.
        //
        // Shutdown1 is gone from the whole file, so there is no in-process
        // cleanup left to attempt, so there is no optimistic path to take and
        // no ArmCrashed=2 to write. The sentinel is a single value again: the
        // next launch does not touch NGX, clears, and the one after arms.
        //
        // This is a smaller promise than V44 made and it is one we can keep.
        // The game stays up, which was always the part that mattered.
        mgpu::diag::warn("[MGPU][V44] ArmCrashed=1. The game is still running and the neural stage "
                         "is off for the rest of this launch. The NEXT launch will run without "
                         "touching NGX to clear the residue, and the one after that will arm. "
                         "Shutdown1 is deliberately not called here - see the V48 note in "
                         "stream_shutdown for the three times it took the process down.");
        (void)ui_ini_write("ArmCrashed", 1);
    }

    bool stream_nr_create(stream_state &s, ID3D12Device *ndev)
    {
        char line[900];

        ngx_modules mods;
        mods.core = GetModuleHandleW(L"_nvngx.dll");
        if (mods.core == nullptr) mods.core = LoadLibraryW(L"_nvngx.dll");
        // ---- V45: SAME RULE AS THE PROBE. THE PRIVATE COPY WINS. ----
        //
        // This is the site that actually crashes, so it matters more than the
        // probe. LoadLibraryW on the same path the probe used returns the same
        // module and bumps the refcount; it does not load a third copy.
        {
            char arm_where[MAX_PATH * 2] = {};
            mods.snippet = nr_load_private_snippet(arm_where, sizeof arm_where);
            if (mods.snippet != nullptr)
            {
                char sl[MAX_PATH * 2 + 160];
                snprintf(sl, sizeof sl,
                         "[MGPU][P4.1] using the PRIVATE NR snippet at %s - this module instance "
                         "is ours alone, so the Init below is the FIRST one it has seen and NGX "
                         "binds to the adapter we pass.", arm_where);
                mgpu::diag::info(sl);
            }
            else
            {
                mods.snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
                if (mods.snippet == nullptr) mods.snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
                if (mods.snippet != nullptr)
                    mgpu::diag::warn(
                        "[MGPU][P4.1] NO PRIVATE NR SNIPPET - falling back to the copy beside the "
                        "executable, which the title has almost certainly already initialised on "
                        "ITS adapter. CreateFeature below is expected to fault. Put "
                        "nvngx_dlssnr.dll in the mgpu folder beside the add-on.");
            }
        }
        if (mods.core == nullptr || mods.snippet == nullptr)
        {
            mgpu::diag::error("[MGPU][P4.1] NGX modules not reachable - the stream runs "
                              "transport-only and says so in the summary. If nvngx_dlssnr.dll was "
                              "moved into the mgpu folder, check it is named exactly that and "
                              "sits beside the .addon64.");
            return false;
        }

        char w[9][160] = {};
        ngx_pf_init           p_init  = (ngx_pf_init)          ngx_resolve(mods, "NVSDK_NGX_D3D12_Init",                    ngx_prefer::core,    w[0], sizeof w[0]);
        ngx_pf_get_cap_params p_caps  = (ngx_pf_get_cap_params)ngx_resolve(mods, "NVSDK_NGX_D3D12_GetCapabilityParameters", ngx_prefer::core,    w[1], sizeof w[1]);
        ngx_pf_init_ext       p_iext  = (ngx_pf_init_ext)      ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_Init_Ext", w[2], sizeof w[2]);
        ngx_pf_populate_params p_pop  = (ngx_pf_populate_params)ngx_resolve_strict(mods.snippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl", w[3], sizeof w[3]);
        ngx_pf_create_feature p_cre   = (ngx_pf_create_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_CreateFeature",           ngx_prefer::snippet, w[4], sizeof w[4]);
        s.nr_eval    = (ngx_pf_evaluate_feature)ngx_resolve(mods, "NVSDK_NGX_D3D12_EvaluateFeature", ngx_prefer::snippet, w[5], sizeof w[5]);
        s.nr_release = (ngx_pf_release_feature) ngx_resolve(mods, "NVSDK_NGX_D3D12_ReleaseFeature",  ngx_prefer::snippet, w[6], sizeof w[6]);
        // V36. Keep the session's own closer, and the device it belongs to.
        // See stream_ngx_shutdown() for why this is now called.
        s.ngx_shutdown = (ngx_pf_shutdown1)ngx_resolve(mods, "NVSDK_NGX_D3D12_Shutdown1",
                                                       ngx_prefer::core, w[7], sizeof w[7]);
        s.ngx_destroy_params = (ngx_pf_destroy_params)ngx_resolve(
            mods, "NVSDK_NGX_D3D12_DestroyParameters", ngx_prefer::core, w[8], sizeof w[8]);
        s.ngx_dev = ndev;
        if (p_init == nullptr || p_caps == nullptr || p_iext == nullptr || p_pop == nullptr ||
            p_cre == nullptr || s.nr_eval == nullptr)
        {
            mgpu::diag::error("[MGPU][P4.1] an NGX entry point did not resolve - transport-only");
            return false;
        }

        wchar_t data_path[MAX_PATH] = {};
        {
            wchar_t mp[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(mgpu::module_handle(), mp, MAX_PATH);
            if (n != 0 && n < MAX_PATH)
            {
                size_t cut = 0;
                for (size_t i = 0; i + 1 < (size_t)n; ++i) if (mp[i] == L'\\') cut = i + 1;
                for (size_t i = 0; i < cut; ++i) data_path[i] = mp[i];
            }
        }

        // P9.0. THE CALLBACK GOES ON THE STREAM INIT TOO.
        //
        // It was installed on the P1.0c probe init and nowhere else, so an
        // ARMED run - the only kind that matters - carried none of the
        // snippet's own account of what it received. Every argument this
        // project has had about "a key was SENT, not necessarily HONOURED"
        // was unanswerable for that reason alone, and the answer was two
        // lines away.
        //
        // With this installed, each evaluate prints the resources, the
        // subrects and the mvec scale AS THE MODEL SAW THEM:
        //
        //   DLSSNR: Color=... MVec=... Depth=... Output=... intensity=... reset=...
        //   DLSSNR:   color (0,0 WxH) mvec (0,0 WxH) scale (x,y)
        //
        // That is the vendor reporting, not us inferring. DisableOtherLoggingSinks
        // stays false so the driver's own sinks keep their copy.
        NVSDK_NGX_FeatureCommonInfo common{};
        common.LoggingInfo.LoggingCallback = ngx_log_callback;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        common.LoggingInfo.DisableOtherLoggingSinks = false;
        NVSDK_NGX_Result r = p_init(0ULL, data_path, ndev, &common, NVSDK_NGX_Version_API);
        snprintf(line, sizeof line, "[MGPU][P4.1] Init: result=0x%08X (%s)",
                 (unsigned)r, ngx_result_name(r));
        mgpu::diag::info(line);

        // ---- V31: THE Init RESULT IS NOT THE ERROR. GUARD REMOVED. ----
        //
        // V29 and V30 refused to enter CreateFeature when Init returned
        // FAIL_OutOfDate. That was WRONG and it was wrong against evidence
        // already on the record, twice stated: arming BY HAND works with the
        // same error present, repeatedly, on the same session. A correlation
        // that holds in one arm path and not the other is not a cause.
        //
        // Worse, the guard did not make anything work. It converted a crash
        // into a run with NO NEURAL STAGE AT ALL - nine thousand frames of
        // transport and nothing to show - which is not a smaller failure, it
        // is the feature switched off. Recorded here so nobody reintroduces it
        // as a safety measure.
        //
        // THE CRASH IS STILL INSIDE CreateFeature(Reserved18) AND STILL
        // UNEXPLAINED. The arm-step markers stay, because they are what
        // localised it and they will localise it again.

        r = p_caps(&s.nr_params);
        if (r != NVSDK_NGX_Result_Success || s.nr_params == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][P4.1] GetCapabilityParameters failed 0x%08X (%s)",
                     (unsigned)r, ngx_result_name(r));
            mgpu::diag::error(line);
            return false;
        }
        (void)p_iext(0ULL, data_path, ndev, NVSDK_NGX_Version_API, s.nr_params);
        (void)p_pop(s.nr_params);
        s.nr_params->Set("DLSSNR.Width",  (unsigned int)s.width);
        s.nr_params->Set("DLSSNR.Height", (unsigned int)s.height);

        // NATIVE FORMAT, no conversion. P3.1 established DLSS-NR consumes the
        // game's R10G10B10A2 buffer as rendered, so the stream hands it over
        // untouched and no per-frame conversion pass exists in this pipeline.
        //
        // P7.8: native, but never _SRGB - see nr_linear_format. The bytes are
        // still the game's, unconverted; only the way the hardware is told to
        // interpret them changes, and only for formats that have an sRGB
        // variant at all.
        // P7.9: read once at arm; the panel drives them live afterwards.
        s.tuning_on          = ini_read_flag("Tuning");
        s.tone_strength      = ini_read_float("ToneStrength",      1.0f);
        s.structure_strength = ini_read_float("StructureStrength", 1.0f);
        s.skin_strength      = ini_read_float("SkinStrength",      1.0f);
        s.style              = ini_read_float("Style",             0.0f);
        s.auto_mask          = ini_read_flag("AutoMask");
        if (s.tuning_on)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P7.9] TUNING ON - tone=%.2f structure=%.2f skin=%.2f style=%.2f "
                     "automask=%u are set on every evaluate. Default is OFF and every published "
                     "figure was measured with these UNSET, so a run with this line present is not "
                     "comparable to one without it.",
                     s.tone_strength, s.structure_strength, s.skin_strength, s.style,
                     s.auto_mask ? 1u : 0u);
            mgpu::diag::warn(line);
        }

        const DXGI_FORMAT nrfmt = nr_linear_format(s.format);

        // P7.8b. tex_in only; the UAV textures have no choice. See
        // ini_read_srgb_input for what each outcome means.
        const bool srgb_in = ini_read_srgb_input();
        const DXGI_FORMAT infmt = srgb_in ? s.format : nrfmt;
        if (srgb_in)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P7.8b] EXPERIMENT: SrgbInput=1. tex_in is created DXGI %d (the game's "
                     "own format) instead of %d, so that if NGX builds its SRV from the resource "
                     "format the hardware linearises on read at no cost. tex_out/tex_pong stay %d - "
                     "they are UAVs and D3D12 has no typed UAV on _SRGB. Compare both halves in "
                     "Present=split: matching halves mean the free decode works and only the encode "
                     "on the way out is left; still washed means NGX overrides the view format and a "
                     "real conversion pass is required. THIS IS NOT A SHIPPING SETTING.",
                     (int)s.format, (int)nrfmt, (int)nrfmt);
            mgpu::diag::warn(line);
        }

        if (nrfmt != s.format)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P7.8] this game renders to an _SRGB backbuffer (DXGI %d). The neural "
                     "textures are created as DXGI %d - the same bytes, without the hardware sRGB "
                     "conversion on every read and write. The present chain stays at %d, matching "
                     "the game's own swapchain, so the compositor treats both identically. Without "
                     "this the output is washed by one extra linearisation, and tex_out would be a "
                     "typed UAV on an _SRGB format, which D3D12 does not support.",
                     (int)s.format, (int)nrfmt, (int)s.format);
            mgpu::diag::info(line);
        }

        // ---- R26: one input texture, or two ----
        //
        // tex_in[1] is created ONLY with the copy queue on. A CopyQueue=0 run
        // therefore allocates exactly what every published run allocated, and
        // the extra ~14.7 MB at 1440p is charged to the experiment that asked
        // for it rather than to the default.
        //
        // THE RESTING STATE FOLLOWS THE MODE, and this is not a detail. D3D12
        // decays any resource a COPY queue accessed back to COMMON when that
        // ExecuteCommandLists completes - the promotion into COPY_DEST for the
        // copy itself is implicit and does not survive it. So with the queue
        // on, COMMON is where the pair genuinely rests and COMMON is what the
        // barrier on the direct queue must name as its before-state. Creating
        // them in COPY_DEST and then transitioning FROM COPY_DEST, which is
        // what the R26 note said, would be describing a state the resource is
        // not in: the debug layer flags it and a release build accepts it in
        // silence. See the note above stream_cq_submit.
        s.tex_in_rest = s.copy_queue ? D3D12_RESOURCE_STATE_COMMON
                                     : D3D12_RESOURCE_STATE_COPY_DEST;
        HRESULT h = make_tex(ndev, s.width, s.height, infmt,
                             D3D12_RESOURCE_FLAG_NONE,
                             s.tex_in_rest, &s.tex_in[0]);
        if (SUCCEEDED(h) && s.copy_queue)
            h = make_tex(ndev, s.width, s.height, infmt,
                         D3D12_RESOURCE_FLAG_NONE,
                         s.tex_in_rest, &s.tex_in[1]);
        if (SUCCEEDED(h)) s.tex_in_show = s.tex_in[0];
        if (SUCCEEDED(h))
            h = make_tex(ndev, s.width, s.height, nrfmt,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.tex_out);
        // P6.4: always created now. The pass count is changeable at runtime, so
        // "will it be used" is no longer knowable at arm time, and allocating it
        // lazily would mean a CreateFeature-sized stall the first time someone
        // moved the slider.
        if (SUCCEEDED(h))
            h = make_tex(ndev, s.width, s.height, nrfmt,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s.tex_pong);
        if (SUCCEEDED(h)) h = make_buf(ndev, 1024, D3D12_HEAP_TYPE_READBACK, &s.nr_read);

        // P8.0. Always created, like tex_pong and for the same reason: the mode
        // is live, so "will it be used" is not knowable at arm time and a lazy
        // allocation would stall the first time it was turned on.
        if (SUCCEEDED(h))
            h = make_tex(ndev, s.width, s.height, DXGI_FORMAT_R32G32_FLOAT,
                         D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_mvec);
        if (SUCCEEDED(h))
        {
            const D3D12_RESOURCE_DESC md = s.tex_mvec->GetDesc();
            UINT   mrows = 0;
            UINT64 mrowb = 0;
            ndev->GetCopyableFootprints(&md, 0, 1, 0, &s.mvec_fp, &mrows, &mrowb, &s.mvec_bytes);
            h = make_buf(ndev, s.mvec_bytes, D3D12_HEAP_TYPE_UPLOAD, &s.mvec_up);
            if (SUCCEEDED(h))
            {
                D3D12_RANGE nore{0, 0};
                h = s.mvec_up->Map(0, &nore, reinterpret_cast<void **>(&s.mvec_cpu));
            }
        }

        // ---- T1b: the depth textures ----
        //
        // Created at the DEPTH region's own size and format, which the arm
        // already read from the resource. Not at s.width/s.height: those happen
        // to be equal today because R60 put the tap target at output
        // resolution, and writing the assumption down is how R30's 1664x936
        // became wrong.
        if (SUCCEEDED(h) && s.depth_mode != 0 && s.depth_w != 0 && s.depth_h != 0)
        {
            h = make_tex(ndev, s.depth_w, s.depth_h, (DXGI_FORMAT)s.depth_format,
                         D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_depth[0]);
            if (SUCCEEDED(h))
                h = make_tex(ndev, s.depth_w, s.depth_h, (DXGI_FORMAT)s.depth_format,
                             D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_depth[1]);
        }

        // ---- R78: the MVec pair, at the GAME'S velocity size and format ----
        //
        // Not at s.width/s.height and not at R32G32_FLOAT. Both would be
        // wrong on this title: R70 measured the velocity target at the DISPLAY
        // extent while colour renders at ~65% of it, and the engine's format
        // is its own choice. The arm read both from the resource; this uses
        // what it read.
        //
        // Allocated ONLY in mode 3, unlike the depth pair. The depth comment
        // argues 5.76 MB is not worth a conditional - this is 14.7 MB EACH at
        // a 1440p display extent, so a colour-only run does not pay for it.
        if (SUCCEEDED(h) && s.mvec_mode == 3 && s.mvec_w != 0 && s.mvec_h != 0)
        {
            h = make_tex(ndev, s.mvec_w, s.mvec_h, (DXGI_FORMAT)s.mvec_format,
                         D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_mvec_r[0]);
            if (SUCCEEDED(h))
                h = make_tex(ndev, s.mvec_w, s.mvec_h, (DXGI_FORMAT)s.mvec_format,
                             D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST, &s.tex_mvec_r[1]);
        }

        // ---- P9.0: the title profile ----
        //
        // One greppable line per armed run. Collecting this by hand across
        // titles is how the Dawnwalker/Cyberpunk format difference stayed
        // folklore instead of a table.
        //
        // The support bits are what the DRIVER CLAIMS. They are logged rather
        // than acted on: a claimed capability is not a proven one, and the
        // composite deliberately writes through an RTV rather than a typed
        // UAV so that none of this is load-bearing. It is recorded because
        // the next design decision will want it and because a bit that
        // disagrees with a readback is itself a finding.
        {
            wchar_t exew[MAX_PATH] = {};
            char    exe[128] = "unknown";
            const DWORD en = GetModuleFileNameW(nullptr, exew, MAX_PATH);
            if (en != 0 && en < MAX_PATH)
            {
                size_t cut = 0;
                for (size_t i = 0; i + 1 < (size_t)en; ++i) if (exew[i] == L'\\') cut = i + 1;
                size_t o = 0;
                for (size_t i = cut; exew[i] != L'\0' && o + 1 < sizeof exe; ++i, ++o)
                    exe[o] = (char)(exew[i] < 128 ? exew[i] : '?');
                exe[o] = '\0';
            }

            struct fs_row { DXGI_FORMAT f; const char *name; };
            const fs_row rows[3] = {
                { s.format,                          "game" },
                { DXGI_FORMAT_R16G16B16A16_FLOAT,    "rgba16f" },
                { DXGI_FORMAT_R32G32_FLOAT,          "rg32f" },
            };
            char bits[300];
            int bw = 0;
            for (unsigned i = 0; i < 3 && bw >= 0 && (size_t)bw < sizeof bits; ++i)
            {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
                fs.Format = rows[i].f;
                const HRESULT fh = ndev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                             &fs, sizeof fs);
                const bool ok  = SUCCEEDED(fh);
                const bool rt  = ok && (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
                const bool st  = ok && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
                const bool ld  = ok && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
                bw += snprintf(bits + bw, sizeof bits - (size_t)bw,
                               "%s%s(%d):rtv=%d uavstore=%d uavload=%d",
                               (i == 0) ? "" : " | ", rows[i].name, (int)rows[i].f,
                               rt ? 1 : 0, st ? 1 : 0, ld ? 1 : 0);
            }

            snprintf(line, sizeof line,
                     "[MGPU][P9.0] TITLE PROFILE exe=\"%s\" source=%ux%u dxgi=%d nr_tex=%d "
                     "in_tex=%d | %s | bound: Color=yes Output=yes MVec=%s Depth=no. "
                     "Depth has never been bound on the stream path and MVec only as a control "
                     "field - two of the four buffers Streamline registers as required. Grep "
                     "P9.0 across titles for the format table.",
                     exe, s.width, s.height, (int)s.format, (int)nrfmt, (int)infmt, bits,
                     (s.mvec_mode == 0) ? "no" : "synthetic");
            mgpu::diag::info(line);
        }
        if (FAILED(h))
        {
            snprintf(line, sizeof line, "[MGPU][P4.1] resource creation failed hr=0x%08X", (unsigned)h);
            mgpu::diag::error(line);
            return false;
        }

        // The liveness sample: a 64x4 corner of the OUTPUT, 1024 bytes, copied
        // once per frame. It is a rate check, not a quality check - see the
        // summary text for exactly what a high identical-rate does and does not
        // mean.
        s.nr_fp.Offset = 0;
        // P7.8: the footprint describes the layout of the bytes in the shared
        // heap for the copy INTO tex_in, so it must name tex_in's format - which
        // P7.8b can change. Both formats are the same typeless family and the
        // same bytes per pixel, so the copy itself is unaffected either way.
        s.nr_fp.Footprint.Format = ini_read_srgb_input() ? s.format
                                                         : nr_linear_format(s.format);
        s.nr_fp.Footprint.Width = 64;
        s.nr_fp.Footprint.Height = 4;
        s.nr_fp.Footprint.Depth = 1;
        s.nr_fp.Footprint.RowPitch = 256;

        // CreateFeature records init work into the list it is handed, and that
        // work must execute before anything it touched is released - P1.0's
        // teardown crash. One list, closed, executed, waited.
        // P6.0: ONE FEATURE PER PASS, all created into the SAME list and
        // executed once. CreateFeature records init work into the list it is
        // handed and that work must run before anything it touched is released
        // (P1.0's teardown crash), so the close/execute/wait below covers every
        // handle rather than each one separately.
        // ---- ARM STEP MARKERS, ADDED 2026-09-12 ----
        //
        // AutoArm=1 dies somewhere in this block. Arming by hand through the
        // same code does not. Three explanations were proposed and measured
        // wrong - the NGX Init warning, Reflex, and the point in the bridge
        // loop AutoArm calls stream_request() from - and each cost a build.
        //
        // The Reflex work was solved by making every call announce itself
        // BEFORE it ran, so a dead process names its own last call. Same
        // technique here. These lines are cheap, they run once per arm, and
        // they stay in: the next person to hit an arm crash should not have to
        // add them again.
        mgpu::diag::info("[MGPU][P4.1] arm step 1: resetting the arm command allocator and list");
        HRESULT ch = s.na->Reset();
        if (SUCCEEDED(ch)) ch = s.nl->Reset(s.na, nullptr);
        {
            char am[160];
            snprintf(am, sizeof am, "[MGPU][P4.1] arm step 2: list open hr=0x%08X", (unsigned)ch);
            mgpu::diag::info(am);
        }
        if (SUCCEEDED(ch))
        {
            LARGE_INTEGER fq{}; QueryPerformanceFrequency(&fq);
            // P6.4: MAX_PASSES handles, not `passes` of them. The pass count is
            // now changeable while the stream runs, and CreateFeature takes
            // 200-450 ms - doing that mid-stream would stall the consumer for
            // twenty frames and show up as a fault that was really a UI click.
            // Paying for all four up front costs about a second of arm time and
            // three extra sets of history buffers; the weight heap is shared
            // (the snippet log says "Released network resources after FINAL
            // feature release", so it is refcounted, not duplicated).
            // V43. THE SENTINEL GOES ON BEFORE THE FIRST CALL AND COMES OFF
            // AFTER THE LAST ONE, not around each iteration. The file write
            // costs a few milliseconds and doing it four times inside the
            // loop would put three of them in the hot part of the arm for no
            // extra coverage - any death in any iteration is the same event
            // as far as the next launch is concerned.
            arm_sentinel_set();
            for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i)
            {
                const LARGE_INTEGER t0 = [] { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v; }();
                {
                    char am[200];
                    snprintf(am, sizeof am,
                             "[MGPU][P4.1] arm step 3.%u: entering CreateFeature(Reserved18) "
                             "%ux%u - IF THIS IS THE LAST LINE, IT DIED INSIDE NGX",
                             i + 1, s.width, s.height);
                    mgpu::diag::info(am);
                }
                // V44. Guarded. See ngx_create_guarded for why it is its own
                // function and why we do not re-enter NGX after a catch.
                unsigned long seh_code = 0ul;
                r = ngx_create_guarded((ngx_pf_create_seh_fn)p_cre, s.nl,
                                       (NVSDK_NGX_Feature)NVSDK_NGX_Feature_Reserved18,
                                       s.nr_params, &s.nr_handle[i], &seh_code);
                if ((unsigned)r == NGX_RESULT_MGPU_SEH_FAULT)
                {
                    // DO NOT close or execute the init list. NGX faulted with
                    // that list open and whatever it recorded into it is not
                    // something to hand to a queue. Bail out of the arm here;
                    // the stream continues transport-only and the window says
                    // what happened.
                    arm_fault_recover(s, seh_code);
                    return false;
                }
                LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
                snprintf(line, sizeof line,
                         "[MGPU][P4.1] CreateFeature(Reserved18) handle %u/%u %ux%u fmt=%d: "
                         "result=0x%08X (%s) handle=0x%p elapsed=%.0fms",
                         i + 1, stream_state::MAX_PASSES, s.width, s.height, (int)s.format,
                         (unsigned)r, ngx_result_name(r), (void *)s.nr_handle[i],
                         (fq.QuadPart > 0) ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart) : 0.0);
                mgpu::diag::info(line);
                if (r != NVSDK_NGX_Result_Success || s.nr_handle[i] == nullptr) break;
            }
            // Survived NGX. Whether the RESULT was a success does not matter
            // here - a returned error code means the process is alive, which
            // is the only thing this key claims.
            arm_sentinel_clear();
            mgpu::diag::info("[MGPU][P4.1] arm step 4: closing the init list");
            ch = s.nl->Close();
        }
        if (SUCCEEDED(ch))
        {
            mgpu::diag::info("[MGPU][P4.1] arm step 5: executing the init list and waiting on "
                             "its fence");
            ID3D12CommandList *const ls[1] = { s.nl };
            s.nq->ExecuteCommandLists(1, ls);
            ++s.nf_value;
            ch = s.nq->Signal(s.nf, s.nf_value);
            if (SUCCEEDED(ch))
            {
                s.nf->SetEventOnCompletion(s.nf_value, s.nev);
                if (WaitForSingleObject(s.nev, 20000) != WAIT_OBJECT_0) ch = E_FAIL;
            }
        }
        bool all = (SUCCEEDED(ch) && r == NVSDK_NGX_Result_Success);
        for (unsigned i = 0; i < stream_state::MAX_PASSES && all; ++i)
            if (s.nr_handle[i] == nullptr) all = false;
        if (!all)
        {
            // Partial success is a FAILURE here, deliberately. A run with three
            // of four handles would evaluate three times and report passes=4,
            // which is a wrong answer in the shape of a right one.
            mgpu::diag::error("[MGPU][P4.1] the neural stage did not come up (or not every "
                              "pass did) - the stream continues TRANSPORT-ONLY and the "
                              "summary says so. Handles already created are released with "
                              "the stream.");
            return false;
        }
        // P8.1. LEFT NULL ON PURPOSE, and this is a fix rather than a
        // tidy-up. This used to point at tex_out or tex_pong the moment the
        // handles existed - before any evaluate had run and while both
        // textures still held undefined memory. stream_present_source would
        // then hand that to the backbuffer copy, which on a fresh default heap
        // reads as BLACK: indistinguishable from a dark scene, a dead stream,
        // or a failed copy.
        //
        // Null means stream_present_source returns nothing and the bridge
        // keeps showing the T5 cycling colour, which is the honest state -
        // "nothing has been produced yet" - and is already explained by the
        // P5.0 line when it does start. It is advanced below, per frame, and
        // ONLY when the final pass actually wrote.
        s.nr_final = nullptr;
        return true;
    }
}

// NOTE: at namespace scope, NOT in the anonymous namespace above. It is
// declared in the header, so it needs external linkage; defining it beside
// str() gave it internal linkage and worker.cpp failed to link against it.
// The file-static helpers it calls are reachable from here because this is
// the same translation unit.
// ---- P7.0: size the bridge window to the game's frame ----
//
// T4 fixed the window at 1280x720 and T5 created the swapchain non-resizable,
// deliberately - "no ResizeBuffers at P0". That was right while the window's job
// was to prove the loop was alive. It is wrong now that the window is the
// product: a 1280x720 letterbox out of a 2048x1152 frame throws away three
// quarters of what the neural stage produced, and it is not what anyone would
// record a video of.
//
// Called ONCE per stream, from the bridge thread, on the first frame whose
// source dimensions are known - which is why it cannot happen at T4: the game's
// backbuffer size is not known until a seal has carried it across.
//
// Window= in mgpu.ini picks the shape:
//   crop  the P5 behaviour, unchanged. 1280x720, bordered, centre crop.
//   match the window and the swapchain both become the SOURCE size, borderless.
//         One CopyTextureRegion, 1:1, whole frame, nothing resampled anywhere.
//         Clamped to the monitor if the source is larger.
//   fit   borderless at the MONITOR's size with the swapchain still at source
//         size, so DXGI scales on presentation. Fills the screen; the scaling is
//         the compositor's, not ours, and the banner says so rather than
//         claiming a 1:1 crop it no longer is.
//
// Any failure falls back to the existing chain and says which step failed. A
// window that is the wrong size is a cosmetic problem; a chain that has been
// half torn down is not.
// P7.3. DEFECT H - THE COLOURS. The bridge window showed flashing colour noise
// and never the game, from the first frame, on Tainted Grail.
//
// The bridge's swapchain is created R10G10B10A2_UNORM, hardcoded at P5.0
// because THAT game rendered R10G10B10A2 and the comment there says exactly why
// it must match: "CopyTextureRegion requires the two formats to match exactly -
// there is no conversion in a copy". That reasoning was right and the constant
// was the bug. The format was pinned to one title's backbuffer and then never
// asked again. Tainted Grail renders R8G8B8A8_UNORM - the arm line says so,
// fmt=28 - so the copy pushed 8-bit-per-channel bytes into a 10:10:10:2
// destination. Same 32 bits per pixel, completely different channel boundaries:
// every pixel is reinterpreted, and reinterpreted garbage that changes with the
// scene is precisely "colours blinking, never the game".
//
// It survived this long because every earlier test title happened to render
// R10G10B10A2. The invariant "the chain matches the source" was true by
// coincidence, and a coincidence that holds is indistinguishable from a
// constraint that is enforced - until the coincidence stops.
//
// Two things follow. The chain now takes the SOURCE's format, from the seal,
// which is the only authority on what the game actually rendered. And the
// resize is no longer only a window-shape operation: a format mismatch has to
// be corrected in Window=crop too, where no geometry changes at all - which is
// why `mode == 0` no longer returns early on its own.
bool present_resize(UINT src_w, UINT src_h, int mode, DXGI_FORMAT src_fmt)   // 0=crop 1=match 2=fit
{
    if (src_w == 0 || src_h == 0) return false;

    auto &S = st();
    HWND hwnd = nullptr;
    IDXGISwapChain3 *sc = nullptr;
    ID3D12Device *dev = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    UINT cur_w = 0, cur_h = 0;
    DXGI_FORMAT cur_fmt = DXGI_FORMAT_UNKNOWN;
    {
        std::lock_guard<std::mutex> lk(S.cs);
        if (S.sized_to_source) return false;   // one per stream
        hwnd = S.hwnd; sc = S.swapchain; dev = S.device; queue = S.queue;
        heap = S.rtv_heap; fence = S.fence; ev = S.fence_event; fv = S.fence_value;
        cur_w = S.chain_w; cur_h = S.chain_h; cur_fmt = S.chain_fmt;
        if (hwnd == nullptr || sc == nullptr || dev == nullptr) return false;
        S.sized_to_source = true;   // set before the work: one attempt, not a retry loop
    }

    char l[900];

    // The format the chain must end up in. UNKNOWN from the caller means the
    // seal did not carry one, in which case leave the chain as it is rather
    // than resize it to nothing.
    const DXGI_FORMAT want_fmt = (src_fmt != DXGI_FORMAT_UNKNOWN) ? src_fmt : cur_fmt;
    const bool fmt_wrong = (want_fmt != cur_fmt);

    // In crop there is no geometry to change, so this runs ONLY to correct the
    // format - and if the format is already right there is nothing to do at all.
    if (mode == 0 && !fmt_wrong) return false;

    if (fmt_wrong)
    {
        snprintf(l, sizeof l,
                 "[MGPU][P7.3] BACKBUFFER FORMAT MISMATCH CORRECTED: the present chain was created "
                 "DXGI format %d and this game renders %d. CopyTextureRegion does not convert, so "
                 "every pixel copied so far was reinterpreted across different channel boundaries - "
                 "that is the colour noise on the bridge window, and it is a display fault only: "
                 "the neural stage and the transport were unaffected and their figures stand. "
                 "Resizing the chain to %d now.",
                 (int)cur_fmt, (int)want_fmt, (int)want_fmt);
        mgpu::diag::warn(l);
    }

    // The monitor this window is on, so a source larger than the panel does not
    // produce a window that cannot be seen.
    // P7.1. THE ORIGIN IS PART OF THE ANSWER. P7.0 read this rectangle for its
    // SIZE and then positioned the window at (0,0), which is the origin of the
    // VIRTUAL DESKTOP, not of this monitor - so on a two-card, two-monitor rig
    // the borderless window jumped onto whichever panel Windows calls primary,
    // which on this rig is the one the GAME is on. The bridge window landing on
    // the game's monitor is not a cosmetic annoyance: it puts GPU 1's output on
    // GPU 0's panel, which is the exact cross-adapter present the topology is
    // supposed to avoid, and it hides the thing being measured behind the thing
    // being measured. Keep the whole rect and move the window to mon_x,mon_y.
    UINT mon_w = src_w, mon_h = src_h;
    int  mon_x = 0, mon_y = 0;
    bool mon_ok = false;
    {
        HMONITOR mh = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (mh != nullptr && GetMonitorInfoW(mh, &mi) != FALSE)
        {
            mon_w = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left);
            mon_h = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
            mon_x = (int)mi.rcMonitor.left;
            mon_y = (int)mi.rcMonitor.top;
            mon_ok = true;
        }
    }
    // MonitorFromWindow answers for the window's CURRENT rectangle, which is the
    // 1280x720 P5 window wherever it was created. That is the monitor the user
    // has been watching the bridge on, so it is the right target - but it is an
    // inference from the window's position, not a statement about which adapter
    // drives that panel. If they ever disagree the log line below is what says
    // so, because it now prints the origin it moved to.

    // Buffers stay at the SOURCE size in both modes - that is what keeps the
    // copy 1:1. In `fit` the window is bigger and DXGI stretches; in `match` the
    // window equals the buffers and nothing scales at all.
    UINT buf_w = src_w, buf_h = src_h;
    UINT win_w, win_h;
    if (mode == 0)
    {
        // Format-only correction. The P5 window keeps its size, its border and
        // its position; nothing here is a shape change and the crop maths
        // downstream must keep seeing the size it already has.
        win_w = cur_w; win_h = cur_h;
        buf_w = cur_w; buf_h = cur_h;
    }
    else if (mode == 2) { win_w = mon_w; win_h = mon_h; }
    else
    {
        win_w = (src_w < mon_w) ? src_w : mon_w;
        win_h = (src_h < mon_h) ? src_h : mon_h;
        buf_w = win_w; buf_h = win_h;   // match: buffers follow the window
    }

    // The window's top-left on THIS monitor. `fit` fills it, so the origin is
    // the monitor's origin; `match` may be smaller than the panel, so centre it
    // there rather than pinning it to a corner.
    int win_x = mon_x, win_y = mon_y;
    if (mode == 1)
    {
        if (win_w < mon_w) win_x = mon_x + (int)((mon_w - win_w) / 2);
        if (win_h < mon_h) win_y = mon_y + (int)((mon_h - win_h) / 2);
    }

    // P7.1. SAY THIS BEFORE IT HAPPENS. The drain below stops the consumer for
    // about a second while the producer keeps writing, so the seal ring wraps
    // and the next few frames come back DROPPED / REORDERED / STALE. Every P7.0
    // fit run shows that burst and it is not a transport fault - it is this
    // function holding the consumer still. Reading it as a bridge defect is the
    // instrument-failure pattern again, so the log names the cause in advance
    // and the recovery to gap=1 on the following frames is the proof.
    mgpu::diag::info("[MGPU][P7.1] resizing the present window - draining GPU 1. The producer is "
                     "NOT paused, so expect one burst of DROPPED / REORDERED / STALE seals across "
                     "the next few frames. That is this resize, not the transport; frames after it "
                     "return to gap=1 OK.");

    // GPU idle first. ResizeBuffers requires every backbuffer reference
    // released, and releasing a resource the GPU is still reading is the P1.0
    // teardown crash in a different costume.
    if (fence != nullptr && queue != nullptr && ev != nullptr)
    {
        ++fv;
        if (SUCCEEDED(queue->Signal(fence, fv)))
        {
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, 2000);
        }
        std::lock_guard<std::mutex> lk(S.cs);
        S.fence_value = fv;
    }

    {
        std::lock_guard<std::mutex> lk(S.cs);
        for (int i = 0; i < 2; ++i)
            if (S.backbuffer[i] != nullptr) { S.backbuffer[i]->Release(); S.backbuffer[i] = nullptr; }
    }

    // Borderless, then move. WS_POPUP with no caption and no thick frame; the
    // window still belongs to the bridge thread, which is the thread running
    // this, so SetWindowLongPtr and SetWindowPos are both legal here.
    // mode 0 touches neither the style nor the rectangle: it is here only to
    // put the backbuffers in the right format.
    if (mode != 0)
    {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style &= ~(WS_OVERLAPPEDWINDOW);
        style |= WS_POPUP;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, win_x, win_y, (int)win_w, (int)win_h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        // Client rect is now exactly win_w x win_h: WS_POPUP has no non-client area.
    }

    // want_fmt, not a constant. This is DEFECT H's actual repair.
    const HRESULT rb = sc->ResizeBuffers(2, buf_w, buf_h, want_fmt, 0);
    bool ok = SUCCEEDED(rb);

    // Re-acquire the backbuffers and rebuild the two RTVs.
    if (ok)
    {
        ID3D12Resource *b0 = nullptr, *b1 = nullptr;
        HRESULT h0 = sc->GetBuffer(0, IID_PPV_ARGS(&b0));
        HRESULT h1 = SUCCEEDED(h0) ? sc->GetBuffer(1, IID_PPV_ARGS(&b1)) : h0;
        if (SUCCEEDED(h1) && heap != nullptr)
        {
            const UINT rs = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE d0 = heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE d1 = d0; d1.ptr += rs;
            dev->CreateRenderTargetView(b0, nullptr, d0);
            dev->CreateRenderTargetView(b1, nullptr, d1);
            std::lock_guard<std::mutex> lk(S.cs);
            S.backbuffer[0] = b0;
            S.backbuffer[1] = b1;
            S.chain_w = buf_w;
            S.chain_h = buf_h;
            S.chain_fmt = want_fmt;
            if (mode != 0) S.borderless = true;
        }
        else
        {
            if (b0 != nullptr) b0->Release();
            if (b1 != nullptr) b1->Release();
            ok = false;
        }
    }

    if (!ok)
    {
        // The chain has no backbuffers now. present_frame will fail on the next
        // call and log its own one-shot line; say here WHY, because "Present
        // failed" on its own would send the next person looking in the wrong
        // place entirely.
        snprintf(l, sizeof l,
                 "[MGPU][P7.1] RESIZE FAILED - ResizeBuffers hr=0x%08X for %ux%u. The present "
                 "chain has been left without backbuffers and the window will stop presenting; "
                 "the game and the neural stage are UNAFFECTED, this is the bridge's own display "
                 "path only. Set Window=crop in mgpu.ini to get the P5 1280x720 window back.",
                 (unsigned)rb, buf_w, buf_h);
        mgpu::diag::error(l);
        return false;
    }

    snprintf(l, sizeof l,
             "[MGPU][P7.3] present chain resized: mode=%s source=%ux%u fmt=%d -> window %ux%u at "
             "(%d,%d), swapchain %ux%u fmt=%d, monitor %ux%u at (%d,%d)%s. %s",
             (mode == 0) ? "crop (format only - window untouched)"
                         : ((mode == 2) ? "fit" : "match"),
             src_w, src_h, (int)src_fmt, win_w, win_h, win_x, win_y,
             buf_w, buf_h, (int)want_fmt, mon_w, mon_h, mon_x, mon_y,
             mon_ok ? "" : " (GetMonitorInfo FAILED - origin assumed 0,0, so the window may be on "
                           "the wrong panel)",
             (mode == 2 && (buf_w != win_w || buf_h != win_h))
               ? "DXGI SCALES on presentation - the copy into the backbuffer is still 1:1 and "
                 "nothing in this add-on resamples, but what reaches the panel has been "
                 "stretched by the compositor. Say so in any capture."
               : "Window and swapchain are the same size, so nothing scales anywhere.");
    mgpu::diag::info(l);
    return true;
}

// P5.1. THE PRESENT GATE - the pipeline cleanup, and it is two fixes in one.
//
// The bridge's present loop ran at the display's refresh - 210 fps - and
// P5.0 copied a full frame into the backbuffer on every one of them, new
// or not. At 1600x900 that is ~1.2 GB/s of GPU 1 bandwidth spent
// re-showing frames already on screen, plus a DWM cross-adapter copy per
// present while GPU 1 is headless, on the SAME LINK the payload uses. The
// measurement arm was competing with itself.
//
// Now the loop presents only when a new neural frame exists - about 57 per
// second instead of 210, so three quarters of those copies and three
// quarters of that DWM traffic simply stop happening.
//
// AND IT FIXES THE PACING COUPLING FROM SECTION 05a AS A SIDE EFFECT. The
// consumer used to be polled once per present, so its cadence was the
// bridge swapchain's vsync - 3.23x the producer's rate on a 210 Hz panel
// and 1.01x on a 60 Hz one. Now the idle path blocks on the SHARED FENCE
// event for the next frame instead, with a short timeout as a backstop, so
// the consumer wakes when a frame actually lands and its rate no longer
// depends on what display GPU 1 is attached to.
//
// THE FENCE WAIT HAPPENS OUTSIDE THE LOCK. The stream mutex is taken by
// the event handler on the GAME'S render thread every frame; holding it
// across a wait is the one hazard in this add-on that can reach the
// application. The pointer and the target value are copied out under the
// lock, the lock is released, and only then does anything block.
//
// Returns true when the caller should present.
bool stream_present_gate(unsigned long timeout_ms)
{
    stream_state &s = str();
    ID3D12Fence *f = nullptr;
    UINT64 want = 0;
    HANDLE ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        // Not streaming: behave exactly as the loop always has. The
        // cycling colour is T5's liveness proof and must not stop because
        // the stream is idle.
        //
        // NOTE THE CONDITION IS ONLY `armed`. An earlier version of this
        // also short-circuited on `!nr_ok` and on `profile`, which had the
        // gate wide open in exactly the two configurations that exist to
        // minimise overhead - the transport-only control and the
        // measurement run would both have presented at 210 fps and paid
        // the full DWM cross-adapter cost this gate was written to remove.
        // Gating is about whether a new FRAME exists, not about whether we
        // intend to draw it: the cycling colour updating at the producer's
        // rate instead of the display's is still a live window.
        if (!s.armed || s.summarised)
        { ++s.gate_presents; return true; }

        if (s.consumed != s.presented)
        {
            s.presented = s.consumed;
            ++s.gate_presents;
            return true;
        }
        f = s.nfence; want = (UINT64)(s.consumed + 1); ev = s.gate_ev;
        ++s.gate_idle;
    }

    if (f != nullptr && ev != nullptr)
    {
        ++str().gate_waits;
        f->SetEventOnCompletion(want, ev);
        WaitForSingleObject(ev, timeout_ms);
    }
    return false;
}

bool probes_enabled()
{
    return ini_read_probes();
}

// P7.10. Both read the same mgpu.ini the arm line reports, through the same
// reader, so a value that appears in the log is the value that took effect.
// They are wrappers rather than the readers themselves because the reader and
// its buffer live in this file's anonymous namespace and worker.cpp cannot see
// them - and duplicating an ini parser in a second file is how two files start
// disagreeing about what the settings say.
int monitor_index()
{
    return ini_read_monitor_index();
}

// ---- DUPLICATE (CLONE) DISPLAY DETECTION ----
//
// The distinction that matters is not "how many monitors" - it is whether one
// display SOURCE is driving more than one TARGET. That is precisely what clone
// mode is, and QueryDisplayConfig reports it exactly:
//
//   extended : each active path has its own source id
//   duplicate: two or more active paths share (adapterId, sourceInfo.id) and
//              differ only in targetInfo.id
//
// COMPARING MONITOR RECTANGLES IS THE TEMPTING SHORTCUT AND IT IS WRONG. Two
// cloned monitors do report the same rect, but so can an extended pair that
// has been positioned on top of each other, and a mirrored pair at different
// resolutions does not report identical rects at all. It is kept below only as
// a fallback for when the CCD API is unavailable, and it says so when it runs.
//
// Resolved by GetProcAddress rather than linked: these live in user32 and have
// since Windows 7, but this file does not add link dependencies for a
// convenience feature.
namespace dispcfg
{
    typedef LONG (WINAPI *pfn_sizes)(UINT32, UINT32 *, UINT32 *);
    typedef LONG (WINAPI *pfn_query)(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *,
                                     UINT32 *, DISPLAYCONFIG_MODE_INFO *,
                                     DISPLAYCONFIG_TOPOLOGY_ID *);

    struct rect_bag { RECT r[8]; int n; };

    static BOOL CALLBACK mon_cb(HMONITOR h, HDC, LPRECT, LPARAM lp)
    {
        rect_bag *b = (rect_bag *)lp;
        if (b == nullptr || b->n >= 8) return TRUE;
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(h, &mi)) return TRUE;
        b->r[b->n] = mi.rcMonitor;
        ++b->n;
        return TRUE;
    }

    // Fallback only. Same rect for two monitors is the clone signature here,
    // and it is a signature rather than a proof - see the note above.
    bool duplicated_by_rect()
    {
        rect_bag b{};
        EnumDisplayMonitors(nullptr, nullptr, mon_cb, (LPARAM)&b);
        for (int i = 0; i < b.n; ++i)
            for (int j = i + 1; j < b.n; ++j)
                if (b.r[i].left  == b.r[j].left  && b.r[i].top    == b.r[j].top &&
                    b.r[i].right == b.r[j].right && b.r[i].bottom == b.r[j].bottom)
                    return true;
        return false;
    }

    bool duplicated()
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        pfn_sizes p_sizes = (u != nullptr)
            ? (pfn_sizes)(void *)GetProcAddress(u, "GetDisplayConfigBufferSizes") : nullptr;
        pfn_query p_query = (u != nullptr)
            ? (pfn_query)(void *)GetProcAddress(u, "QueryDisplayConfig") : nullptr;

        if (p_sizes == nullptr || p_query == nullptr)
        {
            mgpu::diag::warn("[MGPU][T4] QueryDisplayConfig unavailable - falling back to "
                             "comparing monitor rectangles, which cannot tell a cloned pair "
                             "from two extended displays placed on top of each other.");
            return duplicated_by_rect();
        }

        UINT32 np = 0, nm = 0;
        if (p_sizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0)
            return duplicated_by_rect();

        DISPLAYCONFIG_PATH_INFO *paths =
            (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(DISPLAYCONFIG_PATH_INFO));
        DISPLAYCONFIG_MODE_INFO *modes =
            (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof(DISPLAYCONFIG_MODE_INFO));
        if (paths == nullptr || modes == nullptr)
        { free(paths); free(modes); return duplicated_by_rect(); }

        bool dup = false;
        if (p_query(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) == ERROR_SUCCESS)
        {
            for (UINT32 i = 0; i < np && !dup; ++i)
                for (UINT32 j = i + 1; j < np && !dup; ++j)
                {
                    const DISPLAYCONFIG_PATH_INFO &a = paths[i];
                    const DISPLAYCONFIG_PATH_INFO &b = paths[j];
                    if (a.sourceInfo.adapterId.LowPart  == b.sourceInfo.adapterId.LowPart &&
                        a.sourceInfo.adapterId.HighPart == b.sourceInfo.adapterId.HighPart &&
                        a.sourceInfo.id == b.sourceInfo.id &&
                        a.targetInfo.id != b.targetInfo.id)
                        dup = true;   // one source, two targets = clone
                }
        }
        else
        {
            free(paths); free(modes);
            return duplicated_by_rect();
        }

        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][T4] display topology: %u active path(s) -> %s",
                 (unsigned)np, dup ? "DUPLICATE (one source driving two targets)"
                                   : "extended or single");
        mgpu::diag::info(l);

        free(paths); free(modes);
        return dup;
    }
}

// NoActivate = 0 off | 1 always on | 2 AUTO (default).
//
// AUTO is the shipping behaviour and the reason the detection above exists:
// the window style only matters in DUPLICATE mode, where the bridge output is
// on the same picture as the game and a click on it costs the game its
// foreground - and with it the keyboard, the mouse and the pad. In extended
// mode the two windows are on different screens and taking foreground is
// normal, wanted behaviour, so nothing changes there.
bool window_no_activate()
{
    const int m = ini_read_sr_int("NoActivate", 2, 0, 2);
    if (m == 0) return false;
    if (m == 1)
    {
        mgpu::diag::info("[MGPU][T4] NoActivate=1 - forced on regardless of display topology.");
        return true;
    }
    return dispcfg::duplicated();
}

unsigned autoarm_frames()
{
    return ini_read_autoarm_frames();
}

// R145. Does an mgpu.ini exist at all? ini_read_autoarm_frames returns 0 both
// when the file is missing and when the file says AutoArm=0, and the idle
// screen has to tell those apart: the first is an unfinished install and the
// second is somebody's decision. This is the only question ini_slurp's return
// value answers on its own, so it is asked directly.
//
// Called once, from a function-local static, and never from a hot path.
static bool ini_file_present()
{
    char buf[INI_BYTES];
    return ini_slurp(buf, sizeof buf);
}

// ---- P6.3: intensity on the hotkeys ----
//
// Until now Intensity was read from mgpu.ini once, at arm time. Finding the
// right value therefore cost one game launch per value, with the scene reset
// each time and the comparison spread across runs - the same cross-run problem
// that has confounded half the measurements in this project.
//
// This works for one specific reason: P1.2 established that NGX parameters are
// LIVE PER EVALUATE (intensity 0.00 vs 1.60 changed 98.03% of pixels while the
// same-value control was byte-identical), and the pass loop sets them fresh
// every frame. So a value changed between frames takes effect on the very next
// one, with no rebuild, no re-arm and no relaunch.
//
// Bridge thread only, by construction: RegisterHotKey delivers WM_HOTKEY to the
// thread that registered it, which is the bridge thread, which is also the only
// thread that reads intensity[]. Nothing here needs a lock.

// ---- P6.4: the accessors the overlay panel drives ----
//
// BRIDGE THREAD ONLY is NOT true of these - the ReShade overlay callback runs
// on whichever thread presents the runtime it belongs to. So unlike the hotkey
// helpers above, these take the stream's lock. They are deliberately tiny and
// never block on anything: a UI callback that can stall is a UI callback that
// can stall a present.
//
// gpu1_context still names no ReShade and no ImGui type. The panel lives in
// dllmain.cpp, where those headers already are, and talks to the stream through
// plain scalars - the same rule that has kept this file portable since T3.
// Defined with the rest of the Reflex code further down this file. Declared
// here because ui_read is above it and the panel needs the state.
namespace reflex { void status(int &was_out, int &now_out, bool &applied_out); void restore();
                   void clear_residue(ID3D12Device *dev); }

// R108 / V26. What the idle screen should say, read from the stream state.
// Called from present_frame on the bridge thread, before any other lock is
// taken there, so this never nests with S.cs.
// V27. Panel thread. Stages a Super Resolution rebuild; stream_poll commits it.
// Pass -1 for a field to leave it alone.
// ======= V32: RELEASE THE NGX FEATURES WHEN THE GAME CLOSES =======
//
// THE BUG THIS FIXES, STATED PLAINLY: nothing released them. `stream_release()`
// has exactly two callers - a failed arm, and a stream that reached its own
// frame bound - and neither of those is what happens when a player closes the
// game. Every ordinary exit left alive, on GPU 1:
//
//   - two DLSS-NR feature handles
//   - one DLSS Super Resolution feature handle
//   - both NGX parameter blocks
//   - the driver's nvngx_dlss.dll, LoadLibrary'd by us
//
// and then `gpu1::shutdown()` released the D3D12 DEVICE those features were
// created against. Releasing a device out from under live NGX features is the
// oldest rule in this file broken at the very last step, every single run.
//
// WHY IT ONLY BIT AFTER SUPER RESOLUTION EXISTED. NR alone leaked the same way
// for months without a second-launch crash. SR added a SECOND NGX consumer on
// that device, with its own parameter block, created against a snippet DLL the
// game does not otherwise use. Whatever driver-side state survives a process,
// two abandoned consumers are not the same as one.
//
// AND IT EXPLAINS THE SHAPE NOTHING ELSE DID: first launch clean, second
// launch dead in CreateFeature; unaffected by AutoArm, depth, passes, Reflex;
// cleared by opening the NVIDIA app, which resets exactly this kind of state.
// The FAIL_OutOfDate on the line above every crash is a SYMPTOM of the
// previous session, not a cause of this one - which is why guarding on it
// (V29, V30) prevented the crash and fixed nothing.
//
// Called from the bridge thread's ordered teardown, BEFORE gpu1::shutdown().
// Order matters and it is the same order the rest of this file uses: the
// features go before the device they were created against.
void stream_shutdown()
{
    // Undo the driver mode we set on GPU 0, before anything else and outside
    // the lock. Runs even when the stream was never armed, and is itself
    // idempotent.
    reflex::restore();

    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);

    // V38. FIRST CALLER WINS. s.armed is NOT cleared by stream_release(), so
    // without this a second call would run the whole release again and free
    // things that are already gone - turning a safety net into a new crash.
    if (s.ngx_shut_done) return;
    s.ngx_shut_done = true;

    if (!s.requested && !s.armed && s.nr_handle[0] == nullptr && s.sr_handle == nullptr)
        return;   // nothing was ever built; say nothing

    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][T5] releasing NGX before the device goes: armed=%d neural=%d "
             "NR handles=%d/%d SR handle=%s. THIS IS THE STEP THAT WAS MISSING - every "
             "exit before V32 abandoned these and then released the device underneath them.",
             s.armed ? 1 : 0, s.nr_ok ? 1 : 0,
             (s.nr_handle[0] != nullptr) ? 1 : 0,
             (stream_state::MAX_PASSES > 1 && s.nr_handle[1] != nullptr) ? 1 : 0,
             (s.sr_handle != nullptr) ? "yes" : "no");
    mgpu::diag::info(l);

    // Vendor calls, so they announce themselves first (roadmap C0).
    // ReleaseFeature is NGX's, and P1.0b already faulted once inside NGX's
    // teardown - if the process dies here, this line names the reason.
    mgpu::diag::info("[MGPU][T5] entering the ordered release - IF THIS IS THE LAST LINE, "
                     "IT DIED IN NGX ReleaseFeature OR IN A RESOURCE RELEASE UNDER IT");
    stream_release();
    mgpu::diag::info("[MGPU][T5] NGX features released.");

    // ---- V40: DESTROY THE PARAMETER BLOCKS. NEVER DONE BEFORE. ----
    //
    // The stream creates two NGX parameter blocks - one for the neural stage,
    // one for Super Resolution, kept separate because DEFECT C says two NGX
    // consumers must not share one - and until now NEITHER WAS EVER
    // DESTROYED. DestroyParameters only ran in the P1.0c probe path, which the
    // stream does not use. Two NGX-owned objects leaked per launch.
    //
    // THE ORDER IS THE POINT, and it is the same order this file uses
    // everywhere else: the features go first (done above), then the parameter
    // blocks they were created against, then the session that owns both. A
    // Shutdown1 called with parameter blocks still outstanding is a plausible
    // reason for a session that does not close cleanly - which is exactly the
    // fault that cost us a night.
    //
    // Announced first, because it is a vendor call in a teardown path, and
    // this project has now been killed twice by exactly that.
    if (s.ngx_destroy_params != nullptr)
    {
        if (s.nr_params != nullptr)
        {
            mgpu::diag::info("[MGPU][T5] DestroyParameters (neural) - IF THIS IS THE LAST "
                             "LINE, IT DIED IN NGX");
            const NVSDK_NGX_Result dr = s.ngx_destroy_params(s.nr_params);
            char dl[220];
            snprintf(dl, sizeof dl, "[MGPU][T5] DestroyParameters (neural) -> 0x%08X (%s)",
                     (unsigned)dr, ngx_result_name(dr));
            mgpu::diag::info(dl);
            s.nr_params = nullptr;
        }
        if (s.sr_params != nullptr)
        {
            mgpu::diag::info("[MGPU][T5] DestroyParameters (super resolution) - IF THIS IS THE "
                             "LAST LINE, IT DIED IN NGX");
            const NVSDK_NGX_Result dr = s.ngx_destroy_params(s.sr_params);
            char dl[220];
            snprintf(dl, sizeof dl, "[MGPU][T5] DestroyParameters (SR) -> 0x%08X (%s)",
                     (unsigned)dr, ngx_result_name(dr));
            mgpu::diag::info(dl);
            s.sr_params = nullptr;
        }
    }
    else
    {
        mgpu::diag::warn("[MGPU][T5] DestroyParameters unresolved - the two NGX parameter "
                         "blocks are leaked, as they have been in every build before V40.");
    }

    // ==== V48: Shutdown1 IS NOT CALLED. THE REASON V36 ADDED IT WAS WRONG. ====
    //
    // V36 added NVSDK_NGX_D3D12_Shutdown1 here on one theory: that the
    // second-launch crash was caused by an NGX session this add-on never
    // closed, because "the OS reclaims it at process exit" had been assumed
    // and never checked.
    //
    // THAT THEORY IS DISPROVEN. Proven 2026-09-13 with our own NGX code
    // provably disabled - the log carried "[P1.0c] SKIPPED - recovery launch"
    // and "[P4.0] arm REFUSED" - and the snippet initialised anyway, on the
    // GAME'S thread, 1.5 seconds before our bridge thread existed. The cause
    // was nvngx_dlssnr.dll sitting beside the executable where the title's
    // Streamline scans for it. Our own install instructions put it there. The
    // session was never the problem, and closing it was never the fix.
    //
    // AND THE CALL ITSELF KILLS THE PROCESS. Three times on the record:
    //   P1.0b            - faulted inside Shutdown1, which is why it was
    //                      removed in the first place;
    //   V44, 2026-09-13  - called after a caught CreateFeature fault, took
    //                      the process down one millisecond later;
    //   Dawnwalker, 2026-09-13 - a CLEAN teardown. Features released, both
    //                      parameter blocks destroyed, both Success, then
    //                      EXCEPTION_ACCESS_VIOLATION writing 0x0 two frames
    //                      deep inside _nvngx. The marker line below was the
    //                      last thing in the log.
    //
    // So the call was added to fix something it did not cause, does not fix,
    // and crashes while attempting. It is removed. The features are released
    // and the parameter blocks destroyed above - that part is real work and it
    // stays. The session is left to the process, which is where P1.0c left it
    // for good reasons that turned out to be the right ones for the wrong
    // stated reason.
    //
    // DO NOT REINTRODUCE THIS without a reproduction that shows an unclosed
    // session causing a fault. Three crashes and one disproven theory is the
    // whole case file.
    (void)s.ngx_dev;
    s.ngx_shutdown = nullptr;
    s.ngx_dev = nullptr;
    mgpu::diag::info("[MGPU][T5] NGX session left open on purpose - Shutdown1 is NOT called. It "
                     "crashed on a clean teardown (Dawnwalker, 2026-09-13) and after a caught "
                     "fault (V44), and the second-launch crash it was added to fix turned out to "
                     "be nvngx_dlssnr.dll sitting beside the executable. Features and parameter "
                     "blocks above ARE released; only the session is left to the process.");
}

// ===== V41: THE PANEL ARMS, AND HOLDS AUTOARM WHILE IT IS OPEN =====
//
// WHY A REQUEST AND NOT A CALL. stream_request() is documented BRIDGE THREAD
// ONLY and the hotkey path honours that. The overlay draws on a presenting
// thread, so the panel stages and the bridge loop commits - the same shape the
// SR rebuild uses, and for the same reason.
//
// WHY AUTOARM HOLDS. Settings are read from mgpu.ini AT ARM TIME, so anything
// changed before arming applies in THIS session with no restart. That is the
// whole point of a settings menu, and it is worthless if AutoArm fires while
// somebody is still reading the menu. So AutoArm waits while the panel is open
// and the user arms when ready - or closes the panel and lets it fire.
//
// OPEN IS A TIMESTAMP, NOT A FLAG. ReShade gives no "overlay closed" event, so
// the panel stamps this every time it draws and "open" means stamped within
// the last half second. A flag would stick forever the moment someone closed
// the overlay with the mouse.
static std::atomic<unsigned long long> g_panel_seen{0};
static std::atomic<unsigned> g_arm_request{0};

static unsigned long long now_ms()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (f.QuadPart > 0) ? (unsigned long long)((c.QuadPart * 1000ll) / f.QuadPart) : 0ull;
}

void ui_panel_drawn()
{
    g_panel_seen.store(now_ms(), std::memory_order_relaxed);
}

bool ui_panel_is_open()
{
    const unsigned long long t = g_panel_seen.load(std::memory_order_relaxed);
    if (t == 0ull) return false;
    const unsigned long long n = now_ms();
    return (n >= t) && (n - t) < 500ull;
}

void ui_request_arm()
{
    g_arm_request.store(1u, std::memory_order_relaxed);
}

// Bridge thread. True once per request.
bool ui_take_arm_request()
{
    return g_arm_request.exchange(0u, std::memory_order_relaxed) != 0u;
}

void ui_set_sr_request(int quality, int preset, int scale)
{
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    if (quality >= 0) s.sr_req_quality = quality;
    if (preset  >= 0) s.sr_req_preset  = preset;
    // V39. R is now settable from the panel, because without it the quality
    // modes are nearly inert: in ordinary DLSS, picking Quality or Performance
    // IS how the render resolution is chosen, and in this pipeline R came from
    // the game instead. Measured 2026-09-13 - fourteen rebuilds cycling
    // Quality 0/1/2 against a fixed R=1280x720, every one of them Success and
    // none of them visibly different. The mode was arriving; there was nothing
    // for it to do.
    if (scale   >= 0) s.sr_req_scale   = scale;
    s.sr_rebuild_req = 1u;
}

// V28. Panel thread. Writes a key into mgpu.ini and returns whether it landed.
// THE RUNNING SESSION DOES NOT CHANGE - every key here is read at arm. That is
// the compromise for the settings that cannot be live: Reflex is a per-device
// driver mode engaged once on the game's first frame, and the DLAA lever moves
// R before the feature exists. Rather than pretend either is a toggle, the
// panel edits the file and says the next launch will pick it up.
bool ui_ini_write(const char *key, int value)
{
    const bool ok = ini_write_int(key, value);
    if (ok) ini_mirror_set(key, value);
    return ok;
}

// V28. The FILE's value, not the running session's. Cached after the first
// read so the panel can call it every time it draws.
int ui_ini_read(const char *key, int dflt)
{
    return ini_mirror_get(key, dflt);
}

// ---- V43: THE ARM SENTINEL ----
//
// ArmCrashed=1 is written to mgpu.ini immediately BEFORE CreateFeature and
// cleared immediately after it returns. If it is still set on the next
// launch, the previous launch died inside NGX and never came back.
//
// WHY A FILE AND NOT A FLAG: the thing we are detecting is a process that
// stopped existing. Nothing in memory survives it, no destructor runs, no
// handler fires. A file written before the call and removed after it is the
// only record that can outlive the event it records.
// SELF-INITIALISING ON PURPOSE. The alternative was a call site in
// worker.cpp that had to sit above ngx_probe, and "a fix that only works if
// it is called early enough" is how the NGX session went unclosed for a
// month. in_recovery() runs this once, so every gate below is correct no
// matter which one the process reaches first.
bool recovery_begin()
{
    const int v = ini_mirror_get("ArmCrashed", 0);
    if (v == 0) return false;

    // ---- V44: TWO SENTINEL VALUES, AND THEY MEAN DIFFERENT THINGS ----
    //
    //   1 = the previous launch entered CreateFeature and the PROCESS DIED.
    //       Nothing ran afterwards, nothing was cleaned. This launch must be
    //       the hard clearing launch: no NGX at all.
    //
    //   ANY NON-ZERO value means the same thing in V48. V44 briefly used 2
    //       for "faulted but caught and cleaned up in process", which relied
    //       on calling Shutdown1 after the fault. That call is gone - it kills
    //       the process - so there is no in-process cleanup and no optimistic
    //       state left to distinguish. A 2 left in an ini by a V44 build must
    //       NOT be treated as "run normally": that was the one value that
    //       skipped the clearing launch, and it would now skip it forever.
    stream_state &s = str();
    {
        std::lock_guard<std::mutex> lk(s.cs);
        s.recovery        = true;
        s.recovery_clears = true;
        s.neural          = false;
    }
    mgpu::diag::warn(
        "[MGPU][V43] RECOVERY LAUNCH. mgpu.ini still has ArmCrashed=1, so the previous launch "
        "entered CreateFeature and never returned. This launch does NOT touch NGX at all - no "
        "probe Init, no arm, no AutoArm - because a launch in which this module leaves NGX "
        "alone is what clears the residue. That is the same thing as renaming the add-on to "
        ".bak and running the game once, which is the only procedure that has ever worked. "
        "Quit normally and launch again and the bridge will arm.");
    return true;
}

bool in_recovery()
{
    // std::call_once rather than a plain static bool: the first caller can be
    // the bridge thread (ngx_probe) or the game thread (an arm), and a torn
    // read here would let one of them past the gate.
    static std::once_flag once;
    std::call_once(once, [] { (void)recovery_begin(); });

    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    return s.recovery;
}

// Cleared only after the recovery launch has demonstrably RUN, not at the
// moment we notice the flag. If we cleared it on sight, a user who alt-F4s
// two seconds in would have spent their clearing launch on nothing and the
// next one would crash again with no sentinel left to catch it.
void recovery_tick_presented()
{
    // Through in_recovery() rather than reading the flag, so this path also
    // runs the one-shot init if it somehow got here first.
    if (!in_recovery()) return;

    stream_state &s = str();
    bool clear_now = false;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        if (!s.recovery || s.recovery_cleared) return;
        // V44. Only the sentinel-entered recovery clears the key. A recovery
        // entered from a fault caught THIS launch has already rewritten it on
        // purpose, and clearing it here would erase the note as fast as it
        // was written.
        if (!s.recovery_clears) return;
        if (++s.recovery_frames < 600u) return;
        s.recovery_cleared = true;
        clear_now = true;
    }
    if (clear_now)
    {
        mgpu::diag::info("[MGPU][V43] recovery launch has presented 600 frames - clearing "
                         "ArmCrashed. The NEXT launch will arm normally.");
        (void)ui_ini_write("ArmCrashed", 0);
    }
}

void arm_sentinel_set()
{
    mgpu::diag::info("[MGPU][V43] writing ArmCrashed=1 before entering NGX. If this launch dies "
                     "in CreateFeature the key stays set and the next launch runs without NGX to "
                     "clear the residue.");
    (void)ui_ini_write("ArmCrashed", 1);
}

void arm_sentinel_clear()
{
    mgpu::diag::info("[MGPU][V43] CreateFeature returned - clearing ArmCrashed.");
    (void)ui_ini_write("ArmCrashed", 0);
}

// ---- V44: THE GAME SURVIVED THE FAULT. NOW MAKE THE LAUNCH USEFUL ----
//
// Called only from the one place that catches an access violation inside
// CreateFeature. Three jobs, in this order:
//
//   1. Close the NGX session. Every feature handle from this arm is either
//      null or was never returned, so nothing is outstanding and this is the
//      ordering V36 established as the safe one. This is the one thing we do
//      that might make the CURRENT launch count as a clearing launch.
//   2. Put the bridge into recovery state, so the window shows FIXING
//      RESIDUAL ERROR and nothing touches NGX again this run.
//   3. Write the sentinel, escalating if the soft path has already been
//      tried and did not work.
//
// The order matters: if step 1 kills us after all, the sentinel from
// arm_sentinel_set() is still on disk at 1, which is the hard-recovery value
// and exactly what we would want.
// ---- V46: WHAT THE PANEL NEEDS TO WARN ABOUT THE INSTALL LAYOUT ----
//
// One int, because the panel needs one sentence. The nuance belongs in the
// log, where someone reading a bug report can find it; a settings screen that
// explains module instancing to a player is a settings screen nobody reads.
//
//   0 = correct. A private copy exists under a subfolder and there is none
//       beside the executable, so the title cannot load it and NGX binds to
//       the adapter we pass.
//   1 = THE BUG. A copy sits beside the executable. The title's Streamline
//       scans that folder and binds NGX to the game's GPU before our thread
//       exists. Whether a private copy also exists does not make this safe to
//       ignore - it works, but the user is one file away from the crash and
//       should be told.
//   2 = nvngx_dlssnr.dll is nowhere we can find it. Neural rendering cannot
//       start at all.
//
// Filesystem only - no LoadLibrary, no NGX - so the panel can call it on the
// first frame it draws, long before anything arms, and in a recovery launch
// where NGX is deliberately untouched.
int ui_install_layout()
{
    wchar_t p[MAX_PATH * 2] = {};
    const bool have_private = find_private_snippet(L"nvngx_dlssnr.dll", p, MAX_PATH * 2);
    const bool beside       = nr_snippet_is_beside_exe();

    if (beside) return 1;
    return have_private ? 0 : 2;
}

void present_screen_state(int &st_out, const char *&l1, const char *&l2)
{
    // ---- R145: THE TWO LATCHES, TAKEN BEFORE THE LOCK ----
    //
    // ini_file_present() opens a file. s.cs is the mutex the game's render
    // thread takes EVERY FRAME, and DEFECT E is the standing rule that nothing
    // which can block may be done while holding it. A function-local static
    // initialises exactly once, on the first present, and every present after
    // that reads a bool and an unsigned. Both answers are fixed for the
    // process: the config is read once at startup and never re-read.
    static const bool     ini_found  = ini_file_present();
    static const unsigned autoarm_at = autoarm_frames();

    // R150. Roughly one minute at 60 fps. Chosen against measurement, not
    // taste: a healthy arm on Resonance completes at 252 frames on the depth
    // lane and ~1305 on the velocity lane, so this is four times the slowest
    // lane of a known-good run.
    const unsigned long long ARM_REPORT_FRAMES = 3600ull;

    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);

    // R142. Read ONCE: the branch below tests it twice and the two tests must
    // not be able to disagree with each other across a store from dllmain.
    const int tap_state = g_tap_state_game.load(std::memory_order_relaxed);
    // R143. The second half of the same question, from the path that survives
    // when the effect runtime does not. Read here for the same reason.
    const bool fx_absent = g_game_fx_absent.load(std::memory_order_relaxed);

    l1 = MGPU_IDLE_L1;
    // V43. FIRST, because it outranks everything else this screen can say.
    // In a recovery launch nothing is going to arm and nothing is waiting, so
    // every other message on this screen would be a lie. The colour is the
    // error red, since this IS an error condition - it is simply one the
    // add-on is repairing by itself.
    if (s.recovery)
    {
        st_out = mgpu::screen::st_error;
        l1 = "FIXING RESIDUAL ERROR";
        l2 = "PLEASE RESTART THE GAME";
    }
    else if (s.summarised)
    {
        // The run ended on its own bound. Say so - this used to be indicated
        // by the colours coming back, which nobody could be expected to know.
        st_out = mgpu::screen::st_idle;
        l2 = "RUN COMPLETE - RESTART THE GAME TO RUN AGAIN";
    }
    // ---- THE STREAM IS ARMED. ONLY THE NEURAL STAGE CAN STILL FAIL. ----
    //
    // R145 HOISTED THESE TWO ABOVE EVERYTHING BELOW. Armed is the one state
    // this function can read without inference: the resources exist and the
    // ring is live. Every branch after it is an answer to "why is it NOT
    // armed", and none of them can be right while it is.
    else if (s.armed && s.neural && s.nr_tried && !s.nr_ok)
    {
        // V30. THE FIRST CONDITION WORTH AN ERROR CODE, AND IT EARNED ONE THE
        // hard way: a run on 2026-09-12 sat on "WAITING FOR THE FIRST FRAME"
        // for NINE THOUSAND FRAMES while the log had already said, five
        // seconds in, that the neural stage was never coming up. The screen
        // said "waiting" and nothing was waiting. That is the exact failure
        // the idle screen was written to prevent, reproduced by the idle
        // screen itself.
        st_out = mgpu::screen::st_error;
        l1 = MGPU_E302_L1;
        // The one screen someone stares at when nothing works at all, so it
        // carries the place to send the log rather than just the symptom.
        l2 = "DLSS DID NOT START - SEND RESHADE.LOG TO GITHUB.COM/MAOHGAD-WEB/NEURAL-COPROCESSOR";
    }
    else if (s.armed)
    {
        st_out = mgpu::screen::st_waiting;
        l2 = MGPU_IDLE_L2;
    }

    // ================= NOT ARMED. THE REST OF THIS FUNCTION SAYS WHY. =======
    //
    // R145, AND THE WHOLE REASON IT EXISTS. Until this milestone the not-armed
    // half of this chain had exactly two answers - "ARMING" if the stream had
    // been requested and "STARTING - THE GAME WILL APPEAR WHEN THE STREAM
    // ARMS" if it had not - and BOTH of them are promises. Measured
    // 2026-09-16 on two titles: no mgpu.ini in the game folder, so AutoArm
    // defaulted to 0, so stream_request() was never called, so
    // stream_on_finish_effects returned at its first line every frame. The
    // add-on was loaded, healthy, and never going to do anything, and the
    // screen said the game would appear. Two test sessions were spent on it.
    //
    // The order below is deliberate: FURTHEST FROM WORKING FIRST. A missing
    // config outranks a held arm, because an arm that was never requested
    // cannot be held. Read top to bottom it is the sequence a run has to pass
    // through, and wherever it stops is the thing to fix.

    // ---- IT WAS NEVER ASKED FOR ----
    //
    // 205 IS AN ERROR AND 206 IS NOT. Nobody chooses to have no mgpu.ini: the
    // shipped file sets AutoArm=1 and Depth=1 and the code defaults are 0 and
    // 0, so a missing file silently disables both the stream and the depth
    // path - and [P7.2] in the log now says so in the same words. AutoArm=0
    // with a file present is a decision, usually a measurement one, so it gets
    // the neutral field and a flat statement instead of a red screen.
    else if (!s.requested && !ini_found)
    {
        st_out = mgpu::screen::st_error;
        l1 = MGPU_E205_L1;
        l2 = MGPU_E205_L2;
    }
    else if (!s.requested && autoarm_at == 0)
    {
        st_out = mgpu::screen::st_idle;
        l1 = MGPU_S206_L1;
        l2 = MGPU_S206_L2;
    }
    else if (!s.requested)
    {
        // AutoArm is on and has not counted out yet. This is the only state in
        // the not-armed half that is genuinely just early, and it is the one
        // the old default text was written for. It keeps that text.
        st_out = mgpu::screen::st_idle;
        l2 = "STARTING - THE GAME WILL APPEAR WHEN THE STREAM ARMS";
    }

    // ---- IT WAS ASKED FOR AND THE ARM IS HELD. ON WHAT? ----
    //
    // R142/R143: THE TAP, WHICH IS THE ONLY ONE OF THESE THAT IS PROVABLY A
    // FAULT. With Depth on and no usable tap on the GAME runtime the arm holds
    // forever by design - a chain of guarded early returns that never touches
    // a game resource, so it cannot crash and cannot draw attention to itself.
    //
    //   fx_absent -> 204. The GAME runtime has presented past AutoArm's
    //         threshold without ever running an effect pass, so the
    //         enumeration that sets tap_state can never run. R138 wrote the
    //         rule down a milestone before R142 ignored it: AN ABSENCE CANNOT
    //         BE REPORTED BY THE CODE THE ABSENCE SILENCES.
    //   -1 -> 204, enumerated nothing. The search path does not reach the
    //         file. [R142] in the log prints their EffectSearchPaths and ours.
    //    0 -> 203, enumerated but off, and the self-enable did not take.
    //
    // R145: tap_state IS NOW LIVE rather than latched at first settle - see
    // probe::tap_state(). A latched 0 was a false 203 on a healthy run.
    //
    // Depth=0 IS EXCLUDED DELIBERATELY: that run does not want depth, and an
    // absent tap is not a fault in it. -2 is excluded too - no scan has
    // completed, and reporting a fault before one has is the "empty list
    // recorded as a fact" mistake P1.6 exists to avoid.
    else if (s.depth_mode != 0 &&
             (fx_absent || (tap_state >= -1 && tap_state <= 0)))
    {
        st_out = mgpu::screen::st_error;
        if (fx_absent || tap_state < 0) { l1 = MGPU_E204_L1; l2 = MGPU_E204_L2; }
        else                            { l1 = MGPU_E203_L1; l2 = MGPU_E203_L2; }
    }

    // ---- HELD, AND NOT PROVABLY BROKEN. NAME THE LANE AND STAY GREY. ----
    //
    // R63 measured 20 seconds of real scene before ReShade bound a depth
    // buffer and R56 measured 35, both from a menu; R78's velocity lane wants
    // 240 consecutive frames of the same handle on top of that. So a held arm
    // is NORMAL for the first half-minute of a launch and going red on it
    // would train people to ignore the red. What was wrong was never the
    // colour - it was that "ARMING" for ninety seconds is indistinguishable
    // from a hang, and said nothing about which of the two lanes was holding
    // while the log had the answer in [R63] and [R78].
    //
    // depth_arm_waits and mvec_arm_waits are incremented by the two arm-hold
    // paths in stream_on_finish_effects and by nothing else, so a non-zero
    // count IS that path having run. No new state, no inference.
    // R150. PAST A MINUTE THE LANE NAME IS NO LONGER THE USEFUL THING.
    //
    // Naming the lane was R145's fix for "ARMING" being indistinguishable from
    // a hang, and for the first half-minute it is the right answer - the hold
    // is normal and the screen should look normal. Past that it stops being
    // information: the person has read it, it has not changed, and what they
    // need is somewhere to send the log.
    //
    // ARM_REPORT_FRAMES is counted in GAME frames by the same two counters the
    // hold paths increment, so a faster machine reaches it sooner in wall
    // clock. That is the right direction - a 120 fps rig that has held for
    // 5400 frames has waited 45 seconds with twice the chances to succeed.
    //
    // The screen does NOT claim a fault here. See MGPU_H207_L2: it states the
    // condition under which this is one, and leaves the judgement with the
    // person who can see whether a scene is on screen.
    else if (s.hold_lane == 1)
    {
        if (s.depth_arm_waits > ARM_REPORT_FRAMES)
        {
            st_out = mgpu::screen::st_error;
            l1 = MGPU_H207_DEPTH_L1;
            l2 = MGPU_H207_L2;
        }
        else
        {
            st_out = mgpu::screen::st_waiting;
            l2 = MGPU_WAIT_DEPTH_L2;
        }
    }
    else if (s.hold_lane == 2)
    {
        if (s.mvec_arm_waits > ARM_REPORT_FRAMES)
        {
            st_out = mgpu::screen::st_error;
            l1 = MGPU_H207_MVEC_L1;
            l2 = MGPU_H207_L2;
        }
        else
        {
            st_out = mgpu::screen::st_waiting;
            l2 = MGPU_WAIT_MVEC_L2;
        }
    }
    else
    {
        // Requested, nothing reporting a hold. The arm is in flight.
        st_out = mgpu::screen::st_waiting;
        l2 = MGPU_WAIT_ARM_L2;
    }
}

void ui_read(ui_state &out)
{
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    out.armed       = s.armed;
    out.summarised  = s.summarised;
    out.neural      = s.neural;
    out.nr_ok       = s.nr_ok;
    // The header cannot see MAX_PASSES (it is private to this file), so
    // ui_state::intensity carries a second copy of the bound. A duplicated
    // constant that only one side updates is how the hardcoded R10G10B10A2 in
    // ResizeBuffers survived a format change; this makes the drift a compile
    // error instead of a truncated copy at runtime.
    static_assert(sizeof(ui_state::intensity) / sizeof(float) >= stream_state::MAX_PASSES,
                  "ui_state::intensity is smaller than MAX_PASSES - update gpu1_context.hpp");

    out.tuning_on          = s.tuning_on;
    out.tone_strength      = s.tone_strength;
    out.structure_strength = s.structure_strength;
    out.skin_strength      = s.skin_strength;
    out.style              = s.style;
    out.auto_mask          = s.auto_mask;

    out.passes      = s.passes;
    out.max_passes  = stream_state::MAX_PASSES;
    out.profile     = s.profile;
    out.present_mode = s.present_mode;
    out.mvec_mode    = s.mvec_mode;
    out.mvec_dx      = s.mvec_dx;
    out.mvec_dy      = s.mvec_dy;
    out.mvec_scale_x = s.mvec_scale_x;
    out.mvec_scale_y = s.mvec_scale_y;
    out.preset       = s.preset;
    out.split_pos    = s.split_pos;
    for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i) out.intensity[i] = s.intensity[i];
    out.consumed    = s.consumed;
    out.produced    = s.produced;
    out.dropped     = s.dropped;
    out.overrun     = s.overrun;
    out.skipped     = s.nr_skipped;
    out.reordered   = s.reordered;
    out.bad_magic   = s.bad_magic;
    out.contract    = s.contract;

    // ---- rates, from the panel's own cadence ----
    {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        if (s.ui_tlast.QuadPart != 0 && f.QuadPart != 0)
        {
            const double dt = (double)(now.QuadPart - s.ui_tlast.QuadPart) / (double)f.QuadPart;
            if (dt >= 0.25)
            {
                s.ui_fps_prod = (double)(s.produced - s.ui_plast) / dt;
                s.ui_fps_cons = (double)(s.consumed - s.ui_clast) / dt;
                s.ui_tlast = now; s.ui_plast = s.produced; s.ui_clast = s.consumed;
            }
        }
        else { s.ui_tlast = now; s.ui_plast = s.produced; s.ui_clast = s.consumed; }
    }
    out.fps_produced = s.ui_fps_prod;
    out.fps_consumed = s.ui_fps_cons;

    // ---- GPU 1's own timestamps. THE SAME DIVISOR THE P2.2 SUMMARY USES. ----
    // A panel that computes a mean differently from the summary is a second
    // instrument disagreeing with the first, which is how an afternoon goes.
    if (s.ts_ok && s.ts_n > 0)
    {
        const double n = (double)s.ts_n;
        out.gpu1_copy_ms   = s.ts_sum[0] / n;
        out.gpu1_unpack_ms = s.ts_sum[1] / n;
        out.gpu1_eval_ms   = s.ts_sum[2] / n;
        out.gpu1_out_ms    = s.ts_sum[3] / n;
        out.gpu1_ts_ok     = true;
    }
    if (s.ready_n > 0) out.lat_ready_ms  = s.ready_sum / (double)s.ready_n;
    if (s.lat_n   > 0) out.lat_submit_ms = s.lat_sum   / (double)s.lat_n;
    if (s.q_n     > 0) out.backlog_mean  = (double)s.q_sum / (double)s.q_n;
    out.backlog_max  = s.q_max;
    out.ring_depth   = stream_state::RING;
    out.ring_window  = (s.rw_on != 0u) ? s.rw : 0u;
    out.ring_skipped = s.rw_skipped;

    // ---- DLSS Super Resolution ----
    out.sr_on        = (s.sr_on != 0u && s.sr_handle != nullptr);
    out.sr_requested = (s.sr_on != 0u);
    out.sr_w         = s.sr_w;
    out.sr_h         = s.sr_h;
    out.out_w        = (unsigned)s.width;
    out.out_h        = (unsigned)s.height;
    out.sr_quality   = s.sr_quality;
    out.sr_preset    = s.sr_preset;
    out.sr_scale_pct = s.sr_scale_pct;
    out.sr_mv_mode   = s.sr_mv_mode;
    out.sr_mv_lowres = s.sr_mv_lowres;
    out.sr_mv_fix_x  = s.sr_mv_fix_x;
    out.sr_mv_fix_y  = s.sr_mv_fix_y;
    out.auto_on        = (s.auto_on != 0u);
    out.auto_target_fps = s.auto_target_fps;
    out.auto_rung      = s.auto_rung;
    out.auto_rungs     = stream_state::AUTO_RUNGS;
    out.auto_changes   = s.auto_changes;
    out.auto_last_mean = s.auto_last_mean;
    out.auto_budget_ms = s.auto_budget_ms;
    out.sr_rebuild_pending = (s.sr_rebuild_req != 0u);
    out.sr_rebuild_count   = s.sr_rebuild_n;
    out.sr_rebuild_last_ok = s.sr_rebuild_last_ok;
    out.sr_snippet_requested = (s.sr_snippet != 0u);
    out.sr_snippet_driver    = s.sr_snippet_driver;

    // Three plain scalars written once at arm on the game thread.
    reflex::status(out.reflex_was, out.reflex_now, out.reflex_applied);
}

// P7.4. Re-apply the intensity SHAPE for the current pass count. Caller holds
// s.cs. Manual does nothing at all - that is the whole point of manual.
//
// front: pass 1 at PRESET_FULL, every other pass at PRESET_FLOOR.
// back:  pass s.passes at PRESET_FULL, every other pass at PRESET_FLOOR.
//
// "Every other pass" means every pass in the ARRAY, not just the active ones,
// so a pass that is currently inactive is already at the floor when the count
// grows to include it. Without that, raising the count would briefly show a
// pass at whatever it held from an earlier configuration.
static void preset_apply_locked(stream_state &s)
{
    if (s.preset == 0) return;
    for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i)
        s.intensity[i] = stream_state::PRESET_FLOOR;
    const unsigned hot = (s.preset == 1) ? 0u
                                         : ((s.passes >= 1 ? s.passes : 1u) - 1u);
    s.intensity[hot] = stream_state::PRESET_FULL;
    ++s.intensity_edits;
}

// P7.4. Pick the shape. 0 manual, 1 front-loaded, 2 back-loaded. Applied now
// and re-applied on every pass-count change until something drops it back to
// manual, which is what makes the delta between front and back a single
// variable even while the count is moving on camera.
void ui_set_preset(int mode)
{
    if (mode < 0 || mode > 2) return;
    stream_state &s = str();
    unsigned n = 1;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        if (s.preset == mode && mode == 0) return;
        s.preset = mode;
        preset_apply_locked(s);
        n = s.passes;
    }
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][P7.4] intensity preset = %s, %u pass%s. %s The shape FOLLOWS the pass "
             "count: change the count and the peak moves with it, so front and back stay "
             "comparable as the count climbs. Moving any single slider returns to manual.",
             (mode == 0) ? "MANUAL" : ((mode == 1) ? "FRONT-LOADED" : "BACK-LOADED"),
             n, (n == 1) ? "" : "es",
             (mode == 0) ? "Per-pass values are whatever they were; nothing is rewritten."
                         : ((mode == 1)
                                ? "Pass 1 at 2.00, every other pass at 0.10."
                                : "The LAST pass at 2.00, every other pass at 0.10."));
    mgpu::diag::info(l);
}

void ui_set_passes(unsigned n)
{
    if (n < 1) n = 1;
    if (n > stream_state::MAX_PASSES) n = stream_state::MAX_PASSES;
    stream_state &s = str();
    unsigned was;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        was = s.passes;
        if (was == n) return;
        s.passes = n;
        // Every handle exists from arm time, so this is a count change and
        // nothing is created or destroyed here. nr_final is recomputed on the
        // next consumed frame.
        ++s.intensity_edits;
        // P7.4: and the shape follows. Absolute per-pass values already
        // survived a count change - they were never reset - but surviving is
        // exactly what breaks a preset: "max on the last pass" set at 4 becomes
        // "max on the second to last" at 5 while the panel still claims
        // back-loaded. preset_apply_locked moves the peak instead.
        preset_apply_locked(s);
    }
    char l[300];
    snprintf(l, sizeof l,
             "[MGPU][P6.4] passes %u -> %u, live from the next frame. Handles for all %u were "
             "created at arm, so nothing is built or torn down here.",
             was, n, stream_state::MAX_PASSES);
    mgpu::diag::info(l);
}

// P7.9. Live, from the panel. Values are clamped only where the guide gives a
// range; the strengths are not documented with one, so they are passed as typed
// and the log records what was in force.
void ui_set_tuning(bool on)
{
    stream_state &s = str();
    float tone, structure, skin, style;
    bool mask;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        s.tuning_on = on;
        ++s.intensity_edits;   // a tuning change makes this a tuning run, and
                               // the summary already reports that count.
        tone = s.tone_strength; structure = s.structure_strength;
        skin = s.skin_strength; style = s.style; mask = s.auto_mask;
    }
    char l[420];
    snprintf(l, sizeof l,
             "[MGPU][P7.9] tuning %s - tone=%.2f structure=%.2f skin=%.2f style=%.2f automask=%u. "
             "With this ON the run is a TUNING run and its figures are not comparable to the "
             "published ones, which were all taken with these unset.",
             on ? "ON" : "OFF", tone, structure, skin, style, mask ? 1u : 0u);
    mgpu::diag::info(l);
}

void ui_set_tuning_value(int which, float v)
{
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    switch (which)
    {
    case 0: s.tone_strength      = v; break;
    case 1: s.structure_strength = v; break;
    case 2: s.skin_strength      = v; break;
    case 3: s.style              = v; break;
    case 4: s.auto_mask = (v != 0.0f); break;
    default: return;
    }
    // First touch enables the group. The panel shows these controls whether or
    // not tuning is on, because they are the first thing to reach for when the
    // image looks wrong - but nothing is SET until someone moves one, so a run
    // nobody touched is still identical to every published measurement.
    s.tuning_on = true;
    ++s.intensity_edits;
}

// P8.0. Live from the panel. Nothing is created or destroyed - the texture
// exists either way - so both arms of the control can be compared inside one
// launch rather than one launch per arm.
void ui_set_mvec_mode(int mode)
{
    stream_state &s = str();
    int was = 0;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        was = s.mvec_mode;

        // ---- R78: MODE 3 IS NOT A LIVE CONTROL AND THIS KEY MUST NOT MOVE IT ----
        //
        // The synthetic field is a texture on GPU 1 that can be refilled at
        // any time, which is why this toggle exists. THE REAL TRANSPORT IS AN
        // ARM-TIME DECISION: the slot was sized with an MVec region in it, the
        // seal describes that region, and the consumer checks the description
        // against what the arm recorded. Toggling out of mode 3 mid-run would
        // leave a bus carrying vectors nobody binds and a contract nobody
        // reads - and the run would look healthy while quietly having become a
        // different experiment. An accidental hotkey press is not allowed to
        // do that. The control is a relaunch, which is what a control is.
        if (s.mvec_mode == 3)
        {
            mgpu::diag::warn("[MGPU][R78] MVec toggle IGNORED: this run is MVec=3 (REAL "
                             "transport), which is decided at arm time because the slot is "
                             "sized for it. Change MVec in mgpu.ini and relaunch. Nothing "
                             "about this run has changed.");
            return;
        }

        if (mode < 0) mode = 0;
        if (mode > 1) mode = 1;   // 2 has no estimator in this build
        s.mvec_mode = mode;
        s.mvec_dirty = true;
    }
    if (was != mode)
    {
        char l[440];
        snprintf(l, sizeof l,
                 "[MGPU][P8.0] MVec -> %s. A run whose MVec state changed mid-stream covers frames "
                 "produced under both and its timings must not be quoted as a figure for either.",
                 (mode == 0) ? "OFF" : "SYNTHETIC");
        mgpu::diag::info(l);
    }
}

// P8.1. Live. Takes effect on the next consumed frame - nothing is created or
// destroyed - so all three modes are walkable inside one launch.
void ui_set_pass_reset(int mode)
{
    stream_state &s = str();
    int was = 0;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        was = s.pass_reset;
        if (mode < 0) mode = 0;
        if (mode > 2) mode = 2;
        s.pass_reset = mode;
    }
    if (was != mode)
    {
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][P8.1] PassReset -> %d. A run whose value changed mid-stream covers frames "
                 "produced under both and must not be quoted as a figure for either.", mode);
        mgpu::diag::info(l);
    }
}

// which: 0 dx, 1 dy, 2 scaleX, 3 scaleY.
void ui_set_mvec_value(int which, float v)
{
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    if      (which == 0) s.mvec_dx = v;
    else if (which == 1) s.mvec_dy = v;
    else if (which == 2) s.mvec_scale_x = v;
    else if (which == 3) s.mvec_scale_y = v;
    s.mvec_dirty = true;
}

void ui_set_intensity(unsigned pass_1based, float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    if (pass_1based == 0)
        for (unsigned i = 0; i < stream_state::MAX_PASSES; ++i) s.intensity[i] = v;
    else if (pass_1based <= stream_state::MAX_PASSES)
        s.intensity[pass_1based - 1] = v;
    ++s.intensity_edits;
    // P7.4: touching a value by hand means the shape is no longer a preset.
    // Silently keeping the preset label over hand-edited numbers is how a run
    // gets recorded as back-loaded when it is not.
    s.preset = 0;
}

// P7.5. Move the split seam. `dir` is -1 (left) or +1 (right); `coarse` takes
// a tenth of the frame instead of a fortieth, for crossing it quickly.
//
// Deliberately a hotkey and not only a panel slider: the panel is the ReShade
// overlay, and an overlay open across the frame is the one thing that cannot
// be in the shot while the seam is being dragged over a face. This has to work
// with nothing on screen but the game.
void ui_split_move(int dir, bool coarse)
{
    if (dir == 0) return;
    stream_state &s = str();
    float now;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        s.split_pos += (float)dir * (coarse ? 0.10f : 0.025f);
        if (s.split_pos < 0.0f) s.split_pos = 0.0f;
        if (s.split_pos > 1.0f) s.split_pos = 1.0f;
        now = s.split_pos;
    }
    char l[200];
    snprintf(l, sizeof l,
             "[MGPU][P7.5] split seam at %.0f%% - left of it is the INPUT, right of it is the "
             "NEURAL OUTPUT.", (double)(now * 100.0f));
    mgpu::diag::info(l);
}

// P7.5. Absolute, for the panel slider. 0.0 = all neural output, 1.0 = all
// input.
void ui_set_split_pos(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    s.split_pos = v;
}

// P7.4. The present mode, LIVE. 0 nr, 1 in, 2 split.
//
// This used to be read once from mgpu.ini at arm, which is precisely why the
// input and the output could never be compared: one run showed one of them,
// and no two runs of a game contain the same frame. Live, one keypress puts
// both on screen at once.
void ui_set_present_mode(int mode)
{
    if (mode < 0 || mode > 2) return;
    stream_state &s = str();
    int was;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        was = s.present_mode;
        if (was == mode) return;
        s.present_mode = mode;
    }
    static const char *const nm[3] = { "nr (neural output)",
                                       "in (the frame handed TO the model)",
                                       "split (left input | right output, SAME frame)" };
    char l[360];
    snprintf(l, sizeof l,
             "[MGPU][P7.4] present mode %s -> %s, live from the next present. Nothing is armed, "
             "torn down or re-timed: the neural stage runs identically in all three and only the "
             "copy into the bridge backbuffer changes.",
             nm[was], nm[mode]);
    mgpu::diag::info(l);
}

void ui_set_neural(bool on)
{
    stream_state &s = str();
    bool was;
    {
        std::lock_guard<std::mutex> lk(s.cs);
        was = s.neural;
        if (was == on) return;
        // nr_ok is NOT cleared: the feature handles stay alive so this can be
        // switched back without a 400 ms stall. The consume loop tests `neural`.
        s.neural = on;
        ++s.intensity_edits;
    }
    char l[260];
    snprintf(l, sizeof l, "[MGPU][P6.4] neural stage %s (handles kept alive either way)",
             on ? "ON" : "OFF - transport only, the window shows the last neural frame");
    mgpu::diag::info(l);
}

// Cycle which pass the steps act on: all -> 1 -> 2 -> ... -> passes -> all.
void intensity_cycle_target()
{
    stream_state &s = str();
    const unsigned n = (s.passes >= 1 && s.passes <= stream_state::MAX_PASSES) ? s.passes : 1u;
    s.intensity_target = (s.intensity_target >= n) ? 0u : s.intensity_target + 1u;

    char l[400];
    int w = snprintf(l, sizeof l, "[MGPU][P6.3] intensity target -> ");
    if (s.intensity_target == 0) w += snprintf(l + w, sizeof l - (size_t)w, "ALL passes");
    else                         w += snprintf(l + w, sizeof l - (size_t)w, "pass %u only",
                                               s.intensity_target);
    w += snprintf(l + w, sizeof l - (size_t)w, " | now:");
    for (unsigned i = 0; i < n && (size_t)w < sizeof l; ++i)
        w += snprintf(l + w, sizeof l - (size_t)w, " p%u=%.2f", i + 1, s.intensity[i]);
    mgpu::diag::info(l);
}

// One step up or down. STEP and the clamp are deliberate: 1.60 was exercised in
// P1.2 and worked, so the ceiling is above 1.0 and is not a guess about what
// the model accepts - it is a bound on how far one keypress can take you.
void intensity_step(int dir)
{
    stream_state &s = str();
    const unsigned n = (s.passes >= 1 && s.passes <= stream_state::MAX_PASSES) ? s.passes : 1u;
    const float STEP = 0.05f;
    const float LO = 0.0f, HI = 2.0f;

    for (unsigned i = 0; i < n; ++i)
    {
        if (s.intensity_target != 0 && s.intensity_target != i + 1) continue;
        float v = s.intensity[i] + STEP * (float)dir;
        if (v < LO) v = LO;
        if (v > HI) v = HI;
        s.intensity[i] = v;
        s.preset = 0;   // P7.4: a hand-driven step is a manual edit like any other
    }
    ++s.intensity_edits;

    char l[400];
    int w = snprintf(l, sizeof l, "[MGPU][P6.3] intensity %s (%s) ->",
                     (dir > 0) ? "UP  " : "DOWN",
                     (s.intensity_target == 0) ? "all passes" : "one pass");
    for (unsigned i = 0; i < n && (size_t)w < sizeof l; ++i)
        w += snprintf(l + w, sizeof l - (size_t)w, " p%u=%.2f%s", i + 1, s.intensity[i],
                      (s.intensity_target == i + 1) ? "<" : "");
    if ((size_t)w < sizeof l)
        snprintf(l + w, sizeof l - (size_t)w,
                 "%s", s.armed ? " (live from the next frame)"
                               : " (stream not armed yet - this is the starting value)");
    mgpu::diag::info(l);
}

void stream_request()
{
    // V43. Refuse every arm during a recovery launch - AutoArm, the hotkey,
    // and the panel's START NOW all funnel through here, so one gate covers
    // all three and worker.cpp needs no change. Said out loud rather than
    // ignored: a button that does nothing in silence is how this project
    // lost an evening once already.
    //
    // BEFORE the lock, not after. in_recovery() takes s.cs itself to run its
    // one-shot init, and taking it here first would deadlock on a
    // non-recursive mutex.
    if (in_recovery())
    {
        mgpu::diag::warn("[MGPU][P4.0] arm REFUSED - this is a recovery launch and NGX is not "
                         "being touched. Quit normally and launch again.");
        return;
    }

    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    if (s.finished)
    {
        mgpu::diag::info("[MGPU][P4.0] stream already ran to its bound this launch. One stream "
                         "per process, by design - restart to run another.");
        return;
    }
    if (s.requested) return;
    s.requested = true;
    stream_read_fault(s.fault, sizeof s.fault);
    s.neural = stream_read_neural();
    s.max_frames = stream_read_frames();
    s.profile = stream_read_profile();
    s.present_mode = ini_read_present_mode();
    // P8.0. Mode 2 has no estimator in this build; say so rather than behaving
    // like mode 0 in silence, which is the failure shape this project keeps
    // paying for.
    s.depth_mode = ini_read_depth_mode();
    s.depth_inverted = ini_read_depth_inverted();
    s.mvec_mode = ini_read_mvec_mode();
    if (s.mvec_mode == 2)
    {
        mgpu::diag::warn("[MGPU][P8.0] MVec=estimated requested, but this build has no flow "
                         "estimator (that is P8.1). Falling back to OFF. Use MVec=1 for the "
                         "synthetic control field.");
        s.mvec_mode = 0;
    }
    s.mvec_dx      = ini_read_named_float("MVecDX", 0.0f);
    s.mvec_dy      = ini_read_named_float("MVecDY", 0.0f);
    s.mvec_scale_x = ini_read_named_float("MVecScaleX", 1.0f);
    s.mvec_scale_y = ini_read_named_float("MVecScaleY", 1.0f);
    s.mvec_dirty   = true;
    {
        const int cqm = ini_read_copy_queue();
        if (cqm < 0)
        {
            mgpu::diag::error(
                "[MGPU][R28] CopyQueue= is present but names a value this build does not "
                "recognise. Valid values are 0, 1 and 2. FALLING BACK TO 0 AND SAYING SO - a "
                "setting that is silently not applied is DEFECT D's failure shape and it has "
                "cost this project rig launches before. This run is a CONTROL run.");
        }
        s.copy_queue_mode = (cqm < 0) ? 0 : cqm;
        s.copy_queue      = (s.copy_queue_mode != 0);
        s.cq_gated        = (s.copy_queue_mode == 2);
    }
    {
        // R102. Read here rather than at first evaluate so the arm line reports
        // what took effect, and so a malformed key is refused before a run is
        // spent on it.
        const int sp = ini_read_subrect();
        if (sp < 0)
        {
            mgpu::diag::error(
                "[MGPU][R102] Subrect= is present but names a value this build does not "
                "accept. Valid values are 25 to 100, as a PERCENT OF EACH AXIS. FALLING BACK "
                "TO 100 AND SAYING SO - a setting that is silently not applied is DEFECT D's "
                "failure shape. This run is a full-frame run.");
        }
        s.subrect_pct = (sp < 0) ? 100u : (unsigned)sp;
        if (s.subrect_pct != 100u)
        {
            char sl[700];
            snprintf(sl, sizeof sl,
                     "[MGPU][R102] SUBRECT=%u%% PER AXIS - THIS IS A MEASUREMENT RUN AND ITS "
                     "OUTPUT IS NOT A PICTURE. The model is asked to process %u%% of each axis "
                     "(about %u%% of the area) from base 0,0. Only that region of the output is "
                     "written; the rest of the window holds whatever was there before, so run "
                     "this with Profile=1 and read the P2.2 evaluate bracket, not the screen. "
                     "WHAT IT ANSWERS: whether EvaluateFeature's cost scales with output "
                     "pixels. The evaluate bracket tracking the area is the C2 scaling law "
                     "holding; the bracket not moving is that law being false, which is worth "
                     "just as much and costs the same one run.",
                     s.subrect_pct, s.subrect_pct,
                     (s.subrect_pct * s.subrect_pct) / 100u);
            mgpu::diag::warn(sl);
        }

        // ---- C2-SR: arm ----
        {
            const int sr = ini_read_sr();
            if (sr < 0)
                mgpu::diag::error("[MGPU][C2-SR] SRUpscale= is present but names a value this "
                                  "build does not accept. Valid values are 0 and 1. FALLING "
                                  "BACK TO 0 AND SAYING SO.");
            s.sr_on = (sr < 0) ? 0u : (unsigned)sr;

            if (s.sr_on != 0u && s.subrect_pct != 100u)
            {
                mgpu::diag::error("[MGPU][C2-SR] SRUpscale=1 REFUSES to run with Subrect below "
                                  "100. Subrect crops and SRUpscale reduces; together they "
                                  "measure neither. SRUpscale is off for this run.");
                s.sr_on = 0u;
            }
            s.signal_at    = (unsigned)ini_read_signal_at();
            s.present_vsync = (unsigned)ini_read_sr_int("PresentVsync", 1, 0, 1);
            s.sr_snippet    = (unsigned)ini_read_sr_int("SRSnippet", 1, 0, 1);
            s.rw_on         = (unsigned)ini_read_sr_int("RingWindow", 0, 0, 1);
            s.sr_mv_mode    = (unsigned)ini_read_sr_int("SRMvLowRes", 0, 0, 2);
            g_present_vsync.store(s.present_vsync, std::memory_order_relaxed);
            if (s.present_vsync == 0u)
                mgpu::diag::warn(
                    "[MGPU][L5] PresentVsync=0 - the bridge window presents IMMEDIATELY. "
                    "Expect tearing. This exists because the output side was never measured: "
                    "Present(1,0) waits for vblank and D6 allows three presents in flight, so "
                    "the chain can hold frames that L2 (which stops at consume) and P2.2 "
                    "(which stops at the evaluate) both miss. Nothing spins: this path only "
                    "runs when there is a frame to show, so the producer paces it.");
            s.auto_on         = (unsigned)ini_read_sr_int("Auto", 0, 0, 1);
            s.auto_target_fps = (unsigned)ini_read_sr_int("AutoTargetFps", 60, 20, 240);
            s.auto_budget_ms  = 1000.0 / (double)s.auto_target_fps;
            s.sr_preset    = ini_read_sr_int("SRPreset", 0, 0, 20);
            s.sr_quality   = ini_read_sr_int("SRQuality", 1, 0, 5);
            s.sr_scale_pct = (unsigned)ini_read_sr_int("SRScale", 0, 0, 99);

            if (s.sr_on != 0u && s.passes != 1u)
            {
                mgpu::diag::error("[MGPU][C2-SR] SRUpscale=1 REFUSES Passes above 1. The "
                                  "intermediate passes ping-pong in the display-extent pair "
                                  "while the first and last work at the render extent, which "
                                  "would mix two resolutions in one chain. SRUpscale off.");
                s.sr_on = 0u;
            }
        }
    }
    s.pass_reset   = ini_read_pass_reset();
    {
        static const char *const pr[3] = {
            "0 - every pass keeps its history (the shipped behaviour)",
            "1 - passes 2..N reset EVERY frame: the cascade is broken and later passes become "
            "stateless refinement of pass 1",
            "2 - EVERY pass resets every frame: NO temporal history anywhere. This is the control "
            "for the whole hypothesis - if the artifact largely goes here, it IS the model's own "
            "misaligned history and no flow estimator is needed to establish that" };
        char pl[560];
        snprintf(pl, sizeof pl, "[MGPU][P8.1] PassReset=%s", pr[(s.pass_reset < 0 || s.pass_reset > 2) ? 0 : s.pass_reset]);
        mgpu::diag::info(pl);
    }
    if (s.copy_queue)
    {
        mgpu::diag::warn(
            s.cq_gated
            ? "[MGPU][R28] CopyQueue=2 - GPU-GATED. The unpack is on a dedicated COPY queue AND "
              "that queue waits on the GAME's fence ON THE GPU for a frame that has not arrived "
              "yet, so the copy fires the instant the payload lands rather than when the bridge "
              "thread next wakes and asks. This is the overlap P2.1 named and did not test: "
              "transfer against neural execution on GPU 1. NOTHING ABOUT PRESENTATION CHANGES - "
              "the evaluate is still CPU-gated, one frame at a time, presented on completion, so "
              "R25a's single clock is untouched. NOT THE CONTROL: every published figure was "
              "measured at CopyQueue=0. Watch cq_spec (frames armed before arrival - should be "
              "most of them) and OVERRUN, which has a one-frame-wider window in this mode."
            : "[MGPU][R26] CopyQueue=1 - SPLIT ONLY. The cross-adapter unpack moves off GPU 1's "
              "DIRECT queue onto a dedicated COPY queue, and tex_in becomes a PAIR (an extra "
              "~14.7 MB on GPU 1). Arrival is still tested ON THE CPU, so a copy is only ever "
              "issued for a frame already known to have landed and the overlap happens ONLY when "
              "there is a backlog. In a perfectly paced run cq_ahead will be 0 and nothing will "
              "overlap - that is the expected result of this mode, not a fault. CopyQueue=2 is "
              "the one that overlaps in the steady state. NOT THE CONTROL: every published "
              "figure was measured at CopyQueue=0.");
    }
    {
        char ml[760];
        snprintf(ml, sizeof ml,
                 "[MGPU][P8.0] MVec=%s. THIS IS A CONTROL, NOT A FEATURE: mode 1 binds a CONSTANT "
                 "field so two runs differ in exactly one thing - whether a NON-ZERO DLSSNR.MVec "
                 "is bound. Identical output means NR does not read MVec on this path and no flow "
                 "estimator is worth writing. Different output means it reads it, and the sign and "
                 "scale can then be walked live instead of guessed. Compare the P2.2 mark 2->3 "
                 "across the two runs for what it costs.",
                 (s.mvec_mode == 0) ? "OFF (control arm - byte-identical to every published run)"
                                    : "SYNTHETIC");
        mgpu::diag::info(ml);
        if (s.mvec_mode != 0)
        {
            snprintf(ml, sizeof ml,
                     "[MGPU][P8.0] synthetic flow dx=%.3f dy=%.3f scaleX=%.3f scaleY=%.3f "
                     "(R32G32_FLOAT, full frame). A field of exactly 0,0 carries the same "
                     "information as no field at all, so MVec=1 with dx=dy=0 is a wasted run.",
                     s.mvec_dx, s.mvec_dy, s.mvec_scale_x, s.mvec_scale_y);
            mgpu::diag::info(ml);
        }
    }
    s.preset = ini_read_preset();
    s.window_mode = ini_read_window_mode();
    {
        char buf[INI_BYTES];
        const bool have = ini_slurp(buf, sizeof buf);
        const char *k = have ? ini_find(buf, "Passes") : nullptr;
        const long long asked = (k != nullptr) ? atoll(k) : 1;
        s.passes = ini_read_passes();
        if (k != nullptr && asked != (long long)s.passes)
        {
            char pl[400];
            snprintf(pl, sizeof pl,
                     "[MGPU][P6.0] mgpu.ini asks for Passes=%lld, which this build CLAMPS to %u "
                     "(valid range 1..%u). The run below is %u passes, not %lld - said here so "
                     "the summary is not read as the experiment that was requested.",
                     asked, s.passes, stream_state::MAX_PASSES, s.passes, asked);
            mgpu::diag::warn(pl);
        }
    }

    // P6.2. Read after `passes`, because the per-pass overrides only make
    // sense once we know how many passes there are.
    {
        const unsigned named = ini_read_intensity(s.intensity, s.intensity_set, s.passes);
        s.set_n = ini_read_sets(s.set_key, s.set_f, s.set_u, s.set_is_float);
        // P7.4. A Preset= in the ini OVERRIDES the per-pass numbers, and says
        // so below rather than leaving two sources of truth to be reconciled
        // by whoever reads the log later. Applied after Passes so the peak
        // lands on the right pass.
        if (s.preset != 0)
        {
            preset_apply_locked(s);
            char pr[420];
            snprintf(pr, sizeof pr,
                     "[MGPU][P7.4] Preset=%s OVERRIDES every Intensity/IntensityN in the ini for "
                     "this run: %s Remove the Preset line to use the per-pass numbers instead. "
                     "The panel can change the shape live and any manual slider returns to manual.",
                     (s.preset == 1) ? "front" : "back",
                     (s.preset == 1)
                         ? "pass 1 at 2.00, every other pass at 0.10."
                         : "the LAST active pass at 2.00, every other pass at 0.10.");
            mgpu::diag::warn(pr);
        }

        char kl[900];
        int w = snprintf(kl, sizeof kl, "[MGPU][P6.2] knobs | intensity");
        for (unsigned i = 0; i < s.passes && w > 0 && (size_t)w < sizeof kl; ++i)
            w += snprintf(kl + w, sizeof kl - (size_t)w, " p%u=%.3f%s",
                          i + 1, s.intensity[i], s.intensity_set[i] ? "*" : "");
        if (w > 0 && (size_t)w < sizeof kl)
            w += snprintf(kl + w, sizeof kl - (size_t)w,
                          " (%u per-pass override%s, * marks them)", named,
                          (named == 1) ? "" : "s");
        for (unsigned i = 0; i < s.set_n && w > 0 && (size_t)w < sizeof kl; ++i)
        {
            if (s.set_is_float[i])
                w += snprintf(kl + w, sizeof kl - (size_t)w, " | %s=%.4f(f)",
                              s.set_key[i], s.set_f[i]);
            else
                w += snprintf(kl + w, sizeof kl - (size_t)w, " | %s=%u(u)",
                              s.set_key[i], s.set_u[i]);
        }
        if (w > 0 && (size_t)w < sizeof kl)
            snprintf(kl + w, sizeof kl - (size_t)w,
                     ". Set.* keys are passed to NGX VERBATIM and this add-on does not "
                     "know whether the snippet reads them - a key listed here was SENT, "
                     "not necessarily HONOURED. Intensity is the only one proven live "
                     "(P1.2: 0.00 vs 1.60 changed 98.03%% of pixels).");
        mgpu::diag::info(kl);
    }

    // DEFECT A, found on the rig 2026-09-04 and fixed here. stream_read_fault
    // accepted ANY string, so a name this build cannot inject - "drop" and
    // "stale" were both tried - silently injected nothing, the run came back
    // with every counter zero, and the fault-injected warning then declared
    // that "the checker did not trip and the instrument is not yet
    // trustworthy". That is a FALSE ACCUSATION AGAINST A WORKING CHECKER,
    // produced by the instrument's own permissiveness. An unknown name is now
    // refused loudly, by name, with the implemented set listed.
    {
        static const char *KNOWN[] = { "none", "pitch", "alias", "magic", "drop", "stale" };
        bool ok = false;
        for (unsigned i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; ++i)
            if (strcmp(s.fault, KNOWN[i]) == 0) { ok = true; break; }
        if (!ok)
        {
            char e[500];
            snprintf(e, sizeof e,
                     "[MGPU][P4.0] mgpu.ini requests Fault=\"%s\", which this build CANNOT "
                     "inject. Implemented: pitch, alias, magic, drop, stale. Running clean "
                     "instead - and the summary will say so, because a run that injects nothing "
                     "and reports all-zero counters would otherwise read as a checker that "
                     "failed to trip. NOT implemented: tear, which needs the barcode shader to "
                     "have anything to check against.", s.fault);
            mgpu::diag::error(e);
            s.fault_unimpl = true;
            snprintf(s.fault, sizeof s.fault, "none");
        }
    }
    {
        // B1. Read at arm and never live: a stride that changed mid-run would
        // put two different transport regimes inside one summary.
        const int sv = ini_read_stride();
        if (sv < 0)
        {
            mgpu::diag::error(
                "[MGPU][B1] Stride= is present but names a value this build does not accept. "
                "Valid values are 0 (off, the default) and 2 to 8, which is the CAP on the "
                "stride rather than the stride itself. FALLING BACK TO 0 AND SAYING SO - a "
                "setting silently not applied is DEFECT D's failure shape. This run seals "
                "every frame.");
        }
        s.stride_max = (sv < 0) ? 0u : (unsigned)sv;
        s.cur_stride = 1;
        if (s.stride_max != 0)
        {
            char sl[900];
            snprintf(sl, sizeof sl,
                     "[MGPU][B1] PRODUCER STRIDE ON, cap %u. The producer now asks the consumer "
                     "how far behind it is - `produced - consumed`, both already under this "
                     "file's own mutex, no fence and no new shared state - and stops sealing "
                     "frames nobody will read. A strided frame is NOT a dropped frame: it is "
                     "never sealed, never copied, and `produced` does not advance for it, so the "
                     "consumer's sequence stays contiguous and its gap check stays quiet. WHAT "
                     "THIS SAVES is paid on the RENDER card and on the link, which is the half "
                     "of the transport this project has never priced. Raising the stride is "
                     "gated on the lead reaching 3 and lowering it on the lead falling to 1, so "
                     "a lead that sits at 2 holds - without that deadband the stride flaps and "
                     "the neural cadence flaps with it.",
                     s.stride_max);
            mgpu::diag::info(sl);
        }
    }

    {
        const int cv = ini_read_costcurve();
        if (cv < 0)
            mgpu::diag::error("[MGPU][CURVE] CostCurve= is present but names a value this build "
                              "does not accept. Valid values are 0 (off, the default) and 1. "
                              "FALLING BACK TO 0 AND SAYING SO.");
        s.cc_on = (cv < 0) ? 0u : (unsigned)cv;
    }

    // D4: resolve the fault selector now, so no frame pays a strcmp for it.
    s.fault_id = stream_state::FK_NONE;
    if      (strcmp(s.fault, "drop")  == 0) s.fault_id = stream_state::FK_DROP;
    else if (strcmp(s.fault, "stale") == 0) s.fault_id = stream_state::FK_STALE;
    else if (strcmp(s.fault, "pitch") == 0) s.fault_id = stream_state::FK_PITCH;
    else if (strcmp(s.fault, "alias") == 0) s.fault_id = stream_state::FK_ALIAS;
    else if (strcmp(s.fault, "magic") == 0) s.fault_id = stream_state::FK_MAGIC;

    char l[1100];
    // R102: full frame is the shipped default and prints nothing extra, so a
    // normal arm line is unchanged from every published one.
    char subr[130];
    if (s.subrect_pct == 100u)
        subr[0] = '\0';
    else
        snprintf(subr, sizeof subr,
                 ", SUBRECT=%u%% per axis (MEASUREMENT RUN - partial output by design)",
                 s.subrect_pct);
    // P7.6: an unbounded run says so in words rather than printing a bound of
    // 18446744073709551615, which reads as a defect.
    char bound[160];
    if (s.max_frames == FRAMES_UNBOUNDED)
        snprintf(bound, sizeof bound,
                 "NO BOUND (Frames=0) - this run does not end and prints no summary. "
                 "SESSIONS BEYOND A FEW MINUTES ARE UNTESTED: watch GPU load and temperature");
    else
        snprintf(bound, sizeof bound, "bound %llu frames", s.max_frames);

    snprintf(l, sizeof l,
             "[MGPU][P4.0] stream REQUESTED - ring depth %u, %s, fault=\"%s\", "
             "neural=%s, profile=%s, present=%s, passes=%u, window=%s%s. "
             "Every game frame from the next one is sealed and transited until the bound is "
             "reached, then a summary is printed. Stay in gameplay: a stream of menu frames "
             "measures identity and ordering correctly and tells you nothing about anything "
             "else.",
             stream_state::RING, bound, s.fault,
             s.neural ? "ON (P4.1 - DLSS-NR runs on every consumed frame)"
                      : "off (P4.0 transport-only control)",
             s.profile ? "ON (no on-screen output, no liveness sample - measurement run)"
                       : "off",
             (s.present_mode == 1)
                 ? "IN (the window shows the frame handed TO DLSS-NR, not its output - "
                   "P5.3 discriminator)"
                 : ((s.present_mode == 2)
                       ? "SPLIT (left half input, right half output, SAME frame)"
                       : "nr (the neural output)"),
             s.passes,
             (s.window_mode == 0) ? "crop (1280x720, the P5 behaviour)"
                                  : ((s.window_mode == 1) ? "match (borderless at the source size)"
                                                          : "fit (borderless full screen)"),
             subr);
    mgpu::diag::info(l);
}

// Game thread, every frame, with the game's command list open.
// L3. THE PRODUCED-COUNT SIGNAL, ONE EVENT EARLIER.
//
// stream_on_finish_effects signals for the frame BEFORE the one it handles,
// because ReShade executes the list we record into AFTER that handler returns.
// The cost is a full frame of the consumer's view: the fence cannot report
// frame N until frame N+1's effects event fires. L1 measured the consequence -
// a backlog pinned at exactly 3 on 1335 of 1500 frames - and L2 showed the
// downstream half is only 10.78 ms, so this frame is a real share of what is
// left.
//
// By the time PRESENT is raised for frame N, ReShade has submitted that frame's
// list. That is the same guarantee the next-frame delay was buying, obtained an
// event earlier instead of a frame later.
//
// IF THAT IS WRONG - if ReShade submits after this event rather than before -
// the consumer reads a slot whose copy has not landed, and the SEAL checker
// fires on the first frame with bad_magic or a contract mismatch. That detector
// is why this is safe to try rather than safe to assume.
void stream_on_present(void *cmd_queue_v)
{
    stream_state &s = str();   // str(), not st() - st() is the capture state
    std::lock_guard<std::mutex> lk(s.cs);

    if (s.signal_at != 1u || !s.armed || s.finished) return;   // armed: stream_state's own
    if (s.gfence == nullptr || s.produced == 0) return;

    ID3D12CommandQueue *gq = reinterpret_cast<ID3D12CommandQueue *>(cmd_queue_v);
    if (gq == nullptr) return;

    (void)gq->Signal(s.gfence, s.produced);
}

// ================= REFLEX: FORCE LOW LATENCY ON GPU 0 =====================
//
// RESTORED IN V37, AND THE REASON IT WAS EVER REMOVED IS WORTH KEEPING.
//
// Reflex was cut in V35 because it looked like the cause of a crash that
// poisoned the NEXT launch of the game. It was not. The cause was that this
// add-on NEVER CLOSED ITS NGX SESSION - NVSDK_NGX_D3D12_Shutdown1 was
// deliberately not called on the reasoning that "one NGX session per launch,
// reclaimed by the OS at process exit, is the intended end state". The OS does
// not reclaim it. V36 calls Shutdown1 after releasing every feature, and the
// second-launch crash went away.
//
// Reflex looked guilty because turning it off appeared to fix things - but
// Reflex=0 meant "do not call", so those runs simply had a different NGX
// history, and because REBUILDING THE ADD-ON ALSO CLEARS THE RESIDUE, every
// first launch after a build was clean whatever the ini said. That combination
// produced a false positive that survived several hours.
//
// The teardown work written while chasing it is KEPT, because it is correct on
// its own terms even though it fixed nothing: we restore the sleep mode we set
// (V33) and pair NvAPI_Initialize with NvAPI_Unload (V34). Leaving a driver
// mode set on a user's device after they close the game is wrong regardless of
// whether it is the thing that crashes.

namespace reflex
{
    // RETURNS void*, NOT int. See cause 4 above.
    typedef void * (*pfn_query)(unsigned int);
    typedef int (*pfn_init)(void);
    typedef int (*pfn_unload)(void);   // V34: NvAPI_Unload, 0xD22BDD7E
    typedef int (*pfn_set)(IUnknown *, void *);
    typedef int (*pfn_get)(IUnknown *, void *);
    typedef int (*pfn_sleep)(IUnknown *);          // device only - no struct
    typedef int (*pfn_lat)(IUnknown *, void *);

    // ---- NV_SET_SLEEP_MODE_PARAMS_V1: 44 BYTES, version 0x0001002C ----
    // NvBool is NvU8. The two bools at offset 4 and 5 are ONE BYTE EACH and
    // the compiler pads to the next dword; writing them as dwords is what
    // produced a 20-byte struct and -9.
    struct set_params
    {
        unsigned int  version;              // 0
        unsigned char low_latency;          // 4   bLowLatencyMode
        unsigned char boost;                // 5   bLowLatencyBoost
        unsigned char pad0[2];              // 6
        unsigned int  min_interval_us;      // 8   0 = no cap of our own
        unsigned char use_markers;          // 12  bUseMarkersToOptimize
        unsigned char use_min_queue_time;   // 13  added R615; zero before that
        unsigned char rsvd[30];             // 14..43  MUST BE ZERO
    };

    // ---- NV_GET_SLEEP_STATUS_PARAMS_V1: 136 BYTES, version 0x00010088 ----
    // Everything after version is an OUTPUT. The driver writes the whole
    // struct, which is why the 12-byte version of this crashed on arm rather
    // than returning an error: it wrote 124 bytes past the end.
    struct get_params
    {
        unsigned int  version;                  // 0   IN
        unsigned char low_latency;              // 4   OUT  <- THE ANSWER
        unsigned char fs_vrr;                   // 5   OUT
        unsigned char cpl_vsync_on;             // 6   OUT
        unsigned char pad0;                     // 7
        unsigned int  sleep_interval_us;        // 8   OUT
        unsigned char use_game_sleep;           // 12  OUT
        unsigned char fullscreen_iflip;         // 13  OUT
        unsigned char fg_multiplier;            // 14  OUT
        unsigned char dfg_control;              // 15  OUT
        unsigned int  dfg_frame_time_target_us; // 16  OUT
        unsigned char rsvd[114];                // 20..133  (padded to 136)
    };

    // ---- NV_LATENCY_RESULT_PARAMS_V1: 15400 BYTES, version 0x00013C28 ----
    // 64 frame reports, newest last. crossAdapterCopyTimeUs is in here, and
    // that is the one field in this whole file that speaks directly to 6i:
    // the driver's own measurement of the transport this bridge is built on.
    // It is read-only and it is behind its own ini key because a 15 KB output
    // struct from an ordinal-resolved function is not something to put in the
    // arm path of a feature we are still proving.
    struct frame_report
    {
        unsigned long long frame_id;                  // 0
        unsigned long long input_sample_time;         // 8
        unsigned long long sim_start;                 // 16
        unsigned long long sim_end;                   // 24
        unsigned long long render_submit_start;       // 32
        unsigned long long render_submit_end;         // 40
        unsigned long long present_start;             // 48
        unsigned long long present_end;               // 56
        unsigned long long driver_start;              // 64
        unsigned long long driver_end;                // 72
        unsigned long long os_render_queue_start;     // 80
        unsigned long long os_render_queue_end;       // 88
        unsigned long long gpu_render_start;          // 96
        unsigned long long gpu_render_end;            // 104
        unsigned int       gpu_active_render_time_us; // 112
        unsigned int       gpu_frame_time_us;         // 116
        unsigned long long camera_constructed_time;   // 120
        unsigned int       cross_adapter_copy_time_us;// 128  <- 6i
        unsigned int       ai_frame_time_us;          // 132
        unsigned char      rsvd[104];                 // 136..239
    };
    struct latency_params
    {
        unsigned int version;         // 0
        // 4 bytes of padding here - frame_report is 8-byte aligned
        frame_report frames[64];      // 8
        unsigned char rsvd[32];       // 15368..15399
    };

    // IF ANY OF THESE FAIL THE BUILD, DO NOT "FIX" IT BY CHANGING THE NUMBER.
    // The number is the driver's contract. A failure here means a field type
    // above is wrong, and shipping it would reproduce crash cause 1.
    static_assert(sizeof(set_params) == 44,
                  "NV_SET_SLEEP_MODE_PARAMS_V1 must be 44 bytes - NvBool is 1 byte");
    static_assert(sizeof(get_params) == 136,
                  "NV_GET_SLEEP_STATUS_PARAMS_V1 must be 136 bytes");
    static_assert(sizeof(frame_report) == 240, "NV frame report must be 240 bytes");
    static_assert(sizeof(latency_params) == 15400,
                  "NV_LATENCY_RESULT_PARAMS_V1 must be 15400 bytes");

    // MAKE_NVAPI_VERSION(T, n) = sizeof(T) | (n << 16). Derived from the
    // struct, never typed in, so the static_asserts above guard the version
    // number too.
    static unsigned ver(size_t sz, unsigned n) { return (unsigned)sz | (n << 16); }

    static bool done = false;
    static int  was = -1, now = -1;
    static bool applied = false;

    // V33. THE DEVICE WE SET THE MODE ON, AND THE FUNCTION TO UNSET IT.
    //
    // THIS IS THE BUG THAT COST AN ENTIRE EVENING. SetSleepMode is a PER-DEVICE
    // DRIVER MODE. We set it and never cleared it, so it outlived the process -
    // and the next launch of the game arrived with a low-latency mode already
    // applied that nothing in the new process knew about. The second launch
    // then died at NGX CreateFeature.
    //
    // IT HID BECAUSE EVERY REBUILD MASKED IT. Replacing the .addon64 is the
    // same operation as renaming it to .bak and back, which was the discovered
    // clearing procedure - so the first launch after ANY build was always
    // clean, every fix looked like it worked, and the crash on the second
    // launch got blamed on whatever had changed most recently. Five "clean
    // runs" on V25 were five first-launches on five fresh binaries.
    //
    // V21 IS WHERE IT STARTED and the reason is exact: V21 is the first build
    // in which SetSleepMode ever returned 0. V15 through V20 all returned -9
    // and set nothing, so there was nothing to leave behind.
    //
    // CONFIRMED 2026-09-12 by the first real control ever run: Reflex=0 from a
    // cleared machine, relaunched, no crash and no cleaning needed. Every
    // earlier "Reflex=0 crashed too" result was void - Reflex=0 meant "do not
    // call", so those runs inherited the mode from the previous Reflex=1 run.
    static IUnknown *dev_held = nullptr;
    static pfn_set   p_set_held = nullptr;
    static pfn_unload p_unload_held = nullptr;

    // ---- V43: UNDO A PREVIOUS PROCESS'S WRITE ----
    //
    // restore() handles the launch that set the mode. This handles the launch
    // AFTER one that set the mode and then died, which restore() by
    // construction cannot: it runs in a different process, with no `applied`,
    // no held device, and nothing in memory that remembers anything.
    //
    // It resolves its own pointers rather than reusing engage()'s, because
    // engage() may never run this launch - Reflex=0 is exactly the case this
    // exists for. Ordinals are the same confirmed ones engage() uses.
    //
    // Deliberately NOT gated on the Reflex setting. The key records what the
    // last run DID, which is the only thing that determines whether the
    // driver is currently holding a mode we put there.
    void clear_residue(ID3D12Device *dev)
    {
        if (dev == nullptr) return;
        if (ini_mirror_get("ReflexApplied", 0) == 0) return;

        mgpu::diag::warn("[MGPU][RFX] ReflexApplied=1 from a previous launch - that launch set "
                         "low latency on GPU 0 and did not exit cleanly, so the mode is still "
                         "set in the driver. Putting it back to OFF now.");

        HMODULE m = GetModuleHandleW(L"nvapi64.dll");
        if (m == nullptr) m = LoadLibraryW(L"nvapi64.dll");
        if (m == nullptr)
        {
            mgpu::diag::warn("[MGPU][RFX] residue clear: nvapi64.dll not present. The key is "
                             "left set so a later launch can try again.");
            return;
        }
        pfn_query q = (pfn_query)GetProcAddress(m, "nvapi_QueryInterface");
        if (q == nullptr)
        {
            mgpu::diag::warn("[MGPU][RFX] residue clear: nvapi_QueryInterface not exported. Key "
                             "left set.");
            return;
        }

        pfn_init ci = (pfn_init)(void *)q(0x0150E828u);   // NvAPI_Initialize
        pfn_set  cs = (pfn_set) (void *)q(0xAC1CA9E0u);   // NvAPI_D3D_SetSleepMode
        if (ci == nullptr || cs == nullptr)
        {
            mgpu::diag::warn("[MGPU][RFX] residue clear: could not resolve Initialize or "
                             "SetSleepMode. Key left set.");
            return;
        }

        mgpu::diag::info("[MGPU][RFX] residue clear: NvAPI_Initialize then SetSleepMode(0) - IF "
                         "THIS IS THE LAST LINE, IT DIED IN NvAPI");
        (void)ci();

        set_params sp;
        memset(&sp, 0, sizeof sp);
        sp.version     = ver(sizeof sp, 1u);
        sp.low_latency = 0u;
        sp.boost       = 0u;
        const int rr = cs(dev, &sp);

        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][RFX] residue clear: SetSleepMode(lowLatency=0) -> %d. 0 means the "
                 "driver took it and the residue is gone.", rr);
        if (rr == 0)
        {
            mgpu::diag::info(l);
            (void)ui_ini_write("ReflexApplied", 0);
        }
        else
        {
            mgpu::diag::warn(l);
            mgpu::diag::warn("[MGPU][RFX] residue clear FAILED - the key stays set so the next "
                             "launch tries again rather than leaving the mode on forever.");
        }
    }

    // Called from the ordered teardown. Undoes exactly what engage() did and
    // nothing else: if we turned the mode on, we turn it off. A title that
    // drives Reflex itself is untouched, because in that case we never set it.
    void restore()
    {
        if (!applied || p_set_held == nullptr || dev_held == nullptr)
        {
            if (dev_held != nullptr) { dev_held->Release(); dev_held = nullptr; }
            return;
        }

        mgpu::diag::info("[MGPU][RFX] restoring low latency to OFF on GPU 0 - IF THIS IS THE "
                         "LAST LINE, IT DIED IN NvAPI");

        set_params sp;
        memset(&sp, 0, sizeof sp);
        sp.version     = ver(sizeof sp, 1u);
        sp.low_latency = 0u;
        sp.boost       = 0u;
        const int rr = p_set_held(dev_held, &sp);

        char l[420];
        snprintf(l, sizeof l,
                 "[MGPU][RFX] SetSleepMode(lowLatency=0) -> %d. THIS IS THE FIX FOR THE "
                 "SECOND-LAUNCH CRASH: the mode is per-device and survives the process, so "
                 "leaving it set poisoned the next launch. NvAPI_Unload is deliberately NOT "
                 "called - its ordinal is not confirmed and this file does not call ordinals it "
                 "has not verified.", rr);
        if (rr == 0) mgpu::diag::info(l); else mgpu::diag::warn(l);

        // V43. The breadcrumb comes off only once the driver has taken the
        // restore. If SetSleepMode(0) failed, the mode is still set and the
        // key must stay so the next launch tries again.
        if (rr == 0) (void)ui_ini_write("ReflexApplied", 0);

        applied = false;
        dev_held->Release();
        dev_held = nullptr;
        p_set_held = nullptr;

        // ---- V34: NvAPI_Unload, 0xD22BDD7E ----
        //
        // ORDINAL CONFIRMED against three independent copies of NVIDIA's own
        // nvapi_interface.h, so it passes the rule about never calling an
        // ordinal we have not verified.
        //
        // WHAT THE HEADER ACTUALLY SAYS, because it sets expectations:
        //   - Unload "decrements the ref-counter and when it reaches ZERO,
        //     unloads NVAPI library. This must be called in pairs with
        //     NvAPI_Initialize." Optional, but recommended in pairs. We have
        //     never called it, so our Initialize has been unpaired since V21.
        //   - It can fail: NVAPI_API_IN_USE, or NVAPI_ERROR when a resource is
        //     still locked. Both are reported below rather than ignored.
        //   - NOTHING in NVIDIA's documentation says Unload resets device
        //     state, and NVIDIA'S OWN STREAMLINE CALLS NEITHER SetSleepMode(0)
        //     NOR Unload AT SHUTDOWN. So this is not obviously the fix - it is
        //     the one remaining thing we do that has no counterpart, and it is
        //     cheap to pair correctly. If the residue survives this too, that
        //     is a real finding and it points away from NvAPI entirely.
        if (p_unload_held != nullptr)
        {
            mgpu::diag::info("[MGPU][RFX] calling NvAPI_Unload to pair the Initialize we have "
                             "never paired - IF THIS IS THE LAST LINE, IT DIED IN NvAPI");
            const int ur = p_unload_held();
            char ul[340];
            snprintf(ul, sizeof ul,
                     "[MGPU][RFX] NvAPI_Unload -> %d. 0 is success; -38 is API_IN_USE and means "
                     "something else in this process still holds NvAPI, which is not our bug to "
                     "fix and is not fatal.", ur);
            if (ur == 0) mgpu::diag::info(ul); else mgpu::diag::warn(ul);
            p_unload_held = nullptr;
        }
    }

    // Per-frame state. Only touched on the game's render thread.
    static pfn_sleep      p_sleep  = nullptr;
    static pfn_lat        p_lat    = nullptr;
    static unsigned       sleep_on = 0;
    static unsigned       lat_on   = 0;
    static latency_params *lat_buf = nullptr;
    static unsigned       lat_logs = 0;
    static unsigned long long frames_seen = 0;

    // The call site uses this to decide whether to keep resolving the device
    // every frame. With Reflex set and neither per-frame call armed, there is
    // nothing to do after arm and the per-frame path stays cold.
    static bool needs_per_frame() { return sleep_on != 0u || lat_on != 0u; }

    // Reads the sleep status and returns the low-latency bit, or -1 if the
    // call never succeeded. n is swept 1..3 because NVIDIA may bump the
    // version NUMBER while the size stays put - but the SIZE is not swept any
    // more, it comes from sizeof.
    static int read_status(pfn_get g, IUnknown *dev, const char *when)
    {
        if (g == nullptr) return -1;
        char l[600];
        for (unsigned n = 1u; n <= 3u; ++n)
        {
            get_params gp;
            memset(&gp, 0, sizeof gp);
            gp.version = ver(sizeof gp, n);
            const int r = g(dev, &gp);
            if (r == 0)
            {
                snprintf(l, sizeof l,
                         "[MGPU][RFX] GetSleepStatus %s (REPORTED STATE - not proof of what "
                         "was set; see [MGPU][L1]): ver=%u size=%u -> 0 | low latency %s, "
                         "interval=%u us, fs vrr=%u, cpl vsync=%u, game sleep=%u, "
                         "fullscreen iflip=%u, fg multiplier=%u",
                         when, n, (unsigned)sizeof gp,
                         gp.low_latency ? "ON" : "OFF", gp.sleep_interval_us,
                         (unsigned)gp.fs_vrr, (unsigned)gp.cpl_vsync_on,
                         (unsigned)gp.use_game_sleep, (unsigned)gp.fullscreen_iflip,
                         (unsigned)gp.fg_multiplier);
                mgpu::diag::info(l);
                return gp.low_latency ? 1 : 0;
            }
            snprintf(l, sizeof l, "[MGPU][RFX]   GetSleepStatus %s ver=%u size=%u -> %d",
                     when, n, (unsigned)sizeof gp, r);
            mgpu::diag::info(l);
        }
        return -1;
    }

    // For the panel. Written once by engage() on the game's render thread and
    // read from the presenting thread; three scalars, no tearing that matters.
    void status(int &was_out, int &now_out, bool &applied_out)
    {
        was_out = was; now_out = now; applied_out = applied;
    }

    static void engage(IUnknown *dev, unsigned mode, unsigned want_sleep, unsigned want_lat)
    {
        if (done) return;
        done = true;
        char l[1400];

        mgpu::diag::info("[MGPU][RFX] step 1/7: loading nvapi64.dll");
        HMODULE m = GetModuleHandleW(L"nvapi64.dll");
        if (m == nullptr) m = LoadLibraryW(L"nvapi64.dll");
        if (m == nullptr)
        {
            mgpu::diag::warn("[MGPU][RFX] nvapi64.dll not present. Not an NVIDIA driver, or "
                             "not installed. Reflex control unavailable; the bridge runs "
                             "exactly as it would without this feature.");
            return;
        }

        mgpu::diag::info("[MGPU][RFX] step 2/7: nvapi_QueryInterface");
        pfn_query q = (pfn_query)GetProcAddress(m, "nvapi_QueryInterface");
        if (q == nullptr)
        { mgpu::diag::warn("[MGPU][RFX] nvapi_QueryInterface not exported."); return; }

        // ORDINALS, all confirmed against NVIDIA/nvapi nvapi_interface.h.
        // These are NOT guesses and the earlier suspicion of them was wrong -
        // see cause 2 in the block above before doubting them again.
        pfn_init  p_init = (pfn_init) (void *)q(0x0150E828u);   // NvAPI_Initialize
        pfn_set   p_set  = (pfn_set)  (void *)q(0xAC1CA9E0u);   // NvAPI_D3D_SetSleepMode
        pfn_get   p_get  = (pfn_get)  (void *)q(0xAEF96CA1u);   // NvAPI_D3D_GetSleepStatus
        pfn_sleep p_slp  = (pfn_sleep)(void *)q(0x852CD1D2u);   // NvAPI_D3D_Sleep
        pfn_lat   p_lt   = (pfn_lat)  (void *)q(0x1A587F9Cu);   // NvAPI_D3D_GetLatency
        snprintf(l, sizeof l,
                 "[MGPU][RFX] step 3/7: resolved Initialize=%p SetSleepMode=%p "
                 "GetSleepStatus=%p Sleep=%p GetLatency=%p. THESE ARE ORDINALS AND CANNOT BE "
                 "VALIDATED - a wrong id returns a pointer to a DIFFERENT function and calling "
                 "it crashes with no return code. If this is the last line in the log, one of "
                 "them was wrong.",
                 (void *)p_init, (void *)p_set, (void *)p_get, (void *)p_slp, (void *)p_lt);
        mgpu::diag::info(l);
        if (p_init == nullptr || p_set == nullptr) return;

        mgpu::diag::info("[MGPU][RFX] step 4/7: NvAPI_Initialize");
        const int ir = p_init();
        if (ir != 0)
        { snprintf(l, sizeof l, "[MGPU][RFX] NvAPI_Initialize returned %d - stopping here.", ir);
          mgpu::diag::warn(l); return; }

        // ---- step 5: READ BEFORE WRITING. THIS IS THE EVIDENCE. ----
        // The claim being tested is "the game says Reflex is off and we turn it
        // on anyway". Without this read there is no before, and applied=1 on
        // its own would be indistinguishable from the title having had it on.
        mgpu::diag::info("[MGPU][RFX] step 5/7: NvAPI_D3D_GetSleepStatus (BEFORE)");
        was = read_status(p_get, dev, "BEFORE");

        // ---- step 6: WRITE ----
        mgpu::diag::info("[MGPU][RFX] step 6/7: NvAPI_D3D_SetSleepMode");
        int sr = -9;
        unsigned used_n = 0;
        for (unsigned n = 1u; n <= 3u; ++n)
        {
            set_params sp;
            memset(&sp, 0, sizeof sp);
            sp.version         = ver(sizeof sp, n);
            sp.low_latency     = (mode != 0u) ? 1u : 0u;
            sp.boost           = (mode == 2u) ? 1u : 0u;   // Reflex=2 is on+boost
            sp.min_interval_us = 0u;                       // no frame rate cap of our own
            sp.use_markers     = 0u;                       // we set no latency markers
            sr = p_set(dev, &sp);
            snprintf(l, sizeof l,
                     "[MGPU][RFX]   SetSleepMode ver=%u size=%u lowLatency=%u boost=%u -> %d%s",
                     n, (unsigned)sizeof sp, (unsigned)sp.low_latency, (unsigned)sp.boost, sr,
                     (sr == 0) ? "  <-- ACCEPTED" : "");
            mgpu::diag::info(l);
            if (sr != -9) { used_n = n; break; }
        }
        if (sr == -9)
            mgpu::diag::warn(
                "[MGPU][RFX] SetSleepMode returned -9 (INCOMPATIBLE_STRUCT_VERSION) at size 44 "
                "for versions 1-3. The size is taken from sizeof and the layout is NVIDIA's "
                "own, so DO NOT go back to sweeping sizes - that is how the last four builds "
                "were spent. Either this driver wants a version above 3, or the struct gained "
                "a field. Check nvapi.h against the driver branch in the ReShade log header.");

        // ---- step 7: READ AGAIN ----
        mgpu::diag::info("[MGPU][RFX] step 7/7: NvAPI_D3D_GetSleepStatus (AFTER)");
        now = read_status(p_get, dev, "AFTER");

        applied = (sr == 0);

        // ---- V43: THE BREADCRUMB. THIS IS WHAT MAKES restore() SURVIVE ----
        //
        // restore() only ever runs from stream_shutdown(), and all three of
        // its call sites are clean-exit paths. A launch that dies - and this
        // one dies often - leaves lowLatency=1 set on GPU 0, where it is a
        // PER-DEVICE DRIVER MODE that outlives the process.
        //
        // Worse, and this is the part that made it a trap: restore() is gated
        // on `applied`, which is only true when Reflex=1 actually engaged. So
        // a user who crashes with Reflex=1 and then sets Reflex=0 to be safe
        // has made the residue PERMANENT - the off position cannot undo the
        // on position.
        //
        // The key below is written the moment the driver accepts our write,
        // and cleared in restore(). It is checked at startup by
        // reflex_clear_residue(), which does not care what Reflex is set to.
        // It cares what the last run actually did.
        if (applied)
        {
            mgpu::diag::info("[MGPU][RFX] writing ReflexApplied=1. If this launch does not exit "
                             "cleanly, the next one reads this key and puts low latency back to "
                             "OFF before anything else - which is the only way a mode that "
                             "outlives the process can be undone after a crash.");
            (void)ui_ini_write("ReflexApplied", 1);
        }

        // V33. Hold a reference so the teardown can undo this on the SAME
        // device. AddRef because the game owns it and it may be released
        // before our teardown runs; matched by the Release in restore().
        if (applied)
        {
            dev_held = dev;
            dev_held->AddRef();
            p_set_held = p_set;
            p_unload_held = (pfn_unload)(void *)q(0xD22BDD7Eu);   // NvAPI_Unload
        }

        // Per-frame calls are armed only if the mode actually took. Sleep into
        // a device whose sleep mode was rejected is a per-frame call for
        // nothing, on the render thread.
        if (applied)
        {
            if (want_sleep != 0u && p_slp != nullptr) { p_sleep = p_slp; sleep_on = 1u; }
            if (want_lat != 0u && p_lt != nullptr)
            {
                lat_buf = (latency_params *)calloc(1, sizeof(latency_params));
                if (lat_buf != nullptr) { p_lat = p_lt; lat_on = 1u; }
            }
        }

        snprintf(l, sizeof l,
                 "[MGPU][RFX] DONE. was=%s SetSleepMode=%d (ver %u) now=%s | Sleep %s, "
                 "GetLatency %s. This is a per-device DRIVER mode - the title's settings file "
                 "does not control it and cannot revert it. || DO NOT READ now= AS THE VERDICT. "
                 "A SetSleepMode of 0 with now=OFF is the EXPECTED shape on a title that does "
                 "not drive Reflex itself, and it was measured with the queue collapsing to the "
                 "floor at the same time. THE VERDICT IS [MGPU][L1]: a backlog near 1.00 instead "
                 "of 1.9, which is the signature the game's own menu produces, together with a "
                 "frame rate about 15%% lower - Reflex costs that, and a run that gets FASTER is "
                 "an instrument problem rather than a win. was= is still worth reading: was=ON "
                 "means the title already had it and the run says nothing about the mechanism.",
                 (was < 0) ? "unknown" : (was ? "ON" : "OFF"), sr, used_n,
                 (now < 0) ? "unknown" : (now ? "ON" : "OFF"),
                 sleep_on ? "ON (per frame)" : "off",
                 lat_on ? "ON (sampled)" : "off");
        if (applied && now != 0) mgpu::diag::info(l); else mgpu::diag::warn(l);
    }

    // Game render thread, once per frame, ONLY for the game's own swapchain.
    // Both calls are optional and both are off by default.
    static void per_frame(IUnknown *dev)
    {
        ++frames_seen;

        // NvAPI_D3D_Sleep takes THE DEVICE ONLY - no struct, no version. It is
        // the pacing half of Reflex: it blocks the calling thread so the game
        // submits later, which is what actually shortens the queue on a title
        // that never calls it. SetSleepMode alone only permits that.
        if (sleep_on != 0u && p_sleep != nullptr) (void)p_sleep(dev);

        // Sampled, not every frame, and logged at most 3 times. The buffer is
        // 15 KB of driver-written output; reading it often costs more than the
        // number is worth mid-run.
        if (lat_on != 0u && p_lat != nullptr && lat_buf != nullptr && lat_logs < 3u
            && frames_seen % 600ull == 0ull)
        {
            memset(lat_buf, 0, sizeof(latency_params));
            lat_buf->version = ver(sizeof(latency_params), 1u);
            const int r = p_lat(dev, lat_buf);
            char l[1200];
            if (r != 0)
            {
                snprintf(l, sizeof l,
                         "[MGPU][RFX-LAT] GetLatency -> %d at frame %llu. Read-only and "
                         "non-fatal; nothing else in the run depends on it.",
                         r, frames_seen);
                mgpu::diag::info(l);
                lat_logs = 3u;   // do not keep asking
            }
            else
            {
                // Newest report last. A zero frame_id means the driver filled
                // fewer than 64 - walk back to the last non-zero one.
                int idx = 63;
                while (idx > 0 && lat_buf->frames[idx].frame_id == 0ull) --idx;
                const frame_report &f = lat_buf->frames[idx];
                const double sim_to_present =
                    (f.present_end > f.sim_start)
                        ? (double)(f.present_end - f.sim_start) / 1000.0 : 0.0;
                const double input_to_present =
                    (f.present_end > f.input_sample_time)
                        ? (double)(f.present_end - f.input_sample_time) / 1000.0 : 0.0;
                snprintf(l, sizeof l,
                         "[MGPU][RFX-LAT] frame %llu, driver report [%d] id=%llu | "
                         "input->present end %.2f ms | sim start->present end %.2f ms | "
                         "gpu frame %u us, gpu active render %u us | CROSS ADAPTER COPY %u us | "
                         "ai frame %u us. THE CROSS ADAPTER FIGURE IS THE DRIVER'S OWN "
                         "MEASUREMENT OF THE TRANSPORT THIS BRIDGE IS BUILT ON - compare it "
                         "with [MGPU][P2.2] seal copy, which measures the same move from our "
                         "side on GPU 1's queue. These are DRIVER timestamps on GPU 0 and they "
                         "cover the game's frame, not ours: input->present end does NOT include "
                         "GPU 1's evaluate or the bridge's present.",
                         frames_seen, idx, f.frame_id, input_to_present, sim_to_present,
                         f.gpu_frame_time_us, f.gpu_active_render_time_us,
                         f.cross_adapter_copy_time_us, f.ai_frame_time_us);
                mgpu::diag::info(l);
                ++lat_logs;
            }
        }
    }
}

void stream_on_finish_effects(void *cmd_list_v, void *cmd_queue_v,
                              unsigned long long rtv_handle,
                              unsigned long long depth_handle,
                              unsigned long long mvec_handle)
{
    // ---- REFLEX, BEFORE ANY LOCK, ON THE GAME'S OWN RENDER THREAD ----
    //
    // The first attempt put this in stream_on_present and it crashed on arm.
    // Two reasons, both about placement rather than about NvAPI:
    //
    //  1. IT HELD s.cs ACROSS LoadLibrary AND NvAPI_Initialize. Those take the
    //     loader lock and call into the driver. This file warns twice that its
    //     mutex must not be held across anything that can re-enter, and arming
    //     concurrently is exactly when that turns into a crash.
    //
    //  2. THE PRESENT EVENT FIRES FOR EVERY SWAPCHAIN. Our bridge window
    //     presents too, from the bridge thread, which already holds the present
    //     chain's lock - so the device could be GPU 1's and the lock order was
    //     inverted. Reflex has to be set on GPU 0, and only GPU 0.
    //
    // Here both are settled by construction: this handler IS the game's render
    // thread and cmd_queue_v IS the game's queue, it runs every frame whether
    // or not the stream is armed, and the work happens before the mutex is
    // taken. The local static makes it exactly once per process.
    {
        static bool  rfx_once = false;
        static void *rfx_game_queue = nullptr;

        ID3D12CommandQueue *rq = reinterpret_cast<ID3D12CommandQueue *>(cmd_queue_v);

        // Once armed, only the queue we armed on gets looked at again - and if
        // nothing per-frame is armed, rfx_game_queue stays null and this whole
        // block goes cold after the first match.
        if (rq != nullptr && (!rfx_once
                              || (rfx_game_queue != nullptr && cmd_queue_v == rfx_game_queue)))
        {
            bool is_game = (rfx_game_queue != nullptr && cmd_queue_v == rfx_game_queue);
            ID3D12Device *rdev = nullptr;
            if (SUCCEEDED(rq->GetDevice(IID_PPV_ARGS(&rdev))) && rdev != nullptr)
            {
                if (!is_game)
                {
                    // THE GATE THAT WAS MISSING. Compare adapters, not order of
                    // arrival. st().cs is taken and released here - it must NOT
                    // be held across engage(), which loads a library and calls
                    // into the driver.
                    const LUID rl = rdev->GetAdapterLuid();
                    LUID want{}; bool known = false;
                    {
                        std::lock_guard<std::mutex> g(st().cs);
                        want = st().game_luid; known = st().game_luid_known;
                    }
                    is_game = known
                              && rl.LowPart  == want.LowPart
                              && rl.HighPart == want.HighPart;
                }

                if (is_game)
                {
                    if (!rfx_once)
                    {
                        rfx_once = true;

                        // ---- V43: CLEAR A PREVIOUS RUN'S RESIDUE FIRST ----
                        //
                        // BEFORE reading Reflex, and it runs whatever Reflex
                        // says. This is the fix for the trap: the mode is per
                        // device and outlives the process, so a crash leaves
                        // it set, and setting Reflex=0 afterwards cannot undo
                        // it because engage() never ran and restore() returns
                        // early on !applied. The ONLY record that survives is
                        // the ReflexApplied key, and this is where it is
                        // spent. rdev is LUID-gated to GPU 0 above, which is
                        // the device the mode was set on.
                        reflex::clear_residue(rdev);

                        const int rm  = ini_read_sr_int("Reflex", 0, 0, 2);
                        const int rs  = ini_read_sr_int("ReflexSleep", 0, 0, 1);
                        const int rlt = ini_read_sr_int("ReflexLatency", 0, 0, 1);
                        if (rm != 0)
                        {
                            reflex::engage(rdev, (unsigned)rm, (unsigned)rs, (unsigned)rlt);
                            if (reflex::needs_per_frame()) rfx_game_queue = cmd_queue_v;
                        }
                    }
                    else
                    {
                        reflex::per_frame(rdev);
                    }
                }
                rdev->Release();
            }
        }
    }

    stream_state &s = str();
    std::lock_guard<std::mutex> lk(s.cs);
    if (!s.requested || s.finished) return;

    ID3D12GraphicsCommandList *gl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list_v);
    ID3D12Resource *src = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)rtv_handle);
    ID3D12CommandQueue *gq = reinterpret_cast<ID3D12CommandQueue *>(cmd_queue_v);
    if (gl == nullptr || src == nullptr || gq == nullptr) return;

    // FILTER FIRST, ALWAYS. The bridge's own effect runtime raises this event
    // too, and by LUID rather than by pointer: ReShade wraps D3D12 objects, so
    // a pointer comparison rejects every event including the right ones.
    ID3D12Device *ld = nullptr;
    if (FAILED(gl->GetDevice(IID_PPV_ARGS(&ld))) || ld == nullptr) return;
    const LUID ll = ld->GetAdapterLuid();
    ld->Release();
    {
        LUID want{}; bool known = false;
        {
            std::lock_guard<std::mutex> g(st().cs);
            want = st().game_luid; known = st().game_luid_known;
        }
        if (!known || ll.LowPart != want.LowPart || ll.HighPart != want.HighPart)
        {
            if (!s.said_other)
            {
                s.said_other = true;
                mgpu::diag::info("[MGPU][P4.0] ignoring events from the bridge's own runtime "
                                 "(correct - said once)");
            }
            return;
        }
    }

    char line[900];

    // ---- arm on the first game-adapter event, record nothing ----
    if (!s.armed)
    {
        if (s.tried) return;

        // R61/R63: THE ARM WAITS FOR DEPTH.
        //
        // The slot is sized ONCE, here, from the source resource. The depth
        // region cannot be sized before ReShade has bound a depth buffer, and
        // R54 measured that taking 20 seconds of real scene, R56 thirty-five.
        // So when Depth is on, the whole arm holds.
        //
        // s.tried is deliberately left FALSE so the next frame tries again.
        // This is the only path in this function that returns without
        // consuming the attempt, and it is why the check sits here rather
        // than after it.
        if (s.depth_mode != 0 && depth_handle == 0)
        {
            ++s.depth_arm_waits;
            s.hold_lane = 1;                      // R146: live, this frame
            if (!s.depth_arm_logged)
            {
                s.depth_arm_logged = true;
                mgpu::diag::info(
                    "[MGPU][R63] stream arm HELD: Depth is on and ReShade has not bound a depth "
                    "buffer yet, so the slot cannot be sized. This clears itself the moment a "
                    "scene is on screen - R54 saw 20 seconds, R56 saw 35, both from a menu. If "
                    "it NEVER clears, the tap did not load: check the [R53] TECHNIQUE LANE line "
                    "for TAP = ON, and the compile lines for mgpu_depth_tap.fx.");
            }
            return;
        }

        // R78: AND IT WAITS FOR THE VELOCITY BUFFER, for the same reason.
        //
        // The MVec region cannot be sized before the probe has identified a
        // velocity buffer, and the probe cannot identify one until the game
        // has rendered a scene - it ranks RENDER TARGET BINDS, and a menu
        // binds none of the scene's. So the whole arm holds, exactly as it
        // holds for depth, and for the same duration in practice: both clear
        // on the first frame of real gameplay.
        //
        // s.tried stays FALSE here too. This is the second of the only two
        // paths in this function that return without consuming the attempt.
        // R99: same handle for STABLE_NEED consecutive frames before we size
        // anything from it. The probe republishes on its own dump cadence, so
        // this costs a second or two and nothing else.
        if (s.mvec_mode == 3 && mvec_handle != 0)
        {
            if (mvec_handle == s.mvec_seen_handle) ++s.mvec_stable;
            else { s.mvec_seen_handle = mvec_handle; s.mvec_stable = 1; }
        }
        const unsigned long long STABLE_NEED = 240ull;   // ~4 s at 60 fps
        if (s.mvec_mode == 3 && (mvec_handle == 0 || s.mvec_stable < STABLE_NEED))
        {
            ++s.mvec_arm_waits;
            s.hold_lane = 2;                      // R146: live, this frame
            if (!s.mvec_arm_logged)
            {
                s.mvec_arm_logged = true;
                mgpu::diag::info(
                    "[MGPU][R78] stream arm HELD: MVec=3 (REAL) and the probe has not published "
                    "a velocity buffer yet, so the slot cannot be sized. This clears on the "
                    "first frame of gameplay. If it NEVER clears, the probe found nothing: "
                    "check the [R71] line for a MVEC SOURCE with a non-zero handle, and check "
                    "that its width and height match the scene extents - the probe refuses to "
                    "publish a candidate that does not.");
            }
            return;
        }

        // R146. Both holds are behind us this frame, so nothing is holding.
        // Cleared HERE rather than at s.armed = true because the allocation
        // below can fail and re-enter next frame - and during those attempts
        // the honest answer is "arming", not "still waiting for a lane that
        // already delivered".
        s.hold_lane = 0;

        s.tried = true;

        ID3D12Device *gdev = nullptr;
        if (FAILED(src->GetDevice(IID_PPV_ARGS(&gdev))) || gdev == nullptr) return;
        s.gdev = gdev;

        QueryPerformanceFrequency(&s.freq);

        const D3D12_RESOURCE_DESC rd = src->GetDesc();
        s.width = (UINT)rd.Width; s.height = rd.Height; s.format = rd.Format;
        UINT rows = 0; UINT64 rowb = 0;
        gdev->GetCopyableFootprints(&rd, 0, 1, 0, &s.fp, &rows, &rowb, &s.payload_bytes);

        UINT64 slot_end = SEAL_STRIDE + s.payload_bytes;

        // ---- R30 SLOT LAYOUT v2, SIZED FROM THE RESOURCE ----
        //
        // R30 wrote the arithmetic down: 1664x936 plane 0. R54 then measured
        // 1072x604 at a different output resolution, and R60 moved the source
        // to the tap target, which is at OUTPUT resolution and single-plane
        // R32F. Every one of those numbers is a property of the run, so the
        // only correct size is the one the resource reports here.
        //
        // Depth starts on its own 512 boundary because D3D12 placed footprints
        // require it, which is the same reason the colour payload starts at
        // SEAL_STRIDE rather than at sizeof(MgpuSeal).
        if (s.depth_mode != 0 && depth_handle != 0)
        {
            ID3D12Resource *dsrc =
                reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)depth_handle);
            const D3D12_RESOURCE_DESC dd = dsrc->GetDesc();
            s.depth_w = (unsigned)dd.Width;
            s.depth_h = (unsigned)dd.Height;
            s.depth_format = (unsigned)dd.Format;
            UINT drows = 0; UINT64 drowb = 0;
            gdev->GetCopyableFootprints(&dd, 0, 1, 0, &s.depth_fp,
                                        &drows, &drowb, &s.depth_bytes);
            s.depth_off = ((slot_end + SEAL_STRIDE - 1) / SEAL_STRIDE) * SEAL_STRIDE;
            slot_end = s.depth_off + s.depth_bytes;
        }

        // ---- R78: the MVec region, on its own 512 boundary after depth ----
        //
        // SIZED FROM THE RESOURCE, never written down, for the reason the
        // depth comment above gives and one more that is specific to this
        // buffer: R70 measured the game's velocity target at the DISPLAY
        // extent while colour renders at 65-67% of it. The two are different
        // sizes on this title and the difference is not a constant - it is
        // whatever the engine's dynamic resolution chose. A hardcoded size
        // here would be wrong on the first frame and silently wrong after.
        if (s.mvec_mode == 3 && mvec_handle != 0)
        {
            ID3D12Resource *msrc =
                reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)mvec_handle);
            const D3D12_RESOURCE_DESC md2 = msrc->GetDesc();
            s.mvec_w = (unsigned)md2.Width;
            s.mvec_h = (unsigned)md2.Height;
            s.mvec_format = (unsigned)md2.Format;
            UINT mrows2 = 0; UINT64 mrowb2 = 0;
            gdev->GetCopyableFootprints(&md2, 0, 1, 0, &s.mvec_fp2,
                                        &mrows2, &mrowb2, &s.mvec_bytes2);
            s.mvec_off = ((slot_end + SEAL_STRIDE - 1) / SEAL_STRIDE) * SEAL_STRIDE;
            slot_end = s.mvec_off + s.mvec_bytes2;
        }

        // One slot = seal (padded to the placement alignment) + payload, plus
        // the depth region when it is on, rounded up so that every slot offset
        // is itself 512-aligned. Whole slots only: sub-slot arithmetic is how
        // a ring aliases.
        s.slot_bytes = ((slot_end + SEAL_STRIDE - 1) / SEAL_STRIDE) * SEAL_STRIDE;

        const UINT64 ALIGN = 65536;
        const UINT64 heap_bytes =
            ((s.slot_bytes * stream_state::RING + ALIGN - 1) / ALIGN) * ALIGN;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = heap_bytes; hd.Properties = hp; hd.Alignment = ALIGN;
        hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED |
                                      D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = heap_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        HRESULT h = gdev->CreateHeap(&hd, IID_PPV_ARGS(&s.gheap));
        if (SUCCEEDED(h))
            h = gdev->CreatePlacedResource(s.gheap, 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                                           nullptr, IID_PPV_ARGS(&s.gxfer));
        if (SUCCEEDED(h))
            h = gdev->CreateSharedHandle(s.gheap, nullptr, GENERIC_ALL, nullptr, &s.gshare);

        // The seal staging buffer, persistently mapped. One UPLOAD buffer with
        // RING seal slots: the CPU writes slot k while the GPU may still be
        // reading slot k-1, which is what the ring is for.
        if (SUCCEEDED(h))
            h = make_buf(gdev, SEAL_STRIDE * stream_state::RING,
                         D3D12_HEAP_TYPE_UPLOAD, &s.gup);
        if (SUCCEEDED(h))
        {
            D3D12_RANGE none{0, 0};
            h = s.gup->Map(0, &none, reinterpret_cast<void **>(&s.gup_cpu));
            if (SUCCEEDED(h) && s.gup_cpu != nullptr)
                memset(s.gup_cpu, 0, (size_t)(SEAL_STRIDE * stream_state::RING));
            else h = E_FAIL;
        }

        // The produced-count fence. Its value IS the frame index, which is why
        // there is only one of them for the whole ring.
        if (SUCCEEDED(h))
            h = gdev->CreateFence(0, (D3D12_FENCE_FLAGS)(D3D12_FENCE_FLAG_SHARED |
                                                         D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER),
                                  IID_PPV_ARGS(&s.gfence));
        if (SUCCEEDED(h))
            h = gdev->CreateSharedHandle(s.gfence, nullptr, GENERIC_ALL, nullptr,
                                         &s.gfence_share);

        ID3D12Device *ndev = nullptr;
        {
            std::lock_guard<std::mutex> g(st().cs);
            ndev = st().device;
        }
        s.ndev_b = ndev;   // R28, borrowed - never released here
        if (SUCCEEDED(h) && ndev != nullptr)
        {
            h = ndev->OpenSharedHandle(s.gshare, IID_PPV_ARGS(&s.nheap));
            if (SUCCEEDED(h))
                h = ndev->CreatePlacedResource(s.nheap, 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&s.nxfer));
            if (SUCCEEDED(h)) h = ndev->OpenSharedHandle(s.gfence_share,
                                                         IID_PPV_ARGS(&s.nfence));
            if (SUCCEEDED(h))
                h = make_buf(ndev, SEAL_STRIDE * stream_state::RING,
                             D3D12_HEAP_TYPE_READBACK, &s.nseal);
            if (SUCCEEDED(h))
            {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                h = ndev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.nq));
            }
            if (SUCCEEDED(h))
                h = ndev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&s.na));
            if (SUCCEEDED(h))
                h = ndev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.na, nullptr,
                                            IID_PPV_ARGS(&s.nl));
            if (SUCCEEDED(h)) h = s.nl->Close();
            if (SUCCEEDED(h))
                h = ndev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.nf));
            if (SUCCEEDED(h))
            {
                s.nev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (s.nev == nullptr) h = E_FAIL;
            }
            if (SUCCEEDED(h))
            {
                s.gate_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (s.gate_ev == nullptr) h = E_FAIL;
            }

            // P2.2a. A timestamp query heap and its resolve target. Failure
            // here is NOT fatal: the stream is a correctness instrument first
            // and it must not stop transporting because a profiler could not be
            // built. ts_ok gates every use and the summary says when it is off.
            if (SUCCEEDED(h))
            {
                D3D12_QUERY_HEAP_DESC qh{};
                qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                qh.Count = stream_state::TS_MARKS;
                qh.NodeMask = 0;
                HRESULT th = ndev->CreateQueryHeap(&qh, IID_PPV_ARGS(&s.tsheap));
                if (SUCCEEDED(th))
                    th = make_buf(ndev, (UINT64)stream_state::TS_MARKS * 8,
                                  D3D12_HEAP_TYPE_READBACK, &s.tsread);
                if (SUCCEEDED(th)) th = s.nq->GetTimestampFrequency(&s.ts_freq);
                s.ts_ok = SUCCEEDED(th) && s.ts_freq > 0;
                for (UINT i = 0; i + 1 < stream_state::TS_MARKS; ++i) s.ts_min[i] = 1e30;
                for (UINT q = 0; q < stream_state::TS_SEGS; ++q) s.ts_seg_min[q] = 1e30;   // D5b
                char tl[400];
                snprintf(tl, sizeof tl,
                         "[MGPU][P2.2] GPU timestamps on GPU 1: query heap + "
                         "GetTimestampFrequency hr=0x%08X freq=%llu ticks/s -> %s. These are "
                         "GPU time, not wall-clock; no cross-adapter clock is involved.",
                         (unsigned)th, (unsigned long long)s.ts_freq,
                         s.ts_ok ? "ON" : "OFF (the stream runs unprofiled)");
                mgpu::diag::info(tl);
            }

            // ---- R26: the COPY queue, its lists, and its fence ----
            //
            // Here rather than in stream_nr_create, which is where the R26
            // note put it: every queue, allocator, list and fence this stream
            // owns is built at arm, and splitting one of them out into the
            // lazy neural bring-up would put half the teardown order in a
            // different function from the other half. tex_in stays in
            // stream_nr_create, where the geometry it needs actually is.
            //
            // FAILURE HERE IS NOT FATAL AND DOES NOT FAIL THE ARM. It turns
            // the toggle back off, loudly, and the run continues on the
            // shipped path - which is the control anyway, so a run that fell
            // back is still a valid run rather than a lost one.
            if (SUCCEEDED(h) && s.copy_queue)
            {
                D3D12_COMMAND_QUEUE_DESC cqd{};
                cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                HRESULT ch = ndev->CreateCommandQueue(&cqd, IID_PPV_ARGS(&s.cq));
                for (unsigned i = 0; i < 2 && SUCCEEDED(ch); ++i)
                {
                    ch = ndev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                      IID_PPV_ARGS(&s.cqa[i]));
                    if (SUCCEEDED(ch))
                        ch = ndev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                                     s.cqa[i], nullptr,
                                                     IID_PPV_ARGS(&s.cql[i]));
                    if (SUCCEEDED(ch)) ch = s.cql[i]->Close();
                }
                if (SUCCEEDED(ch))
                    ch = ndev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.cqf));

                // Mode 2 gates the copy on the GAME's fence, opened on GPU 1.
                // Without it there is nothing to wait on, so fall back to mode
                // 1 rather than to something that looks like mode 2 and is not.
                if (SUCCEEDED(ch) && s.cq_gated && s.nfence == nullptr)
                {
                    mgpu::diag::error(
                        "[MGPU][R28] CopyQueue=2 asked for a GPU-side arrival gate, but the "
                        "game's shared fence did not open on GPU 1. Falling back to CopyQueue=1 "
                        "- the queue split without the gate. The run is still valid; it is not "
                        "the arm that was asked for, and cq_spec will be 0 to prove it.");
                    s.cq_gated = false;
                    s.copy_queue_mode = 1;
                }

                if (FAILED(ch))
                {
                    char cl2[420];
                    snprintf(cl2, sizeof cl2,
                             "[MGPU][R26] COPY QUEUE FAILED TO COME UP hr=0x%08X - CopyQueue is "
                             "forced back to 0 and this run is on the shipped DIRECT path. That "
                             "is the control configuration, so the run is still valid; it is "
                             "simply not the experiment that was asked for.",
                             (unsigned)ch);
                    mgpu::diag::error(cl2);
                    if (s.cqf != nullptr) { s.cqf->Release(); s.cqf = nullptr; }
                    for (int i = 1; i >= 0; --i)
                    {
                        if (s.cql[i] != nullptr) { s.cql[i]->Release(); s.cql[i] = nullptr; }
                        if (s.cqa[i] != nullptr) { s.cqa[i]->Release(); s.cqa[i] = nullptr; }
                    }
                    if (s.cq != nullptr) { s.cq->Release(); s.cq = nullptr; }
                    s.copy_queue = false;
                }
                else
                {
                    // The timestamp trap, both halves of it. A COPY queue
                    // needs its own query heap TYPE and its own frequency, and
                    // the adapter has to support copy-queue timestamps at all.
                    // Four queries: two marks, ping-ponged by parity, because
                    // a prefetched copy can resolve into this buffer before
                    // the previous frame's marks have been read.
                    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
                    HRESULT th = ndev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,
                                                           &o3, sizeof o3);
                    if (SUCCEEDED(th) && !o3.CopyQueueTimestampQueriesSupported) th = E_NOTIMPL;
                    if (SUCCEEDED(th))
                    {
                        D3D12_QUERY_HEAP_DESC cqh{};
                        cqh.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
                        cqh.Count = 4;
                        cqh.NodeMask = 0;
                        th = ndev->CreateQueryHeap(&cqh, IID_PPV_ARGS(&s.cq_tsheap));
                    }
                    if (SUCCEEDED(th)) th = make_buf(ndev, 4ull * 8ull,
                                                     D3D12_HEAP_TYPE_READBACK, &s.cq_tsread);
                    if (SUCCEEDED(th)) th = s.cq->GetTimestampFrequency(&s.cq_ts_freq);
                    s.cq_ts_ok = SUCCEEDED(th) && s.cq_ts_freq > 0;
                    if (!s.cq_ts_ok)
                    {
                        if (s.cq_tsread != nullptr) { s.cq_tsread->Release(); s.cq_tsread = nullptr; }
                        if (s.cq_tsheap != nullptr) { s.cq_tsheap->Release(); s.cq_tsheap = nullptr; }
                    }
                    char cl2[560];
                    snprintf(cl2, sizeof cl2,
                             "[MGPU][R26] COPY QUEUE UP on GPU 1, MODE %d (%s): TYPE_COPY queue "
                             "+ 2 allocators + 2 lists + a copy fence. Timestamps hr=0x%08X "
                             "freq=%llu ticks/s -> "
                             "%s. THIS CLOCK IS NOT THE DIRECT QUEUE's (%llu ticks/s) and the two "
                             "are never mixed; the P2.2 unpack bracket on the direct queue goes "
                             "to roughly zero in this mode BY CONSTRUCTION, because the copy is "
                             "no longer on that queue - it is not a saving until the R26 line in "
                             "the summary says the copies actually overlapped.",
                             s.copy_queue_mode,
                             s.cq_gated ? "GPU-gated arrival, R28-B"
                                        : "split only, CPU-tested arrival, R26",
                             (unsigned)th, (unsigned long long)s.cq_ts_freq,
                             s.cq_ts_ok ? "ON"
                                        : "OFF (this adapter does not report copy-queue "
                                          "timestamp support, or the heap did not come up - the "
                                          "transport still runs, only the measurement is absent)",
                             (unsigned long long)s.ts_freq);
                    mgpu::diag::info(cl2);
                }
            }
        }
        else if (ndev == nullptr) h = E_FAIL;

        snprintf(line, sizeof line,
                 "[MGPU][P4.0] stream arm hr=0x%08X source=%ux%u fmt=%d rowPitch=%u "
                 "payload=%llu slot=%llu ring=%u heap=%llu bytes. The heap is created on the "
                 "GAME's device: its command list can only reference resources from the device "
                 "that made it. | DEPTH mode=%d (%s) %ux%u fmt=%u pitch=%u bytes=%llu at slot "
                 "offset %llu, held %llu frame(s) waiting for ReShade to bind depth. Seal v%u, "
                 "%u bytes. The depth region is sized from the RESOURCE at arm time, never "
                 "written down: R30's 1664x936 was true of one resolution and R60 moved the "
                 "source to output resolution anyway.",
                 (unsigned)h, s.width, s.height, (int)s.format,
                 (unsigned)s.fp.Footprint.RowPitch, (unsigned long long)s.payload_bytes,
                 (unsigned long long)s.slot_bytes, stream_state::RING,
                 (unsigned long long)heap_bytes,
                 s.depth_mode,
                 (s.depth_mode == 0) ? "OFF - control arm, byte-identical to every published run"
                                     : ((s.depth_mode == 1) ? "TRANSPORTED AND BOUND"
                                                            : "ON THE BUS, NOT BOUND - the "
                                                              "transport-cost arm"),
                 s.depth_w, s.depth_h, s.depth_format,
                 (unsigned)s.depth_fp.Footprint.RowPitch,
                 (unsigned long long)s.depth_bytes, (unsigned long long)s.depth_off,
                 s.depth_arm_waits, SEAL_VERSION, (unsigned)sizeof(MgpuSeal));
        mgpu::diag::info(line);

        // R78. ITS OWN LINE, not eleven more conversions appended to a format
        // string that already has eleven. P12.5 lost six report blocks to an
        // edit that stayed brace-balanced; a separate snprintf cannot do that.
        if (s.mvec_mode == 3)
        {
            char ml[900];
            snprintf(ml, sizeof ml,
                     "[MGPU][R78] MVEC ARM: mode=3 REAL, region %ux%u fmt=%u pitch=%u "
                     "bytes=%llu at slot offset %llu of %llu, held %llu frame(s) waiting for "
                     "the probe. THE COST IS ON THIS LINE: %llu bytes per frame added to the "
                     "bus on top of colour and depth. The velocity target is at the DISPLAY "
                     "extent on this title while colour renders at ~65%% of it, so this region "
                     "is LARGER than the colour payload (%llu bytes) - that is the engine's "
                     "choice and not a mistake in the sizing. If frametime moves in this run "
                     "and MVec=0 is clean, the bus is where to look first.",
                     s.mvec_w, s.mvec_h, s.mvec_format,
                     (unsigned)s.mvec_fp2.Footprint.RowPitch,
                     (unsigned long long)s.mvec_bytes2,
                     (unsigned long long)s.mvec_off,
                     (unsigned long long)s.slot_bytes,
                     s.mvec_arm_waits,
                     (unsigned long long)s.mvec_bytes2,
                     (unsigned long long)s.payload_bytes);
            mgpu::diag::info(ml);
        }

        if (FAILED(h))
        {
            mgpu::diag::error("[MGPU][P4.0] arm failed - the stream is inert for this launch and "
                              "the game's command list is never touched");
            s.finished = true;
            stream_release();
            return;
        }
        s.armed = true;
        // R88: gxfer exists and the mvec geometry is final from here.
        // The flags are zeroed explicitly: std::atomic's default constructor
        // does not value-initialise before C++20, and relying on the static
        // storage being zero is the kind of assumption this file pays for.
        for (unsigned q = 0; q < stream_state::RING; ++q)
            s.mvec_slot_valid[q].store(0u, std::memory_order_relaxed);
        s.mvec_live.store(s.mvec_mode == 3 && s.mvec_bytes2 != 0,
                          std::memory_order_release);
        return;    // record nothing on the arming frame
    }

    // ---- SIGNAL THE PREVIOUS FRAME, THEN RECORD THIS ONE ----
    //
    // The same ordering rule P2.0 established, now generalised to a stream.
    // ReShade executes the list we recorded into AFTER this handler returns, so
    // a signal issued in the same event sits AHEAD of our own copies and would
    // clear before the data existed. By the time the next event arrives, the
    // previous frame's list has necessarily been submitted - it had to be, to
    // present - so a Signal here lands behind it. One fence, whose value is the
    // count of frames whose copies are known to have been submitted.
    if (s.produced > 0)
    {
        // L1: sample BEFORE the signal, so the reading is the backlog this
        // frame's copy is being queued behind rather than one that includes it.
        // Sampled whichever event ends up issuing the signal, so the histogram
        // stays comparable across SignalAt.
        if (s.gfence != nullptr)
        {
            const unsigned long long done = (unsigned long long)s.gfence->GetCompletedValue();
            const unsigned long long back = (s.produced > done) ? (s.produced - done) : 0ull;
            unsigned b = (back > 7ull) ? 7u : (unsigned)back;
            ++s.q_hist[b];
            s.q_sum += back; ++s.q_n;
            if (back > s.q_max) s.q_max = back;
        }
        // L3: at SignalAt=1 the signal is issued from the PRESENT event
        // instead, one event earlier, which is a whole frame earlier. See
        // stream_on_present.
        if (s.signal_at == 0u)
            (void)gq->Signal(s.gfence, s.produced);
    }

    if (s.produced >= s.max_frames)
    {
        // Bound reached. Stop touching the game's list; the bridge thread
        // prints the summary once the last frames have been consumed.
        s.finished = true;
        // ---- HANGFIX 2026-09-12: STOP THE MID-FRAME MVec RECORDER TOO ----
        //
        // This return stops stream_on_finish_effects touching the game's list,
        // and for four months that read as "the producer has stopped". It is
        // not. stream_mvec_copy is a SEPARATE entry point on the game's render
        // thread, gated only on mvec_live, and it went on recording copies into
        // gxfer every frame after the bound was reached - with no gfence signal
        // behind them any more, because this return skips that too.
        //
        // stream_release then cleared mvec_live (R88, which stops RECORDING)
        // and freed gxfer immediately (which does not wait for EXECUTION). At
        // 1440p the game's queue always drained first and it never showed. At
        // 4K, with 22 MB per copy and about a second between the bound and the
        // release, it did not: DXGI_ERROR_DEVICE_HUNG on the GAME's device,
        // 3.1 seconds after STREAM PASSED.
        //
        // Clearing the flag here means the last copy into gxfer is in frame
        // `produced`'s list, and the Signal issued a few lines above covers it.
        // That is what makes the drain in stream_release sufficient.
        s.mvec_live.store(false, std::memory_order_release);
        return;
    }

    // ---- B1: decide whether this game frame is worth sealing at all ----
    //
    // Placed AFTER the produced-count signal and the bound check, so neither
    // changes behaviour, and BEFORE anything touches the game's command list -
    // the whole point is that a strided frame costs the render card nothing.
    ++s.game_frames;
    if (s.stride_max != 0)
    {
        // Re-decided once per window, not once per frame. A per-frame decision
        // on a per-frame-noisy signal is how a controller starts oscillating,
        // and the neural cadence would oscillate with it.
        if (s.game_frames - s.win_frames_at >= stream_state::STRIDE_WINDOW)
        {
            // BUG, caught by the Passes=2 run on 2026-09-12: `nr_evals`
            // COUNTS PASSES, NOT FRAMES. P4.1 says so in its own text - "NOTE
            // evaluates COUNTS PASSES, not frames" - and this controller read
            // it as frames, so at Passes=2 it saw half the true ratio and at
            // Passes=N it would see an Nth. The run had a real ratio of 2.44
            // with 58.9% of transport being discarded, and the stride sat at 1
            // because the number it was handed was 1.22.
            //
            // Divide by the pass count. This is the denominator the stride
            // decision actually needs: how many GAME FRAMES pass per FRAME
            // that gets evaluated, whatever that frame costs internally.
            const unsigned long long gf = s.game_frames - s.win_frames_at;
            const unsigned pz = (s.passes >= 1u) ? s.passes : 1u;
            const unsigned long long ev = (s.nr_evals > s.win_evals_at)
                                        ? ((s.nr_evals - s.win_evals_at) / pz) : 0ull;
            s.win_frames_at = s.game_frames;
            s.win_evals_at  = s.nr_evals;

            // No evaluates in a whole window means the neural stage is not
            // running - not armed yet, or a menu. Fall back to sealing every
            // frame rather than striding against a denominator of zero.
            unsigned want = 1u;
            if (ev != 0ull)
            {
                // MARGIN, NOT A BARE FLOOR - but 1.25 was too much of one.
                //
                // Plain floor division makes stride k engage the instant the
                // ratio reaches k, and at exactly k the seals arrive at
                // precisely the evaluate rate with nothing left for jitter. So
                // a margin is needed. 1.25 was a guess and the Passes=2 run
                // showed it was the wrong guess: a true ratio of 2.44, 58.9% of
                // transport being discarded on arrival, and the stride refused
                // to move because 2.44 < 2.5.
                //
                // Check it against what the frames are actually doing rather
                // than against a round number. At a ratio of 2.44 the consumer
                // evaluates 41% of frames; stride 2 seals 50%. It delivers more
                // than the consumer can use, with 22% to spare, and the 6-deep
                // ring absorbs the jitter on top of that.
                //
                // 7/8 puts the threshold at 1.14k, so stride 2 engages at 2.3
                // and stride 3 at 3.4. Still refuses the genuinely marginal
                // case - a ratio of exactly 2.0 gives 1.75 and stays at 1,
                // which is right, because stride 2 there would have zero
                // headroom.
                const unsigned long long r = (gf * 7ull) / (ev * 8ull);
                want = (r < 1ull) ? 1u
                     : ((r > (unsigned long long)s.stride_max) ? s.stride_max
                                                               : (unsigned)r);
                ++s.ratio_hist[(r > 8ull) ? 8u : (unsigned)r];
            }
            // One step per window. Floor division is already conservative -
            // a ratio of 1.9 asks for stride 1 - so the damping here is about
            // how FAST it moves, not how far.
            if (want > s.cur_stride && s.cur_stride < s.stride_max) ++s.cur_stride;
            else if (want < s.cur_stride && s.cur_stride > 1u)      --s.cur_stride;
        }
        ++s.stride_hist[(s.cur_stride > 8u) ? 8u : s.cur_stride];

        if (s.cur_stride > 1u)
        {
            if (++s.stride_phase < s.cur_stride)
            {
                ++s.stride_skipped;
                return;   // nothing sealed, nothing copied, produced unchanged
            }
            s.stride_phase = 0;
        }
    }

    const unsigned long long fi = s.produced + 1;

    // FAULT "drop": burn frame index 40 without writing anything for it. The
    // fence still advances, so the consumer is told frame 40 landed and finds
    // whatever the slot held three frames ago. Expected diagnosis: REORDERED at
    // f=40 (the slot holds an older index), then DROPPED gap=2 at f=41.
    // Deliberately models a PRODUCER drop - a frame the game rendered that
    // never made it into the ring - which is a different failure from the
    // consumer falling behind, and is counted separately for that reason.
    if (s.fault_id == stream_state::FK_DROP && fi == 40)
    {
        s.produced = fi;
        s.produced_a.store(fi, std::memory_order_release);
        return;
    }

    const unsigned slot = (unsigned)((fi - 1) % stream_state::RING);
    const UINT64 slot_off = (UINT64)slot * s.slot_bytes;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    MgpuSeal seal{};
    seal.magic = SEAL_MAGIC;
    seal.seal_version = SEAL_VERSION;
    seal.frame_index = fi;
    seal.qpc_submit = (unsigned long long)now.QuadPart;
    seal.payload_bytes = s.payload_bytes;
    seal.width = s.width;
    seal.height = s.height;
    seal.dxgi_format = (unsigned)s.format;
    seal.row_pitch = s.fp.Footprint.RowPitch;
    seal.slot_index = slot;
    seal.barcode = 0;      // NOT IMPLEMENTED - see the note at the top

    // ---- seal v2: describe the depth region, or say honestly that there is none ----
    seal.depth_offset = s.depth_off;
    seal.depth_bytes  = s.depth_bytes;
    seal.depth_width  = s.depth_w;
    seal.depth_height = s.depth_h;
    seal.depth_format = s.depth_format;
    seal.depth_pitch  = s.depth_fp.Footprint.RowPitch;
    seal.depth_mode   = (unsigned)s.depth_mode;
    seal.depth_valid  = 0u;

    // ---- seal v3: describe the MVec region, or say honestly there is none ----
    //
    // THE VALIDITY IS READ, NOT DECIDED, and that is the difference from
    // depth. Depth's decision is made right here, from a handle the caller
    // just handed us. MVec's was made earlier in this same frame, by
    // stream_mvec_copy, at the moment the bind event gave us the resource -
    // because that is the only moment it existed. All that is left to do here
    // is report what happened.
    seal.mvec_offset = s.mvec_off;
    seal.mvec_bytes  = s.mvec_bytes2;
    seal.mvec_width  = s.mvec_w;
    seal.mvec_height = s.mvec_h;
    seal.mvec_format = s.mvec_format;
    seal.mvec_pitch  = s.mvec_fp2.Footprint.RowPitch;
    seal.mvec_mode   = (unsigned)s.mvec_mode;
    seal.mvec_valid  = 0u;
    if (s.mvec_mode == 3)
    {
        if (s.mvec_bytes2 != 0 && s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u) seal.mvec_valid = 1u;
        else ++s.mvec_missing;
    }

    // ---- THE DEPTH DECISION IS MADE HERE, BEFORE THE SEAL IS STAGED ----
    //
    // It is a pure CPU decision - a handle test and a GetDesc - so it can be
    // made before anything is recorded, and then the seal is staged ONCE with
    // the truth in it. The first draft of this set depth_valid after the copy
    // and re-staged the seal, which happened to work only because the game
    // submits the list after this function returns. Correct by accident is not
    // correct.
    ID3D12Resource *dsrc = nullptr;
    bool depth_ok = false;
    if (s.depth_mode != 0)
    {
        if (depth_handle != 0 && s.depth_bytes != 0)
        {
            dsrc = reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)depth_handle);
            const D3D12_RESOURCE_DESC dd = dsrc->GetDesc();

            // THE SIZE GUARD. The slot was sized ONCE at arm time. If the
            // player changes resolution mid-stream the tap target is recreated
            // at the new size, and a copy against the old footprint would write
            // past the depth region - into the next slot. Refused, counted, and
            // the frame ships as depth-invalid rather than as corruption.
            if ((unsigned)dd.Width == s.depth_w && (unsigned)dd.Height == s.depth_h &&
                (unsigned)dd.Format == s.depth_format)
            {
                depth_ok = true;
            }
            else
            {
                ++s.depth_size_rejects;
                dsrc = nullptr;
            }
        }

        if (depth_ok) { seal.depth_valid = 1u; ++s.depth_valid_frames; }
        else          { ++s.depth_invalid_frames; }

        // The consumer unpacks BEFORE the seal readback is available - the seal
        // is copied to a readback buffer and inspected a frame later - so it
        // cannot ask the seal whether this slot carries depth. This is how it
        // knows. Same process, same mutex, and the ring is 6 deep against a
        // consumer at most one frame behind, so the slot cannot have been
        // rewritten under it.
        s.depth_slot_valid[slot] = depth_ok ? 1u : 0u;
    }

    // B1. Same mechanism, same reasoning, for the game-frame number. R87's
    // alignment metric counts GAME frames between evaluates; once the stride
    // makes `produced` stop counting them, the seal index is the wrong unit
    // and the metric would quietly under-report the true motion gap. This is
    // the right unit, recorded where the seal is written.
    s.slot_game_frame[slot] = s.game_frames;

    // Fault injection. Each of these corrupts exactly one field, so the
    // checker's diagnosis names the field it was given. Absent = none.
    if (s.fault_id == stream_state::FK_PITCH && fi == 30) seal.row_pitch += 20;
    if (s.fault_id == stream_state::FK_ALIAS && fi == 30) seal.slot_index =
        (slot + 1) % stream_state::RING;
    if (s.fault_id == stream_state::FK_MAGIC && fi == 30) seal.magic = 0xDEADBEEFu;

    memcpy(s.gup_cpu + (size_t)(slot * SEAL_STRIDE), &seal, sizeof seal);

    // Seal first, payload second, in one list. Command-list order guarantees
    // the seal is written before the pixels within the same submission, and the
    // single fence signal covers both.
    gl->CopyBufferRegion(s.gxfer, slot_off, s.gup, (UINT64)slot * SEAL_STRIDE, sizeof(MgpuSeal));

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION cs{}, cd{};
    cs.pResource = src; cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    cd.pResource = s.gxfer; cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = s.fp; cd.PlacedFootprint.Offset = slot_off + SEAL_STRIDE;
    gl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);

    // Restore EXACTLY. The game did not ask us to change its resource state and
    // must not be able to tell that we did.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gl->ResourceBarrier(1, &b);

    // ---- R63: the depth region, into the SAME list and the SAME submission ----
    //
    // NO BARRIER HERE, AND THAT IS DELIBERATE. dllmain wraps this call with
    // ReShade's own barrier - resource_usage::shader_resource -> copy_source
    // and back - because this file holds no ReShade types and, more to the
    // point, because R56 proved that RESHADE'S mapping is the correct one for
    // a resource ReShade owns. P10.3 already spent a device on guessing a raw
    // D3D12 state for a resource this project did not create.
    //
    // The colour copy above transitions the game's own back buffer and is
    // therefore raw. The two are not the same case and are not written the
    // same way.
    if (depth_ok && dsrc != nullptr)
    {
        D3D12_TEXTURE_COPY_LOCATION ds{}, dt{};
        ds.pResource = dsrc;
        ds.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ds.SubresourceIndex = 0;
        dt.pResource = s.gxfer;
        dt.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dt.PlacedFootprint = s.depth_fp;
        dt.PlacedFootprint.Offset = slot_off + s.depth_off;
        gl->CopyTextureRegion(&dt, 0, 0, 0, &ds, nullptr);
    }

    s.produced = fi;
    s.produced_a.store(fi, std::memory_order_release);   // R88: the lock-free mirror

    // ---- R78: ARM THE NEXT SLOT'S MVec FLAG AT ZERO ----
    //
    // THE ONE LINE THAT MAKES mvec_valid MEAN ANYTHING. The next frame's
    // vectors are written by stream_mvec_copy from the bind event, which may
    // simply not fire - a menu, a load screen, a frame the engine skipped the
    // pass on. Without this the flag left over from the last frame that DID
    // fire would still be set, the seal would claim vectors, and the consumer
    // would bind a slot region holding the previous frame's motion. The whole
    // failure would be invisible: every counter would read healthy and the
    // picture would just be subtly wrong when the camera stopped.
    //
    // Frame fi used slot (fi-1) % RING, so frame fi+1 uses fi % RING. Cleared
    // here, under this mutex, BEFORE the next frame's scene can be drawn.
    s.mvec_slot_valid[(unsigned)(fi % stream_state::RING)].store(0u, std::memory_order_release);
}

// ---- R78: THE MID-FRAME MOTION VECTOR COPY ----
//
// Called from the RENDER TARGET BIND event, not from finish-effects, and that
// is the entire design. The velocity buffer is a transient: the engine writes
// it during the scene and has bound it as a render target again by the time
// effects run, so finish_effects - where colour and depth are copied - is too
// late to read it. R70 identified it by ranking binds 720:1 for exactly this
// reason; this is the other half of that finding put to use.
//
// THE BARRIER IS THE CALLER'S. The probe issues render_target -> copy_source
// and back around this call, because that transition belongs to a ReShade
// type and this file holds none - the same boundary rule the depth copy
// follows, and the same one P10.3 spent a device learning.
//
// THE SLOT IS KNOWABLE HERE, and this is the only subtle part. All this
// function does is record a copy into THE SLOT THIS FRAME IS ABOUT TO SEAL.
// stream_on_finish_effects will use fi = produced + 1 and slot = (fi - 1) %
// RING, which is produced % RING - read here under the same mutex, before the
// frame is sealed, so the two cannot disagree.
//
// ORDERING IS FREE. This copy is recorded into the game's list mid-scene and
// the seal is recorded at finish-effects; command lists execute in submission
// order, so the vectors are in the slot before the seal that describes them,
// and the single fence signal covers both.
void stream_mvec_copy(void *cmd_list_v, unsigned long long mvec_handle)
{
    stream_state &s = str();

    // ---- try_lock, AND THIS IS A DELIBERATE TRADE ----
    //
    // This runs on the GAME'S RENDER THREAD, MID-SCENE - earlier in the frame
    // than anything else this file does to that thread. DEFECT E is the
    // standing lesson: a lock this thread waits on is a lock that can reach
    // the application, and it cost 60 -> 49.8 fps when it did.
    //
    // The consumer's critical sections are short and hold no waits since
    // DEFECT E was fixed, so contention here should be rare. But "should be
    // rare" is not a reason to make the game's render thread wait mid-scene
    // for the bridge thread. A frame that loses its motion vectors is
    // invisible - the seal says so, the model is handed none, and the next
    // frame is normal. A stall in the middle of the scene is not invisible.
    //
    // So: if the lock is busy, this frame ships without vectors and the skip
    // is counted. If that counter is ever more than a handful over a run, the
    // consumer is holding the mutex longer than it should be and THAT is the
    // finding - not a reason to start waiting here.
    // R88: mvec_cs, not cs. try_lock is KEPT so lock-skipped stays a
    // measurement - if it does not go to zero on the next run, this fix did
    // not work and I would rather the log say so than assume it.
    if (!s.mvec_live.load(std::memory_order_acquire)) return;

    std::unique_lock<std::mutex> lk(s.mvec_cs, std::try_to_lock);
    if (!lk.owns_lock())
    {
        s.mvec_lock_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Re-checked under the lock: stream_release clears it before freeing.
    if (!s.mvec_live.load(std::memory_order_relaxed)) return;
    if (s.mvec_mode != 3) return;
    if (s.gxfer == nullptr || s.mvec_bytes2 == 0) return;

    ID3D12GraphicsCommandList *gl =
        reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list_v);
    ID3D12Resource *msrc =
        reinterpret_cast<ID3D12Resource *>((void *)(uintptr_t)mvec_handle);
    if (gl == nullptr || msrc == nullptr) return;

    // THE SIZE GUARD, identical in purpose to depth's. The slot was sized once
    // at arm time. This title runs dynamic resolution: if the engine rescales
    // the velocity target mid-run, a copy against the old footprint would
    // write past the MVec region and into the next slot. Refused, counted, and
    // the frame ships without vectors rather than as corruption.
    const D3D12_RESOURCE_DESC md = msrc->GetDesc();
    if ((unsigned)md.Width != s.mvec_w || (unsigned)md.Height != s.mvec_h ||
        (unsigned)md.Format != s.mvec_format)
    {
        ++s.mvec_size_rejects;
        return;
    }

    // R88: from the atomic mirror. produced is written under cs, which this
    // function no longer holds; a 64-bit read without one is a torn value
    // waiting to happen, and a torn slot index is a copy into the wrong slot.
    const unsigned slot =
        (unsigned)(s.produced_a.load(std::memory_order_acquire) % stream_state::RING);
    const UINT64 slot_off = (UINT64)slot * s.slot_bytes;

    D3D12_TEXTURE_COPY_LOCATION ms{}, mt{};
    ms.pResource = msrc;
    ms.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ms.SubresourceIndex = 0;
    mt.pResource = s.gxfer;
    mt.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    mt.PlacedFootprint = s.mvec_fp2;
    mt.PlacedFootprint.Offset = slot_off + s.mvec_off;
    gl->CopyTextureRegion(&mt, 0, 0, 0, &ms, nullptr);

    // Counted ONCE per frame even if the engine binds the target twice, so
    // that copies + missing equals the number of frames sealed and a
    // disagreement between those two numbers is a real finding.
    if (s.mvec_slot_valid[slot].load(std::memory_order_relaxed) == 0u) ++s.mvec_copies;
    s.mvec_slot_valid[slot].store(1u, std::memory_order_release);
}

// ---- R26: the cross-adapter unpack, on GPU 1's dedicated COPY queue ----
//
// Records and submits frame `f`'s payload copy (the shared cross-adapter
// buffer -> tex_in[f & 1]) and signals cqf with f. Returns false if any step
// failed, and the caller MUST then fall back to the inline unpack for that
// frame: a direct queue left waiting on a fence value nobody will signal is a
// hang on GPU 1, in the file that is the whole bridge.
//
// TWO D3D12 RULES SHAPE THIS AND BOTH ARE EASY TO GET WRONG.
//
// 1. A COPY QUEUE CANNOT DO THE BARRIER. The states allowed on
//    D3D12_COMMAND_LIST_TYPE_COPY are COMMON / COPY_DEST / COPY_SOURCE, and
//    NON_PIXEL_SHADER_RESOURCE - the only state this project has ever handed
//    NGX an input in - is not among them. The transition therefore lives on
//    the DIRECT queue, after its Wait on cqf. R26 named this one.
//
// 2. THE BARRIER'S BEFORE-STATE IS COMMON, NOT COPY_DEST. R26 wrote
//    COPY_DEST -> NON_PIXEL_SHADER_RESOURCE; that is wrong and the difference
//    is not cosmetic. D3D12 DECAYS every resource a COPY queue accessed back
//    to COMMON when that ExecuteCommandLists completes, and the promotion into
//    COPY_DEST for the copy itself is implicit and does not survive it. A
//    barrier claiming COPY_DEST would be naming a state the resource is not
//    in: the debug layer flags it, and a release build accepts it in silence -
//    which is the shape of bug this project keeps paying for. So tex_in rests
//    in COMMON in this mode, is read as NON_PIXEL_SHADER_RESOURCE on the
//    direct queue, and is put back to COMMON at the end of that list, ready
//    for the copy queue to take it again two frames later.
static bool stream_cq_submit(stream_state &s, unsigned long long f)
{
    const unsigned ii = (unsigned)(f & 1ull);
    if (s.cq == nullptr || s.cqf == nullptr || s.cqa[ii] == nullptr ||
        s.cql[ii] == nullptr || s.tex_in[ii] == nullptr || s.nxfer == nullptr)
        return false;

    // Is this allocator's last submission finished? Non-blocking; see
    // cqa_value in stream_state for why the answer is a fallback and not a
    // wait. In the paced case this is always already true.
    if (s.cqf->GetCompletedValue() < s.cqa_value[ii]) return false;

    const unsigned slot = (unsigned)((f - 1) % stream_state::RING);
    const UINT64 slot_off = (UINT64)slot * s.slot_bytes;

    HRESULT h = s.cqa[ii]->Reset();
    if (SUCCEEDED(h)) h = s.cql[ii]->Reset(s.cqa[ii], nullptr);
    if (FAILED(h)) return false;

    // Two marks per copy, ping-ponged by parity: a prefetched copy can resolve
    // into this buffer before the previous frame's marks have been read, and a
    // single pair of slots would hand frame N+1's numbers back labelled N.
    if (s.cq_ts_ok)
        s.cql[ii]->EndQuery(s.cq_tsheap, D3D12_QUERY_TYPE_TIMESTAMP, ii * 2u);

    D3D12_TEXTURE_COPY_LOCATION us{}, ud{};
    us.pResource = s.nxfer;
    us.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    us.PlacedFootprint = s.fp;
    us.PlacedFootprint.Offset = slot_off + SEAL_STRIDE;
    ud.pResource = s.tex_in[ii];
    ud.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ud.SubresourceIndex = 0;
    s.cql[ii]->CopyTextureRegion(&ud, 0, 0, 0, &us, nullptr);

    // ---- WHERE THE ACQUISITION PAYLOAD LANDS (R26a step 2) ----
    //
    // T1b: depth is that second CopyTextureRegion, in THIS batch, covered by
    // the same Signal below - one batch, one fence, no extra round trip,
    // exactly as R26a specified before any of it existed. MVec is still
    // synthetic and uploaded on GPU 1, so it never appears here.
    //
    // No barrier: identical to the colour copy above. The copy queue's own
    // submission decays the destination to COMMON, and the direct queue
    // transitions from there - which is what the before-state comment in
    // stream_poll is about.
    if (s.depth_mode != 0 && s.tex_depth[ii] != nullptr &&
        s.depth_bytes != 0 && s.depth_slot_valid[slot] != 0u)
    {
        D3D12_TEXTURE_COPY_LOCATION dsu{}, ddu{};
        dsu.pResource = s.nxfer;
        dsu.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dsu.PlacedFootprint = s.depth_fp;
        dsu.PlacedFootprint.Offset = slot_off + s.depth_off;
        ddu.pResource = s.tex_depth[ii];
        ddu.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ddu.SubresourceIndex = 0;
        s.cql[ii]->CopyTextureRegion(&ddu, 0, 0, 0, &dsu, nullptr);
        ++s.depth_unpacks;
    }

    // R78: and MVec, in the SAME batch, under the SAME Signal. Third region,
    // same slot, same argument as depth's - one batch, one fence, no extra
    // round trip. The synthetic modes still never appear here: they are
    // uploaded on GPU 1 and never touch the bus.
    if (s.mvec_mode == 3 && s.tex_mvec_r[ii] != nullptr &&
        s.mvec_bytes2 != 0 && s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u)
    {
        D3D12_TEXTURE_COPY_LOCATION msu{}, mdu{};
        msu.pResource = s.nxfer;
        msu.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        msu.PlacedFootprint = s.mvec_fp2;
        msu.PlacedFootprint.Offset = slot_off + s.mvec_off;
        mdu.pResource = s.tex_mvec_r[ii];
        mdu.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        mdu.SubresourceIndex = 0;
        s.cql[ii]->CopyTextureRegion(&mdu, 0, 0, 0, &msu, nullptr);
        ++s.mvec_unpacks;
    }

    if (s.cq_ts_ok)
    {
        s.cql[ii]->EndQuery(s.cq_tsheap, D3D12_QUERY_TYPE_TIMESTAMP, ii * 2u + 1u);
        s.cql[ii]->ResolveQueryData(s.cq_tsheap, D3D12_QUERY_TYPE_TIMESTAMP,
                                    ii * 2u, 2, s.cq_tsread, (UINT64)ii * 16ull);
    }
    if (FAILED(s.cql[ii]->Close())) return false;

    // ---- R28-B: THE GPU-SIDE ARRIVAL GATE ----
    //
    // This is the one line that separates mode 2 from mode 1, and it is the
    // primitive P2.1 built, proved 8/8 byte-exact and then filed as "a
    // building block for the overlap that can win" - where it has sat unused
    // since 2026-09-04. Every other fence read in the live stream is
    // GetCompletedValue() on the bridge thread.
    //
    // A QUEUE wait, so the CPU does not block and is not even consulted: the
    // copy queue stalls on GPU 1 until the GAME's queue signals f on the
    // shared fence, then copies immediately - on the DMA engine, while the 3D
    // engine is still inside frame f-1's EvaluateFeature. That is the overlap.
    //
    // IT MUST BE ISSUED BEFORE THE ExecuteCommandLists IT GATES, and it gates
    // the QUEUE rather than the list - everything submitted after it on this
    // queue is behind it too, which is what makes the ordering free and is
    // also why a wait that never satisfies is a hazard. stream_release
    // releases it deliberately; see the note there.
    if (s.cq_gated && s.nfence != nullptr)
    {
        if (FAILED(s.cq->Wait(s.nfence, (UINT64)f))) return false;
    }

    ID3D12CommandList *const cls[1] = { s.cql[ii] };
    s.cq->ExecuteCommandLists(1, cls);
    if (FAILED(s.cq->Signal(s.cqf, (UINT64)f))) return false;
    s.cqf_value = (UINT64)f;
    s.cqa_value[ii] = (UINT64)f;
    return true;
}

// Bridge thread, once per present. Cheap and does nothing until frames exist.
// D2. True when this fault line should be written. The first FAULT_LOG_CAP of
// each KIND are logged in full, because the first few are what a person reads
// to work out what is happening; after that the kind goes quiet and the
// periodic line in stream_poll carries the count. Per kind rather than in
// aggregate, so a storm of one kind cannot hide a single instance of another -
// which is exactly the case a shared budget would lose.
static bool fault_log(stream_state &s, unsigned long long &per_kind)
{
    ++per_kind;
    if (per_kind <= stream_state::FAULT_LOG_CAP) return true;
    ++s.faults_muted;
    ++s.faults_muted_total;
    return false;
}

// ---- INNER LOOP PHASE 1: solve the line and report it ----
//
// Two points, two unknowns. k is the slope in ms per unit of area, F is what is
// left when the area goes to zero - the part of an evaluate that is not pixels.
//
// The p99 factor is 1.30 and it is NOT fitted here. D5b measured the
// distribution over 13,265 evaluates: nothing at all beyond 40% over the floor,
// 99.5% inside 30%. Sizing the area so the AVERAGE frame fits means half of all
// frames overrun by construction, which is arithmetic rather than a tuning
// problem, so the loop plans against the floor times that factor.
// PHASE 1b. Correct the arm-time solve against the run's own floor.
//
// See the CC_REFIT_N block in stream_state for why this exists. In one line:
// min-of-12 is a biased estimator of the floor, the bias is multiplicative and
// close to equal at both points, so the line's SHAPE is right and only its
// SCALE is wrong. One factor fixes both constants.
//
// run_floor_per_pass is ts_min[2] divided by the pass count - the cheapest
// full-frame evaluate the run has actually seen, which is what the curve was
// trying to estimate at area 1.0 and could not, from twelve samples.
//
// This reports and does not act. If the scale comes back far from 1 in either
// direction the refit says so and refuses, because a factor that large is more
// likely to be a changed area, a changed resolution or a changed pass count
// than a mis-estimated floor, and silently rescaling on top of one of those
// would produce a confident wrong law.
static void costcurve_refit(stream_state &s, double run_floor_per_pass)
{
    char l[1500];
    const double arm_full = s.cc_F + s.cc_k;      // what the solve thought
    if (arm_full <= 0.0 || run_floor_per_pass <= 0.0)
        return;

    const double scale = run_floor_per_pass / arm_full;

    if (scale < 0.55 || scale > 1.25)
    {
        snprintf(l, sizeof l,
                 "[MGPU][CURVE] REFIT REFUSED: the run's full-frame floor is %.3f ms per pass "
                 "against the arm-time solve's %.3f, a factor of %.3f. The refit corrects a "
                 "floor ESTIMATE and a factor that far out is more likely a changed area, "
                 "resolution or pass count than a mis-estimated floor, so the arm-time "
                 "constants stand unchanged and this is reported rather than applied. The "
                 "arm-time law is the one above and it is known to read high.",
                 run_floor_per_pass, arm_full, scale);
        mgpu::diag::warn(l);
        return;
    }

    const double F = s.cc_F * scale;
    const double k = s.cc_k * scale;
    const double full_floor = F + k;
    const double full_p99   = 1.30 * full_floor;
    const double resid = 2.85;
    const double be = 1.30 * full_floor + resid;

    snprintf(l, sizeof l,
             "[MGPU][CURVE] REFIT against this run's own floor, %llu evaluates after the solve: "
             "eval_floor = %.3f ms fixed + %.3f ms x area | full-frame floor %.3f, p99 estimate "
             "%.3f | the arm-time solve said %.3f fixed + %.3f x area, full-frame %.3f, and it "
             "read HIGH by %.1f%%. WHY: the solve takes the minimum of %u evaluates per point, "
             "and min-of-%u from a broad content-dependent distribution is not the floor. The "
             "bias is multiplicative and near-equal at both points, so the RATIO between them "
             "survives it and only the scale needed correcting - one factor, %.4f, applied to "
             "both constants. THESE ARE THE NUMBERS TO COMPARE ACROSS RESOLUTIONS, not the "
             "arm-time pair, because that pair carries a bias that depends on the scene the "
             "calibration happened to run in. Still nothing changed: the area is 1.0 and the "
             "rest of the run is the shipped path. FULL-QUALITY CEILING %.1f fps at an ASSUMED "
             "%.2f ms residual.",
             (unsigned long long)(unsigned)stream_state::CC_REFIT_N,
             F, k, full_floor, full_p99,
             s.cc_F, s.cc_k, arm_full,
             (arm_full / full_floor - 1.0) * 100.0,
             (unsigned)stream_state::CC_SAMPLES, (unsigned)stream_state::CC_SAMPLES,
             scale,
             (be > 0.0) ? 1000.0 / be : 0.0, resid);
    mgpu::diag::info(l);
}

static void costcurve_solve(stream_state &s)
{
    const double a0 = s.cc_area[0], a1 = s.cc_area[1];
    const double e0 = s.cc_floor[0], e1 = s.cc_floor[1];
    char l[1400];

    if (a0 <= a1 + 1e-6 || e0 <= 0.0 || e1 <= 0.0)
    {
        snprintf(l, sizeof l,
                 "[MGPU][CURVE] COST-CURVE FIT FAILED: points (%.4f area, %.3f ms) and (%.4f, %.3f). "
                 "Two usable points at different areas are needed and this run did not get "
                 "them - most often because the stream ended or the scene changed before both "
                 "collected. Nothing is assumed in its place.",
                 a0, e0, a1, e1);
        mgpu::diag::error(l);
        return;
    }

    const double k = (e0 - e1) / (a0 - a1);
    const double F = e0 - k * a0;
    const double full_floor = F + k;              // area 1.0
    const double full_p99   = 1.30 * full_floor;

    // Phase 1b. Kept so the refit has something to correct, and the clock
    // started so the refit knows how much of the run it has seen.
    s.cc_F = F;
    s.cc_k = k;
    s.cc_refit_at = s.ts_n;
    s.cc_refit_done = false;

    snprintf(l, sizeof l,
             "[MGPU][CURVE] COST CURVE MEASURED ON THIS CARD at %ux%u, per pass: "
             "eval_floor = %.3f ms fixed + %.3f ms x area | points (%.4f, %.3f) and "
             "(%.4f, %.3f) | full-frame floor %.3f, p99 estimate %.3f (floor x 1.30, from "
             "D5b's 13,265-sample distribution). "
             "COMPARE F AGAINST OTHER RESOLUTIONS: if F is the same number at a different "
             "output size then it is a genuine per-evaluate cost and these two constants "
             "transfer anywhere; if it scales, they are only true here and the loop must "
             "calibrate on every arm. That is the question this phase exists to answer and it "
             "is not answered by one resolution. "
             "NOTHING WAS CHANGED BY THIS - the area is back at 1.0 and the rest of the run is "
             "the shipped path.",
             s.width, s.height, F, k, a0, e0, a1, e1, full_floor, full_p99);
    mgpu::diag::info(l);

    // The areas that fit, at the residual this rig measured after the debt was
    // cleared. Reported rather than used: Phase 2 measures the residual instead
    // of inheriting it, and until then this line is an illustration.
    {
        const double resid = 2.85;
        char t[900]; int w = 0;
        w += snprintf(t + w, sizeof t - (size_t)w,
                      "[MGPU][CURVE] implied areas, ASSUMING a %.2f ms non-GPU residual "
                      "(measured on the development rig, NOT on this one - Phase 2 measures "
                      "it):", resid);
        static const double TARGETS[3] = { 60.0, 90.0, 120.0 };
        for (unsigned i = 0; i < 3 && w < (int)sizeof t - 80; ++i)
        {
            const double budget = 1000.0 / TARGETS[i] - resid - 1.30 * F;
            const double area = (k > 0.0) ? (budget / (1.30 * k)) : 0.0;
            w += snprintf(t + w, sizeof t - (size_t)w, " %.0f fps -> %.2f%s",
                          TARGETS[i], (area > 1.0) ? 1.0 : ((area < 0.0) ? 0.0 : area),
                          (area > 1.0) ? " (full frame, with room)" : "");
        }
        const double be = 1.30 * (F + k) + resid;
        snprintf(t + w, sizeof t - (size_t)w,
                 " | FULL-QUALITY CEILING %.1f fps - above that the area has to move.",
                 1000.0 / be);
        mgpu::diag::info(t);
    }
}

// ---- D1: the seal checker, lifted out so ONE copy serves TWO callers ----
//
// This is the block that used to sit inside stream_poll's drain loop, moved
// verbatim. It is a function now because D1 gave it a second caller - the bulk
// pre-pass below, which consumes the frames that will NOT evaluate - and a
// checker that exists twice is a checker that starts disagreeing with itself.
// The file already says that about ini parsers; it is the same rule.
//
// run_nr / use_cq / ii are passed rather than recomputed: the timestamp reads
// inside are gated on them, and on the bulk path they are false, so those
// blocks cost nothing and are never fed stale marks.
//
// CALLER CONTRACT: the caller advances s.consumed past f whether this returns
// early or not. That is why the Map-failure path is a bare return.
static void seal_consume(stream_state &s, unsigned long long f, unsigned slot,
                         bool run_nr, bool use_cq, unsigned ii)
{
    char line[1400];
        MgpuSeal got{};
        const unsigned char *m = nullptr;
        D3D12_RANGE rr{ (SIZE_T)(slot * SEAL_STRIDE),
                        (SIZE_T)(slot * SEAL_STRIDE + sizeof(MgpuSeal)) };
        if (SUCCEEDED(s.nseal->Map(0, &rr, (void **)&m)) && m != nullptr)
        {
            memcpy(&got, m + (size_t)(slot * SEAL_STRIDE), sizeof got);
            D3D12_RANGE none{0, 0};
            s.nseal->Unmap(0, &none);
        }
        else return;   // D1: the caller advances s.consumed either way

        // R28. THE FIRST FRAME THROUGH THE NEURAL STAGE, DRAINED ONCE.
        // A wrong resource state is reported by the debug layer at submission,
        // not at record, so it lands here rather than at a returned HRESULT -
        // and nothing in the stream path has ever read that queue. With no
        // debug layer this is a failed QueryInterface and returns immediately.
        // Enable it for the game's exe (Graphics Tools / dxcpl) to make the
        // R26 barrier question answerable at all; see R28's test.
        if (!s.iq_first_done && run_nr && s.ndev_b != nullptr)
        {
            s.iq_first_done = true;
            transit_drain_info_queue(s.ndev_b,
                                     s.copy_queue ? "stream f1 CopyQueue=1"
                                                  : "stream f1 CopyQueue=0");
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        // P2.2a: the per-stage GPU times for this frame. Read only when the
        // neural stage is up, because with it off marks 2..4 are never written
        // and the deltas would be garbage rather than zero.
        // P6.4: only when the evaluates actually happened. On a skipped frame
        // marks 2..4 are never written and the deltas would be whatever the
        // previous resolve left behind - a stale number that looks exactly like
        // a real one.
        if (s.ts_ok && run_nr)
        {
            const UINT64 *tv = nullptr;
            D3D12_RANGE tr{0, (SIZE_T)(stream_state::TS_MARKS * 8)};
            if (SUCCEEDED(s.tsread->Map(0, &tr, (void **)&tv)) && tv != nullptr)
            {
                bool sane = true;
                for (UINT i = 1; i < stream_state::TS_MARKS; ++i)
                    if (tv[i] < tv[i - 1]) sane = false;   // a wrapped or unwritten mark
                if (sane)
                {
                    for (UINT i = 0; i + 1 < stream_state::TS_MARKS; ++i)
                    {
                        const double ms = (double)(tv[i + 1] - tv[i]) * 1000.0
                                        / (double)s.ts_freq;

                        // ---- LEDGER 6b, FIXED: the cost curve must not poison
                        // the instruments that describe the run ----
                        //
                        // CostCurve spends 15 frames at half area. Those are
                        // cheap, so they set ts_min[2] - the floor D5 buckets
                        // against and D5b reports per quarter - and the whole
                        // run then reads as enormous against a floor that was
                        // never a full frame. Two runs showed it plainly. At
                        // 4K: D5 "over the floor (17.575 ms) ... 75%+=1151",
                        // D5b q1 min=17.575 against q2 min=30.155. At 2560x1440
                        // on 2026-09-12: floor read as 7.062 with 75%+=1310,
                        // against q2/q3/q4 minima of 12.991/12.920/12.928.
                        // Both instruments correct about numbers that meant
                        // nothing together.
                        //
                        // A calibration frame is measured BY the cost curve and
                        // is not part of the run being described, so it is out
                        // of every run statistic: sum, min, max, histogram,
                        // segments and n alike. cc_floor is separate and is
                        // where those frames DO land.
                        const bool cc_busy = (s.cc_state == 1u || s.cc_state == 2u);
                        // V28. The inner loop's only input: what the evaluate
                        // ACTUALLY cost this frame, on this card, in this
                        // scene, at the R we are running now. Calibration
                        // frames are excluded for the same reason they are
                        // excluded from every other statistic - they describe
                        // the measurement, not the run.
                        if (i == 2 && !cc_busy)
                        { s.auto_win_sum += ms; ++s.auto_win_n; }
                        if (!cc_busy)
                        {
                            s.ts_sum[i] += ms;
                            if (ms < s.ts_min[i]) s.ts_min[i] = ms;
                            if (ms > s.ts_max[i]) s.ts_max[i] = ms;
                        }
                        // D5: mark 2->3 is EvaluateFeature and it is the only
                        // bracket whose shape is an open question.
                        if (i == 2 && s.ts_min[2] > 0.0)
                        {
                            if (!cc_busy && s.ts_min[2] < 1e29)
                            {
                            const double over = (ms - s.ts_min[2]) / s.ts_min[2] * 100.0;
                            unsigned b = 0;
                            if      (over <  5.0) b = 0;
                            else if (over < 10.0) b = 1;
                            else if (over < 20.0) b = 2;
                            else if (over < 30.0) b = 3;
                            else if (over < 40.0) b = 4;
                            else if (over < 50.0) b = 5;
                            else if (over < 75.0) b = 6;
                            else                  b = 7;
                            ++s.ts_hist[b];
                            }

                            // ---- Phase 1: collect the floor for this point ----
                            //
                            // Per pass, so the number is comparable whatever
                            // Passes is set to - the law is about ONE evaluate
                            // and the bracket spans all of them.
                            if (s.cc_state == 1u || s.cc_state == 2u)
                            {
                                const unsigned ci = s.cc_state - 1u;
                                const double per_pass =
                                    ms / (double)((s.passes >= 1u) ? s.passes : 1u);
                                ++s.cc_n;
                                if (s.cc_n > stream_state::CC_WARMUP)
                                {
                                    if (s.cc_floor[ci] == 0.0 || per_pass < s.cc_floor[ci])
                                        s.cc_floor[ci] = per_pass;
                                }
                                if (s.cc_n >= stream_state::CC_WARMUP
                                               + stream_state::CC_SAMPLES)
                                {
                                    s.cc_n = 0;
                                    ++s.cc_state;
                                    if (s.cc_state == 3u) costcurve_solve(s);
                                }
                            }

                            // ---- Phase 1b: the refit ----
                            //
                            // Once the solve has landed and the run has spent
                            // CC_REFIT_N evaluates at area 1.0 WITHOUT the
                            // calibration frames in the statistic, ts_min[2] is
                            // the real full-frame floor and the arm-time scale
                            // can be corrected against it. Gated on Subrect=100
                            // because a run held at a smaller extent never
                            // produces a floor at area 1.0 and there would be
                            // nothing to compare - a refit there would be
                            // comparing two different areas and calling the
                            // difference bias. Fires once.
                            else if (s.cc_state == 3u && !s.cc_refit_done
                                     && s.subrect_pct == 100u
                                     && s.ts_min[2] < 1e29
                                     && s.ts_n >= s.cc_refit_at
                                                  + (unsigned long long)stream_state::CC_REFIT_N)
                            {
                                s.cc_refit_done = true;
                                costcurve_refit(s, s.ts_min[2]
                                    / (double)((s.passes >= 1u) ? s.passes : 1u));
                            }

                            // D5b. Which quarter of the run this frame is in.
                            // A bounded run splits by its own bound, which is
                            // exact. An unbounded one has no midpoint to split
                            // by, so it falls back to fixed blocks of 500
                            // evaluates and the last block absorbs the rest -
                            // the drift question is still answerable, the
                            // quarters just stop being quarters.
                            unsigned seg;
                            if (s.max_frames != FRAMES_UNBOUNDED && s.max_frames != 0)
                                seg = (unsigned)((f * (unsigned long long)stream_state::TS_SEGS)
                                                 / s.max_frames);
                            else
                                seg = (unsigned)(s.ts_n / 500ull);
                            if (seg >= stream_state::TS_SEGS) seg = stream_state::TS_SEGS - 1;
                            if (!cc_busy)
                            {
                                if (ms < s.ts_seg_min[seg]) s.ts_seg_min[seg] = ms;
                                if (ms > s.ts_seg_max[seg]) s.ts_seg_max[seg] = ms;
                                s.ts_seg_sum[seg] += ms;
                                ++s.ts_seg_n[seg];
                            }
                        }
                    }
                    if (s.cc_state != 1u && s.cc_state != 2u) ++s.ts_n;
                }
                D3D12_RANGE tn{0, 0};
                s.tsread->Unmap(0, &tn);
            }
        }

        // R26: the same frame's unpack, measured on the COPY queue's own
        // clock. Divided by cq_ts_freq and never by ts_freq - the two are
        // different clocks and mixing them is a wrong number that formats
        // beautifully. Read from this parity's half of the buffer, which the
        // prefetch for f+1 cannot have overwritten because it resolves into
        // the other half.
        if (use_cq && s.cq_ts_ok && run_nr)
        {
            const UINT64 *cv = nullptr;
            D3D12_RANGE cr{ (SIZE_T)(ii * 16u), (SIZE_T)(ii * 16u + 16u) };
            if (SUCCEEDED(s.cq_tsread->Map(0, &cr, (void **)&cv)) && cv != nullptr)
            {
                const UINT64 *const mk = cv + (size_t)ii * 2u;
                if (mk[1] >= mk[0])
                {
                    const double cms = (double)(mk[1] - mk[0]) * 1000.0
                                     / (double)s.cq_ts_freq;
                    s.cq_ts_sum += cms;
                    if (cms < s.cq_ts_min) s.cq_ts_min = cms;
                    if (cms > s.cq_ts_max) s.cq_ts_max = cms;
                    ++s.cq_ts_n;
                }
                D3D12_RANGE cn{0, 0};
                s.cq_tsread->Unmap(0, &cn);
            }
        }

        // The liveness sample. Compared against the PREVIOUS frame's, not
        // against a value we chose - section 00a's rule. What a high
        // identical-rate means is ambiguous by construction and the summary
        // says so: a static scene produces identical NR output legitimately.
        // It is a rate to be read alongside the scene, not a verdict.
        if (run_nr && !s.profile)
        {
            unsigned char *sm = nullptr;
            D3D12_RANGE sr{0, 1024};
            if (SUCCEEDED(s.nr_read->Map(0, &sr, (void **)&sm)) && sm != nullptr)
            {
                ++s.nr_frames;
                if (s.nr_have_prev && memcmp(sm, s.nr_prev, 1024) == 0) ++s.nr_same;
                memcpy(s.nr_prev, sm, 1024);
                s.nr_have_prev = true;
                D3D12_RANGE nn{0, 0};
                s.nr_read->Unmap(0, &nn);
            }
        }

        if (got.magic != SEAL_MAGIC || got.seal_version != SEAL_VERSION)
        {
            ++s.bad_magic;
            snprintf(line, sizeof line,
                     "[MGPU][SEAL] BAD MAGIC f=%llu slot=%u: magic=0x%08X version=%u. Nothing "
                     "arrived at this offset, or the two ends disagree about the layout.",
                     f, slot, got.magic, got.seal_version);
            mgpu::diag::error(line);
            // DEFECT B, found on the rig 2026-09-04. A rejected seal never
            // reaches the frame_index bookkeeping below, so `last_seen` stays
            // where it was and the NEXT frame computes gap=2 and reports a
            // DROPPED that did not happen. The magic run showed exactly that:
            // one injected corruption, two counters, and a producer blamed for
            // a fault entirely on the consumer's side. One rejected seal is
            // one failure; the following frame resynchronises silently and is
            // counted here so the suppression is visible rather than implied.
            ++s.resync;
            s.skip_next_gap = true;
        }
        else
        {
            if (got.slot_index != slot)
            {
                ++s.alias;
                snprintf(line, sizeof line,
                         "[MGPU][SEAL] RING ALIAS f=%llu: seal claims slot %u, read from slot %u",
                         f, got.slot_index, slot);
                mgpu::diag::error(line);
            }
            if (got.width != s.width || got.height != s.height ||
                got.dxgi_format != (unsigned)s.format ||
                got.row_pitch != s.fp.Footprint.RowPitch ||
                got.payload_bytes != s.payload_bytes)
            {
                ++s.contract;
                snprintf(line, sizeof line,
                         "[MGPU][SEAL] CONTRACT MISMATCH f=%llu: seal %ux%u fmt=%u pitch=%u "
                         "bytes=%llu | expected %ux%u fmt=%u pitch=%u bytes=%llu",
                         f, got.width, got.height, got.dxgi_format, got.row_pitch,
                         (unsigned long long)got.payload_bytes,
                         s.width, s.height, (unsigned)s.format,
                         (unsigned)s.fp.Footprint.RowPitch,
                         (unsigned long long)s.payload_bytes);
                mgpu::diag::error(line);
            }

            // ---- R63: the depth half of the contract ----
            //
            // Checked against OUR arm-time values, not against the seal's own
            // fields, so a producer and a consumer that disagree report a
            // mismatch instead of quietly agreeing with themselves.
            if (s.depth_mode != 0)
            {
                if (got.depth_mode != (unsigned)s.depth_mode ||
                    got.depth_offset != s.depth_off ||
                    got.depth_bytes != s.depth_bytes ||
                    got.depth_width != s.depth_w ||
                    got.depth_height != s.depth_h ||
                    got.depth_format != s.depth_format ||
                    got.depth_pitch != (unsigned)s.depth_fp.Footprint.RowPitch)
                {
                    ++s.depth_contract;
                    snprintf(line, sizeof line,
                             "[MGPU][R63] DEPTH CONTRACT MISMATCH f=%llu: seal mode=%u off=%llu "
                             "bytes=%llu %ux%u fmt=%u pitch=%u | expected mode=%d off=%llu "
                             "bytes=%llu %ux%u fmt=%u pitch=%u",
                             f, got.depth_mode, (unsigned long long)got.depth_offset,
                             (unsigned long long)got.depth_bytes,
                             got.depth_width, got.depth_height, got.depth_format,
                             got.depth_pitch,
                             s.depth_mode, (unsigned long long)s.depth_off,
                             (unsigned long long)s.depth_bytes,
                             s.depth_w, s.depth_h, s.depth_format,
                             (unsigned)s.depth_fp.Footprint.RowPitch);
                    mgpu::diag::error(line);
                }

                if (got.depth_valid != 0u) ++s.depth_seen_valid;
                else                       ++s.depth_seen_invalid;

                // The seal crossed the adapter; depth_slot_valid did not. They
                // describe the same fact by two different routes, so a
                // disagreement is a real finding rather than a tie to break.
                if ((got.depth_valid != 0u) != (s.depth_slot_valid[slot] != 0u))
                    ++s.depth_flag_disagree;

                // Periodic, because Frames=0 runs print no summary and a
                // transport nobody can see the state of is a transport nobody
                // can trust.
                if (f >= s.depth_report_at)
                {
                    s.depth_report_at = f + 300ull;
                    const unsigned long long tot = s.depth_seen_valid + s.depth_seen_invalid;
                    snprintf(line, sizeof line,
                        "[MGPU][R63] DEPTH TRANSPORT f=%llu mode=%d (%s) | producer: valid=%llu "
                        "invalid=%llu size-rejected=%llu, arm held %llu frame(s) | consumer: "
                        "seals seen valid=%llu invalid=%llu contract mismatches=%llu | region "
                        "%ux%u fmt=%u pitch=%u bytes=%llu at slot offset %llu of %llu. INVALID "
                        "IS NOT AN ERROR: ReShade unbinds depth in menus and scene changes, and "
                        "R61 established that shipping the zeros it leaves behind would hand the "
                        "model a false flat plane. An invalid frame carries the colour and says "
                        "so. WHAT WOULD BE AN ERROR is contract mismatches above zero, or valid "
                        "staying at zero once you are in gameplay - that means the tap is not "
                        "feeding us. %.1f%% of sealed frames carried depth. | GPU 1: unpacked="
                        "%llu bound=%llu skipped=%llu, DepthInverted=%d, flag disagreements="
                        "%llu (the seal crossed the adapter and the slot flag did not, so any "
                        "disagreement between them is real). BOUND vs SKIPPED IS THE R61 TEST: "
                        "skipped counts frames where ReShade had no depth and the model was "
                        "given NONE rather than the previous frame's - open a menu mid-stream "
                        "and this must move.",
                        f, s.depth_mode,
                        (s.depth_mode == 1) ? "TRANSPORTED AND BOUND"
                                            : "ON THE BUS, NOT BOUND - transport-cost arm",
                        s.depth_valid_frames, s.depth_invalid_frames, s.depth_size_rejects,
                        s.depth_arm_waits,
                        s.depth_seen_valid, s.depth_seen_invalid, s.depth_contract,
                        s.depth_w, s.depth_h, s.depth_format,
                        (unsigned)s.depth_fp.Footprint.RowPitch,
                        (unsigned long long)s.depth_bytes,
                        (unsigned long long)s.depth_off,
                        (unsigned long long)s.slot_bytes,
                        (tot != 0) ? (100.0 * (double)s.depth_seen_valid / (double)tot) : 0.0,
                        s.depth_unpacks, s.depth_bound, s.depth_skipped,
                        s.depth_inverted, s.depth_flag_disagree);
                    mgpu::diag::info(line);
                }
            }

            // ---- R78: the MVec half of the contract ----
            //
            // Checked against OUR arm-time values rather than against the
            // seal's own fields, for the reason the depth block above gives.
            if (s.mvec_mode == 3)
            {
                if (got.mvec_mode != (unsigned)s.mvec_mode ||
                    got.mvec_offset != s.mvec_off ||
                    got.mvec_bytes != s.mvec_bytes2 ||
                    got.mvec_width != s.mvec_w ||
                    got.mvec_height != s.mvec_h ||
                    got.mvec_format != s.mvec_format ||
                    got.mvec_pitch != (unsigned)s.mvec_fp2.Footprint.RowPitch)
                {
                    ++s.mvec_contract;
                    snprintf(line, sizeof line,
                             "[MGPU][R78] MVEC CONTRACT MISMATCH f=%llu: seal mode=%u off=%llu "
                             "bytes=%llu %ux%u fmt=%u pitch=%u | expected mode=%d off=%llu "
                             "bytes=%llu %ux%u fmt=%u pitch=%u",
                             f, got.mvec_mode, (unsigned long long)got.mvec_offset,
                             (unsigned long long)got.mvec_bytes,
                             got.mvec_width, got.mvec_height, got.mvec_format,
                             got.mvec_pitch,
                             s.mvec_mode, (unsigned long long)s.mvec_off,
                             (unsigned long long)s.mvec_bytes2,
                             s.mvec_w, s.mvec_h, s.mvec_format,
                             (unsigned)s.mvec_fp2.Footprint.RowPitch);
                    mgpu::diag::error(line);
                }

                if (got.mvec_valid != 0u) ++s.mvec_seen_valid;
                else                      ++s.mvec_seen_invalid;

                // The seal crossed the adapter; mvec_slot_valid did not. Same
                // fact by two routes, so a disagreement is a finding.
                if ((got.mvec_valid != 0u) != (s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u))
                    ++s.mvec_flag_disagree;

                if (f >= s.mvec_report_at)
                {
                    s.mvec_report_at = f + 300ull;

                    // ---- R118: THE AUTO-FALLBACK ----
                    //
                    // MEASURED, Battlefield 6, 2026-09-14, two runs one
                    // variable: with MvecFromEval=1 the transport carried 1931
                    // of 2705 frames; with it at 0 it carried ZERO - on the
                    // same run, holding the SAME correct source address handed
                    // over by R103. The address was never the problem on that
                    // engine. The barrier trigger simply does not fire there.
                    //
                    // On Plague Tale the barrier route works, and R106b exists
                    // because BOTH routes reaching the copy corrupted the
                    // picture. So neither setting is right for every title,
                    // and a global default would break one engine to fix the
                    // other.
                    //
                    // The bridge can tell which engine it is on in 300 frames:
                    // if the barrier route has produced NOTHING by then, and
                    // the calibrator holds the game's own table, the route is
                    // dead here and the evaluate is the only one left. Arm it.
                    //
                    // It cannot fire where the barrier route works, because
                    // copies would not be zero. That is the whole safety
                    // argument and it is structural, not a guard we maintain.
                    if (mgpu::calibrator::eval_copy_mode() == 2 &&
                        !s.mvec_auto_armed &&
                        s.mvec_copies.load(std::memory_order_relaxed) == 0ull)
                    {
                        mgpu::calibrator::table at{};
                        if (mgpu::calibrator::read(at) &&
                            (at.have & mgpu::calibrator::KEY_MVEC) != 0u &&
                            at.mvec != 0ull)
                        {
                            s.mvec_auto_armed = true;
                            mgpu::calibrator::set_eval_copy(1);
                            char al[900];
                            snprintf(al, sizeof al,
                                "[MGPU][R118] MVEC AUTO-FALLBACK ARMED at frame %llu. The "
                                "barrier route has produced ZERO copies in %llu frames while "
                                "the calibrator holds the game's own table (MVEC=0x%llx), so "
                                "on this engine the barrier trigger does not fire and the DLSS "
                                "evaluate is the only route left. Copying from the evaluate "
                                "from here. THIS LINE IS THE ENGINE TELLING US WHAT IT IS - it "
                                "cannot appear on a title where the barrier route works, "
                                "because copies would not be zero. If it appears and copies "
                                "STAY at zero, the evaluate is not carrying either and the "
                                "finding is that neither route reaches this title.",
                                f, f, at.mvec);
                            mgpu::diag::warn(al);
                        }
                    }
                    const unsigned long long mtot = s.mvec_seen_valid + s.mvec_seen_invalid;
                    // Its own buffer: `line` is 1400 and the depth report
                    // above already fills most of it.
                    char ml[1600];
                    snprintf(ml, sizeof ml,
                        "[MGPU][R78] MVEC TRANSPORT f=%llu mode=3 REAL | producer: copies=%llu "
                        "missing=%llu size-rejected=%llu lock-skipped=%llu, arm held %llu "
                        "frame(s) | consumer: "
                        "seals seen valid=%llu invalid=%llu contract mismatches=%llu flag "
                        "disagreements=%llu | region %ux%u fmt=%u pitch=%u bytes=%llu at slot "
                        "offset %llu of %llu | GPU 1: unpacked=%llu bound=%llu skipped=%llu, "
                        "scale=(%.3f, %.3f). %.1f%% of sealed frames carried vectors. COPIES IS "
                        "THE ONE TO WATCH: it counts frames where the bind hook fired and a copy "
                        "was recorded. If it stays at zero the probe published a handle nobody "
                        "binds, and MVec never reaches the model whatever the rest of this line "
                        "says. SKIPPED IS NOT AN ERROR - it counts frames with no vectors, which "
                        "is what a menu looks like, and those frames are handed NONE rather than "
                        "the last frame's motion.",
                        f, s.mvec_copies.load(std::memory_order_relaxed),
                        s.mvec_missing.load(std::memory_order_relaxed),
                        s.mvec_size_rejects.load(std::memory_order_relaxed),
                        s.mvec_lock_skips.load(std::memory_order_relaxed),
                        s.mvec_arm_waits,
                        s.mvec_seen_valid, s.mvec_seen_invalid, s.mvec_contract,
                        s.mvec_flag_disagree,
                        s.mvec_w, s.mvec_h, s.mvec_format,
                        (unsigned)s.mvec_fp2.Footprint.RowPitch,
                        (unsigned long long)s.mvec_bytes2,
                        (unsigned long long)s.mvec_off,
                        (unsigned long long)s.slot_bytes,
                        s.mvec_unpacks, s.mvec_bound, s.mvec_skipped,
                        s.mvec_scale_x, s.mvec_scale_y,
                        (mtot != 0) ? (100.0 * (double)s.mvec_seen_valid / (double)mtot) : 0.0);
                    mgpu::diag::info(ml);

                    // ---- R87: the alignment histogram ----
                    char el[900];
                    unsigned long long tot_ev = 0;
                    for (unsigned q = 0; q < 8; ++q) tot_ev += s.mvec_elapsed_hist[q];
                    snprintf(el, sizeof el,
                        "[MGPU][R87] MVEC FRAME ALIGNMENT f=%llu | evaluates=%llu | elapsed "
                        "1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8+=%llu | worst=%llu "
                        "| %.2f%% at 1. A motion vector describes ONE game frame; NR reprojects "
                        "by whatever it is handed. When the frame NR blends against is older "
                        "than one, history lands SHORT by this factor - zero error at rest, "
                        "growing with speed, and no constant MVecScale can fix it. The scale is "
                        "multiplied by this per evaluate. IF THIS LINE IS FLAT AT 1 AND THE "
                        "ARTEFACT IS UNCHANGED, PACING IS ELIMINATED and what remains is units "
                        "or sign. P4.1's skipped-frame counter predicts about 99.5%% at 1, so a "
                        "materially lower figure here is a desync no counter has shown us.",
                        f, tot_ev,
                        s.mvec_elapsed_hist[0], s.mvec_elapsed_hist[1],
                        s.mvec_elapsed_hist[2], s.mvec_elapsed_hist[3],
                        s.mvec_elapsed_hist[4], s.mvec_elapsed_hist[5],
                        s.mvec_elapsed_hist[6], s.mvec_elapsed_hist[7],
                        s.mvec_elapsed_max,
                        (tot_ev != 0) ? (100.0 * (double)s.mvec_elapsed_hist[0] / (double)tot_ev)
                                      : 0.0);
                    mgpu::diag::info(el);
                }
            }

            // Equal is NOT reuse here - this consumer never re-reads a frame,
            // so an equal index means the slot held the previous frame's seal
            // when it should have held this one. That is a genuine stale read,
            // and calling it "reuse" (as the first build's counter did) would
            // have filed a real fault under an expected-behaviour label.
            if (got.frame_index == s.last_seen && s.last_seen != 0)
            {
                ++s.dropped;
                if (fault_log(s, s.logged_stale))
                {
                    snprintf(line, sizeof line,
                             "[MGPU][SEAL] STALE f=%llu: the slot still holds seal %llu. The fence "
                             "said frame %llu had landed and it had not.",
                             f, (unsigned long long)got.frame_index, f);
                    mgpu::diag::error(line);
                }
            }
            else if (got.frame_index < s.last_seen)
            {
                ++s.reordered;
                if (fault_log(s, s.logged_reord))
                {
                    snprintf(line, sizeof line,
                             "[MGPU][SEAL] REORDERED f=%llu: seal says %llu, last seen %llu",
                             f, (unsigned long long)got.frame_index, s.last_seen);
                    mgpu::diag::error(line);
                }
            }
            else
            {
                const unsigned long long gap = got.frame_index - s.last_seen;
                if (s.skip_next_gap)
                {
                    // Resynchronising after a rejected seal - see DEFECT B.
                    s.skip_next_gap = false;
                }
                else if (s.last_seen != 0 && gap != 1)
                {
                    ++s.dropped;
                    if (fault_log(s, s.logged_drop))
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][SEAL] DROPPED f=%llu gap=%llu (last_new=%llu)",
                                 f, gap, s.last_seen);
                        mgpu::diag::error(line);
                    }
                }
                const double lat = (s.freq.QuadPart > 0)
                    ? ((double)(now.QuadPart - (long long)got.qpc_submit) * 1000.0
                       / (double)s.freq.QuadPart) : 0.0;
                if (lat < s.lat_min) s.lat_min = lat;
                if (lat > s.lat_max) { s.lat_max = lat; s.lat_max_frame = got.frame_index; }
                if (s.lat_n == 0) s.first_lat = lat;
                s.lat_sum += lat; ++s.lat_n;

                // L2: from the fence sighting, not from the record.
                if (s.seen_fence_at != 0 && s.freq.QuadPart > 0)
                {
                    const double rl = (double)(now.QuadPart - (long long)s.seen_fence_at)
                                      * 1000.0 / (double)s.freq.QuadPart;
                    if (rl >= 0.0)
                    {
                        if (rl < s.ready_min) s.ready_min = rl;
                        if (rl > s.ready_max) s.ready_max = rl;
                        s.ready_sum += rl; ++s.ready_n;
                    }
                }
                if (slot < stream_state::RING) ++s.slot_hits[slot];
                s.last_seen = got.frame_index;

                // 61, not 60: the stride must be COPRIME with the ring depth or
                // every sampled line lands on the same slot and the log implies
                // a ring that is not being used.
                if ((got.frame_index % 61) == 0)
                {
                    snprintf(line, sizeof line,
                             "[MGPU][SEAL] new f=%llu slot=%u gap=%llu lat=%.2fms pitch=%u "
                             "fmt=%u bytes=%llu bc=%u(unimplemented) OK",
                             (unsigned long long)got.frame_index, slot, gap, lat,
                             got.row_pitch, got.dxgi_format,
                             (unsigned long long)got.payload_bytes, got.barcode);
                    mgpu::diag::info(line);
                }
            }
        }
}

void stream_poll()
{
    stream_state &s = str();
    // DEFECT E, found on the rig 2026-09-05 by running Passes=2 and watching the
    // GAME slow down from ~60 to ~49.8 fps.
    //
    // This was a lock_guard held for the whole function - INCLUDING the fence
    // wait below, which blocks until GPU 1 has finished the unpack, every
    // evaluate and the sample. stream_on_finish_effects takes this same mutex
    // on the GAME'S RENDER THREAD every frame. So the game's render thread was
    // waiting on GPU 1's neural work, once per frame.
    //
    // The header has warned about exactly this since P4.0 - "holding it across
    // a wait is the one hazard in this add-on that can reach the application" -
    // and P5.1 applied that discipline to stream_present_gate. stream_poll was
    // never checked against it.
    //
    // It was invisible at one pass because GPU 1's ~8 ms fitted inside the
    // frame interval. Two passes is ~16 ms against ~17 ms, and the game fell
    // over the edge. THE ARCHITECTURE'S CLAIM - that neural work is free to the
    // game - was true of the design and false of this build.
    //
    // unique_lock, not lock_guard, so the wait can happen with it released.
    std::unique_lock<std::mutex> lk(s.cs);
    if (!s.armed || s.summarised) return;

    // ---- V28: THE INNER LOOP. Decide, then fall through to the commit. ----
    //
    // Runs BEFORE the rebuild block on purpose: a decision made here is
    // committed in the same poll rather than one later.
    //
    // It only ever acts through the SAME staged-rebuild path the panel uses.
    // There is deliberately no second way to change R - two code paths that
    // both tear down an NGX feature is how the teardown crash comes back.
    if (s.auto_on != 0u && s.sr_on != 0u && s.sr_handle != nullptr
        && s.sr_rebuild_req == 0u && s.auto_win_n >= 120u
        && s.consumed >= s.auto_hold_until)
    {
        const double mean = s.auto_win_sum / (double)s.auto_win_n;
        s.auto_win_sum = 0.0; s.auto_win_n = 0;
        s.auto_last_mean = mean;

        // THE BUDGET IS NOT THE WHOLE FRAME. GPU 1 also pays transport and the
        // sample, and the consumer is serialised, so spending the entire frame
        // interval on the evaluate guarantees missing it. 85% leaves the rest.
        const double budget = s.auto_budget_ms * 0.85;

        char al[420];
        if (mean > budget * 1.05 && s.auto_rung + 1u < stream_state::AUTO_RUNGS)
        {
            ++s.auto_rung;
            s.sr_req_scale = (int)stream_state::AUTO_LADDER[s.auto_rung];
            s.sr_rebuild_req = 1u;
            ++s.auto_changes;
            s.auto_hold_until = s.consumed + 600ull;
            snprintf(al, sizeof al,
                     "[MGPU][AUTO] evaluate %.2f ms over the %.2f ms budget (%u fps target) - "
                     "stepping R DOWN to %u%% of the display extent, rung %u of %u. Change #%u. "
                     "Holding for 600 frames.",
                     mean, budget, s.auto_target_fps,
                     stream_state::AUTO_LADDER[s.auto_rung], s.auto_rung + 1u,
                     stream_state::AUTO_RUNGS, s.auto_changes);
            mgpu::diag::info(al);
        }
        else if (mean < budget * 0.80 && s.auto_rung > 0u)
        {
            --s.auto_rung;
            s.sr_req_scale = (int)stream_state::AUTO_LADDER[s.auto_rung];
            s.sr_rebuild_req = 1u;
            ++s.auto_changes;
            s.auto_hold_until = s.consumed + 600ull;
            snprintf(al, sizeof al,
                     "[MGPU][AUTO] evaluate %.2f ms, well under the %.2f ms budget - stepping R "
                     "UP to %u%%, rung %u of %u. Change #%u. THE GAP BETWEEN THE TWO THRESHOLDS "
                     "(80%% up, 105%% down) IS WHAT STOPS THIS OSCILLATING; do not narrow it "
                     "without a run that shows it settling.",
                     mean, budget, stream_state::AUTO_LADDER[s.auto_rung], s.auto_rung + 1u,
                     stream_state::AUTO_RUNGS, s.auto_changes);
            mgpu::diag::info(al);
        }
        else if (s.auto_changes == 0u)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                snprintf(al, sizeof al,
                         "[MGPU][AUTO] on target: evaluate %.2f ms against a %.2f ms budget "
                         "(%u fps). Nothing to change - said once, then quiet.",
                         mean, budget, s.auto_target_fps);
                mgpu::diag::info(al);
            }
        }
    }

    // ---- V27: COMMIT A STAGED SR REBUILD ----
    //
    // THE SAFE MOMENT IS HERE AND ONLY HERE. Every submission to GPU 1's queue
    // in this file signals s.nf and waits on it before returning, so when a
    // poll begins there is nothing outstanding on that queue - which is also
    // most of what 6i is about. A rebuild anywhere else, and in particular from
    // the overlay callback on the presenting thread, would be racing the
    // evaluate.
    //
    // Every step announces itself first. The arm markers earned that habit and
    // this is a teardown-and-rebuild of a live NGX feature, which is the exact
    // shape of the P1.0 crash.
    if (s.sr_rebuild_req != 0u)
    {
        s.sr_rebuild_req = 0u;
        if (s.sr_on == 0u || s.sr_handle == nullptr)
        {
            mgpu::diag::warn("[MGPU][C2-SR] rebuild requested but Super Resolution is not "
                             "running - ignored. Nothing was released.");
            s.sr_rebuild_last_ok = 0;
        }
        else
        {
            char rl[420];
            const int oq = s.sr_quality, op = s.sr_preset;
            const unsigned osc = s.sr_scale_pct;
            if (s.sr_req_quality >= 0) s.sr_quality   = s.sr_req_quality;
            if (s.sr_req_preset  >= 0) s.sr_preset    = s.sr_req_preset;
            // V28. A SCALE CHANGE MOVES R, so this rebuild is no longer
            // feature-only: stream_sr_create sizes the reduce target, the NR
            // input pair and the SR input from R, and stream_sr_release_only
            // frees exactly those. The two are each other's inverse and that
            // is the only reason changing R here is safe.
            if (s.sr_req_scale   >= 0) s.sr_scale_pct = (unsigned)s.sr_req_scale;
            s.sr_req_quality = -1; s.sr_req_preset = -1; s.sr_req_scale = -1;
            (void)osc;

            snprintf(rl, sizeof rl,
                     "[MGPU][C2-SR] rebuild step 1: quality %d -> %d, preset %d -> %d, "
                     "SRScale %u -> %u. Releasing the Super Resolution feature and everything "
                     "sized from R. THE TRANSPORT, THE RING AND THE NEURAL STAGE ARE NOT "
                     "TOUCHED - they are sized from the DISPLAY extent, which never moves "
                     "here.", oq, s.sr_quality, op, s.sr_preset, osc, s.sr_scale_pct);
            mgpu::diag::info(rl);

            ID3D12Device *rdev = nullptr;
            { std::lock_guard<std::mutex> g(st().cs); rdev = st().device; }

            // Defensive, and cheap: every path already waits, so this should
            // return immediately. If it ever does not, the assumption above is
            // wrong and this is where it will show.
            if (s.nf != nullptr && s.nev != nullptr && s.nf->GetCompletedValue() < s.nf_value)
            {
                mgpu::diag::info("[MGPU][C2-SR] rebuild step 2: GPU 1 still had work "
                                 "outstanding at poll entry - waiting on its fence. THIS IS "
                                 "UNEXPECTED; the consumer is supposed to be fully serialised.");
                s.nf->SetEventOnCompletion(s.nf_value, s.nev);
                (void)WaitForSingleObject(s.nev, 20000);
            }

            mgpu::diag::info("[MGPU][C2-SR] rebuild step 3: releasing - IF THIS IS THE LAST "
                             "LINE, IT DIED IN THE TEARDOWN");
            stream_sr_release_only(s);

            mgpu::diag::info("[MGPU][C2-SR] rebuild step 4: creating - IF THIS IS THE LAST "
                             "LINE, IT DIED IN NGX");
            const bool rok = (rdev != nullptr) && stream_sr_create(s, rdev);
            ++s.sr_rebuild_n;
            s.sr_rebuild_last_ok = rok ? 1 : 0;
            if (rok)
            {
                snprintf(rl, sizeof rl,
                         "[MGPU][C2-SR] rebuild step 5: DONE, rebuild #%u. Read the "
                         "nvngx_dlss_*.log to see which preset was HONOURED - this line only "
                         "says what was asked for.", s.sr_rebuild_n);
                mgpu::diag::info(rl);
            }
            else
            {
                s.sr_on = 0u;
                mgpu::diag::error("[MGPU][C2-SR] rebuild FAILED. Super Resolution is off for "
                                  "the rest of this run and the stream continues without it - "
                                  "the same behaviour a failed create at arm has always had. "
                                  "The figures from before the rebuild are still valid; the "
                                  "ones after it are a different pipeline and must not be "
                                  "pooled with them.");
            }
        }
    }

    // WHY THIS IS STILL CALLED FROM THE PRESENT LOOP, AND WHY THAT IS WRONG.
    //
    // One poll per bridge present ties the consumer's cadence to the bridge
    // swapchain's vsync. That was invisible while GPU 1 was headless and its
    // present loop ran at 210 fps; attaching a 60 Hz monitor made it 1.01x the
    // producer's rate and the coupling became the limiting factor.
    //
    // The right design is a consumer that blocks on the shared fence event for
    // frame consumed+1 and wakes exactly when it lands - display-independent,
    // lower latency, no polling at all. That needs the consumer off the bridge
    // thread, and it is NOT written here on purpose: the stream mutex is also
    // taken by stream_on_finish_effects on the GAME'S RENDER THREAD every
    // frame, so a consumer thread that held it across a GPU wait would stall
    // the game. Getting that locking wrong is the one bug in this add-on that
    // could reach into the application, and it is not something to write blind
    // against a rig I cannot run.
    //
    // So: ring depth absorbs it for now (see RING), the summary says loudly
    // when the margin is gone, and the real fix lands with the display path
    // that actually needs it.
    ++s.polls;

    // D2: one line per 500 suppressed faults, at most. Emitted here rather
    // than at the fault site so it costs nothing during the drain itself.
    if (s.faults_muted >= 500)
    {
        char ml[420];
        snprintf(ml, sizeof ml,
                 "[MGPU][SEAL] %llu further fault lines suppressed (%llu this run). The COUNTERS "
                 "are complete and the summary totals are true; only the per-occurrence lines "
                 "are capped, because writing them was costing the consumer the frame time it "
                 "needed to stop producing them.",
                 s.faults_muted, s.faults_muted_total);
        mgpu::diag::warn(ml);
        s.faults_muted = 0;
    }

    const unsigned long long completed =
        (s.nfence != nullptr) ? (unsigned long long)s.nfence->GetCompletedValue() : 0;

    // L2: the instant this poll first saw the fence move. Every frame the
    // advance exposed became READY at or before now, and this is the earliest
    // moment we could possibly have known.
    if (completed > s.seen_fence_val)
    {
        LARGE_INTEGER fnow{};
        QueryPerformanceCounter(&fnow);
        s.seen_fence_at  = (unsigned long long)fnow.QuadPart;
        s.seen_fence_val = completed;
    }

    char line[1400];   // P6.0: the P2.2 and P4.1 summaries carry the pass split now

    if (s.consumed >= completed) ++s.idle_polls;

    // P4.1: bring the neural stage up on the first frame that is actually
    // consumable. Not at arm time - the geometry is only trustworthy once a
    // seal has carried it across, and CreateFeature costs ~180 ms that would
    // otherwise be spent before we knew the stream worked at all.
    if (s.neural && !s.nr_tried && s.consumed < completed)
    {
        // V29. Only latched once the attempt actually reached NGX. A stale
        // session clears the latch below and comes back in ~120 frames.
        s.nr_tried = true;
        ++s.nr_attempts;
        ID3D12Device *ndev = nullptr;
        {
            std::lock_guard<std::mutex> g(st().cs);
            ndev = st().device;
        }
        s.nr_ok = (ndev != nullptr) && stream_nr_create(s, ndev);

        // V31. No retry, no stale-session special case. If CreateFeature
        // fails it fails the way it always has, and if it kills the process
        // the last arm-step line says which call did it.
        // C2-SR is built AFTER NR, deliberately. It needs the NGX core already
        // initialised on this device (NR's create does that), it needs
        // s.tex_in[0] to exist so it can match its format, and if NR did not
        // come up there is nothing for SR to enlarge.
        if (s.nr_ok && s.sr_on != 0u && ndev != nullptr)
        {
            if (!stream_sr_create(s, ndev))
            {
                s.sr_on = 0u;
                s.sr_ready = false;
            }
        }

        // Phase 1 arms here, not at stream_request: the calibration needs the
        // neural stage up and the geometry carried across by a seal, and this
        // is the first point where both are true.
        //
        // Refused when a subrect is already set by hand - the calibration owns
        // the subrect while it runs, and silently fighting a deliberate setting
        // is how a run reports numbers nobody asked for.
        if (s.nr_ok && s.cc_on != 0)
        {
            if (s.subrect_pct != 100u)
                mgpu::diag::warn("[MGPU][CURVE] CostCurve=1 ignored: Subrect is set by hand and "
                                 "the calibration needs to own the subrect while it runs. Clear "
                                 "Subrect (or set it to 100) to calibrate.");
            else
                s.cc_state = 1;
        }

        // P7.0. Here and not at arm: the source dimensions come from the seal,
        // so this is the earliest point they are known to be real. Not in
        // profile mode - that run has no on-screen output to size.
        // P7.3: no longer gated on window_mode. A format mismatch has to be
        // corrected in crop too, where the window does not change at all - and
        // present_resize itself decides there is nothing to do when the mode is
        // crop AND the format already matches.
        if (!s.profile)
            (void)present_resize(s.width, s.height, s.window_mode, s.format);
    }

    // ---- RING WINDOW: hold no more depth than this run has earned ----
    //
    // Runs AHEAD of the overrun walk below, so the walk only ever sees what
    // this let through. Frames the window steps over are counted separately
    // from `overrun` and from `dropped`: an overrun is the producer beating the
    // consumer past the whole ring and is a fault, a drop is data lost in
    // transport, and this is neither - it is the consumer choosing the newest
    // frame on purpose. Conflating the three would turn a working feature into
    // a scary summary.
    if (s.rw_at == 0ull)
    {
        s.rw_at = s.consumed;
        s.rw_over_at = s.overrun;
    }
    if (s.rw_on != 0u && completed > s.consumed + (unsigned long long)s.rw)
    {
        const unsigned long long target = completed - (unsigned long long)s.rw;
        s.rw_skipped += target - s.consumed;
        s.consumed = target;
        // THE SKIP MUST NOT READ AS A DROP. last_seen is what the seal checker
        // compares the next frame's index against, so advancing consumed
        // without it made every skip look like a transport gap. The 2026-09-12
        // run measured the damage exactly: 348 deliberate skips reported as
        // dropped=142 reordered=32, on a run where the transport was perfect.
        // A comment in this file already said skips are neither drops nor
        // overruns; this is the line that makes that true.
        if (target > s.last_seen) s.last_seen = target;
    }
    if (s.rw_on != 0u && s.consumed >= s.rw_at + (unsigned long long)stream_state::RW_WINDOW)
    {
        const unsigned long long over = s.overrun - s.rw_over_at;
        if (over > 0ull)
        {
            // Grow immediately. An overrun is visible; a frame of latency is not.
            if (s.rw < stream_state::RW_MAX) ++s.rw;
            s.rw_clean = 0;
        }
        else
        {
            ++s.rw_clean;
            if (s.rw_clean >= 3u)
            {
                if (s.rw > stream_state::RW_MIN) --s.rw;
                s.rw_clean = 0;
            }
        }
        if (s.rw <= 6u) ++s.rw_hist[s.rw];
        s.rw_at = s.consumed;
        s.rw_over_at = s.overrun;
    }

    // ---- D1 STEP 1: walk past recycled slots. CPU only, no submission. ----
    //
    // Hoisted out of the drain loop so that everything below it is guaranteed
    // to be a contiguous, non-recycled range - which is what lets the bulk
    // pass address distinct ring slots without checking. The loop below keeps
    // its own overrun branch as unreachable defence rather than deleting it:
    // if this walk is ever wrong, the old path still names the condition
    // instead of silently reading a rewritten slot.
    while (s.consumed < completed && completed >= s.consumed + 1 + stream_state::RING)
    {
        const unsigned long long of = s.consumed + 1;
        ++s.overrun;
        s.consumed = of;
        if (!s.said_overrun)
        {
            s.said_overrun = true;
            snprintf(line, sizeof line,
                     "[MGPU][SEAL] CONSUMER OVERRUN at f=%llu: the producer is %llu frames "
                     "ahead of us and ring depth is %u, so this slot was rewritten before it "
                     "was read. This is OUR slowness, not a dropped frame - counted "
                     "separately for exactly that reason. Said once; the summary carries the "
                     "total.",
                     of, completed - of, stream_state::RING);
            mgpu::diag::warn(line);
        }
    }

    // ---- D1 STEP 2: THE BULK SEAL PASS ----
    //
    // Every frame but the newest is going to have its neural work skipped -
    // that is the `newest` gate below and it has always been right. What was
    // wrong is what those frames cost: the drain loop submitted a command
    // list, signalled a fence and BLOCKED ON A FULL GPU ROUND TRIP for each of
    // them, to copy 64 bytes of seal and nothing else. A backlog of k frames
    // therefore cost k round trips to make ONE frame of visible progress,
    // which is what made falling behind self-amplifying: the further behind we
    // got, the more we paid per unit of catching up.
    //
    // Here they go into ONE list, ONE submission and ONE wait. The seal is
    // still read for every frame, in order, by the same checker - identity and
    // ordering are the instrument and they are not what was costing anything.
    //
    // WHAT THIS DOES NOT CHANGE: which frame is evaluated, and which frame
    // reaches the screen. Both were already the newest and both still are.
    if (s.consumed + 1 < completed)
    {
        const unsigned long long bulk_last = completed - 1;
        unsigned long long bfi[stream_state::RING] = {};
        unsigned           bsl[stream_state::RING] = {};
        unsigned           n = 0;

        HRESULT bh = s.na->Reset();
        if (SUCCEEDED(bh)) bh = s.nl->Reset(s.na, nullptr);
        if (SUCCEEDED(bh))
        {
            for (unsigned long long bf = s.consumed + 1;
                 bf <= bulk_last && n < stream_state::RING; ++bf)
            {
                unsigned sl = (unsigned)((bf - 1) % stream_state::RING);
                // The same injection the drain loop applies, applied here for
                // the same frame - otherwise turning on a fault would change
                // which path the run takes as well as what it tests.
                if (s.fault_id == stream_state::FK_STALE && bf == 40)
                    sl = (sl + stream_state::RING - 1) % stream_state::RING;
                s.nl->CopyBufferRegion(s.nseal, (UINT64)sl * SEAL_STRIDE,
                                       s.nxfer, (UINT64)sl * s.slot_bytes,
                                       sizeof(MgpuSeal));
                bfi[n] = bf; bsl[n] = sl; ++n;
            }
            bh = s.nl->Close();
        }
        if (SUCCEEDED(bh) && n != 0)
        {
            ID3D12CommandList *const bls[1] = { s.nl };
            s.nq->ExecuteCommandLists(1, bls);
            ++s.nf_value;
            bh = s.nq->Signal(s.nf, s.nf_value);
            if (SUCCEEDED(bh))
            {
                s.nf->SetEventOnCompletion(s.nf_value, s.nev);
                // DEFECT E's discipline, unchanged: the one blocking call runs
                // with the mutex RELEASED, so the game's render thread is free
                // to record and signal while we are here.
                lk.unlock();
                const DWORD bwr = WaitForSingleObject(s.nev, 5000);
                lk.lock();
                if (bwr != WAIT_OBJECT_0) bh = E_FAIL;
            }
        }
        if (SUCCEEDED(bh))
        {
            for (unsigned q = 0; q < n; ++q)
            {
                // Counted here rather than in the loop below, because these are
                // exactly the frames the `newest` gate would have skipped.
                ++s.nr_skipped;
                seal_consume(s, bfi[q], bsl[q], false, false, 0u);
                s.consumed = bfi[q];
            }
        }
        else
        {
            // A failed batch must not be retried forever. Advance past it; the
            // seals it would have checked are lost and the next frame's gap
            // check will say so, which is the correct report.
            s.consumed = bulk_last;
        }
    }

    while (s.consumed < completed)
    {
        const unsigned long long f = s.consumed + 1;

        // The slot for frame f has been recycled if the producer is more than
        // RING frames ahead. That is a real, nameable condition - THE CONSUMER
        // FELL BEHIND - and it is emphatically NOT a producer drop. Conflating
        // the two would report our own slowness as the game's fault.
        if (completed >= f + stream_state::RING)
        {
            ++s.overrun;
            s.consumed = f;
            if (!s.said_overrun)
            {
                s.said_overrun = true;
                snprintf(line, sizeof line,
                         "[MGPU][SEAL] CONSUMER OVERRUN at f=%llu: the producer is %llu frames "
                         "ahead of us and ring depth is %u, so this slot was rewritten before it "
                         "was read. This is OUR slowness, not a dropped frame - counted "
                         "separately for exactly that reason. Said once; the summary carries the "
                         "total.",
                         f, completed - f, stream_state::RING);
                mgpu::diag::warn(line);
            }
            continue;
        }

        unsigned slot = (unsigned)((f - 1) % stream_state::RING);

        // FAULT "stale": read the PREVIOUS slot once, at f=40. This is a
        // consumer-side injection on purpose - it models the ring's indexing
        // being wrong rather than the transport being wrong, and those have
        // different fixes. Expected diagnosis: RING ALIAS (the seal's own
        // slot_index will not match the slot we read) plus REORDERED.
        if (s.fault_id == stream_state::FK_STALE && f == 40)
            slot = (slot + stream_state::RING - 1) % stream_state::RING;

        const UINT64 slot_off = (UINT64)slot * s.slot_bytes;

        // ---- P6.4: DO NOT RUN NR ON A FRAME THAT IS ALREADY STALE ----
        //
        // stream_poll consumes EVERY arrived frame before it returns, and the
        // loop presents once afterwards. When the consumer is behind, that
        // meant evaluating six frames and showing the last one. The Passes=2
        // run on 2026-09-05 did exactly that: 2651 frames of neural work, 451
        // presents - 83% of it computed and discarded, which is what turned
        // "17.5 ms of work against a 16.9 ms budget" into a 9 fps window.
        //
        // The seal is still read for every frame, because identity and ordering
        // are the point of the instrument and cost microseconds. Only the
        // EVALUATES are skipped, and only for frames a newer one has already
        // superseded. Those are counted separately from `dropped`: a frame we
        // chose not to denoise because it was already old is not the same event
        // as a frame the transport lost, and conflating them would report our
        // own scheduling as a fault.
        //
        // DECLARED HERE, at the frame's scope, NOT inside the command-list
        // block below: the timestamp read and the liveness sample both test it
        // and both live after that block closes.
        const bool newest = (f >= completed);
        const bool run_nr = s.nr_ok && newest && s.neural;
        if (!newest) ++s.nr_skipped;

        // ---- R26: the unpack goes to the COPY queue, one frame ahead when
        // there is a frame to go ahead to ----
        //
        // WHAT THE PREFETCH CAN AND CANNOT BUY. READ THIS BEFORE READING A
        // FLAT RESULT AS A FAILURE.
        //
        // This loop blocks on the direct fence for frame f before it records
        // frame f+1. A copy submitted inside frame f's own iteration therefore
        // runs strictly BEFORE that frame's evaluate and overlaps nothing: the
        // queue split alone moves the copy to another engine and changes no
        // wall-clock total. The only overlap available here is a frame that
        // HAS ALREADY ARRIVED while we are still working on its predecessor -
        // its copy is submitted first and runs on the DMA engine while the 3D
        // engine is inside EvaluateFeature.
        //
        // In a perfectly paced run - completed == consumed+1 at every poll,
        // which is what the 1.06x control run was - that condition never holds
        // and the overlap is ZERO BY CONSTRUCTION, not by fault. Getting it in
        // the steady state means deferring the per-frame CPU wait, which means
        // double-buffering the model's OUTPUT as well, which changes which
        // frame is on screen. That is a different decision on a different
        // property and it is deliberately not taken here.
        //
        // cq_ahead counts the frames that actually got the overlap, so the run
        // states which case it was instead of leaving it to be assumed.
        //
        // ONE FRAME OF PREFETCH, NEVER TWO: tex_in has two halves, so frame
        // f+2 would land in the one frame f is reading. The bound and the pair
        // are the same fact.
        unsigned ii = 0;
        bool use_cq = false;
        if (s.copy_queue && run_nr && s.cq != nullptr)
        {
            ii = (unsigned)(f & 1ull);
            // Never re-issue for a frame the overrun check already walked past.
            if (s.cq_issued + 1 < f) s.cq_issued = f - 1;
            // MODE 1: only ever arm a frame the CPU already knows has landed,
            // so the overlap needs a backlog and a paced run gets none.
            // MODE 2: arm f+1 WHETHER OR NOT IT HAS ARRIVED. The queue wait in
            // stream_cq_submit holds it on the GPU until the game signals, so
            // the copy starts at the instant of arrival rather than at the
            // bridge thread's next poll - which is where the steady-state
            // overlap actually comes from.
            //
            // STILL EXACTLY ONE AHEAD IN BOTH MODES. tex_in has two halves;
            // f+2 would land in the one frame f is reading. The bound and the
            // pair are the same fact and neither mode may exceed it.
            const unsigned long long want = (s.cq_gated || completed > f) ? (f + 1) : f;
            bool ok = true;
            while (ok && s.cq_issued < want)
            {
                const unsigned long long n = s.cq_issued + 1;
                ok = stream_cq_submit(s, n);
                if (ok)
                {
                    s.cq_issued = n;
                    if (n > f) ++s.cq_ahead;
                    if (n > completed) ++s.cq_spec;   // armed before arrival - mode 2 only
                }
            }
            use_cq = (s.cq_issued >= f);
            if (!use_cq) ++s.cq_fails;
        }

        HRESULT h = s.na->Reset();
        if (SUCCEEDED(h)) h = s.nl->Reset(s.na, nullptr);
        if (SUCCEEDED(h))
        {
            if (s.ts_ok) s.nl->EndQuery(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 0);

            s.nl->CopyBufferRegion(s.nseal, (UINT64)slot * SEAL_STRIDE,
                                   s.nxfer, slot_off, sizeof(MgpuSeal));

            if (s.ts_ok) s.nl->EndQuery(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 1);

            // ---- P4.1: the neural stage, in the SAME list as the seal read ----
            //
            // Unpack this slot's payload into the NR input, evaluate, and take a
            // small sample of the output. One list, one submission, one wait per
            // consumed frame - which is also why the consumer's pace with the
            // stage attached is directly comparable to its pace without it.
            if (run_nr)
            {
                // R26: on the copy-queue path this copy has already been
                // submitted on the DMA engine and the direct queue only Waits
                // for it (below, before ExecuteCommandLists). The inline copy
                // is still the shipped path and is still the fallback for any
                // single frame whose copy submission failed.
                if (!use_cq)
                {
                    D3D12_TEXTURE_COPY_LOCATION us{}, ud{};
                    us.pResource = s.nxfer;
                    us.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    us.PlacedFootprint = s.fp;
                    us.PlacedFootprint.Offset = slot_off + SEAL_STRIDE;
                    ud.pResource = s.tex_in[ii];
                    ud.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    ud.SubresourceIndex = 0;
                    s.nl->CopyTextureRegion(&ud, 0, 0, 0, &us, nullptr);
                }
                // THE BEFORE-STATE, AND WHY IT IS NOT ONE CONSTANT.
                //   use_cq        : COMMON. The copy queue's submission decayed
                //                   it there; see stream_cq_submit.
                //   inline copy   : COPY_DEST. A texture in COMMON used as a
                //                   copy destination on a DIRECT queue is
                //                   implicitly promoted, and promotion to a
                //                   write state does not decay - so it is
                //                   genuinely in COPY_DEST by this line,
                //                   whichever mode the run is in.
                barrier(s.nl, s.tex_in[ii],
                        use_cq ? D3D12_RESOURCE_STATE_COMMON
                               : D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                // ---- T1b: depth, and it follows tex_in exactly ----
                //
                // Same source buffer, same slot, a different offset the seal
                // describes. Inline when the copy queue is off; already in the
                // batch when it is on. The before-state is the same two cases
                // and for the same two reasons, so it is written the same way
                // rather than reasoned about a second time.
                const bool depth_here =
                    (s.depth_mode != 0 && s.tex_depth[ii] != nullptr &&
                     s.depth_bytes != 0 && s.depth_slot_valid[slot] != 0u);
                if (depth_here)
                {
                    if (!use_cq)
                    {
                        D3D12_TEXTURE_COPY_LOCATION dsu{}, ddu{};
                        dsu.pResource = s.nxfer;
                        dsu.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dsu.PlacedFootprint = s.depth_fp;
                        dsu.PlacedFootprint.Offset = slot_off + s.depth_off;
                        ddu.pResource = s.tex_depth[ii];
                        ddu.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        ddu.SubresourceIndex = 0;
                        s.nl->CopyTextureRegion(&ddu, 0, 0, 0, &dsu, nullptr);
                        ++s.depth_unpacks;
                    }
                    barrier(s.nl, s.tex_depth[ii],
                            use_cq ? D3D12_RESOURCE_STATE_COMMON
                                   : D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                // ---- R78: MVec, and it follows depth exactly ----
                //
                // Third region of the same slot, same two before-state cases,
                // written the same way rather than reasoned about a third
                // time. mvec_slot_valid is what makes a frame the bind hook
                // missed unpack nothing and bind nothing.
                const bool mvec_here =
                    (s.mvec_mode == 3 && s.tex_mvec_r[ii] != nullptr &&
                     s.mvec_bytes2 != 0 && s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u);
                if (mvec_here)
                {
                    if (!use_cq)
                    {
                        D3D12_TEXTURE_COPY_LOCATION msu{}, mdu{};
                        msu.pResource = s.nxfer;
                        msu.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        msu.PlacedFootprint = s.mvec_fp2;
                        msu.PlacedFootprint.Offset = slot_off + s.mvec_off;
                        mdu.pResource = s.tex_mvec_r[ii];
                        mdu.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        mdu.SubresourceIndex = 0;
                        s.nl->CopyTextureRegion(&mdu, 0, 0, 0, &msu, nullptr);
                        ++s.mvec_unpacks;
                    }
                    barrier(s.nl, s.tex_mvec_r[ii],
                            use_cq ? D3D12_RESOURCE_STATE_COMMON
                                   : D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                // ---- P8.0: the motion vector field ----
                // A CONSTANT field costs one upload and only when it changes,
                // so the steady state adds nothing to this list. The cost that
                // matters is whether EvaluateFeature itself gets more expensive
                // with MVec bound, and mark 2->3 already measures that. No new
                // timestamp mark is needed and none is added.
                if ((s.mvec_mode == 1 || s.mvec_mode == 2) &&
                    s.tex_mvec != nullptr && s.mvec_cpu != nullptr &&
                    s.mvec_dirty)
                {
                    for (UINT row = 0; row < s.height; ++row)
                    {
                        float *dst = reinterpret_cast<float *>(
                            s.mvec_cpu + (size_t)row * (size_t)s.mvec_fp.Footprint.RowPitch);
                        for (UINT col = 0; col < s.width; ++col)
                        {
                            dst[col * 2 + 0] = s.mvec_dx;
                            dst[col * 2 + 1] = s.mvec_dy;
                        }
                    }
                    if (s.mvec_in_read)
                        barrier(s.nl, s.tex_mvec,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION ms{}, md{};
                    ms.pResource = s.mvec_up;
                    ms.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    ms.PlacedFootprint = s.mvec_fp;
                    md.pResource = s.tex_mvec;
                    md.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    md.SubresourceIndex = 0;
                    s.nl->CopyTextureRegion(&md, 0, 0, 0, &ms, nullptr);
                    barrier(s.nl, s.tex_mvec, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    s.mvec_in_read = true;
                    s.mvec_dirty = false;
                    ++s.mvec_fills;
                }

                if (s.ts_ok) s.nl->EndQuery(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 2);

                // ---- P6.0: the pass chain ----
                //
                // Pass 1 reads tex_in and writes tex_out. Every later pass
                // reads what the one before it wrote and writes the other UAV
                // texture. Transport happened ONCE, above; only this loop
                // multiplies. That asymmetry is the entire architectural claim
                // and this is the first code that exercises it.
                //
                // The synchronisation between passes is not optional: pass k+1
                // reads the texture pass k wrote, on the same queue, and
                // without a barrier the driver is free to overlap them. The
                // state transition below serves as that barrier - see the note
                // inside the loop.
                // P8.1. Whether the LAST pass actually wrote its output. Only
                // that texture may become nr_final.
                // ---- R87: how many GAME frames since the last evaluate ----
                //
                // First evaluate of a run has no predecessor, so 1. A consumer
                // that never falls behind reads 1 forever and this is inert.
                // B1: measured in GAME frames, not sealed frames. With the
                // stride off these are the same number and this is identical
                // to every published run; with it on, only this version is
                // true. Falls back to the seal index if the slot never got a
                // game-frame stamp, which can only happen on a frame sealed
                // before this field existed.
                const unsigned long long gnow =
                    (s.slot_game_frame[slot] != 0ull) ? s.slot_game_frame[slot] : f;
                unsigned long long mv_elapsed = 1ull;
                if (s.nr_last_eval_fi != 0 && gnow > s.nr_last_eval_fi)
                    mv_elapsed = gnow - s.nr_last_eval_fi;
                if (mv_elapsed > 64ull) mv_elapsed = 64ull;   // a resync, not motion
                {
                    const unsigned bkt = (unsigned)((mv_elapsed >= 8ull) ? 7ull
                                                                        : (mv_elapsed - 1ull));
                    ++s.mvec_elapsed_hist[bkt];
                    if (mv_elapsed > s.mvec_elapsed_max) s.mvec_elapsed_max = mv_elapsed;
                }
                s.nr_last_eval_fi = gnow;   // B1: game frames, see above
                const float mv_el = (float)mv_elapsed;

                // ---- R102: the evaluated extent ----
                //
                // Computed here rather than at arm because s.width and s.height
                // are only real once a seal has carried them across, and this
                // is inside the branch where that is already true.
                //
                // Rounded DOWN to a multiple of 8 and floored at 64. An extent
                // that is not a sane multiple risks measuring the model's
                // internal tiling rather than its area, which is the one
                // reading that would make the whole test uninterpretable.
                //
                // THE DEPTH AND MVEC SUBRECTS ARE DELIBERATELY NOT TOUCHED.
                // Those describe the extent of VALID DATA IN THEIR OWN
                // RESOURCE - depth really is 2560x1440 of depth - and whether
                // the model expects them to co-shrink with the colour subrect
                // or to stay native is not something this file knows. Guessing
                // would put an unknown inside the one measurement that exists
                // to remove one. So the test is run COLOUR-ONLY (Depth=0,
                // MVec=0), which is sound because 2026-09-11 established the
                // evaluate is invariant to those inputs: 15.049 with all three
                // bound against 15.054 with colour alone. If the scaling law
                // holds and this becomes a shipping lever, the correspondence
                // question has to be answered before depth and vectors come
                // back - and it is a separate question from this one.
                UINT ew = s.width, eh = s.height;

                // Phase 1: the calibration owns the extent while it runs. Point
                // A is the full frame, point B is CC_PCT_B per axis. Two points
                // determine the line; a third would only test linearity, and
                // R102 already did that to 1.9% across a 4x range.
                unsigned eff_pct = s.subrect_pct;
                if (s.cc_state == 1u)      eff_pct = 100u;
                else if (s.cc_state == 2u) eff_pct = stream_state::CC_PCT_B;

                if (eff_pct != 100u)
                {
                    ew = (UINT)(((unsigned long long)s.width  * eff_pct) / 100ull) & ~7u;
                    eh = (UINT)(((unsigned long long)s.height * eff_pct) / 100ull) & ~7u;
                    if (ew < 64u) ew = 64u;
                    if (eh < 64u) eh = 64u;
                    if (ew > s.width)  ew = s.width;
                    if (eh > s.height) eh = s.height;
                    if (!s.said_subrect)
                    {
                        s.said_subrect = true;
                        char rl[420];
                        snprintf(rl, sizeof rl,
                                 "[MGPU][R102] evaluating %ux%u of %ux%u (%.1f%% of the area, "
                                 "base 0,0). Compare this run's P2.2 evaluate bracket against "
                                 "the full-frame figure for the same scene: 15.05 mean / 12.83 "
                                 "floor at 2560x1440 on 2026-09-11. Said once.",
                                 ew, eh, s.width, s.height,
                                 100.0 * ((double)ew * (double)eh)
                                       / ((double)s.width * (double)s.height));
                        mgpu::diag::info(rl);
                    }
                }

                // Phase 1: the area ACTUALLY evaluated, after the multiple-of-8
                // rounding, not the nominal percentage. The solve uses this.
                if (s.cc_state == 1u || s.cc_state == 2u)
                    s.cc_area[s.cc_state - 1u] =
                        ((double)ew * (double)eh) / ((double)s.width * (double)s.height);

                // ---- C2-SR: reduce D -> R, then NR runs on the small pair ----
                //
                // One dispatch, two outputs. Colour is filtered, depth is not.
                // After this the model's whole world is R, which is why ew/eh
                // are overridden here and not merged into the subrect maths -
                // Subrect and SRUpscale are different ideas and mixing them in
                // one expression is how a run ends up measuring neither.
                if (s.sr_ready)
                {
                    ew = s.sr_w; eh = s.sr_h;

                    sr_write_descriptors(s, s.ndev_b, ii, s.tex_in[ii],
                                         (s.depth_mode == 1) ? s.tex_depth[ii] : nullptr);

                    barrier(s.nl, s.sr_color[ii], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    ID3D12DescriptorHeap *heaps[1] = { s.ds_heap };
                    s.nl->SetDescriptorHeaps(1, heaps);
                    s.nl->SetComputeRootSignature(s.ds_rs);
                    s.nl->SetPipelineState(s.ds_pso);
                    D3D12_GPU_DESCRIPTOR_HANDLE gh =
                        s.ds_heap->GetGPUDescriptorHandleForHeapStart();
                    gh.ptr += (UINT64)(ii * 4u) * s.ds_inc;
                    s.nl->SetComputeRootDescriptorTable(0, gh);
                    const UINT cnst[4] = { s.width, s.height, s.sr_w, s.sr_h };
                    s.nl->SetComputeRoot32BitConstants(1, 4, cnst, 0);
                    s.nl->Dispatch((s.sr_w + 7u) / 8u, (s.sr_h + 7u) / 8u, 1u);

                    barrier(s.nl, s.sr_color[ii], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                bool final_pass_ok = false;
                for (unsigned pi = 0; pi < s.passes; ++pi)
                {
                    ID3D12Resource *src = (pi == 0)
                                            ? s.tex_in[ii]
                                            : ((pi % 2u == 1u) ? s.tex_out : s.tex_pong);
                    ID3D12Resource *dst = (pi % 2u == 0u) ? s.tex_out : s.tex_pong;
                    // C2-SR: the first pass reads the reduced colour and the
                    // last writes the reduced output, which is SR's input.
                    // Intermediate passes keep ping-ponging in the big pair,
                    // which is why SRUpscale refuses Passes>1 at arm rather
                    // than silently mixing two resolutions in one chain.
                    if (s.sr_ready)
                    {
                        if (pi == 0)             src = s.sr_color[ii];
                        if (pi + 1u >= s.passes) dst = s.sr_nrout;
                    }

                    // STATE, not just a UAV barrier. Pass 1's input (tex_in) is
                    // put in NON_PIXEL_SHADER_RESOURCE above, which is the only
                    // state this project has ever handed NGX an input in. A
                    // later pass reads a texture that has been sitting in
                    // UNORDERED_ACCESS because the pass before it wrote there,
                    // and feeding NGX an input in that state is untested here -
                    // so it is transitioned to the same state pass 1's input
                    // uses, and put back afterwards. The transition also IS the
                    // write-to-read dependency between consecutive passes, so
                    // no separate UAV barrier is needed.
                    if (pi > 0)
                        barrier(s.nl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                    s.nr_params->Set("DLSSNR.Color", src);
                    s.nr_params->Set("DLSSNR.Output", dst);
                    s.nr_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
                    s.nr_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
                    // R102: ew/eh, not s.width/s.height. They ARE s.width and
                    // s.height at Subrect=100, so a default run sets exactly
                    // what every published run set. Colour and output take the
                    // same extent because this stage is 1:1 - it denoises, it
                    // does not upscale, and a mismatched pair here would be
                    // asking for a resample nobody requested.
                    s.nr_params->Set("DLSSNR.ColorSubrectWidth",  (unsigned int)ew);
                    s.nr_params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)eh);
                    s.nr_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
                    s.nr_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
                    s.nr_params->Set("DLSSNR.OutputSubrectWidth",  (unsigned int)ew);
                    s.nr_params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)eh);
                    // P8.0. Bound for EVERY pass: each pass owns its own handle
                    // and therefore its own history, so each needs the
                    // reprojection. Unset when the mode is off, which leaves
                    // this evaluate byte-identical to every published run.
                    // R78. THREE CASES NOW, AND THE SUBRECT IS WHY THEY CANNOT
                    // SHARE A BRANCH. The synthetic field is built at
                    // s.width/s.height, so its subrect is the colour subrect.
                    // The REAL field is the game's own buffer, which on this
                    // title is at the DISPLAY extent while colour renders at
                    // ~65% of it - binding it with the colour subrect would
                    // hand NGX a window into the top-left ~65% of the motion
                    // and call it the whole frame. R30 established that each
                    // input carries its own subrect; this is the input that
                    // makes that mandatory rather than tidy.
                    if (s.mvec_mode == 3 && s.tex_mvec_r[ii] != nullptr &&
                        s.mvec_bytes2 != 0 && s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u)
                    {
                        s.nr_params->Set("DLSSNR.MVec", s.tex_mvec_r[ii]);
                        // R87: scaled by the frames that actually elapsed. With
                        // a consumer that never falls behind this multiplies by
                        // 1.0 and the evaluate is byte-identical to R83's.
                        mgpu::calibrator::override_scale(s.mvec_scale_x, s.mvec_scale_y);   // R105
                        s.nr_params->Set("DLSSNR.MVecScaleX", s.mvec_scale_x * mv_el);
                        s.nr_params->Set("DLSSNR.MVecScaleY", s.mvec_scale_y * mv_el);
                        mgpu::calibrator::apply_jitter_offset(s.nr_params, s.mvec_scale_x, s.mvec_scale_y);   // R104
                        s.nr_params->Set("DLSSNR.MVecSubrectBaseX", 0u);
                        s.nr_params->Set("DLSSNR.MVecSubrectBaseY", 0u);
                        s.nr_params->Set("DLSSNR.MVecSubrectWidth",  (unsigned int)s.mvec_w);
                        s.nr_params->Set("DLSSNR.MVecSubrectHeight", (unsigned int)s.mvec_h);
                        if (pi == 0) ++s.mvec_bound;
                    }
                    else if (s.mvec_mode == 3)
                    {
                        // R61's rule, applied to motion. The bind hook did not
                        // fire this frame - a menu, a load screen. UNSET it.
                        // Leaving the previous frame's vectors bound would
                        // reproject the history against a motion that did not
                        // happen, which is worse than no reprojection at all.
                        s.nr_params->Set("DLSSNR.MVec", (ID3D12Resource *)nullptr);
                        if (pi == 0) ++s.mvec_skipped;
                    }
                    else if (s.mvec_mode != 0 && s.tex_mvec != nullptr)
                    {
                        s.nr_params->Set("DLSSNR.MVec", s.tex_mvec);
                        mgpu::calibrator::override_scale(s.mvec_scale_x, s.mvec_scale_y);   // R105
                        s.nr_params->Set("DLSSNR.MVecScaleX", s.mvec_scale_x);
                        s.nr_params->Set("DLSSNR.MVecScaleY", s.mvec_scale_y);
                        mgpu::calibrator::apply_jitter_offset(s.nr_params, s.mvec_scale_x, s.mvec_scale_y);   // R104
                        s.nr_params->Set("DLSSNR.MVecSubrectBaseX", 0u);
                        s.nr_params->Set("DLSSNR.MVecSubrectBaseY", 0u);
                        s.nr_params->Set("DLSSNR.MVecSubrectWidth",  (unsigned int)s.width);
                        s.nr_params->Set("DLSSNR.MVecSubrectHeight", (unsigned int)s.height);
                    }

                    // ---- T1b: DEPTH, bound only on frames that actually have it ----
                    //
                    // Mode 1 binds. Mode 2 does not, and that is the whole
                    // difference between them: identical bytes on the bus,
                    // identical unpack, and an evaluate that is byte-identical
                    // to a colour-only run. Any frametime difference between
                    // the two arms is the MODEL getting more expensive with
                    // depth bound, and nothing else.
                    //
                    // The condition is recomputed here rather than carried in
                    // from the unpack: it is three loads, and it cannot be
                    // wrong about a scope.
                    if (s.depth_mode == 1 && s.tex_depth[ii] != nullptr &&
                        s.depth_slot_valid[slot] != 0u)
                    {
                        s.nr_params->Set("DLSSNR.Depth", s.tex_depth[ii]);
                        s.nr_params->Set("DLSSNR.DepthSubrectBaseX", 0u);
                        s.nr_params->Set("DLSSNR.DepthSubrectBaseY", 0u);
                        s.nr_params->Set("DLSSNR.DepthSubrectWidth",  (unsigned int)s.depth_w);
                        s.nr_params->Set("DLSSNR.DepthSubrectHeight", (unsigned int)s.depth_h);
                        s.nr_params->Set("DLSSNR.DepthInverted",
                                         (unsigned int)s.depth_inverted);
                        if (pi == 0) ++s.depth_bound;
                    }
                    else if (s.depth_mode == 1)
                    {
                        // R61. ReShade had no depth this frame - a menu, a
                        // scene change. UNSET it. Leaving the previous frame's
                        // texture bound would hand the model a stale depth for
                        // a world that has changed, which is worse than none:
                        // the model would reproject confidently against a
                        // geometry that is gone.
                        s.nr_params->Set("DLSSNR.Depth", (ID3D12Resource *)nullptr);
                        if (pi == 0) ++s.depth_skipped;
                    }
                    // P6.2: per pass, not one hardcoded value for all of them.
                    s.nr_params->Set("DLSSNR.Intensity", s.intensity[pi]);
                    // P7.9. Only when explicitly enabled - see stream_state.
                    // Set BEFORE the generic Set.<key> loop so an ini line still
                    // overrides a slider, keeping the escape hatch on top.
                    if (s.tuning_on)
                    {
                        s.nr_params->Set("DLSSNR.LocalToneStrength",      s.tone_strength);
                        s.nr_params->Set("DLSSNR.LocalStructureStrength", s.structure_strength);
                        s.nr_params->Set("DLSSNR.SkinStructureStrength",  s.skin_strength);
                        s.nr_params->Set("DLSSNR.Style",                  s.style);
                        s.nr_params->Set("DLSSNR.UseAutoMask", s.auto_mask ? 1u : 0u);
                    }
                    // P6.2: whatever the ini named, verbatim, before every
                    // evaluate. Applied AFTER Intensity so a Set.DLSSNR.Intensity
                    // line deliberately wins - that is the escape hatch if the
                    // per-pass path ever needs to be bypassed.
                    for (unsigned si = 0; si < s.set_n; ++si)
                    {
                        if (s.set_is_float[si]) s.nr_params->Set(s.set_key[si], s.set_f[si]);
                        else                    s.nr_params->Set(s.set_key[si], s.set_u[si]);
                    }
                    // RESET ON THE FIRST FRAME ONLY, PER HANDLE. Every probe
                    // before the stream set Reset=1 on every evaluate, because
                    // each was an independent experiment and history between
                    // them would have contaminated the control. A stream is the
                    // opposite case: dlssnr_prev_output is temporal history and
                    // it is supposed to carry. Each pass owns its own feature
                    // handle precisely so that its history is the PREVIOUS
                    // FRAME's output of that same pass, not the previous pass of
                    // this frame - so the flag is per-frame, not per-pass. If
                    // the output ever looks smeared or ghosted, this is the
                    // first line to try at 1.
                    // P8.1. The comment above says this is the first line to
                    // try at 1 when the output looks smeared or ghosted. It is
                    // now a setting rather than an edit, because which passes
                    // keep history is exactly the variable in question.
                    unsigned rst = s.nr_first ? 1u : 0u;
                    if (s.pass_reset == 2) rst = 1u;
                    else if (s.pass_reset == 1 && pi > 0) rst = 1u;
                    s.nr_params->Set("DLSSNR.Reset", rst);

                    const NVSDK_NGX_Result er =
                        s.nr_eval(s.nl, s.nr_handle[pi], s.nr_params, nullptr);
                    ++s.nr_evals;
                    if (pi + 1u >= s.passes)
                        final_pass_ok = (er == NVSDK_NGX_Result_Success);
                    if (er != NVSDK_NGX_Result_Success)
                    {
                        ++s.nr_fails;
                        if (s.nr_fails <= 3)
                        {
                            snprintf(line, sizeof line,
                                     "[MGPU][P4.1] EvaluateFeature f=%llu pass %u/%u: "
                                     "0x%08X (%s)",
                                     f, pi + 1, s.passes, (unsigned)er, ngx_result_name(er));
                            mgpu::diag::error(line);
                        }
                    }

                    // Put it back: every UAV texture must be in
                    // UNORDERED_ACCESS at the end of the list, because that is
                    // the state the next frame - and stream_present_source -
                    // both assume.
                    if (pi > 0)
                        barrier(s.nl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                s.nr_first = false;
                // P6.4: `passes` can change between frames now, so the final
                // texture is recomputed here rather than fixed at arm time.
                //
                // P8.1: ONLY WHEN THE LAST PASS WROTE. tex_out and tex_pong are
                // never cleared, so advancing after a failed evaluate points
                // the presenter at undefined memory. Holding the previous good
                // frame instead shows a stale but VALID image - which reads as
                // a stall rather than as a fault in the model, and is the
                // difference between a diagnosable symptom and a black screen.
                // R26. The half THIS frame fed the model, for Present=in and
                // the split's left half. It is safe for the presenter to read
                // it after this poll returns: the loop never prefetches on its
                // last iteration (want == f when completed == f), so no copy is
                // ever in flight into the half the presenter is about to show.
                s.tex_in_show = s.tex_in[ii];

                // ---- C2-SR: enlarge R -> D with a real upscaler ----
                //
                // The enlargement is done by a temporal upscaler with the
                // motion vectors behind it, not by a filter. That is the whole
                // argument for this path over a blit: a blit band-limits the
                // detail NR just synthesised, and this does not.
                //
                // Section 11's disclaimer still applies and is not cancelled by
                // doing it here: SR is upscaling a TONE-MAPPED image, which
                // costs it headroom above white. It is a trade, not a free win.
                if (s.sr_ready && final_pass_ok)
                {
                    barrier(s.nl, s.sr_nrout, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                    s.sr_params->Set(NVSDK_NGX_Parameter_Color,  s.sr_nrout);
                    s.sr_params->Set(NVSDK_NGX_Parameter_Output, s.tex_out);
                    s.sr_params->Set(NVSDK_NGX_Parameter_Depth,  s.sr_depth);
                    s.sr_params->Set(NVSDK_NGX_Parameter_MotionVectors,
                                     (s.mvec_mode != 0 && s.tex_mvec_r[ii] != nullptr)
                                         ? s.tex_mvec_r[ii] : nullptr);
                    // The vectors are already at R, so the scale is the game's
                    // own and not a ratio between two extents.
                    // The game's scale, corrected by R/mvec. At SRScale=0 the two
                    // extents are equal, the factors are 1.0, and this is
                    // byte-identical to what shipped before.
                    s.sr_params->Set(NVSDK_NGX_Parameter_MV_Scale_X,
                                     s.mvec_scale_x * s.sr_mv_fix_x);
                    s.sr_params->Set(NVSDK_NGX_Parameter_MV_Scale_Y,
                                     s.mvec_scale_y * s.sr_mv_fix_y);
                    // JITTER IS ZERO, AND THAT IS CORRECT HERE - but read the
                    // consequence. The frame we hand SR has already been
                    // resolved by the GAME'S DLSS and tone mapped; it carries
                    // no sub-pixel jitter, because the jitter was consumed
                    // upstream. Passing the game's jitter values would tell SR
                    // about sampling diversity that is not in the pixels.
                    //
                    // The consequence is that SR gets none of the sub-pixel
                    // diversity it normally reconstructs FROM. It degrades
                    // toward a temporally stable spatial upscaler. That is
                    // still better than a filtered blit, which is the honest
                    // claim - not that this recovers native detail.
                    s.sr_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
                    s.sr_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
                    s.sr_params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                    s.sr_params->Set(NVSDK_NGX_Parameter_ExposureTexture, s.sr_expose);
                    s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
                    s.sr_params->Set(NVSDK_NGX_Parameter_Reset, s.sr_first ? 1u : 0u);
                    s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                                     (unsigned int)s.sr_w);
                    s.sr_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
                                     (unsigned int)s.sr_h);
                    s.sr_first = false;

                    const NVSDK_NGX_Result sres =
                        s.sr_eval(s.nl, s.sr_handle, s.sr_params, nullptr);
                    ++s.sr_evals;
                    if (sres != NVSDK_NGX_Result_Success)
                    {
                        ++s.sr_fails;
                        if (s.sr_fails <= 3)
                        {
                            char sl2[300];
                            snprintf(sl2, sizeof sl2,
                                     "[MGPU][C2-SR] EvaluateFeature f=%llu: 0x%08X (%s)",
                                     f, (unsigned)sres, ngx_result_name(sres));
                            mgpu::diag::error(sl2);
                        }
                    }

                    barrier(s.nl, s.sr_nrout, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }

                if (final_pass_ok)
                {
                    // C2-SR writes the display-extent result into tex_out
                    // whatever the pass parity did at R, so the presenter takes
                    // tex_out directly rather than the ping-pong winner.
                    s.nr_final = s.sr_ready
                                   ? s.tex_out
                                   : ((s.passes % 2u == 1u) ? s.tex_out : s.tex_pong);
                }
                else
                {
                    ++s.nr_unwritten;
                    if (s.nr_unwritten <= 3)
                    {
                        snprintf(line, sizeof line,
                                 "[MGPU][P8.1] f=%llu the final pass did not write - holding the "
                                 "previous neural frame rather than presenting an unwritten "
                                 "texture. THE FAILURE IS THE EvaluateFeature ABOVE, not this "
                                 "line. Logged three times, then counted silently; the run "
                                 "summary carries the total.", f);
                        mgpu::diag::error(line);
                    }
                }

                if (s.ts_ok) s.nl->EndQuery(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 3);

                if (!s.profile)
                {
                // P6.0: sample the LAST pass's output, not tex_out. With
                // Passes=1 nr_final IS tex_out; with an even pass count it is
                // tex_pong, and sampling tex_out there would compare the
                // second-to-last pass frame over frame while the window showed
                // the last one - a liveness check watching a different image
                // from the one on screen.
                ID3D12Resource *const fin = s.nr_final;
                barrier(s.nl, fin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION ss{}, sd{};
                ss.pResource = fin;
                ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                ss.SubresourceIndex = 0;
                sd.pResource = s.nr_read;
                sd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sd.PlacedFootprint = s.nr_fp;
                D3D12_BOX box{ 0, 0, 0, 64, 4, 1 };
                s.nl->CopyTextureRegion(&sd, 0, 0, 0, &ss, &box);
                barrier(s.nl, fin, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                // Back to the mode's resting state - COPY_DEST on the shipped
                // path, COMMON with the copy queue on, so the copy queue can
                // take this half again two frames from now.
                barrier(s.nl, s.tex_in[ii], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        s.tex_in_rest);

                // T1b: depth has the same lifecycle and therefore the same
                // resting state - tex_in_rest is COPY_DEST on the shipped path
                // and COMMON with the copy queue on, and depth is written by
                // whichever of the two wrote tex_in.
                if (s.depth_mode != 0 && s.tex_depth[ii] != nullptr &&
                    s.depth_slot_valid[slot] != 0u)
                    barrier(s.nl, s.tex_depth[ii],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.tex_in_rest);

                // R78: and MVec, same lifecycle, same resting state. THE
                // CONDITION MUST MATCH THE UNPACK'S EXACTLY - a frame that was
                // transitioned INTO NON_PIXEL_SHADER_RESOURCE and not put back
                // would leave the next frame's barrier naming a state the
                // resource is not in, which the debug layer flags and a release
                // build accepts in silence. That is the shape of bug this file
                // has a standing note about; it is written the same way as
                // depth's so the two cannot drift apart.
                if (s.mvec_mode == 3 && s.tex_mvec_r[ii] != nullptr &&
                    s.mvec_bytes2 != 0 && s.mvec_slot_valid[slot].load(std::memory_order_acquire) != 0u)
                    barrier(s.nl, s.tex_mvec_r[ii],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.tex_in_rest);

                if (s.ts_ok) s.nl->EndQuery(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 4);
            }

            // Resolve after every mark is written, never before: the resolve
            // reads the heap on the GPU timeline and a mark recorded after it
            // would not be in the buffer we map.
            if (s.ts_ok && run_nr)
                s.nl->ResolveQueryData(s.tsheap, D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                       stream_state::TS_MARKS, s.tsread, 0);

            h = s.nl->Close();
        }
        if (SUCCEEDED(h))
        {
            // R26. A QUEUE wait, not a list command, and it must be issued
            // before the submission it gates. This is the whole synchronisation
            // of the split: the direct queue does not start frame f's chain
            // until the copy queue has finished frame f's unpack, and the
            // barrier that a copy queue is not allowed to record happens on
            // this side of it.
            if (use_cq && s.cqf != nullptr) (void)s.nq->Wait(s.cqf, (UINT64)f);
            ID3D12CommandList *const ls[1] = { s.nl };
            s.nq->ExecuteCommandLists(1, ls);
            ++s.nf_value;
            h = s.nq->Signal(s.nf, s.nf_value);
            if (SUCCEEDED(h))
            {
                s.nf->SetEventOnCompletion(s.nf_value, s.nev);
                // DEFECT E. THE ONLY BLOCKING CALL IN THIS FUNCTION, AND IT
                // RUNS WITH THE MUTEX RELEASED. What the game thread does while
                // we are here is exactly what it should be free to do: record
                // its copies, signal its own fence, advance `produced`. None of
                // that touches the bridge-side objects this loop is using
                // (s.nl, s.nq, s.nev are bridge thread only), and the seal for
                // this frame was copied into s.nseal by the GPU before the wait
                // can clear - so nothing read after re-acquiring is stale.
                //
                // If the producer laps us while we are unlocked, that is the
                // overrun condition and the check at the top of the next
                // iteration names it. Being lapped is a fact about our speed;
                // blocking the game to avoid it never was.
                lk.unlock();
                const DWORD wr = WaitForSingleObject(s.nev, 5000);
                lk.lock();
                if (wr != WAIT_OBJECT_0) h = E_FAIL;
            }
        }
        if (FAILED(h)) { s.consumed = f; continue; }

        // D1: one checker, two callers. See seal_consume.
        seal_consume(s, f, slot, run_nr, use_cq, ii);
        s.consumed = f;
    }

    // ---- summary, once, after the producer has stopped and drained ----
    if (s.finished && s.consumed >= s.produced && !s.summarised)
    {
        s.summarised = true;

        // ---- R89: THE COUNTERS THAT ONLY EVER LIVED IN A PERIODIC LINE ----
        //
        // Ledger 13b, filed at R78 and never done. The depth, MVec and
        // alignment counters print every 300 frames and nowhere else, so a
        // 600-frame run shows them at f=301 and never again - which is exactly
        // how R87 lost the two-pass alignment histogram and cost a rig launch.
        // A run that ends should not have to be read backwards to find out
        // what it did.
        {
            unsigned long long al3 = 0;
            for (unsigned q = 2; q < 8; ++q) al3 += s.mvec_elapsed_hist[q];
            char fc[1100];
            snprintf(fc, sizeof fc,
                "[MGPU][R89] FINAL COUNTERS at %llu consumed frames. | DEPTH mode=%d "
                "valid=%llu invalid=%llu bound=%llu skipped=%llu mismatches=%llu "
                "disagreements=%llu | MVEC mode=%d copies=%llu missing=%llu lock-skipped=%llu "
                "size-rejected=%llu bound=%llu skipped=%llu mismatches=%llu disagreements=%llu "
                "| ALIGNMENT 1=%llu 2=%llu 3+=%llu worst=%llu. EVERY MISMATCH AND EVERY "
                "DISAGREEMENT SHOULD BE 0; lock-skipped above a handful means the consumer is "
                "holding its mutex too long; ALIGNMENT well below 100%% at 1 means GPU 1 is not "
                "keeping up with the frame interval, which is a throughput fact and not a vsync "
                "bug.",
                (unsigned long long)s.consumed,
                s.depth_mode, s.depth_valid_frames, s.depth_invalid_frames,
                s.depth_bound, s.depth_skipped, s.depth_contract, s.depth_flag_disagree,
                s.mvec_mode,
                s.mvec_copies.load(std::memory_order_relaxed),
                s.mvec_missing.load(std::memory_order_relaxed),
                s.mvec_lock_skips.load(std::memory_order_relaxed),
                s.mvec_size_rejects.load(std::memory_order_relaxed),
                s.mvec_bound, s.mvec_skipped, s.mvec_contract, s.mvec_flag_disagree,
                s.mvec_elapsed_hist[0], s.mvec_elapsed_hist[1], al3, s.mvec_elapsed_max);
            mgpu::diag::info(fc);
        }
        const double mean = (s.lat_n > 0) ? (s.lat_sum / (double)s.lat_n) : 0.0;
        // The first sample carries arm-to-first-poll time, which is not
        // transit. Reported, and excluded from the mean beside it, so both
        // numbers are available and neither is silently doing the other's job.
        const double mean_x = (s.lat_n > 1)
            ? ((s.lat_sum - s.first_lat) / (double)(s.lat_n - 1)) : 0.0;
        snprintf(line, sizeof line,
                 "[MGPU][SEAL] summary: produced=%llu consumed=%llu new=%llu dropped=%llu "
                 "reordered=%llu overrun=%llu bad_magic=%llu contract=%llu alias=%llu "
                 "resync=%llu fault=\"%s\"%s seal_version=%u",
                 s.produced, s.consumed, s.lat_n, s.dropped, s.reordered,
                 s.overrun, s.bad_magic, s.contract, s.alias, s.resync, s.fault,
                 s.fault_unimpl ? " (an UNIMPLEMENTED fault was requested - see the error above; "
                                  "this ran clean and proves nothing about the checker)" : "",
                 SEAL_VERSION);
        mgpu::diag::info(line);
        {
            // The rate ratio, out of quantities this consumer can observe, plus
            // the proof that every slot was used. A ring whose hits are not
            // even is a ring that is not rotating.
            char sl[300]; size_t off = 0;
            for (unsigned i = 0; i < stream_state::RING && off < sizeof sl - 24; ++i)
                off += (size_t)snprintf(sl + off, sizeof sl - off, "%s%u:%llu",
                                        (i == 0) ? "" : " ", i, s.slot_hits[i]);
            const double ratio = (s.lat_n > 0) ? ((double)s.polls / (double)s.lat_n) : 0.0;
            snprintf(line, sizeof line,
                     "[MGPU][SEAL] pacing: polls=%llu idle=%llu busy=%llu -> the consumer ran "
                     "%.2fx the producer's rate (polls per new frame). slot hits: %s - all %u "
                     "slots must appear and should be within one of each other.",
                     s.polls, s.idle_polls, s.polls - s.idle_polls, ratio,
                     sl, stream_state::RING);
            mgpu::diag::info(line);
            if (ratio < 1.5)
            {
                // D3. THIS LINE USED TO BLAME THE BRIDGE SWAPCHAIN'S REFRESH
                // RATE, and it was wrong. On 2026-09-11 the bounded runs found
                // work on 95.2% and 94.9% of polls (idle=65 of 1346, 70 of
                // 1363): the consumer is not waiting for a vblank, it is busy.
                // It is SERVICE-TIME limited, not refresh limited, and the old
                // text sent a day of work at the wrong thing.
                snprintf(line, sizeof line,
                         "[MGPU][SEAL] PACING MARGIN GONE (%.2fx) - the consumer is not keeping "
                         "up with the producer. Read `idle` on the line above FIRST, because it "
                         "says which of two different problems this is. idle near ZERO means the "
                         "consumer is SATURATED: it found work on almost every poll and its "
                         "service time is the limit, so the question is what that time is spent "
                         "on - see the P2.2 breakdown, and compare the GPU total there against "
                         "the wall-clock interval per consumed frame; the difference is CPU. "
                         "idle HIGH with a low ratio means the opposite, that the consumer is "
                         "being woken too rarely rather than running too slowly. Ring depth %u "
                         "is absorbing this for now and overrun is still 0, but one hitch or a "
                         "faster scene spends that margin.",
                         ratio, stream_state::RING);
                mgpu::diag::warn(line);
            }
        }
        snprintf(line, sizeof line,
                 "[MGPU][SEAL] latency ms: min=%.2f mean=%.2f max=%.2f (at f=%llu) n=%llu | "
                 "first sample %.2f (arm-to-first-poll, NOT transit); mean excluding it %.2f. "
                 "SUBMIT-TO-CONSUME only - it excludes the game's render before it and GPU 1's "
                 "present after it, and it is wall-clock around a CPU-visible completion rather "
                 "than a GPU timestamp. Perishable: one cabling, one link width.",
                 (s.lat_n > 0) ? s.lat_min : 0.0, mean, s.lat_max, s.lat_max_frame, s.lat_n,
                 s.first_lat, mean_x);
        mgpu::diag::info(line);

        // L1: the producer-side backlog, printed beside the latency it explains.
        if (s.q_n > 0)
        {
            char ql[1100]; int qw = 0;
            qw += snprintf(ql + qw, sizeof ql - (size_t)qw,
                           "[MGPU][L1] GPU 0 BACKLOG AT COPY-RECORD TIME, n=%llu mean=%.2f "
                           "max=%llu | frames behind:", s.q_n,
                           (double)s.q_sum / (double)s.q_n, s.q_max);
            for (unsigned b = 0; b < 8 && qw < (int)sizeof ql - 120; ++b)
                qw += snprintf(ql + qw, sizeof ql - (size_t)qw, " %u%s=%llu",
                               b, (b == 7) ? "+" : "", s.q_hist[b]);
            snprintf(ql + qw, sizeof ql - (size_t)qw,
                     ". THIS IS WHERE THE SUBMIT-TO-CONSUME LATENCY LIVES, and two "
                     "explanations have already been ruled out by measurement: ring depth "
                     "(the consumer idles on three quarters of its polls, so the ring is "
                     "nearly empty) and the game's queue depth (ReflexMode made no "
                     "difference, 62.68 -> 64.14 ms). A BACKLOG OF 1 IS THE FLOOR AND IS "
                     "STRUCTURAL: the signal for frame N is issued on frame N+1's event "
                     "because ReShade executes our list after the handler returns. Mass at 1 "
                     "means that fence lag IS the latency and the only lever is where the "
                     "signal is issued. Mass at 3-4 means GPU 0 is genuinely that far behind. "
                     "Mass at 0 with the latency unchanged would mean the copy itself is slow, "
                     "which would be the most interesting answer of the three.");
            mgpu::diag::info(ql);

            if (s.rw_on != 0u)
            {
                char rw[900]; int w = 0;
                w += snprintf(rw + w, sizeof rw - (size_t)w,
                              "[MGPU][RW] RING WINDOW ended at %u of %u slots, skipped %llu "
                              "frame(s) to stay inside it | held:", s.rw,
                              (unsigned)stream_state::RING, s.rw_skipped);
                for (unsigned d = stream_state::RW_MIN; d <= 6u && w < (int)sizeof rw - 120; ++d)
                    w += snprintf(rw + w, sizeof rw - (size_t)w, " %u=%u", d, s.rw_hist[d]);
                snprintf(rw + w, sizeof rw - (size_t)w,
                         ". ALL SIX SLOTS STAY ALLOCATED - only the usable depth moves, "
                         "because reallocating mid-stream is how P1.0's teardown crash comes "
                         "back. Skipped frames are NOT drops and NOT overruns: the consumer "
                         "chose the newest frame on purpose. Settling at %u with zero skips "
                         "means the depth was never needed; growing back to %u means it was.",
                         stream_state::RW_MIN, (unsigned)stream_state::RING);
                mgpu::diag::info(rw);
            }

            // ---- L6: THE UNIVERSAL LOW-LATENCY CHECK ----
            //
            // Reflex cannot be read portably - Streamline on some titles,
            // NvAPI on others, absent on many, and nothing at all on another
            // vendor. But the SYMPTOM is already measured here. A deep queue is
            // what a missing or disabled low-latency mode looks like from the
            // outside, whatever the title calls it.
            //
            // Measured on this rig, same scene, same preset, add-on loaded
            // but not streaming: enabling the game's own low-latency mode cost
            // about 15% of the frame rate and bought 7.8 ms of latency, with
            // the producer queue going from 1.89 frames deep to 1.00.
            //
            // AN EARLIER READING OF THIS PAIR SHOWED THE FRAME RATE DOUBLING.
            // It was an instrument failure, not a finding, and it is not
            // repeated here or anywhere public - a controlled A/B on the same
            // scene did not reproduce it. The queue-depth signature below is
            // the part that held up, so that is the part this check uses.
            {
                const double qm = (double)s.q_sum / (double)s.q_n;
                if (qm >= 2.5)
                    mgpu::diag::warn(
                        "[MGPU][L6] THE GAME IS QUEUEING DEEP (backlog 2.5+ frames). That is "
                        "what a disabled low-latency mode looks like from out here - Reflex, "
                        "Anti-Lag, or whatever this title calls it. The bridge's own cost is "
                        "unaffected, but the latency you feel is mostly this, and it is "
                        "roughly queue depth times frame time. SET Reflex=1 IN mgpu.ini - the "
                        "bridge sets the driver's low-latency mode on GPU 0 directly, which a "
                        "title that rewrites its own settings file cannot revert, and which "
                        "works whether or not the title exposes the option at all. Measured on "
                        "this rig it took the backlog from 1.88 to 1.01 and cost about 15% of "
                        "the frame rate. Turning it on in the game's menu does the same thing "
                        "where that menu exists.");
                else if (qm <= 1.6)
                    mgpu::diag::info(
                        "[MGPU][L6] The game's queue is shallow (backlog under 1.6 frames), "
                        "which is what a working low-latency mode looks like. The latency "
                        "left is the bridge's own and it is small.");
            }

        // L2, printed beside L1 so the two are never read apart.
        if (s.ready_n > 0)
        {
            char rl2[1100];
            snprintf(rl2, sizeof rl2,
                     "[MGPU][L2] READY-TO-CONSUME ms: min=%.2f mean=%.2f max=%.2f n=%llu. "
                     "THIS IS THE LATENCY A PLAYER EXPERIENCES. It starts at the first poll "
                     "that saw the produced fence reach the frame - the earliest instant the "
                     "data existed and we could have known - rather than at the copy-record "
                     "timestamp the line above uses. L1 showed GPU 0 is 3 frames behind when "
                     "we record, because the CPU runs 3 frames ahead and THE FRAME HAS NOT "
                     "BEEN RENDERED YET at that point. The copy is appended to that frame's "
                     "own list, so it executes the moment that frame's graphics finish and is "
                     "not queued behind anything. If this figure is near 10 ms, the 3-frame "
                     "floor was an artifact of where a timestamp was taken, three proposed "
                     "fixes were all aimed at nothing, and the GPU 0 copy queue is "
                     "unnecessary. If it is near 60, that reasoning is wrong.",
                     s.ready_min, s.ready_sum / (double)s.ready_n, s.ready_max, s.ready_n);
            mgpu::diag::info(rl2);
        }
        }

        if (s.neural)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P4.1] neural stage: %s | passes=%u | evaluates=%llu (=%llu frames "
                     "x %u passes) failures=%llu | output sample identical to the previous frame "
                     "%llu times (%.1f%%) | %llu frames had their NEURAL WORK SKIPPED because a "
                     "newer frame was already waiting - those are not drops, their seals were "
                     "checked and they are counted here so the evaluate count and the frame "
                     "count are not expected to agree. ONE FEATURE HANDLE PER PASS, DLSSNR.Reset=1 on the "
                     "first frame only, so each pass's temporal history is its OWN output a "
                     "frame ago rather than the previous pass of this frame. NOTE evaluates "
                     "COUNTS PASSES, not frames - divide by %u before comparing it with the "
                     "frame count above. The identical-rate is NOT a verdict: a static scene "
                     "produces identical output legitimately, so read it against what was on "
                     "screen. A rate near 100%% with a moving scene is the signal that NR "
                     "stopped writing.",
                     s.nr_ok ? "UP" : "NOT RUNNING (transport-only)",
                     s.passes,
                     s.nr_evals,
                     (s.passes > 0) ? s.nr_evals / s.passes : s.nr_evals, s.passes,
                     s.nr_fails, s.nr_same,
                     (s.nr_frames > 1) ? 100.0 * (double)s.nr_same / (double)(s.nr_frames - 1) : 0.0,
                     s.nr_skipped,
                     s.passes);
            mgpu::diag::info(line);
        }

        if (s.neural)
        {
            int w = snprintf(line, sizeof line,
                             "[MGPU][P6.3] intensity AT THE END of this run:");
            for (unsigned i = 0; i < s.passes && (size_t)w < sizeof line; ++i)
                w += snprintf(line + w, sizeof line - (size_t)w, " p%u=%.2f",
                              i + 1, s.intensity[i]);
            if ((size_t)w < sizeof line)
                snprintf(line + w, sizeof line - (size_t)w,
                         " | %llu hotkey edit%s during the run. %s",
                         s.intensity_edits, (s.intensity_edits == 1) ? "" : "s",
                         (s.intensity_edits != 0)
                           ? "THE VALUE CHANGED WHILE THIS RAN, so the frames in this summary "
                             "were NOT all produced at the same strength - the timings and the "
                             "identical-rate above cover a moving target and must not be quoted "
                             "as a figure for any one value."
                           : "Unchanged from the ini for the whole run, so the summary above "
                             "describes a single configuration.");
            mgpu::diag::info(line);
        }

        {
            snprintf(line, sizeof line,
                     "[MGPU][P5.1] present gate: presents=%llu idle=%llu fence waits=%llu. The "
                     "bridge presented once per NEW neural frame instead of once per vsync, so "
                     "the full-frame backbuffer copy and the DWM cross-adapter copy of this "
                     "window happen at the producer's rate rather than the display's. The idle "
                     "path blocks on the shared fence, so the consumer's cadence no longer "
                     "depends on which display GPU 1 is attached to.%s",
                     s.gate_presents, s.gate_idle, s.gate_waits,
                     s.profile ? " PROFILE MODE: no on-screen output and no liveness sample this "
                                 "run - the identical-rate above is therefore absent by design, "
                                 "not a failure." : "");
            mgpu::diag::info(line);
        }

        if (s.ts_ok && s.ts_n > 0)
        {
            const double n = (double)s.ts_n;
            snprintf(line, sizeof line,
                     "[MGPU][P2.2] GPU TIME on GPU 1, per consumed frame, n=%llu | seal copy "
                     "mean=%.3f | unpack (cross-adapter buffer -> NR input) mean=%.3f min=%.3f "
                     "max=%.3f | EVALUATE(%u pass%s) mean=%.3f min=%.3f max=%.3f -> per pass "
                     "mean=%.3f min=%.3f | output sample mean=%.3f ms. These are GPU 1's own "
                     "timestamps on GPU 1's own queue - execution, not wall-clock, and no "
                     "cross-adapter clock is involved. THE EVALUATE BRACKET SPANS EVERY PASS, so "
                     "the per-pass figure is the one to compare against the reference tool's "
                     "14.2 ms evaluateGPU and against earlier single-pass runs; the total is "
                     "what GPU 1 actually spends. Transport is paid ONCE per frame whatever the "
                     "pass count is - that asymmetry is the whole point of the run. STILL "
                     "PERISHABLE: one rig, one link, one resolution, one scene.%s",
                     s.ts_n,
                     s.ts_sum[0] / n,
                     s.ts_sum[1] / n, s.ts_min[1], s.ts_max[1],
                     s.passes, (s.passes == 1) ? "" : "es",
                     s.ts_sum[2] / n, s.ts_min[2], s.ts_max[2],
                     (s.ts_sum[2] / n) / (double)s.passes, s.ts_min[2] / (double)s.passes,
                     s.ts_sum[3] / n,
                     // D3. This used to say "READ THE MIN, NOT THE MEAN" and
                     // blame the present chain for sharing GPU 1's queue.
                     // DISPROVED 2026-09-11: a Profile=1 run with no on-screen
                     // output at all measured the evaluate mean at 15.049 ms
                     // against 14.959 with the output on. It went UP. The
                     // spread between min and mean is the model's own variance
                     // and it is not yet explained - see the leftover ledger.
                     s.profile ? "" : " The on-screen output is enabled in this run. That costs "
                                      "consumer-thread CPU but does NOT inflate these brackets: "
                                      "measured 15.049 mean with the output off against 14.959 "
                                      "with it on, so the min-to-mean spread here is the model's "
                                      "own variance rather than contention from the present.");
            mgpu::diag::info(line);

            // D5. Printed as its own line because P2.2 is already at the edge
            // of its buffer, and because this answers a different question:
            // P2.2 says HOW MUCH, this says WHAT SHAPE.
            {
                unsigned long long th = 0;
                for (UINT b = 0; b < stream_state::TS_BUCKETS; ++b) th += s.ts_hist[b];
                char dl[900];
                snprintf(dl, sizeof dl,
                         "[MGPU][D5] EVALUATE DISTRIBUTION over the floor (%.3f ms), n=%llu | "
                         "<5%%=%llu 5-10%%=%llu 10-20%%=%llu 20-30%%=%llu 30-40%%=%llu "
                         "40-50%%=%llu 50-75%%=%llu 75%%+=%llu. HOW TO READ IT: mass piled at "
                         "<5%% with a thin tail is occasional interference and the floor is the "
                         "real cost. TWO humps is two code paths or two clock states. A broad "
                         "even spread is content-dependent work. A run that starts left and "
                         "ends right is thermal or power, and no code change will touch it. "
                         "Buckets are percent over the floor, so this reads the same at any "
                         "subrect and on any card.",
                         s.ts_min[2], th,
                         s.ts_hist[0], s.ts_hist[1], s.ts_hist[2], s.ts_hist[3],
                         s.ts_hist[4], s.ts_hist[5], s.ts_hist[6], s.ts_hist[7]);
                mgpu::diag::info(dl);
            }

            // B1. What the stride actually saved, in the units that matter:
            // frames the render card never copied, and bytes that never
            // crossed the link.
            if (s.stride_max != 0)
            {
                const unsigned long long tot = s.game_frames;
                const double pct = (tot != 0)
                    ? (100.0 * (double)s.stride_skipped / (double)tot) : 0.0;
                const double mib = (double)s.stride_skipped * (double)s.slot_bytes
                                 / (1024.0 * 1024.0);
                char bl[1200];
                snprintf(bl, sizeof bl,
                         "[MGPU][B1] PRODUCER STRIDE, cap %u | game frames seen=%llu, sealed=%llu, "
                         "STRIDED=%llu (%.1f%%) | that is %.0f MiB the render card never copied "
                         "and never put on the link | stride residency 1=%llu 2=%llu 3=%llu "
                         "4=%llu 5=%llu 6=%llu 7=%llu 8=%llu | frames-per-evaluate seen at the "
                         "window boundaries: 1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu "
                         "8+=%llu. HOW TO READ IT: THE RATIO HISTOGRAM IS THE WHOLE STORY. All "
                         "mass at 1 means the consumer is evaluating nearly every frame and "
                         "THERE IS NOTHING FOR THE STRIDE TO SAVE - stride 1, STRIDED 0, and "
                         "that is the correct answer rather than a failure. An integer stride "
                         "can only cut 0%% or 50%%, so a 15%% waste is not worth cutting and "
                         "this loop is right to leave it. Mass at 2 and above is where it pays, "
                         "and it is a property of the PAIRING - a fast render card against a "
                         "slower neural one - not of this code. dropped, reordered and overrun "
                         "in the SEAL summary must all still be 0: a strided frame is not a hole "
                         "in the sequence and must never look like one.",
                         s.stride_max, tot, s.produced, s.stride_skipped, pct, mib,
                         s.stride_hist[1], s.stride_hist[2], s.stride_hist[3],
                         s.stride_hist[4], s.stride_hist[5], s.stride_hist[6],
                         s.stride_hist[7], s.stride_hist[8],
                         s.ratio_hist[1], s.ratio_hist[2], s.ratio_hist[3],
                         s.ratio_hist[4], s.ratio_hist[5], s.ratio_hist[6],
                         s.ratio_hist[7], s.ratio_hist[8]);
                mgpu::diag::info(bl);
            }

            // D5b. The floor, quarter by quarter.
            {
                char ql[1000]; int w = 0;
                w += snprintf(ql + w, sizeof ql - (size_t)w,
                              "[MGPU][D5b] EVALUATE OVER TIME, %u segments |",
                              stream_state::TS_SEGS);
                for (UINT q = 0; q < stream_state::TS_SEGS && w < (int)sizeof ql - 120; ++q)
                {
                    if (s.ts_seg_n[q] == 0)
                    {
                        w += snprintf(ql + w, sizeof ql - (size_t)w, " q%u: none |", q + 1);
                        continue;
                    }
                    w += snprintf(ql + w, sizeof ql - (size_t)w,
                                  " q%u: min=%.3f mean=%.3f max=%.3f n=%llu |",
                                  q + 1, s.ts_seg_min[q],
                                  s.ts_seg_sum[q] / (double)s.ts_seg_n[q],
                                  s.ts_seg_max[q], s.ts_seg_n[q]);
                }
                snprintf(ql + w, sizeof ql - (size_t)w,
                         " READ THE MIN ACROSS THE QUARTERS FIRST. Content-dependent work does "
                         "NOT move the floor - the cheapest frame in a scene stays cheap however "
                         "hard the others get. A floor that CLIMBS is the card: clocks, power or "
                         "temperature, and no change to this code touches it. A flat floor with a "
                         "climbing mean is the scene getting harder, which is a fact about where "
                         "you walked and not about the pipeline. Both flat means the D5 spread "
                         "above is genuinely content and is the honest description of it.");
                mgpu::diag::info(ql);
            }
        }
        else if (s.neural)
            mgpu::diag::warn("[MGPU][P2.2] no GPU timings collected - the query heap did not "
                             "come up, or the neural stage did not. The transport result above "
                             "stands; there is simply no profile for this run.");

        // ---- R26: what the copy queue actually did ----
        //
        // Two lines, deliberately. The first is the measurement; the second is
        // the only reading of it that is honest, and it is not the flattering
        // one. The direct queue's unpack bracket goes to roughly zero in this
        // mode BECAUSE THE COPY IS NO LONGER ON THAT QUEUE, which is a fact
        // about where the work was recorded and not a saving. What would be a
        // saving is overlap, and overlap is cq_ahead.
        if (s.copy_queue)
        {
            if (s.cq_ts_ok && s.cq_ts_n > 0)
            {
                snprintf(line, sizeof line,
                         "[MGPU][R26] COPY QUEUE mode %d, n=%llu | unpack on the COPY queue's own "
                         "clock (%llu ticks/s, NOT the direct queue's %llu) mean=%.3f min=%.3f "
                         "max=%.3f ms | armed AHEAD of the previous frame's evaluate: %llu | "
                         "armed BEFORE THE FRAME ARRIVED (the GPU gate, mode 2 only): %llu | "
                         "fell back to the inline unpack: %llu.",
                         s.copy_queue_mode, s.cq_ts_n, (unsigned long long)s.cq_ts_freq,
                         (unsigned long long)s.ts_freq,
                         s.cq_ts_sum / (double)s.cq_ts_n, s.cq_ts_min, s.cq_ts_max,
                         s.cq_ahead, s.cq_spec, s.cq_fails);
                mgpu::diag::info(line);
            }
            else
            {
                snprintf(line, sizeof line,
                         "[MGPU][R26] COPY QUEUE ran with NO TIMESTAMPS on it (this adapter does "
                         "not report copy-queue timestamp support, or the heap did not come up). "
                         "The transport is unaffected; the unpack simply has no measured cost "
                         "this run. Mode %d. Armed ahead: %llu | armed before arrival: %llu | "
                         "fell back to the inline unpack: %llu. DO NOT read the P2.2 unpack "
                         "bracket above as the copy's cost - it is near zero because the copy is "
                         "on the other queue.",
                         s.copy_queue_mode, s.cq_ahead, s.cq_spec, s.cq_fails);
                mgpu::diag::warn(line);
            }

            if (s.cq_gated && s.cq_spec > 0)
            {
                snprintf(line, sizeof line,
                    "[MGPU][R28] THE GPU GATE FIRED on %llu frames - each armed on the copy "
                    "queue before it existed and released by the GAME's own fence signal rather "
                    "than by the bridge thread noticing. Those unpacks ran on the DMA engine "
                    "while the 3D engine was inside the previous frame's EvaluateFeature, which "
                    "is the overlap P2.1 named and did not test. THE ACCEPTANCE TEST IS THE "
                    "COMPARISON, NOT THIS LINE: per-frame GPU 1 time down by roughly the unpack "
                    "against the CopyQueue=0 control, with overrun, dropped and the "
                    "identical-rate ALL unmoved. Check OVERRUN first - this mode arms a copy one "
                    "frame earlier, so a producer that laps the ring has a wider window to "
                    "rewrite a slot under an armed copy. It is not silent when it happens: the "
                    "seal is read by the DIRECT queue from the same slot, so a rewritten slot "
                    "shows up as DROPPED or REORDERED above rather than as a quietly wrong "
                    "picture.",
                    s.cq_spec);
                mgpu::diag::info(line);
            }
            else if (s.cq_gated)
                mgpu::diag::warn(
                    "[MGPU][R28] CopyQueue=2 RAN BUT THE GATE NEVER FIRED: cq_spec is 0, so no "
                    "copy was ever armed for a frame that had not already arrived. That should "
                    "not happen in this mode and it means the run is really a mode 1 run. Do "
                    "not compare it as mode 2. Suspect the arming bound in stream_poll first.");
            else if (s.cq_ahead == 0)
                mgpu::diag::warn(
                    "[MGPU][R26] NOTHING OVERLAPPED, AND THAT IS THE EXPECTED RESULT OF A "
                    "PERFECTLY PACED RUN IN THIS MODE, not a fault. Mode 1 tests arrival on the "
                    "CPU, so it only ever arms a copy for a frame already known to have landed, "
                    "and the consume loop waits on the direct fence for frame f before it "
                    "records f+1. The overlap therefore needs a backlog, and this run had none: "
                    "the copy moved to the DMA engine and bought no wall-clock time. Expect "
                    "per-frame GPU 1 time UNCHANGED against the CopyQueue=0 control; if it "
                    "moved, something else moved with it. CopyQueue=2 is the mode that overlaps "
                    "in the steady state - it gates the copy on the GAME's fence on the GPU and "
                    "changes nothing about presentation.");
            else
                mgpu::diag::info(
                    "[MGPU][R26] Some copies did overlap - see the armed-ahead count above. "
                    "Those are frames that had already arrived while their predecessor was "
                    "still being evaluated, so their unpack ran on the DMA engine inside the 3D "
                    "engine's shadow. The win applies to THOSE FRAMES ONLY, so this run is not "
                    "the paced case and must not be compared as though it were.");
        }

        // R28. Everything the device complained about across the whole run,
        // once, at the end. Silence here with the debug layer ON is the
        // evidence that every barrier this file records is describing a state
        // the resource is actually in - which is the one thing about R26 that
        // could not be checked without a rig.
        if (s.ndev_b != nullptr) transit_drain_info_queue(s.ndev_b, "stream summary");

        const bool clean = (s.dropped == 0 && s.reordered == 0 && s.bad_magic == 0 &&
                            s.contract == 0 && s.alias == 0);
        if (strcmp(s.fault, "none") != 0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P4.0] FAULT-INJECTED RUN (\"%s\") - this is a NEGATIVE CONTROL and "
                     "must NOT be recorded as a clean stream. Acceptance is that the counter "
                     "matching the injected fault is non-zero above; a clean summary here means "
                     "the checker did not trip and the instrument is not yet trustworthy.",
                     s.fault);
            mgpu::diag::warn(line);
        }
        else if (clean && s.lat_n > 0)
        {
            snprintf(line, sizeof line,
                     "[MGPU][P4.0] STREAM PASSED - %llu game frames sealed, transited and "
                     "checked with no drop, no reorder, no alias and no contract mismatch. "
                     "Identity, ordering and age hold across a continuous stream, which is the "
                     "first thing in this project that a one-shot probe could not have shown. "
                     "NOTE THE SCOPE: pixels are NOT verified per frame (P1.5 established the "
                     "payload crosses byte-exact) and the barcode is unimplemented, so "
                     "seal-to-pixel identity is UNCHECKED. A green run here is not evidence "
                     "until the fault-injection runs in P1_INSTRUMENT section 04 have been seen "
                     "to trip this same checker.",
                     s.produced);
            mgpu::diag::info(line);
        }
        else
        {
            mgpu::diag::error("[MGPU][P4.0] STREAM FAILED - see the counters above; each "
                              "non-zero one names its own failure and they have different "
                              "causes. Do not average them into a verdict.");
        }
        stream_release();
    }
}

}
