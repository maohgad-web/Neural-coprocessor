// MGPU Bridge - adapter enumeration and selection (T2)
//
// Brief section 06, four rules. The gates are evaluated in an order that is
// safe under partial application - every gate can only withhold a selection,
// never cause a wrong one:
//
//   [rule 2] the game LUID gate, first: a selection may proceed only from
//            the swapchain-derived game LUID (the device
//            CreateSwapChainForHwnd was called on). UE5's probe devices make
//            the first init_device untrustworthy - one run captured a
//            *software adapter* as the game LUID. init_device captures a
//            provisional value that the swapchain value overrides.
//   [rule 3] the software filter: an adapter is software if it carries
//            DXGI_ADAPTER_FLAG_SOFTWARE *or* reports Microsoft's vendor id
//            0x1414. The flag alone is not sufficient - a rig run enumerated
//            two "Microsoft Basic Render Driver" adapters with identical
//            vendor, device id and memory reporting different Flags in the
//            same session (0x0 and 0x2), so an unflagged WARP survived the
//            filter and made the tiebreak ambiguous. Software adapters are
//            still enumerated and logged (Flags in hex, VendorId and
//            DedicatedVideoMemory); they simply cannot be selected.
//            DedicatedVideoMemory is logged and never filtered on -
//            integrated GPUs legitimately report zero.
//   [rule 4] the output count: logged for every adapter; consulted only as a
//            tiebreak when more than two hardware adapters exist. It is never
//            the primary discriminator - this project's topology puts the
//            display on the target card.
//   [rule 1] refuse rather than guess, last: exactly one candidate or
//            nothing. Listed first in the brief because it replaces the old
//            last-in-enum-order fallback and governs every non-unanimous
//            outcome; it is the final gate before a selection is committed.
//
// The binding keys on the LUID, never the enumeration index (index order is
// not stable across driver restarts). The selected IDXGIAdapter1 is held
// AddRef'd from the enumeration itself - T3's D3D12CreateDevice takes that
// pointer directly, so no LUID re-resolution is needed.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>   // IDXGIFactory4 (EnumAdapters1 is inherited from
                      // IDXGIFactory1; the spec names factory 4) and the
                      // DXGI 1.4 surface this file calls: IDXGIAdapter3 and
                      // its QueryVideoMemoryInfo (DXGI_QUERY_VIDEO_MEMORY_INFO,
                      // DXGI_MEMORY_SEGMENT_GROUP - all declared in dxgi1_4.h,
                      // not dxgi1_3.h)
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "adapter.hpp"
#include "diag.hpp"

// get_native() returns uint64_t: unwrapping it to ID3D12Device * needs
// reinterpret_cast, not static_cast (integer to pointer is only
// reinterpret_cast). GetAdapterLuid() takes no parameters and returns the
// LUID by value - there is no SUCCEEDED to check.

namespace mgpu::adapter
{
namespace
{
    struct entry
    {
        IDXGIAdapter1 *adapter = nullptr;   // AddRef'd by EnumAdapters1
        LUID luid{};
        UINT flags = 0;
        UINT vendor_id = 0;
        UINT device_id = 0;
        UINT64 dedicated_vram = 0;          // bytes, DXGI_ADAPTER_DESC1
        UINT outputs = 0;
        bool has_vmem_info = false;
        // Diagnostics only (QueryVideoMemoryInfo, LOCAL segment): this
        // process's usage on this adapter. Multi-GB where the game renders,
        // near-zero elsewhere. Nothing below reads these back into the
        // candidate logic - they must not influence the selection.
        UINT64 vram_budget = 0;
        UINT64 vram_current_usage = 0;
        UINT64 vram_avail_reservation = 0;
        UINT64 vram_cur_reservation = 0;
        char desc[128]{};
    };

    struct state
    {
        std::atomic<bool> initialised{false};
        HANDLE ready = nullptr;             // manual-reset event
        std::mutex cs;
        std::vector<entry> table;
        bool table_enumerated = false;      // sticky: first successful pass
        bool provisional_known = false;     // first init_device's LUID
        LUID provisional_luid{};
        bool decided = false;               // selected or terminally refused
        selection_result result;
    };

    state &st()
    {
        static state s;
        return s;
    }

    // The CAS alone published `initialised = true` before `ready` was
    // assigned, so a thread that lost the race could read a null handle from
    // ready_event() while the winner was still inside CreateEventW. Ordering
    // by argument (game thread first) is not a guarantee once the bridge
    // thread exists. std::call_once closes it: every caller blocks until the
    // handle is written.
    std::once_flag g_init_once;

