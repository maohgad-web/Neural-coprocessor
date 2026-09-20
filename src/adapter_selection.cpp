// MGPU Bridge - pure adapter-choice policy (T2)
#include "adapter_selection.hpp"

#include <vector>

namespace mgpu::adapter
{
namespace
{
    bool luid_eq(const LUID &a, const LUID &b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }
}

choice_result choose_adapter(const choice_input *entries, std::size_t count,
                             LUID game_luid)
{
    choice_result out;
    if (entries == nullptr && count != 0)
        return out;

    std::vector<std::size_t> hardware;
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < count; ++i)
    {
        const bool game_match = luid_eq(entries[i].luid, game_luid);
        out.game_luid_found = out.game_luid_found || game_match;
        if (entries[i].software)
            continue;

        // `hardware` stays a count of PHYSICAL adapters, including ones that
        // cannot run the neural stage. It is only consulted by the rule 4
        // gate below, whose question is "how many real GPUs are in this
        // machine" - and an iGPU is a real GPU. Narrowing it here would
        // change that gate's meaning for a reason unrelated to it.
        hardware.push_back(i);

        // R131. What DOES narrow is candidacy. An adapter that cannot run
        // the neural stage is not a thing to select, so it must not reach
        // the tiebreak and must not be able to make it ambiguous. This is
        // strictly narrowing: it can only remove an adapter that would have
        // failed after selection, so no selection that succeeds today
        // changes.
        if (!entries[i].neural_capable)
            continue;

        if (!game_match)
            candidates.push_back(i);
    }

    // This gate must precede all candidate/tiebreak logic. Otherwise a
    // missing game LUID turns every hardware adapter into a false candidate.
    if (!out.game_luid_found || candidates.empty())
        return out;

    if (candidates.size() == 1)
    {
        out.valid = true;
        out.selected_index = candidates[0];
        return out;
    }

    // ---- R189-A: THE TIEBREAK DEPENDS ON HOW MANY DISPLAYS EXIST ----
    //
    // Original behaviour, kept unchanged for an EXTENDED desktop: pick the
    // candidate that has a display. Two monitors one per card is the measured
    // arrangement, the bridge presents its own window on the second screen,
    // and it needs an adapter that owns that screen. Nothing about that case
    // was ever wrong and none of it moves here.
    //
    // SINGLE DISPLAY INVERTS IT. With one monitor there is no second screen
    // for the bridge to own, and the one display marks the card Windows
    // renders the game on. Selecting it puts the neural stage on the game's
    // own GPU - both loads on one card, the other idle - and the only symptom
    // is that everything is slower, which reads as a hardware problem rather
    // than a selection one. So with one display the bridge takes the card
    // with NOTHING plugged into it.
    //
    // The display count is summed from this table's own `outputs`, not from
    // the CCD namespace, deliberately: the tiebreak already decides on DXGI
    // output counts, and a gate that asked a different namespace could
    // disagree with the branch it is gating.
    //
    // Reachable only with THREE OR MORE physical GPUs and more than one
    // NVIDIA candidate, which together need the swapchain-derived game LUID
    // to land on an adapter that is neither discrete card - an enabled iGPU
    // being the ordinary way that happens. A two-GPU machine never executes
    // this in either form.
    if (hardware.size() > 2)
    {
        UINT total_outputs = 0;
        for (const std::size_t i : hardware)
            total_outputs += entries[i].outputs;
        const bool single_display = (total_outputs <= 1u);

        std::size_t pick = static_cast<std::size_t>(-1);
        std::size_t matches = 0;
        for (const std::size_t i : candidates)
        {
            const bool want = single_display ? (entries[i].outputs == 0)
                                             : (entries[i].outputs > 0);
            if (want) { pick = i; ++matches; }
        }
        if (matches == 1)
        {
            out.valid = true;
            out.degenerate = true;
            out.selected_index = pick;
        }
    }
    return out;
}
}
