#include "storm/fx/wave_clock.hpp"
#include "storm/fx/wave_source_history.hpp"
#include <cstdio>

using namespace storm::fx;
int main()
{
    int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failed; } } while (false)
    WaveSource a; a.kind = WaveSource::Kind::Hull; a.identity = 11; a.speed = 5;
    WaveSource b = a; b.identity = 22; b.position[0] = 20;
    WaveSourceHistory history;
    history.Remember(std::array{a, b});
    a.position[1] = b.position[1] = 1;
    std::array reversed{b, a};
    auto sample = history.Sample(reversed, 1, .2f, .5f);
    CHECK(sample.identity == 11 && sample.position[0] == 0 && sample.position[1] == .5);
    CHECK(history.Sample(reversed, 0, .2f, .5f).position[0] == 20);
    std::array duplicate{a, a};
    CHECK(history.Sample(duplicate, 0, .2f, .5f).position[1] == 1);
    history.Remember(duplicate);
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 1);
    history.Remember({});
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 1);
    history.Remember(std::span(&a, 1));
    a.position[0] = 100; // teleport
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[0] == 100);
    history.Remember(std::span(&a, 1));
    a.forward = {0, -1}; a.position[1] = 2;
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 2);
    history.Reset();
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 2);
    a.identity = b.identity = 0; a.forward = {0, 1}; a.position = {0, 0};
    history.Remember(std::array{a, b}); a.position[1] = 1;
    CHECK(history.Sample(std::array{a, b}, 0, .2f, .5f).position[1] == 1);
    // A single anonymous caller is unambiguous, but an id change never borrows its motion.
    history.Remember(std::span(&a, 1)); a.position[1] = 2;
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 1.5);
    a.identity = 33;
    CHECK(history.Sample(std::span(&a, 1), 0, .2f, .5f).position[1] == 2);

    // Integrate a straight 5 m/s path and a fixed 90-degree turn over identical game time.
    // Midpoint source positions remove the frame-end offset at x1/x2/x4 without any extra GPU step.
    auto pose = [](double t) {
        WaveSource source; source.kind = WaveSource::Kind::Hull; source.identity = 1; source.speed = 5;
        const double angle = std::max(0.0, t - 4) * (std::acos(-1.0) / 8);
        source.position = {5 * 8 / std::acos(-1.0) * (1 - std::cos(angle)),
            t <= 4 ? 5*t : 20 + 5 * 8 / std::acos(-1.0) * std::sin(angle)};
        source.forward = {float(std::sin(angle)), float(std::cos(angle))};
        return source;
    };
    for (int scale : {1, 2, 4})
    {
        WaveClock clock; WaveSourceHistory track; auto source = pose(0); track.Remember(std::span(&source, 1));
        double worst = 0, integral = 0;
        for (int frame = 1; frame <= 240 / scale; ++frame)
        {
            const float frame_dt = scale / 30.0f;
            float step = 0; bool dropped = false; const int steps = clock.Due(frame_dt, step, dropped);
            CHECK(!dropped);
            source = pose(frame * scale / 30.0);
            for (int j = 0; j < steps; ++j)
            {
                const float t = (j + .5f) / steps;
                const auto actual = track.Sample(std::span(&source, 1), 0, frame_dt, t);
                const auto exact = pose((frame - 1 + double(t)) * scale / 30.0);
                worst = std::max(worst, std::hypot(actual.position[0] - exact.position[0], actual.position[1] - exact.position[1]));
                if (frame <= 120 / scale) integral += actual.position[1] * step;
            }
            track.Remember(std::span(&source, 1));
        }
        std::printf("source x%d maximum path error %.6f m; straight integral %.6f (expected 40)\n", scale, worst, integral);
        CHECK(worst < .005 && std::abs(integral - 40) < .00001);
    }
    return failed ? 1 : 0;
}
