// The lamps' shadow faces as the frame uses them (src/effects/lamp_shadows.hpp), on OpenGL ES and Vulkan in an SDL
// window (an X display, like cutout_caster_check). A 6 x 6 m floor seen from straight above, a lamp 3 m over its
// middle, a 2 x 2 m opaque plate 1 m over the floor drawn as a character (a lit draw: the dynamic faces):
//   * same frame: the plate is drawn after the floor, in one frame only. The floor behind it is in shadow in that
//     very frame: the faces that hold a caster are a frame uniform set while the plate is drawn, after the floor's
//     draw, and bgfx gives every draw of the frame its value at the end of the frame. The next frame without the
//     plate the floor is lit again;
//   * on demand: frames without a caster neither clear nor draw the characters' atlas (render::PerfCounters
//     lamp_atlas_clears, lamp_face_draws); a frame with one clears it once and draws the plate into the faces its
//     sphere reaches (not all six);
//   * reach: a plate inside the lamp's radius but beyond the reach of its light (fx::LampReach of a dim lamp) is
//     drawn into no face;
//   * a light that moves (a carried lantern) takes its whole shape every frame: another attenuation at the same
//     place and range changes the floor's light at once, as much as a light that had it from the start;
//     another range draws its still faces again.
#include "device.hpp"
#include "check_backend.hpp"
#include "effects/effects.hpp"

#include <SDL2/SDL.h>
#include "storm/fx/lamp_shadows.hpp"
#include "storm/fx/light_falloff.hpp"
#include "storm/fx/settings.hpp"
#include "storm/render/effects.hpp"
#include "storm/render/perf_stats.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using namespace storm;
using shim::Device;

namespace
{

constexpr uint32_t kSize = 96;

void check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "FAIL: %s returned 0x%08x\n", what, static_cast<unsigned>(result));
        std::exit(1);
    }
}

struct Vertex
{
    float pos[3];
    float nrm[3];
    uint32_t color;
    float uv[2];
};

