// Shadow casters are collected by the lights, not by the camera (src/effects/shadow_casters.hpp,
// storm/render/shadow_casters.hpp).
//
// First the rules without a GPU: a volume's bounds from an orthographic and a perspective projection, spaces with
// different origins, and which caster a static and a dynamic volume take.
//
// Then through the shim, on OpenGL ES or Vulkan in an SDL window (an X display, like cutout_caster_check): a lit
// 6 x 6 m floor seen from straight above under a slanting sun (its shadows fall 0.5 m towards -x per metre of height),
// and 2 x 2 m plates:
//   * a plate the camera sees casts, and is submitted into the sun's view once;
//   * a plate far outside the camera's volume, which the game's traversal would cull, casts onto the visible floor
//     when the traversal draws it for the shadow volumes only (render::ShadowCasterOnly), and draws no colour;
//   * an off-screen plate inside the sun's volume whose shadow falls beside the camera's view is not submitted;
//   * a plate the camera sees but the sun's volume does not reach is not submitted into the sun's view at all;
//   * a plate that is still geometry of the location is left to the sun's static layer (it casts nothing here: the
//     reported geometry does not hold it), still geometry of a ship casts;
//   * a skinned plate off screen casts where its bones put it, tested with the node's sphere
//     (render::ShadowCasterBounds); without one it is counted as unbounded and still casts.
#include "device.hpp"
#include "check_backend.hpp"
#include "effects/effects.hpp"
#include "effects/shadow_casters.hpp"

#include <SDL2/SDL.h>
#include "skinning.hpp"
#include "skinning_palette.hpp"
#include "storm/render/effects.hpp"
#include "storm/render/shadow_casters.hpp"

#include <bx/math.h>

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
constexpr uint64_t kShip = 7;
bool ok = true;

void check(HRESULT result, const char *what)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "FAIL: %s returned 0x%08x\n", what, static_cast<unsigned>(result));
        std::exit(1);
    }
}

void expect(bool condition, const char *what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    ok &= condition;
}

// ---------------------------------------------------------------------------------------------------------------
// The rules, without a GPU.

effects::ShadowCaster caster_at(float x, float y, float z, float radius)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    effects::ShadowCaster caster;
    caster.world = identity;
    caster.center = {x, y, z};
    caster.radius = radius;
    caster.lit = caster.sun = true;
    return caster;
}

bool takes(const effects::ShadowVolume &volume, const effects::ShadowCaster &caster)
{
    effects::ShadowCasters casters;
    casters.publish(volume);
    return casters.takes(caster);
}

