#include "track_sampling.hpp"
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
    std::printf("foam turn/stop/reset/width checks: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
