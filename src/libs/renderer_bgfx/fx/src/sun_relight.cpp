#include "storm/fx/sun_relight.hpp"

#include "gpu_util.hpp"
#include "storm/fx/log.hpp"
#include "storm/fx/shadows.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>

namespace storm::fx
{
using namespace detail;

namespace
{
constexpr float kSurfaceOffset = 0.05f; // metres along the normal, as fx::SunVisibility's rays start
constexpr uint64_t kStatic = BGFX_TEXTURE_NONE | kClampPoint;

bgfx::TextureHandle Static(int width, int height, bgfx::TextureFormat::Enum format, const std::vector<uint8_t> &texels)
{
    return bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, format, kStatic,
                                 bgfx::copy(texels.data(), static_cast<uint32_t>(texels.size())));
}

bool Renders(bgfx::TextureFormat::Enum format, bool mips)
{
    const uint16_t caps = bgfx::getCaps()->formats[format];
    return bgfx::isTextureValid(0, false, 1, format, BGFX_TEXTURE_RT) &&
           (!mips || (caps & BGFX_CAPS_FORMAT_TEXTURE_MIP_AUTOGEN) != 0);
}

// The unit normal folded onto the octahedron, 0..255 per axis (decoded by fs_fx_sun_relight.sc octDecode).
std::array<uint8_t, 2> OctEncode(float x, float y, float z)
{
    const float l1 = std::max(std::fabs(x) + std::fabs(y) + std::fabs(z), 1e-6f);
    float u = x / l1, v = z / l1;
    if (y < 0.0f)
    {
        const float fu = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        const float fv = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = fu;
        v = fv;
    }
    const auto byte = [](float t) { return static_cast<uint8_t>(std::lround(std::clamp(t * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f)); };
    return {byte(u), byte(v)};
}
} // namespace

SunRelight::~SunRelight()
{
    Shutdown();
}

bool SunRelight::Supported()
{
    const bgfx::Caps *caps = bgfx::getCaps();
    const auto sampled = [caps](bgfx::TextureFormat::Enum f) { return (caps->formats[f] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0; };
    return SupportsShadowMap() && sampled(bgfx::TextureFormat::RGBA8) &&
           (Renders(bgfx::TextureFormat::R8, true) || Renders(bgfx::TextureFormat::RGBA8, true));
}

std::array<int, 2> SunRelight::RatioSize(int width, int height, float texel_metres, int largest)
{
    const float most = static_cast<float>(largest) / static_cast<float>(std::max({width, height, 1}));
    const float scale = std::clamp(texel_metres / kRatioTexelMetres, 1.0f, std::max(most, 1.0f));
    const auto side = [&](int texels) {
        return std::min(static_cast<int>(std::ceil(static_cast<float>(texels) * scale - 1e-3f)), std::max(texels, largest));
    };
    return {side(width), side(height)};
}

void SunRelight::EnsureResources()
{
    if (bgfx::isValid(compose_))
        return;
    compose_ = LoadProgram("vs_fx_screen", "fs_fx_sun_relight");
    s_position_hi_ = bgfx::createUniform("s_fxRelightPositionHi", bgfx::UniformType::Sampler);
    s_position_lo_ = bgfx::createUniform("s_fxRelightPositionLo", bgfx::UniformType::Sampler);
    s_baked_ = bgfx::createUniform("s_fxRelightBaked", bgfx::UniformType::Sampler);
    s_normal_ = bgfx::createUniform("s_fxRelightNormal", bgfx::UniformType::Sampler);
    s_sun_ = bgfx::createUniform("s_fxRelightSun", bgfx::UniformType::Sampler);
    u_matrix_ = bgfx::createUniform("u_fxRelightMtx", bgfx::UniformType::Mat4);
    u_params_ = bgfx::createUniform("u_fxRelight", bgfx::UniformType::Vec4, 10);
    place_map_ = bgfx::createTexture2D(kPlaceSize, kPlaceSize, false, 1, bgfx::TextureFormat::D16,
                                       BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | kClampLinear);
    place_target_ = bgfx::createFrameBuffer(1, &place_map_, true);
    place_drawn_ = false;
}

void SunRelight::DestroyMap(MapData &m)
{
    Destroy(m.target);
    Destroy(m.bake_target);
    for (bgfx::TextureHandle *t : {&m.target_texture, &m.bake_texture})
        Destroy(*t);
}

SunRelight::Map SunRelight::AddMap(const RelightTexels &texels, int ratio_width, int ratio_height)
{
    const size_t count = static_cast<size_t>(texels.width) * static_cast<size_t>(texels.height);
    if (texels.width <= 0 || texels.height <= 0 || ratio_width <= 0 || ratio_height <= 0 || texels.position.size() != count * 4 ||
        texels.normal_sky.size() != count * 4 || texels.game.size() != count * 4)
        return kNoMap;
    EnsureResources();
    MapData map;
    map.src_width = texels.width;
    map.src_height = texels.height;
    map.width = ratio_width;
    map.height = ratio_height;
    // The positions to 16 bits within their bounds (a few millimetres over a town), split into a high and a low byte
    // per axis: two RGBA8 textures with the sky and the coverage in their fourth channels, the normal and the game's
    // luminance in a third.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < count; ++i)
        if (texels.position[i * 4 + 3] > 0.0f)
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], texels.position[i * 4 + k]);
                hi[k] = std::max(hi[k], texels.position[i * 4 + k]);
            }
    if (lo[0] > hi[0])
        lo = hi = Vec3{0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 3; ++k)
    {
        map.origin[k] = lo[k];
        map.extent[k] = std::max(hi[k] - lo[k], 1e-3f);
    }
    std::vector<uint8_t> &position_hi = map.position_hi, &position_lo = map.position_lo, &normal = map.normal;
    position_hi.resize(count * 4);
    position_lo.resize(count * 4);
    normal.resize(count * 4);
    for (size_t i = 0; i < count; ++i)
    {
        const bool covered = texels.position[i * 4 + 3] > 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float t = covered ? (texels.position[i * 4 + k] - map.origin[k]) / map.extent[k] : 0.0f;
            const auto q = static_cast<uint32_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f));
            position_hi[i * 4 + k] = static_cast<uint8_t>(q >> 8);
            position_lo[i * 4 + k] = static_cast<uint8_t>(q & 255);
        }
        position_hi[i * 4 + 3] = texels.normal_sky[i * 4 + 3];
        position_lo[i * 4 + 3] = covered ? 255 : 0;
        const auto unpack = [&](int k) { return texels.normal_sky[i * 4 + k] / 127.5f - 1.0f; };
        const std::array<uint8_t, 2> n = OctEncode(unpack(0), unpack(1), unpack(2));
        normal[i * 4] = n[0];
        normal[i * 4 + 1] = n[1];
        const uint8_t *g = &texels.game[i * 4];
        normal[i * 4 + 2] = static_cast<uint8_t>(std::lround(0.299f * g[0] + 0.587f * g[1] + 0.114f * g[2])); // as the fit's
        normal[i * 4 + 3] = 0;
    }
    // The ratio map: the draws sample it with the lightmap's own sampler state (trilinear, anisotropic), so it has
    // mips; the backend makes them when the pass ends.
    const auto fw = static_cast<uint16_t>(map.width), fh = static_cast<uint16_t>(map.height);
    map.target_texture = bgfx::createTexture2D(fw, fh, true, 1, RatioFormat(), BGFX_TEXTURE_RT);
    bgfx::Attachment attachment;
    attachment.init(map.target_texture, bgfx::Access::Write, 0, 1, 0, BGFX_ATTACHMENT_AUTO_GEN_MIPS);
    map.target = bgfx::createFrameBuffer(1, &attachment, false);
    // The bake's sun on the texels (BakeVisibility): read point-sampled by the compose.
    map.bake_texture = bgfx::createTexture2D(fw, fh, false, 1, BakeFormat(), BGFX_TEXTURE_RT | kClampPoint);
    map.bake_target = bgfx::createFrameBuffer(1, &map.bake_texture, false);
    if (!bgfx::isValid(map.target) || !bgfx::isValid(map.bake_target))
    {
        Log(LogLevel::Error, "SunRelight: a %d x %d ratio map could not be made", map.width, map.height);
        DestroyMap(map);
        return kNoMap;
    }
    maps_.push_back(map);
    return static_cast<Map>(maps_.size() - 1);
}

