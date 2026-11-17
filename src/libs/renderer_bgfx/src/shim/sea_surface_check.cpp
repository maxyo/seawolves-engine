#include "storm/render/sea_surface_check.hpp"

#include "device.hpp"
#include "shim.hpp"

#include <bx/math.h>

#include <cstdio>
#include <cstring>

namespace storm::render
{
namespace
{
// The check in flight or last finished (one at a time). Its target and read-back texture live until the next check
// or the end of the backend; `pixels` until the read-back's frame is back (bgfx::read writes it then).
struct Check
{
    uint32_t ticket = 0;
    uint32_t generation = 0; // Context::backend_generation() of the handles
    uint32_t ready_frame = 0;
    uint32_t count = 0;
    bool pending = false;
    bool half = false; // RGBA16F target (no renderable RGBA32F)
    bgfx::TextureHandle target = BGFX_INVALID_HANDLE, staging = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle frame_buffer = BGFX_INVALID_HANDLE;
    std::vector<uint8_t> pixels;
};
Check check;
uint32_t next_ticket = 1;

void release(Check &c)
{
    if (c.generation == shim::Context::instance().backend_generation() && shim::Context::instance().initialised())
    {
        if (bgfx::isValid(c.frame_buffer))
            bgfx::destroy(c.frame_buffer);
        for (const bgfx::TextureHandle texture : {c.target, c.staging})
            if (bgfx::isValid(texture))
                bgfx::destroy(texture);
    }
    c.frame_buffer = BGFX_INVALID_HANDLE;
    c.target = c.staging = BGFX_INVALID_HANDLE;
    c.pending = false;
}

// The read-back of the check in flight has arrived (or never will: the backend went away).
void settle()
{
    shim::Context &context = shim::Context::instance();
    if (check.pending && (check.generation != context.backend_generation() || context.last_frame() >= check.ready_frame))
    {
        const bool lost = check.generation != context.backend_generation();
        release(check);
        if (lost)
            check.ticket = 0;
    }
}
} // namespace

uint32_t StartSeaSurfaceCheck(shim::BaseTexture *field, const float *registers, const float *points, uint32_t count,
                              std::string &why, bool &retry)
{
    shim::Context &context = shim::Context::instance();
    shim::Device *device = shim::Device::current;
    settle();
    retry = true;
    if (check.pending)
    {
        why = "an earlier check is still in flight";
        return 0;
    }
    if (!device || !context.ready())
    {
        why = "no device ready";
        return 0;
    }
    retry = false;
    if (count == 0 || count > kSeaSurfaceCheckMaxPoints)
    {
        why = "a point count out of range";
        return 0;
    }
    if (context.device_info().vertex_texture_units <= 0)
    {
        why = "the device reports no vertex texture units";
        return 0;
    }
    const bgfx::Caps *caps = context.caps();
    if (!caps)
    {
        why = "no device capabilities";
        return 0;
    }
    const auto renderable = [caps](bgfx::TextureFormat::Enum format) {
        return (caps->formats[format] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) != 0;
    };
    const bool full = renderable(bgfx::TextureFormat::RGBA32F);
    if (!full && !renderable(bgfx::TextureFormat::RGBA16F))
    {
        why = "the device renders into neither RGBA32F nor RGBA16F";
        return 0;
    }
    shim::TexStore *store = shim::store_of(field);
    if (!store)
    {
        why = "no height field texture";
        return 0;
    }
    store->sync();
    if (!bgfx::isValid(store->handle))
    {
        why = "the height field has no GPU texture";
        return 0;
    }
    const bgfx::ProgramHandle program =
        device->program(shim::shader_id_by_name("vs_sea_check"), shim::shader_id_by_name("fs_sea_check"));
    if (!bgfx::isValid(program))
    {
        why = "the check's program could not be created";
        return 0;
    }
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    if (bgfx::getAvailTransientVertexBuffer(count * 3, layout) < count * 3)
    {
        retry = true;
        why = "no transient vertex memory this frame";
        return 0;
    }

    release(check);
    check.generation = context.backend_generation();
    check.half = !full;
    check.count = count;
    const bgfx::TextureFormat::Enum format = full ? bgfx::TextureFormat::RGBA32F : bgfx::TextureFormat::RGBA16F;
    constexpr uint64_t kPoint = BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP;
    const auto width = static_cast<uint16_t>(count);
    check.target = bgfx::createTexture2D(width, 1, false, 1, format, BGFX_TEXTURE_RT | kPoint);
    check.staging = bgfx::createTexture2D(width, 1, false, 1, format,
                                          BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | kPoint);
    if (bgfx::isValid(check.target))
        check.frame_buffer = bgfx::createFrameBuffer(1, &check.target, false);
    if (!bgfx::isValid(check.target) || !bgfx::isValid(check.staging) || !bgfx::isValid(check.frame_buffer))
    {
        release(check);
        why = "the check's target or read-back texture could not be created";
        return 0;
    }

    // Point i: a thin triangle over pixel i's centre of the count x 1 target (no other pixel centre inside).
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, count * 3, layout);
    auto *out = reinterpret_cast<float *>(vertices.data);
    for (uint32_t i = 0; i < count; ++i)
    {
        const float centre = (2.0f * static_cast<float>(i) + 1.0f) / static_cast<float>(count) - 1.0f;
        const float half_pixel = 0.9f / static_cast<float>(count);
        const float corners[3][2] = {{centre - half_pixel, -3.0f}, {centre + half_pixel, -3.0f}, {centre, 3.0f}};
        for (const auto &corner : corners)
        {
            *out++ = points[i * 2];
            *out++ = 0.0f;
            *out++ = points[i * 2 + 1];
            *out++ = corner[0];
            *out++ = corner[1];
        }
    }

