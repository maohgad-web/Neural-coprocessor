#include "adapter_selection.hpp"

#include <cstdio>
#include <cstdlib>

namespace
{
    LUID luid(LONG high, DWORD low)
    {
        LUID out{};
        out.HighPart = high;
        out.LowPart = low;
        return out;
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "adapter selection regression: %s\n", message);
            std::exit(1);
        }
    }

    void check_case(const char *name, const mgpu::adapter::choice_input *entries,
                    std::size_t count, LUID game_luid, bool expect_valid,
                    std::size_t expect_index, bool expect_game_found,
                    bool expect_degenerate)
    {
        const auto result = mgpu::adapter::choose_adapter(entries, count, game_luid);
        char message[256];
        std::snprintf(message, sizeof message, "%s: unexpected valid result", name);
        require(result.valid == expect_valid, message);
        std::snprintf(message, sizeof message, "%s: unexpected game-LUID presence", name);
        require(result.game_luid_found == expect_game_found, message);
        std::snprintf(message, sizeof message, "%s: unexpected selected index", name);
        require(result.selected_index == expect_index, message);
        std::snprintf(message, sizeof message, "%s: unexpected degenerate result", name);
        require(result.degenerate == expect_degenerate, message);
    }
}

int main()
{
    using mgpu::adapter::choice_input;

    // Missing authoritative game identity must refuse for every hardware
    // count, including the old >2-device output-count corner case.
    const choice_input missing_one[] = {
        {luid(0, 1), false, 1},
    };
    check_case("missing game with one hardware adapter", missing_one, 1,
               luid(0, 99), false, static_cast<std::size_t>(-1), false, false);

    const choice_input missing_two[] = {
        {luid(0, 1), false, 0},
        {luid(0, 2), false, 1},
    };
    check_case("missing game with two hardware adapters", missing_two, 2,
               luid(0, 99), false, static_cast<std::size_t>(-1), false, false);

    const choice_input missing_three[] = {
        {luid(0, 1), false, 0},
        {luid(0, 2), false, 1},
        {luid(0, 3), false, 0},
    };
    check_case("missing game with three hardware adapters", missing_three, 3,
               luid(0, 99), false, static_cast<std::size_t>(-1), false, false);

    // Normal two-GPU exclusion remains selectable.
    const choice_input two_gpu[] = {
        {luid(0, 10), false, 1},
        {luid(0, 11), false, 0},
    };
    check_case("two-GPU exclusion", two_gpu, 2, luid(0, 10), true, 1, true,
               false);

    // Software entries are excluded from candidates, but a software game
    // adapter still proves that the authoritative game LUID was found.
    const choice_input software_filter[] = {
        {luid(0, 20), false, 1},
        {luid(0, 21), true, 1},
        {luid(0, 22), false, 0},
    };
    check_case("software filtering", software_filter, 3, luid(0, 20), true, 2,
               true, false);

    const choice_input software_game[] = {
        {luid(0, 30), true, 0},
        {luid(0, 31), false, 0},
    };
    check_case("software game adapter", software_game, 2, luid(0, 30), true, 1,
               true, false);

    const choice_input no_candidates[] = {
        {luid(0, 40), false, 1},
        {luid(0, 41), true, 0},
    };
    check_case("no hardware candidates", no_candidates, 2, luid(0, 40), false,
               static_cast<std::size_t>(-1), true, false);

    // The documented >2-hardware output tiebreak remains valid when the game
    // LUID is present and exactly one non-game candidate has outputs.
    const choice_input unique_output[] = {
        {luid(0, 50), false, 1},
        {luid(0, 51), false, 1},
        {luid(0, 52), false, 0},
    };
    check_case("unique output tiebreak", unique_output, 3, luid(0, 50), true,
               1, true, true);

    // R189-A. THE SAME TOPOLOGY WITH ONE DISPLAY INVERTS THE TIEBREAK.
    // Extended desktop above: the bridge takes the card that owns the second
    // screen, because that is where its window goes. One display below: the
    // single display marks the card the game renders on, so the bridge takes
    // the card with nothing plugged into it. Selecting the display card there
    // would put both stages on one GPU.
    const choice_input unique_output_single_display[] = {
        {luid(0, 50), false, 1},   // game card, and the only display
        {luid(0, 51), false, 0},
        {luid(0, 52), false, 0},
    };
    check_case("single-display tiebreak refuses when both candidates are dark",
               unique_output_single_display, 3, luid(0, 50), false,
               static_cast<std::size_t>(-1), true, false);

    const choice_input single_display_on_candidate[] = {
        {luid(0, 55), false, 0},   // game card, nothing plugged in
        {luid(0, 56), false, 1},   // candidate holding the only display
        {luid(0, 57), false, 0},   // candidate with nothing plugged in
    };
    check_case("single display selects the dark candidate",
               single_display_on_candidate, 3, luid(0, 55), true, 2, true,
               true);

    const choice_input ambiguous_output[] = {
        {luid(0, 60), false, 1},
        {luid(0, 61), false, 1},
        {luid(0, 62), false, 1},
    };
    check_case("ambiguous output tiebreak", ambiguous_output, 3, luid(0, 60),
               false, static_cast<std::size_t>(-1), true, false);

    // R131. An adapter that cannot run the neural stage is real hardware but
    // never a candidate. Without the filter this topology - game card, a
    // capable second card, and an iGPU with a display - has two candidates
    // with outputs and refuses as ambiguous. With it, exactly one candidate
    // remains and the selection is direct rather than degenerate.
    const choice_input igpu_with_display[] = {
        {luid(0, 80), false, 1, true},   // game card
        {luid(0, 81), false, 1, true},   // the second NVIDIA card
        {luid(0, 82), false, 1, false},  // iGPU, display attached on purpose
    };
    check_case("iGPU with a display is never a candidate", igpu_with_display, 3,
               luid(0, 80), true, 1, true, false);

    // The same topology WITHOUT the filter is the 0.2.2 failure, kept as a
    // test so the regression is visible rather than remembered.
    const choice_input igpu_unfiltered[] = {
        {luid(0, 80), false, 1, true},
        {luid(0, 81), false, 1, true},
        {luid(0, 82), false, 1, true},
    };
    check_case("three capable adapters with outputs still refuse",
               igpu_unfiltered, 3, luid(0, 80), false,
               static_cast<std::size_t>(-1), true, false);

    // A non-capable adapter must not stop the game's own LUID being found,
    // and must not be selectable even when it is the only non-game adapter.
    const choice_input only_igpu[] = {
        {luid(0, 90), false, 1, true},
        {luid(0, 91), false, 1, false},
    };
    check_case("iGPU as the only non-game adapter refuses", only_igpu, 2,
               luid(0, 90), false, static_cast<std::size_t>(-1), true, false);

    // LUID comparison must include HighPart; matching only LowPart would
    // incorrectly treat the second entry as the game adapter.
    const choice_input high_luid_mismatch[] = {
        {luid(1, 0x1234), false, 1},
        {luid(2, 0x1234), false, 0},
        {luid(9, 0x9999), true, 0},
    };
    check_case("high LUID half mismatch", high_luid_mismatch, 3,
               luid(1, 0x1234), true, 1, true, false);

    // Selection is based on identity, not enumeration position; reordering
    // the same topology must select the entry that is now at index zero.
    const choice_input reordered[] = {
        {luid(0, 71), false, 0},
        {luid(0, 70), false, 1},
        {luid(0, 72), true, 0},
    };
    check_case("enumeration reorder", reordered, 3, luid(0, 70), true, 0,
               true, false);

    return 0;
}