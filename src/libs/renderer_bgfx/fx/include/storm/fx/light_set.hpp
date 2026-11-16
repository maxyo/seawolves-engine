// Point and spot lights for the per-pixel lighting path: the lights the
// fixed-function pipeline receives through SetLight / LightEnable (location
// torches and lanterns, ship lanterns), plus short-lived flash lights for shots,
// explosions and lightning. Selection of the few lights that matter
// for one draw call happens here, on the CPU.
#pragma once

#include "storm/fx/light_falloff.hpp"
#include "storm/fx/aggregate_light.hpp"
#include "storm/fx/sky_model.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace storm::fx
{

inline constexpr int kMaxPointLights = 8; // per draw call; the fixed-function limit of the engine

// One D3DLIGHT9 of type POINT or SPOT, world space. `color` is D3DLIGHT9.Diffuse with whatever flicker or
// fade the engine applied this frame; its range and attenuation are the light's shape (light_falloff.hpp:
// ShapeOfGameLight).
struct PointLight
{
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float range = 10.0f;    // LightShape: no light beyond it
    float intensity = 1.0f; // LightShape
    float core = 0.4f;      // LightShape
    // Spot lights only (D3DLIGHT_SPOT); a point light keeps cos_outer = -1.
    Vec3 direction{0.0f, -1.0f, 0.0f}; // unit, the way the light points
    float cos_inner = 1.0f;            // cos(Theta / 2)
    float cos_outer = -1.0f;           // cos(Phi / 2)
    float falloff = 1.0f;              // D3DLIGHT9.Falloff
    // True when the light is already in the baked vertex colours of static location geometry (the lighter
    // tool's output). Such lights must not be added again to geometry drawn with those colours.
    bool baked = false;
    AggregateRef aggregate;
    float aggregate_scale = 1.0f; // energy scale, uniform over every member
    uint8_t device_slot = 0xff; // enabled device slot, 0xff for independent baked-frame lights
    Vec3 aggregate_origin{}; // absolute world = point-light world + this origin
};

// The light's shape and its gamma-space falloff at `distance` (LightFalloffGammaAt; fxLightFalloff of fx_lighting.sh).
LightShape ShapeOf(const PointLight &light);
float LightAttenuation(const PointLight &light, float distance);

struct LightSelection
{
    std::array<PointLight, kMaxPointLights> lights{};
    std::array<uint32_t, kMaxPointLights> indices{};
    int count = 0;
    int dropped = 0; // lights that reach the object but did not fit into the budget (the weakest ones)
};

// The `max_count` lights that contribute most to an object with the given bounding sphere, strongest first.
// Lights that cannot reach the sphere are dropped. `include_baked = false` is for geometry whose vertex
// colours already contain the baked lights: only dynamic lights (flashes, carried lanterns) are returned.
// `fade_rank`: when lights are left out, the weakest kept one is dimmed as the strongest left-out one catches up
// (full at kRankFade of its score ahead, off when equal), so a light entering or leaving the budget never pops.
inline constexpr float kRankFade = 0.5f;
LightSelection SelectLights(std::span<const PointLight> lights, Vec3 center, float radius, int max_count, bool include_baked = true,
                            bool fade_rank = false);

// Short-lived lights of shots and explosions (no shadow, no lamp slot): the game reports each one where it happens
// (render::EffectsFlash), the renderer ages them once per presented frame and hands the ones it keeps to the per-pixel
// point-light path of lit and baked draws. A storm's lightning is not one of them: a bolt is far away and lights the
// whole scene at once, which a local light with a falloff cannot do (the game flashes the screen for it).
enum class FlashKind : uint8_t
{
    Gunshot,       // pistol or musket muzzle (particle system "gunfire")
    Cannon,        // a cannon of a ship or a fort, at its muzzle (AICannon::RealFire)
    BallBlast,     // a ball bursting on a fort, an island or a hull ("blast")
    Explosion,     // a powder keg, a blown cannon, a quest explosion ("blast_inv", "town_explodion")
    ShipExplosion, // a ship's powder room ("ShipExplode")
};
inline constexpr int kFlashKinds = 5;

// What a flash of one kind looks like: peak colour (may exceed 1), the reach of a D3D light of the game with
// attenuation 1 / (1 + 3 d / range + 16 d² / range²) (about 1/20 of the peak at `range`), and its envelope: linear rise
// over `rise` seconds, then (1 - t / decay)² down to nothing.
struct FlashProfile
{
    Vec3 color{1.0f, 1.0f, 1.0f};
    float range = 10.0f;
    float rise = 0.03f;
    float decay = 0.2f;
};
const FlashProfile &FlashProfileOf(FlashKind kind);
// 0..1 at `age` seconds after the flash.
float FlashEnvelope(const FlashProfile &profile, float age);
// The flash's shape in the one falloff of every light (light_falloff.hpp). ShapeOfGameLight ends a game light at
// kGameLightReach; a flash wider than the ranges that fit there is the same light scaled up in space (range and core
// by the ratio, intensity by its square), so a ship's explosion lights as far as its profile says.
LightShape FlashShapeOf(const FlashProfile &profile, const LightFalloff &falloff);

class FlashLights
{
  public:
    // At the event; absolute world. A second flash within kMergeDistance of one added since the last Update is the
    // same event reported twice (a ship explosion is two particle systems, "ShipExplode" and "blast_inv"):
    // the stronger of the two stays.
    void Add(FlashKind kind, Vec3 position);
    // Once per presented frame: ages the flashes (gone after rise + decay) and ranks them by their strength at the
    // camera `eye` (absolute). The `cap` strongest light; one that falls out of them (or comes back in) fades over
    // kCapFade instead of switching, so hitting the cap never flickers. cap = 0: nothing lights.
    void Update(float delta_seconds, Vec3 eye, int cap);
    // The flashes that light this frame (their weight above zero), absolute world, envelope and fade applied, shaped
    // with `falloff` (FlashShapeOf).
    void AppendTo(std::vector<PointLight> &lights, const LightFalloff &falloff) const;
    void Clear() { flashes_.clear(); }
    size_t Count() const { return flashes_.size(); }
    // Flashes with light this frame (at most cap plus those fading out of it).
    size_t Lit() const;

    static constexpr size_t kCapacity = 32;     // the weakest one is replaced when full
    static constexpr float kCapFade = 0.08f;    // seconds for a light entering or leaving the cap
    static constexpr float kMergeDistance = 0.5f;

  private:
    struct Flash
    {
        FlashKind kind;
        Vec3 position;
        float age = 0.0f;
        float weight = 0.0f; // 0..1, the cap's fade
        bool fresh = true;   // added since the last Update: takes its rank at once (its envelope rises anyway) and
                             // is dark until that Update
        float score = 0.0f;
    };
    std::vector<Flash> flashes_;
    Vec3 eye_{0.0f, 0.0f, 0.0f}; // of the last Update: ranks a flash added before the next one
};

// Uniform block u_fxPoint / u_fxPointCount of fx_lighting.sh.
struct PointLightUniforms
{
    std::array<std::array<float, 4>, kMaxPointLights * 4> u_fxPoint{};
    std::array<float, 4> u_fxPointCount{};
    int dropped = 0; // LightSelection::dropped, for Lighting's statistics
};

// Production failure policy: retain independent pixel lights and sun masks while compounds return to raw vertices.
struct PointLightFallback
{
    PointLightUniforms points;
    uint8_t diffuse_off = 0, specular_off = 0;
};
PointLightFallback PackUnmergedPointLights(const LightSelection &selection, const std::array<float,16> &view,
                                          uint8_t old_diffuse, uint8_t old_specular, uint8_t directional_mask);

// `view` is the world -> view matrix (row-vector convention, as D3DTS_VIEW / bx).
PointLightUniforms PackPointLights(const LightSelection &selection, const std::array<float, 16> &view);

} // namespace storm::fx
