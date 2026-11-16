#pragma once

#include "vm_verify.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace storm::script
{
constexpr size_t kFixed7Size = 64; // the longest text: a sign, 39 digits of FLT_MAX, the point, 7 digits

// The text of printf("%.7f", value), which every float assigned to a script attribute is stored as (and saved as):
// the exact value rounded to 7 decimals, ties to even. Returns its length; `out` has kFixed7Size bytes.
// A script loop that updates a few floats of every character each frame spent more in printf than in its own
// arithmetic. Values of 2^40 and above, infinities and NaN are left to the C library.
inline size_t FormatFixed7(float value, char *out) noexcept
{
    constexpr uint64_t kScale = 10'000'000;
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponentBits = (bits >> 23) & 0xff;
    uint64_t mantissa = bits & 0x7fffff;
    int exponent = -149; // |value| = mantissa * 2^exponent
    if (exponentBits != 0)
    {
        mantissa |= 0x800000;
        exponent = static_cast<int>(exponentBits) - 150;
    }
    if (exponentBits == 0xff || exponent > 16)
        return static_cast<size_t>(snprintf(out, kFixed7Size, "%.7f", static_cast<double>(value)));

    uint64_t scaled; // |value| * 10^7 rounded to an integer; below 2^64 for |value| < 2^40
    if (exponent >= 0)
        scaled = (mantissa << exponent) * kScale;
    else if (exponent <= -64)
        scaled = 0; // mantissa * 10^7 < 2^48: far below half a unit
    else
    {
        const int shift = -exponent;
        const uint64_t product = mantissa * kScale;
        scaled = product >> shift;
        const uint64_t rest = product & ((uint64_t{1} << shift) - 1);
        const uint64_t half = uint64_t{1} << (shift - 1);
        if (rest > half || (rest == half && (scaled & 1)))
            scaled++;
    }

    char *at = out;
    if (bits >> 31)
        *at++ = '-'; // also -0.0 and negatives that round to zero, as printf writes them
    at = std::to_chars(at, at + 24, scaled / kScale).ptr;
    *at++ = '.';
    uint32_t fraction = static_cast<uint32_t>(scaled % kScale);
    for (int digit = 6; digit >= 0; digit--)
    {
        at[digit] = static_cast<char>('0' + fraction % 10);
        fraction /= 10;
    }
    at += 7;
    *at = 0;
#ifdef STORM_VM_VERIFY
    vm_verify::Count(vm_verify::Check::FloatText);
    char reference[kFixed7Size];
    snprintf(reference, sizeof(reference), "%.7f", static_cast<double>(value));
    if (strcmp(reference, out) != 0)
        vm_verify::Mismatch("%%.7f of the float 0x%08x: '%s', printf writes '%s'", bits, out, reference);
#endif
    return static_cast<size_t>(at - out);
}
} // namespace storm::script
