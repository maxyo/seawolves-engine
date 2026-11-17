// CPU side of the sun shadows: two layers on one projection.
//   * the static layer holds the place's still geometry. It is anchored in the world and drawn again only when the
//     shadowed region leaves its guard band, the sun has moved by kSunStepDegrees, or it was invalidated;
//   * the dynamic layer holds the moving casters (characters, ships, sails, props) and is drawn every frame that has
//     any. It follows the shadowed region inside the static layer's square.
// Both use the light basis of the direction the static layer was drawn with and the same depth range, so a receiver
// computes one coordinate: the dynamic layer's uv is the static one's scaled and shifted. Both move in whole texels
// of their own grid, so shadow edges do not crawl when the camera moves.
#pragma once

#include "storm/fx/settings.hpp"
#include "storm/fx/sky_model.hpp"

#include <array>
#include <span>

namespace storm::fx
{

// The shadowed region: a sphere of this radius around the focus (metres); shadows fade out over its last tenth.
// On land 30 m (finer texels than 40). At sea, where the focus is the
// player's ship and there is no static layer to keep fine, 45 m: a ship of the line with its bowsprit and the
// water beside it.
inline constexpr float kSunShadowRadius = 30.0f;
inline constexpr float kSunShadowRadiusSea = 45.0f;
// How far the region may move from the static layer's anchor before the layer is drawn again (each light axis).
inline constexpr float kSunStaticBand = 10.0f;
// How far towards the sun casters are still captured (masts, palms).
inline constexpr float kSunCasterReach = 120.0f;
// The static layer is drawn again when the sun is this far from the direction it was drawn with.
inline constexpr float kSunStepDegrees = 0.25f;

// What a quality level spends on the sun shadows (the device classes' budget).
struct SunShadowBudget
{
    int size = 0;         // texels per side of each layer (0: no sun shadows)
    int static_taps = 0;  // hardware-filtered taps of the static layer on a lit receiver: 1 or 4
    int dynamic_taps = 0; // ... of the dynamic layer
};
SunShadowBudget SunShadowBudgetFor(ShadowQuality quality);

// Share of the sun's direct light a fully shadowed receiver loses, for a sun at height `sun_dir_y` (unit vector y).
// All of it: what stays in the shadow is the ambient (and the lamps), so a shadow reads as a shadow, about as dark
// as the baked ground shadows of the towns' lightmaps (shade / sun 0.4-0.6 in Town_Martinique day2). A low sun gives
// endless, shimmering shadows: they fade out between 7° and the horizon.
float SunShadowStrength(float sun_dir_y);

struct SunLayers
{
    // The light basis (unit, world): z points away from the sun, x and y span the maps.
    Vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, 0.0f, 1.0f}, forward{0.0f, -1.0f, 0.0f};
    Vec3 sun{0.0f, 1.0f, 0.0f}; // the direction (towards the sun) the layers are drawn with
    Vec3 anchor{};              // light-space position of the static layer's centre (x, y snapped to its texels)
    std::array<float, 2> dynamic_center{}; // light-space x, y of the dynamic layer's centre (snapped to its texels)
    float static_half = 0.0f;  // half the static layer's side, metres
    float dynamic_half = 0.0f; // ... the dynamic layer's
    float near_plane = 0.0f, far_plane = 0.0f; // light-space z range relative to the anchor, both layers
    float static_texel = 0.0f, dynamic_texel = 0.0f; // world size of one texel
    int size = 0;

    // World -> light space around the anchor (row-vector convention, as bx); `origin`: the world position the
    // caller's coordinates are relative to (a camera-relative device world).
    std::array<float, 16> View(Vec3 origin = {}) const;
    // Light space -> clip (orthographic, depth 0..1 over [near, far]) of each layer.
    std::array<float, 16> StaticProjection() const;
    std::array<float, 16> DynamicProjection() const;
    // Static layer uv (0..1, top-left origin) and depth (0..1) of a world position.
    std::array<float, 3> StaticCoord(Vec3 world) const;
    // The dynamic layer's uv of a static one: uv * [0] + ([1], [2]) (top-left origin for both).
    std::array<float, 3> DynamicFromStatic() const;
};

// Decides, frame by frame, where the layers are and whether the static one must be drawn again.
class SunLayerTracker
{
  public:
    enum class Redraw
    {
        None,
        First,       // nothing was drawn yet (a new place, quality, device)
        Band,        // the shadowed region left the guard band
        Sun,         // the sun moved by kSunStepDegrees
    };
    struct Frame
    {
        SunLayers layers;
        Redraw redraw = Redraw::None;
    };
    // `sun`: towards the sun; `focus`: the shadowed region's centre, absolute world; `size`: texels per side;
    // `radius`: the region's (kSunShadowRadius, kSunShadowRadiusSea).
    Frame Advance(Vec3 sun, Vec3 focus, int size, float radius = kSunShadowRadius);
    // The static layer's content is void (geometry, a cut-out texture, quality or the device changed).
    void Invalidate() { valid_ = false; }
    bool valid() const { return valid_; }

  private:
    bool valid_ = false;
    SunLayers drawn_{};
};

// The sun's map of a whole place, for what the place keeps per texel instead of per frame: the town light's ratio maps
// (effects/town_light.hpp) read the hour's sun on their texels from it. An orthographic view along the sun over the
// box of `ground` (the lightmapped geometry), casters up to `reach` metres towards the sun.
struct SunPlace
{
    Vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, 0.0f, 1.0f}, forward{0.0f, -1.0f, 0.0f}; // the light basis, as SunLayers
    Vec3 sun{0.0f, 1.0f, 0.0f};
    std::array<float, 2> center{}, half{}; // light-space x, y of the map's rectangle
    float near_plane = 0.0f, far_plane = 0.0f; // light-space z
    std::array<float, 2> texel{}; // world size of one texel along x and y
    int size = 0;

    std::array<float, 16> View(Vec3 origin = {}) const;   // world -> light space around (center, near)
    std::array<float, 16> Projection() const;              // -> clip, depth 0..1
    std::array<float, 3> Coord(Vec3 world) const;          // uv (top-left origin) and depth
    // A sphere meets the map's box (its casters).
    bool Reaches(Vec3 center_world, float radius) const;
};
SunPlace ComputeSunPlace(Vec3 sun, std::span<const Vec3> ground, float reach, int size);

} // namespace storm::fx
