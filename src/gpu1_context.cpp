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
#include <cstring>
#include <mutex>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"

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
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *command_list = nullptr;
        ID3D12Fence *fence = nullptr;
        HANDLE fence_event = nullptr;
        UINT64 fence_value = 0;
        bool present_failed_logged = false;
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
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
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
        if (cl != nullptr)        { cl->Release(); cl = nullptr; }
        if (allocator != nullptr) { allocator->Release(); allocator = nullptr; }
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
        const HRESULT fhr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2),
                                               reinterpret_cast<void **>(&factory));
        if (FAILED(fhr))
            return fail("CreateDXGIFactory2", fhr);

        // The desc is flat (gotcha 4): no BufferDesc, no OutputWindow, no
        // Windowed member - the HWND is a parameter of
        // CreateSwapChainForHwnd and pFullscreenDesc == nullptr is what
        // makes it windowed. Flip model (gotcha 3): FLIP_DISCARD,
        // BufferCount 2, SampleDesc.Count 1, R8G8B8A8_UNORM (no SRGB, no
        // MSAA - the older DISCARD/SEQUENTIAL effects fail outright).
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    // 4. Command allocator + command list. The allocator is single:
    // present_frame waits on the fence before returning, which is what
    // makes the next frame's Reset safe.
    {
        // CreateCommandAllocator takes the list type directly - there is
        // no D3D12_COMMAND_ALLOCATION_DESC and no allocator flags enum.
        const HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator));
        if (FAILED(hr))
            return fail("CreateCommandAllocator", hr);
        const HRESULT clr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                   nullptr, IID_PPV_ARGS(&cl));
        if (FAILED(clr))
            return fail("CreateCommandList", clr);
        // CreateCommandList returns the list in the RECORDING state. The
        // first frame calls Reset on it, and Reset on a recording list is
        // invalid - so close it once here, immediately after creation.
        const HRESULT cchr = cl->Close();
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
        S.swapchain = sc3;
        S.rtv_heap = heap;
        S.backbuffer[0] = back0;
        S.backbuffer[1] = back1;
        S.allocator = allocator;
        S.command_list = cl;
        S.fence = fence;
        S.fence_event = event;
        S.fence_value = 0;
        S.present_failed_logged = false;
        // The state owns them now; the locals must not release them twice.
        queue = nullptr;
        sc3 = nullptr;
        heap = nullptr;
        back0 = nullptr;
        back1 = nullptr;
        allocator = nullptr;
        cl = nullptr;
        fence = nullptr;
        event = nullptr;
        factory->Release();
        factory = nullptr;
    }

    snprintf(line, sizeof line,
             "[MGPU][T5] present chain created: format=DXGI_FORMAT_R8G8B8A8_UNORM buffers=2 "
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
bool present_frame(float r, float g, float b)
{
    auto &S = st();

    ID3D12Device *dev = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *sc = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *backbuffer[2] = {};
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;

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
        allocator = S.allocator;
        cl = S.command_list;
        fence = S.fence;
        event = S.fence_event;
        fence_value = S.fence_value;
    }

    char line[320];

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

    // The single command allocator is reused every frame: the fence wait
    // at the end of the PREVIOUS call is what makes these Resets safe.
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

    const float color[4] = { r, g, b, 1.0f };
    cl->ClearRenderTargetView(rtv, color, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &barrier);

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

    hr = sc->Present(1, 0);
    if (FAILED(hr))
        return fail("Present", static_cast<unsigned>(hr));

    fence->SetEventOnCompletion(fence_value, event);
    const DWORD wr = WaitForSingleObject(event, INFINITE);
    if (wr != WAIT_OBJECT_0)
        return fail("fence wait (GetLastError)", static_cast<unsigned>(GetLastError()));

    // Publish the advanced fence value (bridge thread only: no other
    // writer). A failure before this line leaves it un-advanced; the
    // final drain in shutdown() re-signals and the wait still completes
    // (a fence value already reached fires its completion immediately,
    // and device removal signals all fences to UINT64_MAX - brief
    // section 09).
    {
        std::lock_guard<std::mutex> lk(S.cs);
        S.fence_value = fence_value;
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
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
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
        allocator = S.allocator;
        cl = S.command_list;
        fence = S.fence;
        event = S.fence_event;
        fence_value = S.fence_value;
        S.queue = nullptr;
        S.swapchain = nullptr;
        S.rtv_heap = nullptr;
        S.backbuffer[0] = nullptr;
        S.backbuffer[1] = nullptr;
        S.allocator = nullptr;
        S.command_list = nullptr;
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
        cl->Release();
        allocator->Release();
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
}

bool ngx_probe(UINT width, UINT height)
{
    auto &S = st();
    char line[600];

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

    mods.snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (mods.snippet != nullptr)
    {
        snip_how = "already resident";
    }
    else
    {
        mods.snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
        snip_how = (mods.snippet != nullptr) ? "loaded" : "not found";
    }

    snprintf(line, sizeof line,
             "[MGPU][P1.0c] ngx modules: _nvngx.dll=0x%p (%s) nvngx_dlssnr.dll=0x%p (%s)",
             (void *)mods.core, core_how, (void *)mods.snippet, snip_how);
    mgpu::diag::info(line);

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
        mgpu::diag::warn("[MGPU][P1.0c] PROBE FAILED at module - nvngx_dlssnr.dll is not present "
                         "beside dxgi.dll. P1.0b exists to call the snippet; without it this probe "
                         "would repeat P1.0 exactly. Copy nvngx_dlssnr.dll into the deploy "
                         "directory and rerun. Bridge continues.");
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

        if (r != NVSDK_NGX_Result_Success)
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
        const DXGI_FORMAT fmt_color = DXGI_FORMAT_R8G8B8A8_UNORM;
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
            fill_pattern(mapped + fp_color.Offset, width, height, fp_color.Footprint.RowPitch);
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
        if (ref_local != nullptr && st().game_luid_known)
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
    return true;
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
    // Why this is the question now rather than "is transit viable": this rig
    // is PCIe 3.0 x2 on chipset lanes, ~1.6 GB/s usable. GPU 0 moves ~28 MiB
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
    char line[700];

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
        // On a PCIe 3.0 x2 chipset link the crossing is expected to cost
        // something; this says HOW MUCH of the 35 ms is the link rather than
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
                     "LINK: this rig is PCIe 3.0 x2 on chipset lanes (~1.6 GB/s usable), so "
                     "~28 MiB across it is ~17.5 ms of pure link time at 1440p. These absolute "
                     "numbers are a property of THIS interconnect and do not decide whether the "
                     "architecture is viable. What they do decide is A versus A', because both "
                     "are measured over the same link.",
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
                 "[MGPU][P1.5] already requested. event_seen=%s armed=%s recorded=%s - if "
                 "event_seen is no, the game's runtime is not raising finish_effects at all.",
                 c.said_seen ? "yes" : "NO", c.armed ? "yes" : "no",
                 c.recorded ? "yes" : "no");
        mgpu::diag::info(l2);
        return;
    }
    c.requested = true;
    mgpu::diag::info("[MGPU][P1.5] capture REQUESTED - the next game frame allocates and arms, "
                     "the one after it is captured. Stay in gameplay; a black source frame "
                     "makes the verdict inconclusive and the shot is not repeatable.");
}

