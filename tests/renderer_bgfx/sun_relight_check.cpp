// The towns' relit lightmaps take the hour's sun from the place's sun map on the GPU (src/effects/town_light.hpp,
// fx/sun_relight.hpp), on OpenGL ES and Vulkan in an SDL window (an X display, like cutout_caster_check).
// A 40 x 40 m square with three houses and a thin pole; its lightmap (DXT1, 256 x 256) is painted as the game's are: dark where the
// houses shade it from a sun 200° round and 60° up. The place is reported as a town; the hour's sun stands opposite
// (20° round, 60° up). The square is drawn with the game's lightmap at stage 0 and nothing else:
//   * where only the bake's sun was shaded the ground is bright now, where only the hour's sun is shaded it is dark
//     (the shadows moved to the other side of the houses), open ground keeps its light;
//   * the result agrees with the same formula over fx::SunVisibility's rays (the CPU path this replaced);
//   * the place's sun map is drawn once and every lightmap composed once: not again while the sun stands, once more
//     per step of the sun; no sun bake runs on the bake thread; the bake's sun is read once, from the place map drawn
//     along it;
//   * with the hour's sun at the bake's the relit lightmap is the game's: both suns are read through the same map
//     and filter, so what it misses (a pole thinner than its texels) keeps its painted shadow;
//   * a plate drawn lit over open ground (a moving caster) darkens the ground by the sun's share of the lightmap's
//     tone, and adds nothing where the houses shade the ground already.
#include "device.hpp"
#include "check_backend.hpp"

#include "effects/effects.hpp"
#include "storm/fx/place_probes.hpp"
#include "storm/fx/town_light.hpp"
#include "storm/render/effects.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
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

constexpr uint32_t kSize = 256;
constexpr int kMap = 256;       // the lightmap's side
constexpr float kHalf = 20.0f;  // the square's half side
constexpr float kEye = 30.0f;   // the camera's height

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

void add_box(std::vector<std::array<float, 3>> &p, std::vector<uint32_t> &idx, std::array<float, 3> lo, std::array<float, 3> hi)
{
    const auto base = static_cast<uint32_t>(p.size());
    for (int i = 0; i < 8; ++i)
        p.push_back({i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]});
    const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto &f : faces)
        for (const int k : {0, 1, 2, 0, 2, 3})
            idx.push_back(base + static_cast<uint32_t>(f[k]));
}

fx::Vec3 sun_of(float azimuth_degrees, float elevation_degrees)
{
    const float az = azimuth_degrees * 0.017453293f, el = elevation_degrees * 0.017453293f;
    return {std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
}

// Grey DXT1: per 4 x 4 block its brightest and darkest texel as the endpoints, each texel the nearest of the four levels.
std::vector<uint8_t> encode_dxt1(const std::vector<uint8_t> &grey, int size)
{
    std::vector<uint8_t> out(static_cast<size_t>(size / 4) * (size / 4) * 8);
    const auto to565 = [](int g) { return static_cast<uint16_t>(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3)); };
    const auto from565 = [](uint16_t c) { return static_cast<int>(((c >> 5) & 63) * 255 / 63); };
    for (int by = 0; by < size / 4; ++by)
        for (int bx = 0; bx < size / 4; ++bx)
        {
            int lo = 255, hi = 0;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    const int g = grey[static_cast<size_t>(by * 4 + y) * size + bx * 4 + x];
                    lo = std::min(lo, g);
                    hi = std::max(hi, g);
                }
            uint16_t c0 = to565(hi), c1 = to565(lo);
            if (c0 == c1)
                c1 = c0 > 0 ? static_cast<uint16_t>(c0 - 1) : 0, c0 = c0 > 0 ? c0 : 1; // four-colour mode needs c0 > c1
            if (c0 < c1)
                std::swap(c0, c1);
            const int levels[4] = {from565(c0), from565(c1), (2 * from565(c0) + from565(c1)) / 3, (from565(c0) + 2 * from565(c1)) / 3};
            uint32_t bits = 0;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    const int g = grey[static_cast<size_t>(by * 4 + y) * size + bx * 4 + x];
                    int best = 0;
                    for (int k = 1; k < 4; ++k)
                        if (std::abs(levels[k] - g) < std::abs(levels[best] - g))
                            best = k;
                    bits |= static_cast<uint32_t>(best) << (2 * (y * 4 + x));
                }
            uint8_t *block = &out[(static_cast<size_t>(by) * (size / 4) + bx) * 8];
            std::memcpy(block, &c0, 2);
            std::memcpy(block + 2, &c1, 2);
            std::memcpy(block + 4, &bits, 4);
        }
    return out;
}

