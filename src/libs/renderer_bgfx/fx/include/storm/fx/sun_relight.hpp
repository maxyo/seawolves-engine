// The hour's sun on the towns' lightmaps (fx/town_light.hpp), on the GPU, as a light ratio: no rays, no CPU compose, no
// upload, and no copy of the game's lightmap.
//   * The place's sun map: one D16 map of the place's still geometry along the sun (fx::SunPlace), drawn by the
//     caller when the sun has moved (the same casters as the sun shadows' static layer).
//   * The bake's sun: once per lightmap, the place map drawn along the direction the game baked with, each texel's
//     visibility read as the compose reads the hour's (BakeVisibility) into a small target of its own. Both suns go
//     through one map and one filter: what the map cannot resolve (a railing, a pole, a fence thinner than its
//     texel) is missed by both, so the game's painted shadow of it stays instead of being lit away, and with the
//     hour's sun at the bake's the ratio is 1 wherever the sky term agrees.
//   * Compose: per lightmap one pass into its own ratio map (one channel with mips): each texel reads the sun on its
//     world position from the place map (four hardware-filtered taps over its footprint) and writes the light's
//     ratio, (a + b sun + c sky) now over the same with the bake's sun then, log-encoded. The lightmapped
//     draws multiply the game's own lightmap by it (ff_fragment.sh ffTownRatio, one more texture fetch): the paint,
//     its painted shadows, crevices and grain, always come from the game's lightmap at its full resolution.
// The ratio is smooth where the paint is not: its map is far coarser than the paint, a texel about 0.2 m (RatioSize;
// the moving edges a little softer than at the place map's own 0.04-0.08 m texel, the
// painted darks the same, under 1 MB for a town instead of 5-9).
// Its encoding (fs_fx_sun_relight.sc encodeRatio writes it, ff_fragment.sh ffTownRatio reads it): a texel e (0..255)
// holds the ratio 3^((e - 127) / 127): 127 is exactly 1 (the game's light kept), 0 a third, 254 three times; steps of
// 0.9 %, filtered in the log (a blend of two ratios is their geometric mean).
// What does not depend on the hour (the texels' positions, normals, sky) is packed once per lightmap, at the resolution
// the lighting is fitted at (a mip of the game's lightmap, location.cpp ReadLightmap: 512 of a town's 2048), into 10
// bytes a texel (positions to 1/65535 of the lightmap's bounds), and kept on the CPU: it is uploaded for each pass (the
// bake's sun once, then a compose per sun step, a few seconds apart) and let go after it, so the GPU keeps only the
// ratio map and the bake's sun between steps (2.5 MB less for a town's 512 lightmap).
#pragma once