void capture_on_finish_effects(void *runtime_v, void *cmd_list_v,
                               unsigned long long rtv_handle)
{
    (void)runtime_v;
    capture_state &c = cap();
    std::lock_guard<std::mutex> lk(c.cs);
    // Inert until requested. The operator picks the frame, because the probe
    // cannot tell a loading screen from gameplay and only gets one.
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
        ID3D12Device *ld = nullptr;
        const bool got = SUCCEEDED(gl->GetDevice(IID_PPV_ARGS(&ld))) && ld != nullptr;
        const bool same = got && (ld == c.gdev);
        if (got) ld->Release();
        if (!same)
        {
            // Not the game's list. Silent after the first, because both
            // runtimes raise this every frame and we may wait several.
            if (!c.said_wrongq)
            {
                c.said_wrongq = true;
                mgpu::diag::warn("[MGPU][P1.5] armed, but this event's command list belongs to a "
                                 "different device - waiting for the game's runtime. Recording "
                                 "here would build a list referencing another device's "
                                 "resources, which executes nothing and looks like a transit "
                                 "failure.");
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

    // WE DO NOT OWN THE GAME'S QUEUE, so we cannot signal a fence on it and
    // cannot know the copy has retired. We wait a generous number of bridge
    // presents instead, and the sentinel below is what turns "waited too
    // little" into a named diagnosis rather than a wrong answer. A shared fence
    // makes this exact and belongs to P2.
    const unsigned WAIT_POLLS = 240;
    if (++c.polls < WAIT_POLLS) return;
    c.done = true;

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

    const unsigned char *pn = nullptr, *pg = nullptr;
    D3D12_RANGE all{0, (SIZE_T)c.bytes};
    const bool mn = SUCCEEDED(c.nread->Map(0, &all, (void **)&pn)) && pn != nullptr;
    const bool mg = SUCCEEDED(c.gread->Map(0, &all, (void **)&pg)) && pg != nullptr;
    if (mn && mg)
    {
        unsigned long long diff = 0, sent = 0, nonzero = 0;
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
                if (b2[0] || b2[1] || b2[2]) ++nonzero;
            }
        }
        snprintf(line, sizeof line,
                 "[MGPU][P1.5] %ux%u fmt=%d %.2f MiB | received vs sent: differing=%llu of %llu "
                 "| sentinel survivors=%llu | source non-black pixels=%llu",
                 c.width, c.height, (int)c.format,
                 (double)c.bytes / (1024.0 * 1024.0), diff, total, sent, nonzero);
        mgpu::diag::info(line);

        if (sent > 0)
        {
            // TWO causes produce surviving sentinel and they are told apart by
            // the reference buffer, not by this count. Say which.
            if (nonzero == 0)
                mgpu::diag::error("[MGPU][P1.5] PROBE FAILED - sentinel survived AND the "
                                  "game-side reference readback is empty. The reference needs "
                                  "no bus, so this is NOT a transit or timing fault: the "
                                  "recorded copies never executed at all. Suspect the command "
                                  "list we recorded into, not the link.");
            else
                mgpu::diag::error("[MGPU][P1.5] PROBE FAILED - sentinel survived while the "
                                  "game-side reference HAS content, so the copies did run and "
                                  "the crossing did not complete before we read. This is the "
                                  "synchronisation gap: we cannot signal a fence on a queue we "
                                  "do not own. Raise WAIT_POLLS, or wait for P2's shared "
                                  "fence.");
        }
        else if (nonzero == 0)
        {
            mgpu::diag::error("[MGPU][P1.5] PROBE INCONCLUSIVE - the SOURCE frame is entirely "
                              "black, so an all-black arrival proves nothing. Capture during "
                              "gameplay, not on a loading screen or a faded menu.");
        }
        else if (diff == 0)
        {
            mgpu::diag::info("[MGPU][P1.5] PROBE PASSED - THE GAME'S OWN FRAME CROSSED THE "
                             "ADAPTER BOUNDARY INTACT. Every byte received on the second adapter "
                             "matches the reference copy taken from the same command list at the "
                             "same instant. The payload is real rendered content at the game's "
                             "native resolution and format, not a pattern of ours.");
        }
        else
        {
            snprintf(line, sizeof line,
                     "[MGPU][P1.5] PROBE FAILED - %llu of %llu pixels differ from the reference. "
                     "Both copies came from one source in one list, so the two cannot have "
                     "diverged before transit: this is the payload changing on the way across. "
                     "Check the footprint arithmetic first - row pitch is %u for a %u-pixel row.",
                     diff, total, (unsigned)c.fp.Footprint.RowPitch, c.width);
            mgpu::diag::error(line);
        }
    }
    else mgpu::diag::error("[MGPU][P1.5] readback Map failed - no verdict possible");

    D3D12_RANGE nothing{0, 0};
    if (mn) c.nread->Unmap(0, &nothing);
    if (mg) c.gread->Unmap(0, &nothing);
    capture_release();
}

}