void check_rules()
{
    // An orthographic map: x, y within 10 m, depth 0..20 m along z.
    effects::ShadowVolume box;
    box.set_clip_bounds({0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 1}, false);
    expect(box.plane_count == 6, "an orthographic projection bounds its volume with six planes");
    expect(takes(box, caster_at(0, 0, 10, 1)), "a caster inside the orthographic volume is taken");
    expect(takes(box, caster_at(10.5f, 0, 10, 1)), "a caster whose sphere crosses the volume's side is taken");
    expect(!takes(box, caster_at(11.5f, 0, 10, 1)), "a caster beside the orthographic volume is left out");
    expect(!takes(box, caster_at(0, 0, -1.5f, 1)), "a caster before the volume's near plane is left out");
    expect(!takes(box, caster_at(0, 0, 21.5f, 1)), "a caster beyond the volume's far plane is left out");
    expect(takes(box, caster_at(500, 0, 10, -1)), "a caster without bounds is taken by every volume");
    // The same map with the clip depth -w..w.
    effects::ShadowVolume gl;
    gl.set_clip_bounds({0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, -1, 1}, true);
    expect(takes(gl, caster_at(0, 0, 0.5f, 0.1f)) && !takes(gl, caster_at(0, 0, -1.5f, 1)) && !takes(gl, caster_at(0, 0, 21.5f, 1)),
           "a projection with the clip depth -w..w bounds the same volume");
    // A lamp's face: 90 degrees along +z from the origin, 10 m far, within the lamp's range.
    effects::ShadowVolume face;
    std::array<float, 16> projection;
    bx::mtxProj(projection.data(), 90.0f, 1.0f, 0.1f, 10.0f, false);
    face.set_clip_bounds(projection, false);
    face.sphere = {0, 0, 0, 10};
    expect(face.plane_count == 6, "a perspective projection bounds its volume with six planes");
    expect(takes(face, caster_at(0, 0, 5, 0.5f)), "a caster inside a lamp's face is taken");
    expect(!takes(face, caster_at(0, 0, -5, 0.5f)), "a caster behind a lamp's face is left out");
    expect(!takes(face, caster_at(6, 0, 3, 0.5f)), "a caster beside a lamp's face is left out");
    expect(!takes(face, caster_at(0, 0, 11, 0.5f)), "a caster out of the lamp's range is left out");
    // Spaces: the volume in a world moved by 1000 m, the caster in a device world whose origin is 990 m away.
    effects::ShadowVolume moved = box;
    moved.origin = {1000, 0, 0};
    effects::ShadowCaster relative = caster_at(10, 0, 10, 1);
    relative.origin = {990, 0, 0};
    expect(takes(moved, relative), "a caster is brought into the volume's space by the two origins");
    relative.origin = {970, 0, 0};
    expect(!takes(moved, relative), "... and left out when that puts it beside the volume");

    // Static and dynamic layers.
    effects::ShadowVolume dynamic_layer = box, static_layer = box;
    dynamic_layer.layer = effects::ShadowLayer::Dynamic;
    dynamic_layer.still = render::kShadowStillLocation;
    static_layer.layer = effects::ShadowLayer::Static;
    static_layer.still = render::kShadowStillLocation;
    effects::ShadowCaster moving = caster_at(0, 0, 10, 1), place = moving, ship = moving, unlit = moving, unlit_place = moving,
                          no_sun = moving;
    place.still = render::kShadowStillLocation;
    ship.still = kShip;
    unlit.lit = unlit.sun = false;
    unlit_place.lit = unlit_place.sun = false;
    unlit_place.still = render::kShadowStillLocation;
    no_sun.sun = false;
    expect(takes(dynamic_layer, moving) && takes(dynamic_layer, ship), "a dynamic volume takes what moves and another owner's still geometry");
    expect(!takes(dynamic_layer, place), "a dynamic volume leaves out the still geometry its static layer holds");
    expect(!takes(dynamic_layer, unlit) && !takes(dynamic_layer, unlit_place), "a dynamic volume takes lit draws only");
    expect(takes(static_layer, place) && takes(static_layer, unlit_place), "a static volume takes its owner's still geometry, lit or not");
    expect(!takes(static_layer, moving) && !takes(static_layer, ship), "a static volume takes nothing else");
    expect(!takes(dynamic_layer, no_sun), "the sun's volume takes only what the sun lights");
    effects::ShadowVolume lamp = dynamic_layer;
    lamp.pass = effects::Casters::Pass::Lamp;
    expect(takes(lamp, no_sun), "a lamp's volume takes a lit draw the sun does not light");

    // Shadows nobody sees: a camera looking along +z from the origin (90 degrees, 0.1..50 m), a lamp 10 m ahead.
    {
        effects::ShadowCasters seen;
        std::array<float, 16> camera;
        bx::mtxProj(camera.data(), 90.0f, 1.0f, 0.1f, 50.0f, false);
        seen.set_camera(camera, {});
        effects::ShadowVolume all;
        all.sphere = {0, 0, 10, 30};
        all.light = {0, 0, 10, 1};
        all.pass = effects::Casters::Pass::Lamp;
        seen.publish(all);
        expect(seen.takes(caster_at(0, 0, 5, 0.5f)), "a caster in view is taken by a lamp");
        expect(seen.takes(caster_at(0, 0, 14, 0.5f)), "a caster behind the lamp, in view, is taken");
        expect(!seen.takes(caster_at(0, 0, -5, 0.5f)), "a caster behind the camera whose shadow falls further behind is left out");
        expect(seen.takes(caster_at(12, 0, 10, 0.5f)) == false && seen.takes(caster_at(-3, 0, -1, 0.5f)) == false,
               "casters beside and behind the view whose shadows point away from it are left out");
        effects::ShadowCasters across;
        across.set_camera(camera, {});
        all.sphere = {0, 0, -10, 30};
        all.light = {0, 0, -10, 1};
        across.publish(all);
        expect(across.takes(caster_at(0, 0, -5, 0.5f)), "a caster behind the camera between it and the lamp shadows the view: taken");
        // The first draw of a frame may come from a pass with other near and far planes (at sea the far pass: near
        // 500 m): only the field of view bounds what is seen, so the ships 120 m ahead still cast.
        effects::ShadowCasters far_pass;
        std::array<float, 16> far_camera;
        bx::mtxProj(far_camera.data(), 90.0f, 1.0f, 500.0f, 5000.0f, false);
        far_pass.set_camera(far_camera, {});
        all.sphere = {0, 0, 126, 30};
        all.light = {0, 0, 126, 1};
        far_pass.publish(all);
        expect(far_pass.takes(caster_at(0, 0, 120, 0.5f)), "a near plane beyond the casters does not leave them out");
    }

    // reach(): the traversal's question, for the current owner.
    effects::ShadowCasters casters;
    casters.publish(dynamic_layer);
    expect(casters.reach({0, 0, 10}, 1, {}, 0) && !casters.reach({0, 0, 10}, 1, {}, render::kShadowStillLocation) &&
               !casters.reach({50, 0, 10}, 1, {}, 0),
           "reach() answers for the sphere and the still owner");
    casters.end_frame();
    expect(casters.volumes() == 0 && !casters.reach({0, 0, 10}, 1, {}, 0), "the volumes last one frame");
}