    void ensure_init()
    {
        std::call_once(g_init_once, [] {
            st().ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            st().initialised.store(true, std::memory_order_release);
        });
    }

    bool luid_eq(const LUID &a, const LUID &b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    // Enumerate the full adapter table. Caller holds S.cs. The VRAM figures
    // are diagnostics only (brief: "must not influence selection") - they
    // are logged and stored, and the selection logic never reads them.
    bool enumerate_table_locked(state &S)
    {
        IDXGIFactory4 *factory = nullptr;
        const HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4),
                                              reinterpret_cast<void **>(&factory));
        if (FAILED(hr) || factory == nullptr)
        {
            char line[192];
            snprintf(line, sizeof line,
                     "[MGPU][T2] CreateDXGIFactory2 hr=0x%08X - enumeration impossible this pass; "
                     "selection deferred until a later device event retries",
                     (unsigned)hr);
            mgpu::diag::error(line);
            if (factory != nullptr)
                factory->Release();
            return false;
        }

        S.table.clear();
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1 *ad1 = nullptr;
            if (FAILED(factory->EnumAdapters1(i, &ad1)))
                break;   // DXGI_ERROR_NOT_FOUND: table exhausted
            entry e;
            e.adapter = ad1;
            DXGI_ADAPTER_DESC1 d{};
            ad1->GetDesc1(&d);
            e.luid = d.AdapterLuid;
            e.flags = d.Flags;
            e.vendor_id = d.VendorId;
            e.device_id = d.DeviceId;
            e.dedicated_vram = static_cast<UINT64>(d.DedicatedVideoMemory);

            // Output count: logged for every adapter; a rule 4 tiebreak
            // input only when more than two hardware adapters are present.
            // Never the primary discriminator.
            IDXGIAdapter *ad0 = nullptr;
            if (SUCCEEDED(ad1->QueryInterface(__uuidof(IDXGIAdapter),
                                              reinterpret_cast<void **>(&ad0))))
            {
                IDXGIOutput *out = nullptr;
                while (SUCCEEDED(ad0->EnumOutputs(e.outputs, &out)))
                {
                    out->Release();
                    ++e.outputs;
                }
                ad0->Release();
            }

