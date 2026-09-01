// MGPU Bridge — adapter enumeration and selection (T2)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "adapter.hpp"
#include "diag.hpp"

namespace mgpu::adapter
{
namespace
{
    struct state
    {
        std::atomic<bool> initialised{false};
        std::atomic<bool> ran{false};     // one-shot guard: init_device fires for
                                          // every D3D12CreateDevice in the process,
                                          // with no thread guarantee
        HANDLE ready = nullptr;           // manual-reset event
        std::mutex cs;
        selection_result result;
    };

    state &st()
    {
        static state s;
        return s;
    }

    // Called on the game thread, before the bridge thread is spawned and
    // before run_once() — so the event is published without a race.
    void ensure_init()
    {
        bool expected = false;
        if (st().initialised.compare_exchange_strong(expected, true))
            st().ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    bool luid_eq(const LUID &a, const LUID &b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }
}

bool run_once(::reshade::api::device *game_device, const char *trigger)
{
    bool expected = false;
    if (!st().ran.compare_exchange_strong(expected, true))
        return false;   // a later device/swapchain event: the caller handles it

    ensure_init();
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);

    // 1. Capture the game's adapter LUID FIRST. The first D3D12CreateDevice
    //    after add-on load is the game's; everything else in the process
    //    (including T3's own device) arrives later and is logged, not trusted.
    if (game_device != nullptr &&
        game_device->get_api() == ::reshade::api::device_api::d3d12)
    {
        if (auto *dev12 = static_cast<ID3D12Device *>(game_device->get_native()))
        {
            LUID luid{};
            if (SUCCEEDED(dev12->GetAdapterLuid(&luid)))
            {
                S.result.game_luid = luid;
                S.result.game_luid_known = true;
            }
        }
    }
    if (S.result.game_luid_known)
    {
        char line[240];
        snprintf(line, sizeof line,
                 "[MGPU][T2] enumeration triggered by %s; game luid=0x%08X-0x%08X "
                 "(captured from the game's d3d12 device)",
                 trigger, (unsigned)S.result.game_luid.HighPart, (unsigned)S.result.game_luid.LowPart);
        mgpu::diag::info(line);
    }
    else
    {
        char line[240];
        snprintf(line, sizeof line,
                 "[MGPU][T2] enumeration triggered by %s; game LUID UNKNOWN (no d3d12 device on "
                 "trigger) - exclusion rule disabled",
                 trigger);
        mgpu::diag::warn(line);
    }

