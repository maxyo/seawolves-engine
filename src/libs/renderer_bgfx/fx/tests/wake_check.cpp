// The ship wakes without the GPU (fx::SeaWakes): the Kelvin pattern of include/fx_kelvin.sh (the shader's own code)
// and the ribbon along a hull's track (wake_path.hpp): the wake trails behind the bow, inside the 19.47-degree V,
// with the transverse wavelength 2 pi U^2 / g, fading with distance and age, bending with a turn without folding.
#include "storm/fx/wake_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace kelvin
{
struct vec4
{
    float x, y, z, w;
    vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float exp(float v) { return std::exp(v); }
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float smoothstep(float a, float b, float v)
{
    const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
#include "../shaders/include/fx_kelvin.sh"
} // namespace kelvin

namespace
{
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (false)

using storm::fx::WakePaths;
using storm::fx::WakeVertex;
using storm::fx::WaveSource;
constexpr float kPi = 3.14159265f;
constexpr float kCusp = 19.47f * kPi / 180.0f;

WaveSource Hull(double x, double z, float heading, float speed)
{
    WaveSource s;
    s.kind = WaveSource::Kind::Hull;
    s.identity = 7;
    s.position = {x, z};
    s.forward = {std::sin(heading), std::cos(heading)};
    s.stern = -15.0f;
    s.stem = 15.0f;
    s.half_width = 5.0f;
    s.speed = speed;
    return s;
}
} // namespace

int main()
{
    // The pattern: U = 5 m/s, k0 = g / U^2.
    const float k0 = 9.81f / 25.0f;
    const auto envelope = [&](float x, float y) { return kelvin::fxKelvin(x, y, k0, 15.0f, 1.0f, 1.0f, 1.0f).w; };
    CHECK(envelope(-10.0f, 0.0f) == 0.0f && envelope(0.0f, 3.0f) == 0.0f); // nothing ahead of the disturbance
    // At 150 m, the waves are highest on the cusp line and gone well outside the wedge.
    float best = 0.0f, best_angle = 0.0f;
    for (float angle = 0.5f; angle < 35.0f; angle += 0.25f)
    {
        const float a = angle * kPi / 180.0f;
        const float e = envelope(150.0f * std::cos(a), 150.0f * std::sin(a));
        if (e > best)
            best = e, best_angle = angle;
    }
    std::printf("kelvin: envelope peak at %.2f degrees (cusp 19.47)\n", best_angle);
    CHECK(std::fabs(best_angle - 19.47f) < 2.0f);
    const float outside = envelope(150.0f * std::cos(30.0f * kPi / 180.0f), 150.0f * std::sin(30.0f * kPi / 180.0f));
    std::printf("kelvin: envelope at 30 degrees %.5f of the peak\n", outside / best);
    CHECK(outside < 0.01f * best);
    // Symmetric about the track; the side slope changes sign with the side.
    const auto left = kelvin::fxKelvin(80.0f, -12.0f, k0, 15.0f, 1.0f, 1.0f, 1.0f);
    const auto right = kelvin::fxKelvin(80.0f, 12.0f, k0, 15.0f, 1.0f, 1.0f, 1.0f);
    CHECK(std::fabs(left.x - right.x) < 1e-5f && std::fabs(left.y - right.y) < 1e-5f && std::fabs(left.z + right.z) < 1e-5f);
    // The transverse waves on the track: crests 2 pi / k0 apart (16 m at 5 m/s).
    std::vector<float> crossings;
    float previous = kelvin::fxKelvin(40.0f, 0.0f, k0, 15.0f, 1.0f, 1.0f, 0.0f).x;
    for (float x = 40.05f; x < 200.0f; x += 0.05f)
    {
        const float h = kelvin::fxKelvin(x, 0.0f, k0, 15.0f, 1.0f, 1.0f, 0.0f).x;
        if ((h > 0.0f) != (previous > 0.0f))
            crossings.push_back(x);
        previous = h;
    }
    CHECK(crossings.size() > 4);
    if (crossings.size() > 4)
    {
        const float wavelength = 2.0f * (crossings.back() - crossings.front()) / static_cast<float>(crossings.size() - 1);
        std::printf("kelvin: transverse wavelength %.2f m (2 pi U^2 / g = %.2f m)\n", wavelength, 2.0f * kPi / k0);
        CHECK(std::fabs(wavelength - 2.0f * kPi / k0) < 0.3f);
    }
    // The slope is the height's gradient (finite differences off the cusp).
    {
        const float x = 90.0f, y = 14.0f, e = 0.01f;
        const auto c = kelvin::fxKelvin(x, y, k0, 15.0f, 1.0f, 1.0f, 1.0f);
        const float gx = (kelvin::fxKelvin(x + e, y, k0, 15.0f, 1.0f, 1.0f, 1.0f).x -
                          kelvin::fxKelvin(x - e, y, k0, 15.0f, 1.0f, 1.0f, 1.0f).x) / (2.0f * e);
        const float gy = (kelvin::fxKelvin(x, y + e, k0, 15.0f, 1.0f, 1.0f, 1.0f).x -
                          kelvin::fxKelvin(x, y - e, k0, 15.0f, 1.0f, 1.0f, 1.0f).x) / (2.0f * e);
        std::printf("kelvin: slope (%.4f, %.4f), finite differences (%.4f, %.4f)\n", c.y, c.z, gx, gy);
        CHECK(std::fabs(c.y - gx) < 0.15f * std::hypot(gx, gy) + 1e-3f && std::fabs(c.z - gy) < 0.15f * std::hypot(gx, gy) + 1e-3f);
    }
    // Waves shorter than the band limit are left out: the divergent waves close to the track.
    CHECK(std::fabs(kelvin::fxKelvin(100.0f, 0.5f, k0, 15.0f, 4.0f, 0.0f, 1.0f).w) < 1e-4f);

    // A hull sailing north (+z) at 5 m/s for 40 s of 1/30 s frames.
    WakePaths paths;
    double t = 0.0, z = 0.0;
    for (int frame = 0; frame < 1200; ++frame)
    {
        t = frame / 30.0;
        z = frame * 5.0 / 30.0;
        paths.Update(std::span<const WaveSource>(std::vector<WaveSource>{Hull(0.0, z, 0.0f, 5.0f)}), t);
    }
    CHECK(paths.Paths().size() == 1);
    const auto &path = paths.Paths().front();
    std::vector<WakeVertex> ribbon;
    const size_t count = WakePaths::Ribbon(path, t, {0.0, z}, 512.0f, false, ribbon);
    CHECK(count >= 40 && count % 2 == 0);
    bool behind = true, symmetric = true, ordered = true, fading = true, in_wedge = true;
    for (size_t i = 0; i + 1 < count; i += 2)
    {
        const WakeVertex &a = ribbon[i], &b = ribbon[i + 1];
        // Back along the track is south: the wake trails behind the bow, never ahead of it or to the side.
        behind &= a.back_z < -0.99f && std::fabs(a.back_x) < 0.01f;
        // The bow is at z - 0 + stem 15: every vertex lies at or behind it, its distance behind matching.
        const double vz = a.y * 256.0 + z;
        behind &= vz <= z + 15.0 + 0.01 && std::fabs((z + 15.0 - vz) - a.along) < 0.05;
        symmetric &= std::fabs(a.side + b.side) < 1e-4f && std::fabs(a.x + b.x) < 1e-6f;
        if (i >= 2)
        {
            ordered &= a.along > ribbon[i - 2].along;
            fading &= a.fade <= ribbon[i - 2].fade + 1e-6f;
        }
        // The ribbon holds the V and its margin: wider than the cusp line, not much more.
        if (a.along > 30.0f)
            in_wedge &= std::fabs(b.side) > a.along * std::tan(kCusp) &&
                        std::fabs(b.side) < a.along * std::tan(kCusp) * 1.5f + 15.0f;
    }
    std::printf("wake: behind %d symmetric %d ordered %d fading %d in wedge %d\n", behind, symmetric, ordered, fading, in_wedge);
    CHECK(behind && symmetric && ordered && fading && in_wedge);
    CHECK(ribbon[count - 1].fade == 0.0f);                 // the far end fades out
    CHECK(ribbon[count - 1].along <= WakePaths::kMaxLength + WakePaths::kSpacing);
    CHECK(std::fabs(ribbon[0].k0 - 9.81f / 25.0f) < 1e-4f);
    std::printf("wake: %zu vertices over %.0f m of track, half width at 100 m %.1f m\n", count, ribbon[count - 1].along,
                [&] { for (size_t i = 0; i < count; i += 2) if (ribbon[i].along >= 100.0f) return std::fabs(ribbon[i].side); return 0.0f; }());

    // The rows of a target that are not GL's run from the top: the ribbon is mirrored in y only.
    std::vector<WakeVertex> flipped;
    WakePaths::Ribbon(path, t, {0.0, z}, 512.0f, true, flipped);
    CHECK(flipped.size() == count && flipped[5].y == -ribbon[5].y && flipped[5].x == ribbon[5].x);

    // A turn (radius 60 m): the wake follows the circle sailed, its inner edge never folds back.
    WakePaths turning;
    const double radius = 60.0, omega = 5.0 / radius;
    for (int frame = 0; frame < 900; ++frame)
    {
        t = frame / 30.0;
        const double a = omega * t;
        // Centre of the circle at (radius, 0): sailing clockwise seen from above, heading turning right.
        const WaveSource s = Hull(radius - radius * std::cos(a), radius * std::sin(a), static_cast<float>(a), 5.0f);
        turning.Update(std::span<const WaveSource>(&s, 1), t);
    }
    std::vector<WakeVertex> bent;
    const size_t bent_count = WakePaths::Ribbon(turning.Paths().front(), t, {0.0, 0.0}, 512.0f, false, bent);
    bool on_track = true, unfolded = true;
    for (size_t i = 0; i + 1 < bent_count; i += 2)
    {
        // The centre between the two vertices is on the circle of the bow's track (radius sqrt(60^2 + 15^2)).
        const double cx = 0.5 * (bent[i].x + bent[i + 1].x) * 256.0, cz = 0.5 * (bent[i].y + bent[i + 1].y) * 256.0;
        on_track &= std::fabs(std::hypot(cx - radius, cz) - std::hypot(radius, 15.0)) < 0.5;
        if (i >= 2)
            for (size_t side = 0; side < 2; ++side)
            {
                // Each edge keeps going back along the track.
                const double ex = (bent[i + side].x - bent[i - 2 + side].x) * 256.0;
                const double ez = (bent[i + side].y - bent[i - 2 + side].y) * 256.0;
                unfolded &= ex * bent[i].back_x + ez * bent[i].back_z > 0.0;
            }
    }
    CHECK(bent_count > 20 && on_track && unfolded);

    // A stopped hull lays no more track; a jump starts a new one; a hull gone for kLife is dropped.
    WakePaths stopping;
    stopping.Update(std::span<const WaveSource>(std::vector<WaveSource>{Hull(0.0, 0.0, 0.0f, 0.0f)}), 0.0);
    stopping.Update(std::span<const WaveSource>(std::vector<WaveSource>{Hull(0.0, 0.2, 0.0f, 0.1f)}), 1.0);
    CHECK(stopping.Paths().front().points.size() == 1);
    stopping.Update(std::span<const WaveSource>(std::vector<WaveSource>{Hull(500.0, 0.0, 0.0f, 5.0f)}), 2.0);
    CHECK(stopping.Paths().front().points.size() == 1 && stopping.Paths().front().points.front().x == 500.0);
    stopping.Update({}, 2.0 + WakePaths::kLife + 1.0);
    CHECK(stopping.Paths().empty());

    std::printf("wake checks: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
