// CPU stencil control for the production shader's packed arithmetic. GPU/backend acceptance remains separate.
#include "storm/fx/wave_clock.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace storage
{
using std::abs;
using std::clamp;
using std::exp2;
using std::floor;
using std::log2;
using std::sqrt;
float sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }
#include "../shaders/include/fx_waves_storage.sh"
#include "../shaders/include/fx_waves_surface_storage.sh"
}

namespace
{
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr int size = 128; // 64 m: the 8 s front cannot reach the boundary from the central source.
constexpr float reference = storm::fx::WaveClock::kStep;
enum class Mode { Float, OldPacked, Packed };
float QuantizeHeight(float h)
{
    return storage::fxWavesDecodeHeight(storage::fxWavesEncodeHeight(h, 2.0f), 2.0f);
}
float QuantizeVelocity(float v)
{
    return storage::fxWavesDecodeVelocity(storage::fxWavesEncodeVelocity(v));
}
std::vector<float> Run(Mode mode, int scale, bool changing = false, bool source = true)
{
    std::vector<float> height(size * size, mode == Mode::OldPacked ? 2.0f / 65535.0f : 0.0f);
    std::vector<float> history = height, next(height.size()), next_history(height.size()), rate(height.size());
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
    {
        const float px = (x + 0.5f - size / 2) * 0.5f, py = (y + 0.5f - size / 2) * 0.5f;
        const float e = (px * px + py * py) / (1.5f * 1.5f);
        rate[y * size + x] = source && e <= 4 ? -0.02f * std::exp(-2.5f * e) : 0;
    }
    storm::fx::WaveClock clock;
    float last = reference, elapsed = 0;
    for (int frame = 0; elapsed < 8.0f - 1e-4f; ++frame)
    {
        // A second control switches dt each frame while preserving the same physical end time.
        const int s = changing ? (frame % 3 == 0 ? 4 : frame % 3 == 1 ? 1 : 2) : scale;
        const float frame_time = std::min(s / 30.0f, 8.0f - elapsed);
        float dt = 0; bool dropped = false;
        const int steps = clock.Due(frame_time, dt, dropped);
        CHECK(!dropped);
        elapsed += frame_time;
        for (int step = 0; step < steps; ++step)
        {
            const float keep = std::pow(0.997f, dt / reference);
            const float lap_factor = std::pow(2.5f * dt / 0.5f, 2) * 0.5f * (1 + last / dt);
            for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
            {
                const int i = y * size + x;
                const float h = height[i];
                const float lap = (x ? height[i-1] : 0) + (x+1<size ? height[i+1] : 0) +
                                  (y ? height[i-size] : 0) + (y+1<size ? height[i+size] : 0) - 4*h;
                const float displacement = mode == Mode::Packed ? history[i] * keep * dt / reference :
                                            (h - history[i]) * keep * dt / last;
                const float input = rate[i] * storm::fx::WaveClock::ForcingStep(dt, last);
                const float value = std::clamp(h + displacement + lap_factor * lap + input, -1.9f, 1.9f);
                const float base = h;
                if (mode == Mode::Packed)
                {
                    next[i] = QuantizeHeight(value);
                    next_history[i] = QuantizeVelocity((value - base) * reference / dt);
                }
                else
                {
                    next[i] = mode == Mode::Float ? value :
                        std::floor((value * 0.25f + 0.5f) * 65535.0f + 0.5f) / 65535.0f * 4.0f - 2.0f;
                    next_history[i] = base;
                }
            }
            height.swap(next); history.swap(next_history); last = dt;
        }
    }
    return height;
}
float Error(const std::vector<float> &a, const std::vector<float> &b)
{
    double difference = 0, energy = 0;
    for (size_t i = 0; i < a.size(); ++i) { difference += std::pow(a[i]-b[i], 2); energy += a[i]*a[i]; }
    return static_cast<float>(std::sqrt(difference / energy));
}
}

int main()
{
    const auto surface = [](float h) {
        const float code = std::floor(storage::fxWavesEncodeSurfaceHeight(h) * 255.0f + .5f);
        return storage::fxWavesDecodeSurfaceHeight(code / 255.0f);
    };
    CHECK(surface(0) == 0 && surface(-.5f) == -.5f && surface(.5f) == .5f);
    CHECK(std::abs(surface(.001f) - .001f) < .0002f);
    for (int q = 1; q <= 255; ++q)
    {
        const float channel = q / 255.0f;
        const float value = storage::fxWavesDecodeSurfaceHeight(channel);
        CHECK(std::floor(storage::fxWavesEncodeSurfaceHeight(value) * 255.0f + .5f) == q);
    }
    // Decoding the linear-filtered encoded channel biases geometry; the vertex path decodes each corner first.
    const float correct_midpoint = (surface(0) + surface(.08f)) * .5f;
    const float wrong_midpoint = storage::fxWavesDecodeSurfaceHeight(
        (storage::fxWavesEncodeSurfaceHeight(0) + storage::fxWavesEncodeSurfaceHeight(.08f)) * .5f);
    CHECK(std::abs(correct_midpoint - .04f) < .001f && wrong_midpoint < .021f);
    CHECK(QuantizeHeight(0) == 0 && QuantizeVelocity(0) == 0);
    CHECK(QuantizeHeight(-2) == -2 && QuantizeHeight(2) == 2);
    CHECK(QuantizeVelocity(-4) == -4 && QuantizeVelocity(4) == 4);
    // Every byte pair and both signs, including still water; storage code is shared with the shaders.
    for (int q = 1; q <= 65535; ++q)
    {
        const float code = static_cast<float>(q);
        const float hi = std::floor(code / 256), lo = code - hi * 256;
        const float recovered = std::floor((hi / 255) * 255 + 0.5f) * 256 + std::floor((lo / 255) * 255 + 0.5f);
        CHECK(recovered == code);
        CHECK(storage::fxWavesEncode(storage::fxWavesDecode(code, 2.0f), 2.0f) == code);
        CHECK(storage::fxWavesEncodeVelocity(storage::fxWavesDecodeVelocity(code)) == code);
        CHECK(storage::fxWavesEncodeHeight(storage::fxWavesDecodeHeight(code, 2.0f), 2.0f) == code);
    }
    const auto quiet = Run(Mode::Packed, 4, true, false);
    CHECK(std::all_of(quiet.begin(), quiet.end(), [](float x) { return x == 0; }));
    const auto base = Run(Mode::Packed, 1), original = Run(Mode::OldPacked, 1);
    for (int scale : {1, 2, 4})
    {
        const auto packed = Run(Mode::Packed, scale), full = Run(Mode::Float, scale);
        const float scale_error = Error(base, packed), storage_error = Error(full, packed);
        std::printf("packed x%d scale L2=%.6f versus float L2=%.6f\n", scale, scale_error, storage_error);
        CHECK(scale_error < 0.03f && storage_error < 0.03f);
    }
    const float negative_control = Error(original, Run(Mode::OldPacked, 2));
    std::printf("old absolute-height storage x2 L2=%.6f (negative control)\n", negative_control);
    CHECK(negative_control > 0.30f);
    const float switching = Error(Run(Mode::Float, 1, true), Run(Mode::Packed, 1, true));
    std::printf("changing-step packed versus float L2=%.6f\n", switching);
    CHECK(switching < 0.03f);
    return failures ? 1 : 0;
}