class Harness
{
  public:
    explicit Harness(Device &device) : d_(device)
    {
        check(d_.CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback_, nullptr),
              "CreateOffscreenPlainSurface");
        check(d_.GetRenderTarget(0, &back_), "GetRenderTarget");
    }
    ~Harness()
    {
        readback_->Release();
        back_->Release();
    }

    // The floor from 5 m above, orthographic over 6 x 6 m; with `plate`, the plate centred over (x, z), drawn after
    // the floor as a character. `lit_floor`: the floor is a lit draw (a moving light's receiver), else baked.
    std::vector<uint8_t> render(bool plate, float x = 0.0f, float z = 0.0f, bool lit_floor = false)
    {
        check(d_.BeginScene(), "BeginScene");
        check(d_.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0), "Clear");
        D3DMATRIX identity{};
        identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
        D3DMATRIX view{};
        view.m[0][0] = 1.0f;
        view.m[1][2] = -1.0f;
        view.m[2][1] = 1.0f;
        view.m[3][2] = 5.0f;
        view.m[3][3] = 1.0f;
        D3DMATRIX projection{};
        projection.m[0][0] = projection.m[1][1] = 1.0f / 3.0f;
        projection.m[2][2] = 0.1f;
        projection.m[3][3] = 1.0f;
        render::SetEffectsWorldOrigin({0.0f, 0.0f, 0.0f});
        d_.SetTransform(D3DTS_WORLD, &identity);
        d_.SetTransform(D3DTS_VIEW, &view);
        d_.SetTransform(D3DTS_PROJECTION, &projection);
        d_.SetRenderState(D3DRS_ZENABLE, TRUE);
        d_.SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d_.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d_.SetRenderState(D3DRS_AMBIENT, 0xFF404040);
        d_.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d_.SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        D3DMATERIAL9 material{};
        material.Diffuse = material.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
        d_.SetMaterial(&material);
        d_.LightEnable(0, FALSE);
        d_.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        d_.SetTexture(0, nullptr);
        d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d_.SetRenderState(D3DRS_LIGHTING, lit_floor ? TRUE : FALSE);
        std::vector<Vertex> floor;
        const int n = 12;
        for (int zi = 0; zi < n; ++zi)
            for (int xi = 0; xi < n; ++xi)
            {
                const float corners[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
                for (const auto &c : corners)
                    floor.push_back({{-3.0f + 6.0f * (xi + c[0]) / n, 0.0f, -3.0f + 6.0f * (zi + c[1]) / n},
                                     {0.0f, 1.0f, 0.0f},
                                     0xFF909090u,
                                     {0.0f, 0.0f}});
            }
        check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(floor.size() / 3), floor.data(), sizeof(Vertex)),
              "draw floor");
        if (plate)
        {
            d_.SetRenderState(D3DRS_LIGHTING, TRUE);
            render::SetEffectsTechnique("animation");
            const Vertex quad[6] = {
                {{x - 1.0f, 1.0f, z - 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                {{x + 1.0f, 1.0f, z - 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {1.0f, 0.0f}},
                {{x + 1.0f, 1.0f, z + 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {1.0f, 1.0f}},
                {{x - 1.0f, 1.0f, z - 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                {{x + 1.0f, 1.0f, z + 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {1.0f, 1.0f}},
                {{x - 1.0f, 1.0f, z + 1.0f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 1.0f}},
            };
            check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(Vertex)), "draw plate");
            render::SetEffectsTechnique(nullptr);
        }
        check(d_.EndScene(), "EndScene");
        check(d_.GetRenderTargetData(back_, readback_), "GetRenderTargetData");
        D3DLOCKED_RECT locked;
        check(readback_->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect(readback)");
        std::vector<uint8_t> pixels(kSize * kSize * 4);
        for (uint32_t y = 0; y < kSize; ++y)
            std::memcpy(&pixels[y * kSize * 4], static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch, kSize * 4);
        readback_->UnlockRect();
        check(d_.Present(), "Present");
        return pixels;
    }

  private:
    Device &d_;
    storm::shim::Surface *readback_ = nullptr, *back_ = nullptr;
};

// Mean green of a few pixels around the floor point (x, z).
float floor_at(const std::vector<uint8_t> &p, float x, float z)
{
    const int cx = static_cast<int>((x + 3.0f) / 6.0f * kSize), cy = static_cast<int>((3.0f - z) / 6.0f * kSize);
    float sum = 0.0f;
    int count = 0;
    for (int y = cy - 1; y <= cy + 1; ++y)
        for (int xx = cx - 1; xx <= cx + 1; ++xx)
        {
            sum += p[(static_cast<size_t>(y) * kSize + xx) * 4 + 1];
            ++count;
        }
    return sum / static_cast<float>(count);
}

bool ok = true;
void expect(bool pass, const char *what, const char *detail)
{
    std::printf("%s %s: %s\n", pass ? "PASS" : "FAIL", what, detail);
    ok &= pass;
}

// A lamp slot fades in and out over kFadeSeconds of the steady clock, not over frames: how many frames that takes
// depends on the machine's load, so the checks wait for the fade's end, never for a frame count.
bool lamps_fading()
{
    return effects::Host::instance().lighting().lamp_shadows().fading();
}

// Frames until the preset is active, the lamps' still faces are drawn and their fades are over (render(false) each
// frame); `lamps` reports this frame's lights first.
template <typename Lights> void settle(Harness &harness, Lights lamps, bool lit_floor = false)
{
    for (int frame = 0, active = 0;; ++frame) // on the preparation's progress, no frame budget
    {
        lamps();
        harness.render(false, 0.0f, 0.0f, lit_floor);
        active = render::CurrentEffects().pending || lamps_fading() ? 0 : active + 1;
        if (active > 20 && frame > 40)
            break;
        SDL_Delay(5);
    }
}

} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video: %s (an X display is needed)\n", SDL_GetError());
        return 1;
    }
    host_check::UseBackendFromEnvironment();
    SDL_Window *window = SDL_CreateWindow("lamp_shadow_check", 0, 0, kSize, kSize, 0);
    if (!window || !shim::Context::instance().init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no window: %s\n", SDL_GetError());
        return 1;
    }
    {
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = kSize;
        params.BackBufferHeight = kSize;
        params.EnableAutoDepthStencil = TRUE;
        params.AutoDepthStencilFormat = D3DFMT_D24S8;
        auto *device = new Device(window, params);
        {
            Harness harness(*device);
            const std::vector<std::array<float, 3>> positions = {{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f},
                                                                 {-3.0f, 0.0f, 3.0f}};
            const std::vector<std::array<float, 2>> uvs(positions.size(), {0.0f, 0.0f});
            const std::vector<uint32_t> colors(positions.size(), 0xFF909090u);
            const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
            render::SetEffectsLocationGeometry({"lamp_shadow_check", positions, colors, uvs, indices, {}, true,
                                                {-3.0f, -0.2f, -3.0f}, {3.0f, 3.5f, 3.0f}});
            render::ConfigureEffects(2, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 8}, {"fx_shadows", 0}});
            const render::EffectsPointLight lamp{{0.0f, 3.0f, 0.0f}, {4.0f, 3.6f, 3.0f}, 15.0f, 0.5f, 0.0f, 0.1f};
            const auto bright = [&] { render::SetEffectsLamps(std::span(&lamp, 1)); };
            settle(harness, bright);
            char detail[256];

            // Same frame, on demand.
            render::PerfCounters start = render::PerfTotals();
            bright();
            const std::vector<uint8_t> empty = harness.render(false);
            render::PerfCounters d = render::PerfTotals().Since(start);
            std::snprintf(detail, sizeof(detail), "atlas clears %llu, face draws %llu", static_cast<unsigned long long>(d.lamp_atlas_clears),
                          static_cast<unsigned long long>(d.lamp_face_draws));
            expect(d.lamp_atlas_clears == 0 && d.lamp_face_draws == 0, "a frame without casters draws no lamp face", detail);
            start = render::PerfTotals();
            bright();
            const std::vector<uint8_t> shaded = harness.render(true);
            d = render::PerfTotals().Since(start);
            std::snprintf(detail, sizeof(detail), "atlas clears %llu, face draws %llu", static_cast<unsigned long long>(d.lamp_atlas_clears),
                          static_cast<unsigned long long>(d.lamp_face_draws));
            expect(d.lamp_atlas_clears == 1 && d.lamp_face_draws >= 1 && d.lamp_face_draws < 6,
                   "a frame with one caster clears the characters' atlas once and draws only the faces it reaches", detail);
            const float before = floor_at(empty, -1.125f, -1.125f), during = floor_at(shaded, -1.125f, -1.125f),
                        open = floor_at(shaded, 2.4f, 2.4f);
            bright();
            const std::vector<uint8_t> after = harness.render(false);
            const float later = floor_at(after, -1.125f, -1.125f);
            std::snprintf(detail, sizeof(detail), "floor behind the plate %.1f before, %.1f in its frame, %.1f after; open %.1f",
                          before, during, later, open);
            expect(during < 0.85f * before && later > 0.95f * before && during < 0.85f * open,
                   "a caster drawn after its receiver shadows it in the same frame, and only then", detail);

            // Reach: a dim lamp lights its whole radius, but its light counts only near it. Its colour and
            // attenuation are chosen so that the plate 2 m under it (its sphere 1.41 m) is within its reach and the
            // plate 4.06 m from it is not (fx::LampReach with the flicker's headroom, at the preset's
            // Settings::lamp_reach_light for lamp_gain 1).
            render::EffectsPointLight dim{{0.0f, 3.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 15.0f, 0.5f, 0.0f, 0.1f};
            fx::LightShape dim_shape;
            float dim_reach = 0.0f;
            bool found = false;
            for (const float att2 : {0.02f, 0.05f, 0.1f, 0.3f})
                for (const float c : {0.05f, 0.02f, 0.01f, 0.005f, 0.003f, 0.002f})
                {
                    if (found)
                        break;
                    dim_shape = fx::ShapeOfGameLight(dim.range, dim.att0, dim.att1, att2, fx::LightFalloff{});
                    const float h = c * fx::kLampReachHeadroom;
                    dim_reach = fx::LampReach({dim.position, {h, h, h}, dim_shape.range, dim_shape.intensity, dim_shape.core},
                                              fx::Settings::FromPreset(fx::QualityPreset::High).lamp_reach_light);
                    dim.att2 = att2;
                    dim.color = {c, c, c};
                    found = dim_reach > 0.8f && dim_reach < 2.6f && dim_shape.range > 5.5f;
                }
            std::snprintf(detail, sizeof(detail), "reach %.2f m of radius %.2f m", dim_reach, dim_shape.range);
            expect(found, "a dim lamp's light counts only near it, well inside its radius", detail);
            const auto dimmed = [&] { render::SetEffectsLamps(std::span(&dim, 1)); };
            settle(harness, dimmed);
            start = render::PerfTotals();
            dimmed();
            harness.render(true, 2.5f, 2.5f); // its centre 4.06 m from the lamp
            const uint64_t far_draws = render::PerfTotals().Since(start).lamp_face_draws;
            start = render::PerfTotals();
            dimmed();
            harness.render(true, 0.0f, 0.0f); // 2 m under it
            const uint64_t near_draws = render::PerfTotals().Since(start).lamp_face_draws;
            std::snprintf(detail, sizeof(detail), "face draws %llu far, %llu near", static_cast<unsigned long long>(far_draws),
                          static_cast<unsigned long long>(near_draws));
            expect(far_draws == 0 && near_draws > 0, "a caster beyond the reach of a lamp's light casts into none of its faces",
                   detail);

            // A light that moves takes its whole shape every frame.
            render::SetEffectsLamps({});
            render::EffectsPointLight lantern{{0.0f, 3.0f, 0.0f}, {1.5f, 1.4f, 1.2f}, 12.0f, 0.5f, 0.0f, 0.05f};
            const auto carry = [&] { render::SetEffectsDynamicLights(std::span(&lantern, 1)); };
            settle(harness, carry, true);
            carry();
            const float soft = floor_at(harness.render(false, 0.0f, 0.0f, true), 2.4f, 2.4f);
            lantern.att2 = 0.6f; // the same place and range, a steeper falloff: less light at the floor's corner
            for (int i = 0; i < 3; ++i)
            {
                carry();
                harness.render(false, 0.0f, 0.0f, true);
            }
            carry();
            const float steep = floor_at(harness.render(false, 0.0f, 0.0f, true), 2.4f, 2.4f);
            // The same light from the start.
            render::SetEffectsDynamicLights({});
            do // the slot fades out and is freed
            {
                harness.render(false, 0.0f, 0.0f, true);
                SDL_Delay(5);
            } while (lamps_fading());
            settle(harness, carry, true);
            carry();
            const float fresh = floor_at(harness.render(false, 0.0f, 0.0f, true), 2.4f, 2.4f);
            std::snprintf(detail, sizeof(detail), "floor corner %.1f, then %.1f with the steeper falloff, %.1f from the start", soft,
                          steep, fresh);
            expect(steep < soft - 3.0f && std::fabs(steep - fresh) <= 2.0f,
                   "a moving light's intensity and core follow the light while it keeps its slot", detail);
            // Its still faces end at its radius (2 m: shorter than the game's light reaches, so the shape's radius
            // follows it). The lit floor itself casts into the lantern's dynamic faces: one clear every frame, a
            // second one when the still row is drawn again.
            const auto clears = [&] {
                const render::PerfCounters at = render::PerfTotals();
                carry();
                harness.render(false, 0.0f, 0.0f, true);
                return render::PerfTotals().Since(at).lamp_atlas_clears;
            };
            const uint64_t same = clears();
            lantern.range = 2.0f;
            const uint64_t changed = clears(), after_change = clears();
            std::snprintf(detail, sizeof(detail), "row clears %llu, %llu at the new radius, %llu after", static_cast<unsigned long long>(same),
                          static_cast<unsigned long long>(changed), static_cast<unsigned long long>(after_change));
            expect(same == 1 && changed == 2 && after_change == 1, "a moving light's new radius draws its still faces again (once)",
                   detail);

            render::SetEffectsDynamicLights({});
            render::SetEffectsLamps({});
            render::SetEffectsLocationGeometry({});
            render::ConfigureEffects(0, {});
            harness.render(false);
        }
        device->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(ok ? "PASS: lamp faces: same-frame caster faces, atlas on demand, reach, moving light shape\n"
                   : "FAIL: lamp faces\n");
    return ok ? 0 : 1;
}
