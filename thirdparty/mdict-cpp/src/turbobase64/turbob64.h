/*
    SPDX-License-Identifier: GPL-2.0-or-later

    Small compatibility layer for the base64 calls used by mdict-cpp.
*/

#pragma once

#include <cstddef>
#include <cstdint>

inline size_t tb64enclen(size_t size)
{
    return ((size + 2) / 3) * 4;
}

inline size_t tb64declen(const uint8_t *data, size_t size)
{
    while (size && data[size - 1] == '=') {
        --size;
    }
    return size * 3 / 4;
}

inline void tb64enc(const uint8_t *source, size_t size, uint8_t *target)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output = 0;
    for (size_t input = 0; input < size; input += 3) {
        const uint32_t value = (uint32_t(source[input]) << 16) | (input + 1 < size ? uint32_t(source[input + 1]) << 8 : 0) | (input + 2 < size ? source[input + 2] : 0);
        target[output++] = alphabet[(value >> 18) & 63];
        target[output++] = alphabet[(value >> 12) & 63];
        target[output++] = input + 1 < size ? alphabet[(value >> 6) & 63] : '=';
        target[output++] = input + 2 < size ? alphabet[value & 63] : '=';
    }
}

inline void tb64dec(const uint8_t *source, size_t size, uint8_t *target)
{
    uint32_t value = 0;
    int bits = 0;
    size_t output = 0;
    for (size_t input = 0; input < size && source[input] != '='; ++input) {
        const uint8_t character = source[input];
        int digit = -1;
        if (character >= 'A' && character <= 'Z') {
            digit = character - 'A';
        } else if (character >= 'a' && character <= 'z') {
            digit = character - 'a' + 26;
        } else if (character >= '0' && character <= '9') {
            digit = character - '0' + 52;
        } else if (character == '+') {
            digit = 62;
        } else if (character == '/') {
            digit = 63;
        }
        if (digit < 0) {
            continue;
        }
        value = (value << 6) | uint32_t(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            target[output++] = uint8_t(value >> bits);
        }
    }
}
