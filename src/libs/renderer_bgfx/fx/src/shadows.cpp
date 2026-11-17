#include "storm/fx/shadows.hpp"

#include "gpu_util.hpp"
#include "storm/fx/log.hpp"

#include <bx/math.h>

#include <algorithm>

namespace storm::fx
{
using namespace detail;

namespace
{
// The place's own shade on a ground receiver counts when its caster stands this far (metres along the light) over
// the surface: the still geometry lies in layers a few centimetres apart (ground covers, planks, paths).
constexpr float kGroundShadeClearance = 0.3f;
} // namespace

Shadows::~Shadows()
{
    Shutdown();
}

void Shadows::Shutdown()
{
    Destroy(static_target_);
    Destroy(dynamic_target_);
    Destroy(map_);
    Destroy(caster_);
    Destroy(s_map_);
    Destroy(u_matrix_);
    Destroy(u_params_);
    map_size_ = 0;
    static_valid_ = false;
    dynamic_dirty_ = true;
}

void Shadows::EnsureResources(int map_size)
{
    if (!bgfx::isValid(s_map_))
    {
        s_map_ = bgfx::createUniform("s_fxShadowMap", bgfx::UniformType::Sampler);
        u_matrix_ = bgfx::createUniform("u_fxShadowMtx", bgfx::UniformType::Mat4);
        u_params_ = bgfx::createUniform("u_fxShadow", bgfx::UniformType::Vec4, 4);
    }
    if (map_size_ == map_size && bgfx::isValid(map_))
        return;
    Destroy(static_target_);
    Destroy(dynamic_target_);
    Destroy(map_);
    // Linear filtering on a compare sampler is the hardware 2x2 percentage-closer filter.
    map_ = bgfx::createTexture2D(static_cast<uint16_t>(map_size), static_cast<uint16_t>(map_size), false, 2, bgfx::TextureFormat::D16,
                                 BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | kClampLinear);
    // A framebuffer per layer: drawing one neither loads nor stores the other.
    // (No resolve flags: Attachment::init's default asks for mips, which a depth attachment cannot have.)
    bgfx::Attachment layer;
    layer.init(map_, bgfx::Access::Write, 0, 1, 0, BGFX_ATTACHMENT_NONE);
    static_target_ = bgfx::createFrameBuffer(1, &layer, false);
    layer.init(map_, bgfx::Access::Write, 1, 1, 0, BGFX_ATTACHMENT_NONE);
    dynamic_target_ = bgfx::createFrameBuffer(1, &layer, false);
    if (!bgfx::isValid(static_target_) || !bgfx::isValid(dynamic_target_))
        Log(LogLevel::Error, "Shadows: the sun map's layers could not be made render targets (D16 array, %d x %d): no sun shadows", map_size, map_size);
    map_size_ = map_size;
    static_valid_ = false;
    dynamic_dirty_ = true;
}

bgfx::ProgramHandle Shadows::CasterProgram()
{
    if (!bgfx::isValid(caster_))
        caster_ = LoadProgram("vs_fx_shadow_caster", "fs_fx_shadow_caster");
    return caster_;
}

bool Shadows::Begin(const Settings &settings, const SunFrame &frame)
{
    const int size = SunShadowBudgetFor(settings.shadows).size;
    active_ = false;
    static_wanted_ = false;
    dynamic_casters_ = 0;
    params_ = {}; // strength 0: receivers are fully lit
    if (size == 0)
        return false;
    if (!SupportsShadowMap())
    {
        STORM_FX_ERROR_ONCE(reported_bind_, "Shadows::Begin(): no D16 array depth render target on this device (Support::shadow_map)");
        return false;
    }
    EnsureResources(size);
    if (frame.strength <= 0.0f || frame.layers.size != size || !bgfx::isValid(static_target_) || !bgfx::isValid(dynamic_target_))
        return false;
    active_ = true;
    static_wanted_ = frame.redraw_static || !static_valid_;

    const SunLayers &layers = frame.layers;
    const bgfx::Caps *caps = bgfx::getCaps();
    const std::array<float, 16> view = layers.View(frame.origin);
    // The model's projections map depth to 0..1; GL clip space wants -1..1.
    const auto clip = [caps](std::array<float, 16> projection) {
        if (caps->homogeneousDepth)
        {
            projection[10] *= 2.0f;
            projection[14] = projection[14] * 2.0f - 1.0f;
        }
        return projection;
    };
    const uint16_t side = static_cast<uint16_t>(size);
    // Default view mode: bgfx groups the depth-only draws by program.
    if (static_wanted_)
    {
        bgfx::setViewName(frame.static_view, "fx sun map, still geometry");
        bgfx::setViewMode(frame.static_view, bgfx::ViewMode::Default);
        bgfx::setViewFrameBuffer(frame.static_view, static_target_);
        bgfx::setViewRect(frame.static_view, 0, 0, side, side);
        bgfx::setViewClear(frame.static_view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
        bgfx::setViewTransform(frame.static_view, view.data(), clip(layers.StaticProjection()).data());
        bgfx::touch(frame.static_view);
        static_valid_ = true;
    }
    dynamic_view_ = frame.dynamic_view;
    bgfx::setViewName(dynamic_view_, "fx sun map, moving casters");
    bgfx::setViewMode(dynamic_view_, bgfx::ViewMode::Default);
    bgfx::setViewFrameBuffer(dynamic_view_, dynamic_target_);
    bgfx::setViewRect(dynamic_view_, 0, 0, side, side);
    bgfx::setViewClear(dynamic_view_, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx::setViewTransform(dynamic_view_, view.data(), clip(layers.DynamicProjection()).data());

    // Receivers: callers' world -> light clip -> uv and 0..1 depth; Bind() puts the camera's view space in front.
    float world_to_clip[16];
    bx::mtxMul(world_to_clip, view.data(), layers.StaticProjection().data());
    const float flip = caps->originBottomLeft ? 0.5f : -0.5f;
    const float bias[16] = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, flip, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f};
    bx::mtxMul(world_to_shadow_.data(), world_to_clip, bias);
    camera_view_ = {}; // recomputed by the next Bind()
    const float depth_range = layers.far_plane - layers.near_plane;
    const std::array<float, 3> k = layers.DynamicFromStatic(); // top-left origin
    const float dynamic_v = caps->originBottomLeft ? 1.0f - k[0] - k[2] : k[2];
    params_[0] = {1.0f / static_cast<float>(size), frame.strength, 1.5f * layers.static_texel, 1.5f * layers.static_texel / depth_range};
    params_[1] = {k[0], k[1], dynamic_v, 1.5f * layers.dynamic_texel / depth_range};
    ground_clearance_ = std::max(kGroundShadeClearance, 1.5f * layers.static_texel) / depth_range;
    params_[2] = {1.5f * layers.dynamic_texel, 0.0f, 0.0f, -4.0f * depth_range}; // facing: 1 from 14° to the sun on
    return true;
}

void Shadows::End()
{
    if (!active_)
        return;
    active_ = false;
    if (dynamic_casters_ > 0)
        dynamic_dirty_ = true; // its draws run the view, with its clear
    else if (dynamic_dirty_)
    {
        bgfx::touch(dynamic_view_); // once: back to "open"
        dynamic_dirty_ = false;
    }
}

bool Shadows::BindUnshadowed(const Settings &settings)
{
    const int size = SunShadowBudgetFor(settings.shadows).size;
    if (size == 0 || !SupportsShadowMap())
    {
        STORM_FX_ERROR_ONCE(reported_bind_, "Shadows::BindUnshadowed(): no shadow map for these settings");
        return false;
    }
    EnsureResources(size);
    static const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const std::array<std::array<float, 4>, 4> unshadowed{};
    bgfx::setUniform(u_matrix_, identity.data());
    bgfx::setUniform(u_params_, unshadowed.data(), 4);
    bgfx::setTexture(kShadowMapStage, s_map_, map_);
    return true;
}

void Shadows::Bind(const std::array<float, 16> &camera_view, const SunReceiver &receiver)
{
    if (!bgfx::isValid(map_))
    {
        STORM_FX_ERROR_ONCE(reported_bind_, "Shadows::Bind() before Shadows::Begin(): no shadow map exists");
        return;
    }
    if (camera_view != camera_view_)
    {
        float inverse_view[16];
        bx::mtxInverse(inverse_view, camera_view.data());
        bx::mtxMul(view_to_shadow_.data(), inverse_view, world_to_shadow_.data());
        camera_view_ = camera_view;
    }
    std::array<std::array<float, 4>, 4> params = params_;
    params[2][1] = static_casters_ ? 1.0f : 0.0f;
    params[2][2] = receiver.dynamic_reaches ? 1.0f : 0.0f;
    // A ground receiver is not in the dynamic layer: its tap needs only the depth format's own step (D16, x4).
    params[3] = {ground_clearance_, receiver.shade, 4.0f / 65535.0f, receiver.ground ? 1.0f : 0.0f};
    bgfx::setUniform(u_matrix_, view_to_shadow_.data());
    bgfx::setUniform(u_params_, params.data(), 4);
    bgfx::setTexture(kShadowMapStage, s_map_, map_);
}

} // namespace storm::fx
