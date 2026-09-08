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

    const choice_input ambiguous_output[] = {
        {luid(0, 60), false, 1},
        {luid(0, 61), false, 1},
        {luid(0, 62), false, 1},
    };
    check_case("ambiguous output tiebreak", ambiguous_output, 3, luid(0, 60),
               false, static_cast<std::size_t>(-1), true, false);

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