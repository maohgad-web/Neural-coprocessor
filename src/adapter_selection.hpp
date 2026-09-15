// MGPU Bridge - pure adapter-choice policy (T2)
#pragma once

#include <windows.h>
#include <cstddef>

namespace mgpu::adapter
{
    struct choice_input
    {
        LUID luid{};
        bool software = false;
        UINT outputs = 0;

        // R131. Can this adapter run the neural stage at all?
        //
        // The stage is DLSS Neural Rendering, which needs an NVIDIA RTX
        // adapter. An integrated AMD or Intel GPU is hardware, is not
        // software, and could never have been a valid selection - but until
        // R131 it entered the candidate list anyway, and on a machine with a
        // display attached to it, it made the rule 4 tiebreak ambiguous and
        // the whole selection refuse. Reported against 0.2.2 on a rig with
        // three adapters carrying displays by design.
        //
        // DEFAULTS TRUE so that every existing aggregate initialiser - the
        // test file included - keeps its meaning. The vendor rule itself
        // lives in adapter.cpp beside the enumeration that owns vendor ids
        // and does the logging, exactly as `software` already does. This
        // file stays pure policy and knows no vendor numbers.
        bool neural_capable = true;
    };

    struct choice_result
    {
        bool valid = false;
        bool game_luid_found = false;
        bool degenerate = false;
        std::size_t selected_index = static_cast<std::size_t>(-1);
    };

    // Select only when the authoritative game LUID is present in the
    // enumerated table. A missing game adapter is never evidence that every
    // hardware adapter is a candidate: selecting one in that state can bind
    // the bridge to the wrong GPU.
    choice_result choose_adapter(const choice_input *entries, std::size_t count,
                                 LUID game_luid);
}
