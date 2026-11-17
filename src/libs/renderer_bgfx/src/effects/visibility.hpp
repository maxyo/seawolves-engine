// How open to the sky every point of the current location is (fx/visibility.hpp), on the GPU: the visibility volume
// of the place's probes (effects/place_bake.hpp) as an RG8 3D texture, R sky, G open. Every lit and baked
// fixed-function draw binds it at fx::kVisibilityStage (ff_fragment.sh: ffVisibility); a draw before the volume is
// ready, or where there is none, binds a 1x1x1 open volume (sky 1, open 1) and u_fxVisibilityParams.x = 0.
#pragma once

#include "storm/fx/visibility.hpp"

#include <bgfx/bgfx.h>

#include <array>

namespace storm::effects
{

class Visibility
{
  public:
    struct Bound
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        std::array<float, 16> world_to_uvw{}; // row-vector absolute world -> 0..1 texture coordinates
        float offset = 0.0f;                  // metres: the receivers read it this far off their surface
    };
    // Main thread: uploads `volume`. False when the texture cannot be created (nothing is bound then).
    bool set(const fx::VisibilityVolume &volume, float offset);
    // The volume to bind, or null.
    const Bound *bound() const { return bgfx::isValid(bound_.texture) ? &bound_ : nullptr; }
    void reset();

  private:
    Bound bound_{};
};

} // namespace storm::effects
