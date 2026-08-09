#pragma once

#include <stddef.h>
#include <stdint.h>

namespace text_encoding {

struct Utf8Decoder {
    uint32_t codepoint;
    uint32_t minimum;
    uint8_t remaining;
};

enum class DecodeResult : uint8_t {
    Pending,
    Complete,
    Invalid,
    InvalidRetry,
};

inline void reset(Utf8Decoder& decoder) {
    decoder = {};
}

inline DecodeResult decode_utf8_byte(Utf8Decoder& decoder,
                                     uint8_t byte,
                                     uint32_t& codepoint) {
    if (decoder.remaining == 0) {
        if (byte < 0x80) {
            codepoint = byte;
            return DecodeResult::Complete;
        }
        if (byte >= 0xC2 && byte <= 0xDF) {
            decoder.codepoint = byte & 0x1Fu;
            decoder.minimum = 0x80;
            decoder.remaining = 1;
            return DecodeResult::Pending;
        }
        if (byte >= 0xE0 && byte <= 0xEF) {
            decoder.codepoint = byte & 0x0Fu;
            decoder.minimum = 0x800;
            decoder.remaining = 2;
            return DecodeResult::Pending;
        }
        if (byte >= 0xF0 && byte <= 0xF4) {
            decoder.codepoint = byte & 0x07u;
            decoder.minimum = 0x10000;
            decoder.remaining = 3;
            return DecodeResult::Pending;
        }
        return DecodeResult::Invalid;
    }

    if ((byte & 0xC0u) != 0x80u) {
        reset(decoder);
        return DecodeResult::InvalidRetry;
    }

    decoder.codepoint = (decoder.codepoint << 6) | (byte & 0x3Fu);
    --decoder.remaining;
    if (decoder.remaining != 0) {
        return DecodeResult::Pending;
    }

    codepoint = decoder.codepoint;
    const uint32_t minimum = decoder.minimum;
    reset(decoder);
    if (codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return DecodeResult::Invalid;
    }
    return DecodeResult::Complete;
}

// Neutrino VTY cells currently hold one byte. Map Unicode text to the CP437
// glyph order used by the built-in 256-glyph console font.
inline uint8_t unicode_to_cp437(uint32_t codepoint) {
    if (codepoint < 0x80) {
        return static_cast<uint8_t>(codepoint);
    }

    static constexpr uint16_t kExtendedCodepoints[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
        0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
        0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
        0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
        0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
    };

    for (size_t i = 0; i < 128; ++i) {
        if (kExtendedCodepoints[i] == codepoint) {
            return static_cast<uint8_t>(i + 0x80);
        }
    }
    return '?';
}

inline uint8_t basic_font_fallback(uint8_t glyph) {
    if (glyph == 0xB3 || glyph == 0xBA) {
        return '|';
    }
    if (glyph == 0xC4 || glyph == 0xCD) {
        return '-';
    }
    if (glyph >= 0xB4 && glyph <= 0xDA) {
        return '+';
    }
    return '?';
}

}  // namespace text_encoding
