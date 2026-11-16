// The sun's two shadow layers through the shim (src/effects/shadows.hpp), on OpenGL ES and Vulkan in an SDL window (an
// X display, like cutout_caster_check). A 6 x 6 m floor seen from 5 m above under a sun 45° up in +x:
//   * a plate drawn lit 1 m over the floor (a moving caster: the dynamic layer), its shadow at (-1.7, -1.5);
//   * a plate reported with the place's still geometry and not drawn (the static layer), its shadow at (-1.7, 0.8);
//   * a small lit plate whose shadow falls inside the still plate's, at (-1.55, 1.5).
// Receivers:
//   * a lit floor (per-pixel lighting) is dark under both layers;
//   * a floor with baked colours (the plain fixed-function program: fs_ff_dyn) is dark only under the moving plate;
//     the still plate's shadow is the bake's business, and inside it the moving caster adds nothing;
//   * the same with a dynamic light on it (the baked per-pixel variant's ground mode).
// The static layer is drawn once and not again while the camera stays inside its guard band or the sun moves less
// than a step; it is drawn again when the camera leaves the band and when the sun steps. The dynamic layer is drawn
// only in frames with a moving caster. Without sun shadows (the option off, Низкая) no receiver is dark and nothing
// is drawn into either layer.
#include "device.hpp"
#include "check_backend.hpp"

#include "effects/effects.hpp"
#include "storm/render/effects.hpp"

#include <SDL2/SDL.h>

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

constexpr uint32_t kSize = 128;

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

struct Scene
{
    bool lit_floor = false;  // the floor is drawn with lighting (else baked colours)
    shim::Texture2D *floor_texture = nullptr; // the floor as the game draws its location geometry: textured, alpha test on
    bool plates = true;      // the moving plates are drawn
    std::array<float, 2> eye{0.0f, 0.0f}; // the camera's x, z (5 m up, looking down)
    std::array<float, 3> origin{};        // the device world's origin in the absolute world (the sea's camera-relative world)
    D3DLIGHT9 sun{};
};

void quad(std::vector<Vertex> &out, float x0, float x1, float y, float z0, float z1, uint32_t color)
{
    const float c[6][2] = {{x0, z0}, {x1, z0}, {x0, z1}, {x0, z1}, {x1, z0}, {x1, z1}};
    for (const auto &p : c)
        out.push_back({{p[0], y, p[1]}, {0.0f, 1.0f, 0.0f}, color, {0.0f, 0.0f}});
}

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

    std::vector<uint8_t> render(const Scene &scene)
    {
        check(d_.BeginScene(), "BeginScene");
        check(d_.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0), "Clear");
        D3DMATRIX identity{};
        identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
        // Row-vector view: world x -> view x, world z -> view y, world y -> towards the camera; the eye 5 m up.
        D3DMATRIX view{};
        view.m[0][0] = 1.0f;
        view.m[1][2] = -1.0f;
        view.m[2][1] = 1.0f;
        view.m[3][0] = -scene.eye[0];
        view.m[3][1] = -scene.eye[1];
        view.m[3][2] = 5.0f;
        view.m[3][3] = 1.0f;
        // Perspective: the 6 m floor fills the picture at 5 m.
        const float zn = 0.5f, zf = 50.0f, cot = 5.0f / 3.0f;
        D3DMATRIX projection{};
        projection.m[0][0] = projection.m[1][1] = cot;
        projection.m[2][2] = zf / (zf - zn);
        projection.m[2][3] = 1.0f;
        projection.m[3][2] = -zn * zf / (zf - zn);
        render::SetEffectsWorldOrigin(scene.origin);
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
        d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d_.SetTexture(0, nullptr);

        d_.SetRenderState(D3DRS_LIGHTING, scene.lit_floor ? TRUE : FALSE);
        if (scene.floor_texture)
        {
            d_.SetTexture(0, scene.floor_texture);
            d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            d_.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            d_.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            d_.SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            d_.SetRenderState(D3DRS_ALPHAREF, 0xa0);
            d_.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        }
        std::vector<Vertex> floor;
        const int n = 12;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
                quad(floor, -3.0f + 6.0f * x / n, -3.0f + 6.0f * (x + 1) / n, 0.0f, -3.0f + 6.0f * z / n, -3.0f + 6.0f * (z + 1) / n,
                     0xFF909090u);
        check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(floor.size() / 3), floor.data(), sizeof(Vertex)), "draw floor");
        d_.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d_.SetTexture(0, nullptr);
        d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        if (scene.plates)
        {
            d_.SetRenderState(D3DRS_LIGHTING, TRUE);
            std::vector<Vertex> plate;
            quad(plate, -1.0f, 1.0f, 1.0f, -2.4f, -0.6f, 0xFFFFFFFFu);
            check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, plate.data(), sizeof(Vertex)), "draw plate");
            plate.clear();
            quad(plate, -1.2f, -0.2f, 0.5f, 1.0f, 2.0f, 0xFFFFFFFFu);
            check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, plate.data(), sizeof(Vertex)), "draw small plate");
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

