#include "storm/fx/wave_clock.hpp"
#include "storm/fx/wave_idle.hpp"
#include <cmath>
#include <cstdio>

namespace storage
{
using std::abs; using std::clamp; using std::floor; using std::sqrt;
float sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }
#include "../shaders/include/fx_waves_storage.sh"
}
using storm::fx::WaveIdle;
using storm::fx::WaveClock;
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (false)
bool Near(float a, float b, float tolerance = 1e-5f) { return std::abs(a-b) < tolerance; }

int main()
{
    CHECK(WaveIdle::Envelope(0) == 1 && WaveIdle::Envelope(8) == 1);
    CHECK(WaveIdle::Envelope(10) == 0.5f && WaveIdle::Envelope(12) == 0);
    // Partition-independent retention, including frames which do not yet schedule a simulation step.
    for (int hz : {30, 60, 240}) for (int scale : {1, 2, 4})
    {
        WaveIdle idle; WaveClock clock;
        float field = 0.03547f, previous = field;
        int simulated = 0;
        while (!idle.Sleeping())
        {
            const float dt = static_cast<float>(scale) / hz;
            idle.Tick(dt, false);
            float step; bool dropped;
            const int steps = clock.Due(dt, step, dropped);
            CHECK(!dropped);
            if (steps)
            {
                const float keep = std::pow(idle.TakeRetention(), 1.0f / steps);
                for (int i = 0; i < steps; ++i) { field *= keep; previous *= keep; ++simulated; }
                CHECK(Near(field / 0.03547f, WaveIdle::Envelope(idle.seconds), 0.0002f));
            }
        }
        CHECK(simulated > 0 && Near(field, 0));
        CHECK(previous == field); // no spurious velocity from fading only height
        idle.Tick(0, true);
        CHECK(!idle.Sleeping() && idle.TakeRetention() == 1); // wake an already reset field
    }
    // Paused game clock does not spend the idle budget or attenuate its field.
    WaveIdle paused; paused.Tick(10, false);
    float field = 0.1f * paused.TakeRetention();
    for (int i = 0; i < 300; ++i) { paused.Tick(0, false); field *= paused.TakeRetention(); }
    CHECK(paused.seconds == 10 && Near(field, 0.05f));
    // Source reentry keeps the state already faded, with no inverse-envelope amplification.
    paused.Tick(1.0f / 60, true);
    CHECK(paused.seconds == 0 && paused.TakeRetention() == 1 && Near(field, 0.05f));
    // A delayed substep must still consume the old attenuation when a source returns.
    WaveIdle delayed; delayed.Tick(10, false); delayed.Tick(0, true);
    CHECK(delayed.TakeRetention() == 0.5f && delayed.TakeRetention() == 1);
    // Zero retention reaches EXACT zero in both packed components, not a half-LSB bias.
    CHECK(storage::fxWavesDecode(storage::fxWavesEncode(0.03547f * 0, 2), 2) == 0);
    CHECK(storage::fxWavesDecodeVelocity(storage::fxWavesEncodeVelocity(0.002f * 0)) == 0);
    // A present but stationary hull never enters this source-removal policy.
    WaveIdle stopped;
    for (int i = 0; i < 1800; ++i) stopped.Tick(1.0f / 60, true);
    CHECK(stopped.seconds == 0 && stopped.TakeRetention() == 1);
    std::printf("wave idle checks: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
