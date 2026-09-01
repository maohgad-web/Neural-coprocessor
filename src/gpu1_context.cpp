// MGPU Bridge - the private D3D12 device on the selected adapter (T3)
#include <windows.h>
#include <combaseapi.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "adapter.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"

namespace mgpu::gpu1
{
namespace
{
    struct state
    {
        std::mutex cs;
        ID3D12Device *device = nullptr;
        LUID device_luid{};
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

    LUID luid{};
    dev->GetAdapterLuid(&luid);

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

void shutdown()
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    if (S.device != nullptr)
    {
        S.device->Release();
        S.device = nullptr;
        mgpu::diag::info("[MGPU][T3] device released");
    }
}

bool has_device()
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    return S.device != nullptr;
}
}