bgfx::TextureFormat::Enum SunRelight::RatioFormat()
{
    // One channel with mips where the device renders and mips it; RGBA8 (Supported) where not: four times the bytes,
    // said once.
    static const bgfx::TextureFormat::Enum format = [] {
        if (Renders(bgfx::TextureFormat::R8, true))
            return bgfx::TextureFormat::R8;
        Log(LogLevel::Info, "SunRelight: no R8 render target with mips on this device: the light ratio maps take RGBA8");
        return bgfx::TextureFormat::RGBA8;
    }();
    return format;
}

bgfx::TextureFormat::Enum SunRelight::BakeFormat()
{
    // One channel where the device renders to it; RGBA8 (every device) where not: four times the bytes, said once.
    static const bgfx::TextureFormat::Enum format = [] {
        if (Renders(bgfx::TextureFormat::R8, false))
            return bgfx::TextureFormat::R8;
        Log(LogLevel::Info, "SunRelight: no R8 render target on this device: the bake's sun on the texels takes RGBA8");
        return bgfx::TextureFormat::RGBA8;
    }();
    return format;
}

size_t SunRelight::Bytes() const
{
    const size_t ratio = RatioFormat() == bgfx::TextureFormat::R8 ? 1 : 4, bake = BakeFormat() == bgfx::TextureFormat::R8 ? 1 : 4;
    size_t total = 0;
    for (const MapData &m : maps_)
    {
        const size_t target = static_cast<size_t>(m.width) * m.height;
        total += target * ratio * 4 / 3 + target * bake;
    }
    return total;
}