// ---------------------------------------------------------------------------------------------------------------
// Through the shim.

struct Vertex
{
    float pos[3];
    float nrm[3];
    uint32_t color;
    float uv[2];
};

struct Plate
{
    std::array<float, 3> at{};  // its centre; 2 x 2 m, horizontal
    bool only = false;          // the game's traversal culled it by the camera and draws it for the shadow volumes
    uint64_t still = 0;         // still geometry of this owner
    bool skinned = false;       // GEOS::AVERTEX0 vertices at the origin; one bone carries them to `at`
    bool bounds = false;        // skinned: inside a render::ShadowCasterBounds
    std::array<float, 3> bounds_at{};
};

struct Probe
{
    std::array<float, 3> center{};
    float radius = 0.0f;
    bool reach = false; // out
};

struct Scene
{
    std::vector<Plate> plates;
    std::vector<Probe> probes; // render::ShadowCasterReach asked while the scene is drawn
};

class Harness
{
  public:
    explicit Harness(Device &device) : d_(device)
    {
        check(d_.CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback_, nullptr),
              "CreateOffscreenPlainSurface");
        check(d_.GetRenderTarget(0, &back_), "GetRenderTarget");
        check(d_.CreateTexture(model::kPaletteTexelsPerBone, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &palette_,
                               nullptr),
              "CreateTexture(palette)");
    }
    ~Harness()
    {
        palette_->Release();
        readback_->Release();
        back_->Release();
    }

    // The floor from 5 m above, orthographic over 6 x 6 m and 500 m deep; returns the picture (BGRA).
    std::vector<uint8_t> render(Scene &scene, const D3DLIGHT9 &sun)
    {
        check(d_.BeginScene(), "BeginScene");
        check(d_.Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0), "Clear");
        D3DMATRIX identity{};
        identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1.0f;
        // Row-vector view: world x -> view x, world z -> view y, world y -> towards the camera; eye 5 m up.
        D3DMATRIX view{};
        view.m[0][0] = 1.0f;
        view.m[1][2] = -1.0f;
        view.m[2][1] = 1.0f;
        view.m[3][2] = 5.0f;
        view.m[3][3] = 1.0f;
        D3DMATRIX projection{};
        projection.m[0][0] = projection.m[1][1] = 1.0f / 3.0f;
        projection.m[2][2] = 0.002f;
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
        d_.SetRenderState(D3DRS_LIGHTING, TRUE);
        D3DMATERIAL9 material{};
        material.Diffuse = material.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
        d_.SetMaterial(&material);
        d_.SetLight(0, &sun);
        d_.LightEnable(0, TRUE);
        d_.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d_.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d_.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d_.SetTexture(0, nullptr);

        // The traversal's questions come before its draws: the first of a frame publishes the frame's volumes.
        for (Probe &probe : scene.probes)
            probe.reach = render::ShadowCasterReach(probe.center, probe.radius);

        // The floor: 12 x 12 quads.
        d_.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        std::vector<Vertex> floor;
        const int n = 12;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
            {
                const float corners[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
                for (const auto &c : corners)
                    floor.push_back({{-3.0f + 6.0f * (x + c[0]) / n, 0.0f, -3.0f + 6.0f * (z + c[1]) / n},
                                     {0.0f, 1.0f, 0.0f},
                                     0xFF909090u,
                                     {0.0f, 0.0f}});
            }
        check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(floor.size() / 3), floor.data(), sizeof(Vertex)), "draw floor");

        for (const Plate &plate : scene.plates)
        {
            const render::ShadowCasterStill still(plate.still);
            const auto draw = [&] {
                const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
                if (!plate.skinned)
                {
                    Vertex vertices[6];
                    for (int i = 0; i < 6; ++i)
                        vertices[i] = {{plate.at[0] + corners[i][0], plate.at[1], plate.at[2] + corners[i][1]},
                                       {0.0f, 1.0f, 0.0f},
                                       0xFFFFFFFFu,
                                       {0.0f, 0.0f}};
                    d_.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                    check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, sizeof(Vertex)), "draw plate");
                    return;
                }
                // The vertices in the bind pose around the origin; the bone carries them to the plate's place. The
                // skinning kernel mirrors x (skinning_palette.hpp), so the bone's x is the mirrored one.
                skinning::Source vertices[6]{};
                for (int i = 0; i < 6; ++i)
                {
                    vertices[i].pos[0] = corners[i][0];
                    vertices[i].pos[2] = corners[i][1];
                    vertices[i].weight = 1.0f;
                    vertices[i].nrm[1] = 1.0f;
                    vertices[i].color = -1;
                }
                const float bone[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -plate.at[0], plate.at[1], plate.at[2], 1};
                D3DLOCKED_RECT locked;
                check(palette_->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD), "LockRect(palette)");
                model::writePaletteRow(bone, static_cast<float *>(locked.pBits));
                palette_->UnlockRect(0);
                d_.SetTexture(D3DVERTEXTEXTURESAMPLER0, palette_);
                d_.SetFVF(model::kSkinnedSourceFvf);
                check(d_.DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, model::kSkinnedSourceStride), "draw skinned plate");
                d_.SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
            };
            const auto draw_bounded = [&] {
                if (!plate.bounds)
                    return draw();
                const render::ShadowCasterBounds bounds(plate.bounds_at, 1.5f);
                draw();
            };
            if (plate.only)
            {
                const render::ShadowCasterOnly only;
                draw_bounded();
            }
            else
                draw_bounded();
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
    storm::shim::Texture2D *palette_ = nullptr;
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

} // namespace

