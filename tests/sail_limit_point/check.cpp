// The bulge of a sail with a limit point (src/libs/rigging/src/sail_limit_point.hpp) is the original engine's value:
// `v | vN + fD` with its CVECTOR (an implicit float constructor spreading fD over x, y, z), reproduced here operator
// for operator. The point's signed distance to the sail plane (the former "precedence fix") ballooned the sails of
// many ships into huge shells; it must differ from the expected value, or this check proves nothing.
#include "../../src/libs/rigging/src/sail_limit_point.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace
{
// The original CVECTOR's operators that the expression uses.
struct CVECTOR
{
    float x, y, z;
    CVECTOR(float a) : x(a), y(a), z(a) {}
    CVECTOR(float a, float b, float c) : x(a), y(b), z(c) {}
    friend CVECTOR operator+(const CVECTOR &a, const CVECTOR &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend CVECTOR operator-(const CVECTOR &a, const CVECTOR &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend CVECTOR operator^(const CVECTOR &a, const CVECTOR &b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    friend float operator|(const CVECTOR &a, const CVECTOR &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    friend CVECTOR operator!(const CVECTOR &a)
    {
        const float l = std::sqrt(a | a);
        return l > 0.0f ? CVECTOR(a.x / l, a.y / l, a.z / l) : CVECTOR(0.0f);
    }
};

// The original engine's SAILONE::GetDistanceFromPointTo3Point, verbatim.
float Original(const CVECTOR &v, const CVECTOR &vB1, const CVECTOR &vB2, const CVECTOR &vB3)
{
    const CVECTOR vN = !((vB1 - vB2) ^ (vB3 - vB2));
    const float fD = -(vN | vB2);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
    const float f = v | vN + fD; // `+` binds tighter than `|`: dot(v, vN + (fD, fD, fD))
#pragma GCC diagnostic pop
    return f;
}

void require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
} // namespace

int main()
{
    std::mt19937 random(507);
    std::uniform_real_distribution<float> coordinate(-12.0f, 12.0f);
    int differs = 0;
    for (int i = 0; i < 10000; ++i)
    {
        const glm::vec3 p[4] = {{coordinate(random), coordinate(random), coordinate(random)},
                                {coordinate(random), coordinate(random), coordinate(random)},
                                {coordinate(random), coordinate(random), coordinate(random)},
                                {coordinate(random), coordinate(random), coordinate(random)}};
        auto legacy = [](const glm::vec3 &v) { return CVECTOR(v.x, v.y, v.z); };
        const float expected = Original(legacy(p[0]), legacy(p[1]), legacy(p[2]), legacy(p[3]));
        const float actual = storm::rigging::SailLimitPointBulge(p[0], p[1], p[2], p[3]);
        require(std::fabs(actual - expected) <= 1e-4f * (1.0f + std::fabs(expected)),
                "the limit-point bulge is the original engine's value");
        const glm::vec3 n = storm::math::normalizeOrZero(glm::cross(p[1] - p[2], p[3] - p[2]));
        const float distance = glm::dot(p[0], n) - glm::dot(n, p[2]);
        differs += std::fabs(distance - expected) > 1e-2f;
    }
    require(differs > 9000, "the plane distance differs from the original value (the check can tell them apart)");
    std::puts("sail_limit_point: ok");
    return 0;
}
