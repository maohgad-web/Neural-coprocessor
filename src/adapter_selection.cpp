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

    if (hardware.size() > 2)
    {
        std::size_t with_outputs = static_cast<std::size_t>(-1);
        std::size_t count_with_outputs = 0;
        for (const std::size_t i : candidates)
            if (entries[i].outputs > 0)
            {
                with_outputs = i;
                ++count_with_outputs;
            }
        if (count_with_outputs == 1)
        {
            out.valid = true;
            out.degenerate = true;
            out.selected_index = with_outputs;
        }
    }
    return out;
}
}
