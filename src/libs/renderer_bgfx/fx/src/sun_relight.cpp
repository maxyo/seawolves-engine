#include "storm/fx/sun_relight.hpp"

#include "gpu_util.hpp"
#include "storm/fx/log.hpp"
#include "storm/fx/shadows.hpp"

#include <bx/math.h>

#include <algorithm>

namespace storm::fx
{
using namespace detail;

namespace
{
constexpr float kSurfaceOffset = 0.05f; // metres along the normal, as fx::SunVisibility's rays start
constexpr uint64_t kStatic = BGFX_TEXTURE_NONE | kClampPoint;

bgfx::TextureHandle StaticRgba(int width, int height, std::span<const uint8_t> rgba)
{
    return bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::RGBA8,
                                 kStatic, bgfx::copy(rgba.data(), static_cast<uint32_t>(rgba.size())));
}
} // namespace

SunRelight::~SunRelight()
{
    Shutdown();
}

bool SunRelight::Supported()
{
    const bgfx::Caps *caps = bgfx::getCaps();
    return SupportsShadowMap() && (caps->formats[bgfx::TextureFormat::RGBA32F] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0 &&
           bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT) &&
           (caps->formats[bgfx::TextureFormat::RGBA8] & BGFX_CAPS_FORMAT_TEXTURE_MIP_AUTOGEN) != 0;
}

void SunRelight::EnsureResources()
{
    if (bgfx::isValid(compose_))
        return;
    compose_ = LoadProgram("vs_fx_screen", "fs_fx_sun_relight");
    s_position_ = bgfx::createUniform("s_fxRelightPosition", bgfx::UniformType::Sampler);
    s_normal_ = bgfx::createUniform("s_fxRelightNormal", bgfx::UniformType::Sampler);
    s_baked_ = bgfx::createUniform("s_fxRelightBaked", bgfx::UniformType::Sampler);
    s_game_ = bgfx::createUniform("s_fxRelightGame", bgfx::UniformType::Sampler);
    s_sun_ = bgfx::createUniform("s_fxRelightSun", bgfx::UniformType::Sampler);
    u_matrix_ = bgfx::createUniform("u_fxRelightMtx", bgfx::UniformType::Mat4);
    u_params_ = bgfx::createUniform("u_fxRelight", bgfx::UniformType::Vec4, 7);
    place_map_ = bgfx::createTexture2D(kPlaceSize, kPlaceSize, false, 1, bgfx::TextureFormat::D16,
                                       BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | kClampLinear);
    place_target_ = bgfx::createFrameBuffer(1, &place_map_, true);
    place_drawn_ = false;
}

SunRelight::Map SunRelight::AddMap(const RelightTexels &texels, int scale)
{
    const size_t count = static_cast<size_t>(texels.width) * static_cast<size_t>(texels.height);
    if (texels.width <= 0 || texels.height <= 0 || texels.position.size() != count * 4 || texels.normal_sky.size() != count * 4 ||
        texels.game.size() != count * 4)
        return kNoMap;
    EnsureResources();
    MapData map;
    map.src_width = texels.width;
    map.src_height = texels.height;
    map.width = texels.width * scale;
    map.height = texels.height * scale;
    const auto w = static_cast<uint16_t>(texels.width), h = static_cast<uint16_t>(texels.height);
    const auto fw = static_cast<uint16_t>(map.width), fh = static_cast<uint16_t>(map.height);
    map.position = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA32F, kStatic,
                                         bgfx::copy(texels.position.data(), static_cast<uint32_t>(texels.position.size_bytes())));
    map.normal_sky = StaticRgba(texels.width, texels.height, texels.normal_sky);
    map.game = StaticRgba(texels.width, texels.height, texels.game);
    // The lightmap itself: the draws sample it with their own sampler state (trilinear, anisotropic), so it has mips;
    // the backend makes them when the pass ends.
    // `scale` times the game's texels per side: the sun's shadow edges are read that much finer than the game's
    // lightmap holds them (the game's paint and tone are interpolated between its texels).
    map.target_texture = bgfx::createTexture2D(fw, fh, true, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    bgfx::Attachment attachment;
    attachment.init(map.target_texture, bgfx::Access::Write, 0, 1, 0, BGFX_ATTACHMENT_AUTO_GEN_MIPS);
    map.target = bgfx::createFrameBuffer(1, &attachment, false);
    // The bake's sun on the texels (BakeVisibility): read point-sampled by the compose.
    map.bake_texture = bgfx::createTexture2D(fw, fh, false, 1, BakeFormat(), BGFX_TEXTURE_RT | kClampPoint);
    map.bake_target = bgfx::createFrameBuffer(1, &map.bake_texture, false);
    if (!bgfx::isValid(map.target) || !bgfx::isValid(map.bake_target))
    {
        Log(LogLevel::Error, "SunRelight: a %d x %d lightmap target could not be made", map.width, map.height);
        Destroy(map.target);
        Destroy(map.bake_target);
        for (bgfx::TextureHandle *t : {&map.position, &map.normal_sky, &map.game, &map.target_texture, &map.bake_texture})
            Destroy(*t);
        return kNoMap;
    }
    maps_.push_back(map);
    return static_cast<Map>(maps_.size() - 1);
}

