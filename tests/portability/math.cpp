#include "float_to_int.h"
#include <xmmintrin.h> // x86 reference oracle; this test is intentionally host-only
#include <cfenv>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include "math_inlines.h"

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
static void check_conversion(float value)
{
    volatile float input = value;
    const auto vector = _mm_load_ss(const_cast<const float *>(&input));
    require(storm::math::round_to_int(input) == _mm_cvtss_si32(vector), "round differs from SSE");
    require(storm::math::truncate_to_int(input) == _mm_cvttss_si32(vector), "truncate differs from SSE");
    require(ftoi(input) == _mm_cvtss_si32(vector), "ftoi wrapper");
    require(fftoi(input) == _mm_cvtss_si32(vector), "fftoi wrapper");
    require(fftol(input) == _mm_cvttss_si32(vector), "fftol wrapper");
    require(ffloor(input) == _mm_cvtss_si32(_mm_add_ss(vector, _mm_set_ss(-.5f))), "legacy ffloor wrapper");
}
int main()
{
    std::mt19937 random(0x53544f52);
    const int original = std::fegetround();
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
    {
        require(std::fesetround(rounding) == 0, "rounding mode unavailable");
        for (float v : {0.f, -0.f, .5f, -.5f, 1.5f, -1.5f, 2.5f, -2.5f,
                        2147483648.f, -2147483648.f, 2147483520.f,
                        INFINITY, -INFINITY, NAN}) check_conversion(v);
        for (int i = 0; i < 100000; ++i)
        {
            const uint32_t bits = random();
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            check_conversion(value);
        }
    }
    std::fesetround(original);
    std::cout << "PASS: 400056 float conversions; SSE=" << STORM_MATH_HAS_SSE << '\n';
}
