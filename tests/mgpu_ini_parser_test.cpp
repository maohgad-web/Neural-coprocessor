#include "../src/mgpu_ini_parser.hpp"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using mgpu::config::MAX_BYTES;
using mgpu::config::validation;

static void expect(validation got, validation want, const char *name)
{
    if (got != want)
    {
        std::cerr << "FAIL: " << name << "\n";
        std::abort();
    }
}

// This is the bounded-read behavior that existed before the candidate: take
// the first 8191 bytes and let the ordinary line reader decide what is there.
// Keeping it in the executable makes the regression an actual before/after
// check rather than a source-text claim.
static bool legacy_reader_finds_passes(const std::string &text)
{
    const std::size_t legacy_limit = 8191;
    const std::size_t taken = (text.size() < legacy_limit) ? text.size() : legacy_limit;
    std::string clipped(text.data(), taken);
    clipped.push_back('\0');
    const char *p = clipped.c_str();
    const char *key = "Passes";
    const std::size_t key_size = std::strlen(key);
    while (*p != '\0')
    {
        const char *q = p;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != ';' && *q != '#' && std::strncmp(q, key, key_size) == 0 &&
            q[key_size] == '=')
            return true;
        while (*p != '\0' && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    return false;
}

static void truncation_regression()
{
    std::string text(MAX_BYTES / 2, '#');
    text += "\n";
    text.append(MAX_BYTES / 2 - 20, '#');
    text += "\nPasses=4\n";
    assert(text.size() > 8191);

    // The old bounded reader cannot see a setting beyond its 8191-byte read.
    assert(!legacy_reader_finds_passes(text));
    // The candidate parser validates and finds the same setting in full.
    expect(mgpu::config::validate(text.data(), text.size()), validation::ok,
           "overlong-comment candidate document");
    const char *passes = mgpu::config::find(text.data(), text.size(), "Passes");
    assert(passes != nullptr && std::strncmp(passes, "4", 1) == 0);
    std::cout << "PASS: executable baseline reader fails and bounded candidate reader passes\n";
}

int main(int argc, char **argv)
{
    expect(mgpu::config::validate(nullptr, 1), validation::invalid_input, "null input");
    truncation_regression();

    if (argc > 1)
    {
        std::ifstream in(argv[1], std::ios::binary);
        assert(in.good());
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(mgpu::config::validate(text.data(), text.size()), validation::ok,
               "shipped comment-heavy config");
        const char *passes = mgpu::config::find(text.data(), text.size(), "Passes");
        assert(passes != nullptr && std::strncmp(passes, "1", 1) == 0);
        std::cout << "PASS: shipped comment-heavy mgpu.ini is read in full\n";
        return 0;
    }

    {
        const char text[] = "; Passes=99\r\n  Passes=4\r\n# Frames=3\r\nFrames=120\r\n";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::ok, "ordinary config");
        const char *passes = mgpu::config::find(text, sizeof(text) - 1, "Passes");
        const char *frames = mgpu::config::find(text, sizeof(text) - 1, "Frames");
        assert(passes != nullptr && std::strncmp(passes, "4", 1) == 0);
        assert(frames != nullptr && std::strncmp(frames, "120", 3) == 0);
    }

    {
        const char text[] = "\xEF\xBB\xBFPasses=4\n";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::ok, "UTF-8 BOM");
        const char *passes = mgpu::config::find(text, sizeof(text) - 1, "Passes");
        assert(passes != nullptr && std::strncmp(passes, "4", 1) == 0);
    }

    {
        const char text[] = "\xFF\xFEP\0a\0s\0s\0e\0s\0=\0";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::utf16_bom, "UTF-16 BOM");
    }
    {
        const char text[] = "Passes=4\0Frames=120";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::embedded_nul, "embedded NUL");
    }
    {
        const char text[] = "# bad UTF-8: \xC3\x28\nPasses=4\n";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::invalid_utf8, "malformed UTF-8");
    }
    {
        const char text[] = "Passes=4\x01\n";
        expect(mgpu::config::validate(text, sizeof(text) - 1), validation::invalid_control, "control byte");
    }

    // A key at the final byte of the bounded document must still be visible.
    {
        std::string text(MAX_BYTES - 9, '#');
        text += "\nPasses=4";
        assert(text.size() == MAX_BYTES);
        expect(mgpu::config::validate(text.data(), text.size()), validation::ok, "exact boundary");
        const char *passes = mgpu::config::find(text.data(), text.size(), "Passes");
        assert(passes != nullptr && std::strncmp(passes, "4", 1) == 0);
    }
    {
        std::string text(MAX_BYTES + 1, '#');
        expect(mgpu::config::validate(text.data(), text.size()), validation::too_large,
               "overlong comment rejected");
    }

    std::cout << "PASS: mgpu.ini parser boundary, BOM, malformed-input, and comment tests\n";
    return 0;
}
