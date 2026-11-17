// The ambient-occlusion volume of the current interior (fx/ao_volume.hpp, Settings::ao_volume, option fx_ao_volume),
// on the GPU: the occlusion lobes of the place's probes (effects/place_bake.hpp) as an RGBA8 3D texture; the lit and
// baked fixed-function variants sample it. Outdoors, or before the probes are ready, there is none and nothing
// darkens.
#pragma once

#include "storm/fx/ao_volume.hpp"

#include <bgfx/bgfx.h>

#include <array>

namespace storm::effects
{

class AoVolumes
{
  public:
    struct Bound
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        std::array<float, 16> world_to_uvw{}; // row-vector world -> 0..1 texture coordinates
        float cell = 0.0f;                     // metres: the receivers read it one cell off their surface
    };
    // Main thread: uploads `volume`. False when the texture cannot be created.
    bool set(const fx::AoVolume &volume);
    const Bound *bound() const { return bgfx::isValid(bound_.texture) ? &bound_ : nullptr; }
    void reset();

  private:
    Bound bound_{};
};

} // namespace storm::effects
