#include "storm/fx/sea_waves.hpp"

#include "gpu_util.hpp"
#include "storm/fx/log.hpp"

#include <algorithm>
#include <cmath>

namespace storm::fx
{
using namespace detail;

namespace
{
// The water loses half its waves' energy in about 3 s.
constexpr float kVelocityKeep = 0.996f; // per 1/60 s: half the energy in about 3 s
} // namespace

SeaWaves::~SeaWaves()
{
    Shutdown();
}

bool SeaWaves::Prepare(int &budget)
{
    for (auto [program, fragment] : {std::pair{&step_, "fs_fx_waves_step"}, std::pair{&derive_, "fs_fx_waves_surface"}})
    {
        if (bgfx::isValid(*program))
            continue;
        if (budget <= 0)
            return false;
        *program = LoadProgram("vs_fx_screen", fragment);
        --budget;
    }
    return true;
}

void SeaWaves::Create(bool float_state, bool float_surface)
{
    Destroy(state_[0]);
    Destroy(state_[1]);
    Destroy(surface_);
    const bgfx::TextureFormat::Enum format = float_state ? bgfx::TextureFormat::RG32F : bgfx::TextureFormat::RGBA8;
    for (auto &state : state_)
        state = bgfx::createFrameBuffer(kSize, kSize, format, BGFX_TEXTURE_RT | kClampPoint);
    surface_ = bgfx::createFrameBuffer(kSize, kSize, float_surface ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::RGBA8,
                                       BGFX_TEXTURE_RT | kClampLinear);
    Log(LogLevel::Info, "sea waves: state %s, surface %s",
        float_state ? "RG32F" : "packed RGBA8", float_surface ? "RGBA16F" : "RGBA8");
    float_state_ = float_state;
    float_surface_ = float_surface;
    reset_ = true;
    surface_ready_ = false;
}

int SeaWaves::StepsDue(float dt)
{
    bool dropped = false;
    const int steps = clock_.Due(dt, step_seconds_, dropped);
    source_discontinuous_ |= dropped;
    if (dropped && !dropped_logged_)
    {
        Log(LogLevel::Info, "sea waves: a frame held %.2f s of game time, more than the simulation steps at once "
                            "(%d x %.2f s): the water slows down in such frames (logged once)",
            static_cast<double>(dt), WaveClock::kMaxSteps, static_cast<double>(WaveClock::kMaxStep));
        dropped_logged_ = true;
    }
    return steps;
}

void SeaWaves::Reset()
{
    reset_ = true;
    surface_ready_ = false;
    clock_.pending = 0.0f;
    source_history_.Reset();
}

void SeaWaves::Advance(std::span<const bgfx::ViewId> views, std::array<double, 2> camera,
                       std::span<const WaveSource> sources, bool float_state, bool float_surface, float retention)
{
    if (views.size() < 2)
        return;
    if (!bgfx::isValid(state_[0]) || float_state_ != float_state || float_surface_ != float_surface)
        Create(float_state, float_surface);
    if (!bgfx::isValid(step_))
        step_ = LoadProgram("vs_fx_screen", "fs_fx_waves_step");
    if (!bgfx::isValid(derive_))
        derive_ = LoadProgram("vs_fx_screen", "fs_fx_waves_surface");
    if (!bgfx::isValid(s_state_))
    {
        s_state_ = bgfx::createUniform("s_wavesState", bgfx::UniformType::Sampler);
        u_step_ = bgfx::createUniform("u_wavesStep", bgfx::UniformType::Vec4, 4);
        u_sources_ = bgfx::createUniform("u_wavesSrc", bgfx::UniformType::Vec4, kMaxSources * kSourceVectors);
    }

    // The centre in whole texels; the state moves with it by whole texels (exact under point sampling).
    const std::array<int64_t, 2> cell{static_cast<int64_t>(std::floor(camera[0] / kCell + 0.5)),
                                      static_cast<int64_t>(std::floor(camera[1] / kCell + 0.5))};
    std::array<float, 2> shift{0.0f, 0.0f};
    if (!reset_)
    {
        const int64_t dx = cell[0] - cell_[0], dz = cell[1] - cell_[1];
        if (std::abs(dx) >= kSize / 2 || std::abs(dz) >= kSize / 2)
            reset_ = true; // a jump: nothing of the old water is near
        else
            shift = {static_cast<float>(dx) / kSize, static_cast<float>(dz) / kSize};
    }
    cell_ = cell;
    centre_ = {static_cast<double>(cell[0]) * kCell, static_cast<double>(cell[1]) * kCell};

    const size_t count = std::min<size_t>(sources.size(), kMaxSources);
    if (reset_ || source_discontinuous_) source_history_.Reset();
    source_discontinuous_ = false;

    // Unequal steps: (u+ - u) / h - (u - u-) / h_last = (h + h_last) / 2 * c^2 lap / dx^2.
    const float courant = kWaveSpeed * step_seconds_ / kCell;
    const float keep = std::pow(kVelocityKeep, step_seconds_ / kStep);
    const size_t steps = views.size() - 1;
    const float step_retention = std::pow(std::clamp(retention, 0.0f, 1.0f), 1.0f / static_cast<float>(steps));
    for (size_t i = 0; i < steps; ++i)
    {
        const float fraction = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
        std::array<float, kMaxSources * kSourceVectors * 4> packed{};
        for (size_t source_index = 0; source_index < count; ++source_index)
        {
            const WaveSource s = source_history_.Sample(sources.first(count), source_index,
                static_cast<float>(steps) * step_seconds_, fraction);
            float *a = packed.data() + source_index * kSourceVectors * 4;
            a[0] = static_cast<float>(s.position[0] - centre_[0]);
            a[1] = static_cast<float>(s.position[1] - centre_[1]);
            a[2] = s.forward[0];
            a[3] = s.forward[1];
            a[4] = std::max(s.half_length, kCell);
            a[5] = std::max(s.half_width, kCell);
            a[6] = s.speed;
            a[7] = 0.0f;
            a[8] = s.kind == WaveSource::Kind::Hull ? 1.0f : 0.0f;
            a[9] = s.stroke;
            // A phase of its own from its position, so that two swimmers side by side do not stroke in step.
            a[10] = static_cast<float>(std::fmod(std::abs(s.position[0] * 1.7 + s.position[1] * 2.3), 6.2831853));
            a[11] = 0.0f;
            a[12] = s.stern;
            a[13] = s.stem;
            for (int j = 0; j < WaveSource::kHullStations; ++j)
            {
                a[16 + j] = s.station_half[j];
                a[24 + j] = s.station_side[j];
            }
        }
        const float ratio = reset_ ? 1.0f : step_seconds_ / last_step_;
        time_ += step_seconds_;
        const int next = 1 - current_;
        SetupView(views[i], "fx sea waves step", state_[next], kSize, kSize);
        // The first step of the frame moves with the centre; the others are already centred.
        const float step[16] = {
            i == 0 ? shift[0] : 0.0f, i == 0 ? shift[1] : 0.0f, courant * courant * 0.5f * (1.0f + 1.0f / ratio),
            keep * ratio, 1.0f / kSize, kCell, step_seconds_, static_cast<float>(count),
            static_cast<float>(std::fmod(time_, 1000.0)), reset_ ? 1.0f : 0.0f, float_state_ ? 0.0f : 1.0f, kExtent,
            WaveClock::ForcingStep(step_seconds_, reset_ ? step_seconds_ : last_step_), step_retention,
            keep * step_seconds_ / kStep, kStep / step_seconds_,
        };
        bgfx::setUniform(u_step_, step, 4);
        bgfx::setUniform(u_sources_, packed.data(), kMaxSources * kSourceVectors);
        bgfx::setTexture(0, s_state_, bgfx::getTexture(state_[current_]), kClampPoint);
        SubmitScreenTriangle(views[i], step_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
        current_ = next;
        reset_ = false;
        last_step_ = step_seconds_;
    }

    source_history_.Remember(sources.first(count));

    const bgfx::ViewId view = views[steps];
    SetupView(view, "fx sea waves surface", surface_, kSize, kSize);
    const float surface[16] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f / kSize, kCell, 0.0f, 0.0f,
        0.0f, 0.0f, float_state_ ? 0.0f : 1.0f, kExtent,
        0.0f, float_surface_ ? 0.0f : 1.0f, 0.0f, 0.0f,
    };
    bgfx::setUniform(u_step_, surface, 4);
    bgfx::setTexture(0, s_state_, bgfx::getTexture(state_[current_]), kClampPoint);
    SubmitScreenTriangle(view, derive_, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    surface_ready_ = true;
}

bgfx::TextureHandle SeaWaves::Surface() const
{
    if (!bgfx::isValid(surface_))
        return BGFX_INVALID_HANDLE;
    return bgfx::getTexture(surface_);
}

void SeaWaves::Shutdown()
{
    Destroy(state_[0]);
    Destroy(state_[1]);
    Destroy(surface_);
    Destroy(step_);
    Destroy(derive_);
    Destroy(s_state_);
    Destroy(u_step_);
    Destroy(u_sources_);
    reset_ = true;
    surface_ready_ = false;
}

} // namespace storm::fx