    // A view of its own beside the game's current one, into the check's target only (ViewSchedule: Target lane, ahead
    // of the scene; it reads nothing the frame draws): the scene's pass is not cut.
    const uint16_t draw = device->side_view("sea surface check", check.target.idx);
    if (draw == UINT16_MAX)
    {
        release(check);
        retry = true;
        why = "no view left this frame";
        return 0;
    }
    bgfx::setViewFrameBuffer(draw, check.frame_buffer);
    bgfx::setViewRect(draw, 0, 0, width, 1);
    bgfx::setViewClear(draw, BGFX_CLEAR_COLOR, 0, 1.0f, 0);
    bgfx::setViewMode(draw, bgfx::ViewMode::Sequential);
    float constants[9 * 4] = {};
    std::memcpy(constants + 4 * 4, registers, 5 * 4 * sizeof(float));
    bgfx::setUniform(device->uniforms[shim::Device::U_VSC].handle, constants, 9);
    bgfx::setVertexBuffer(0, &vertices);
    // As Device::bind_vertex_texture binds the sea's field for its ports.
    bgfx::setTexture(static_cast<uint8_t>(shim::kVertexTextureUnit), context.sampler(shim::kVertexTextureUnit),
                     store->handle, kPoint);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(draw, program);
    device->invalidate_uniforms(); // the check's constants replaced the game's: the next draw sends them again
    // A view that copies the target at its start, after the check's draw (it records the read).
    const bgfx::ViewId copy = device->blit_view(check.target);
    bgfx::TextureRegion from, to;
    from.init(check.target);
    to.init(check.staging);
    bgfx::blit(copy, to, from);
    check.pixels.assign(static_cast<size_t>(count) * (full ? 16 : 8), 0);
    check.ready_frame = bgfx::read(to, check.pixels.data());
    check.pending = true;
    check.ticket = next_ticket++;
    return check.ticket;
}

SeaSurfaceCheckState PollSeaSurfaceCheck(uint32_t ticket, std::vector<SeaSurfaceSample> &samples)
{
    settle();
    if (ticket == 0 || ticket != check.ticket)
        return SeaSurfaceCheckState::Lost;
    if (check.pending)
        return SeaSurfaceCheckState::Pending;
    samples.resize(check.count);
    for (uint32_t i = 0; i < check.count; ++i)
    {
        float value[4];
        if (check.half)
        {
            uint16_t halves[4];
            std::memcpy(halves, check.pixels.data() + i * 8, 8);
            for (int k = 0; k < 4; ++k)
                value[k] = bx::halfToFloat(halves[k]);
        }
        else
            std::memcpy(value, check.pixels.data() + i * 16, 16);
        samples[i] = {value[0], value[1], value[2], value[3]};
    }
    return SeaSurfaceCheckState::Done;
}

std::string SeaSurfaceCheckDevice()
{
    const shim::Context &context = shim::Context::instance();
    const shim::DeviceInfo &info = context.device_info();
    char text[512];
    std::snprintf(text, sizeof(text), "%s, %s / %s / %s, %d vertex texture units", info.vulkan ? "Vulkan" : "OpenGL ES",
                  info.vendor.c_str(), info.renderer.c_str(), info.version.c_str(), info.vertex_texture_units);
    return text;
}

} // namespace storm::render
