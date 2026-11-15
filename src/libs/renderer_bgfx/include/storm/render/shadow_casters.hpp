// Shadow casters as the game's traversal sees them. Game thread only.
//
// What casts a shadow is decided by the lights, not by the camera: the renderer publishes the shadow volumes of the
// frame (the sun's maps, the lamps' faces: src/effects/shadow_casters.hpp) and every draw into the game picture is
// tested against them by the shim on its own, with the bounding sphere of its vertices. The traversal's part is what
// the shim cannot see:
//   * geometry the camera rejects is never drawn. Where the traversal culls by the camera (a model's nodes, a
//     geometry's objects) it asks ShadowCasterReach* for what it rejects, and draws a hit inside a ShadowCasterOnly:
//     the same draw calls with the same pose, materials and bone palette, which reach the shadow volumes only (no
//     colour, no texture demand). Nothing of the scene is updated a second time;
//   * skinned vertices move with their bones: the node's sphere bounds them (ShadowCasterBounds);
//   * what stands still (ShadowCasterStill): a light whose static layer already holds an owner's still geometry
//     leaves its draws out of the dynamic layer, and its static layer takes only them.
// With no shadow volume in the frame (the effects are off, the picture being drawn is not the game's) the queries
// answer false and the scopes cost nothing.
#pragma once

#include <array>
#include <cstdint>

namespace storm::render
{

// The owner of the location's still geometry (render::SetEffectsLocationGeometry). A ship is its own owner: the id
// it reports its lanterns and its still geometry with (render::EffectsShip::id, never 0 or 1).
inline constexpr uint64_t kShadowStillLocation = 1;

// While one lives, the draws are still geometry of `owner`, as reported to the effects (the location's nodes of
// SetEffectsLocationGeometry, a ship's model of SetEffectsShipGeometry); 0: they move (a subtree that is animated
// inside a still model). Nested scopes restore the outer owner.
class ShadowCasterStill
{
  public:
    explicit ShadowCasterStill(uint64_t owner);
    ~ShadowCasterStill();
    ShadowCasterStill(const ShadowCasterStill &) = delete;
    ShadowCasterStill &operator=(const ShadowCasterStill &) = delete;

  private:
    uint64_t previous_;
};

// True when a shadow volume of the frame could take a caster inside this sphere, drawn now (the current render
// target, camera and ShadowCasterStill owner): `center` in the absolute world, or (Local) in the space of the current
// D3DTS_WORLD transform, where `radius` is scaled by it. The first call of a frame publishes the frame's volumes.
bool ShadowCasterReach(const std::array<float, 3> &center, float radius);
bool ShadowCasterReachLocal(const std::array<float, 3> &center, float radius);

// While one lives, the sphere (absolute world) bounds every draw, whatever its pose: the model node being drawn. The
// shim tests a skinned draw with it (its vertices are in the bind pose); a skinned draw outside any such scope has
// no bounds: every volume takes it, and it is counted and logged.
class ShadowCasterBounds
{
  public:
    ShadowCasterBounds(const std::array<float, 3> &center, float radius);
    ~ShadowCasterBounds();
    ShadowCasterBounds(const ShadowCasterBounds &) = delete;
    ShadowCasterBounds &operator=(const ShadowCasterBounds &) = delete;

  private:
    std::array<float, 4> previous_;
};

// While one lives, the draws are submitted to the shadow volumes only: what the camera does not see.
class ShadowCasterOnly
{
  public:
    ShadowCasterOnly();
    ~ShadowCasterOnly();
    ShadowCasterOnly(const ShadowCasterOnly &) = delete;
    ShadowCasterOnly &operator=(const ShadowCasterOnly &) = delete;

  private:
    bool previous_;
};

} // namespace storm::render