            // Diagnostics only: this process's VRAM usage on this adapter.
            // Expected multi-GB on the adapter the game renders on and
            // near-zero on every other one - it identifies the game's
            // adapter independently of output counts.
            IDXGIAdapter3 *ad3 = nullptr;
            if (SUCCEEDED(ad1->QueryInterface(__uuidof(IDXGIAdapter3),
                                              reinterpret_cast<void **>(&ad3))))
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO vmem{};
                if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0,
                                    DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmem)))
                {
                    e.has_vmem_info = true;
                    e.vram_budget = vmem.Budget;
                    e.vram_current_usage = vmem.CurrentUsage;
                    e.vram_avail_reservation = vmem.AvailableForReservation;
                    e.vram_cur_reservation = vmem.CurrentReservation;
                }
                ad3->Release();
            }

            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1,
                                e.desc, 127, nullptr, nullptr);
            e.desc[127] = '\0';
            S.table.push_back(e);

            char line[512];
            snprintf(line, sizeof line,
                     "[MGPU][T2] adapter[%u] luid=0x%08X-0x%08X flags=0x%X vendor=0x%04X "
                     "device=0x%04X dedicated_vram=%lluMB outputs=%u desc=\"%s\"",
                     i, (unsigned)e.luid.HighPart, (unsigned)e.luid.LowPart,
                     (unsigned)e.flags, (unsigned)e.vendor_id, (unsigned)e.device_id,
                     (unsigned long long)(e.dedicated_vram / (1024ull * 1024ull)),
                     (unsigned)e.outputs, e.desc);
            mgpu::diag::info(line);
            if (e.has_vmem_info)
            {
                snprintf(line, sizeof line,
                         "[MGPU][T2] adapter[%u] vmem(local) budget=%lluMB current_usage=%lluMB "
                         "avail_reservation=%lluMB current_reservation=%lluMB "
                         "(diagnostic only - this process's usage on this adapter)",
                         i,
                         (unsigned long long)(e.vram_budget / (1024ull * 1024ull)),
                         (unsigned long long)(e.vram_current_usage / (1024ull * 1024ull)),
                         (unsigned long long)(e.vram_avail_reservation / (1024ull * 1024ull)),
                         (unsigned long long)(e.vram_cur_reservation / (1024ull * 1024ull)));
                mgpu::diag::info(line);
            }
        }

        factory->Release();
        if (S.table.empty())
        {
            mgpu::diag::error("[MGPU][T2] zero adapters enumerated - no selection possible; "
                              "a later device event will retry");
            return false;
        }
        S.table_enumerated = true;
        return true;
    }

    // The selection pipeline. Caller holds S.cs. Runs at most once: it
    // decides (selects or terminally refuses), sets S.decided, and signals
    // the ready event. A deferral returns without a decision - the ready
    // event stays unset and a later event may complete the selection.
    void try_select_locked(state &S)
    {
        if (S.decided)
            return;
        // [rule 2] the gate: the swapchain-derived game LUID is the only
        // value a selection may proceed from. No swapchain yet -> refuse to
        // select (a deferral, not a terminal refusal).
        if (!S.result.game_luid_from_swapchain)
            return;
        if (!S.table_enumerated)
            return;   // the failure was logged at the enumeration site

        // == the swapchain LUID, by the gate above.
        const LUID game = S.result.game_luid;

        char line[512];

        // [rule 3] the software filter, plus exclusion of the game's own
        // adapter (a LUID match, never an index match).
        std::vector<size_t> hw;     // hardware adapters
        std::vector<size_t> cand;   // of those, luid != the game LUID
        for (size_t i = 0; i < S.table.size(); ++i)
        {
            // The flag is not reliable on its own - see the rule 3 note at
            // the top of this file. Vendor 0x1414 is Microsoft, which ships
            // no hardware GPU, so it is the backstop that catches an
            // unflagged WARP / Basic Render Driver / virtual adapter.
            // Never gate on dedicated_vram == 0: integrated GPUs report zero.
            const bool is_software =
                (S.table[i].flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                S.table[i].vendor_id == 0x1414;
            if (is_software)
            {
                snprintf(line, sizeof line,
                         "[MGPU][T2] adapter[%zu] rejected as software (flags=0x%X vendor=0x%04X) "
                         "- enumerated and logged, never selectable",
                         i, (unsigned)S.table[i].flags, (unsigned)S.table[i].vendor_id);
                mgpu::diag::info(line);
                continue;
            }
            hw.push_back(i);
            if (!luid_eq(S.table[i].luid, game))
                cand.push_back(i);
        }

        const char *rule = "none";
        size_t sel = static_cast<size_t>(-1);
        bool degenerate = false;

        if (cand.size() == 1)
        {
            // [rule 1] satisfied: exactly one candidate.
            sel = cand[0];
            rule = "exclusion (luid != swapchain game luid) + software filter";
        }
        else if (cand.empty())
        {
            // [rule 1] refused: nothing besides the game's own card.
            snprintf(line, sizeof line,
                     "[MGPU][T2] REFUSING: no hardware adapter differs from the swapchain-derived "
                     "game luid=0x%08X-0x%08X (%zu hardware adapters total; the game's is the only "
                     "one) - single-adapter topology, P0 needs a second GPU. Selecting nothing: a "
                     "missing device is diagnosable, a device on the wrong adapter is not.",
                     (unsigned)game.HighPart, (unsigned)game.LowPart, hw.size());
            mgpu::diag::error(line);
            rule = "none (refused: no non-game hardware adapter)";
        }
        else
        {
            // [rule 1] not yet satisfied: more than one candidate.
            // [rule 4] the output count is consulted only when more than
            // two hardware adapters exist - and it must never be the
            // primary discriminator.
            for (size_t c : cand)
            {
                snprintf(line, sizeof line,
                         "[MGPU][T2] candidate adapter[%zu] luid=0x%08X-0x%08X outputs=%u "
                         "(rule 4 input)",
                         c, (unsigned)S.table[c].luid.HighPart,
                         (unsigned)S.table[c].luid.LowPart,
                         (unsigned)S.table[c].outputs);
                mgpu::diag::info(line);
            }
            if (hw.size() > 2)
            {
                // This project's topology puts the display on the target
                // card: prefer the single candidate that has outputs, when
                // exactly one does. Any ambiguity refuses - a rig with
                // displays on both cards breaks that rule silently.
                size_t with_outputs = static_cast<size_t>(-1);
                size_t n_with_outputs = 0;
                for (size_t c : cand)
                    if (S.table[c].outputs > 0)
                    {
                        with_outputs = c;
                        ++n_with_outputs;
                    }
                if (n_with_outputs == 1)
                {
                    sel = with_outputs;
                    degenerate = true;
                    rule = "exclusion + software filter + output-count tiebreak "
                           "(display on target card)";
                }
                else
                {
                    snprintf(line, sizeof line,
                             "[MGPU][T2] REFUSING: %zu non-game hardware adapters remain and the "
                             "output-count tiebreak is ambiguous (%zu with outputs>0) - it applies "
                             "only when it picks exactly one. Selecting nothing rather than guessing.",
                             cand.size(), n_with_outputs);
                    mgpu::diag::error(line);
                    rule = "none (refused: ambiguous)";
                }
            }
            else
            {
                // Two hardware adapters total, both "candidates": the
                // swapchain-derived game LUID matched no enumerated
                // adapter. The game's own card is unidentified - refusing
                // is the only safe outcome.
                snprintf(line, sizeof line,
                         "[MGPU][T2] REFUSING: the swapchain-derived game luid=0x%08X-0x%08X matches "
                         "no enumerated adapter (%zu hardware adapters, all candidates) - the "
                         "game's own card is unidentified. Selecting nothing rather than guessing.",
                         (unsigned)game.HighPart, (unsigned)game.LowPart, hw.size());
                mgpu::diag::error(line);
                rule = "none (refused: game luid not in adapter table)";
            }
        }

        S.decided = true;
        if (sel != static_cast<size_t>(-1))
        {
            const entry &e = S.table[sel];
            // Release every adapter except the selected one, which stays
            // AddRef'd for T3's D3D12CreateDevice (it takes the pointer
            // directly - no LUID re-resolution); adapter::shutdown()
            // releases it. selected_index is for the log only.
            for (size_t i = 0; i < S.table.size(); ++i)
            {
                if (i != sel)
                {
                    S.table[i].adapter->Release();
                    S.table[i].adapter = nullptr;
                }
            }
            S.result.valid = true;
            S.result.degenerate = degenerate;
            S.result.selected_luid = e.luid;
            S.result.selected_index = static_cast<UINT>(sel);
            S.result.selected_outputs = e.outputs;
            strncpy(S.result.selected_desc, e.desc, sizeof S.result.selected_desc - 1);
            S.result.selected_desc[sizeof S.result.selected_desc - 1] = '\0';
            S.result.rule = rule;
            S.result.selected_adapter = e.adapter;

            snprintf(line, sizeof line,
                     "[MGPU][T2] SELECTED adapter[%u] luid=0x%08X-0x%08X desc=\"%s\" outputs=%u "
                     "rule=\"%s\" | game luid=0x%08X-0x%08X (source: swapchain)",
                     (unsigned)S.result.selected_index,
                     (unsigned)e.luid.HighPart, (unsigned)e.luid.LowPart,
                     e.desc, (unsigned)e.outputs, rule,
                     (unsigned)game.HighPart, (unsigned)game.LowPart);
            mgpu::diag::info(line);
            if (degenerate)
                mgpu::diag::warn("[MGPU][T2] WARNING: an output-count tie-break was used - confirm "
                                 "the binding manually before trusting anything downstream");
        }
        else
        {
            // Terminal refusal: release every adapter reference.
            for (entry &e : S.table)
            {
                e.adapter->Release();
                e.adapter = nullptr;
            }
            S.result.valid = false;
            S.result.degenerate = false;
            S.result.rule = rule;
            mgpu::diag::error("[MGPU][T2] no adapter selected (see the REFUSING line above) - T3 "
                              "will refuse to create a device");
        }

        // A decision was made - an adapter selected, or a terminal refusal
        // logged. The worker may now read get_selection(); deferrals never
        // reach this line.
        SetEvent(S.ready);
    }
}

