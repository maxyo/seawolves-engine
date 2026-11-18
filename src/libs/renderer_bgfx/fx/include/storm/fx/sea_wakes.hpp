// Ship wakes on the legacy sea (part of Settings::sea_waves): the Kelvin waves behind every hull under way, drawn
// along the track it sailed into a top-down target around the camera that the Sea2 shaders sample
// (shaders/include/sea_waves.sh) for their normals, foam and (Settings::sea_waves_lift) a vertex lift.
//
// Ship waves are dispersive (deep water: the longer a wave, the faster it travels), which is what shapes a wake: the
// 19.47-degree V of divergent waves from the bow, transverse waves filling it behind the stern, the whole pattern
// standing still around the hull and fading behind it. The ripple field (sea_waves.hpp) travels at one speed and
// cannot make that pattern, so the wakes are the stationary-phase solution (include/fx_kelvin.sh), evaluated per
// texel on a ribbon along each hull's track (wake_path.hpp): the bow's and, opposite in sign, the stern's wave
// systems, the band limited to what the target holds. The game's own foam (ShipTracks: trailShip along the bow
// wave, trailKeel the stern wash) is drawn over it as before.
//
// The target: kSize x kSize RGBA8 texels over kExtent metres, centred on the camera in whole texels, cleared and
// redrawn every frame (no state); the sea fades it out where a pixel covers more than a texel or two. R, G: slope in
// world x, z (+-kSlope around 128/255), B: height (+-kHeight), A: the blend weight. One draw per hull, blended over
// the others.
#pragma once

#include "storm/fx/wake_path.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <span>
#include <vector>

namespace storm::fx
{

class SeaWakes
{
  public:
    static constexpr uint16_t kSize = 512;
    static constexpr float kExtent = 512.0f; // metres the target covers
    static constexpr float kTexel = kExtent / kSize;
    static constexpr float kSlope = 0.5f;  // RGBA8 range of the slopes
    static constexpr float kHeight = 1.0f; // ... of the height (m)
    static constexpr size_t kMaxHulls = 8; // nearest to the camera

    SeaWakes() = default;
    SeaWakes(const SeaWakes &) = delete;
    SeaWakes &operator=(const SeaWakes &) = delete;
    ~SeaWakes();

    // Programs for the settings switch (counts against `budget` for each it has to load).
    bool Prepare(int &budget);
    // This frame's sources (hulls lay track; others are ignored) at game time `now` (seconds).
    void Update(std::span<const WaveSource> sources, double now);
    // Draws the wakes around `camera` (absolute world x, z) into the target in `view`. False: nothing drawn (no wake
    // in reach), the target is not to be sampled.
    bool Render(bgfx::ViewId view, std::array<double, 2> camera, double now);
    // Drops every track (a new scene, a teleport).
    void Reset();
    bgfx::TextureHandle Target() const { return texture_; }
    // The absolute world x, z of the target's centre: uv = (world - centre) / kExtent + 0.5.
    std::array<double, 2> Centre() const { return centre_; }
    void Shutdown();

  private:
    WakePaths paths_;
    std::vector<WakeVertex> vertices_;
    bgfx::TextureHandle texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_wake_ = BGFX_INVALID_HANDLE;
    std::array<double, 2> centre_{};
};

} // namespace storm::fx
