// MGPU Bridge - R399 FGAhead: the command list of the generator call for the picked game frame. See fg_ahead.hpp.

#include "fg_ahead.hpp"

#include <cstdio>

namespace mgpu::fgahead
{
namespace
{
ID3D12CommandQueue *g_queue = nullptr;          // the neural queue, AddRef'd
ID3D12CommandAllocator *g_alloc[2] = {};        // one per turn: a turn's allocator is reset only after its list has run
ID3D12GraphicsCommandList *g_list = nullptr;
ID3D12Fence *g_fence = nullptr;
UINT64 g_fence_value = 0ull;
UINT64 g_alloc_value[2] = {};
unsigned g_parity = 0u;
bool g_up = false, g_open = false;
unsigned long long g_lists = 0ull, g_busy = 0ull, g_failed = 0ull;
}

bool up() { return g_up; }

bool create(ID3D12Device *dev, ID3D12CommandQueue *queue, char *why, size_t n)
{
    if (why != nullptr && n != 0) why[0] = 0;
    release();
    if (dev == nullptr || queue == nullptr) { if (why != nullptr) snprintf(why, n, "no device or queue"); return false; }
    {   // the list is recorded on the neural device and run on this queue: one device object
        ID3D12Device *qd = nullptr;
        const HRESULT qh = queue->GetDevice(IID_PPV_ARGS(&qd));
        const bool same = SUCCEEDED(qh) && qd == dev;
        if (qd != nullptr) qd->Release();
        if (!same) { if (why != nullptr) snprintf(why, n, "the queue is not on the neural device object"); return false; }
    }
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    HRESULT h = dev->CreateCommandAllocator(type, IID_PPV_ARGS(&g_alloc[0]));
    if (SUCCEEDED(h)) h = dev->CreateCommandAllocator(type, IID_PPV_ARGS(&g_alloc[1]));
    if (SUCCEEDED(h)) h = dev->CreateCommandList(0, type, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list));
    if (SUCCEEDED(h)) h = g_list->Close();   // made open; every use starts with Reset
    if (SUCCEEDED(h)) h = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(h))
    {
        if (why != nullptr) snprintf(why, n, "the list, its allocators or its fence could not be made (0x%08X)", (unsigned)h);
        release();
        return false;
    }
    queue->AddRef(); g_queue = queue;
    g_fence_value = 0ull; g_alloc_value[0] = g_alloc_value[1] = 0ull; g_parity = 0u; g_open = false;
    g_lists = g_busy = g_failed = 0ull;
    g_up = true;
    return true;
}

ID3D12GraphicsCommandList *begin()
{
    if (!g_up || g_open) return nullptr;
    const unsigned par = g_parity & 1u;
    if (g_fence->GetCompletedValue() < g_alloc_value[par]) { ++g_busy; return nullptr; }   // its last list has not run yet
    if (FAILED(g_alloc[par]->Reset()) || FAILED(g_list->Reset(g_alloc[par], nullptr))) { ++g_failed; return nullptr; }
    g_open = true;
    return g_list;
}

unsigned long long submit(ID3D12Fence *unpack, unsigned long long unpack_value)
{
    if (!g_up || !g_open) return 0ull;
    g_open = false;
    if (FAILED(g_list->Close())) { ++g_failed; return 0ull; }
    // The picked frame's textures are written by the copy queue: this queue does not start the list before that is done.
    if (unpack != nullptr && unpack->GetCompletedValue() < (UINT64)unpack_value)
        (void)g_queue->Wait(unpack, (UINT64)unpack_value);
    ID3D12CommandList *const ls[1] = { g_list };
    g_queue->ExecuteCommandLists(1, ls);
    ++g_fence_value;
    if (FAILED(g_queue->Signal(g_fence, g_fence_value))) { ++g_failed; }   // the list still runs; done() would never say so, the frame is not shown
    g_alloc_value[g_parity & 1u] = g_fence_value;
    ++g_parity;
    ++g_lists;
    return (unsigned long long)g_fence_value;
}

void cancel()
{
    if (!g_up || !g_open) return;
    g_open = false;
    (void)g_list->Close();   // never run: its allocator is reset at the next begin()
    ++g_failed;
}

bool done(unsigned long long value)
{
    ID3D12Fence *const f = g_fence;
    return value != 0ull && f != nullptr && f->GetCompletedValue() >= (UINT64)value;
}

void summary(char *out, size_t n)
{
    if (out == nullptr || n == 0) return;
    snprintf(out, n, "lists run %llu, refused because the list of that turn was still running %llu, could not be recorded or run %llu",
             g_lists, g_busy, g_failed);
}

void release()
{
    if (g_fence != nullptr && g_fence_value != 0ull && g_fence->GetCompletedValue() < g_fence_value)
    {   // the last list may still write a frame-generation texture: bounded wait, then free anyway
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev != nullptr)
        {
            if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fence_value, ev))) (void)WaitForSingleObject(ev, 2000);
            CloseHandle(ev);
        }
    }
    g_up = false; g_open = false;
    if (g_list != nullptr) { g_list->Release(); g_list = nullptr; }
    for (unsigned k = 0; k < 2u; ++k) if (g_alloc[k] != nullptr) { g_alloc[k]->Release(); g_alloc[k] = nullptr; }
    if (g_fence != nullptr) { g_fence->Release(); g_fence = nullptr; }
    if (g_queue != nullptr) { g_queue->Release(); g_queue = nullptr; }
}
}
