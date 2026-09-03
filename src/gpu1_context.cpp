// MGPU Bridge - the private D3D12 device on the selected adapter (T3;
// the device-removal poll accessor arrives with T4)
#include <windows.h>
#include <combaseapi.h>
#include <d3d12.h>
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
    ID3D12Resource *tex_output = nullptr;   // NR output  (RT|UAV - never the input)
    ID3D12Resource *tex_depth = nullptr;    // cleared depth, only if NR demands one
    ID3D12Resource *tex_mvec = nullptr;     // zero motion
    ID3D12Resource *buf_upload = nullptr;
    ID3D12Resource *buf_read_in = nullptr;
    ID3D12Resource *buf_read_out = nullptr;

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
        if (buf_read_out != nullptr) { buf_read_out->Release(); buf_read_out = nullptr; }
        if (buf_read_in  != nullptr) { buf_read_in->Release();  buf_read_in  = nullptr; }
        if (buf_upload   != nullptr) { buf_upload->Release();   buf_upload   = nullptr; }
        if (tex_mvec     != nullptr) { tex_mvec->Release();     tex_mvec     = nullptr; }
        if (tex_depth    != nullptr) { tex_depth->Release();    tex_depth    = nullptr; }
        if (tex_output   != nullptr) { tex_output->Release();   tex_output   = nullptr; }
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
    // P1.1 - does the model EXECUTE on GPU 1, and does it consume what we
    //        hand it?
    //
    // P1.0c proved the feature can be built here. That is not the same as
    // the network running: CreateFeature accepted a schema, it dispatched
    // no tensors. Everything below is local to GPU 1 - our own textures,
    // our own command list, no shared handles, nothing across the bus.
    // Transit is deliberately not part of this milestone, for the same
    // reason P1.0 came before transit: if the model will not execute on a
    // display-less adapter, every byte moved would have been wasted.
    //
    // The proof is NUMERIC, not visual. NR reconstructs organic sub-pixel
    // detail; on the flat cycling colour this window presents, a working
    // model and a no-op are indistinguishable by eye. So the probe uploads
    // a structured pattern, evaluates, copies input and output back to the
    // CPU and counts differing pixels. Non-zero means tensors ran.
    // =================================================================
    {
        // NR must never write to its own input (the guide is explicit, and
        // the reference path creates a separate "private output"). Colour
        // in and colour out are therefore two distinct textures.
        //
        // R8G8B8A8_UNORM matches this runtime's swapchain and is
        // display-referred, which is what NR expects - it is a post-tone-map
        // pass, and feeding it linear HDR blows out the image rather than
        // merely looking different. The reference used R10G10B10A2 at 1440p;
        // if the format is rejected the result code will say so and this is
        // the one line to change.
        const DXGI_FORMAT fmt_color = DXGI_FORMAT_R8G8B8A8_UNORM;
        const DXGI_FORMAT fmt_mvec  = DXGI_FORMAT_R16G16_FLOAT;
        // R32_FLOAT rather than a real depth format on purpose: NR reads
        // depth as a plain texture, and a D32_FLOAT resource would need
        // ALLOW_DEPTH_STENCIL, which conflicts with the simple copy path
        // used to initialise it.
        const DXGI_FORMAT fmt_depth = DXGI_FORMAT_R32_FLOAT;

        HRESULT hr = make_tex(dev, width, height, fmt_color,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, &tex_color);
        // COPY_DEST, not UNORDERED_ACCESS: the output is pre-filled with a
        // sentinel before evaluation (see below) and transitions to UAV
        // afterwards.
        if (SUCCEEDED(hr))
            hr = make_tex(dev, width, height, fmt_color,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_output);
        if (SUCCEEDED(hr))
            hr = make_tex(dev, width, height, fmt_mvec,
                          D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_mvec);
        if (SUCCEEDED(hr))
            hr = make_tex(dev, width, height, fmt_depth,
                          D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &tex_depth);

        snprintf(line, sizeof line,
                 "[MGPU][P1.1] resources hr=0x%08X color=0x%p output=0x%p(UAV) mvec=0x%p "
                 "depth=0x%p %ux%u",
                 (unsigned)hr, (void *)tex_color, (void *)tex_output,
                 (void *)tex_mvec, (void *)tex_depth, width, height);
        mgpu::diag::info(line);
        if (FAILED(hr))
        {
            teardown("P1.1 resource creation");
            return false;
        }

        // ---- footprints and one upload buffer for all three inputs ----
        //
        // Row pitches are padded to 256 bytes and each subresource's offset
        // to 512, so the sizes come from GetCopyableFootprints rather than
        // from width x height x bpp. At 1280 wide the colour pitch is
        // already 5120 and needs no padding, but that is a property of this
        // resolution and must not be assumed.
        D3D12_RESOURCE_DESC d_color = tex_color->GetDesc();
        D3D12_RESOURCE_DESC d_mvec  = tex_mvec->GetDesc();
        D3D12_RESOURCE_DESC d_depth = tex_depth->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_color{}, fp_mvec{}, fp_depth{};
        UINT64 sz_color = 0, sz_mvec = 0, sz_depth = 0;
        UINT rows_color = 0, rows_mvec = 0, rows_depth = 0;
        UINT64 rb_color = 0, rb_mvec = 0, rb_depth = 0;

        dev->GetCopyableFootprints(&d_color, 0, 1, 0, &fp_color, &rows_color, &rb_color, &sz_color);
        const UINT64 off_mvec = (sz_color + 511) & ~(UINT64)511;
        dev->GetCopyableFootprints(&d_mvec, 0, 1, off_mvec, &fp_mvec, &rows_mvec, &rb_mvec, &sz_mvec);
        const UINT64 off_depth = (off_mvec + sz_mvec + 511) & ~(UINT64)511;
        dev->GetCopyableFootprints(&d_depth, 0, 1, off_depth, &fp_depth, &rows_depth, &rb_depth, &sz_depth);

        // A fourth region: the SENTINEL that pre-fills the output.
        //
        // Without it "output differs from input" is not proof of anything.
        // A texture NR never wrote is not black - it is UNINITIALISED, and
        // uninitialised memory differs from the input too. The probe would
        // then report PROBE PASSED on garbage, which is the precise class of
        // false positive this project exists to avoid.
        //
        // With it the comparison is three-way and each outcome is
        // unambiguous: still sentinel = NR wrote nothing; equal to input =
        // NR copied; neither = NR processed.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_out{};
        UINT64 sz_out = 0, rb_out = 0; UINT rows_out = 0;
        const UINT64 off_out = (off_depth + sz_depth + 511) & ~(UINT64)511;
        dev->GetCopyableFootprints(&d_color, 0, 1, off_out, &fp_out, &rows_out, &rb_out, &sz_out);
        const UINT64 upload_bytes = off_out + sz_out;

        // Chosen so it cannot occur in the pattern: fill_pattern always
        // writes B = 0.70*R, so any pixel with B far above R is ours.
        const unsigned char SENT_R = 0x10, SENT_G = 0x20, SENT_B = 0xF0;

        hr = make_buf(dev, upload_bytes, D3D12_HEAP_TYPE_UPLOAD, &buf_upload);
        if (SUCCEEDED(hr))
            hr = make_buf(dev, sz_color, D3D12_HEAP_TYPE_READBACK, &buf_read_in);
        if (SUCCEEDED(hr))
            hr = make_buf(dev, sz_color, D3D12_HEAP_TYPE_READBACK, &buf_read_out);
        if (FAILED(hr))
        {
            snprintf(line, sizeof line, "[MGPU][P1.1] staging buffers hr=0x%08X", (unsigned)hr);
            mgpu::diag::error(line);
            teardown("P1.1 staging buffers");
            return false;
        }

        {
            unsigned char *mapped = nullptr;
            D3D12_RANGE none{0, 0};   // we only write; nothing to read back in
            hr = buf_upload->Map(0, &none, reinterpret_cast<void **>(&mapped));
            if (FAILED(hr))
            {
                snprintf(line, sizeof line, "[MGPU][P1.1] upload Map hr=0x%08X", (unsigned)hr);
                mgpu::diag::error(line);
                teardown("P1.1 upload Map");
                return false;
            }
            // Motion and depth are ZERO for this milestone. Zero flow is the
            // correct "nothing moved" input and it makes the result
            // interpretable: any change in the output is the spatial model,
            // not temporal reprojection. QuantMotion's real 320x180 flow
            // arrives in P1.4 with the subrect and MVecScale keys that a
            // lower-resolution motion buffer requires.
            memset(mapped + off_mvec, 0, (size_t)(upload_bytes - off_mvec));
            fill_pattern(mapped + fp_color.Offset, width, height, fp_color.Footprint.RowPitch);
            for (UINT y = 0; y < height; ++y)
            {
                unsigned char *row = mapped + fp_out.Offset + (size_t)y * fp_out.Footprint.RowPitch;
                for (UINT x = 0; x < width; ++x)
                {
                    unsigned char *p = row + (size_t)x * 4;
                    p[0] = SENT_R; p[1] = SENT_G; p[2] = SENT_B; p[3] = 255;
                }
            }
            buf_upload->Unmap(0, nullptr);
        }

        // ---- record: upload -> barriers -> evaluate -> readback ----
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
        copy_in(tex_color,  fp_color);
        copy_in(tex_mvec,   fp_mvec);
        copy_in(tex_depth,  fp_depth);
        copy_in(tex_output, fp_out);     // the sentinel

        const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(pcmd, tex_color, D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        barrier(pcmd, tex_mvec,  D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        barrier(pcmd, tex_depth, D3D12_RESOURCE_STATE_COPY_DEST, read_state);
        barrier(pcmd, tex_output, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // ---- the parameters ----
        //
        // Namespaced keys, like DLSSNR.Width in P1.0c. PopulateParameters_Impl
        // registered these; it did not fill them, and per the guide it does
        // NOT register the subrect keys at all - those are set here by hand.
        //
        // The subrect naming has no separator: DLSSNR.ColorSubrectWidth, NOT
        // DLSSNR.Color.SubrectWidth. Getting it wrong fails silently - the
        // value is stored, nothing reads it, and the feature runs on the
        // full resource extent. That is the same class of failure that cost
        // P1.0c a rig cycle on DLSSNR.Width.
        params->Set("DLSSNR.Color",  tex_color);
        params->Set("DLSSNR.Output", tex_output);
        params->Set("DLSSNR.MVec",   tex_mvec);

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

        // Motion is at colour resolution this milestone, so the scale is
        // 1.0 - NOT because 1.0 is a safe default but because the two
        // extents genuinely match. When QuantMotion's 320x180 grid arrives
        // this becomes the ratio between the two, and leaving it at 1.0
        // would silently misread every vector.
        params->Set("DLSSNR.MVecScaleX", 1.0f);
        params->Set("DLSSNR.MVecScaleY", 1.0f);
        params->Set("DLSSNR.DepthInverted", 0u);
        params->Set("DLSSNR.Reset", 1u);          // first frame of a new feature

        // Tuning, taken from the reference tool's own working configuration
        // rather than invented. Recorded in VENDOR_LOCK.md.
        params->Set("DLSSNR.Intensity", 0.842f);
        params->Set("DLSSNR.LocalToneStrength", 1.142f);
        params->Set("DLSSNR.LocalStructureStrength", 1.092f);
        params->Set("DLSSNR.SkinStructureStrength", 1.025f);
        params->Set("DLSSNR.UseAutoMask", 1u);

        // ---- evaluate, depth-free FIRST ----
        //
        // The reference tool runs guidanceMode=2 with depthInterval=4, which
        // proves that mode exists but NOT that depth may be absent - it has
        // the game's depth buffer and supplies it on a cadence. So this is a
        // test, not an assumption: try without depth, and if NR refuses,
        // bind the cleared depth and try again. Two result codes, one run,
        // and the question is settled either way.
        LARGE_INTEGER f{}, e0{}, e1{};
        QueryPerformanceFrequency(&f);

        params->Set("DLSSNR.Depth", (ID3D12Resource *)nullptr);
        QueryPerformanceCounter(&e0);
        NVSDK_NGX_Result er = p_evaluate(pcmd, handle, params, nullptr);
        QueryPerformanceCounter(&e1);
        double ems = (f.QuadPart > 0)
            ? ((double)(e1.QuadPart - e0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
        snprintf(line, sizeof line,
                 "[MGPU][P1.1] EvaluateFeature (no depth): result=0x%08X (%s) record_cpu=%.2fms",
                 (unsigned)er, ngx_result_name(er), ems);
        mgpu::diag::info(line);

        bool used_depth = false;
        if (er != NVSDK_NGX_Result_Success)
        {
            mgpu::diag::warn("[MGPU][P1.1] retrying WITH a cleared depth texture - the depth-free "
                             "hypothesis did not hold on this build");
            params->Set("DLSSNR.Depth", tex_depth);
            params->Set("DLSSNR.DepthSubrectBaseX", 0u);
            params->Set("DLSSNR.DepthSubrectBaseY", 0u);
            params->Set("DLSSNR.DepthSubrectWidth", (unsigned int)width);
            params->Set("DLSSNR.DepthSubrectHeight", (unsigned int)height);
            QueryPerformanceCounter(&e0);
            er = p_evaluate(pcmd, handle, params, nullptr);
            QueryPerformanceCounter(&e1);
            ems = (f.QuadPart > 0)
                ? ((double)(e1.QuadPart - e0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
            used_depth = true;
            snprintf(line, sizeof line,
                     "[MGPU][P1.1] EvaluateFeature (with depth): result=0x%08X (%s) "
                     "record_cpu=%.2fms",
                     (unsigned)er, ngx_result_name(er), ems);
            mgpu::diag::info(line);
        }

        // The readback is recorded whatever the result codes said. An
        // evaluate that returned Success and wrote nothing is a real and
        // very quiet failure mode, and it is exactly what this comparison
        // exists to catch.
        barrier(pcmd, tex_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
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
        copy_out(tex_color,  buf_read_in);
        copy_out(tex_output, buf_read_out);

        // ---- flush here rather than in teardown ----
        //
        // The comparison needs the GPU to have finished, and teardown
        // releases the buffers it would read. So the drain happens now;
        // list_open goes false and teardown skips its own.
        mgpu::diag::info("[MGPU][P1.1] flush: Close -> Execute -> fence wait ...");
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
                     "[MGPU][P1.1] flush FAILED hr=0x%08X wait=0x%08X - not reading back, and "
                     "not releasing anything (the GPU may still hold these resources)",
                     (unsigned)hr, (unsigned)wr);
            mgpu::diag::error(line);
            // Deliberate leak, same rule as P1.0c's abandoned teardown.
            mgpu::diag::warn("[MGPU][P1.1] PROBE FAILED at flush. Bridge continues.");
            return false;
        }
        QueryPerformanceCounter(&g1);
        const double gms = (f.QuadPart > 0)
            ? ((double)(g1.QuadPart - g0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
        // WHOLE LIST, not the evaluate alone: four uploads, the evaluate and
        // two readback copies, plus submission overhead. It is an upper
        // bound and nothing more. A real per-pass GPU figure needs timestamp
        // queries, which belong with the measurement work in
        // P1_INSTRUMENT.md - do NOT compare this number against the
        // reference tool's 14.2 ms evaluateGPU.
        snprintf(line, sizeof line,
                 "[MGPU][P1.1] flush: GPU idle after %.2f ms (WHOLE LIST: uploads + evaluate + "
                 "readback - an upper bound, not an evaluate timing)", gms);
        mgpu::diag::info(line);

        // ---- the actual proof ----
        {
            const unsigned char *pin = nullptr;
            const unsigned char *pout = nullptr;
            D3D12_RANGE all{0, (SIZE_T)sz_color};
            const HRESULT h1 = buf_read_in->Map(0, &all, (void **)&pin);
            const HRESULT h2 = buf_read_out->Map(0, &all, (void **)&pout);
            if (FAILED(h1) || FAILED(h2) || pin == nullptr || pout == nullptr)
            {
                snprintf(line, sizeof line,
                         "[MGPU][P1.1] readback Map hr=0x%08X/0x%08X - cannot compare",
                         (unsigned)h1, (unsigned)h2);
                mgpu::diag::error(line);
            }
            else
            {
                unsigned long long differing = 0, nonzero_out = 0, total = 0;
                unsigned long long untouched = 0;   // still exactly the sentinel
                unsigned long long sum_abs = 0;
                unsigned int max_abs = 0;
                const UINT pitch = fp_color.Footprint.RowPitch;
                for (UINT y = 0; y < height; ++y)
                {
                    const unsigned char *ri = pin + (size_t)y * pitch;
                    const unsigned char *ro = pout + (size_t)y * pitch;
                    for (UINT x = 0; x < width; ++x)
                    {
                        const unsigned char *a = ri + (size_t)x * 4;
                        const unsigned char *b = ro + (size_t)x * 4;
                        bool diff = false;
                        for (int c = 0; c < 3; ++c)
                        {
                            const int d = (int)b[c] - (int)a[c];
                            const unsigned int ad = (unsigned int)(d < 0 ? -d : d);
                            if (ad != 0) diff = true;
                            sum_abs += ad;
                            if (ad > max_abs) max_abs = ad;
                        }
                        if (b[0] || b[1] || b[2]) ++nonzero_out;
                        if (b[0] == SENT_R && b[1] == SENT_G && b[2] == SENT_B) ++untouched;
                        if (diff) ++differing;
                        ++total;
                    }
                }
                const double pct = total ? (100.0 * (double)differing / (double)total) : 0.0;
                const double mean = total ? ((double)sum_abs / (double)(total * 3)) : 0.0;

                snprintf(line, sizeof line,
                         "[MGPU][P1.1] readback: pixels=%llu differing_from_input=%llu (%.2f%%) "
                         "still_sentinel=%llu mean_abs_delta=%.3f max_abs_delta=%u "
                         "output_nonzero=%llu depth=%s",
                         total, differing, pct, untouched, mean, max_abs, nonzero_out,
                         used_depth ? "bound" : "null");
                mgpu::diag::info(line);

                if (er != NVSDK_NGX_Result_Success)
                {
                    mgpu::diag::warn("[MGPU][P1.1] EVALUATE FAILED - the numbers above describe an "
                                     "output NR did not write. Read them as a control, not a "
                                     "result.");
                }
                else if (untouched == total)
                {
                    mgpu::diag::error("[MGPU][P1.1] EVALUATE RETURNED SUCCESS BUT THE OUTPUT IS "
                                      "STILL THE SENTINEL, every pixel - NR wrote nothing to the "
                                      "resource we bound. Suspect the DLSSNR.Output key name, the "
                                      "UAV state, or a subrect name. This is the case that would "
                                      "have read as a pass without the sentinel.");
                }
                else if (nonzero_out == 0)
                {
                    mgpu::diag::error("[MGPU][P1.1] EVALUATE RETURNED SUCCESS BUT THE OUTPUT IS "
                                      "ENTIRELY BLACK - written, but with nothing. Suspect the "
                                      "input binding rather than the output binding.");
                }
                else if (differing == 0)
                {
                    mgpu::diag::error("[MGPU][P1.1] EVALUATE RETURNED SUCCESS BUT OUTPUT == INPUT "
                                      "byte for byte - NR copied rather than processed. Suspect a "
                                      "silently-ignored parameter; the snippet log is the place to "
                                      "look.");
                }
                else
                {
                    snprintf(line, sizeof line,
                             "[MGPU][P1.1] PROBE PASSED - DLSS-NR EXECUTED ON THE NON-GAME "
                             "ADAPTER. The output is neither the sentinel nor a copy of the "
                             "input: %llu of %llu pixels overwritten with processed data. "
                             "Tensors ran on GPU 1.",
                             total - untouched, total);
                    mgpu::diag::info(line);
                }
            }
            D3D12_RANGE nothing{0, 0};
            if (pin != nullptr)  buf_read_in->Unmap(0, &nothing);
            if (pout != nullptr) buf_read_out->Unmap(0, &nothing);
        }
    }

    // ---- 8. leave nothing behind ----
    teardown(nullptr);

    mgpu::diag::info("[MGPU][P1.0c] create/teardown cycle complete - see the [MGPU][P1.1] lines "
                     "above for whether the model actually executed");
    return true;
}
}
