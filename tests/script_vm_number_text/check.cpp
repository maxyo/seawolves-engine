// A float written to a script attribute is stored as printf("%.7f") wrote it; the engine's own formatter
// (src/libs/core/src/number_text.hpp) must give the same text for every float. Random bit patterns, every exponent's
// edges, and the values that fall exactly between two 7-digit decimals (rounded to even).
#include "number_text.hpp"

#include <cstdio>
#include <cstring>
#include <random>

namespace
{
uint64_t checked = 0;
int failures = 0;

void Check(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    char mine[storm::script::kFixed7Size];
    char theirs[storm::script::kFixed7Size];
    const size_t length = storm::script::FormatFixed7(value, mine);
    snprintf(theirs, sizeof(theirs), "%.7f", static_cast<double>(value));
    checked++;
    if (strcmp(mine, theirs) != 0 || length != strlen(theirs))
        if (++failures <= 20)
            std::fprintf(stderr, "FAIL 0x%08x: '%s' (length %zu), printf writes '%s'\n", bits, mine, length, theirs);
}
} // namespace

int main()
{
    std::mt19937 random(7);
    for (int n = 0; n < 8'000'000; n++)
        Check(random());
    // The values scripts hold: small magnitudes with few fraction bits (hit points, coordinates, timers).
    for (int n = 0; n < 2'000'000; n++)
    {
        const float value = static_cast<float>(static_cast<int32_t>(random() % 20'000'001) - 10'000'000) /
                            static_cast<float>(1u << (random() % 20));
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        Check(bits);
    }
    // Every exponent with the smallest, the largest and a few mantissas, both signs: zeros, denormals, the 2^40
    // hand-over to the C library, infinities and NaNs.
    for (uint32_t exponent = 0; exponent < 256; exponent++)
        for (const uint32_t mantissa : {0u, 1u, 2u, 0x400000u, 0x555555u, 0x7ffffeu, 0x7fffffu})
            for (const uint32_t sign : {0u, 0x80000000u})
                Check(sign | (exponent << 23) | mantissa);
    // Ties: odd multiples of 2^-8 (x * 10^7 ends in .5) and their neighbours.
    for (uint32_t n = 1; n < 2'000'000; n += 2)
    {
        const float value = static_cast<float>(n) / 256.0f;
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        for (const uint32_t near : {bits - 1, bits, bits + 1, bits | 0x80000000u})
            Check(near);
    }
    if (failures)
    {
        std::fprintf(stderr, "%d failure(s) in %llu floats\n", failures, static_cast<unsigned long long>(checked));
        return 1;
    }
    std::printf("script VM number text: %llu floats format as printf's %%.7f\n", static_cast<unsigned long long>(checked));
    return 0;
}