void on_device(::reshade::api::device *device)
{
    ensure_init();
    auto &S = st();

    LUID luid{};
    bool is_d3d12 = false;
    if (device != nullptr &&
        device->get_api() == ::reshade::api::device_api::d3d12 &&
        reinterpret_cast<ID3D12Device *>(device->get_native()) != nullptr)
    {
        luid = reinterpret_cast<ID3D12Device *>(device->get_native())->GetAdapterLuid();
        is_d3d12 = true;
    }

    std::lock_guard<std::mutex> lk(S.cs);

    if (is_d3d12 && !S.provisional_known)
    {
        // [rule 2] provisional capture only. The first init_device in a UE5
        // startup is frequently a throwaway probe device (one run captured a
        // software adapter as the "game" LUID), so this value never
        // authorises a selection - the swapchain-derived LUID overrides it.
        // The first capture wins; later device events are logged, not
        // captured.
        S.provisional_known = true;
        S.provisional_luid = luid;
        S.result.game_luid = luid;
        S.result.game_luid_known = true;
        S.result.game_luid_from_swapchain = false;
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][T2] init_device luid=0x%08X-0x%08X captured as PROVISIONAL game luid "
                 "(init_device is not trusted - a UE5 probe device is not the game's renderer; "
                 "the swapchain-derived luid overrides it)",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart);
        mgpu::diag::info(line);
    }
    else if (is_d3d12)
    {
        // A later D3D12CreateDevice in the process - at P0 that is our own
        // T3 device. A free, independent confirmation of the T3 binding;
        // logged with the T3 id, never trusted for selection.
        log_device_luid("init_device (subsequent)", device);
    }
    else
    {
        mgpu::diag::info("[MGPU][T2] init_device: not a d3d12 device - ignored for selection");
    }

    // The first successful pass builds the full table (with the VRAM
    // diagnostics) so it exists in the log even if no swapchain ever
    // arrives; a failed pass retries on the next device event.
    if (!S.table_enumerated)
        enumerate_table_locked(S);

    // A device event never makes the selection on its own: [rule 2]
    // requires the swapchain-derived game LUID. This call only completes a
    // selection whose adapter table was still missing when the swapchain
    // fired.
    try_select_locked(S);
}