bgfx::TextureFormat::Enum SunRelight::BakeFormat()
{
    // One channel where the device renders to it; RGBA8 (every device) where not: four times the bytes, said once.
    static const bgfx::TextureFormat::Enum format = [] {
        if (bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R8, BGFX_TEXTURE_RT))
            return bgfx::TextureFormat::R8;
        Log(LogLevel::Info, "SunRelight: no R8 render target on this device: the bake's sun on the texels takes RGBA8");
        return bgfx::TextureFormat::RGBA8;
    }();
    return format;
}

size_t SunRelight::Bytes() const
{
    const size_t bake = BakeFormat() == bgfx::TextureFormat::R8 ? 1 : 4;
    size_t total = 0;
    for (const MapData &m : maps_)
    {
        const size_t fine = static_cast<size_t>(m.width) * m.height, src = static_cast<size_t>(m.src_width) * m.src_height;
        total += fine * 4 * 4 / 3 + fine * bake + src * (16 + 4 + 4);
    }
    return total;
}

bgfx::TextureHandle SunRelight::Texture(Map map) const
{
    return map < maps_.size() && maps_[map].composed ? maps_[map].target_texture : bgfx::TextureHandle BGFX_INVALID_HANDLE;
}

void SunRelight::ClearMaps()
{
    for (MapData &m : maps_)
    {
        Destroy(m.target);
        Destroy(m.target_texture);
        Destroy(m.bake_target);
        Destroy(m.bake_texture);
        Destroy(m.position);
        Destroy(m.normal_sky);
        Destroy(m.game);
    }
    maps_.clear();
}

void SunRelight::BeginPlace(bgfx::ViewId view, const SunPlace &place, Vec3 origin)
{
    EnsureResources();
    place_ = place;
    std::array<float, 16> projection = place.Projection();
    if (bgfx::getCaps()->homogeneousDepth)
    {
        projection[10] *= 2.0f;
        projection[14] = projection[14] * 2.0f - 1.0f;
    }
    bgfx::setViewName(view, "fx sun map, the place");
    bgfx::setViewMode(view, bgfx::ViewMode::Default);
    bgfx::setViewFrameBuffer(view, place_target_);
    bgfx::setViewRect(view, 0, 0, kPlaceSize, kPlaceSize);
    bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx::setViewTransform(view, place.View(origin).data(), projection.data());
    bgfx::touch(view);
    place_drawn_ = true;
}

void SunRelight::BakeVisibility(bgfx::ViewId view, Map map, float footprint)
{
    if (map >= maps_.size() || !bgfx::isValid(compose_) || !place_drawn_)
        return;
    Pass(view, maps_[map], RelightTone{}, true, footprint, false, true);
    maps_[map].bake_ready = true;
}

void SunRelight::InvalidateBakes()
{
    for (MapData &m : maps_)
        m.bake_ready = false;
}

