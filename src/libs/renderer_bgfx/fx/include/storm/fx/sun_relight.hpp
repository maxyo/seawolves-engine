// The hour's sun on our relit town lightmaps (fx/town_light.hpp), on the GPU: no rays, no CPU compose, no upload.
//   * The place's sun map: one D16 map of the place's still geometry along the sun (fx::SunPlace), drawn by the
//     caller when the sun has moved (the same casters as the sun shadows' static layer).
//   * Compose: per lightmap one pass into its own RGBA8 target with mips. Each texel reads the sun on its world
//     position from the place map (four hardware-filtered taps over its footprint) and writes the game's paint
//     relit: (a + b sun + c sky) now over the same with the bake's sun then; the alpha stays the game's.
// What does not depend on the hour (the texels' positions, normals, sky, the bake's sun, the game's paint) is
// uploaded once per lightmap.
#pragma once

#include "storm/fx/shadow_model.hpp"

#include <bgfx/bgfx.h>

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
    std::span<const uint8_t> baked;      // 4: a the bake's sun on the texel (rgb unused)
    std::span<const uint8_t> game;       // 4: the game's lightmap
};

struct RelightTone
{
    float a = 0.0f, b = 0.0f, c = 0.0f;
    float game_sky[8] = {};
};

class SunRelight
{
  public:
    // Size of the place's sun map (texels per side).
    static constexpr int kPlaceSize = 2048;

    SunRelight() = default;
    ~SunRelight();
    SunRelight(const SunRelight &) = delete;
    SunRelight &operator=(const SunRelight &) = delete;

    // The device can do it: D16 compare target, RGBA32F point-sampled texture, RGBA8 target with mips.
    static bool Supported();

    using Map = uint32_t;
    static constexpr Map kNoMap = UINT32_MAX;
    // A lightmap's static inputs and its target; kNoMap when a span has the wrong size.
    Map AddMap(const RelightTexels &texels);
    // The relit lightmap (invalid until the map was composed once).
    bgfx::TextureHandle Texture(Map map) const;
    void ClearMaps();

    // The place's sun map for `place`: sets up `view` (cleared). The caller submits the still casters into it with
    // Shadows::kCasterState. `origin`: the world position the casters' coordinates are relative to.
    void BeginPlace(bgfx::ViewId view, const SunPlace &place, Vec3 origin);
    bool PlaceDrawn() const { return place_drawn_; }
    // Composes `map` in `view` for the sun the place map was drawn with (`sun_up` false: night, no sun term).
    // `footprint`: the radius the four taps spread over (metres); `debug`: the sun's visibility as grey.
    void Compose(bgfx::ViewId view, Map map, const RelightTone &tone, bool sun_up, float footprint, bool debug);

    void Shutdown();
    void OnDeviceLost();

  private:
    struct MapData
    {
        int width = 0, height = 0;
        bgfx::TextureHandle position = BGFX_INVALID_HANDLE, normal_sky = BGFX_INVALID_HANDLE, baked = BGFX_INVALID_HANDLE,
                            game = BGFX_INVALID_HANDLE, target_texture = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
        bool composed = false;
    };
    void EnsureResources();

    std::vector<MapData> maps_;
    bgfx::TextureHandle place_map_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle place_target_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle compose_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_position_ = BGFX_INVALID_HANDLE, s_normal_ = BGFX_INVALID_HANDLE, s_baked_ = BGFX_INVALID_HANDLE,
                        s_game_ = BGFX_INVALID_HANDLE, s_sun_ = BGFX_INVALID_HANDLE,
                        u_matrix_ = BGFX_INVALID_HANDLE, u_params_ = BGFX_INVALID_HANDLE;
    SunPlace place_{};
    bool place_drawn_ = false;
};

} // namespace storm::fx