#include "storm/fx/shadow_model.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace storm::fx
{

// One lightmap's texels, row by row, `width` x `height`.
struct RelightTexels
{
    int width = 0, height = 0;
    std::span<const float> position;     // 4 per texel: world xyz, w 1 = relit (0: the texel keeps the game's)
    std::span<const uint8_t> normal_sky; // 4: the unit normal * 0.5 + 0.5, the sky's visibility
    std::span<const uint8_t> game;       // 4: the game's lightmap (RGBA8): its luminance tells a texel it painted dark
};

// The ratio keeps the paint's lit level at every hour: the sun term takes the bake's own cosine on each texel, so only
// the shadows move (a texel the hour's sun no longer reaches drops to the game's shade, one it newly reaches rises to
// the game's lit level). The light may rise above the game's only by `lift` (RelightLift: the hour's sun far enough
// from the bake's), and on a texel only as far as the game's paint shows the darkness our bake's shadow explains
// (painted shadows of what we do not model are never lit away).
struct RelightTone
{
    float a = 0.0f, b = 0.0f, c = 0.0f;
    float game_sky[8] = {};
    std::array<float, 3> bake_sun{0.0f, 1.0f, 0.0f}; // towards the sun the game baked with (world, unit)
    float lift = 0.0f;                               // 0..1, RelightLift
};

// Below kLiftStartDegrees between the hour's sun and the bake's nothing may become lighter than the game painted it
// (the fit of the bake's direction is that uncertain: r 0.35-0.4 on the towns); the lift is full from kLiftFullDegrees.
inline constexpr float kLiftStartDegrees = 15.0f;
inline constexpr float kLiftFullDegrees = 25.0f;
float RelightLift(float degrees_from_bake);

class SunRelight
{
  public:
    // Size of the place's sun map (texels per side).
    static constexpr int kPlaceSize = 2048;
    // The world size the ratio map's texels aim at (RatioSize).
    static constexpr float kRatioTexelMetres = 0.2f;

    // `retire`: takes a ratio map ClearMaps lets go of (kept until no draw can read it any more); null: destroyed at once.
    using Retire = void (*)(bgfx::TextureHandle);
    explicit SunRelight(Retire retire = nullptr) : retire_(retire) {}
    ~SunRelight();
    SunRelight(const SunRelight &) = delete;
    SunRelight &operator=(const SunRelight &) = delete;

    // The device can do it: D16 compare target, RGBA8 textures, a one-channel (or RGBA8) target with mips.
    static bool Supported();

    // The ratio map's size for a lightmap of `width` x `height` texels `texel_metres` apart: texels about
    // kRatioTexelMetres apart, at least the lightmap's own, at most `largest` per side (the same scale on both sides).
    static std::array<int, 2> RatioSize(int width, int height, float texel_metres, int largest);

    using Map = uint32_t;
    static constexpr Map kNoMap = UINT32_MAX;
    // A lightmap's static inputs and its targets, the ratio map `ratio_width` x `ratio_height`; kNoMap when a span has
    // the wrong size or a target cannot be made (logged).
    Map AddMap(const RelightTexels &texels, int ratio_width, int ratio_height);
    // GPU bytes the maps keep between passes (their targets), and the inputs one pass uploads at most (the log).
    size_t Bytes() const;
    size_t PassBytes() const;
    // The ratio map (invalid until the map was composed once).
    bgfx::TextureHandle Texture(Map map) const;
    // Every map goes; a composed ratio map is retired (`retire`), the rest destroyed.
    void ClearMaps();

    // The place's sun map for `place`: sets up `view` (cleared). The caller submits the still casters into it with
    // Shadows::kCasterState. `origin`: the world position the casters' coordinates are relative to.
    void BeginPlace(bgfx::ViewId view, const SunPlace &place, Vec3 origin);
    bool PlaceDrawn() const { return place_drawn_; }
    // Reads the bake's sun on `map`'s texels in `view`: the place map must be drawn along the bake's direction.
    // `footprint`: the radius the four taps spread over (metres), as Compose's. False: no place map.
    bool BakeVisibility(bgfx::ViewId view, Map map, float footprint);
    bool BakeReady(Map map) const { return map < maps_.size() && maps_[map].bake_ready; }
    // The place's still geometry changed (a cut-out texture): every bake's sun is read again.
    void InvalidateBakes();
    // Composes `map`'s ratio in `view` for the sun the place map was drawn with (`sun_up` false: night, no sun term),
    // over its bake's sun (BakeReady). `footprint`: as BakeVisibility's; `debug`: the hour's sun visibility instead of
    // the ratio (its draws show it as grey). False: not composed (no bake's sun yet).
    bool Compose(bgfx::ViewId view, Map map, const RelightTone &tone, bool sun_up, float footprint, bool debug);

    void Shutdown();

  private:
    struct MapData
    {
        int width = 0, height = 0;         // the targets'
        int src_width = 0, src_height = 0; // the inputs'
        Vec3 origin{}, extent{};           // the bounds the positions are packed in
        std::vector<uint8_t> position_hi, position_lo, normal; // the packed inputs (RGBA8 each; normal: xy the
                                                               // normal, z the game's luminance), uploaded per pass
        bgfx::TextureHandle target_texture = BGFX_INVALID_HANDLE, bake_texture = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE, bake_target = BGFX_INVALID_HANDLE;
        bool composed = false, bake_ready = false;
    };
    void EnsureResources();
    static bgfx::TextureFormat::Enum RatioFormat();
    static bgfx::TextureFormat::Enum BakeFormat();
    static void DestroyMap(MapData &map);
    void Pass(bgfx::ViewId view, MapData &m, const RelightTone &tone, bool sun, float footprint, bool debug, bool bake);

    Retire retire_ = nullptr;
    std::vector<MapData> maps_;
    bgfx::TextureHandle place_map_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle place_target_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle compose_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_position_hi_ = BGFX_INVALID_HANDLE, s_position_lo_ = BGFX_INVALID_HANDLE, s_normal_ = BGFX_INVALID_HANDLE,
                        s_baked_ = BGFX_INVALID_HANDLE, s_sun_ = BGFX_INVALID_HANDLE, u_matrix_ = BGFX_INVALID_HANDLE,
                        u_params_ = BGFX_INVALID_HANDLE;
    SunPlace place_{};
    bool place_drawn_ = false;
};

} // namespace storm::fx
