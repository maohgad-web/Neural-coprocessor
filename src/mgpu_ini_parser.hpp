// Portable, bounded parser primitives for the public mgpu.ini format.
//
// This header intentionally contains no Windows or ReShade dependency so the
// reader's boundary behavior can be regression-tested without loading a DLL.
#pragma once

#include <cstddef>
#include <cstring>

namespace mgpu::config
{
// The file buffer keeps one byte for a terminating NUL.  The limit is a
// contract: a document at or below it is parsed in full; a larger document is
// rejected before any setting is consumed.
// 64 KiB is deliberately large enough for the shipped, comment-heavy
// configuration while still making allocation and read behavior bounded.
static constexpr std::size_t MAX_BYTES = 65535;

enum class validation
{
    invalid_input,
    ok,
    empty,
    too_large,
    utf16_bom,
    embedded_nul,
    invalid_control,
    invalid_utf8,
};

inline bool has_utf8_bom(const char *data, std::size_t size) noexcept
{
    return size >= 3 && static_cast<unsigned char>(data[0]) == 0xEFu &&
           static_cast<unsigned char>(data[1]) == 0xBBu &&
           static_cast<unsigned char>(data[2]) == 0xBFu;
}

inline validation validate(const char *data, std::size_t size) noexcept
{
    if (data == nullptr) return validation::invalid_input;
    if (size == 0) return validation::empty;
    if (size > MAX_BYTES) return validation::too_large;
    if (size >= 2 && ((static_cast<unsigned char>(data[0]) == 0xFFu &&
                       static_cast<unsigned char>(data[1]) == 0xFEu) ||
                      (static_cast<unsigned char>(data[0]) == 0xFEu &&
                       static_cast<unsigned char>(data[1]) == 0xFFu)))
        return validation::utf16_bom;

    std::size_t i = has_utf8_bom(data, size) ? 3u : 0u;
    while (i < size)
    {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        // Config syntax is text.  NUL is never a valid byte in a file read
        // from disk, and other C0 controls cannot be represented safely in
        // line-oriented parsing.  Tabs and CR/LF remain valid for INI syntax.
        if (c == 0u) return validation::embedded_nul;
        if (c < 0x20u && c != '\t' && c != '\r' && c != '\n')
            return validation::invalid_control;
        if (c < 0x80u) { ++i; continue; }

        // Validate UTF-8 so a truncated/malformed comment cannot make the
        // reader walk a different byte sequence than the file contained.
        std::size_t need = 0;
        unsigned int codepoint = 0;
        if (c >= 0xC2u && c <= 0xDFu) { need = 1; codepoint = c & 0x1Fu; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; codepoint = c & 0x0Fu; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; codepoint = c & 0x07u; }
        else return validation::invalid_utf8;
        if (i + need >= size) return validation::invalid_utf8;
        for (std::size_t j = 1; j <= need; ++j)
        {
            const unsigned char t = static_cast<unsigned char>(data[i + j]);
            if ((t & 0xC0u) != 0x80u) return validation::invalid_utf8;
            codepoint = (codepoint << 6) | (t & 0x3Fu);
        }
        if ((need == 2 && codepoint < 0x800u) ||
            (need == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
            return validation::invalid_utf8;
        i += need + 1;
    }
    return validation::ok;
}

// Remove a leading UTF-8 BOM in-place after validate() has accepted it.  A
// UTF-16 BOM is intentionally not accepted: this public reader is byte-based,
// and treating UTF-16 as if it were ASCII would silently ignore every key.
inline bool strip_utf8_bom(char *data, std::size_t &size) noexcept
{
    if (data == nullptr) return false;
    if (!has_utf8_bom(data, size)) return false;
    std::memmove(data, data + 3, size - 3);
    size -= 3;
    data[size] = '\0';
    return true;
}

// Find the first active, line-start key while preserving the existing syntax:
// optional leading spaces/tabs, key=, and ';'/'#' whole-line comments.
inline const char *find(const char *data, std::size_t size, const char *key) noexcept
{
    if (data == nullptr || key == nullptr) return nullptr;
    const std::size_t key_size = std::strlen(key);
    std::size_t p = has_utf8_bom(data, size) ? 3u : 0u;
    while (p < size)
    {
        const std::size_t line_end = [&]() {
            std::size_t e = p;
            while (e < size && data[e] != '\n') ++e;
            return e;
        }();
        std::size_t q = p;
        while (q < line_end && (data[q] == ' ' || data[q] == '\t')) ++q;
        if (q < line_end && data[q] != ';' && data[q] != '#' &&
            key_size <= line_end - q &&
            std::memcmp(data + q, key, key_size) == 0 &&
            q + key_size < line_end && data[q + key_size] == '=')
            return data + q + key_size + 1;
        p = (line_end < size) ? line_end + 1 : size;
    }
    return nullptr;
}
} // namespace mgpu::config