void SunRelight::Compose(bgfx::ViewId view, Map map, const RelightTone &tone, bool sun_up, float footprint, bool debug)
{
    if (map >= maps_.size() || !bgfx::isValid(compose_) || !maps_[map].bake_ready)
        return;
    Pass(view, maps_[map], tone, sun_up && place_drawn_, footprint, debug, false);
    maps_[map].composed = true;
}

void SunRelight::Pass(bgfx::ViewId view, MapData &m, const RelightTone &tone, bool sun, float footprint, bool debug, bool bake)
{
    // World -> the place map's uv and 0..1 depth.
    float world_to_clip[16], matrix[16];
    bx::mtxMul(world_to_clip, place_.View().data(), place_.Projection().data());
    const float flip = bgfx::getCaps()->originBottomLeft ? 0.5f : -0.5f;
    const float bias[16] = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, flip, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f};
    bx::mtxMul(matrix, world_to_clip, bias);
    // The texel stands off its surface by the rays' offset and at least a texel and a half of the place map (its
    // own depth must not shade it); the depth bias is the map's step over a texel at a 15° sun.
    const float texel = std::max(place_.texel[0], place_.texel[1]);
    const float depth = std::max(place_.far_plane - place_.near_plane, 1.0f);
    const std::array<std::array<float, 4>, 7> params = {{
        {tone.a, tone.b, tone.c, debug ? 1.0f : 0.0f},
        {place_.sun[0], place_.sun[1], place_.sun[2], sun ? 1.0f : 0.0f},
        {tone.game_sky[0], tone.game_sky[1], tone.game_sky[2], tone.game_sky[3]},
        {tone.game_sky[4], tone.game_sky[5], tone.game_sky[6], tone.game_sky[7]},
        {footprint, std::max(kSurfaceOffset, 1.5f * texel), 1.5f * texel / depth, 0.0f},
        {1.0f / static_cast<float>(m.width), 1.0f / static_cast<float>(m.height), bake ? 1.0f : 0.0f, 0.0f},
        {static_cast<float>(m.src_width), static_cast<float>(m.src_height), 1.0f / static_cast<float>(m.src_width),
         1.0f / static_cast<float>(m.src_height)},
    }};
    SetupView(view, bake ? "fx town lightmap, the bake's sun" : "fx town lightmap", bake ? m.bake_target : m.target,
              static_cast<uint16_t>(m.width), static_cast<uint16_t>(m.height), true);
    bgfx::setUniform(u_matrix_, matrix);
    bgfx::setUniform(u_params_, params.data(), 7);
    bgfx::setTexture(0, s_position_, m.position);
    bgfx::setTexture(1, s_normal_, m.normal_sky);
    // The pass that writes the bake's visibility does not read it (its own target): the game's paint stands in.
    bgfx::setTexture(2, s_baked_, bake ? m.game : m.bake_texture);
    bgfx::setTexture(4, s_game_, m.game);
    bgfx::setTexture(5, s_sun_, place_map_);
    SubmitScreenTriangle(view, compose_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
}

void SunRelight::Shutdown()
{
    ClearMaps();
    Destroy(place_target_); // owns the map
    place_map_ = BGFX_INVALID_HANDLE;
    Destroy(compose_);
    for (bgfx::UniformHandle *u : {&s_position_, &s_normal_, &s_baked_, &s_game_, &s_sun_, &u_matrix_, &u_params_})
        Destroy(*u);
    place_drawn_ = false;
}

void SunRelight::OnDeviceLost()
{
    maps_.clear(); // the handles went with the device
    place_target_ = BGFX_INVALID_HANDLE;
    place_map_ = BGFX_INVALID_HANDLE;
    compose_ = BGFX_INVALID_HANDLE;
    for (bgfx::UniformHandle *u : {&s_position_, &s_normal_, &s_baked_, &s_game_, &s_sun_, &u_matrix_, &u_params_})
        *u = BGFX_INVALID_HANDLE;
    place_drawn_ = false;
}

} // namespace storm::fx