int main()
{
    check_rules();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video: %s (an X display is needed)\n", SDL_GetError());
        return 1;
    }
    host_check::UseBackendFromEnvironment();
    SDL_Window *window = SDL_CreateWindow("shadow_casters_check", 0, 0, kSize, kSize, 0);
    if (!window || !shim::Context::instance().init(window, kSize, kSize, false))
    {
        std::fprintf(stderr, "FAIL: no render window: %s\n", SDL_GetError());
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
            // The location's still geometry: the floor slab (so the sun's static layer holds the place).
            const std::vector<std::array<float, 3>> positions = {{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 3.0f}};
            const std::vector<std::array<float, 2>> uvs(positions.size(), {0.0f, 0.0f});
            const std::vector<uint32_t> colors(positions.size(), 0xFF909090u);
            const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
            const std::array<float, 3> room_lo{-3.0f, -0.2f, -3.0f}, room_hi{3.0f, 3.5f, 3.0f};
            render::SetEffectsLocationGeometry({"shadow_casters_check", positions, colors, uvs, indices, {}, false, room_lo, room_hi});
            // The sun from (1, 2, 0): a shadow lies 0.5 m towards -x per metre of height.
            D3DLIGHT9 sun{};
            sun.Type = D3DLIGHT_DIRECTIONAL;
            sun.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
            sun.Direction = {-1.0f, -2.0f, 0.0f};
            render::EffectsWeather weather;
            weather.sun_dir = {0.4472f, 0.8944f, 0.0f};
            weather.sky_sun_dir = weather.sun_dir;
            weather.sun_present = true;
            render::ConfigureEffects(2, {{"fx_ao_volume", 0}, {"fx_lamp_shadows", 0}, {"fx_shadows", 2}});

            constexpr uint32_t kFloorTriangles = 12 * 12 * 2;
            // The plate the camera cannot see: 8.4 m up (above the eye) and 4.2 m to the side (outside the 6 m the
            // camera covers); its shadow lies on the middle of the floor, x -1..1.
            const std::array<float, 3> high{4.2f, 8.4f, 0.0f};
            // The same, held by a bone 1 m further along z: its shadow covers z 0..2.
            const std::array<float, 3> posed{4.2f, 8.4f, 1.0f};
            struct Case
            {
                const char *name;
                Scene scene;
                std::array<float, 2> shadow; // the floor point the plate's shadow covers (or would)
                bool shadowed;
                uint32_t candidates, casters, offscreen, skinned, unbounded; // the frame's counts
                uint32_t sun_draws, sun_triangles;                           // the sun's view
            };
            Case cases[] = {
                {"a plate the camera sees casts, submitted once", {{{{0.0f, 1.0f, 0.0f}}}, {}}, {-1.25f, 0.0f}, true, 2, 2, 0, 0, 0, 2, kFloorTriangles + 2},
                {"an off-screen plate the traversal culled casts nothing (the control)", {{}, {{high, 1.5f}, {{0.0f, -400.0f, 0.0f}, 1.5f}}}, {0.0f, 0.0f}, false, 1, 1, 0, 0, 0, 1, kFloorTriangles},
                {"an off-screen plate drawn for the shadow volumes casts onto the visible floor", {{{high, true}}, {}}, {0.0f, 0.0f}, true, 2, 2, 1, 0, 0, 2, kFloorTriangles + 2},
                {"a plate in view drawn for the shadow volumes only draws no colour", {{{{0.0f, 1.0f, 0.0f}, true}}, {}}, {0.0f, 0.0f}, true, 2, 2, 1, 0, 0, 2, kFloorTriangles + 2},
                {"a plate the camera sees beyond the sun's volume is not submitted to it", {{{{0.0f, 1.0f, 0.0f}}, {{0.0f, -400.0f, 0.0f}}}, {}}, {-1.25f, 0.0f}, true, 3, 2, 0, 0, 0, 2, kFloorTriangles + 2},
                {"still geometry of the location is left to the sun's static layer", {{{{0.0f, 1.0f, 0.0f}, false, render::kShadowStillLocation}}, {}}, {-1.25f, 0.0f}, false, 2, 1, 0, 0, 0, 1, kFloorTriangles},
                {"still geometry of a ship casts into the sun's map", {{{{0.0f, 1.0f, 0.0f}, false, kShip}}, {}}, {-1.25f, 0.0f}, true, 2, 2, 0, 0, 0, 2, kFloorTriangles + 2},
                {"an off-screen plate whose shadow falls outside the camera's view is not submitted", {{{{-8.0f, 2.0f, 0.0f}, true}}, {}}, {0.0f, 0.0f}, false, 2, 1, 0, 0, 0, 1, kFloorTriangles},
                {"a skinned plate off screen casts where its bone puts it", {{{posed, true, 0, true, true, posed}}, {}}, {0.0f, 1.5f}, true, 2, 2, 1, 1, 0, 2, kFloorTriangles + 2},
                {"a skinned plate whose node lies beyond the sun's volume is not submitted", {{{posed, true, 0, true, true, {0.0f, -400.0f, 0.0f}}}, {}}, {0.0f, 1.5f}, false, 2, 1, 0, 0, 0, 1, kFloorTriangles},
                {"a skinned plate without a node's bounds casts, counted as unbounded", {{{posed, true, 0, true, false, {}}}, {}}, {0.0f, 1.5f}, true, 2, 2, 1, 1, 1, 2, kFloorTriangles + 2},
            };
            for (Case &c : cases)
            {
                std::vector<uint8_t> picture;
                // The preset switches over a few frames; then the map is of the frame that draws it.
                for (int frame = 0, active = 0;; ++frame) // on the preparation's progress, no frame budget
                {
                    render::SetEffectsLamps({});
                    render::SetEffectsWeather(weather);
                    picture = harness.render(c.scene, sun);
                    active = render::CurrentEffects().pending ? 0 : active + 1;
                    if (active > 3)
                        break;
                    SDL_Delay(5);
                }
                if (const char *dump = std::getenv("SHADOW_CASTERS_CHECK_DUMP"))
                {
                    char file[512];
                    std::snprintf(file, sizeof(file), "%s/case_%d.ppm", dump, static_cast<int>(&c - cases));
                    if (FILE *f = std::fopen(file, "wb"))
                    {
                        std::fprintf(f, "P6 %u %u 255\n", kSize, kSize);
                        for (size_t i = 0; i < picture.size(); i += 4)
                            std::fputc(picture[i + 2], f), std::fputc(picture[i + 1], f), std::fputc(picture[i], f);
                        std::fclose(f);
                    }
                }
                const float shadow = floor_at(picture, c.shadow[0], c.shadow[1]);
                const float open = floor_at(picture, 2.4f, -2.4f); // no plate's shadow reaches it
                const effects::ShadowCasters::FrameCount &count = effects::Host::instance().lighting().shadow_casters().last();
                uint32_t draws = 0, triangles = 0;
                for (const effects::ShadowCasters::ViewCount &view : count.views)
                    draws += view.draws, triangles += view.triangles;
                bool pass = !render::CurrentEffects().pending && count.views.size() == 1;
                pass &= c.shadowed ? shadow < 0.85f * open : shadow > 0.9f * open;
                pass &= count.candidates == c.candidates && count.casters == c.casters && count.offscreen == c.offscreen &&
                        count.skinned == c.skinned && count.unbounded == c.unbounded && draws == c.sun_draws &&
                        triangles == c.sun_triangles;
                std::printf("%s %s: floor in the plate's shadow %.1f, open %.1f; candidates %u, casters %u (off screen %u, skinned "
                            "%u, unbounded %u), %zu shadow views with %u draws and %u triangles\n",
                            pass ? "PASS" : "FAIL", c.name, shadow, open, count.candidates, count.casters, count.offscreen,
                            count.skinned, count.unbounded, count.views.size(), draws, triangles);
                ok &= pass;
                if (!c.scene.probes.empty())
                    expect(c.scene.probes[0].reach && !c.scene.probes[1].reach,
                           "the traversal is told that the off-screen plate reaches a shadow volume, and that a sphere 400 m below does not");
            }
            render::SetEffectsLocationGeometry({});
            render::ConfigureEffects(0, {});
            Scene none;
            harness.render(none, sun);
        }
        device->Release();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(ok ? "PASS: the lights collect their casters: off screen, skinned, once per volume, static apart from dynamic\n"
                   : "FAIL: shadow casters\n");
    return ok ? 0 : 1;
}