void on_swapchain(::reshade::api::swapchain *swapchain, bool resize)
{
    ensure_init();
    if (swapchain == nullptr)
        return;
    ::reshade::api::device *dev = swapchain->get_device();
    ID3D12Device *dev12 = nullptr;
    if (dev != nullptr &&
        dev->get_api() == ::reshade::api::device_api::d3d12 &&
        reinterpret_cast<ID3D12Device *>(dev->get_native()) != nullptr)
        dev12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
    if (dev12 == nullptr)
    {
        mgpu::diag::warn("[MGPU][T2] init_swapchain: the swapchain's device is not d3d12 - this "
                         "event establishes no game luid");
        return;
    }
    // The device that owns the swapchain is the device
    // CreateSwapChainForHwnd was called on - the authoritative game render
    // device.
    const LUID luid = dev12->GetAdapterLuid();

    auto &S = st();
    std::lock_guard<std::mutex> lk(S.cs);

    if (S.result.game_luid_from_swapchain)
    {
        // A subsequent swapchain event (a resize, a re-create, another
        // window's swapchain): logged, never re-select. The selection is
        // one-shot; whatever was decided stands.
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][T2] init_swapchain%s luid=0x%08X-0x%08X - swapchain-derived game luid "
                 "0x%08X-0x%08X already established; not re-selecting",
                 resize ? " (resize)" : "",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart,
                 (unsigned)S.result.game_luid.HighPart,
                 (unsigned)S.result.game_luid.LowPart);
        mgpu::diag::info(line);
        return;
    }

    // [rule 2] the authoritative value; it overrides whatever the
    // provisional init_device capture held.
    S.result.game_luid = luid;
    S.result.game_luid_known = true;
    S.result.game_luid_from_swapchain = true;
    {
        char line[320];
        snprintf(line, sizeof line,
                 "[MGPU][T2] init_swapchain%s: swapchain-derived game luid=0x%08X-0x%08X "
                 "established (the device CreateSwapChainForHwnd was called on)%s",
                 resize ? " [first event was a resize]" : "",
                 (unsigned)luid.HighPart, (unsigned)luid.LowPart,
                 S.provisional_known ? " - overrides the provisional init_device value"
                                     : " - no provisional value had been captured");
        mgpu::diag::info(line);
        if (S.provisional_known && !luid_eq(S.provisional_luid, luid))
        {
            snprintf(line, sizeof line,
                     "[MGPU][T2] WARNING: the provisional init_device luid=0x%08X-0x%08X DIFFERS "
                     "from the swapchain-derived luid - the first init_device was a probe device; "
                     "the swapchain value wins (the case rule 2 exists for)",
                     (unsigned)S.provisional_luid.HighPart,
                     (unsigned)S.provisional_luid.LowPart);
            mgpu::diag::warn(line);
        }
    }

    if (!S.table_enumerated)
        enumerate_table_locked(S);
    try_select_locked(S);
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
    // Drop the decision's wake-up so a re-armed bridge thread (see
    // worker::ensure_started's re-arm) starts from an unset event and
    // cannot act on this run's stale selection.
    if (S.ready != nullptr)
        ResetEvent(S.ready);
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
        if (auto *dev12 = reinterpret_cast<ID3D12Device *>(device->get_native()))
        {
            luid = dev12->GetAdapterLuid();
            have = true;
        }
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