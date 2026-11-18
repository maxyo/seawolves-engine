// Waves from objects on the legacy sea (Settings::sea_waves): a wave-equation height field on the GPU around the
// camera, stirred every step by swimmers and waders, and read by the Sea2 shaders (shaders/include/sea_waves.sh)
// for their normals, crest foam and (Settings::sea_waves_lift) a vertex lift. The hulls' own waves are their wakes
// (sea_wakes.hpp).
//
// The field: kSize x kSize texels over kExtent metres, centred on the camera in whole texels (the content is shifted
// by whole texels as the centre moves, so the water does not swim under the camera), stepped in game time
// (WaveClock: 60 Hz steps, longer ones up to kMaxStep when a frame holds more game time, at most kMaxSteps a frame).
// Each step is one screen pass that reads the previous state and writes the next one:
// the discrete 2D wave equation (Hugo Elias' ripple scheme, the explicit leapfrog integration, its second
// difference in time taken over unequal steps), with velocity damping per second, a sponge at the border and every
// source applied in the same pass. The waves travel at kWaveSpeed, about what deep water gives the shortest waves
// the field holds (1-2 m): a wader's ripples die out within some metres. A body pushes the water up ahead of it and
// draws it down behind, and strokes when it swims; a hull holds the water still inside its waterline (ripples
// reflect off it). Then a surface pass derives, per texel, the height, its gradient and a
// crest-foam amount into the surface texture, which the sea samples with linear filtering in its vertex shaders and
// as a cubic B-spline (four linear taps) in its pixel shaders: the normals are smooth across the 0.5 m texels.
//
// Support::sea_waves_float selects RG32F state; sea_waves_surface_float independently selects RGBA16F surface.
// Each unsupported format falls back independently: RGBA8 stores a signed 16-bit height and a 16-bit
// reference-step displacement (logarithmically encoded for precision near still water), both exactly zeroable.
// Keeping displacement separately avoids persistent velocity from height rounding. An RGBA8 surface stores signed
// square-root height +-kSurfaceHeight (decode before bilinear vertex filtering), and linear slopes +-kSurfaceSlope
// around 128/255, whose 8-bit steps (0.008 of slope) show as faint terraces
// in the reflection.
#pragma once

#include "storm/fx/wave_clock.hpp"
#include "storm/fx/wave_source_history.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <span>

namespace storm::fx
{

class SeaWaves
{
  public:
    static constexpr uint16_t kSize = 256;
    static constexpr float kExtent = 128.0f; // metres the field covers
    static constexpr float kCell = kExtent / kSize;
    static constexpr int kMaxSources = 16;
    static constexpr int kSourceVectors = 8; // vec4 per source in u_wavesSrc (fs_fx_waves_step.sc)
    static constexpr int kMaxSteps = WaveClock::kMaxSteps;
    static constexpr float kStep = WaveClock::kStep;
    static constexpr float kWaveSpeed = 1.2f; // m/s
    // The RGBA8 surface (no Support::sea_waves_surface_float): heights +-kSurfaceHeight metres, slopes +-kSurfaceSlope.
    static constexpr float kSurfaceHeight = 0.5f;
    static constexpr float kSurfaceSlope = 1.0f;

    SeaWaves() = default;
    SeaWaves(const SeaWaves &) = delete;
    SeaWaves &operator=(const SeaWaves &) = delete;
    ~SeaWaves();

    // Programs for the settings switch (counts against `budget` for each it has to load).
    bool Prepare(int &budget);
    // Steps due after `dt` seconds of game time (0..kMaxSteps): the caller provides that many views plus one
    // for the surface pass to Advance (none when 0).
    int StepsDue(float dt);
    // Runs `views.size() - 1` steps, centred on `camera` (absolute world x, z), with this frame's `sources` (the
    // first kMaxSources: the caller sorts them by importance), then the surface pass in the last view.
    // Consecutive poses with the same identity are sampled at each hull substep midpoint.
    // `float_state`: Support::sea_waves_float; `float_surface`: Support::sea_waves_surface_float.
    // `retention`: source-free idle fade applied to both stored components over this frame (normally 1).
    void Advance(std::span<const bgfx::ViewId> views, std::array<double, 2> camera, std::span<const WaveSource> sources,
                 bool float_state, bool float_surface, float retention = 1.0f);
    // Drops the water's state (a new scene, a teleport): the next Advance starts from still water.
    void Reset();
    // The surface of the last Advance (R height, GB gradient in x / z, A crest foam; RGBA16F as they are, or RGBA8
    // as described above when SurfaceFloat() is false), and the absolute world x, z of its centre:
    // uv = (world - centre) / kExtent + 0.5.
    bgfx::TextureHandle Surface() const;
    bool SurfaceFloat() const { return float_surface_; }
    std::array<double, 2> Centre() const { return centre_; }
    bool Ready() const { return surface_ready_; }
    void Shutdown();

  private:
    void Create(bool float_state, bool float_surface);
    bgfx::FrameBufferHandle state_[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
    bgfx::FrameBufferHandle surface_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle step_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle derive_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_state_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_step_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_sources_ = BGFX_INVALID_HANDLE;
    int current_ = 0;           // state_[current_] holds the latest step
    bool float_state_ = false;  // the format state_ was created with
    bool float_surface_ = false; // the independent surface_ format
    bool reset_ = true;         // the next step writes still water
    bool surface_ready_ = false;
    std::array<int64_t, 2> cell_{}; // the centre in whole texels
    std::array<double, 2> centre_{};
    WaveClock clock_;
    WaveSourceHistory source_history_;
    bool source_discontinuous_ = false;
    float step_seconds_ = WaveClock::kStep; // seconds of each step of the next Advance
    float last_step_ = WaveClock::kStep;    // ... of the step that wrote the latest state
    bool dropped_logged_ = false;
    double time_ = 0.0;    // stepped game time (the swimmers' strokes)
};

} // namespace storm::fx