size_t SunRelight::PassBytes() const
{
    size_t most = 0;
    for (const MapData &m : maps_)
        most = std::max(most, m.position_hi.size() + m.position_lo.size() + m.normal.size());
    return most;
}

bgfx::TextureHandle SunRelight::Texture(Map map) const
{
    return map < maps_.size() && maps_[map].composed ? maps_[map].target_texture : bgfx::TextureHandle BGFX_INVALID_HANDLE;
}

void SunRelight::ClearMaps()
{
    for (MapData &m : maps_)
    {
        if (retire_ && m.composed && bgfx::isValid(m.target_texture))
        {
            retire_(m.target_texture); // a draw of this frame may still read it
            m.target_texture = BGFX_INVALID_HANDLE;
        }
        DestroyMap(m);
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

bool SunRelight::BakeVisibility(bgfx::ViewId view, Map map, float footprint)
{
    if (map >= maps_.size() || !bgfx::isValid(compose_) || !place_drawn_)
        return false;
    Pass(view, maps_[map], RelightTone{}, true, footprint, false, true);
    maps_[map].bake_ready = true;
    return true;
}

void SunRelight::InvalidateBakes()
{
    for (MapData &m : maps_)
        m.bake_ready = false;
}

float RelightLift(float degrees_from_bake)
{
    const float t = std::clamp((degrees_from_bake - kLiftStartDegrees) / (kLiftFullDegrees - kLiftStartDegrees), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool SunRelight::Compose(bgfx::ViewId view, Map map, const RelightTone &tone, bool sun_up, float footprint, bool debug)
{
    if (map >= maps_.size() || !bgfx::isValid(compose_) || !maps_[map].bake_ready)
        return false;
    Pass(view, maps_[map], tone, sun_up && place_drawn_, footprint, debug, false);
    maps_[map].composed = true;
    return true;
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
    const std::array<std::array<float, 4>, 10> params = {{
        {tone.a, tone.b, tone.c, debug ? 1.0f : 0.0f},
        {place_.sun[0], place_.sun[1], place_.sun[2], sun ? 1.0f : 0.0f},
        {tone.game_sky[0], tone.game_sky[1], tone.game_sky[2], tone.game_sky[3]},
        {tone.game_sky[4], tone.game_sky[5], tone.game_sky[6], tone.game_sky[7]},
        {footprint, std::max(kSurfaceOffset, 1.5f * texel), 1.5f * texel / depth, 0.0f},
        {1.0f / static_cast<float>(m.width), 1.0f / static_cast<float>(m.height), bake ? 1.0f : 0.0f, 0.0f},
        {static_cast<float>(m.src_width), static_cast<float>(m.src_height), 1.0f / static_cast<float>(m.src_width),
         1.0f / static_cast<float>(m.src_height)},
        {m.origin[0], m.origin[1], m.origin[2], 0.0f},
        {m.extent[0], m.extent[1], m.extent[2], 0.0f},
        {tone.bake_sun[0], tone.bake_sun[1], tone.bake_sun[2], tone.lift},
    }};
    SetupView(view, bake ? "fx town light ratio, the bake's sun" : "fx town light ratio", bake ? m.bake_target : m.target,
              static_cast<uint16_t>(m.width), static_cast<uint16_t>(m.height), true);
    bgfx::setUniform(u_matrix_, matrix);
    bgfx::setUniform(u_params_, params.data(), static_cast<uint16_t>(params.size()));
    // The inputs for this pass only: bgfx destroys them once the frame that reads them is drawn.
    bgfx::TextureHandle position_hi = Static(m.src_width, m.src_height, bgfx::TextureFormat::RGBA8, m.position_hi);
    bgfx::TextureHandle position_lo = Static(m.src_width, m.src_height, bgfx::TextureFormat::RGBA8, m.position_lo);
    bgfx::TextureHandle normal = Static(m.src_width, m.src_height, bgfx::TextureFormat::RGBA8, m.normal);
    bgfx::setTexture(0, s_position_hi_, position_hi);
    bgfx::setTexture(1, s_position_lo_, position_lo);
    // The pass that writes the bake's visibility does not read it (its own target): the normals stand in.
    bgfx::setTexture(2, s_baked_, bake ? normal : m.bake_texture);
    bgfx::setTexture(3, s_normal_, normal);
    bgfx::setTexture(5, s_sun_, place_map_);
    SubmitScreenTriangle(view, compose_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    for (bgfx::TextureHandle *t : {&position_hi, &position_lo, &normal})
        Destroy(*t);
}

void SunRelight::Shutdown()
{
    ClearMaps();
    Destroy(place_target_); // owns the map
    place_map_ = BGFX_INVALID_HANDLE;
    Destroy(compose_);
    for (bgfx::UniformHandle *u : {&s_position_hi_, &s_position_lo_, &s_normal_, &s_baked_, &s_sun_, &u_matrix_, &u_params_})
        Destroy(*u);
    place_drawn_ = false;
}

} // namespace storm::fx
