#include "../../src/libs/ship/src/track_sampling.hpp"
#include <cstdio>
#include <vector>

int main()
{
    int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failed; } } while (false)
    using storm::ship::TrackSpacing;
    using storm::ship::TrackWidth;
    std::vector<double> reference;
    for (int scale : {1, 2, 4})
    {
        TrackSpacing spacing;
        std::vector<double> born;
        double traveled = 0, old_remainder = 0;
        size_t old_count = 0;
        for (int frame = 0; frame < 300 / scale; ++frame)
        {
            const float dt = scale / 30.0f, distance = 5 * dt;
            spacing.Advance(distance, .7f, 0, 0, [&](float along, float fraction, float yaw) {
                CHECK(fraction >= 0 && fraction <= 1 && yaw == 0);
                born.push_back((traveled + along) / 5);
            });
            traveled += distance;
            old_remainder += distance;
            if (old_remainder > .7f) { old_count += size_t(old_remainder / .7f); old_remainder = 0; }
        }
        CHECK(born.size() == 71 && std::abs(spacing.remainder - .3) < .00001);
        for (size_t i = 0; i < born.size(); ++i) CHECK(std::abs(born[i] - (i+1) * .14) < .00001);
        if (reference.empty()) reference = born;
        else for (size_t i = 0; i < born.size(); ++i) CHECK(std::abs(born[i] - reference[i]) < .00001);
        const auto count = born.size();
        spacing.Advance(0, .7f, 0, 0, [&](float, float, float) { born.push_back(-1); });
        CHECK(born.size() == count); // no foam emission at rest
        spacing.Reset();
        spacing.Advance(.6f, .7f, 0, 0, [&](float, float, float) { born.push_back(-1); });
        CHECK(born.size() == count); // scene/teleport reset discards a previous partial interval
        std::printf("foam x%d: %zu distance samples; old frame-reset control %zu; max birth-time error <10 us\n", scale, count, old_count);
    }
    // Several samples in a turning frame have distinct headings and birth times. Interpolate the short way
    // through +/-pi, avoiding a half-turn fan in the ribbon.
    TrackSpacing turning;
    std::vector<float> fractions, angles;
    turning.Advance(3, 1, 3.1f, -3.1f, [&](float along, float fraction, float yaw) {
        CHECK(std::abs(along - fraction*3) < 1e-6f); fractions.push_back(fraction); angles.push_back(yaw);
    });
    CHECK(fractions.size() == 3 && fractions[0] < fractions[1] && fractions[1] < fractions[2]);
    CHECK(angles.front() > 3.1f && angles.back() < 3.19f && angles[0] < angles[1]);
    CHECK(TrackWidth(12, -3, 10, 10) == 0); // a shrinking trail cannot invert its transverse texture mapping
    CHECK(TrackWidth(3, 2, 5, 10) == 10.5f);
    CHECK(TrackWidth(3, 2, 100, 10) == 13); // bounded integral through expiry
    CHECK(TrackWidth(3, 2, 0, 0) == 0);
    // Translating both the camera and hull must not lift/sink the same foam strip.
    // Stock AIShip near/far heights are .2/.9 m; a 10 km world offset exposed the origin bug.
    struct Position { float x, y, z; };
    const Position origin{0, 0, 0}, eye{30, 40, 0};
    const float close = storm::ship::TrackElevation(eye, origin, .2f, .9f);
    CHECK(std::abs(close - .2035f) < 1e-6f); // unchanged at world origin
    for (const Position shift : {Position{10000, 0, 0}, Position{-8000, 0, 6000}})
    {
        const Position shifted_eye{eye.x + shift.x, eye.y + shift.y, eye.z + shift.z};
        const float translated = storm::ship::TrackElevation(shifted_eye, shift, .2f, .9f);
        std::printf("foam world translation: original=%.6f shifted=%.6f\n", close, translated);
        CHECK(std::abs(translated - close) < 1e-6f);
        // Both production ribbons share this lift: Track1 transverse .4..1.4 profile,
        // Track2 uniform 1.0. Hold WaveXZ fixed to isolate the erroneous camera factor.
        for (float profile : {.4f, 1.4f, 1.0f})
            CHECK(std::abs((2.0f + translated * profile) - (2.0f + close * profile)) < 1e-6f);
    }
    const Position hull{10000, 0, -2000};
    CHECK(storm::ship::TrackElevation(hull, hull, .2f, .9f) == .2f);
    CHECK(std::abs(storm::ship::TrackElevation(Position{13000, 4000, -2000}, hull, .2f, .9f) - .55f) < 1e-6f);
    CHECK(std::abs(storm::ship::TrackElevation(Position{22000, 0, -2000}, hull, .2f, .9f) - .9f) < 1e-6f);
    CHECK(storm::ship::TrackElevation(eye, origin, .2f, .2f) == .2f);
    // The trail's foam breaks into patches: always within 0..1, a fresh strip keeps most of it, an old one has
    // holes (patches a few metres long: strips are 1 m apart and a strip keeps its pattern for life) and the two
    // edges differ.
    {
        float fresh_min = 1, old_min = 1, max_step = 0, edge_diff = 0;
        for (int strip = 0; strip < 400; ++strip)
            for (int x = 0; x < 12; ++x)
            {
                const float across = x / 11.0f, along = strip * .05f;
                const float fresh = storm::ship::TrackFoamKeep(across, along, 0, 1.3f);
                const float aged = storm::ship::TrackFoamKeep(across, along, 1, 1.3f);
                CHECK(fresh >= 0 && fresh <= 1 && aged >= 0 && aged <= 1);
                fresh_min = std::min(fresh_min, fresh);
                old_min = std::min(old_min, aged);
                max_step = std::max(max_step, std::abs(aged - storm::ship::TrackFoamKeep(across, along + .05f, 1, 1.3f)));
                if (x == 0)
                    edge_diff = std::max(edge_diff, std::abs(aged - storm::ship::TrackFoamKeep(1, along, 1, 1.3f)));
            }
        CHECK(fresh_min >= .54f && old_min < .05f && max_step < .8f && edge_diff > .5f);
        std::printf("foam breakup: fresh min %.2f, old min %.2f, step between strips %.2f, edges differ %.2f\n",
                    fresh_min, old_min, max_step, edge_diff);
    }
    std::printf("foam turn/stop/reset/width checks: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