// Mean green of a few pixels around the floor point (x, z) (the picture is read back top row first).
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

void dump(const std::vector<uint8_t> &picture, const char *name)
{
    const char *directory = std::getenv("SUN_LAYERS_CHECK_DUMP");
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
    SDL_Window *window = SDL_CreateWindow("sun_layers_check", 0, 0, kSize, kSize, 0);
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
            // The place's still geometry: the floor, and a plate 1 m over it that is not drawn.
            const std::vector<std::array<float, 3>> positions = {{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f},
                                                                 {-3.0f, 0.0f, 3.0f}, {-1.0f, 1.0f, 0.6f},  {1.0f, 1.0f, 0.6f},
                                                                 {1.0f, 1.0f, 2.4f},  {-1.0f, 1.0f, 2.4f}};
            const std::vector<std::array<float, 2>> uvs(positions.size(), {0.0f, 0.0f});
            const std::vector<uint32_t> colors(positions.size(), 0xFF909090u);
            const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
            const std::array<float, 3> room_lo{-3.0f, -0.2f, -3.0f}, room_hi{3.0f, 3.5f, 3.0f};
            render::SetEffectsLocationGeometry({"sun_layers_check", positions, colors, uvs, indices, {}, false, room_lo, room_hi});

            const auto sun_at = [](float degrees_off) {
                // 45° up in +x, turned around the vertical by `degrees_off`.
                const float a = degrees_off * 0.01745329f;
                return std::array<float, 3>{0.7071f * std::cos(a), 0.7071f, 0.7071f * std::sin(a)};
            };
            render::EffectsWeather weather;
            Scene scene;
            const auto set_sun = [&](float degrees_off) {
                weather.sun_dir = sun_at(degrees_off);
                weather.sky_sun_dir = weather.sun_dir;
                weather.sun_present = true;
                scene.sun = {};
                scene.sun.Type = D3DLIGHT_DIRECTIONAL;
                scene.sun.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
                scene.sun.Direction = {-weather.sun_dir[0], -weather.sun_dir[1], -weather.sun_dir[2]};
            };
            set_sun(0.0f);
            const auto frame = [&](const Scene &s) {
                render::SetEffectsLamps({});
                render::SetEffectsWeather(weather);
                return harness.render(s);
            };
            const auto settle = [&](const Scene &s) {
                std::vector<uint8_t> picture;
                for (int f = 0, active = 0;; ++f) // on the preparation's progress, no frame budget
                {
                    picture = frame(s);
                    active = render::CurrentEffects().pending ? 0 : active + 1;
                    if (active > 6 && f > 12)
                        break;
                    SDL_Delay(5);
                }
                return picture;
            };
            shim::Texture2D *white = nullptr;
            check(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white, nullptr), "CreateTexture");
            {
                D3DLOCKED_RECT rect;
                check(white->LockRect(0, &rect, nullptr, 0), "LockRect");
                for (int y = 0; y < 4; ++y)
                    std::memset(static_cast<uint8_t *>(rect.pBits) + y * rect.Pitch, 0xFF, 16);
                white->UnlockRect(0);
            }
            const effects::SunShadows &sun_shadows = effects::Host::instance().lighting().shadows();
            const effects::Lighting &lighting = effects::Host::instance().lighting();
            const render::EffectsPointLight torch{{2.6f, 0.5f, -2.6f}, {1.0f, 0.8f, 0.6f}, 1.5f};

            for (const int preset : {2, 1})
            {
                const char *name = preset == 2 ? "Высокая" : "Средняя";
                render::ConfigureEffects(preset, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 0}, {"fx_town_light", 0}});
                char file[64];
                // ---- receivers ----
                scene.lit_floor = true;
                scene.plates = true;
                scene.eye = {0.0f, 0.0f};
                render::SetEffectsDynamicLights({});
                std::vector<uint8_t> picture = settle(scene);
                std::snprintf(file, sizeof(file), "preset%d_lit", preset);
                dump(picture, file);
                float open = floor_at(picture, 2.4f, 2.4f), moving = floor_at(picture, -1.7f, -1.5f),
                      still = floor_at(picture, -1.7f, 0.8f), both = floor_at(picture, -1.55f, 1.5f);
                std::printf("%s, lit floor: open %.1f, under the moving plate %.1f, under the still plate %.1f, under both %.1f\n",
                            name, open, moving, still, both);
                expect(moving < 0.7f * open && still < 0.7f * open && both < 0.7f * open, "a lit floor is dark under both layers");
                expect(std::fabs(moving - still) < 0.08f * open, "both layers shade a lit floor alike");

                scene.lit_floor = false;
                picture = settle(scene);
                std::snprintf(file, sizeof(file), "preset%d_baked", preset);
                dump(picture, file);
                open = floor_at(picture, 2.4f, 2.4f), moving = floor_at(picture, -1.7f, -1.5f);
                still = floor_at(picture, -1.7f, 0.8f), both = floor_at(picture, -1.55f, 1.5f);
                std::printf("%s, baked floor (fs_ff_dyn): open %.1f, under the moving plate %.1f, under the still plate %.1f, "
                            "under both %.1f; ground draws %u\n",
                            name, open, moving, still, both, lighting.last_frame().ground);
                expect(lighting.last_frame().ground >= 1 && lighting.last_frame().baked == 0, "the baked floor takes the plain ground variant");
                expect(moving < 0.7f * open && moving > 0.3f * open, "a moving caster takes the sun's share of a baked floor");
                expect(still > 0.95f * open, "the still geometry's shadow is the bake's, not drawn on a baked floor");
                expect(both > 0.95f * open, "a moving caster adds nothing inside the still geometry's shade");

                // The floor as the game draws its ground: textured, the alpha test on (alpha to coverage on these presets).
                scene.floor_texture = white;
                picture = settle(scene);
                scene.floor_texture = nullptr;
                open = floor_at(picture, 2.4f, 2.4f), moving = floor_at(picture, -1.7f, -1.5f);
                still = floor_at(picture, -1.7f, 0.8f), both = floor_at(picture, -1.55f, 1.5f);
                std::printf("%s, baked floor with the alpha test (fs_ff_dyn_coverage): open %.1f, moving %.1f, still %.1f, both %.1f; "
                            "coverage draws %u\n", name, open, moving, still, both, lighting.last_frame().coverage);
                expect(lighting.last_frame().coverage >= 1 && lighting.last_frame().ground >= 1, "the alpha-tested floor takes the ground variant's coverage twin");
                expect(moving < 0.7f * open && moving > 0.3f * open && still > 0.95f * open && both > 0.95f * open,
                       "the alpha-tested ground shades like the plain ground variant");

                render::SetEffectsDynamicLights(std::span(&torch, 1));
                for (int f = 0; f < 4; ++f)
                {
                    render::SetEffectsDynamicLights(std::span(&torch, 1));
                    picture = frame(scene);
                }
                std::snprintf(file, sizeof(file), "preset%d_baked_variant", preset);
                dump(picture, file);
                open = floor_at(picture, 2.4f, 2.4f), moving = floor_at(picture, -1.7f, -1.5f);
                still = floor_at(picture, -1.7f, 0.8f), both = floor_at(picture, -1.55f, 1.5f);
                std::printf("%s, baked floor under a dynamic light (the baked variant): open %.1f, moving %.1f, still %.1f, both "
                            "%.1f; baked draws %u\n",
                            name, open, moving, still, both, lighting.last_frame().baked);
                expect(lighting.last_frame().baked >= 1, "a dynamic light moves the baked floor to the per-pixel variant");
                expect(moving < 0.7f * open && moving > 0.3f * open && still > 0.95f * open && both > 0.95f * open,
                       "the baked variant's ground mode shades like the plain ground variant");
                render::SetEffectsDynamicLights({});

                // ---- the static layer is kept ----
                scene.lit_floor = true;
                settle(scene);
                const auto base = sun_shadows.totals();
                for (int f = 0; f < 40; ++f)
                {
                    scene.eye = {3.0f * std::sin(0.4f * f), 3.0f * std::cos(0.3f * f)}; // inside the 10 m band
                    frame(scene);
                }
                expect(sun_shadows.totals().static_redraws == base.static_redraws, "the static layer is not drawn again inside its band");
                expect(sun_shadows.totals().dynamic_frames == base.dynamic_frames + 40, "the dynamic layer is drawn in every frame with casters");
                // A sun that moved less than a step: nothing. A step: once.
                scene.eye = {0.0f, 0.0f};
                set_sun(0.2f);
                for (int f = 0; f < 5; ++f)
                    frame(scene);
                expect(sun_shadows.totals().static_redraws == base.static_redraws, "a sun inside its step keeps the static layer");
                set_sun(1.5f);
                for (int f = 0; f < 5; ++f)
                    frame(scene);
                expect(sun_shadows.totals().static_redraws == base.static_redraws + 1, "a sun step draws the static layer once");
                set_sun(0.0f);
                for (int f = 0; f < 5; ++f)
                    picture = frame(scene);
                expect(sun_shadows.totals().static_redraws == base.static_redraws + 2, "the sun stepping back draws it once more");
                expect(floor_at(picture, -1.7f, 0.8f) < 0.7f * floor_at(picture, 2.4f, 2.4f), "the redrawn static layer still shades");
                // Leaving the band: once; the kept layer shades from the new place too.
                scene.eye = {25.0f, 0.0f};
                for (int f = 0; f < 5; ++f)
                    frame(scene);
                expect(sun_shadows.totals().static_redraws == base.static_redraws + 3, "leaving the band draws the static layer once");
                scene.eye = {0.0f, 0.0f};
                for (int f = 0; f < 5; ++f)
                    picture = frame(scene);
                expect(sun_shadows.totals().static_redraws == base.static_redraws + 4, "coming back draws it once");
                // No moving caster (the floor baked: a lit floor is one itself): the dynamic layer is not drawn, and its
                // last shadow is gone. The still plate's shadow is the bake's then; on a lit floor it stays.
                scene.plates = false;
                scene.lit_floor = false;
                frame(scene);
                const uint32_t dynamic_before = sun_shadows.totals().dynamic_frames;
                for (int f = 0; f < 10; ++f)
                    picture = frame(scene);
                expect(sun_shadows.totals().dynamic_frames == dynamic_before, "the dynamic layer is not drawn without casters");
                open = floor_at(picture, 2.4f, 2.4f);
                expect(floor_at(picture, -1.7f, -1.5f) > 0.95f * open, "no shadow stays behind a caster that left");
                scene.lit_floor = true;
                for (int f = 0; f < 3; ++f)
                    picture = frame(scene);
                open = floor_at(picture, 2.4f, 2.4f);
                expect(floor_at(picture, -1.7f, -1.5f) > 0.9f * open, "a lit floor is lit where the moving plate was"); // 0.9: Высокая's post chain is not flat
                expect(floor_at(picture, -1.7f, 0.8f) < 0.7f * open, "the still plate shades without the moving plates");
                std::printf("%s: static layer drawn %u times in all, %u triangles; dynamic layer in %u frames, %u casters\n", name,
                            sun_shadows.totals().static_redraws, sun_shadows.totals().static_triangles,
                            sun_shadows.totals().dynamic_frames, sun_shadows.totals().dynamic_casters);
            }

            // ---- at sea: no still geometry, a camera-relative world far from the origin, the region on the player ----
            {
                render::ConfigureEffects(2, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 0}, {"fx_town_light", 0}});
                render::SetEffectsLocationGeometry({});
                scene.lit_floor = true;
                scene.plates = true;
                scene.eye = {0.0f, 0.0f};
                scene.origin = {5210.0f, 3.0f, -11840.0f};
                static const int player = 0;
                render::SetEffectsPlayerPosition({scene.origin[0], scene.origin[1], scene.origin[2]}, &player,
                                                 render::EffectsPlayerRole::Ship); // the main ship at sea
                const std::vector<uint8_t> picture = settle(scene);
                dump(picture, "sea");
                const auto before = sun_shadows.totals();
                for (int f = 0; f < 10; ++f)
                    frame(scene);
                const float open = floor_at(picture, 2.4f, 2.4f), moving = floor_at(picture, -1.7f, -1.5f);
                std::printf("at sea (origin %.0f, %.0f, %.0f): open %.1f, under the moving plate %.1f; casters in 10 frames %u\n",
                            scene.origin[0], scene.origin[1], scene.origin[2], open, moving,
                            sun_shadows.totals().dynamic_casters - before.dynamic_casters);
                expect(sun_shadows.totals().dynamic_casters - before.dynamic_casters >= 20, "at sea the moving casters reach the dynamic layer");
                expect(moving < 0.7f * open, "at sea a lit deck is dark under a moving caster");
                expect(sun_shadows.totals().static_redraws == before.static_redraws, "at sea the empty static layer is not drawn again");
                render::ClearEffectsPlayerPosition(&player);
                scene.origin = {};
                render::SetEffectsLocationGeometry({"sun_layers_check", positions, colors, uvs, indices, {}, false, room_lo, room_hi});
            }

            // ---- no sun shadows: nothing is drawn, nothing is dark ----
            for (const int off : {0, 1})
            {
                if (off == 0)
                    render::ConfigureEffects(2, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 0}, {"fx_shadows", 0}, {"fx_town_light", 0}});
                else
                    render::ConfigureEffects(0, {});
                scene.plates = true;
                scene.eye = {0.0f, 0.0f};
                settle(scene); // the settings before these draw until these are prepared
                const auto before = sun_shadows.totals();
                for (const bool lit : {true, false})
                {
                    scene.lit_floor = lit;
                    const std::vector<uint8_t> picture = settle(scene);
                    const float open = floor_at(picture, 2.4f, 2.4f);
                    std::printf("%s, %s floor: open %.1f, under the moving plate %.1f, under the still plate %.1f\n",
                                off == 0 ? "fx_shadows=0" : "Низкая", lit ? "lit" : "baked", open, floor_at(picture, -1.7f, -1.5f),
                                floor_at(picture, -1.7f, 0.8f));
                    expect(floor_at(picture, -1.7f, -1.5f) > 0.9f * open && floor_at(picture, -1.7f, 0.8f) > 0.9f * open,
                           off == 0 ? "fx_shadows=0: no receiver is dark" : "Низкая: no receiver is dark");
                }
                expect(sun_shadows.totals().static_redraws == before.static_redraws &&
                           sun_shadows.totals().dynamic_frames == before.dynamic_frames,
                       off == 0 ? "fx_shadows=0: neither layer is drawn" : "Низкая: neither layer is drawn");
            }
            render::SetEffectsLocationGeometry({});
            render::ConfigureEffects(0, {});
            harness.render({});
            white->Release();
        }
        device->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(ok ? "PASS: the sun's static layer is kept and its dynamic layer shades lit and baked receivers\n"
                   : "FAIL: sun layers\n");
    return ok ? 0 : 1;
}
