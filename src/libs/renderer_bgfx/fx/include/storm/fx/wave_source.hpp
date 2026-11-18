// Wave source data and frame interpolation, independent of the GPU.
#pragma once
#include <array>
#include <cstdint>

namespace storm::fx
{
struct WaveSource
{
    enum class Kind : uint8_t
    {
        Body, // a swimmer or wader: a round disturbance that moves and (swimming) strokes
        Hull, // a boat or ship: its waterline (stern, stem, stations) holds the water inside; it lays a wake
    };
    Kind kind = Kind::Body;
    uint64_t identity = 0; // stable entity id; zero is an anonymous source
    std::array<double, 2> position{}; // absolute world x, z (metres)
    std::array<float, 2> forward{0.0f, 1.0f}; // unit heading in x, z
    float half_length = 0.5f; // metres along `forward` (a body: its radius; a hull: half its waterline length)
    float half_width = 0.5f;  // a hull: its largest half beam at the waterline
    // A hull's waterline (render::EffectsWaveSource): kHullStations stations evenly spaced from `stern` to `stem`
    // (metres along `forward` from `position`), each with the waterline's centre offset to the side (towards
    // (-forward z, forward x)) and its half beam there.
    static constexpr int kHullStations = 8;
    float stern = -0.5f, stem = 0.5f;
    std::array<float, kHullStations> station_side{};
    std::array<float, kHullStations> station_half{};
    float speed = 0.0f;  // m/s along `forward`
    float stroke = 0.0f; // a body: 0 wading .. 1 swimming hard (an oscillating push)
};

} // namespace storm::fx
