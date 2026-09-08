// MGPU Bridge - pure adapter-choice policy (T2)
#pragma once

#include <windows.h>
#include <cstddef>
#include <string_view>

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

    // Existing automatic policy. The caller owns the enumerated table and
    // keeps the binding on the returned LUID; selected_index is only the
    // current table lookup. A missing game LUID always refuses before the
    // output-count tiebreak.
    choice_result choose_adapter(const choice_input *entries, std::size_t count,
                                 LUID game_luid);

    enum class selector_kind
    {
        automatic,
        explicit_luid,
        invalid,
    };

    struct selector_value
    {
        selector_kind kind = selector_kind::automatic;
        LUID luid{};
    };

    // Parse the value supplied by the config reader. Accepted values are
    // exactly "auto" or the current-run adapter log form
    // 0xHIGH-0xLOW. Enumeration indices, descriptions, partial IDs, and
    // trailing comments are invalid rather than guessed.
    selector_value parse_secondary_adapter(std::string_view value) noexcept;

    enum class explicit_decision
    {
        selected,
        invalid_selector,
        game_luid_unknown,
        not_found,
        ambiguous,
        target_is_game,
        target_is_software,
    };

    struct explicit_choice
    {
        explicit_decision why = explicit_decision::invalid_selector;
        std::size_t selected_index = static_cast<std::size_t>(-1);
        LUID selected_luid{};
    };

    // Production explicit-selector policy. Automatic selection is deliberately
    // not represented here: callers must continue through choose_adapter(),
    // preserving the existing default and its separate missing-game fix.
    // A known swapchain game LUID is mandatory, but the explicit target does
    // not infer or require that the game LUID appear in this enumeration: that
    // remains the auto-path source-review concern.
    explicit_choice choose_explicit_adapter(const selector_value &selector,
                                             const choice_input *entries,
                                             std::size_t count,
                                             bool game_luid_known,
                                             LUID game_luid) noexcept;
}
