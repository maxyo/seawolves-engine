#include "effects/visibility.hpp"

namespace storm::effects
{

bool Visibility::set(const fx::VisibilityVolume &v, float offset)
{
    reset();
    bound_.texture = bgfx::createTexture3D(static_cast<uint16_t>(v.size[0]), static_cast<uint16_t>(v.size[1]),
                                           static_cast<uint16_t>(v.size[2]), false, bgfx::TextureFormat::RG8,
                                           BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
                                           bgfx::copy(v.sky_open.data(), static_cast<uint32_t>(v.sky_open.size())));
    if (!bgfx::isValid(bound_.texture))
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const float extent = v.cell * static_cast<float>(v.size[i]);
        bound_.world_to_uvw[i * 5] = 1.0f / extent;
        bound_.world_to_uvw[12 + i] = -v.origin[i] / extent;
    }
    bound_.world_to_uvw[15] = 1.0f;
    bound_.offset = offset;
    return true;
}

void Visibility::reset()
{
    if (bgfx::isValid(bound_.texture))
        bgfx::destroy(bound_.texture);
    bound_ = {};
}

} // namespace storm::effects
