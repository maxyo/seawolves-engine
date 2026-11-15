#include "effects/ao_volume.hpp"

namespace storm::effects
{

bool AoVolumes::set(const fx::AoVolume &v)
{
    reset();
    bound_.texture = bgfx::createTexture3D(static_cast<uint16_t>(v.size[0]), static_cast<uint16_t>(v.size[1]),
                                           static_cast<uint16_t>(v.size[2]), false, bgfx::TextureFormat::RGBA8,
                                           BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
                                           bgfx::copy(v.visibility.data(), static_cast<uint32_t>(v.visibility.size())));
    if (!bgfx::isValid(bound_.texture))
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const float extent = v.cell * static_cast<float>(v.size[i]);
        bound_.world_to_uvw[i * 5] = 1.0f / extent;
        bound_.world_to_uvw[12 + i] = -v.origin[i] / extent;
    }
    bound_.world_to_uvw[15] = 1.0f;
    bound_.cell = v.cell;
    return true;
}

void AoVolumes::reset()
{
    if (bgfx::isValid(bound_.texture))
        bgfx::destroy(bound_.texture);
    bound_ = {};
}

} // namespace storm::effects
