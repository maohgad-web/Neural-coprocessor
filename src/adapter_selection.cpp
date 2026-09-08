// MGPU Bridge - pure adapter-choice policy (T2)
#include "adapter_selection.hpp"

#include <string_view>
#include <vector>

namespace mgpu::adapter
{
namespace
{
    bool luid_eq(const LUID &a, const LUID &b) noexcept
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    bool is_space(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    std::string_view trim(std::string_view value) noexcept
    {
        while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
        while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
        return value;
    }

    bool equals_ascii_ci(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const char ac = (a[i] >= 'A' && a[i] <= 'Z')
                          ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
            const char bc = (b[i] >= 'A' && b[i] <= 'Z')
                          ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
            if (ac != bc) return false;
        }
        return true;
    }

    int hex_digit(char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parse_hex_word(std::string_view value, DWORD &out) noexcept
    {
        if (value.size() != 10 || value[0] != '0' ||
            (value[1] != 'x' && value[1] != 'X'))
            return false;
        DWORD parsed = 0;
        for (std::size_t i = 2; i < value.size(); ++i)
        {
            const int digit = hex_digit(value[i]);
            if (digit < 0) return false;
            parsed = (parsed << 4) | static_cast<DWORD>(digit);
        }
        out = parsed;
        return true;
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
        hardware.push_back(i);
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

selector_value parse_secondary_adapter(std::string_view value) noexcept
{
    value = trim(value);
    if (equals_ascii_ci(value, "auto"))
        return {selector_kind::automatic, {}};

    // The bridge log prints high/low in this exact fixed-width form. Refuse
    // every other spelling so a stale index cannot become a GPU choice.
    if (value.size() != 21 || value[10] != '-')
        return {selector_kind::invalid, {}};

    DWORD high = 0, low = 0;
    if (!parse_hex_word(value.substr(0, 10), high) ||
        !parse_hex_word(value.substr(11, 10), low))
        return {selector_kind::invalid, {}};

    LUID luid{};
    luid.HighPart = static_cast<LONG>(high);
    luid.LowPart = low;
    return {selector_kind::explicit_luid, luid};
}

explicit_choice choose_explicit_adapter(const selector_value &selector,
                                         const choice_input *entries,
                                         std::size_t count,
                                         bool game_luid_known,
                                         LUID game_luid) noexcept
{
    if (selector.kind != selector_kind::explicit_luid ||
        (entries == nullptr && count != 0))
        return {explicit_decision::invalid_selector,
                static_cast<std::size_t>(-1), {}};
    if (!game_luid_known)
        return {explicit_decision::game_luid_unknown,
                static_cast<std::size_t>(-1), {}};

    std::size_t found = static_cast<std::size_t>(-1);
    std::size_t matches = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!luid_eq(entries[i].luid, selector.luid))
            continue;
        found = i;
        ++matches;
    }
    if (matches == 0)
        return {explicit_decision::not_found, static_cast<std::size_t>(-1), {}};
    if (matches != 1)
        return {explicit_decision::ambiguous, static_cast<std::size_t>(-1), {}};
    if (luid_eq(entries[found].luid, game_luid))
        return {explicit_decision::target_is_game,
                static_cast<std::size_t>(-1), {}};
    if (entries[found].software)
        return {explicit_decision::target_is_software,
                static_cast<std::size_t>(-1), {}};
    return {explicit_decision::selected, found, entries[found].luid};
}
}