struct Scene
{
    shim::Texture2D *lightmap = nullptr;
    bool plates = false;
    D3DLIGHT9 sun{};
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

    // The square from kEye above, perspective; returns the picture (BGRA).
    std::vector<uint8_t> render(const Scene &scene)
    {
        check(d_.BeginScene(), "BeginScene");
        check(d_.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0), "Clear");
        D3DMATRIX identity{};
        identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
        D3DMATRIX view{};
        view.m[0][0] = 1.0f;
        view.m[1][2] = -1.0f;
        view.m[2][1] = 1.0f;
        view.m[3][2] = kEye;
        view.m[3][3] = 1.0f;
        const float zn = 1.0f, zf = 100.0f, cot = kEye / kHalf;
        D3DMATRIX projection{};
        projection.m[0][0] = projection.m[1][1] = cot;
        projection.m[2][2] = zf / (zf - zn);
        projection.m[2][3] = 1.0f;
        projection.m[3][2] = -zn * zf / (zf - zn);
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
        d_.SetLight(0, &scene.sun);
        d_.LightEnable(0, TRUE);
        d_.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        // The ground: the lightmap alone, as painted.
        d_.SetRenderState(D3DRS_LIGHTING, FALSE);
        d_.SetTexture(0, scene.lightmap);
        d_.SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d_.SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d_.SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d_.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        const auto vertex = [](float x, float z) {
            return Vertex{{x, 0.0f, z}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {(x + kHalf) / (2.0f * kHalf), (z + kHalf) / (2.0f * kHalf)}};
        };
        const Vertex ground[6] = {vertex(-kHalf, -kHalf), vertex(kHalf, -kHalf), vertex(kHalf, kHalf),
                                  vertex(-kHalf, -kHalf), vertex(kHalf, kHalf),  vertex(-kHalf, kHalf)};
        check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, ground, sizeof(Vertex)), "draw ground");
        if (scene.plates)
        {
            // Plates drawn lit (moving casters).
            d_.SetRenderState(D3DRS_LIGHTING, TRUE);
            d_.SetTexture(0, nullptr);
            d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            d_.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            for (const auto &at : plates)
            {
                const Vertex plate[6] = {
                    {{at[0] - 1.5f, at[1], at[2] - 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                    {{at[0] + 1.5f, at[1], at[2] - 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                    {{at[0] + 1.5f, at[1], at[2] + 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                    {{at[0] - 1.5f, at[1], at[2] - 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                    {{at[0] + 1.5f, at[1], at[2] + 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                    {{at[0] - 1.5f, at[1], at[2] + 1.5f}, {0.0f, 1.0f, 0.0f}, 0xFFFFFFFFu, {0.0f, 0.0f}},
                };
                check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, plate, sizeof(Vertex)), "draw plate");
            }
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

    std::vector<std::array<float, 3>> plates; // centres of 3 x 3 m plates drawn lit

  private:
    Device &d_;
    storm::shim::Surface *readback_ = nullptr, *back_ = nullptr;
};

// Green of the picture at the ground point (x, z) (the picture is read back top row first).
float ground_at(const std::vector<uint8_t> &p, float x, float z)
{
    const int cx = static_cast<int>((x + kHalf) / (2.0f * kHalf) * kSize), cy = static_cast<int>((kHalf - z) / (2.0f * kHalf) * kSize);
    return p[(static_cast<size_t>(std::clamp(cy, 0, static_cast<int>(kSize) - 1)) * kSize + std::clamp(cx, 0, static_cast<int>(kSize) - 1)) * 4 + 1];
}

void dump(const std::vector<uint8_t> &picture, const char *name)
{
    const char *directory = std::getenv("SUN_RELIGHT_CHECK_DUMP");
    if (!directory)
        return;
    char file[512];
    std::snprintf(file, sizeof(file), "%s/%s.ppm", directory, name);
    if (FILE *f = std::fopen(file, "wb"))
    {
        std::fprintf(f, "P6 %u %u 255\n", kSize, kSize);
        for (size_t i = 0; i < picture.size(); i += 4)
            std::fputc(picture[i + 2], f), std::fputc(picture[i + 1], f), std::fputc(picture[i], f);
        std::fclose(f);
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
    SDL_Window *window = SDL_CreateWindow("sun_relight_check", 0, 0, kSize, kSize, 0);
    if (!window || !shim::Context::instance().init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no window: %s\n", SDL_GetError());
        return 1;
    }
    bool ok = true;
    const auto expect = [&ok](bool condition, const char *what) {
        if (!condition)
        {
            std::printf("FAIL: %s\n", what);
            ok = false;
        }
    };
    {
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = kSize;
        params.BackBufferHeight = kSize;
        params.EnableAutoDepthStencil = TRUE;
        params.AutoDepthStencilFormat = D3DFMT_D24S8;
        auto *device = new Device(window, params);
        {
            Harness harness(*device);
            // The lightmapped ground and the place's still geometry: the ground and three houses.
            const std::vector<std::array<float, 3>> ground = {{-kHalf, 0.0f, -kHalf}, {kHalf, 0.0f, -kHalf}, {kHalf, 0.0f, kHalf}, {-kHalf, 0.0f, kHalf}};
            const std::vector<std::array<float, 2>> ground_uv = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            const std::vector<uint32_t> ground_idx = {0, 2, 1, 0, 3, 2};
            const std::vector<uint32_t> ground_map = {0, 0};
            std::vector<std::array<float, 3>> houses;
            std::vector<uint32_t> house_idx;
            add_box(houses, house_idx, {-8.0f, 0.0f, -6.0f}, {-2.0f, 6.0f, 0.0f});
            add_box(houses, house_idx, {4.0f, 0.0f, 3.0f}, {9.0f, 9.0f, 7.0f});
            add_box(houses, house_idx, {-3.0f, 0.0f, 8.0f}, {1.0f, 4.0f, 12.0f});
            // A pole of 1.5 cm (a railing's post, a rope): under a texel of the place's sun map.
            add_box(houses, house_idx, {12.0f, 0.0f, 5.0f}, {12.015f, 6.0f, 5.015f});

            // The game's lightmap, as the CPU would light it: a + b sun + c sky for the bake's sun.
            const fx::Vec3 bake_sun = sun_of(200.0f, 60.0f), hour_sun = sun_of(20.0f, 60.0f);
            const fx::Occluders occluders(houses, house_idx);
            const fx::LightmapTexels texels = fx::RasterizeLightmap(ground, ground_uv, ground_idx, ground_map, 0, kMap, kMap);
            const float texel_metres = 2.0f * kHalf / kMap;
            const std::vector<uint8_t> bake_vis = fx::SunVisibility(occluders, texels, bake_sun, 0.3f, 4, 4);
            const std::vector<uint8_t> hour_vis = fx::SunVisibility(occluders, texels, hour_sun, 0.3f, 4, 4);
            // The sky on the texels, as the town light reads it: from the place's probes (their near sky), laid over
            // the ground with the margins effects/place_bake.cpp gives a town.
            std::vector<uint8_t> sky;
            {
                std::vector<fx::Vec3> place(ground.begin(), ground.end());
                std::vector<uint32_t> place_idx = ground_idx;
                for (const uint32_t i : house_idx)
                    place_idx.push_back(i + 4);
                place.insert(place.end(), houses.begin(), houses.end());
                fx::ProbeParams probes;
                probes.lo = {-kHalf - 8.0f, -2.0f, -kHalf - 8.0f};
                probes.hi = {kHalf + 8.0f, 25.0f, kHalf + 8.0f};
                probes.near_sky = 10.0f;
                probes.threads = 4;
                const fx::Occluders place_rays(place, place_idx);
                const fx::PlaceProbes baked = fx::BakePlaceProbes(place_rays, place, place_idx, probes);
                sky = fx::TexelSky(texels, baked.visibility, baked.near_sky);
            }
            std::vector<uint8_t> grey(static_cast<size_t>(kMap) * kMap, 0);
            for (size_t i = 0; i < texels.texel.size(); ++i)
                grey[texels.texel[i]] = static_cast<uint8_t>(
                    std::lround(255.0f * (0.10f + 0.45f * bake_vis[i] / 255.0f + 0.10f * sky[i] / 255.0f)));
            render::EffectsLightmap lightmap;
            lightmap.width = lightmap.height = kMap;
            lightmap.format = {'D', 'X', 'T', '1'};
            lightmap.data = encode_dxt1(grey, kMap);
            shim::Texture2D *texture = nullptr;
            check(device->CreateTexture(kMap, kMap, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr), "CreateTexture");
            {
                D3DLOCKED_RECT rect;
                check(texture->LockRect(0, &rect, nullptr, 0), "LockRect");
                for (int y = 0; y < kMap; ++y)
                    for (int x = 0; x < kMap; ++x)
                    {
                        const uint32_t g = grey[static_cast<size_t>(y) * kMap + x];
                        const uint32_t texel = 0xFF000000u | g << 16 | g << 8 | g;
                        std::memcpy(static_cast<uint8_t *>(rect.pBits) + y * rect.Pitch + x * 4, &texel, 4);
                    }
                texture->UnlockRect(0);
            }
            render::SetEffectsTextureSource(texture, "lighting\\day1\\sun_relight_check\\shadow.tga", {});
            lightmap.texture = texture;

            std::vector<std::array<float, 3>> still = ground;
            std::vector<uint32_t> still_idx = ground_idx;
            for (const uint32_t i : house_idx)
                still_idx.push_back(i + 4);
            still.insert(still.end(), houses.begin(), houses.end());
            const std::vector<std::array<float, 2>> still_uv(still.size(), {0.0f, 0.0f});
            const std::vector<uint32_t> still_colors(still.size(), 0xFF909090u);
            const std::array<float, 3> room_lo{-kHalf, -0.2f, -kHalf}, room_hi{kHalf, 10.0f, kHalf};

            render::ConfigureEffects(2, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 0}, {"fx_town_walls", 0}});
            render::SetEffectsLocationGeometry({"sun_relight_check", still, still_colors, still_uv, still_idx, {}, false, room_lo, room_hi});
            render::SetEffectsLightmaps("sun_relight_check", ground, ground_uv, ground_idx, ground_map, std::span(&lightmap, 1));

            render::EffectsWeather weather;
            Scene scene;
            scene.lightmap = texture;
            const auto set_sun = [&](const fx::Vec3 &sun) {
                weather.sun_dir = {sun[0], sun[1], sun[2]};
                weather.sky_sun_dir = weather.sun_dir;
                weather.sun_color = {1.0f, 1.0f, 1.0f};
                weather.sun_present = true;
                scene.sun = {};
                scene.sun.Type = D3DLIGHT_DIRECTIONAL;
                scene.sun.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
                scene.sun.Direction = {-sun[0], -sun[1], -sun[2]};
            };
            set_sun(hour_sun);
            const auto frame = [&] {
                render::SetEffectsLamps({});
                render::SetEffectsWeather(weather);
                return harness.render(scene);
            };
            effects::TownLight &town = effects::Host::instance().lighting().town_light();
            // The game's own lightmap first (the place's bake runs on its thread), then ours.
            std::vector<uint8_t> game = frame(), picture;
            // On the bake's progress, no frame or time budget: a bake that never finishes hangs here and run.py's
            // deadlock guard reports the step.
            std::printf("step: the place's lightmap is relit (its bake finished and its tone explains the game's)\n");
            std::fflush(stdout);
            for (int f = 0; !town.active(); ++f)
            {
                picture = frame();
                if (!render::CurrentEffects().pending && f < 3)
                    game = picture;
                SDL_Delay(5);
            }
            for (int f = 0; f < 5; ++f)
                picture = frame();
            dump(game, "game");
            dump(picture, "relit");

            // Shadows moved to the other side of the houses. The first house (x -8..-2, z -6..0, 6 m up): the bake's
            // sun stands south of it and shades the ground north of it, the hour's sun the ground south of it.
            const std::array<float, 2> in_bake{-4.0f, 1.5f}, in_hour{-6.0f, -7.5f};
            const std::array<float, 2> open{-15.0f, 15.0f};
            std::printf("game / relit: in the bake's shadow %.0f / %.0f, in the hour's shadow %.0f / %.0f, open %.0f / %.0f\n",
                        ground_at(game, in_bake[0], in_bake[1]), ground_at(picture, in_bake[0], in_bake[1]),
                        ground_at(game, in_hour[0], in_hour[1]), ground_at(picture, in_hour[0], in_hour[1]),
                        ground_at(game, open[0], open[1]), ground_at(picture, open[0], open[1]));
            expect(ground_at(game, in_bake[0], in_bake[1]) < 0.6f * ground_at(game, open[0], open[1]), "the test's lightmap has the bake's shadow");
            expect(ground_at(picture, in_bake[0], in_bake[1]) > 1.5f * ground_at(game, in_bake[0], in_bake[1]), "the bake's shadow is lit by the hour's sun");
            expect(ground_at(picture, in_hour[0], in_hour[1]) < 0.6f * ground_at(game, in_hour[0], in_hour[1]), "the hour's sun shades where the bake was lit");
            expect(std::fabs(ground_at(picture, open[0], open[1]) - ground_at(game, open[0], open[1])) < 0.08f * ground_at(game, open[0], open[1]),
                   "open ground keeps its light");

            // Against the rays: the texels whose visibility the rays call fully lit or fully shaded agree with the picture.
            {
                size_t compared = 0, agree = 0;
                const float lit_level = ground_at(picture, open[0], open[1]);
                for (size_t i = 0; i < texels.texel.size(); i += 7)
                {
                    const float sun_share = hour_vis[i] / (255.0f * hour_sun[1]);
                    if (sun_share > 0.02f && sun_share < 0.98f)
                        continue; // a shadow's edge: the two filters differ there
                    const int tx = static_cast<int>(texels.texel[i] % kMap), ty = static_cast<int>(texels.texel[i] / kMap);
                    const float x = -kHalf + (tx + 0.5f) * texel_metres, z = -kHalf + (ty + 0.5f) * texel_metres;
                    if (sky[i] < 200 || std::fabs(x) > kHalf - 1.0f || std::fabs(z) > kHalf - 1.0f)
                        continue; // by a wall the sky term differs; the picture's edge
                    ++compared;
                    const bool lit_picture = ground_at(picture, x, z) > 0.75f * lit_level;
                    agree += lit_picture == (sun_share >= 0.98f) ? 1 : 0;
                }
                const double share = compared ? static_cast<double>(agree) / static_cast<double>(compared) : 0.0;
                std::printf("the place's sun map against the rays: %zu texels off the shadows' edges, %.2f %% agree\n", compared, 100.0 * share);
                expect(compared > 3000 && share >= 0.98, "the GPU's sun on the texels agrees with the rays off the shadows' edges (98 %)");
            }

            // Once per sun: the place's map and the compose. Nothing more while the sun stands.
            const auto counters = town.sun_counters();
            std::printf("place map drawn %u time(s), %u compose(s)\n", counters.place_draws, counters.composes);
            expect(counters.place_draws == 1 && counters.composes == 1, "one draw of the place's sun map and one compose for the first sun");
            for (int f = 0; f < 30; ++f)
                frame();
            expect(town.sun_counters().place_draws == 1 && town.sun_counters().composes == 1, "a standing sun draws and composes nothing");
            set_sun(sun_of(20.1f, 60.0f)); // less than a step
            for (int f = 0; f < 5; ++f)
                frame();
            expect(town.sun_counters().place_draws == 1, "a sun inside its step keeps the place's map");
            set_sun(sun_of(22.0f, 60.0f));
            for (int f = 0; f < 5; ++f)
                frame();
            expect(town.sun_counters().place_draws == 2 && town.sun_counters().composes == 2, "a sun step draws the place's map and composes once");
            expect(town.sun_counters().bake_draws == 1, "the bake's sun is read from the place map once");

            // The hour's sun at the bake's: the game's lightmap comes back, painted shadows and all.
            set_sun(bake_sun);
            std::vector<uint8_t> at_bake;
            for (int f = 0; f < 5; ++f)
                at_bake = frame();
            dump(at_bake, "at_bake");
            {
                size_t compared = 0, close = 0;
                double sum = 0.0;
                for (float z = -kHalf + 1.0f; z < kHalf - 1.0f; z += 0.25f)
                    for (float x = -kHalf + 1.0f; x < kHalf - 1.0f; x += 0.25f)
                    {
                        const float d = std::fabs(ground_at(at_bake, x, z) - ground_at(game, x, z));
                        ++compared;
                        sum += d;
                        close += d <= 6.0f ? 1 : 0;
                    }
                std::printf("relit at the bake's sun against the game's: mean difference %.2f, %.2f %% within 6 levels\n",
                            sum / static_cast<double>(compared), 100.0 * static_cast<double>(close) / static_cast<double>(compared));
                expect(sum / static_cast<double>(compared) < 2.0 && close >= compared * 97 / 100,
                       "with the hour's sun at the bake's the relit lightmap is the game's");
            }
            set_sun(hour_sun);
            for (int f = 0; f < 5; ++f)
                picture = frame();

            // A moving caster over open ground takes the sun's share (by the lightmap's tone); over ground the houses
            // shade it adds nothing.
            // One plate 2 m over open ground: its shadow lies 0.4 m west and 1.1 m south of it; (10.6, -13.5) is in it
            // and clear of the plate's own picture. One 6 m up before the tall house's south wall: its shadow falls
            // into the house's shade at (6, -1).
            harness.plates = {{12.0f, 2.0f, -12.0f}, {7.5f, 6.0f, 1.4f}};
            scene.plates = true;
            std::vector<uint8_t> with;
            for (int f = 0; f < 6; ++f)
                with = frame();
            dump(with, "plates");
            const float lit_before = ground_at(picture, 10.6f, -13.5f), lit_after = ground_at(with, 10.6f, -13.5f);
            const float shaded_before = ground_at(picture, 6.0f, -1.0f), shaded_after = ground_at(with, 6.0f, -1.0f);
            std::printf("under a moving plate: open ground %.0f -> %.0f, ground in the houses' shade %.0f -> %.0f\n", lit_before,
                        lit_after, shaded_before, shaded_after);
            expect(lit_after < 0.75f * lit_before && lit_after > 0.2f * lit_before, "a moving caster takes the sun's share of the relit lightmap");
            expect(std::fabs(shaded_after - shaded_before) < 0.06f * std::max(shaded_before, 1.0f), "a moving caster adds nothing in the houses' shade");
            // What the caster leaves is about what the hour's shade leaves: the tone's light without the sun.
            const float hour_shade = ground_at(picture, in_hour[0], in_hour[1]);
            expect(std::fabs(lit_after - hour_shade) < 0.15f * lit_before, "the caster's shadow is as dark as the houses' shade");

            scene.plates = false;
            render::SetEffectsLightmaps({}, {}, {}, {}, {}, {});
            render::SetEffectsLocationGeometry({});
            render::ConfigureEffects(0, {});
            frame();
            texture->Release();
        }
        device->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(ok ? "PASS: the towns' lightmaps are relit from the place's sun map on the GPU\n" : "FAIL: sun relight\n");
    return ok ? 0 : 1;
}
