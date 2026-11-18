#include "storm/fx/sea_wakes.hpp"

#include "gpu_util.hpp"
#include "storm/fx/log.hpp"

#include <algorithm>
#include <cmath>

namespace storm::fx
{
using namespace detail;

SeaWakes::~SeaWakes()
{
    Shutdown();
}

bool SeaWakes::Prepare(int &budget)
{
    if (bgfx::isValid(program_))
        return true;
    if (budget <= 0)
        return false;
    program_ = LoadProgram("vs_fx_wake", "fs_fx_wake");
    --budget;
    return true;
}

void SeaWakes::Update(std::span<const WaveSource> sources, double now)
{
    paths_.Update(sources, now);
}

void SeaWakes::Reset()
{
    paths_.Reset();
}

bool SeaWakes::Render(bgfx::ViewId view, std::array<double, 2> camera, double now)
{
    // The hulls whose track can reach the target, nearest first.
    const auto &paths = paths_.Paths();
    std::vector<size_t> order;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        const double dx = paths[i].head.x - camera[0], dz = paths[i].head.z - camera[1];
        if (paths[i].points.size() >= 1 && std::hypot(dx, dz) < 0.75 * kExtent + WakePaths::kMaxLength)
            order.push_back(i);
    }
    if (order.empty())
        return false;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto d = [&](size_t i) { return std::hypot(paths[i].head.x - camera[0], paths[i].head.z - camera[1]); };
        return d(a) < d(b);
    });
    if (order.size() > kMaxHulls)
        order.resize(kMaxHulls);

    if (!bgfx::isValid(target_))
    {
        texture_ = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | kClampLinear);
        target_ = bgfx::createFrameBuffer(1, &texture_, false);
        Log(LogLevel::Info, "sea wakes: %ux%u RGBA8 target over %.0f m", kSize, kSize,
            static_cast<double>(kExtent));
    }
    if (!bgfx::isValid(program_))
        program_ = LoadProgram("vs_fx_wake", "fs_fx_wake");
    if (!bgfx::isValid(u_wake_))
        u_wake_ = bgfx::createUniform("u_wake", bgfx::UniformType::Vec4, 2);

    // Centred on the camera in whole texels: the waves stay on the same texels while it moves.
    centre_ = {std::floor(camera[0] / kTexel + 0.5) * kTexel, std::floor(camera[1] / kTexel + 0.5) * kTexel};
    SetupView(view, "fx sea waves wake", target_, kSize, kSize);
    // Encoded zero slope and height, no weight.
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x80808000);
    bgfx::touch(view);

    static const bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    static_assert(sizeof(WakeVertex) == 9 * sizeof(float));
    const bool flip_y = !bgfx::getCaps()->originBottomLeft;
    bool drawn = false;
    // Farthest first: the nearest hull's waves are blended over the others'.
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const WakePaths::Path &path = paths[*it];
        vertices_.clear();
        const auto count = static_cast<uint32_t>(WakePaths::Ribbon(path, now, centre_, kExtent, flip_y, vertices_));
        if (count < 4 || bgfx::getAvailTransientVertexBuffer(count, layout) < count)
            continue;
        bgfx::TransientVertexBuffer buffer;
        bgfx::allocTransientVertexBuffer(&buffer, count, layout);
        std::copy(vertices_.begin(), vertices_.end(), reinterpret_cast<WakeVertex *>(buffer.data));
        const float wake[8] = {path.length, path.half_beam, kTexel, 0.0f,
                               kSlope, kHeight, 0.0f, 0.0f};
        bgfx::setUniform(u_wake_, wake, 2);
        bgfx::setVertexBuffer(0, &buffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP |
                       BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
        bgfx::submit(view, program_);
        drawn = true;
    }
    return drawn;
}

void SeaWakes::Shutdown()
{
    Destroy(target_);
    Destroy(texture_);
    Destroy(program_);
    Destroy(u_wake_);
    paths_.Reset();
}

} // namespace storm::fx