    // 2. Enumerate.
    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4),
                                    reinterpret_cast<void **>(&factory));
    if (FAILED(hr))
    {
        char line[160];
        snprintf(line, sizeof line,
                 "[MGPU][T2] CreateDXGIFactory2 hr=0x%08X - enumeration impossible; no adapter selected",
                 (unsigned)hr);
        mgpu::diag::error(line);
        S.result.degenerate = true;
        S.result.rule = "none (factory creation failed)";
        SetEvent(S.ready);
        return true;
    }

    struct entry
    {
        LUID luid{};
        UINT outputs = 0;
        UINT64 vram_mb = 0;
        char desc[128]{};
    };
    std::vector<entry> table;
    std::vector<IDXGIAdapter1 *> adapters;   // held AddRef'd; non-selected ones released below

    for (UINT i = 0;; i++)
    {
        IDXGIAdapter1 *ad1 = nullptr;
        if (FAILED(factory->EnumAdapters1(i, &ad1)))
            break;
        DXGI_ADAPTER_DESC1 d{};
        ad1->GetDesc1(&d);

        UINT outputs = 0;
        IDXGIAdapter *ad0 = nullptr;
        if (SUCCEEDED(ad1->QueryInterface(__uuidof(IDXGIAdapter),
                                          reinterpret_cast<void **>(&ad0))))
        {
            IDXGIOutput *out = nullptr;
            while (SUCCEEDED(ad0->EnumOutputs(outputs, &out)))
            {
                out->Release();
                ++outputs;
            }
            ad0->Release();
        }

        entry e;
        e.luid = d.AdapterLuid;
        e.outputs = outputs;
        e.vram_mb = static_cast<UINT64>(d.DedicatedVideoMemory) / (1024u * 1024u);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, e.desc, 127, nullptr, nullptr);
        e.desc[127] = '\0';
        table.push_back(e);
        adapters.push_back(ad1);

        char line[400];
        snprintf(line, sizeof line,
                 "[MGPU][T2] adapter[%u] luid=0x%08X-0x%08X desc=\"%s\" vram=%lluMB outputs=%u",
                 i, (unsigned)e.luid.HighPart, (unsigned)e.luid.LowPart, e.desc,
                 (unsigned long long)e.vram_mb, outputs);
        mgpu::diag::info(line);
    }

    // 3. Select by exclusion from the game's LUID.
    std::vector<UINT> cand;
    for (UINT i = 0; i < table.size(); i++)
    {
        if (S.result.game_luid_known && luid_eq(table[i].luid, S.result.game_luid))
            continue;                 // the game's own adapter — never
        cand.push_back(i);
    }
    std::vector<UINT> headless;
    for (UINT i : cand)
        if (table[i].outputs == 0)
            headless.push_back(i);

    UINT sel = static_cast<UINT>(-1);
    bool degenerate = false;
    const char *rule = "none";

    if (S.result.game_luid_known)
    {
        if (cand.size() == 1)
        {
            sel = cand[0];
            rule = "exclusion (luid != game luid)";
        }
        else if (cand.empty())
        {
            degenerate = true;
            rule = "none (no non-game adapter)";
            char line[320];
            snprintf(line, sizeof line,
                     "[MGPU][T2] selection DEGENERATE: no adapter differs from game luid=0x%08X-0x%08X "
                     "- single-adapter topology; P0 needs a second GPU. Refusing to select the game's adapter.",
                     (unsigned)S.result.game_luid.HighPart, (unsigned)S.result.game_luid.LowPart);
            mgpu::diag::error(line);
        }
        else if (headless.size() == 1)
        {
            sel = headless[0];
            rule = "exclusion + headless tie-break";
        }
        else
        {
            sel = cand.back();
            degenerate = true;
            rule = "DEGENERATE last-in-enum-order";
            char line[320];
            snprintf(line, sizeof line,
                     "[MGPU][T2] selection DEGENERATE: %zu non-game adapters, no unique headless "
                     "(0 active outputs) tie-break - selecting last-in-enum-order; verify in the table above.",
                     cand.size());
            mgpu::diag::warn(line);
        }
    }
    else
    {
        // Exclusion unavailable: fall back to the headless heuristic, loudly.
        if (headless.size() == 1)
        {
            sel = headless[0];
            degenerate = true;
            rule = "headless only (game luid unknown)";
            mgpu::diag::warn("[MGPU][T2] WARNING: game LUID unknown; selection is the single headless "
                             "adapter - unverified against the game's card");
        }
        else if (!table.empty())
        {
            sel = static_cast<UINT>(table.size() - 1);
            degenerate = true;
            rule = "DEGENERATE last-in-enum-order (game luid unknown)";
            char line[320];
            snprintf(line, sizeof line,
                     "[MGPU][T2] selection DEGENERATE: game LUID unknown and no unique headless adapter "
                     "among %zu - selecting last-in-enum-order; verify in the table above.",
                     table.size());
            mgpu::diag::warn(line);
        }
        else
        {
            degenerate = true;
            rule = "none (no adapters enumerated)";
            mgpu::diag::error("[MGPU][T2] selection DEGENERATE: no adapters enumerated at all");
        }
    }

    // 4. Publish.
    if (sel != static_cast<UINT>(-1))
    {
        for (size_t i = 0; i < adapters.size(); i++)
        {
            if (static_cast<UINT>(i) != sel)
                adapters[i]->Release();
            else
                S.result.selected_adapter = adapters[i];   // AddRef'd, for T3
        }
        S.result.valid = true;
        S.result.degenerate = degenerate;
        S.result.selected_luid = table[sel].luid;
        S.result.selected_index = sel;
        S.result.selected_outputs = table[sel].outputs;
        strncpy(S.result.selected_desc, table[sel].desc, sizeof S.result.selected_desc - 1);
        S.result.selected_desc[sizeof S.result.selected_desc - 1] = '\0';
        S.result.rule = rule;

        char line[512];
        if (S.result.game_luid_known)
            snprintf(line, sizeof line,
                     "[MGPU][T2] selected adapter[%u] luid=0x%08X-0x%08X desc=\"%s\" rule=%s "
                     "| game luid=0x%08X-0x%08X",
                     sel, (unsigned)table[sel].luid.HighPart, (unsigned)table[sel].luid.LowPart,
                     table[sel].desc, rule,
                     (unsigned)S.result.game_luid.HighPart, (unsigned)S.result.game_luid.LowPart);
        else
            snprintf(line, sizeof line,
                     "[MGPU][T2] selected adapter[%u] luid=0x%08X-0x%08X desc=\"%s\" rule=%s "
                     "| game luid=unknown",
                     sel, (unsigned)table[sel].luid.HighPart, (unsigned)table[sel].luid.LowPart,
                     table[sel].desc, rule);
        mgpu::diag::info(line);
        if (degenerate)
            mgpu::diag::warn("[MGPU][T2] WARNING: degenerate selection - confirm the binding manually "
                             "before trusting anything downstream");
    }
    else
    {
        for (IDXGIAdapter1 *a : adapters)
            a->Release();
        S.result.degenerate = degenerate;
        S.result.rule = rule;
        mgpu::diag::error("[MGPU][T2] no adapter selected - see the DEGENERATE lines above; "
                          "T3 will refuse to create a device");
    }

    factory->Release();
    SetEvent(S.ready);
    return true;
}

void shutdown()
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    if (S.result.selected_adapter != nullptr)
    {
        static_cast<IDXGIAdapter1 *>(S.result.selected_adapter)->Release();
        S.result.selected_adapter = nullptr;
    }
}

HANDLE ready_event()
{
    ensure_init();
    return st().ready;
}

void get_selection(selection_result &out)
{
    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);
    out = S.result;
}

void log_device_luid(const char *event, ::reshade::api::device *device)
{
    LUID luid{};
    bool have = false;
    if (device != nullptr && device->get_api() == ::reshade::api::device_api::d3d12)
    {
        if (auto *dev12 = static_cast<ID3D12Device *>(device->get_native()))
            have = SUCCEEDED(dev12->GetAdapterLuid(&luid));
    }
    char line[256];
    if (have)
        snprintf(line, sizeof line, "[MGPU][T3] %s luid=0x%08X-0x%08X",
                 event, (unsigned)luid.HighPart, (unsigned)luid.LowPart);
    else
        snprintf(line, sizeof line,
                 "[MGPU][T3] %s luid=unknown (not a d3d12 device, or get_native failed)", event);
    mgpu::diag::info(line);
}
}
