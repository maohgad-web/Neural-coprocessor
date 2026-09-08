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
