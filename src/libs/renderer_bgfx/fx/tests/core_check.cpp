// Unit checks for the CPU side of the fx package: grading LUTs, tone curve,
// baked noise, the sky model, lighting and point-light selection, the shadow cascade and the
// explicit handling of unsupported settings. No GPU is needed.
#include "storm/fx/ao_volume.hpp"
#include "storm/fx/bakes.hpp"
#include "storm/fx/cpu_admission.hpp"
#include "storm/fx/place_probes.hpp"
#include "storm/fx/cutout.hpp"
#include "storm/fx/capabilities.hpp"
#include "storm/fx/lamp_shadows.hpp"
#include "storm/fx/sun_fit.hpp"
#include "storm/fx/town_light.hpp"
#include "storm/fx/visibility.hpp"
#include "storm/fx/color_lut.hpp"
#include "storm/fx/light_set.hpp"
#include "storm/fx/lighting_model.hpp"
#include "storm/fx/log.hpp"
#include "storm/fx/noise_bake.hpp"
#include "storm/fx/relief_model.hpp"
#include "storm/fx/gloss_map.hpp"
#include "storm/fx/height_map.hpp"
#include "storm/fx/normal_map.hpp"
#include "storm/fx/parallax_exclusions.hpp"
#include "storm/fx/parallax_probe.hpp"
#include "storm/fx/parallax_trust.hpp"
#include "storm/fx/relief_store.hpp"
#include "storm/fx/upscale.hpp"
#include "storm/fx/settings.hpp"
#include "storm/fx/skin_tone.hpp"
#include "storm/fx/wave_clock.hpp"
#include "storm/fx/shadow_model.hpp"
#include "storm/fx/sky_model.hpp"
#include "storm/fx/wetness.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <optional>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <string_view>
#include <vector>

using namespace storm::fx;

namespace
{
int failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

bool Near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

// Execute the production shader helper on the CPU, with only the GLSL vector primitives adapted.
namespace PomGeometry
{
using vec3 = std::array<float, 3>;
using vec2 = std::array<float, 2>;
vec3 operator+(vec3 a, vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
vec3 operator-(vec3 a, vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
vec3 operator-(vec3 a) { return {-a[0], -a[1], -a[2]}; }
vec3 operator*(vec3 a, float b) { return {a[0] * b, a[1] * b, a[2] * b}; }
vec3 &operator*=(vec3 &a, float b) { a = a * b; return a; }
float dot(vec3 a, vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
vec3 cross(vec3 a, vec3 b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float inversesqrt(float x) { return 1.0f / std::sqrt(x); }
vec3 vec3_splat(float x) { return {x, x, x}; }
vec3 unit(vec3 x) { return x * inversesqrt(dot(x, x)); }
#include "../shaders/include/fx_parallax_geometry.sh"
}

void CheckParallaxGeometry()
{
    using namespace PomGeometry;
    // Actual Martinique Warehouse masonry triangle: its smooth corner normals face away at this visible joint.
    // This used to set clamp(dot(Ng,V),0,1)=0 and suppress POM, although the geometric plane faces the eye.
    const vec3 a{43.6957321f, 3.0451393f, 74.9864655f};
    const vec3 b{43.6957321f, 3.5463767f, 74.9864655f};
    const vec3 c{39.4454117f, 3.5463767f, 74.9864578f};
    const vec3 V = unit(vec3{35.8f, 4.71787f, 76.35f} - vec3{43.3298306f, 3.4979782f, 74.9864648f});
    const vec3 smooth = unit(vec3{0.29348446f, 0.0f, 0.95596385f});
    const vec3 dx = b - a, dy = c - a;
    const vec2 dux{-0.0466593206f, 0.0f}, duy{-0.0466592908f, 0.3765614331f};
    CHECK(dot(smooth, V) < -0.11f); // Negative control: the old shader's geometric gate was exactly zero.
    const auto f = fxParallaxFrame(dx, dy, dux, duy, V);
    CHECK(dot(f.normal, V) > 0.17f && dot(f.normal, V) < 0.18f);
    CHECK(Near(dot(f.gu, dx), dux[0], 1e-6f) && Near(dot(f.gu, dy), duy[0], 1e-6f));
    CHECK(Near(dot(f.gv, dx), dux[1], 1e-6f) && Near(dot(f.gv, dy), duy[1], 1e-6f));
    CHECK(std::fabs(dot(f.normal, f.gu)) < 1e-6f && std::fabs(dot(f.normal, f.gv)) < 1e-6f);
    const auto reversed = fxParallaxFrame(dy, dx, duy, dux, V);
    const auto back = fxParallaxFrame(dx, dy, dux, duy, -V);
    const auto mirror = fxParallaxFrame(dx, dy, {-dux[0], dux[1]}, {-duy[0], duy[1]}, V);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(Near(reversed.normal[i], f.normal[i], 1e-6f) && Near(reversed.gu[i], f.gu[i], 1e-6f) &&
              Near(reversed.gv[i], f.gv[i], 1e-6f));
        CHECK(Near(back.normal[i], -f.normal[i], 1e-6f) && Near(back.gu[i], f.gu[i], 1e-6f) &&
              Near(back.gv[i], f.gv[i], 1e-6f));
        CHECK(Near(mirror.gu[i], -f.gu[i], 1e-6f) && Near(mirror.gv[i], f.gv[i], 1e-6f));
    }
    // Flat-normal control: the original orthogonal frame is unchanged. Collapsed/collinear derivatives disable POM.
    const auto flat = fxParallaxFrame({2, 0, 0}, {0, 4, 0}, {1, 0}, {0, 1}, {0, 0, 1});
    CHECK(flat.gu == (vec3{0.5f, 0, 0}) && flat.gv == (vec3{0, 0.25f, 0}) && flat.normal == (vec3{0, 0, 1}));
    for (const auto d : {vec3{0, 0, 0}, dx})
    {
        const auto empty = fxParallaxFrame(dx, d, dux, duy, V);
        CHECK(empty.gu == vec3_splat(0) && empty.gv == vec3_splat(0) && empty.normal == vec3_splat(0));
    }
}

float Chroma(std::array<float, 3> c)
{
    return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
}

void CheckSettings()
{
    const Settings original = Settings::FromPreset(QualityPreset::Low);
    CHECK(original.PostIsIdentity());
    CHECK(!original.sea_band_limit && !original.sea_soft_edges && !original.sea_specular);
    CHECK(original.lighting_flags == kLightingNone);
    const Settings medium = Settings::FromPreset(QualityPreset::Medium);
    const Settings high = Settings::FromPreset(QualityPreset::High);
    CHECK(original.far_lod_pixels == 2.f && medium.far_lod_pixels == 1.5f && high.far_lod_pixels == 1.f);
    CHECK(!medium.PostIsIdentity() && !high.PostIsIdentity());
    CHECK(medium.bloom_downscale == 4 && high.bloom_downscale == 4 && medium.bloom_levels == 3 && high.bloom_levels == 3);
    // FSR 1 on every preset, by an internal scene height; «Низкая» otherwise the original look.
    CHECK(original.sharpen && medium.sharpen && high.sharpen);
    // Monotonic: a higher preset never renders the scene smaller («Низкая» is the fastest).
    CHECK(original.render_height > 0 && original.render_height <= medium.render_height &&
          medium.render_height <= high.render_height);
    // On the owner's 3200x1440 panel all three scale; a 1080-line phone scales Низкая and Средняя; 720 lines none.
    CHECK(RenderScale(original.render_height, 1440) == 0.625f && RenderScale(medium.render_height, 1440) < 0.7f &&
          RenderScale(high.render_height, 1440) < 0.8f);
    CHECK(RenderScale(original.render_height, 1080) < 0.85f && RenderScale(medium.render_height, 1080) < 1.0f &&
          RenderScale(high.render_height, 1080) == 1.0f);
    CHECK(RenderScale(original.render_height, 720) == 1.0f && RenderScale(medium.render_height, 720) == 1.0f &&
          RenderScale(high.render_height, 720) == 1.0f);
    CHECK(RenderScale(900, 2160) == 0.5f && RenderScale(0, 2160) == 1.0f); // never below FSR 1's range
    // MSAA is the primary anti-aliasing of the presets; SMAA is left to the player. Optional looks stay off.
    CHECK(medium.msaa == 4 && high.msaa == 4 && !medium.smaa && !high.smaa && original.msaa == 1);
    CHECK(medium.anisotropy == 4 && high.anisotropy == 8 && original.anisotropy == 1);
    CHECK(medium.dither && !original.dither);
    // The game's own sea everywhere: the band limit and the sun's glint from «Средняя», soft edges at «Высокая».
    CHECK(medium.sea_band_limit && !medium.sea_soft_edges && high.sea_band_limit && high.sea_soft_edges);
    CHECK(medium.sea_specular && high.sea_specular);
    // The sky / ground ambient from «Средняя»; linear light in no preset (the game's lights and bakes add in gamma).
    CHECK((medium.lighting_flags & kLightingHemisphere) && (high.lighting_flags & kLightingHemisphere));
    CHECK(!(medium.lighting_flags & kLightingLinear) && !(high.lighting_flags & kLightingLinear));
    // Waves from objects: Средняя and Высокая, never Низкая; without vertex fetch only the lift goes, loudly.
    CHECK(medium.sea_waves && medium.sea_waves_lift && high.sea_waves && high.sea_waves_lift);
    CHECK(!original.sea_waves && !original.sea_waves_lift);
    {
        Support no_vtf;
        no_vtf.sea_waves_vertex = false;
        std::vector<std::string> changes;
        const Settings constrained = ConstrainSettings(high, no_vtf, changes);
        CHECK(constrained.sea_waves && !constrained.sea_waves_lift);
        bool reported = false;
        for (const std::string &line : changes)
            reported |= line.find("wave lift") != std::string::npos;
        CHECK(reported);
    }
    // Gloss maps on «Высокая» only for now (their cost on «Средняя» waits for phone numbers).
    CHECK((medium.lighting_flags & kLightingPerPixel) && !(medium.lighting_flags & kLightingSpecular) && !medium.gloss &&
          high.gloss && !original.gloss && medium.gloss_map_size == 128 && high.gloss_map_size == 256 &&
          original.gloss_strength == 1.0f && medium.gloss_strength == 0.70f && high.gloss_strength == 0.70f);
    CHECK(medium.shadows == ShadowQuality::Low && high.shadows == ShadowQuality::Soft && original.shadows == ShadowQuality::Off);
    CHECK(original.ao_volume == 0 && medium.ao_volume == 2 && high.ao_volume == 2); // Низкая: no shadows, as the original
    CHECK(original.lamp_shadows == 0 && medium.lamp_shadows == 8 && high.lamp_shadows == 16);
    CHECK(original.wet_surfaces == 0 && medium.wet_surfaces == 1 && high.wet_surfaces == 2);
    CHECK(original.wet_force < 0.0f && medium.wet_force < 0.0f && high.wet_force < 0.0f); // the rain decides
    CHECK(!original.town_light && medium.town_light && high.town_light); // Низкая keeps the game's town lightmaps
    CHECK(!medium.lamp_soft && high.lamp_soft && high.lamp_baked == 0.25f && high.lamp_gain == 1.0f && high.lamp_debug == 0);
    CHECK(medium.lamp_model_lift == 0.2f && high.lamp_model_lift == 0.2f); // the owner: characters a little above the room
    CHECK(medium.lamp_tint == 0.25f && high.lamp_tint == 0.25f); // the owner: a quarter of the lamps' own colour on walls
    CHECK(original.lamp_moving == 0 && medium.lamp_moving == 1 && high.lamp_moving == 2); // carried lanterns, ship lanterns
    // «Средняя»: the cheap looks (tone curve, the grade of the place and hour, aerial perspective, rim light) under
    // the restrained Caribbean grade; «Высокая» adds the sun glare and sun shafts. «Низкая» has none of them.
    for (const Settings *preset : {&medium, &high})
        CHECK(preset->tonemap && preset->tonemap_strength == 0.6f && preset->scene_grading && preset->grade == GradePreset::Caribbean &&
              preset->grade_strength <= 0.4f && (preset->lighting_flags & kLightingRim) && (preset->lighting_flags & kLightingAerial));
    CHECK(!medium.lens_flare && !medium.sun_shafts);
    CHECK(high.lens_flare && high.lens_flare_intensity == 0.5f && high.sun_shafts && high.sun_shafts_intensity == 0.35f);
    CHECK(!original.tonemap && !original.scene_grading && !original.lens_flare && !original.sun_shafts);
    Settings graded = original;
    graded.grade = GradePreset::Caribbean;
    CHECK(!graded.PostIsIdentity());
    graded.grade_strength = 0.0f;
    CHECK(graded.PostIsIdentity());
    CHECK(NameRu(QualityPreset::High) == "Высокая" && NameRu(GradePreset::Overcast) == "Пасмурно");
}

void CheckLut()
{
    // Identity: every texel is its own coordinate, and the trilinear fetch returns the input.
    const int n = kDefaultLutSize;
    const std::vector<uint8_t> identity = BuildLutRgba8(ColorGrade::Preset(GradePreset::Original), n);
    CHECK(identity.size() == static_cast<size_t>(n) * n * n * 4);
    bool exact = true;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const uint8_t *t = &identity[((static_cast<size_t>(b) * n + g) * n + r) * 4];
                const auto expect = [](int i) { return static_cast<int>(std::lround(i * 255.0f / (n - 1))); };
                exact = exact && t[0] == expect(r) && t[1] == expect(g) && t[2] == expect(b) && t[3] == 255;
            }
    CHECK(exact);
    for (float v : {0.0f, 0.1f, 0.37f, 0.5f, 0.93f, 1.0f})
    {
        const auto out = SampleLut(identity, n, {v, 1.0f - v, 0.5f * v});
        CHECK(Near(out[0], v, 0.003f) && Near(out[1], 1.0f - v, 0.003f) && Near(out[2], 0.5f * v, 0.003f));
    }

    const ColorGrade caribbean = ColorGrade::Preset(GradePreset::Caribbean);
    const ColorGrade overcast = ColorGrade::Preset(GradePreset::Overcast);
    CHECK(ColorGrade{}.IsIdentity() && !caribbean.IsIdentity() && !overcast.IsIdentity());

    // Black and white stay (nearly) black and white, greys stay monotonic.
    for (const ColorGrade &grade : {caribbean, overcast})
    {
        CHECK(grade.Apply({0.0f, 0.0f, 0.0f})[1] < 0.03f);
        CHECK(grade.Apply({1.0f, 1.0f, 1.0f})[1] > 0.93f);
        float previous = -1.0f;
        for (int i = 0; i <= 64; ++i)
        {
            const float v = static_cast<float>(i) / 64.0f;
            const auto out = grade.Apply({v, v, v});
            const float luma = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
            CHECK(luma >= previous - 1e-4f);
            previous = luma;
            for (float c : out)
                CHECK(c >= 0.0f && c <= 1.0f);
        }
    }
    // «Карибы» is warmer and more saturated, «Пасмурно» cooler and less saturated.
    const std::array<float, 3> sea{0.20f, 0.45f, 0.60f}, grey{0.5f, 0.5f, 0.5f};
    CHECK(Chroma(caribbean.Apply(sea)) > Chroma(sea) * 1.08f);
    CHECK(Chroma(overcast.Apply(sea)) < Chroma(sea) * 0.85f);
    CHECK(caribbean.Apply(grey)[0] > caribbean.Apply(grey)[2] + 0.02f);
    CHECK(overcast.Apply(grey)[2] > overcast.Apply(grey)[0] + 0.02f);
    // Neither preset changes mid-grey brightness by much.
    CHECK(Near(caribbean.Apply(grey)[1], 0.5f, 0.05f) && Near(overcast.Apply(grey)[1], 0.5f, 0.06f));
    // Interpolation: the end points are the presets, the middle is in between.
    const ColorGrade half = ColorGrade::Lerp(ColorGrade{}, caribbean, 0.5f);
    CHECK(Near(half.saturation, 0.5f * (1.0f + caribbean.saturation), 1e-6f));
    CHECK(ColorGrade::Lerp(ColorGrade{}, caribbean, 0.0f).IsIdentity());
    // A LUT of another size has the right number of texels.
    CHECK(BuildLutRgba8(caribbean, 16).size() == 16u * 16u * 16u * 4u);
}

void CheckToneCurve()
{
    // Mid grey keeps its brightness, the curve is monotonic and stays inside 0..1.
    const float grey = std::sqrt(0.18f);
    CHECK(Near(TonemapTransfer(grey), grey, 0.004f));
    float previous = -1.0f;
    for (int i = 0; i <= 100; ++i)
    {
        const float v = TonemapTransfer(static_cast<float>(i) / 100.0f);
        CHECK(v >= previous && v >= 0.0f && v <= 1.0f);
        previous = v;
    }
    // AgX shape: slightly darker shadows, white rolled off instead of clipped (the full curve takes it to about
    // 0.87, the default strength keeps it above 0.9).
    CHECK(TonemapTransfer(0.2f) < 0.2f);
    CHECK(TonemapTransfer(1.0f) > 0.85f && TonemapTransfer(1.0f) < 0.9f);
    CHECK(TonemapTransfer(1.0f, 1.0f, Settings{}.tonemap_strength) > 0.9f);
    // At the default strength shadows are only mildly deepened.
    const Settings defaults;
    CHECK(TonemapTransfer(0.1f, 1.0f, defaults.tonemap_strength) > 0.055f);
    CHECK(Near(TonemapTransfer(0.37f, 1.0f, 0.0f), 0.37f, 1e-5f));
    CHECK(Near(ExpandLdr(1.0f), 3.0f, 1e-4f) && Near(ExpandLdr(0.0f), 0.0f, 1e-6f));
}

void CheckNoise()
{
    for (int i = 0; i < 300; ++i)
    {
        const float u = 0.013f * i, v = 0.007f * i;
        CHECK(Near(PeriodicFbm(u, v, 4, 3, 9u), PeriodicFbm(u + 1.0f, v + 2.0f, 4, 3, 9u), 2e-3f));
        CHECK(std::fabs(PeriodicFbm(u, v, 4, 3, 9u)) <= 1.0f);
    }
}

void CheckSky()
{
    const auto sky_for = [](float sun_height, float overcast) {
        WeatherInput weather;
        const float c = std::sqrt(1.0f - sun_height * sun_height);
        weather.sun_dir = {c, sun_height, 0.0f};
        weather.sun_color = {0.86f, 0.82f, 0.78f};
        weather.ambient_color = {0.37f, 0.37f, 0.35f};
        weather.fog_color = {0.31f, 0.47f, 0.61f};
        weather.overcast = overcast;
        return ComputeSky(weather);
    };
    const SkyState noon = sky_for(0.85f, 0.0f);
    CHECK(noon.lut.size() == static_cast<size_t>(kSkyLutWidth) * kSkyLutHeight * 4);
    const Vec3 zenith = SkyColor(noon, {0.0f, 1.0f, 0.0f});
    const Vec3 horizon = SkyColor(noon, {-1.0f, 0.0f, 0.0f});
    // Clear day: blue zenith, brighter and paler horizon.
    CHECK(zenith[2] > zenith[0] * 1.8f && zenith[2] > zenith[1]);
    CHECK(horizon[0] + horizon[1] + horizon[2] > zenith[0] + zenith[1] + zenith[2]);
    CHECK(horizon[2] / std::max(horizon[0], 1e-4f) < zenith[2] / std::max(zenith[0], 1e-4f));
    CHECK(noon.night < 0.01f);

    // Sunset: the horizon towards the sun is warm, the opposite side is not; sunlight reddens.
    const SkyState sunset = sky_for(0.03f, 0.0f);
    const Vec3 towards = SkyColor(sunset, {0.999f, 0.04f, 0.0f});
    const Vec3 away = SkyColor(sunset, {-0.999f, 0.04f, 0.0f});
    CHECK(towards[0] > towards[2] * 1.15f);
    CHECK(towards[0] / std::max(towards[2], 1e-4f) > 1.3f * away[0] / std::max(away[2], 1e-4f));
    CHECK(sunset.sun_light[0] > sunset.sun_light[2] * 1.2f);
    CHECK(noon.sun_light[0] < noon.sun_light[2] * 1.5f);

    // Night: dark but not black, no direct light.
    const SkyState night = sky_for(-0.5f, 0.0f);
    const Vec3 night_zenith = SkyColor(night, {0.0f, 1.0f, 0.0f});
    CHECK(night.night > 0.99f);
    CHECK(night_zenith[2] > 0.002f && night_zenith[2] < 0.06f);
    CHECK(night.sun_light[0] < 1e-3f);

    // Overcast: grey, darker dome, weaker sun.
    const SkyState storm = sky_for(0.85f, 1.0f);
    const Vec3 storm_zenith = SkyColor(storm, {0.0f, 1.0f, 0.0f});
    CHECK(storm_zenith[2] / std::max(storm_zenith[0], 1e-4f) < 1.6f);
    CHECK(storm.sun_light[1] < noon.sun_light[1] * 0.5f);

    // `horizon` is the average of the horizon row.
    CHECK(noon.horizon[1] > 0.5f * horizon[1] && noon.horizon[1] < 2.0f * horizon[1]);
    // Below the horizon the panorama clamps to the horizon row.
    const Vec3 below = SkyColor(noon, {-0.8f, -0.6f, 0.0f});
    CHECK(Near(below[1], horizon[1], 1e-4f));

    WeatherInput a, b;
    CHECK(!SkyNeedsRebuild(a, b));
    b.wind = {0.0f, 1.0f};
    CHECK(!SkyNeedsRebuild(a, b));
    b.overcast = 0.5f;
    CHECK(SkyNeedsRebuild(a, b));
}

void CheckLighting()
{
    LightingInput input;
    input.sun_dir = {0.6f, 0.8f, 0.0f};
    input.sun_diffuse = {0.9f, 0.8f, 0.7f};
    input.ambient = {0.4f, 0.4f, 0.4f};
    input.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const LightingUniforms plain = ComputeLighting(input, LightingMaterial::Hull(), nullptr, Settings{});
    CHECK(Near(plain.u_fxLight[0][0], 0.6f, 1e-6f) && Near(plain.u_fxLight[0][1], 0.8f, 1e-6f));
    CHECK(Near(plain.u_fxLight[2][1], 1.0f, 1e-6f));
    // Sky and ground ambient average to the flat ambient the legacy pipeline used.
    for (int i = 0; i < 3; ++i)
        CHECK(Near(0.5f * (plain.u_fxLight[3][i] + plain.u_fxLight[4][i]), 0.4f, 1e-5f));
    CHECK(plain.u_fxLight[3][1] > plain.u_fxLight[4][1]);
    CHECK(plain.u_fxLight[0][3] == LightingMaterial::Hull().specular_power);

    // A rotated camera rotates the vectors, not their length.
    input.view = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 5, 6, 7, 1};
    const LightingUniforms rotated = ComputeLighting(input, LightingMaterial::Sail(), nullptr, Settings{});
    CHECK(Near(rotated.u_fxLight[0][2], 0.6f, 1e-6f) && Near(rotated.u_fxLight[0][1], 0.8f, 1e-6f));
    CHECK(rotated.u_fxLight[2][3] == LightingMaterial::Sail().translucency);
    // The sunlight through sails scales with Settings::sail_light (fx_sail_light) and only lights sails.
    Settings brighter;
    brighter.sail_light = 2.0f;
    CHECK(Near(ComputeLighting(input, LightingMaterial::Sail(), nullptr, brighter).u_fxLight[2][3],
               2.0f * LightingMaterial::Sail().translucency, 1e-6f));
    CHECK(ComputeLighting(input, LightingMaterial::Hull(), nullptr, brighter).u_fxLight[2][3] == 0.0f);

    // With a sky the upper hemisphere takes its hue (bluer than the lower one).
    SkyState sky;
    sky.dome = {0.2f, 0.4f, 0.8f};
    const LightingUniforms tinted = ComputeLighting(input, LightingMaterial::Character(), &sky, Settings{});
    // Relief scale: per material (the texture class sets the strength), scaled by the preset; characters at full scale
    // (their own strength, Settings::character_relief_strength, is applied per draw) and no material highlight.
    Settings half;
    half.normal_map_strength = 0.5f;
    CHECK(Near(ComputeLighting(input, LightingMaterial::Stone(), nullptr, half).u_fxLight[5][0], 0.5f, 1e-6f));
    CHECK(LightingMaterial::Stone().relief == LightingMaterial::Hull().relief && LightingMaterial::Hull().relief > LightingMaterial::Sail().relief);
    CHECK(tinted.u_fxLight[5][0] == 1.0f && LightingMaterial::Character().specular_strength == 0.0f);
    // Actual uniform boundary: Medium/High tuning reaches the shaders; Low carries zero desaturation.
    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High})
    {
        const Settings profile = Settings::FromPreset(preset);
        const auto light = ComputeLighting(input, LightingMaterial::Stone(), nullptr, profile);
        CHECK(Near(light.u_fxLight[5][0], preset == QualityPreset::Low ? 1.0f : 0.75f, 1e-6f));
        CHECK(Near(light.u_fxLight[5][2], preset == QualityPreset::Low ? 0.0f : 0.10f, 1e-6f));
        CHECK(light.u_fxLight[1][0] == input.sun_diffuse[0]); // never change exposure on the CPU
    }
    CHECK(tinted.u_fxLight[3][2] > tinted.u_fxLight[3][0]);
    CHECK(tinted.u_fxLight[4][0] > tinted.u_fxLight[4][2]);
    // Tinted too, sky and ground average to the flat ambient per channel; a deep blue night dome keeps the ground
    // light positive (the night characters stay as bright as without the hemisphere).
    SkyState night;
    night.dome = {0.0005f, 0.001f, 0.02f};
    const LightingUniforms dark = ComputeLighting(input, LightingMaterial::Character(), &night, Settings{});
    for (const LightingUniforms *u : {&tinted, &dark})
        for (int i = 0; i < 3; ++i)
        {
            CHECK(Near(0.5f * (u->u_fxLight[3][i] + u->u_fxLight[4][i]), 0.4f, 1e-5f));
            CHECK(u->u_fxLight[4][i] > 0.0f);
        }

    Settings s;
    CHECK(PointLightBudget(s) == 0);
    s.lighting_flags = kLightingPoint; // baked geometry: dynamic point lights only
    CHECK(PointLightBudget(s) == 4);
    // The programs' run-time switches (fx_lighting.sh): the soft sun filter on Высокая, linear light only when asked.
    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High})
    {
        Settings profile = Settings::FromPreset(preset);
        const auto light = ComputeLighting(input, LightingMaterial::Stone(), nullptr, profile);
        CHECK(light.u_fxLight[4][3] == (preset == QualityPreset::High ? 1.0f : 0.0f) && light.u_fxLight[5][3] == 0.0f);
        profile.lighting_flags |= kLightingLinear;
        CHECK(ComputeLighting(input, LightingMaterial::Stone(), nullptr, profile).u_fxLight[5][3] == 1.0f);
    }
    CHECK(PointLightBudget(Settings::FromPreset(QualityPreset::High)) == 8);
}

// Nothing is downgraded silently: every unsupported request is reported and fixed explicitly.
void CheckSupport()
{
    Support all;
    all.lut_3d = all.shadow_map = all.scene_depth = all.normal_map = true;
    all.lamp_depth = LampDepth::D24;
    all.msaa_samples = 4;
    all.msaa_depth = all.anisotropy = all.ao_volume = all.smaa = all.sea_waves_vertex = all.sea_waves_float = all.sea_waves_surface_float = true;
    for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High})
    {
        const Settings settings = Settings::FromPreset(preset);
        CHECK(ValidateSettings(settings, all).empty());
        std::vector<std::string> changes;
        CHECK(ConstrainSettings(settings, all, changes) == settings && changes.empty());
        // The original look never depends on a capability; only «Низкая»'s upscale needs the post chain, and
        // without it the scene is native and nothing else changes.
        if (preset == QualityPreset::Low)
        {
            changes.clear();
            Settings native = settings;
            native.render_height = 0;
            native.sharpen = false; // RCAS is part of the upscale
            CHECK(ConstrainSettings(settings, Support{}, changes) == native && changes.size() == 1);
            CHECK(ValidateSettings(native, Support{}).empty());
        }
    }
    const Settings high = Settings::FromPreset(QualityPreset::High);

    // No sampleable scene depth: the sea's soft edges off.
    Support no_depth = all;
    no_depth.scene_depth = false;
    // ... and the render scale: the 3D drawn after the scene needs the scene depth on the display-size picture;
    // the sea's turbidity reads the optical depth from it too.
    CHECK(ValidateSettings(high, no_depth).size() == 3);
    std::vector<std::string> changes;
    Settings constrained = ConstrainSettings(high, no_depth, changes);
    CHECK(!constrained.sea_soft_edges && constrained.sea_band_limit && constrained.render_height == 0);
    CHECK(high.sea_turbidity && !constrained.sea_turbidity);
    CHECK(changes.size() == 3);
    CHECK(constrained.shadows == high.shadows && constrained.bloom && constrained.lighting_flags == high.lighting_flags);
    CHECK(ValidateSettings(constrained, no_depth).empty());

    // No shadow maps.
    Support no_shadow = all;
    no_shadow.shadow_map = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_shadow, changes);
    CHECK(constrained.shadows == ShadowQuality::Off && constrained.lamp_shadows == high.lamp_shadows && changes.size() == 1); // sun
    // No 24-bit or float depth target: no lamp shadows (their faces' perspective depth), said once; the sun stays.
    Support no_lamp_depth = all;
    no_lamp_depth.lamp_depth = LampDepth::None;
    changes.clear();
    constrained = ConstrainSettings(high, no_lamp_depth, changes);
    CHECK(constrained.lamp_shadows == 0 && constrained.shadows == high.shadows && changes.size() == 1);
    // At most 8 lamps per pixel (the grid's cell); the presets: 3 on Высокая, 2 on Средняя.
    Settings crowded = high;
    crowded.lamp_per_pixel = 12;
    changes.clear();
    CHECK(ConstrainSettings(crowded, all, changes).lamp_per_pixel == 8 && changes.size() == 1);
    CHECK(high.lamp_per_pixel == 3 && Settings::FromPreset(QualityPreset::Medium).lamp_per_pixel == 2);
    // Filtered lamp shadows per pixel (the others one tap): 2 on Высокая, 1 on Средняя; 1..8.
    CHECK(high.lamp_filtered == 2 && Settings::FromPreset(QualityPreset::Medium).lamp_filtered == 1);
    crowded = high;
    crowded.lamp_filtered = 0;
    changes.clear();
    CHECK(ConstrainSettings(crowded, all, changes).lamp_filtered == 1 && changes.size() == 1);

    // No R8 3D textures: no AO volume, said once; the rest stays.
    Support no_ao = all;
    no_ao.ao_volume = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_ao, changes);
    // ... and no lamp grid or room light: no lamp shadows either, each said once.
    CHECK(constrained.ao_volume == 0 && constrained.lamp_shadows == 0 && changes.size() == 2 && constrained.shadows == high.shadows);
    // The slots of the lights that move are some of the lamp slots: at most two, never more than there are.
    Settings many_moving = high;
    many_moving.lamp_moving = 5;
    changes.clear();
    CHECK(ConstrainSettings(many_moving, all, changes).lamp_moving == 2 && changes.size() == 1);
    Settings no_lamps = high; // without lamp slots the number says nothing and is left alone
    no_lamps.lamp_shadows = 0;
    changes.clear();
    CHECK(ConstrainSettings(no_lamps, all, changes).lamp_moving == 2 && changes.empty());
    Settings five_lamps = high;
    five_lamps.lamp_shadows = 5;
    changes.clear();
    CHECK(ConstrainSettings(five_lamps, all, changes).lamp_shadows == 8 && changes.size() == 1);
    // An AO volume without the fx lighting path has no shader to apply it: switched off, said.
    Settings unlit = high;
    unlit.lighting_flags = kLightingNone;
    unlit.shadows = ShadowQuality::Off;
    unlit.lamp_shadows = 0;
    unlit.town_light = false;
    unlit.town_walls = false;
    unlit.gloss = false;
    unlit.parallax = ParallaxQuality::Off;
    unlit.parallax_shadow = false;
    unlit.relief_cavity = false;
    unlit.wet_surfaces = 0;
    unlit.character_relief = CharacterRelief::Off;
    unlit.flash_lights = 0;
    changes.clear();
    CHECK(ConstrainSettings(unlit, all, changes).ao_volume == 0 && changes.size() == 1);
    // Gloss without per-pixel highlights has nothing to shape: switched off, said.
    Settings matte = high;
    matte.lighting_flags &= ~static_cast<uint32_t>(kLightingSpecular);
    changes.clear();
    CHECK(!ConstrainSettings(matte, all, changes).gloss && changes.size() == 1);
    // Wet surfaces are drawn by the lit / baked programs: without the fx lighting they are switched off, said once.
    Settings unlit_wet = unlit;
    unlit_wet.ao_volume = 0;
    unlit_wet.wet_surfaces = 2;
    changes.clear();
    CHECK(ConstrainSettings(unlit_wet, all, changes).wet_surfaces == 0 && changes.size() == 1);
    Settings too_wet = high;
    too_wet.wet_surfaces = 7;
    changes.clear();
    CHECK(ConstrainSettings(too_wet, all, changes).wet_surfaces == 2 && changes.size() == 1);
    // Flash lights: none on «Низкая», 4 / 8 at once on Средняя / Высокая; without per-pixel point lights none, said.
    CHECK(Settings::FromPreset(QualityPreset::Low).flash_lights == 0 && Settings::FromPreset(QualityPreset::Medium).flash_lights == 4 &&
          high.flash_lights == 8);
    Settings no_points = high;
    no_points.lighting_flags &= ~static_cast<uint32_t>(kLightingPoint);
    changes.clear();
    CHECK(ConstrainSettings(no_points, all, changes).flash_lights == 0 && changes.size() == 1);
    Settings too_strong = high;
    too_strong.ao_volume = 9;
    changes.clear();
    CHECK(ConstrainSettings(too_strong, all, changes).ao_volume == kAoVolumeLevels && changes.size() == 1);
    // Our own town light relights through the fx lighting path: without it, switched off, said.
    Settings flat_town = high;
    flat_town.lighting_flags = kLightingNone;
    flat_town.shadows = ShadowQuality::Off;
    flat_town.lamp_shadows = 0;
    flat_town.ao_volume = 0;
    flat_town.town_walls = false;
    flat_town.gloss = false;
    flat_town.parallax = ParallaxQuality::Off;
    flat_town.parallax_shadow = false;
    flat_town.relief_cavity = false;
    flat_town.wet_surfaces = 0;
    flat_town.character_relief = CharacterRelief::Off;
    flat_town.flash_lights = 0;
    changes.clear();
    CHECK(!ConstrainSettings(flat_town, all, changes).town_light && changes.size() == 1);

    // «Низкая» makes nothing in the background but the far LODs (and even on a device that supports everything);
    // Высокая every product. What the gates of the renderer read (effects/), and its log line.
    {
        std::vector<std::string> low_changes;
        const BackgroundProducts low = BackgroundProductsFor(ConstrainSettings(Settings::FromPreset(QualityPreset::Low), all, low_changes));
        BackgroundProducts far_only;
        far_only.far_lods = true;
        CHECK(low == far_only);
        CHECK(low.Describe() == "far LODs");
        CHECK(BackgroundProducts{}.Describe() == "none");
        const BackgroundProducts top = BackgroundProductsFor(high);
        CHECK(top.far_lods && top.visibility && top.ao_volume && top.town_light && top.town_walls && top.lamp_shadows &&
              top.sun_shadows && top.relief && top.gloss && !top.upscale);
        // «Качество текстур»: the neural ×2 follows the texture level alone, on every preset; nothing else changes.
        CHECK(TextureDegradation(TextureQuality::Low) == 2 && TextureDegradation(TextureQuality::Medium) == 1 &&
              TextureDegradation(TextureQuality::High) == 0 && TextureDegradation(TextureQuality::Ultra) == 0);
        for (const QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High})
        {
            CHECK(!Settings::FromPreset(preset).upscale);
            const BackgroundProducts base = BackgroundProductsFor(Settings::FromPreset(preset));
            for (int level = 0; level < kTextureQualityLevels; ++level)
            {
                Settings textured = Settings::FromPreset(preset);
                ApplyTextureQuality(textured, static_cast<TextureQuality>(level));
                BackgroundProducts products = BackgroundProductsFor(textured);
                CHECK(products.upscale == (level == static_cast<int>(TextureQuality::Ultra)));
                products.upscale = base.upscale;
                CHECK(products == base);
            }
        }
        Settings low_ultra = Settings::FromPreset(QualityPreset::Low);
        ApplyTextureQuality(low_ultra, TextureQuality::Ultra);
        BackgroundProducts far_and_x2 = far_only;
        far_and_x2.upscale = true;
        CHECK(BackgroundProductsFor(low_ultra) == far_and_x2 && BackgroundProductsFor(low_ultra).Describe() == "far LODs, upscale x2");
        // What needs the lit draws goes without them: no visibility volume.
        Settings unlit = high;
        unlit.lighting_flags = kLightingNone;
        const BackgroundProducts dark = BackgroundProductsFor(unlit);
        CHECK(!dark.visibility && !dark.relief && !dark.gloss);
    }

    // No 3D textures: the post chain is reduced to its plain copy; MSAA is the renderer's and stays.
    Support no_lut = all;
    no_lut.lut_3d = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_lut, changes);
    CHECK(!constrained.tonemap && !constrained.bloom && !constrained.sun_shafts && !constrained.dither);
    CHECK(constrained.msaa == 4 && constrained.PostIsIdentity());
    CHECK(ValidateSettings(constrained, no_lut).empty());

    // MSAA without depth resolve: the multisampled scene keeps MSAA and gives up what needs depth.
    Support no_msaa_depth = all;
    no_msaa_depth.msaa_depth = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_msaa_depth, changes);
    CHECK(constrained.msaa == 4 && !constrained.sea_soft_edges && constrained.render_height == 0 &&
          !constrained.sea_turbidity && changes.size() == 3);
    // Only 2x for the scene target formats (per-format caps): 4x is lowered to 2x, reported, alpha to coverage stays.
    Support msaa_x2 = all;
    msaa_x2.msaa_samples = 2;
    changes.clear();
    constrained = ConstrainSettings(high, msaa_x2, changes);
    CHECK(constrained.msaa == 2 && constrained.alpha_to_coverage && !constrained.smaa && changes.size() == 1 &&
          changes[0] == "MSAA 4x is not available for the scene target (RGBA8 + D24S8): 2x instead");
    CHECK(ValidateSettings(constrained, msaa_x2).empty() && !ValidateSettings(high, msaa_x2).empty());
    // No MSAA at all: alpha to coverage goes with it, and then depth-based effects are fine again.
    Support no_msaa = no_msaa_depth;
    no_msaa.msaa_samples = 1;
    changes.clear();
    constrained = ConstrainSettings(high, no_msaa, changes);
    CHECK(constrained.msaa == 1 && !constrained.alpha_to_coverage && constrained.sea_soft_edges);
    // ... and SMAA takes over the anti-aliasing, reported (PowerVR, minimal GLES, a failed MSAA self-test).
    CHECK(constrained.smaa && !high.smaa);
    CHECK(std::find(changes.begin(), changes.end(), std::string("MSAA unavailable: SMAA instead")) != changes.end());
    CHECK(ValidateSettings(constrained, no_msaa).empty());
    // Without the post chain there is no SMAA either: MSAA goes, reported as no anti-aliasing at all.
    const std::string no_aa =
        "MSAA unavailable and SMAA needs the post-processing chain (3D textures) and R8/RG8 textures: no anti-aliasing";
    Support no_msaa_no_lut = no_msaa;
    no_msaa_no_lut.lut_3d = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_msaa_no_lut, changes);
    CHECK(constrained.msaa == 1 && !constrained.smaa && constrained.PostIsIdentity() && constrained.render_height == 0);
    CHECK(std::find(changes.begin(), changes.end(), no_aa) != changes.end());
    // ... nor without its tables: SMAA asked for explicitly is switched off, reported.
    Support no_msaa_no_r8 = no_msaa;
    no_msaa_no_r8.smaa = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_msaa_no_r8, changes);
    CHECK(constrained.msaa == 1 && !constrained.smaa && std::find(changes.begin(), changes.end(), no_aa) != changes.end());
    Settings smaa_on = high;
    smaa_on.smaa = true;
    changes.clear();
    CHECK(!ConstrainSettings(smaa_on, no_msaa_no_r8, changes).smaa &&
          std::find(changes.begin(), changes.end(), std::string("SMAA needs R8 and RG8 textures: switched off")) != changes.end());
    // An explicit MSAA off (fx_msaa=1) is the player's choice, not a missing capability: no SMAA is added.
    Settings msaa_off = high;
    msaa_off.msaa = 1;
    msaa_off.alpha_to_coverage = false;
    changes.clear();
    CHECK(!ConstrainSettings(msaa_off, no_msaa, changes).smaa && changes.empty());
    // The internal height's range; the upscale is part of the post chain (3D textures), the sharpening part of the
    // upscale: neither makes the post effects non-identical by itself.
    Settings scaled = high;
    scaled.render_height = 100;
    changes.clear();
    CHECK(ConstrainSettings(scaled, all, changes).render_height == 360 && changes.size() == 1);
    scaled.render_height = 1000;
    Settings only_scaled;
    only_scaled.render_height = 1000;
    only_scaled.sharpen = true;
    CHECK(Settings{}.PostIsIdentity() && only_scaled.PostIsIdentity());
    CHECK(RendererStateFor(scaled).render_height == 1000);
    changes.clear();
    CHECK(ConstrainSettings(only_scaled, no_lut, changes).render_height == 0 && !changes.empty());
    const RendererState state = RendererStateFor(high);
    CHECK(state.msaa_samples == 4 && state.alpha_to_coverage && state.max_anisotropy == 8);
    CHECK(RendererStateFor(Settings{}).msaa_samples == 1 && !RendererStateFor(Settings{}).alpha_to_coverage);
    Support no_aniso = all;
    no_aniso.anisotropy = false;
    changes.clear();
    CHECK(ConstrainSettings(high, no_aniso, changes).anisotropy == 1 && changes.size() == 1);

    // No EAC textures: normal maps and gloss maps go, and so does the shader define.
    Support no_rg = all;
    no_rg.normal_map = false;
    changes.clear();
    constrained = ConstrainSettings(high, no_rg, changes);
    // Gloss, the parallax, its self-shadow and the cavity go with them.
    CHECK(changes.size() == 6 && !constrained.gloss && !constrained.parallax_shadow && !constrained.relief_cavity &&
          constrained.character_relief == CharacterRelief::Off &&
          !(constrained.lighting_flags & kLightingNormalMap) && constrained.parallax == ParallaxQuality::Off);
    CHECK(high.parallax == ParallaxQuality::Occlusion && Settings::FromPreset(QualityPreset::Medium).parallax == ParallaxQuality::Offset &&
          Settings::FromPreset(QualityPreset::Low).parallax == ParallaxQuality::Off);
    // The self-shadow on Высокая only (it needs occlusion mapping); the classes' depths: stone deepest, wood shallowest.
    CHECK(high.parallax_shadow && !Settings::FromPreset(QualityPreset::Medium).parallax_shadow);
    // The characters' relief: with their parallax on Высокая, their normal maps alone on Средняя, none on Низкая; at half
    // the relief strength. Without the parallax their normal maps stay.
    CHECK(high.character_relief == CharacterRelief::Parallax &&
          Settings::FromPreset(QualityPreset::Medium).character_relief == CharacterRelief::Normals &&
          Settings::FromPreset(QualityPreset::Low).character_relief == CharacterRelief::Off &&
          high.character_relief_strength == 0.5f);
    {
        Settings flat = high;
        flat.parallax = ParallaxQuality::Off;
        flat.parallax_shadow = false;
        changes.clear();
        CHECK(ConstrainSettings(flat, all, changes).character_relief == CharacterRelief::Normals && changes.size() == 1);
    }
    {
        // The characters' skin colour (skin_tone.hpp): a face of warm texels around one colour, with brown hair beside it;
        // its own skin passes, blue cloth, grey steel and black do not, and a grey face is no reference.
        std::vector<uint8_t> face;
        for (int i = 0; i < 200; ++i)
        {
            const int d = (i * 7) % 21 - 10;
            const uint8_t skin[3] = {static_cast<uint8_t>(190 + d), static_cast<uint8_t>(130 + d / 2), static_cast<uint8_t>(100 + d / 3)};
            const uint8_t hair[3] = {70, 45, 25};
            face.insert(face.end(), skin, skin + 3);
            if (i % 3 == 0)
                face.insert(face.end(), hair, hair + 3);
        }
        const SkinTone tone = MakeSkinTone(face);
        CHECK(tone.valid && SkinLikeness(tone, 190 / 255.0f, 130 / 255.0f, 100 / 255.0f) > 0.9f &&
              SkinLikeness(tone, 60 / 255.0f, 80 / 255.0f, 150 / 255.0f) == 0.0f &&
              SkinLikeness(tone, 120 / 255.0f, 120 / 255.0f, 125 / 255.0f) == 0.0f && SkinLikeness(tone, 0.0f, 0.0f, 0.0f) == 0.0f);
        CHECK(PackSkinTone(tone)[7] == 1.0f && PackSkinTone(SkinTone{})[0] == -1.0f);
        CHECK(TorsoSkinMask(SkinTone{}, 1.0f, 0.7f, 0.5f, 0.3f) == 1.0f &&
              TorsoSkinMask(SkinTone{}, 0.25f, 0.7f, 0.5f, 0.3f) == 0.5f &&
              TorsoSkinMask(SkinTone{}, 0.0f, 0.7f, 0.5f, 0.3f) == 0.0f &&
              TorsoSkinMask(tone, 1.0f, 190 / 255.0f, 130 / 255.0f, 100 / 255.0f) > 0.9f &&
              TorsoSkinMask(tone, 1.0f, 60 / 255.0f, 80 / 255.0f, 150 / 255.0f) == 0.0f);
        std::vector<uint8_t> grey;
        for (int i = 0; i < 100; ++i)
        {
            const uint8_t g[3] = {static_cast<uint8_t>(130 + i % 5), static_cast<uint8_t>(128 + i % 5), static_cast<uint8_t>(126 + i % 5)};
            grey.insert(grey.end(), g, g + 3);
        }
        CHECK(!MakeSkinTone(grey).valid && !MakeSkinTone(std::span<const uint8_t>(face.data(), 30)).valid);
        std::vector<uint8_t> red_paint;
        for (int i = 0; i < 100; ++i)
            red_paint.insert(red_paint.end(), {255, 20, 0});
        const SkinTone paint = MakeSkinTone(red_paint);
        CHECK((!paint.valid && std::isfinite(paint.r) && std::isfinite(paint.g) &&
               std::isfinite(paint.inv_rr) && PackSkinTone(paint)[0] == -1.0f));
    }
    CHECK(ClassifyTextureName("characters/blaze").relief == ReliefClass::Character && ReliefHasHeight(ReliefClass::Character) &&
          ReliefDepthTexels(ReliefClass::Character) == 2.0f);
    CHECK(high.relief_cavity && Settings::FromPreset(QualityPreset::Medium).relief_cavity &&
          !Settings::FromPreset(QualityPreset::Low).relief_cavity);
    CHECK(ReliefDepthTexels(ReliefClass::Stone) > ReliefDepthTexels(ReliefClass::Building) &&
          ReliefDepthTexels(ReliefClass::Building) > ReliefDepthTexels(ReliefClass::Plaster) &&
          ReliefDepthTexels(ReliefClass::Plaster) > ReliefDepthTexels(ReliefClass::Wood) &&
          ReliefDepthTexels(ReliefClass::Wood) > ReliefDepthTexels(ReliefClass::Prop) &&
          ReliefDepthTexels(ReliefClass::Prop) > ReliefDepthTexels(ReliefClass::Metal) &&
          ReliefDepthTexels(ReliefClass::Metal) > 0.0f && ReliefDepthTexels(ReliefClass::HeightMap) > 0.0f);
    // Medium and High soften generated normals and added light; Low retains the legacy strengths and colour.
    CHECK(Settings::FromPreset(QualityPreset::Low).normal_map_strength == 1.0f &&
          Settings::FromPreset(QualityPreset::Medium).normal_map_strength == 0.75f && high.normal_map_strength == 0.75f);
    CHECK(Settings::FromPreset(QualityPreset::Low).light_saturation == 1.0f &&
          Settings::FromPreset(QualityPreset::Medium).light_saturation == 0.90f && high.light_saturation == 0.90f);

    // Inconsistent requests are reported as well.
    Settings odd;
    odd.sun_shafts = true; // without bloom
    odd.lighting_flags = kLightingSpecular; // without per-pixel lighting
    odd.shadows = ShadowQuality::Soft;
    CHECK(ValidateSettings(odd, all).size() == 3);
    changes.clear();
    constrained = ConstrainSettings(odd, all, changes);
    CHECK(changes.size() == 3 && constrained == Settings{});
    CHECK(Support{}.Describe().find("NO") != std::string::npos && all.Describe().find("NO") == std::string::npos);
}

void CheckLightFalloff()
{
    // The game's lights.ini lamp, candle and torch: their light in linear light as the relight had it (the D3D
    // attenuation squared, windowed at the distance where it falls to kGameLightFloor), against the new falloff.
    struct Game
    {
        float range, att0, att1, att2;
    };
    const auto game = [](const Game &g, float d, float reach) {
        const float x = std::min(d / reach, 1.0f);
        const float w = (1.0f - x * x * x * x) * (1.0f - x * x * x * x);
        const float a = w / std::max(g.att0 + g.att1 * std::max(d, kGameLightNear) + g.att2 * std::max(d, kGameLightNear) * std::max(d, kGameLightNear), 1e-3f);
        return a * a;
    };
    const LightFalloff falloff;
    CHECK(falloff.reach == 1.6f && falloff.core == 0.4f && falloff.level == 0.85f); // the chosen look
    for (const Game g : {Game{40.0f, 0.2f, 0.8f, 0.05f}, Game{15.0f, 0.5f, 0.0f, 1.5f}, Game{40.0f, 1.2f, 0.9f, 0.0f},
                         Game{32.0f, 0.1f, 3.0f, 0.3f}, Game{55.0f, 0.9f, 0.2f, 0.02f}})
    {
        const LightShape shape = ShapeOfGameLight(g.range, g.att0, g.att1, g.att2, falloff);
        const float reach = shape.range / falloff.reach; // the game light's own reach
        // It ends further than the game's light, at most its reach times `reach`, and nowhere beyond.
        CHECK(reach > 0.5f && reach <= std::min(g.range, kGameLightReach) + 1e-3f);
        CHECK(LightFalloffAt(shape, shape.range) == 0.0f && LightFalloffAt(shape, 0.95f * reach) > 0.0f);
        CHECK(LightFalloffAt(shape, 1.1f * reach) > 0.0f); // the game's light was gone there
        // `level` times the game's in the middle of its reach; never hotter than that next to the flame.
        const float match = kMatchShare * reach;
        CHECK(Near(LightFalloffAt(shape, match), falloff.level * game(g, match, reach), 1e-3f * game(g, match, reach) + 1e-7f));
        CHECK(LightFalloffAt(shape, 0.0f) <= falloff.level * game(g, kGameLightNear, reach) * 1.001f);
        CHECK(shape.core >= falloff.core);
        // Falls with the distance; the square root is the gamma-space value.
        float previous = 1e30f;
        for (int i = 0; i <= 200; ++i)
        {
            const float a = LightFalloffAt(shape, 0.1f * i);
            CHECK(a <= previous && a >= 0.0f);
            previous = a;
        }
        CHECK(Near(LightFalloffGammaAt(shape, 1.0f), std::sqrt(LightFalloffAt(shape, 1.0f)), 1e-6f));
    }
    // The lamp of lights.ini: less than half as hot right by the flame, about as strong at 1-2 m, further reaching.
    {
        const Game lamp{40.0f, 0.2f, 0.8f, 0.05f};
        const LightShape shape = ShapeOfGameLight(lamp.range, lamp.att0, lamp.att1, lamp.att2, falloff);
        const float reach = shape.range / falloff.reach;
        std::printf("light falloff: lamp reach %.2f -> %.2f m, core %.2f m; at 0.15 m %.3f (game %.3f), 1 m %.3f (%.3f), 4 m %.4f (%.4f), 8 m %.4f (%.4f)\n",
                    reach, shape.range, shape.core, LightFalloffAt(shape, 0.15f), game(lamp, 0.15f, reach), LightFalloffAt(shape, 1.0f),
                    game(lamp, 1.0f, reach), LightFalloffAt(shape, 4.0f), game(lamp, 4.0f, reach), LightFalloffAt(shape, 8.0f),
                    game(lamp, 8.0f, reach));
        CHECK(LightFalloffAt(shape, 0.15f) < 0.5f * game(lamp, 0.15f, reach));
        CHECK(LightFalloffAt(shape, 4.0f) > game(lamp, 4.0f, reach) && LightFalloffAt(shape, 8.0f) > 0.0f && game(lamp, 8.0f, reach) == 0.0f);
    }
    // A longer reach, a larger core, a lower level each do what they say.
    {
        const LightShape base = ShapeOfGameLight(40.0f, 0.2f, 0.8f, 0.05f, falloff);
        const LightShape far = ShapeOfGameLight(40.0f, 0.2f, 0.8f, 0.05f, {2.4f, 0.4f, 0.85f});
        const LightShape soft = ShapeOfGameLight(40.0f, 0.2f, 0.8f, 0.05f, {1.6f, 0.8f, 0.85f});
        const LightShape dim = ShapeOfGameLight(40.0f, 0.2f, 0.8f, 0.05f, {1.6f, 0.4f, 0.5f});
        CHECK(far.range > base.range && LightFalloffAt(far, 0.9f * base.range) > LightFalloffAt(base, 0.9f * base.range));
        CHECK(soft.core > base.core && LightFalloffAt(soft, 0.0f) < LightFalloffAt(base, 0.0f));
        CHECK(LightFalloffAt(dim, 2.0f) < LightFalloffAt(base, 2.0f));
    }
    // A light that gives nothing lights nothing.
    CHECK(ShapeOfGameLight(10.0f, 0.0f, 0.0f, 0.0f, falloff).range == 0.0f && ShapeOfGameLight(0.0f, 1.0f, 0.0f, 0.0f, falloff).range == 0.0f);
}

void CheckFlashes();

void CheckLights()
{
    PointLight torch;
    torch.position = {0.0f, 2.0f, 0.0f};
    torch.color = {1.0f, 0.6f, 0.3f};
    torch.range = 10.0f;
    torch.intensity = 1.0f;
    torch.core = 0.4f;
    // The falloff of its shape inside the radius, nothing beyond it.
    CHECK(Near(LightAttenuation(torch, 0.0f), 1.0f / 0.4f, 1e-5f));
    CHECK(LightAttenuation(torch, 10.01f) == 0.0f && LightAttenuation(torch, 10.0f) == 0.0f);
    float previous = 1e9f;
    for (int i = 0; i <= 100; ++i)
    {
        const float a = LightAttenuation(torch, 0.1f * i);
        CHECK(a <= previous && a >= 0.0f);
        previous = a;
    }

    // Selection: strongest at the object first, unreachable lights dropped, the overflow counted.
    std::vector<PointLight> lights;
    for (int i = 0; i < 12; ++i)
    {
        PointLight light = torch;
        light.position = {3.0f * i, 2.0f, 0.0f};
        light.baked = i % 2 == 0;
        lights.push_back(light);
    }
    const LightSelection four = SelectLights(lights, {0.0f, 1.0f, 0.0f}, 1.0f, 4);
    CHECK(four.count == 4 && four.dropped == 0); // lights at x = 0, 3, 6, 9 reach; x = 12 and beyond do not
    CHECK(four.lights[0].position[0] == 0.0f && four.lights[3].position[0] == 9.0f);
    const LightSelection crowded = SelectLights(lights, {15.0f, 1.0f, 0.0f}, 6.0f, 4);
    CHECK(crowded.count == 4 && crowded.dropped > 0);
    CHECK(SelectLights(lights, {15.0f, 1.0f, 0.0f}, 6.0f, 8).dropped < crowded.dropped);
    CHECK(SelectLights(lights, {500.0f, 0.0f, 0.0f}, 1.0f, 8).count == 0);
    CHECK(SelectLights(lights, {0.0f, 1.0f, 0.0f}, 1.0f, 100).count <= kMaxPointLights);
    // Geometry with baked vertex colours: baked lights are left out, dynamic ones stay.
    const LightSelection dynamic = SelectLights(lights, {0.0f, 1.0f, 0.0f}, 1.0f, 8, false);
    CHECK(dynamic.count == 2 && !dynamic.lights[0].baked && !dynamic.lights[1].baked);
    // A brighter light further away can outrank a dim one nearby.
    lights[3].color = {64.0f, 64.0f, 64.0f};
    CHECK(SelectLights(lights, {0.0f, 1.0f, 0.0f}, 1.0f, 1).lights[0].position[0] == 9.0f);

    // A wall of a large baked mesh next to a torch: chosen at the wall's own sphere, the torch lights it, and the
    // choice does not depend on anything else (there is no camera in it: the renderer passes the draw's bounds).
    {
        PointLight wall_torch = torch;
        wall_torch.position = {40.0f, 2.0f, 0.0f};
        std::vector<PointLight> town(6, torch);
        for (int i = 0; i < 6; ++i)
            town[i].position = {-60.0f + 4.0f * i, 2.0f, 0.0f}; // far from the wall
        town.push_back(wall_torch);
        const LightSelection wall = SelectLights(town, {41.0f, 2.0f, 0.0f}, 3.0f, 4, true, true);
        CHECK(wall.count == 1 && wall.lights[0].position[0] == 40.0f && wall.lights[0].color[0] == torch.color[0]);
    }
    // Rank fade: as a left-out light catches up with the weakest kept one, the kept one dims to nothing, so the
    // swap of the two is continuous; with a clear lead it keeps its full colour.
    {
        PointLight a = torch, b = torch;
        a.position = {0.0f, 0.0f, 0.0f};
        b.position = {0.0f, 0.0f, 0.0f};
        a.color = {1.0f, 1.0f, 1.0f};
        float previous_weight = 1.0f;
        for (int step = 0; step <= 10; ++step)
        {
            b.color = {0.1f * step, 0.1f * step, 0.1f * step}; // catches up with a
            const PointLight pair[2] = {a, b};
            const LightSelection one = SelectLights(pair, {0.0f, 0.0f, 0.0f}, 0.0f, 1, true, true);
            CHECK(one.count == 1 && one.lights[0].color[0] <= previous_weight + 1e-6f);
            previous_weight = one.lights[0].color[0];
            if (step <= 4)
                CHECK(Near(one.lights[0].color[0], 1.0f, 1e-6f)); // b at most 40 %: a keeps all of its light
        }
        CHECK(previous_weight < 1e-6f); // equal scores: the kept light is faded out completely
        const PointLight pair[2] = {a, b};
        CHECK(SelectLights(pair, {0.0f, 0.0f, 0.0f}, 0.0f, 1).lights[0].color[0] == 1.0f); // fade off: unchanged
    }

    // Uniform packing: view-space position, the shape, spot cone as scale and bias.
    std::array<float, 16> view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -3, 1};
    LightSelection selection;
    selection.count = 2;
    selection.dropped = 3;
    selection.lights[0] = torch;
    selection.lights[1] = torch;
    selection.lights[1].direction = {0.0f, -1.0f, 0.0f};
    selection.lights[1].cos_inner = 0.9f;
    selection.lights[1].cos_outer = 0.7f;
    selection.lights[1].falloff = 2.0f;
    const PointLightUniforms packed = PackPointLights(selection, view);
    CHECK(packed.u_fxPointCount[0] == 2.0f && packed.dropped == 3);
    CHECK(Near(packed.u_fxPoint[0][0], -1.0f, 1e-6f) && Near(packed.u_fxPoint[0][1], 0.0f, 1e-6f) && packed.u_fxPoint[0][3] == 10.0f);
    CHECK(packed.u_fxPoint[1][3] == 1.0f && Near(packed.u_fxPoint[2][0], 0.16f, 1e-6f) && packed.u_fxPoint[2][1] == 0.0f);
    CHECK(packed.u_fxPoint[2][2] == 0.0f && packed.u_fxPoint[2][3] == 1.0f); // point light: cone factor is always 1
    const float scale = packed.u_fxPoint[6][2], bias = packed.u_fxPoint[6][3];
    CHECK(Near(0.9f * scale + bias, 1.0f, 1e-4f) && Near(0.7f * scale + bias, 0.0f, 1e-4f) && packed.u_fxPoint[7][3] == 2.0f);

    CheckFlashes();
}

void CheckFlashes()
{
    // Envelope: rises over ~30 ms, then (1 - t / decay)² to nothing; decays of 0.15..0.4 s by kind.
    const FlashProfile &gun = FlashProfileOf(FlashKind::Gunshot);
    const FlashProfile &cannon = FlashProfileOf(FlashKind::Cannon);
    const FlashProfile &ship = FlashProfileOf(FlashKind::ShipExplosion);
    CHECK(gun.rise <= 0.04f && cannon.rise <= 0.04f && ship.rise <= 0.04f);
    CHECK(gun.decay >= 0.15f && gun.decay < cannon.decay && cannon.decay < ship.decay && ship.decay <= 0.4f);
    CHECK(FlashEnvelope(cannon, 0.0f) == 0.0f && Near(FlashEnvelope(cannon, 0.5f * cannon.rise), 0.5f, 1e-5f));
    CHECK(Near(FlashEnvelope(cannon, cannon.rise), 1.0f, 1e-5f));
    CHECK(Near(FlashEnvelope(cannon, cannon.rise + 0.5f * cannon.decay), 0.25f, 1e-4f));
    CHECK(FlashEnvelope(cannon, cannon.rise + cannon.decay) == 0.0f);
    const LightFalloff falloff;
    for (int k = 0; k < kFlashKinds; ++k)
    {
        // Every kind lights as far as its range (the game light's reach times the falloff's), and nowhere beyond; the
        // wide ones are the narrower light scaled up, not cut at the game lights' reach.
        const FlashProfile &p = FlashProfileOf(static_cast<FlashKind>(k));
        const LightShape shape = FlashShapeOf(p, falloff);
        CHECK(shape.range > 0.5f * p.range && shape.range < falloff.reach * p.range);
        CHECK(LightFalloffAt(shape, shape.range) == 0.0f && LightFalloffAt(shape, 0.5f * p.range) > 0.0f);
    }
    {
        // Scaled in space: the ship explosion at 3 times the distance is what a 20 m flash is at the distance.
        const FlashProfile wide{{1.0f, 1.0f, 1.0f}, 60.0f, 0.03f, 0.2f}, base{{1.0f, 1.0f, 1.0f}, 20.0f, 0.03f, 0.2f};
        const LightShape a = FlashShapeOf(wide, falloff), b = FlashShapeOf(base, falloff);
        CHECK(Near(a.range, 3.0f * b.range, 1e-3f) && Near(LightFalloffAt(a, 15.0f), LightFalloffAt(b, 5.0f), 1e-5f));
    }
    const float cannon_range = FlashShapeOf(cannon, falloff).range, ship_range = FlashShapeOf(ship, falloff).range;

    const Vec3 eye{0.0f, 0.0f, 0.0f};
    std::vector<PointLight> out;
    const auto lit = [&](const FlashLights &f) {
        out.clear();
        f.AppendTo(out, falloff);
        return out.size();
    };
    {
        // Dark in the frame it is reported, lit from the next one (it ages with the frame it was reported in), at its
        // peak after the rise, gone after rise + decay.
        FlashLights f;
        f.Add(FlashKind::Cannon, {3.0f, 0.0f, 0.0f});
        CHECK(lit(f) == 0);
        f.Update(0.016f, eye, 4); // the frame it was reported in is presented
        CHECK(lit(f) == 1 && Near(out[0].color[0], cannon.color[0] * 0.016f / cannon.rise, 1e-3f));
        f.Update(cannon.rise - 0.016f, eye, 4);
        CHECK(lit(f) == 1 && Near(out[0].color[0], cannon.color[0], 1e-4f) && out[0].range == cannon_range && !out[0].baked);
        f.Update(cannon.decay, eye, 4);
        CHECK(f.Count() == 0 && lit(f) == 0);
        // Cap 0 («Низкая»): nothing lights.
        f.Add(FlashKind::Cannon, {3.0f, 0.0f, 0.0f});
        f.Update(0.0f, eye, 0);
        f.Update(0.02f, eye, 0);
        CHECK(lit(f) == 0);
    }
    {
        // The same event reported twice in one frame (ShipExplode + blast_inv):
        // one flash, the stronger kind.
        FlashLights f;
        f.Add(FlashKind::Explosion, {10.0f, 0.0f, 0.0f});
        f.Add(FlashKind::ShipExplosion, {10.2f, 0.0f, 0.0f});
        f.Add(FlashKind::Cannon, {10.0f, 0.1f, 0.0f});
        CHECK(f.Count() == 1);
        f.Update(0.0f, eye, 4);
        f.Update(0.04f, eye, 4);
        CHECK(lit(f) == 1 && out[0].range == ship_range);
        f.Add(FlashKind::Cannon, {10.0f, 0.0f, 0.0f}); // a later shot at the same place is a new flash
        CHECK(f.Count() == 2);
    }
    {
        // A broadside of 12 guns with cap 4: the 4 strongest at the camera light at once, no more; the cap keeps the
        // nearest, and when one of them drops out it fades (never a one-frame cut).
        FlashLights f;
        for (int gun = 0; gun < 12; ++gun)
            f.Add(FlashKind::Cannon, {2.0f + 3.0f * gun, 0.0f, 10.0f});
        f.Update(0.0f, eye, 4);
        CHECK(f.Lit() == 4);
        f.Update(0.016f, eye, 4);
        CHECK(lit(f) == 4);
        float nearest = 1e9f, farthest = 0.0f;
        for (const PointLight &l : out)
        {
            nearest = std::min(nearest, l.position[0]);
            farthest = std::max(farthest, l.position[0]);
        }
        CHECK(nearest == 2.0f && farthest == 11.0f);
        // A ship explosion close by takes a place in the cap: the weakest of the four fades over kCapFade.
        f.Add(FlashKind::ShipExplosion, {0.0f, 0.0f, 5.0f});
        f.Update(0.016f, eye, 4);
        CHECK(f.Lit() == 5 && lit(f) == 5); // the explosion and four guns, one of them fading out
        float faded = 1.0f;
        for (const PointLight &l : out)
            if (l.position[0] == 11.0f)
                faded = l.color[0] / (cannon.color[0] * FlashEnvelope(cannon, 0.032f));
        CHECK(faded > 0.6f && faded < 0.9f); // 1 - 16 ms / 80 ms
        f.Update(FlashLights::kCapFade, eye, 4);
        CHECK(lit(f) == 4 && f.Lit() == 4);
        bool explosion = false;
        for (const PointLight &l : out)
            explosion |= l.range == ship_range;
        CHECK(explosion);
    }
    {
        // Never more than the pool holds; a full pool replaces its weakest flash.
        FlashLights f;
        for (int i = 0; i < 50; ++i)
        {
            f.Add(FlashKind::Gunshot, {static_cast<float>(i), 0.0f, 0.0f});
            f.Update(0.001f, eye, 8);
        }
        CHECK(f.Count() == FlashLights::kCapacity);
        CHECK(f.Lit() <= 8 + 1);
    }
}

void CheckShadows()
{
    // A shadow takes the whole direct sun away (only the ambient stays); it fades as the sun sets.
    CHECK(SunShadowStrength(0.7f) == 1.0f && SunShadowStrength(0.125f) == 1.0f);
    CHECK(Near(SunShadowStrength(0.0625f), 0.5f, 1e-6f) && SunShadowStrength(0.0f) == 0.0f && SunShadowStrength(-0.3f) == 0.0f);
    // The device classes' budget: none on Низкая, 1024 with one tap each on Средняя, 2048 with four static taps on Высокая.
    CHECK(SunShadowBudgetFor(ShadowQuality::Off).size == 0);
    CHECK(SunShadowBudgetFor(ShadowQuality::Low).size == 1024 && SunShadowBudgetFor(ShadowQuality::Low).static_taps == 1 &&
          SunShadowBudgetFor(ShadowQuality::Low).dynamic_taps == 1);
    CHECK(SunShadowBudgetFor(ShadowQuality::Soft).size == 2048 && SunShadowBudgetFor(ShadowQuality::Soft).static_taps == 4 &&
          SunShadowBudgetFor(ShadowQuality::Soft).dynamic_taps == 1);

    using Redraw = SunLayerTracker::Redraw;
    const Vec3 sun{0.48f, 0.64f, 0.6f};
    const Vec3 focus{1100.0f, 5.0f, -640.0f};
    SunLayerTracker tracker;
    const auto first = tracker.Advance(sun, focus, 1024);
    CHECK(first.redraw == Redraw::First && tracker.valid());
    const SunLayers &layers = first.layers;
    CHECK(Near(layers.static_texel, 80.0f / 1024.0f, 1e-6f) && Near(layers.dynamic_texel, 60.0f / 1024.0f, 1e-6f));
    // The focus is at the centre of the static layer (within a texel), inside the depth range.
    const auto centre = layers.StaticCoord(focus);
    CHECK(Near(centre[0], 0.5f, 1.0f / 1024.0f) && Near(centre[1], 0.5f, 1.0f / 1024.0f));
    CHECK(Near(centre[2], 160.0f / 200.0f, 1e-3f));
    // A caster 100 m up the sun ray: same texel, smaller depth, still inside.
    const auto nearer = layers.StaticCoord({focus[0] + sun[0] * 100.0f, focus[1] + sun[1] * 100.0f, focus[2] + sun[2] * 100.0f});
    CHECK(Near(nearer[0], centre[0], 1e-3f) && Near(nearer[1], centre[1], 1e-3f) && nearer[2] < centre[2] && nearer[2] > 0.0f);

    // The camera stays in the band: the static layer is not drawn again, however many frames pass, and the whole
    // shadowed sphere stays inside both layers.
    int redraws = 0;
    for (int frame = 0; frame < 600; ++frame)
    {
        const float a = 0.05f * static_cast<float>(frame);
        const Vec3 at{focus[0] + 5.0f * std::cos(a), focus[1] + 2.0f * std::sin(0.3f * a), focus[2] + 5.0f * std::sin(a)};
        const auto step = tracker.Advance(sun, at, 1024);
        redraws += step.redraw != Redraw::None ? 1 : 0;
        const auto k = step.layers.DynamicFromStatic();
        for (Vec3 offset : {Vec3{29.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -29.0f}, Vec3{-17.0f, 17.0f, 17.0f}, Vec3{0.0f, -29.0f, 0.0f}})
        {
            const auto c = step.layers.StaticCoord({at[0] + offset[0], at[1] + offset[1], at[2] + offset[2]});
            CHECK(c[0] > 0.0f && c[0] < 1.0f && c[1] > 0.0f && c[1] < 1.0f && c[2] > 0.0f && c[2] < 1.0f);
            const float du = c[0] * k[0] + k[1], dv = c[1] * k[0] + k[2];
            CHECK(du > 0.0f && du < 1.0f && dv > 0.0f && dv < 1.0f);
        }
        // The dynamic layer is centred on the focus (within a texel) and its uv comes from the static one's.
        const auto f = step.layers.StaticCoord(at);
        CHECK(Near(f[0] * k[0] + k[1], 0.5f, 1.5f / 1024.0f) && Near(f[1] * k[0] + k[2], 0.5f, 1.5f / 1024.0f));
    }
    CHECK(redraws == 0);
    // A sun that moved less than the step: still nothing. More: drawn again, with the new direction.
    const float tilt = 0.2f * 0.01745329f;
    const Vec3 near_sun = [&] { Vec3 v{sun[0] + tilt, sun[1], sun[2]}; const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return Vec3{v[0] / l, v[1] / l, v[2] / l}; }();
    CHECK(tracker.Advance(near_sun, focus, 1024).redraw == Redraw::None);
    const float step_tilt = 0.6f * 0.01745329f;
    const Vec3 far_sun = [&] { Vec3 v{sun[0] + step_tilt, sun[1], sun[2]}; const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return Vec3{v[0] / l, v[1] / l, v[2] / l}; }();
    const auto stepped = tracker.Advance(far_sun, focus, 1024);
    CHECK(stepped.redraw == Redraw::Sun && Near(stepped.layers.sun[0], far_sun[0], 1e-6f) && Near(stepped.layers.sun[2], far_sun[2], 1e-6f));
    CHECK(tracker.Advance(far_sun, focus, 1024).redraw == Redraw::None);
    // Leaving the band: drawn again around the new place, with the same sun; then quiet again.
    const Vec3 away{focus[0] + 14.0f, focus[1], focus[2] + 9.0f};
    const auto left = tracker.Advance(far_sun, away, 1024);
    CHECK(left.redraw == Redraw::Band && Near(left.layers.sun[0], far_sun[0], 1e-6f) && Near(left.layers.sun[2], far_sun[2], 1e-6f));
    CHECK(Near(left.layers.StaticCoord(away)[0], 0.5f, 1.0f / 1024.0f));
    CHECK(tracker.Advance(far_sun, away, 1024).redraw == Redraw::None);
    // Along the sun too (a climb): the depth range is anchored like the square.
    const Vec3 up_sun{away[0] + far_sun[0] * 11.0f, away[1] + far_sun[1] * 11.0f, away[2] + far_sun[2] * 11.0f};
    CHECK(tracker.Advance(far_sun, up_sun, 1024).redraw == Redraw::Band);
    // A direction a little off unit length (the weather's, a D3D light's) is not a step.
    CHECK(tracker.Advance({far_sun[0] * 0.9999f, far_sun[1] * 0.9999f, far_sun[2] * 0.9999f}, up_sun, 1024).redraw == Redraw::None);
    // The sea's wider region: its own layers, both wider; the land's again when the place has still geometry.
    {
        const auto sea = tracker.Advance(far_sun, up_sun, 1024, kSunShadowRadiusSea);
        CHECK(sea.redraw == Redraw::First && Near(sea.layers.dynamic_texel, 90.0f / 1024.0f, 1e-6f));
        const auto edge = sea.layers.StaticCoord({up_sun[0] + sea.layers.right[0] * 44.0f, up_sun[1] + sea.layers.right[1] * 44.0f, up_sun[2] + sea.layers.right[2] * 44.0f});
        const auto ks = sea.layers.DynamicFromStatic();
        CHECK(edge[0] * ks[0] + ks[1] > 0.0f && edge[0] * ks[0] + ks[1] < 1.0f); // 44 m off the ship is inside the dynamic layer
        CHECK(tracker.Advance(far_sun, up_sun, 1024, kSunShadowRadiusSea).redraw == Redraw::None);
        CHECK(tracker.Advance(far_sun, up_sun, 1024).redraw == Redraw::First);
    }
    // Invalidated (new geometry, device) or another size: drawn again.
    tracker.Invalidate();
    CHECK(tracker.Advance(far_sun, up_sun, 1024).redraw == Redraw::First);
    CHECK(tracker.Advance(far_sun, up_sun, 2048).redraw == Redraw::First);

    // Texel snapping: a small camera move shifts neither layer by a fraction of a texel.
    const auto before = tracker.Advance(far_sun, up_sun, 2048);
    const auto after = tracker.Advance(far_sun, {up_sun[0] + 0.013f, up_sun[1], up_sun[2] + 0.021f}, 2048);
    const Vec3 probe{up_sun[0] - 10.0f, up_sun[1] - 2.0f, up_sun[2] + 5.0f};
    const auto kb = before.layers.DynamicFromStatic(), ka = after.layers.DynamicFromStatic();
    const auto cb = before.layers.StaticCoord(probe), ca = after.layers.StaticCoord(probe);
    CHECK(cb == ca); // the static layer did not move at all
    const float shift_u = ((ca[0] * ka[0] + ka[1]) - (cb[0] * kb[0] + kb[1])) * 2048.0f;
    const float shift_v = ((ca[1] * ka[0] + ka[2]) - (cb[1] * kb[0] + kb[2])) * 2048.0f;
    CHECK(Near(shift_u, std::round(shift_u), 3e-2f) && Near(shift_v, std::round(shift_v), 3e-2f));
    // The projections agree with the coordinates: clip of the view * projection is the uv.
    {
        const auto view = before.layers.View(), proj = before.layers.StaticProjection(), dyn = before.layers.DynamicProjection();
        float l[3], c[3], d[3];
        for (int j = 0; j < 3; ++j)
            l[j] = probe[0] * view[j] + probe[1] * view[4 + j] + probe[2] * view[8 + j] + view[12 + j];
        for (int j = 0; j < 3; ++j)
        {
            c[j] = l[0] * proj[j] + l[1] * proj[4 + j] + l[2] * proj[8 + j] + proj[12 + j];
            d[j] = l[0] * dyn[j] + l[1] * dyn[4 + j] + l[2] * dyn[8 + j] + dyn[12 + j];
        }
        CHECK(Near(0.5f + 0.5f * c[0], cb[0], 1e-4f) && Near(0.5f - 0.5f * c[1], cb[1], 1e-4f) && Near(c[2], cb[2], 1e-4f));
        CHECK(Near(0.5f + 0.5f * d[0], cb[0] * kb[0] + kb[1], 1e-4f) && Near(0.5f - 0.5f * d[1], cb[1] * kb[0] + kb[2], 1e-4f));
        // A camera-relative world gives the same light space.
        const Vec3 origin{1000.0f, 0.0f, -600.0f};
        const auto relative = before.layers.View(origin);
        const Vec3 p{probe[0] - origin[0], probe[1] - origin[1], probe[2] - origin[2]};
        for (int j = 0; j < 3; ++j)
            CHECK(Near(p[0] * relative[j] + p[1] * relative[4 + j] + p[2] * relative[8 + j] + relative[12 + j], l[j], 2e-3f));
    }
    // The place's map: every ground point inside, with a margin; casters up to the reach towards the sun.
    {
        std::vector<Vec3> ground;
        for (int i = 0; i < 200; ++i)
            ground.push_back({-120.0f + 1.7f * static_cast<float>(i), 2.0f + 0.01f * static_cast<float>(i % 17), 40.0f + 90.0f * std::sin(0.37f * static_cast<float>(i))});
        const SunPlace place = ComputeSunPlace(sun, ground, 300.0f, 2048);
        for (const Vec3 &p : ground)
        {
            const auto c = place.Coord(p);
            CHECK(c[0] > 2.0f / 2048.0f && c[0] < 1.0f - 2.0f / 2048.0f && c[1] > 2.0f / 2048.0f && c[1] < 1.0f - 2.0f / 2048.0f);
            CHECK(c[2] > 0.0f && c[2] < 1.0f);
            const auto up_ray = place.Coord({p[0] + sun[0] * 250.0f, p[1] + sun[1] * 250.0f, p[2] + sun[2] * 250.0f});
            CHECK(Near(up_ray[0], c[0], 1e-3f) && Near(up_ray[1], c[1], 1e-3f) && up_ray[2] > 0.0f && up_ray[2] < c[2]);
            CHECK(place.Reaches(p, 0.0f));
        }
        CHECK(!place.Reaches({5000.0f, 0.0f, 5000.0f}, 50.0f));
        // The view and projection agree with Coord, also in a camera-relative world.
        const Vec3 origin{-100.0f, 0.0f, 50.0f}, p = ground[77];
        const auto view = place.View(origin), proj = place.Projection();
        float l[3], c[3];
        for (int j = 0; j < 3; ++j)
            l[j] = (p[0] - origin[0]) * view[j] + (p[1] - origin[1]) * view[4 + j] + (p[2] - origin[2]) * view[8 + j] + view[12 + j];
        for (int j = 0; j < 3; ++j)
            c[j] = l[0] * proj[j] + l[1] * proj[4 + j] + l[2] * proj[8 + j] + proj[12 + j];
        const auto expect = place.Coord(p);
        CHECK(Near(0.5f + 0.5f * c[0], expect[0], 1e-4f) && Near(0.5f - 0.5f * c[1], expect[1], 1e-4f) && Near(c[2], expect[2], 1e-4f));
    }
    // A sun straight overhead is a valid direction.
    SunLayerTracker zenith;
    const auto top = zenith.Advance({0.0f, 1.0f, 0.0f}, focus, 1024);
    CHECK(Near(top.layers.StaticCoord(focus)[0], 0.5f, 1.0f / 1024.0f));
}
// Decodes texel (x, y) of mip `mip` into a unit normal.
std::array<float, 3> NormalAt(const NormalMap &map, int mip, int x, int y)
{
    size_t offset = 0;
    int w = map.width, h = map.height;
    for (int m = 0; m < mip; ++m)
    {
        offset += static_cast<size_t>(w) * h * 2;
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }
    const float nx = map.rg[offset + (static_cast<size_t>(y) * w + x) * 2] / 255.0f * 2.0f - 1.0f;
    const float ny = map.rg[offset + (static_cast<size_t>(y) * w + x) * 2 + 1] / 255.0f * 2.0f - 1.0f;
    return {nx, ny, std::sqrt(std::max(1.0f - nx * nx - ny * ny, 0.0f))};
}

// Grey RGBA texels from heights.
std::vector<uint8_t> Grey(const std::vector<uint8_t> &heights)
{
    std::vector<uint8_t> rgba;
    for (uint8_t h : heights)
        rgba.insert(rgba.end(), {h, h, h, 255});
    return rgba;
}

NormalMap Derive(const std::vector<uint8_t> &heights, int width, int height, const NormalMapOptions &options)
{
    return DeriveNormalMap(Grey(heights), width, height, options);
}

void CheckNormalMaps()
{
    const int n = 64;
    NormalMapOptions options;
    options.strength = 4.0f;
    options.pre_blur = false;

    // A flat height field: every normal points straight out, at every mip.
    const std::vector<uint8_t> flat(n * n, 77);
    const NormalMap level = Derive(flat, n, n, options);
    CHECK(level.width == n && level.height == n && level.mip_count == 7);
    size_t texels = 0;
    for (int s = n; s >= 1; s /= 2)
        texels += static_cast<size_t>(s) * s;
    CHECK(level.rg.size() == texels * 2);
    CHECK(std::all_of(level.rg.begin(), level.rg.end(), [](uint8_t v) { return v == 128; }));

    // A ramp along u: slope = strength / width per texel, the normal leans against the slope, y stays 0.
    std::vector<uint8_t> ramp(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            ramp[y * n + x] = static_cast<uint8_t>(x * 255 / (n - 1));
    options.wrap = false;
    const NormalMap slope = Derive(ramp, n, n, options);
    const float expected = 4.0f / (n - 1); // dh/du in texels, times strength
    const auto mid = NormalAt(slope, 0, n / 2, n / 2);
    CHECK(Near(mid[0] / mid[2], -expected, 0.012f) && Near(mid[1], 0.0f, 0.01f));
    // Mips keep the world-space slope: two levels down the lean is the same.
    const auto coarse = NormalAt(slope, 2, n / 8, n / 8);
    CHECK(Near(coarse[0] / coarse[2], -expected, 0.012f) && Near(coarse[1], 0.0f, 0.01f));
    // Rotated ramp: the lean follows v instead.
    std::vector<uint8_t> ramp_v(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            ramp_v[y * n + x] = ramp[x * n + y];
    const auto mid_v = NormalAt(Derive(ramp_v, n, n, options), 0, n / 2, n / 2);
    CHECK(Near(mid_v[1] / mid_v[2], -expected, 0.012f) && Near(mid_v[0], 0.0f, 0.01f));

    // A cone: normals point away from the apex, with equal lean all around.
    std::vector<uint8_t> cone(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            cone[y * n + x] = static_cast<uint8_t>(std::clamp(255.0f - 6.0f * std::hypot(x - 31.5f, y - 31.5f), 0.0f, 255.0f));
    const NormalMap hill = Derive(cone, n, n, options);
    for (int i = 0; i < 16; ++i)
    {
        const float angle = 0.3927f * i;
        const int x = static_cast<int>(std::lround(31.5f + 14.0f * std::cos(angle))), y = static_cast<int>(std::lround(31.5f + 14.0f * std::sin(angle)));
        const auto normal = NormalAt(hill, 0, x, y);
        const float lean = std::hypot(normal[0], normal[1]) / normal[2];
        CHECK(Near(lean, 4.0f * 6.0f / 255.0f, 0.02f));
        CHECK(normal[0] * std::cos(angle) + normal[1] * std::sin(angle) > 0.9f * std::hypot(normal[0], normal[1]));
    }

    // A sine that tiles: analytic slope everywhere, and the wrap seam is as smooth as the inside.
    std::vector<uint8_t> wave(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            wave[y * n + x] = static_cast<uint8_t>(std::lround(127.5f + 100.0f * std::sin(6.2831853f * 2.0f * (x + 0.5f) / n)));
    options.wrap = true;
    const NormalMap tiled = Derive(wave, n, n, options);
    for (int x = 0; x < n; ++x)
    {
        const float slope_here = 4.0f * (100.0f / 255.0f) * (6.2831853f * 2.0f / n) * std::cos(6.2831853f * 2.0f * (x + 0.5f) / n);
        const auto normal = NormalAt(tiled, 0, x, 5);
        CHECK(Near(normal[0] / normal[2], -slope_here, 0.012f));
    }
    CHECK(std::abs(tiled.rg[0] - tiled.rg[(n - 1) * 2]) <= std::abs(tiled.rg[2 * 10] - tiled.rg[2 * 11]) + 1);
    // Without wrapping the same texture has an edge: the border normal differs from the wrapped one.
    options.wrap = false;
    CHECK(Derive(wave, n, n, options).rg[0] != tiled.rg[0]);
    options.wrap = true;

    // The pre-blur softens DXT block noise: the steps between 4x4 blocks give smaller peak slopes.
    std::vector<uint8_t> blocky(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            blocky[y * n + x] = static_cast<uint8_t>(128 + 6 * ((((x / 4) * 7 + (y / 4) * 13) % 5) - 2));
    const auto peak = [&](bool blur) {
        NormalMapOptions o = options;
        o.strength = 8.0f;
        o.pre_blur = blur;
        const NormalMap map = Derive(blocky, n, n, o);
        int worst = 0;
        for (int i = 0; i < n * n * 2; ++i)
            worst = std::max(worst, std::abs(map.rg[i] - 128));
        return worst;
    };
    CHECK(peak(false) > 8 && peak(true) * 10 <= peak(false) * 8);

    // Deterministic and cacheable: same input, same bytes; the key changes with every option.
    CHECK(Derive(cone, n, n, options).rg == Derive(cone, n, n, options).rg);
    NormalMapOptions other = options;
    other.strength = 2.0f;
    CHECK(NormalMapCacheKey(options) == NormalMapCacheKey(options) && NormalMapCacheKey(options) != NormalMapCacheKey(other));
    other = options;
    other.pre_blur = !options.pre_blur;
    CHECK(NormalMapCacheKey(options) != NormalMapCacheKey(other));
    // Non-square textures and bad input.
    const NormalMap strip = Derive(std::vector<uint8_t>(256 * 64, 10), 256, 64, options);
    CHECK(strip.mip_count == 9 && strip.rg.size() == 2u * (256 * 64 + 128 * 32 + 64 * 16 + 32 * 8 + 16 * 4 + 8 * 2 + 4 + 2 + 1));
    CHECK(Derive(flat, n, n + 1, options).rg.empty());

    // The size cap: a 256 x 64 texture with max_size 64 is derived at 64 x 16, and DropTopMips trims a chain.
    NormalMapOptions capped = options;
    capped.max_size = 64;
    int w = 0, h = 0;
    NormalMapSize(256, 64, capped, w, h);
    CHECK(w == 64 && h == 16);
    NormalMap small = Derive(std::vector<uint8_t>(256 * 64, 10), 256, 64, capped);
    CHECK(small.width == 64 && small.height == 16 && small.mip_count == 7 && small.rg.size() == NormalMapBytes(64, 16));
    NormalMap trimmed = strip;
    DropTopMips(trimmed, 32);
    CHECK(trimmed.width == 32 && trimmed.height == 8 && trimmed.mip_count == 6 && trimmed.rg.size() == NormalMapBytes(32, 8));
    CHECK(std::equal(trimmed.rg.begin(), trimmed.rg.end(), strip.rg.end() - static_cast<std::ptrdiff_t>(trimmed.rg.size())));
    // EAC RG11 sizes: 16 bytes per 4x4 block, at least one block per level; DropTopMips keeps the right tail.
    CHECK(NormalMapLevelBytes(NormalMapFormat::EacRg11, 256, 64) == 64u * 16 * 16 && NormalMapLevelBytes(NormalMapFormat::EacRg11, 2, 1) == 16);
    CHECK(NormalMapBytes(4, 4, NormalMapFormat::EacRg11) == 3 * 16);
    NormalMap eac{256, 64, 9, NormalMapFormat::EacRg11, std::vector<uint8_t>(NormalMapBytes(256, 64, NormalMapFormat::EacRg11))};
    for (size_t i = 0; i < eac.rg.size(); ++i)
        eac.rg[i] = static_cast<uint8_t>(i * 7);
    NormalMap eac_trimmed = eac;
    DropTopMips(eac_trimmed, 64);
    CHECK(eac_trimmed.width == 64 && eac_trimmed.mip_count == 7 && eac_trimmed.rg.size() == NormalMapBytes(64, 16, NormalMapFormat::EacRg11));
    CHECK(std::equal(eac_trimmed.rg.begin(), eac_trimmed.rg.end(), eac.rg.end() - static_cast<std::ptrdiff_t>(eac_trimmed.rg.size())));
    other = options;
    other.max_size = 512;
    CHECK(NormalMapCacheKey(options) != NormalMapCacheKey(other));
}

// Height maps at a lower preset's size keep their relief.
void CheckHeightMapSizes()
{
    // A lower preset derives a smaller map with the same relief: a 2048-wide stripe pattern derived at
    // 512 leans like the 1024 map (the strength is given at kNormalMapReferenceSize).
    const int big = 2048;
    std::vector<uint8_t> stripes(static_cast<size_t>(big) * 8 * 4);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < big; ++x)
        {
            const uint8_t v = static_cast<uint8_t>(std::lround(127.5f + 100.0f * std::sin(6.2831853f * x / 64.0f)));
            uint8_t *t = &stripes[(static_cast<size_t>(y) * big + x) * 4];
            t[0] = t[1] = t[2] = v;
            t[3] = 255;
        }
    NormalMapOptions full = HeightMapOptions(), half = HeightMapOptions();
    full.strength = half.strength = 7.0f;
    full.max_size = 1024;
    half.max_size = 512;
    const NormalMap at_full = DeriveNormalMap(stripes, big, 8, full), at_half = DeriveNormalMap(stripes, big, 8, half);
    CHECK(at_full.width == 1024 && at_half.width == 512);
    float lean_full = 0.0f, lean_half = 0.0f;
    for (int x = 0; x < 1024; ++x)
        lean_full = std::max(lean_full, std::fabs(NormalAt(at_full, 0, x, 1)[0]));
    for (int x = 0; x < 512; ++x)
        lean_half = std::max(lean_half, std::fabs(NormalAt(at_half, 0, x, 1)[0]));
    CHECK(lean_full > 0.1f && Near(lean_half, lean_full, 0.1f * lean_full));
}

// The relief models around their inference (a stand-in `infer`): the wrapped colour input, the crop, the axes, the
// mips; the relief store.
void CheckReliefModel()
{
    // A 4x2 map from an 8x4 texture: the input is the colour box-halved, wrap-padded by kReliefModelPad, three planes.
    std::vector<uint8_t> rgba(8 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
        {
            uint8_t *t = &rgba[(static_cast<size_t>(y) * 8 + x) * 4];
            t[0] = static_cast<uint8_t>(x * 30), t[1] = static_cast<uint8_t>(y * 60), t[2] = 255, t[3] = 255;
        }
    const int mw = 4, mh = 2, pw = mw + 2 * kReliefModelPad, ph = mh + 2 * kReliefModelPad;
    const std::vector<float> input = ReliefModelInput(rgba, 8, 4, mw, mh);
    const size_t plane = static_cast<size_t>(pw) * ph;
    CHECK(input.size() == plane * 3);
    // The map's texel (0, 0) averages the texture's 2x2 block; the padding repeats the map (the texel kReliefModelPad
    // left of the first column is the last one).
    const auto at = [&](int c, int x, int y) { return input[c * plane + static_cast<size_t>(y + kReliefModelPad) * pw + x + kReliefModelPad]; };
    CHECK(Near(at(0, 0, 0), (0 + 30) / 2.0f / 255.0f, 1e-5f) && Near(at(1, 0, 0), (0 + 60) / 2.0f / 255.0f, 1e-5f) &&
          Near(at(2, 0, 0), 1.0f, 1e-6f));
    CHECK(at(0, -1, 0) == at(0, mw - 1, 0) && at(1, 0, -1) == at(1, 0, mh - 1) && at(0, -kReliefModelPad, 0) == at(0, 0, 0));
    // Run: the stand-in returns a function of each padded texel; the crop takes the map's own texels back.
    int calls = 0;
    const std::vector<float> planes = ReliefModelRun(input, mw, mh, [&](const float *image, int iw, int ih, float *out) {
        ++calls;
        CHECK(iw == pw && ih == ph);
        for (size_t i = 0; i < plane * 3; ++i)
            out[i] = 0.5f + 0.5f * image[i];
        return true;
    });
    CHECK(calls == 1 && planes.size() == static_cast<size_t>(mw) * mh * 3 && Near(planes[0], 0.5f + 0.5f * at(0, 0, 0), 1e-6f));
    CHECK(ReliefModelRun(input, mw, mh, [](const float *, int, int, float *) { return false; }).empty());
    // Normals: the planes' 2 v - 1, unit, in our axes; height: the planes' mean.
    std::vector<float> p(static_cast<size_t>(mw) * mh * 3);
    for (size_t t = 0; t < static_cast<size_t>(mw) * mh; ++t)
        p[t] = 0.75f, p[mw * mh + t] = 0.5f, p[2 * mw * mh + t] = 1.0f; // (0.5, 0, 1): tilted to +u
    const std::vector<float> n = ReliefModelNormals(p, mw, mh), h = ReliefModelHeight(p, mw, mh);
    CHECK(Near(n[0], 0.5f / std::sqrt(1.25f), 1e-5f) && Near(n[1], 0.0f, 1e-6f) && Near(n[2], 1.0f / std::sqrt(1.25f), 1e-5f));
    CHECK(Near(h[0], 0.75f, 1e-6f));

    // The map: mips down to 1x1, the top mip the model's own normals (no rescale), tilts never above 0.99.
    const int w = 300, hh = 170;
    std::vector<float> normals(static_cast<size_t>(w) * hh * 3);
    for (size_t t = 0; t < normals.size() / 3; ++t)
    {
        const float x = 0.3f * static_cast<float>(t % 7) / 7.0f, y = -0.2f;
        const float l = std::sqrt(x * x + y * y + 0.25f);
        normals[t * 3] = x / l, normals[t * 3 + 1] = y / l, normals[t * 3 + 2] = 0.5f / l;
    }
    const NormalMap map = NormalMapOfNormals(normals, w, hh);
    CHECK(map.width == w && map.height == hh && map.mip_count == 9 && map.rg.size() == NormalMapBytes(w, hh));
    float off = 0.0f;
    for (size_t t = 0; t < static_cast<size_t>(w) * hh; ++t)
        for (int c = 0; c < 2; ++c)
            off = std::max(off, std::fabs(map.rg[t * 2 + c] / 255.0f * 2 - 1 - normals[t * 3 + c]));
    CHECK(off < 0.01f);
    std::vector<float> flat_out(normals.size());
    for (size_t t = 0; t < flat_out.size(); t += 3)
        flat_out[t] = 1.0f; // lying flat: x = 1, z = 0
    const NormalMap steep = NormalMapOfNormals(flat_out, w, hh);
    for (size_t i = 0; i < steep.rg.size(); i += 2)
    {
        const float x = steep.rg[i] / 255.0f * 2 - 1, y = steep.rg[i + 1] / 255.0f * 2 - 1;
        CHECK(std::sqrt(x * x + y * y) <= 1.0f);
    }

    // Probe levels: the first level no larger than 64 px, none below a 4x4 block.
    CHECK(ReliefProbeLevel(1024, 512, 11) == 4 && ReliefProbeLevel(64, 64, 7) == 0 && ReliefProbeLevel(512, 4, 8) == -1);
    // Few mips (the decks' 512 textures have three): the smallest level is the probe.
    CHECK(ReliefProbeLevel(256, 256, 1) == 0 && ReliefProbeLevel(512, 256, 3) == 2 && ReliefProbeLevel(512, 512, 3) == 2);
    // Keys follow the path, the probe bytes and the method.
    const uint8_t probe[4] = {1, 2, 3, 4};
    const uint64_t stone = ReliefMethod(ReliefClass::Stone);
    const uint64_t key = ReliefKey("a/b", probe, 4, 4, stone, kReliefMapSize);
    const uint8_t other[4] = {1, 2, 3, 5};
    CHECK(key == ReliefKey("a/b", probe, 4, 4, stone, kReliefMapSize));
    CHECK(key != ReliefKey("a/c", probe, 4, 4, stone, kReliefMapSize));
    CHECK(key != ReliefKey("a/b", other, 4, 4, stone, kReliefMapSize));
    CHECK(key != ReliefKey("a/b", probe, 4, 4, ReliefMethod(ReliefClass::HeightMap), kReliefMapSize));
    // The method covers what makes the map: PBRify's maps (one method for every colour class), the derivation of height
    // maps, the models, the material's height and gloss and the version. The maps of the methods before it (DeepBump's,
    // the heuristic gloss) are never found again.
    CHECK(ReliefMethod(ReliefClass::None) == 0);
    CHECK(ReliefMethod(ReliefClass::Metal) == ReliefMethod(ReliefClass::Stone) &&
          ReliefMethod(ReliefClass::Metal) != ReliefMethod(ReliefClass::HeightMap)); // every colour class has a height
    for (const ReliefClass colour : {ReliefClass::Stone, ReliefClass::Wood, ReliefClass::Building, ReliefClass::Plaster})
        CHECK(ReliefMethod(colour) == ReliefMethod(ReliefClass::Stone) &&
              ReliefMethod(colour) != ReliefMethod(ReliefClass::HeightMap));
    CHECK(kReliefMethodVersion == 8);
    CHECK(ReliefMethod(ReliefClass::Stone) != 3 && key != ReliefKey("a/b", probe, 4, 4, 3, kReliefMapSize));
    // The weights' sha256 prefixes (the ncnn graphs' .bin; checked by CMake too).
    CHECK(kReliefNormalModelDigest == 0xbc13e7082ffb7053ull && kReliefHeightModelDigest == 0xd54154e6179a2178ull &&
          kReliefRoughnessModelDigest == 0x3fe407f25ac9f0d0ull);
    // The gloss maps' method: the version, the roughness network, the class.
    CHECK(GlossMethod(GlossClass::Metal) != GlossMethod(GlossClass::Glass) && kGlossMethod == 0x47000005u);

    // The store: a written map reads back; a damaged file is deleted and reported.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "storm_fx_relief_check";
    std::filesystem::remove_all(dir);
    NormalMap eac{8, 4, 4, NormalMapFormat::EacRg11, std::vector<uint8_t>(NormalMapBytes(8, 4, NormalMapFormat::EacRg11), 7)};
    CHECK(!HasReliefMap(dir, key) && !ReadReliefMap(dir, key));
    CHECK(WriteReliefMap(dir, key, eac) && HasReliefMap(dir, key));
    const std::optional<NormalMap> back = ReadReliefMap(dir, key);
    CHECK(back && back->width == 8 && back->height == 4 && back->mip_count == 4 && back->format == NormalMapFormat::EacRg11 &&
          back->rg == eac.rg);
    // With the material texture: it and its depth read back.
    eac.material.assign(eac.rg.size(), 9);
    eac.depth = 6.5f;
    eac.trust.assign(2, 200);
    eac.trust_coverage = 0.25f;
    CHECK(WriteReliefMap(dir, key, eac));
    const std::optional<NormalMap> with_material = ReadReliefMap(dir, key);
    CHECK(with_material && with_material->rg == eac.rg && with_material->material == eac.material && with_material->depth == 6.5f &&
          with_material->trust == eac.trust && with_material->trust_coverage == 0.25f);
    std::filesystem::resize_file(ReliefFile(dir, key), 40);
    std::string error;
    CHECK(!ReadReliefMap(dir, key, &error) && !error.empty() && !HasReliefMap(dir, key));
    // Force rename to fail after the complete temporary file was written. Cleanup succeeds, but publication must
    // still report failure and leave the obstructing target intact (both APIs share WriteMapFile).
    for (const bool gloss : {false, true})
    {
        const auto blocked = gloss ? GlossFile(dir, key + 1) : ReliefFile(dir, key + 1);
        std::filesystem::create_directories(blocked / "occupied");
        CHECK(!(gloss ? WriteGlossMap(dir, key + 1, eac) : WriteReliefMap(dir, key + 1, eac)));
        CHECK(std::filesystem::is_directory(blocked / "occupied"));
        CHECK(!std::filesystem::exists(blocked.string() + ".tmp"));
        std::filesystem::remove_all(blocked);
        CHECK(gloss ? WriteGlossMap(dir, key + 1, eac) : WriteReliefMap(dir, key + 1, eac));
        CHECK(gloss ? HasGlossMap(dir, key + 1) : HasReliefMap(dir, key + 1));
    }
    std::filesystem::remove_all(dir);
}

// The relief's material texture (height_map.hpp): the height normalised per region (an atlas of a deep and a shallow
// part: each keeps its own range), its depth the class's, the gloss in G, the mips.
void CheckReliefMaterial()
{
    // Every class with relief has a height (the parallax wherever it can work), the grey height maps their own.
    CHECK(ReliefHasHeight(ReliefClass::Stone) && ReliefHasHeight(ReliefClass::Wood) && ReliefHasHeight(ReliefClass::Plaster) &&
          ReliefHasHeight(ReliefClass::Building) && ReliefHasHeight(ReliefClass::HeightMap) && !ReliefHasHeight(ReliefClass::None) &&
          ReliefHasHeight(ReliefClass::Metal) && ReliefHasHeight(ReliefClass::Prop));
    CHECK(ReliefDepthTexels(ReliefClass::Metal) > 0.0f && ReliefDepthTexels(ReliefClass::Metal) < ReliefDepthTexels(ReliefClass::Prop) &&
          ReliefDepthTexels(ReliefClass::None) == 0.0f);
    // The stronger masonry depth must survive both shader limits and old on-device cache metadata. Explicitly
    // included decks/piers and every other non-masonry class keep their old depth and cap.
    CHECK(ReliefParallaxMaxDepth(ReliefClass::Stone) == 0.05f &&
          ReliefParallaxMaxDepth(ReliefClass::Building) == 0.05f);
    for (const auto &[relief, depth] : std::vector<std::pair<ReliefClass, float>>{
             {ReliefClass::HeightMap, 1.5f}, {ReliefClass::Plaster, 1.25f}, {ReliefClass::Wood, 1.0f},
             {ReliefClass::Prop, 0.75f}, {ReliefClass::Metal, 0.375f}, {ReliefClass::None, 0.0f}})
    {
        CHECK(ReliefDepthTexels(relief) == depth);
        CHECK(ReliefParallaxMaxDepth(relief) == 0.025f);
    }
    CHECK(ReliefMethod(ReliefClass::Stone) != ReliefMethod(ReliefClass::HeightMap));
    const int n = 256;
    NormalMap map;
    map.width = map.height = n;
    map.format = NormalMapFormat::Rg8;
    for (int w = n; w >= 1; w /= 2)
        ++map.mip_count;
    map.rg.assign(NormalMapBytes(n, n), 128);
    // Left half: deep blocks (0..1); right half: shallow planks (0.45..0.55, a seam every 16 texels).
    std::vector<float> height(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            height[static_cast<size_t>(y) * n + x] = x < n / 2 ? (((x / 32 + y / 32) & 1) ? 1.0f : 0.0f)
                                                               : (y % 16 < 2 ? 0.45f : 0.55f);
    std::vector<uint8_t> gloss(static_cast<size_t>(n) * n, 200);
    CHECK(AddReliefMaterial(map, ReliefClass::Stone, height, gloss));
    CHECK(map.material.size() == map.rg.size());
    const auto r = [&](int x, int y) { return map.material[(static_cast<size_t>(y) * n + x) * 2]; };
    // The shallow planks' seams are as deep as the blocks' joints are: the region's own range (not a few percent).
    CHECK(r(3 * n / 4, 64) < 70 && r(3 * n / 4, 72) > 150);
    CHECK(r(16, 16) < 120 && r(48, 16) > 180 && r(48, 16) - r(16, 16) > 90);
    CHECK(map.material[1] == 200); // G: the gloss
    CHECK(Near(map.depth, ReliefDepthTexels(ReliefClass::Stone) * 0.5f, 1e-5f)); // texels of this 256 map
    // A building atlas cached under the old Wood classification retains its pixels, but must get the same depth as
    // a newly generated Building map. Preset mip reduction still preserves depth in UV units.
    NormalMap reclassified = map;
    reclassified.depth = ReliefDepthTexels(ReliefClass::Wood) * static_cast<float>(n) / kReliefMapSize;
    const auto cached_normals = reclassified.rg, cached_material = reclassified.material;
    RefreshReliefDepth(reclassified, ReliefClass::Building);
    CHECK(reclassified.depth == ReliefDepthTexels(ReliefClass::Building) * static_cast<float>(n) / kReliefMapSize);
    CHECK(reclassified.rg == cached_normals && reclassified.material == cached_material);
    // Even a 4 m Stone UV span was almost capped at 2.5 cm before. Its x2 target must remain x2 after the shader's
    // world-depth clamp; 8 m spans were already capped, so they need the doubled cap as well.
    for (const auto relief : {ReliefClass::Stone, ReliefClass::Building})
        for (const float span : {1.0f, 4.0f, 8.0f})
        {
            const float old_texels = relief == ReliefClass::Stone ? 3.0f : 2.0f;
            const float old_depth = std::clamp(old_texels / 512.0f * span, 0.005f, 0.025f);
            const float new_depth = std::clamp(ReliefDepthTexels(relief) / 512.0f * span,
                                               0.2f * ReliefParallaxMaxDepth(relief), ReliefParallaxMaxDepth(relief));
            CHECK(Near(new_depth, 2.0f * old_depth, 1e-6f));
        }
    const float reclassified_uv_depth = reclassified.depth / reclassified.width;
    DropTopMips(reclassified, n / 2);
    CHECK(reclassified.depth / reclassified.width == reclassified_uv_depth);
    reclassified.material.clear();
    RefreshReliefDepth(reclassified, ReliefClass::Building);
    CHECK(reclassified.depth == 0.0f);

    // DropTopMips keeps the material with its mips and the depth in texels of the new top.
    NormalMap half = map;
    DropTopMips(half, n / 2);
    CHECK(half.width == n / 2 && half.material.size() == half.rg.size() && Near(half.depth, map.depth * 0.5f, 1e-6f));
    CHECK(std::equal(half.material.begin(), half.material.end(), map.material.begin() + static_cast<std::ptrdiff_t>(n) * n * 2));
    // No gloss given: G 0. A flat height, mismatched sizes or a class without height: no material.
    NormalMap plain = map;
    CHECK(AddReliefMaterial(plain, ReliefClass::Wood, height, {}) && plain.material[1] == 0 &&
          Near(plain.depth, ReliefDepthTexels(ReliefClass::Wood) * 0.5f, 1e-5f));
    const std::vector<float> flat(static_cast<size_t>(n) * n, 0.5f);
    CHECK(!AddReliefMaterial(plain, ReliefClass::Stone, flat, {}) && plain.material.empty() && plain.depth == 0.0f);
    CHECK(!AddReliefMaterial(plain, ReliefClass::Stone, std::span(height).first(10), {}));
    CHECK(!AddReliefMaterial(plain, ReliefClass::None, height, {}) && plain.material.empty());
    CHECK(AddReliefMaterial(plain, ReliefClass::HeightMap, height, {}) && !plain.material.empty());
}

void CheckCheapUpgrades()
{
    WeatherInput weather;
    weather.sun_dir = {0.6f, 0.8f, 0.0f};
    weather.fog_density = 0.002f;
    const SkyState noon = ComputeSky(weather);
    // The sky stays finite for every sun, also straight down (the night's sky_sun_dir): its dome tints the
    // hemisphere ambient and its horizon the aerial perspective.
    for (float h : {-1.0f, -0.5f, 0.0f, 0.3f})
    {
        WeatherInput w = weather;
        w.sun_dir = {0.0f, h, -std::sqrt(1.0f - h * h)};
        const SkyState sky = ComputeSky(w);
        for (int i = 0; i < 3; ++i)
            CHECK(std::isfinite(sky.dome[i]) && std::isfinite(sky.horizon[i]));
    }

    // Aerial perspective: the game's fog density, a haze layer that thins with the camera's height, world up
    // taken from the view matrix, haze colour = the horizon (gamma encoded).
    const std::array<float, 16> view = {0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1}; // world y -> view x
    const AerialUniforms low = ComputeAerial(noon, 2.0f, view), high_up = ComputeAerial(noon, 200.0f, view);
    CHECK(low.u_fxAerial[0][3] == 0.002f && Near(low.u_fxAerial[0][1], std::sqrt(noon.horizon[1]), 1e-5f));
    CHECK(low.u_fxAerial[1][0] > 10.0f * high_up.u_fxAerial[1][0] && high_up.u_fxAerial[1][0] > 0.0f);
    CHECK(low.u_fxAerial[2][0] == 1.0f && low.u_fxAerial[2][1] == 0.0f && low.u_fxAerial[1][3] < 1.0f);
    // The emulator's form: a sixth of the haze, relative to the fog density, milder desaturation, no opacity cap.
    const auto fog_low = ComputeAerialFogHaze(2.0f), fog_high = ComputeAerialFogHaze(200.0f);
    CHECK(Near(fog_low[0] * noon.fog_density * 6.0f, low.u_fxAerial[1][0], 1e-7f) && fog_low[0] > 10.0f * fog_high[0]);
    CHECK(fog_low[1] == low.u_fxAerial[1][1] && kAerialFogDesaturation < low.u_fxAerial[1][2]);

    // Wind: stronger weather, larger and faster sway; the direction is the weather's.
    weather.wind = {0.6f, 0.8f};
    weather.wind_speed = 3.0f;
    const WindUniforms breeze = ComputeWind(weather, 5.0f);
    weather.wind_speed = 30.0f;
    const WindUniforms gale = ComputeWind(weather, 5.0f);
    CHECK(breeze.u_fxWind[0][0] == 0.6f && breeze.u_fxWind[0][2] == 0.8f && breeze.u_fxWind[0][3] == 5.0f);
    CHECK(gale.u_fxWind[1][0] > 2.0f * breeze.u_fxWind[1][0] && gale.u_fxWind[1][1] > breeze.u_fxWind[1][1]);
    CHECK(gale.u_fxWind[1][0] <= 0.5f + 1e-6f); // saturates: trees do not fly away in a storm

    // Scene grades: each mood is distinct, the blend follows the sky continuously.
    const auto sky_at = [](float sun_height) {
        WeatherInput w;
        w.sun_dir = {std::sqrt(1.0f - sun_height * sun_height), sun_height, 0.0f};
        return ComputeSky(w);
    };
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> gain_identity{1.0f, 1.0f, 1.0f};
    const ColorGrade day = SceneGrade(sky_at(0.8f), 0.0f, 0.0f), dusk = SceneGrade(sky_at(0.08f), 0.0f, 0.0f);
    const ColorGrade night = SceneGrade(sky_at(-0.5f), 0.0f, 0.0f), storm = SceneGrade(sky_at(0.8f), 1.0f, 0.0f);
    const ColorGrade indoors = SceneGrade(sky_at(-0.5f), 0.0f, 1.0f);
    CHECK(day == ColorGrade::ForMood(ColorGrade::Mood::TropicalNoon) && night == ColorGrade::ForMood(ColorGrade::Mood::Night));
    CHECK(indoors == ColorGrade::ForMood(ColorGrade::Mood::Interior));
    CHECK(dusk.Apply(grey)[0] > dusk.Apply(grey)[2] + 0.015f);   // warm
    CHECK(night.Apply(grey)[2] > night.Apply(grey)[0] + 0.015f); // cool
    CHECK(storm.Apply(grey)[2] > storm.Apply(grey)[0]); // cool
    // Restrained: the moods shift hue and warmth only, never contrast, saturation or exposure.
    for (const ColorGrade *mood : {&day, &dusk, &night, &storm, &indoors})
        CHECK(mood->contrast == 1.0f && mood->saturation == 1.0f && mood->vibrance == 0.0f && mood->exposure == 0.0f &&
              mood->gain == gain_identity);
    // A small step of the sun is a small step of the grade (no jump at dusk).
    float worst = 0.0f;
    for (int i = 0; i < 60; ++i)
    {
        const auto a = SceneGrade(sky_at(-0.3f + 0.01f * i), 0.0f, 0.0f).Apply(grey), b = SceneGrade(sky_at(-0.29f + 0.01f * i), 0.0f, 0.0f).Apply(grey);
        worst = std::max({worst, std::fabs(a[0] - b[0]), std::fabs(a[2] - b[2])});
    }
    CHECK(worst < 0.02f);
    // The post chain rebuilds the table only when the drift shows in 8-bit output.
    CHECK(GradeDifference(day, day) == 0.0f && GradeDifference(day, night) > 0.01f);
    const ColorGrade user = ColorGrade::Preset(GradePreset::Caribbean);
    // Built two slices at a time (the post chain's background rebuild), the table is the same.
    std::vector<uint8_t> sliced(8 * 8 * 8 * 4);
    for (int first = 0; first < 8; first += 2)
        BuildLutSlices(user, 8, dusk, first, 2, sliced.data());
    CHECK(sliced == BuildLutRgba8(user, 8, dusk));
    CHECK(GradeDifference(SceneGrade(sky_at(0.5f), 0.0f, 0.0f), SceneGrade(sky_at(0.501f), 0.0f, 0.0f)) < 0.5f / 255.0f);
    // The scene grade is applied before the player's grade.
    const std::vector<uint8_t> both = BuildLutRgba8(user, 8, night);
    const auto expected = user.Apply(night.Apply({1.0f / 7.0f, 3.0f / 7.0f, 5.0f / 7.0f}));
    const uint8_t *texel = &both[((5 * 8 + 3) * 8 + 1) * 4];
    CHECK(std::abs(texel[0] - static_cast<int>(std::lround(expected[0] * 255.0f))) <= 1 &&
          std::abs(texel[2] - static_cast<int>(std::lround(expected[2] * 255.0f))) <= 1);
}

int g_log_lines = 0;
int g_debug_lines = 0;

void CheckReliefRules()
{
    CHECK(NormalizeTexturePath("Locations\\Bump_City.tga.tx") == "locations/bump_city");
    CHECK(NormalizeTexturePath("resource\\textures\\Ships\\Brig1\\Hull1\\bortoutbrig.TGA") == "ships/brig1/hull1/bortoutbrig");
    // The world map names its textures with a leading separator (owner's log 1655: its sea frames asked for relief).
    CHECK(NormalizeTexturePath("\\WorldMap\\sea\\ani\\wave_0000") == "worldmap/sea/ani/wave_0000");
    CHECK(NormalizeTexturePath("/worldmap/clouds") == "worldmap/clouds");
    CHECK(ClassifyTextureName(NormalizeTexturePath("/worldmap/sea/ani/wave_0017")).relief == ReliefClass::None);
    CHECK(NameSuggestsHeightMap("bump_houses") && NameSuggestsHeightMap("ships/sailbumpu1") && NameSuggestsHeightMap("sph2_bump2"));
    CHECK(!NameSuggestsHeightMap("bumpy_folder/wall") && !NameSuggestsHeightMap("deckplanksu2"));

    // Grey with the channel error of DXT compression is grey; a tinted texture is not.
    std::vector<uint8_t> grey, tinted;
    for (int i = 0; i < 1024; ++i)
    {
        const uint8_t v = static_cast<uint8_t>(i % 251);
        grey.insert(grey.end(), {v, static_cast<uint8_t>(std::min(v + (i % 5), 255)), static_cast<uint8_t>(std::max(v - (i % 7), 0)), 255});
        tinted.insert(tinted.end(), {v, static_cast<uint8_t>(v / 2), static_cast<uint8_t>(v / 3), 255});
    }
    CHECK(IsGrayscale(grey) && !IsGrayscale(tinted) && !IsGrayscale({}));

    // The table on names from TEHO's RESOURCE/Textures.
    const auto relief = [](const char *path) { return ClassifyTextureName(NormalizeTexturePath(path)).relief; };
    const ReliefClass N = ReliefClass::None, S = ReliefClass::Stone, W = ReliefClass::Wood, B = ReliefClass::Building,
                      P = ReliefClass::Plaster;
    const std::pair<const char *, ReliefClass> cases[] = {
        // stone and brick
        {"StoneH2_walls.tga.tx", S}, {"BigBarBrick.tga", S}, {"rockK1a", S}, {"skala_jungle", S}, {"cobbleM1", S},
        {"TOWN_COB", S}, {"Beton_2", S}, {"fort_fra_0", S}, {"TortugaFortMain", S}, {"cavernTileU4", S}, {"tileU8", S},
        {"063pierres", S}, {"pierStone1", S}, {"lighthouseU1", S}, {"incas_hram", S}, {"Crypt_0", S},
        // wood, planks, ships
        {"woodPlanksU1", W}, {"deckPlanksT5", W}, {"Capdeck_2", W}, {"dm_barque", W}, {"Gundeck_1", W}, {"pierWood1", W},
        {"balkU3", W}, {"doorU1", W}, {"Hold_1", W}, {"TOWN_WOOD", W}, {"Ships\\Brig1\\Hull1\\bortoutbrig", W},
        {"Ships\\Barque1\\Hull3\\deckPlanksT3", W}, {"Ships\\deck", W}, {"cabinet1c", W},
        // building atlases
        {"BigHouse01", B}, {"Residence2_walls", B}, {"Tavern1_walls", B}, {"tavern1ins", B}, {"church2_walls", B},
        {"Foudation_Win_Door.tga.tx", B}, {"Foudation_Win2Door.tga.tx", B},
        {"hut1", B}, {"hut2", B}, {"hut3", B}, {"hut4", B}, {"barn2", B},
        {"Hut1_walls", B}, {"Hut2_walls", B}, {"Hut3_walls", B}, {"Hut4_walls", B}, {"barn2_wall", B},
        {"windmill_walls", B}, {"FlH03", B}, {"SpH4_walls", B}, {"Shipyard1", B}, {"objects_tex2", W},
        // Actual mixed residence interiors and compound floor names stay shallow (no POM).
        {"residence1ins.tga.tx", P}, {"residence4ins.tga.tx", P}, {"residence2_tex.tga.tx", P},
        {"residence3_tex.tga.tx", P}, {"Residence_5_inside.tga.tx", P}, {"SmallResidence.tga.tx", P},
        {"Pirate_residence.tga.tx", P}, {"TwoFloorHouse.tga.tx", P}, {"estateFloorU1.tga.tx", P},
        {"parquetU1.tga.tx", P}, {"floortile.tga.tx", P},
        {"stone_floor", P}, {"residence_carpet", P}, {"house_cloth", P}, {"stone_fabric", P},
        {"residence6_inside", P}, {"residence7ins", P}, {"residence8_tex", P},
        {"residence_ins_wall", B}, {"fort_inside_wall", S}, {"stone_wall", S},
        {"painting_floor", N}, {"leaf_residence_inside", N}, {"loading/residence6_inside", N},
        {"Residence1.tga.tx", B}, {"Residence2.tga.tx", B}, {"Residence3.tga.tx", B},
        {"Residence4.tga.tx", B}, {"Residence4a.tga.tx", B},
        // plaster, ground
        {"Celling", P}, {"floorU3", P}, {"groundK2", P}, {"1sand1", P}, {"shoreU5", P},
        {"dirtK3", P}, {"tropa256", P}, {"jungleU5", P}, {"1grass1", P}, {"grassgreen1", P},
        // no relief: characters, UI, sky, water, glass, foliage cards, cloth and sails, decals, light, lightmaps
        {"Characters\\Lo_Man_1", ReliefClass::Character}, {"INTERFACES\\PORTRAITS\\128\\face_1", N}, {"BATTLE_INTERFACE\\cannons", N},
        {"WEATHER\\SKIES\\day", N}, {"lighting\\day1\\Fort_lm", N}, {"Ships\\parus_common", N},
        {"Ships\\Mayfang1\\Hull1\\mayfang1_alpha", N}, {"Ships\\trailShip", N}, {"sky_up", N}, {"water1", N},
        {"sea_1", N}, {"GlassWindow", N}, {"windBigHouse01", N}, {"res_win", N}, {"occlTempleU1", N}, {"reflChurchU2", N},
        {"MontserratMask", N}, {"PalmsSprites", N}, {"leafPalms", N}, {"Ferns", N}, {"tree4", N}, {"cloth1", P},
        {"carpetU4", P}, {"ropeU1", N}, {"FishingNets", N}, {"SignboardsA", N}, {"smoke_dirt", N}, {"bonfire_small", N},
        {"corona01", N}, {"fonar", ReliefClass::Metal}, {"lighthouseFonarU1", ReliefClass::Metal}, {"candle4c", N}, {"WaterFall", N}, {"River1", N},
        {"metalU4", ReliefClass::Metal}, {"cannon_T1", ReliefClass::Metal}, {"skelet3", ReliefClass::Prop}, {"telo", N}, {"Item_Rapier02", ReliefClass::Metal},
        {"ammo/katana", ReliefClass::Metal}, {"items/purse", ReliefClass::Prop}, {"islands/caiman/caiman", ReliefClass::Prop},
        {"islands/nevis/nevismask", N}, {"grass/grass_1", N}, {"Ships/Brig1/Hull1/cannon_brig", ReliefClass::Metal}, {"MenuWoodFlooooor01", N},
        {"interfaces/Foudation_Win_Door", N}, {"loading/Hut2_walls", N}, {"windhutinside", N},
        {"hutinside", W}, {"barn1", W}, {"hut2_bump", W}, {"hut2_walls_bump", W}, {"HiDeckalpha", N}, {"cap_deck_glass", N}, {"Particles_list", N}, {"shadow2", N}, {"back_jungle", N},
    };
    for (const auto &[path, expected] : cases)
        if (relief(path) != expected)
        {
            std::printf("FAILED relief rule: %s is %s, expected %s\n", path, ReliefClassName(relief(path)), ReliefClassName(expected));
            ++failures;
        }
    // An unknown name is a prop (the last rule, which says so).
    CHECK(ClassifyTextureName("antigua").rule != nullptr && ClassifyTextureName("antigua").relief == ReliefClass::Prop);

    SetLogSink([](LogLevel level, const char *) { ++(level == LogLevel::Debug ? g_debug_lines : g_log_lines); });
    ReliefClassifier classifier;
    const auto verdict = [&](const char *path, const std::vector<uint8_t> &pixels) { return classifier.Classify(path, pixels); };
    CHECK(verdict("bump_city.tga.tx", grey).relief == ReliefClass::HeightMap);
    // A coloured "bump" texture is an ordinary one: here the table's stone.
    CHECK(verdict("bump_rock.tga", tinted).relief == S);
    // The sea's animated bump frames stay without relief (folder rule), grey or not.
    CHECK(verdict("WEATHER\\SEA\\BUMP_G2MX\\seabump0017.tga.tx", grey).relief == N);
    CHECK(verdict("deckPlanksU2.tga", {}).relief == W);
    CHECK(verdict("Antigua.tga.tx", {}).relief == ReliefClass::Prop);
    CHECK(verdict("deckPlanksU2.tga", {}).relief == W); // a repeat: decided again, not logged again
    // One debug line per texture, nothing at the normal levels.
    CHECK(g_debug_lines == 5 && g_log_lines == 0);
    SetLogSink(nullptr);
}
} // namespace

// Every preset on every GPU-family profile: constrained settings are valid, every loss is reported, and the
// low preset never needs anything.
void CheckCapsProfiles()
{
    Support all;
    all.lut_3d = all.shadow_map = all.scene_depth = all.normal_map = true;
    all.lamp_depth = LampDepth::D24;
    all.msaa_samples = 4;
    all.msaa_depth = all.anisotropy = all.smaa = all.sea_waves_vertex = all.sea_waves_float = all.sea_waves_surface_float = true;
    for (int p = 0; p < static_cast<int>(CapsProfile::Count); ++p)
    {
        const auto profile = static_cast<CapsProfile>(p);
        const Support support = RestrictSupport(all, profile);
        CHECK(std::string(CapsProfileName(profile)) != "?");
        for (QualityPreset preset : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High})
        {
            const Settings requested = Settings::FromPreset(preset);
            std::vector<std::string> changes;
            const Settings constrained = ConstrainSettings(requested, support, changes);
            CHECK(ValidateSettings(constrained, support).empty());
            CHECK(changes.empty() == (constrained == requested));
            if (preset == QualityPreset::Low)
                CHECK(changes.empty());
            if (support.msaa_samples < 2)
                CHECK(constrained.msaa == 1 && !constrained.alpha_to_coverage);
            // Every phone without MSAA still gets anti-aliasing on Средняя / Высокая: SMAA in its place.
            if (preset != QualityPreset::Low)
                CHECK(constrained.smaa == (support.msaa_samples < 2));
        }
    }
    // State precision and surface filtering have independent capability fallbacks.
    // A device with packed state must retain its floating surface when it supports it.
    for (const bool state : {false, true})
        for (const bool surface : {false, true})
        {
            Support device;
            device.sea_waves_float = state;
            device.sea_waves_surface_float = surface;
            for (int p = 0; p < static_cast<int>(CapsProfile::Count); ++p)
            {
                const auto profile = static_cast<CapsProfile>(p);
                const Support restricted = RestrictSupport(device, profile);
                const bool minimal = profile == CapsProfile::Gles30Minimal;
                CHECK(restricted.sea_waves_float == (state && !minimal));
                CHECK(restricted.sea_waves_surface_float == (surface && !minimal));
            }
        }
    // A profile only removes.
    CHECK(RestrictSupport(Support{}, CapsProfile::Xclipse).msaa_samples == 1);
}

// Axis-aligned box as 12 triangles (the AO bake only needs surfaces).
void AddBox(std::vector<Vec3> &p, std::vector<uint32_t> &i, Vec3 lo, Vec3 hi)
{
    const uint32_t base = static_cast<uint32_t>(p.size());
    for (int c = 0; c < 8; ++c)
        p.push_back({c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]});
    const uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto &f : faces)
        for (const uint32_t t : {f[0], f[1], f[2], f[0], f[2], f[3]})
            i.push_back(base + t);
}

void CheckFixtures()
{
    // A tavern table with a candlestick, a street lantern on its post by a house wall, a sconce's arm on that wall; the
    // ground under all of them.
    std::vector<Vec3> p;
    std::vector<uint32_t> idx;
    AddBox(p, idx, {-20.0f, -0.1f, -20.0f}, {20.0f, 0.0f, 20.0f});   // 0: the ground
    AddBox(p, idx, {0.0f, 0.75f, 0.0f}, {1.2f, 0.8f, 0.8f});         // 1: the table top
    AddBox(p, idx, {0.55f, 0.8f, 0.35f}, {0.65f, 1.0f, 0.45f});      // 2: the candlestick on it
    AddBox(p, idx, {4.9f, 0.0f, 4.9f}, {5.1f, 2.3f, 5.1f});          // 3: the lantern's post
    AddBox(p, idx, {4.8f, 2.3f, 4.8f}, {5.2f, 2.8f, 5.2f});          // 4: the lantern on it
    AddBox(p, idx, {6.0f, 0.0f, 0.0f}, {6.3f, 4.0f, 10.0f});         // 5: a house wall
    AddBox(p, idx, {5.7f, 2.0f, 2.95f}, {6.0f, 2.1f, 3.05f});        // 6: a sconce's arm on it
    AddBox(p, idx, {8.0f, 0.0f, 7.9f}, {8.15f, 3.0f, 8.05f});        // 7: a gallows post
    AddBox(p, idx, {8.15f, 2.9f, 7.95f}, {8.9f, 3.0f, 8.05f});       // 8: its arm; the lantern under it has no collision
    // The same mesh as the game stores it: every triangle with vertices of its own (normals, texture coordinates).
    std::vector<Vec3> loose;
    std::vector<uint32_t> loose_idx;
    for (const uint32_t v : idx)
    {
        loose_idx.push_back(static_cast<uint32_t>(loose.size()));
        loose.push_back(p[v]);
    }
    const std::vector<std::vector<Vec3>> flames = {
        {{0.6f, 1.15f, 0.4f}}, {{5.0f, 2.55f, 5.0f}}, {{5.75f, 2.25f, 3.0f}}, {{-10.0f, 3.0f, -10.0f}}, {{8.7f, 2.5f, 8.0f}}};
    for (const bool welded : {true, false})
    {
        const std::vector<Fixture> f = welded ? FindFixtures(p, idx, flames) : FindFixtures(loose, loose_idx, flames);
        const auto box_of = [](uint32_t triangle) { return triangle / 12; };
        CHECK(f.size() == 5);
        // The candlestick, not the table under it or the ground.
        CHECK(f[0].pieces == 1 && f[0].triangles.size() == 12 && box_of(f[0].triangles.front()) == 2);
        // The lantern and its post, not the ground or the wall.
        CHECK(f[1].pieces == 2 && f[1].triangles.size() == 24 && box_of(f[1].triangles.front()) == 3 && box_of(f[1].triangles.back()) == 4);
        // The sconce's arm, not the wall.
        CHECK(f[2].pieces == 1 && box_of(f[2].triangles.front()) == 6);
        // A flame with nothing around it: no fixture (the caller falls back to the sphere around the flame).
        CHECK(f[3].pieces == 0 && f[3].triangles.empty());
        CHECK(std::is_sorted(f[1].triangles.begin(), f[1].triangles.end()));
        // The gallows: the arm the lantern hangs from and its post.
        CHECK(f[4].pieces >= 1 && f[4].triangles.size() == 24 && box_of(f[4].triangles.front()) == 7 && box_of(f[4].triangles.back()) == 8);
    }
    // Left out of the lamp's casters: TrianglesInSphere skips them.
    const TriangleGrid grid = BuildTriangleGrid(p, idx, 4.0f);
    std::vector<uint32_t> near, seen;
    const std::vector<Fixture> f = FindFixtures(p, idx, flames);
    TrianglesInSphere(grid, idx, {5.0f, 2.55f, 5.0f}, 3.0f, near, seen, f[1].triangles);
    // (vertex v belongs to box v / 8)
    const auto boxes = [](const std::vector<uint32_t> &list) {
        std::set<uint32_t> in;
        for (const uint32_t v : list)
            in.insert(v / 8);
        return in;
    };
    CHECK(boxes(near) == std::set<uint32_t>({0, 5, 6})); // the ground, the wall, the sconce's arm
    std::vector<uint32_t> all;
    TrianglesInSphere(grid, idx, {5.0f, 2.55f, 5.0f}, 3.0f, all, seen);
    CHECK(boxes(all) == std::set<uint32_t>({0, 3, 4, 5, 6})); // without the skip: the lantern and its post too
}

void CheckLampShadows()
{
    // Three candles of one candlestick become one lamp; the fireplace stays apart.
    const LightShape candle_shape = ShapeOfGameLight(15.0f, 0.5f, 0.0f, 1.5f, LightFalloff{});
    const LightShape fire_shape = ShapeOfGameLight(40.0f, 1.5f, 0.0f, 0.2f, LightFalloff{});
    const Lamp candle{{1.0f, 2.0f, 1.0f}, {2.2f, 1.5f, 0.5f}, candle_shape.range, candle_shape.intensity, candle_shape.core};
    std::vector<Lamp> lamps = {candle, candle, candle,
                               {{-4.0f, 0.8f, 0.0f}, {2.2f, 1.2f, 0.7f}, fire_shape.range, fire_shape.intensity, fire_shape.core}};
    lamps[1].position[0] += 0.1f;
    lamps[2].position[2] -= 0.1f;
    const std::vector<Lamp> clustered = ClusterLamps(lamps, 0.6f);
    CHECK(clustered.size() == 2 && Near(clustered[0].color[0], 6.6f, 1e-4f));
    CHECK(Near(clustered[0].position[0], 1.0333f, 1e-3f) && Near(clustered[0].position[2], 0.9667f, 1e-3f));

    // A merged fixture keeps the largest shape of its flames (the same whatever their flicker); the fireplace reaches
    // farther than a candle.
    CHECK(ShapeOf(clustered[0]) == candle_shape && clustered[1].range > clustered[0].range);

    // The lamp grid: a cell holds the lamps whose radius reaches it, the strongest there first; cells no lamp reaches
    // hold none; a cell that more lamps reach than it holds keeps the strongest and is counted.
    {
        std::vector<Lamp> row;
        for (int i = 0; i < 3; ++i)
            row.push_back({{2.5f + 5.0f * static_cast<float>(i), 1.5f, 2.5f}, {2.0f, 2.0f, 2.0f}, 3.0f, 0.3f, 0.4f});
        const std::vector<float> far(12, 100.0f);
        const LampGrid grid = BuildLampGrid(row, far, {0.0f, 0.0f, 0.0f}, {15.0f, 3.0f, 5.0f}, 1.0f, 64, 16);
        CHECK(grid.nx == 15 && grid.ny == 3 && grid.nz == 5 && grid.rgba.size() == 2u * 15 * 3 * 5 * 4 && grid.overflow_cells == 0);
        const auto cell = [&](int x, int y, int z, int k) {
            return grid.rgba[((static_cast<size_t>(z) * grid.ny + y) * (2 * grid.nx) + x + (k / 4) * grid.nx) * 4 + k % 4];
        };
        CHECK(cell(2, 1, 2, 0) == 1 && cell(2, 1, 2, 1) == 0);  // the first lamp's own cell
        CHECK(cell(7, 1, 2, 0) == 2 && cell(12, 1, 2, 0) == 3); // the second and the third
        CHECK(cell(5, 1, 2, 0) != 0 && cell(5, 1, 2, 1) != 0 && cell(5, 1, 2, 2) == 0); // between two: both
        CHECK(cell(4, 1, 2, 0) == 1 && cell(4, 1, 2, 1) == 2); // nearer the first: it comes first
        std::vector<Lamp> crowd(12, row[0]);
        crowd[11].color = {9.0f, 9.0f, 9.0f}; // the strongest, last in the list
        const LampGrid full = BuildLampGrid(crowd, far, {0.0f, 0.0f, 0.0f}, {5.0f, 3.0f, 5.0f}, 1.0f, 64, 16);
        CHECK(full.max_lamps == 12 && full.overflow_cells > 0);
        const size_t texel = (static_cast<size_t>(2) * full.ny + 1) * (2 * full.nx) + 2;
        CHECK(full.rgba[texel * 4] == 12 && full.rgba[(texel + full.nx) * 4 + 3] != 0);
        // A lamp with radius 0 (a free slot) is in no cell.
        row[1].range = 0.0f;
        const LampGrid without = BuildLampGrid(row, far, {0.0f, 0.0f, 0.0f}, {15.0f, 3.0f, 5.0f}, 1.0f, 64, 16);
        for (size_t i = 0; i < without.rgba.size(); ++i)
            CHECK(without.rgba[i] != 2);
        // A hall larger than the grid holds cells: larger cells, the same coverage.
        const LampGrid hall = BuildLampGrid(row, far, {0.0f, 0.0f, 0.0f}, {200.0f, 3.0f, 5.0f}, 1.0f, 64, 16);
        CHECK(hall.nx == 64 && Near(hall.cell[0], 200.0f / 64.0f, 1e-4f));
        // A lamp is listed only as far as its light counts (its reach), not over its whole radius.
        const std::vector<float> short_reach = {1.2f, 100.0f, 100.0f};
        const LampGrid gated = BuildLampGrid(row, short_reach, {0.0f, 0.0f, 0.0f}, {15.0f, 3.0f, 5.0f}, 1.0f, 64, 16);
        const auto gated_cell = [&](int x, int y, int z, int k) {
            return gated.rgba[((static_cast<size_t>(z) * gated.ny + y) * (2 * gated.nx) + x + (k / 4) * gated.nx) * 4 + k % 4];
        };
        CHECK(gated_cell(2, 1, 2, 0) == 1 && gated_cell(4, 1, 2, 0) == 0 && cell(4, 1, 2, 0) == 1);
    }

    // The reach of a lamp: where its strongest channel's gamma-space light falls under the level; within its range,
    // farther for a brighter lamp, and the light there is the level.
    {
        const float reach = LampReach(candle, kLampNegligible);
        CHECK(reach > 0.0f && reach < candle.range);
        CHECK(Near(2.2f * LightFalloffGammaAt(ShapeOf(candle), reach), kLampNegligible, 1e-4f));
        Lamp bright = candle;
        bright.color = {4.4f, 3.0f, 1.0f};
        CHECK(LampReach(bright, kLampNegligible) > reach && LampReach(bright, kLampNegligible) <= candle.range);
        Lamp dark = candle;
        dark.color = {0.0f, 0.0f, 0.0f};
        CHECK(LampReach(dark, kLampNegligible) == 0.0f);
    }

    // The still triangles a lamp can shadow with: those that touch its sphere, found through the triangle grid; the
    // same list as testing every triangle, for a lamp anywhere (a carried lantern asks every frame).
    {
        std::vector<Vec3> p;
        std::vector<uint32_t> idx;
        AddBox(p, idx, {-0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, 0.5f});   // at the lamp
        AddBox(p, idx, {20.0f, 0.0f, -0.5f}, {21.0f, 1.0f, 0.5f});  // far away
        AddBox(p, idx, {-30.0f, -1.0f, -30.0f}, {30.0f, 0.0f, 30.0f}); // a floor far larger than the lamp's reach
        for (int i = 0; i < 40; ++i) // furniture all over the floor
        {
            const float x = -28.0f + 1.4f * static_cast<float>(i), z = 25.0f * std::sin(1.7f * static_cast<float>(i));
            AddBox(p, idx, {x, 0.0f, z}, {x + 0.8f, 0.9f, z + 0.8f});
        }
        const TriangleGrid grid = BuildTriangleGrid(p, idx, 4.0f);
        CHECK(grid.count == idx.size() / 3 && grid.size[0] >= 15 && grid.size[0] <= kTriangleGridCells && grid.size[1] == 1);
        std::vector<uint32_t> near, seen;
        TrianglesInSphere(grid, idx, {0.0f, 2.0f, 0.0f}, 4.0f, near, seen);
        CHECK(near.size() % 3 == 0 && near.size() >= 36 + 6 && near.size() < idx.size());
        bool far_box = false, floor_top = false;
        for (const uint32_t i : near)
        {
            far_box |= i >= 8 && i < 16;
            floor_top |= i >= 16 && i < 24;
        }
        CHECK(!far_box && floor_top);
        bool same = true;
        for (int probe = 0; probe < 60; ++probe)
        {
            const Vec3 lamp{-29.0f + static_cast<float>(probe), 1.5f, 20.0f * std::cos(0.9f * static_cast<float>(probe))};
            const float reach = 1.0f + 0.2f * static_cast<float>(probe % 30);
            std::vector<uint32_t> expected, got;
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
            {
                Vec3 lo = p[idx[t]], hi = p[idx[t]];
                for (int v = 1; v < 3; ++v)
                    for (int k = 0; k < 3; ++k)
                    {
                        lo[k] = std::min(lo[k], p[idx[t + v]][k]);
                        hi[k] = std::max(hi[k], p[idx[t + v]][k]);
                    }
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float nearest = std::clamp(lamp[k], lo[k], hi[k]);
                    d2 += (nearest - lamp[k]) * (nearest - lamp[k]);
                }
                if (d2 <= reach * reach)
                    expected.insert(expected.end(), {idx[t], idx[t + 1], idx[t + 2]});
            }
            TrianglesInSphere(grid, idx, lamp, reach, got, seen);
            same &= got == expected;
        }
        CHECK(same);
        // Nothing for a lamp outside the place, and for a place without triangles.
        near.clear();
        TrianglesInSphere(grid, idx, {500.0f, 0.0f, 0.0f}, 5.0f, near, seen);
        TrianglesInSphere(BuildTriangleGrid(p, {}, 4.0f), {}, {0.0f, 0.0f, 0.0f}, 5.0f, near, seen);
        CHECK(near.empty());
    }

    // The room light: the baked colours around a point, mostly those of the level surfaces. A room with a bright
    // floor and dark walls and ceiling, one half lit warm: the light where one stands is close to the floor's (the
    // walls count little), the warm half is warm, the upper storey's cell is darker, and it is the same every time.
    {
        std::vector<Vec3> p;
        std::vector<uint32_t> idx;
        AddBox(p, idx, {0.0f, 0.0f, 0.0f}, {12.0f, 6.0f, 6.0f});
        std::vector<uint32_t> colors(p.size());
        for (size_t i = 0; i < p.size(); ++i)
        {
            const uint32_t level = p[i][1] < 1.0f ? 160u : 60u;
            const uint32_t blue = p[i][0] < 6.0f ? level / 3u : level;
            colors[i] = 0xff000000u | (level << 16) | (level << 8) | blue;
        }
        const RoomLight room = BuildRoomLight(p, colors, idx, {0.0f, -0.5f, 0.0f}, {12.0f, 8.5f, 6.0f}, 1.5f, 3.0f, 48, 12);
        CHECK(room.nx == 8 && room.ny == 3 && room.nz == 4 && room.rgba.size() == 8u * 3 * 4 * 4);
        const auto at = [&](int x, int y, int z, int ch) { return room.rgba[((static_cast<size_t>(z) * room.ny + y) * room.nx + x) * 4 + ch]; };
        CHECK(at(4, 0, 2, 0) > 110 && at(4, 0, 2, 0) <= 160); // where one stands: near the floor's 160, above the mean
        CHECK(at(4, 0, 2, 0) > at(4, 2, 2, 0) + 40);          // under the ceiling it is the ceiling's
        CHECK(at(1, 0, 1, 2) + 20 < at(6, 0, 1, 2));          // the warm half has less blue
        const RoomLight again = BuildRoomLight(p, colors, idx, {0.0f, -0.5f, 0.0f}, {12.0f, 8.5f, 6.0f}, 1.5f, 3.0f, 48, 12);
        CHECK(again.rgba == room.rgba);
        CHECK(BuildRoomLight(p, colors, idx, {50.0f, 0.0f, 0.0f}, {60.0f, 6.0f, 6.0f}, 1.5f, 3.0f, 48, 12).rgba.empty());
    }

    // Caster side (face view-projection + tile, the distance fs_ff_lamp_caster writes) and receiver side
    // (LampFaceCoord, the distance to the lamp) agree on atlas position and distance in every face, and the guard
    // band keeps points of a face inside its tile.
    const Vec3 lamp{0.5f, 2.0f, -1.0f};
    const float far_plane = 12.0f;
    const int columns = 6, rows = 4;
    int faces_seen = 0;
    for (int i = 0; i < 200; ++i)
    {
        const float a = static_cast<float>(i) * 2.399963f, y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 200.0f;
        const float r = std::sqrt(1.0f - y * y), distance = 0.05f + 0.05f * static_cast<float>(i % 100);
        const Vec3 p{lamp[0] + r * std::cos(a) * distance, lamp[1] + y * distance, lamp[2] + r * std::sin(a) * distance};
        for (const int slot : {0, 3})
        {
            const LampFaceSample s = LampFaceCoord(lamp, p, slot, columns, rows);
            faces_seen |= 1 << s.face;
            const auto vp = LampFaceViewProj(lamp, s.face, far_plane);
            const auto tile_clip = TileClip(LampAtlasTile(slot, s.face, columns, rows));
            float clip[4], atlas[4];
            for (int j = 0; j < 4; ++j)
                clip[j] = p[0] * vp[j] + p[1] * vp[4 + j] + p[2] * vp[8 + j] + vp[12 + j];
            for (int j = 0; j < 4; ++j)
                atlas[j] = clip[0] * tile_clip[j] + clip[1] * tile_clip[4 + j] + clip[2] * tile_clip[8 + j] + clip[3] * tile_clip[12 + j];
            const float x = atlas[0] / atlas[3] * 0.5f + 0.5f, yy = 0.5f - atlas[1] / atlas[3] * 0.5f;
            CHECK(Near(x, s.x, 1e-4f) && Near(yy, s.y, 1e-4f));
            CHECK(Near(LampCasterDistance(clip[0], clip[1], clip[3]), distance, 1e-3f));
            // The depth the face stores (its own projection) is the one the receiver computes from the distance
            // along the face's axis.
            CHECK(Near(LampFaceDepth(clip[3], far_plane), clip[2] / clip[3], 1e-5f));
            // In front of the near plane (a table top 15 cm under its candle, a wall 3 cm behind its sconce).
            CHECK(clip[3] > kLampShadowNear && clip[2] / clip[3] > 0.0f && clip[2] / clip[3] < 1.0f);
            const AtlasTile t = LampAtlasTile(slot, s.face, columns, rows);
            const float margin = 0.5f * (1.0f - 1.0f / kLampFaceGuard) * t.sx * 0.99f;
            CHECK(s.x >= t.x0 + margin && s.x <= t.x0 + t.sx - margin && s.y >= t.y0 && s.y <= t.y0 + t.sy);
        }
    }
    CHECK(faces_seen == 63);
    // A character 2 m in front of the lamp on +Z is in that face only; one straddling the edge in two; one at the
    // lamp in all six.
    int in_faces = 0, edge_faces = 0, at_lamp = 0;
    for (int f = 0; f < 6; ++f)
    {
        in_faces += SphereInLampFace(lamp, f, {lamp[0], lamp[1], lamp[2] + 2.0f}, 0.4f) ? 1 : 0;
        edge_faces += SphereInLampFace(lamp, f, {lamp[0] + 2.0f, lamp[1], lamp[2] + 2.0f}, 0.4f) ? 1 : 0;
        at_lamp += SphereInLampFace(lamp, f, lamp, 0.4f) ? 1 : 0;
    }
    CHECK(in_faces == 1 && edge_faces == 2 && at_lamp == 6);

    // The still triangles sorted into the faces: a floor quad under the lamp lands in the down face (and the side
    // faces its corners reach), never in the up face; a small triangle next to the flame is in the holder lists,
    // the floor not.
    {
        const std::vector<Vec3> p = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f},
                                     {0.55f, 2.0f, -1.0f}, {0.6f, 2.0f, -1.0f}, {0.55f, 2.05f, -1.0f}};
        const std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3, 4, 5, 6};
        LampFaceLists lists;
        SplitByLampFace(p, idx, lamp, 0.12f, lists);
        const auto count = [&](int face, int near) { return (lists.first[face * 2 + near + 1] - lists.first[face * 2 + near]) / 3; };
        CHECK(count(3, 0) == 2 && count(2, 0) == 0);
        int near = 0;
        for (int f = 0; f < 6; ++f)
        {
            near += static_cast<int>(count(f, 1));
            for (uint32_t i = lists.first[f * 2 + 1]; i < lists.first[f * 2 + 2]; ++i)
                CHECK(lists.indices[i] >= 4); // only the triangle by the flame
        }
        CHECK(near >= 1 && lists.first[12] == lists.indices.size());
    }
}

// Materials whose texture alpha cuts holes (fx/cutout.hpp): the CPU passes cut what the colour pass's alpha test cuts.
void CheckCutout()
{
    // The alpha of a checkerboard of 8 x 8 squares, 255 / 0, and of an opaque image.
    const int n = 64;
    std::vector<uint8_t> checker(static_cast<size_t>(n) * n, 255), solid(static_cast<size_t>(n) * n, 255);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            checker[static_cast<size_t>(y) * n + x] = ((x / 8 + y / 8) & 1) ? 0 : 255;
    const float reference = 160.0f / 255.0f;
    AlphaMask mask, none;
    CHECK(MakeAlphaMask(std::vector<uint8_t>(checker), n, n, reference, mask) && mask.width == n && mask.alpha == checker);
    CHECK(!MakeAlphaMask(std::vector<uint8_t>(solid), n, n, reference, none) && none.alpha.empty()); // no holes: whole
    solid[0] = 0; // one transparent texel in 4096 is not a cut-out
    CHECK(!MakeAlphaMask(std::move(solid), n, n, reference, none));
    CHECK(mask.At(0.06f, 0.06f) == 1.0f && mask.At(0.19f, 0.06f) == 0.0f && mask.At(1.06f, -0.94f) == 1.0f); // wraps
}

void CheckSunFit()
{
    // A 40 x 40 m square with three houses; its "lightmap" is dark where the houses shade it from a sun 200° round
    // and 60° up, with some noise. The fit finds that sun, not the one of the hour.
    std::vector<Vec3> p;
    std::vector<uint32_t> idx;
    AddBox(p, idx, {-8.0f, 0.0f, -6.0f}, {-2.0f, 6.0f, 0.0f});
    AddBox(p, idx, {4.0f, 0.0f, 3.0f}, {9.0f, 9.0f, 7.0f});
    AddBox(p, idx, {-3.0f, 0.0f, 8.0f}, {1.0f, 4.0f, 12.0f});
    const Occluders occluders(p, idx);
    const float az = 200.0f * 0.017453293f, el = 60.0f * 0.017453293f;
    const Vec3 baked{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    std::vector<SunFitSample> samples;
    uint32_t seed = 7;
    auto random = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    for (int i = 0; i < 1500; ++i)
    {
        const Vec3 q{-20.0f + 40.0f * random(), 0.0f, -20.0f + 40.0f * random()};
        const bool shade = occluders.Occluded({q[0], 0.05f, q[2]}, baked, 200.0f);
        samples.push_back({q, (shade ? 0.25f : 0.7f) + 0.15f * (random() - 0.5f)});
    }
    const SunFit fit = FitBakedSun(occluders, samples, 200.0f, 4);
    const float off = std::acos(std::clamp(fit.dir[0] * baked[0] + fit.dir[1] * baked[1] + fit.dir[2] * baked[2], -1.0f, 1.0f)) * 57.29578f;
    std::printf("sun fit: %.1f° off the bake, r %.2f, %.0f %% shaded\n", off, fit.r, 100.0f * fit.shadowed);
    CHECK(fit.valid && off < 3.0f && fit.r > 0.9f);
    CHECK(!FitBakedSun(occluders, {}).valid); // no samples: no direction
}

void CheckTownLight()
{
    // A 20 x 20 m square (its lightmap: u = (x + 10) / 20, v = (z + 10) / 20) and a 4 m cube in the middle. The sun
    // from 45° up in the west (-x): the cube's shadow falls east of it.
    std::vector<Vec3> p{{-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 10.0f}};
    std::vector<std::array<float, 2>> uv{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    std::vector<uint32_t> idx{0, 2, 1, 0, 3, 2};
    std::vector<uint32_t> map{0, 0};
    const LightmapTexels texels = RasterizeLightmap(p, uv, idx, map, 0, 64, 64);
    CHECK(texels.texel.size() == 64 * 64 && texels.gutter.empty()); // the whole square: no gutter left
    std::vector<Vec3> occ;
    std::vector<uint32_t> occ_idx;
    AddBox(occ, occ_idx, {-2.0f, 0.0f, -2.0f}, {2.0f, 4.0f, 2.0f});
    const Occluders occluders(occ, occ_idx);
    const Vec3 sun{-0.7071f, 0.7071f, 0.0f};
    const std::vector<uint8_t> vis = SunVisibility(occluders, texels, sun, 20.0f / 64.0f, 4, 2);
    // The sky on the texels: read from the place's probes (their near sky), one cell off the ground.
    std::vector<Vec3> place = p;
    std::vector<uint32_t> place_idx = idx;
    const uint32_t first_occluder = static_cast<uint32_t>(place.size());
    place.insert(place.end(), occ.begin(), occ.end());
    for (const uint32_t i : occ_idx)
        place_idx.push_back(first_occluder + i);
    ProbeParams probe_params;
    probe_params.cell = 0.5f;
    probe_params.near_sky = 10.0f;
    probe_params.threads = 2;
    const PlaceProbes probes = BakePlaceProbes(occluders, place, place_idx, probe_params);
    const std::vector<uint8_t> sky = TexelSky(texels, probes.visibility, probes.near_sky);
    CHECK(TexelSky(texels, {}, {}) == std::vector<uint8_t>(texels.texel.size(), 255)); // no probes: open
    auto at = [&](const std::vector<uint8_t> &v, float x, float z) {
        const int tx = static_cast<int>((x + 10.0f) / 20.0f * 64.0f), tz = static_cast<int>((z + 10.0f) / 20.0f * 64.0f);
        for (size_t i = 0; i < texels.texel.size(); ++i)
            if (texels.texel[i] == static_cast<uint32_t>(tz * 64 + tx))
                return v[i] / 255.0f;
        return -1.0f;
    };
    std::printf("town light: sun east of the cube %.2f, west %.2f; sky by the cube %.2f, open %.2f\n", at(vis, 4.0f, 0.0f),
                at(vis, -6.0f, 0.0f), at(sky, 2.4f, 0.0f), at(sky, -8.0f, -8.0f));
    CHECK(at(vis, 4.0f, 0.0f) < 0.1f && std::fabs(at(vis, -6.0f, 0.0f) - 0.707f) < 0.01f && at(vis, 8.0f, 6.0f) > 0.69f);
    // A low sun lights the open ground less: 15° up, the cosine.
    const std::vector<uint8_t> low = SunVisibility(occluders, texels, {-0.9659f, 0.2588f, 0.0f}, 20.0f / 64.0f, 4, 2);
    CHECK(std::fabs(at(low, -8.0f, 6.0f) - 0.2588f) < 0.01f);
    CHECK(at(sky, 2.4f, 0.0f) < at(sky, -8.0f, -8.0f) - 0.2f && at(sky, -8.0f, -8.0f) > 0.8f);
    // A "game" lightmap: paint (stripes of grass and sand) under a known light, 0.06 + 0.28 sun + 0.06 sky.
    std::vector<uint8_t> game(64 * 64 * 4, 255);
    for (size_t i = 0; i < texels.texel.size(); ++i)
    {
        const float paint = (texels.texel[i] / 64) % 8 < 4 ? 1.0f : 0.6f;
        const float light = 0.06f + 0.28f * vis[i] / 255.0f + 0.06f * sky[i] / 255.0f;
        for (int ch = 0; ch < 3; ++ch)
            game[texels.texel[i] * 4 + ch] = static_cast<uint8_t>(std::lround(paint * light * 255.0f));
    }
    const TownLightTone tone = CalibrateTownLight(texels, game, vis, sky);
    std::printf("town light tone: %.3f + %.3f sun + %.3f sky (R² %.2f, %u shadow edges)\n", tone.a, tone.b, tone.c, tone.r2, tone.edges);
    // The game's shadow depth under open sky (shadowed over lit, the sun 45° up), taken from its edges: the least
    // squares alone (a, c low, b 0.22) would make it shallower.
    const float depth = (tone.a + tone.c) / (tone.a + tone.c + tone.b * 0.7071f);
    CHECK(tone.valid && tone.edges >= 50 && std::fabs(depth - 0.12f / (0.12f + 0.28f * 0.7071f)) < 0.02f);
    // The relight itself runs on the GPU (fx/sun_relight.hpp): tests/renderer_bgfx/sun_relight_check.cpp.
}

std::atomic<int> g_hot_calls{0};
const char *HotTwice()
{
    return g_hot_calls.fetch_add(1) < 3 ? "check: hot" : nullptr;
}

void CheckBakeThermal()
{
    // The bakes' pieces wait while the device is hot and then run all; the result is that of one thread.
    std::vector<int> once(5000, 0), hot(5000, 0);
    ParallelRanges(once.size(), 1, [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i)
            once[i] = static_cast<int>(i * 7 % 13);
    });
    SetBakeGate(&HotTwice, 10);
    const auto started = std::chrono::steady_clock::now();
    ParallelRanges(hot.size(), 4, [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i)
            hot[i] = static_cast<int>(i * 7 % 13);
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    SetBakeGate(nullptr);
    std::printf("bake thermal: %d status calls, %.0f ms with a pause of 10 ms\n", g_hot_calls.load(), ms);
    CHECK(hot == once && g_hot_calls.load() > 3 && ms >= 10.0);
    // Few heavy items (the sun fit's candidates) still spread over the threads.
    std::atomic<int> pieces{0};
    ParallelRanges(8, 4, [&](size_t first, size_t last) { pieces += 1; (void)first; (void)last; }, 1);
    CHECK(pieces.load() == 8);
}

const char *AlwaysHot()
{
    return "check: always hot";
}

template <class Done> bool WaitFor(Done done)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return done();
}

double MsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void CheckBakeCancel()
{
    // The bakes run on their own thread: a bake in the thermal pause (the device stays hot, the pause a minute) is
    // woken and stopped when its owner lets go of it, a bake queued behind it and let go of never runs, the next one
    // runs at once; ShutdownBakes stops a paused bake and returns at once; a bake posted after it runs again.
    SetBakeGate(&AlwaysHot, 60000);
    std::atomic<bool> started{false}, finished{false}, queued_ran{false}, marker{false};
    std::atomic<int> pieces{0};
    auto paused_bake = [&] {
        started = true;
        ParallelRanges(1000, 2, [&](size_t, size_t) { ++pieces; }, 1);
        finished = true; // not reached: ParallelRanges throws BakeCancelled
    };
    BakeTicket paused = PostBake("check: paused", paused_bake);
    BakeTicket queued = PostBake("check: queued", [&] { queued_ran = true; });
    CHECK(WaitFor([&] { return started.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // in the pause by now
    queued.reset();
    auto t = std::chrono::steady_clock::now();
    paused.reset();
    BakeTicket next = PostBake("check: next", [&] { marker = true; });
    CHECK(WaitFor([&] { return marker.load(); }));
    const double cancel_ms = MsSince(t);
    std::printf("bake cancel: the paused bake let go of, the next one ran after %.0f ms; %d pieces, queued ran %d\n",
                cancel_ms, pieces.load(), queued_ran.load() ? 1 : 0);
    CHECK(!finished && pieces == 0 && !queued_ran && cancel_ms < 2000.0);

    started = false;
    BakeTicket again = PostBake("check: paused at shutdown", paused_bake);
    CHECK(WaitFor([&] { return started.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    t = std::chrono::steady_clock::now();
    ShutdownBakes();
    const double shutdown_ms = MsSince(t);
    std::printf("bake shutdown with a bake in the thermal pause: %.0f ms\n", shutdown_ms);
    CHECK(!finished && pieces == 0 && shutdown_ms < 2000.0);
    SetBakeGate(nullptr);

    marker = false;
    next = PostBake("check: after the shutdown", [&] { marker = true; });
    CHECK(WaitFor([&] { return marker.load(); }));
    ShutdownBakes();
}

void CheckBakeLoads()
{
    // A result in the cache (a bake's load stage) does not wait behind a running bake: the same cache read posted as
    // a plain bake waits for the bake running before it, posted with a load stage it is done at once.
    std::atomic<bool> release{false}, running{false};
    auto long_bake = [&] {
        running = true;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        running = false;
    };
    constexpr auto kHold = std::chrono::milliseconds(300);
    std::atomic<bool> loaded{false};
    BakeTicket hold = PostBake("check: long bake", long_bake);
    CHECK(WaitFor([&] { return running.load(); }));
    auto t = std::chrono::steady_clock::now();
    BakeTicket plain = PostBake("check: cache read as a bake", [&] { loaded = true; });
    std::this_thread::sleep_for(kHold);
    release = true;
    CHECK(WaitFor([&] { return loaded.load(); }));
    const double plain_ms = MsSince(t);

    release = false;
    loaded = false;
    std::atomic<int> baked{0};
    hold = PostBake("check: long bake", long_bake);
    CHECK(WaitFor([&] { return running.load(); }));
    t = std::chrono::steady_clock::now();
    BakeTicket staged = PostBake("check: cache read as a load", [&](BakeStage stage) {
        if (stage == BakeStage::Bake)
            ++baked;
        loaded = true;
        return true; // found in the cache
    });
    CHECK(WaitFor([&] { return loaded.load(); }));
    const double load_ms = MsSince(t);
    const bool still_running = running.load();
    // Not in the cache: the bake stage follows on the bake thread, after the running bake.
    std::atomic<bool> missed{false};
    BakeTicket miss = PostBake("check: not in the cache", [&](BakeStage stage) {
        if (stage == BakeStage::Load)
            return false;
        missed = true;
        return true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool missed_early = missed.load();
    release = true;
    CHECK(WaitFor([&] { return missed.load(); }));
    std::printf("bake loads: a cache read behind a running bake waited %.0f ms as a bake, %.1f ms as a load (the "
                "bake still running: %d); a miss is baked after it (%d)\n",
                plain_ms, load_ms, still_running ? 1 : 0, missed_early ? 0 : 1);
    CHECK(plain_ms >= 300.0 && load_ms < 100.0 && still_running && baked == 0 && !missed_early);
    ShutdownBakes();
}

// A closed solid with its triangles wound outwards (AddBox winds them inwards): the rays must not care which.
void AddSolid(std::vector<Vec3> &p, std::vector<uint32_t> &i, Vec3 lo, Vec3 hi)
{
    const size_t first = i.size();
    AddBox(p, i, lo, hi);
    for (size_t t = first; t + 2 < i.size(); t += 3)
        std::swap(i[t + 1], i[t + 2]);
}

// The rays of the place's bakes (fx::Occluders over the bvh library) against a test of every triangle.
void CheckOccluders()
{
    // A few hundred random triangles in a 20 m cube, a third of them with a checkerboard cut into them.
    uint32_t seed = 11;
    auto random = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    std::vector<Vec3> p;
    std::vector<std::array<float, 2>> uv;
    std::vector<uint32_t> idx;
    const int n = 32;
    AlphaMask checker;
    checker.width = checker.height = n;
    checker.alpha.resize(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            checker.alpha[static_cast<size_t>(y) * n + x] = ((x / 4 + y / 4) & 1) ? 0 : 255;
    const auto mask = std::make_shared<const AlphaMask>(checker);
    std::vector<Occluders::Cut> cut;
    for (int t = 0; t < 400; ++t)
    {
        const Vec3 c{20.0f * random() - 10.0f, 20.0f * random() - 10.0f, 20.0f * random() - 10.0f};
        for (int k = 0; k < 3; ++k)
        {
            p.push_back({c[0] + 3.0f * random() - 1.5f, c[1] + 3.0f * random() - 1.5f, c[2] + 3.0f * random() - 1.5f});
            uv.push_back({random() * 2.0f, random() * 2.0f});
            idx.push_back(static_cast<uint32_t>(p.size() - 1));
        }
        cut.push_back(t % 3 == 0 ? Occluders::Cut{mask, 0.5f} : Occluders::Cut{});
    }
    idx.insert(idx.end(), {0, 0, 1}); // a triangle without an area: hit by nothing, the numbers of the others stay
    cut.push_back({});
    const Occluders occluders(p, idx, uv, cut);
    CHECK(!occluders.Empty() && occluders.Triangles() == 401 && occluders.Bytes() > 0);
    // Every triangle by Möller-Trumbore, as the uniform grid it replaces tested them.
    auto brute = [&](const Vec3 &o, const Vec3 &d, float reach, std::span<const uint32_t> ignore) {
        std::optional<Occluders::Hit> best;
        for (uint32_t t = 0; t < 400; ++t)
        {
            const Vec3 &a = p[idx[3 * t]], &b = p[idx[3 * t + 1]], &c = p[idx[3 * t + 2]];
            const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const Vec3 pv{d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-9f)
                continue;
            const Vec3 s{o[0] - a[0], o[1] - a[1], o[2] - a[2]};
            const float u = (s[0] * pv[0] + s[1] * pv[1] + s[2] * pv[2]) / det;
            const Vec3 q{s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
            const float dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
            if (u < 0.0f || v < 0.0f || u + v > 1.0f || dist <= 1e-3f || dist >= reach || (best && dist >= best->t))
                continue;
            if (cut[t].mask)
            {
                const float w = 1.0f - u - v;
                if (cut[t].mask->At(w * uv[idx[3 * t]][0] + u * uv[idx[3 * t + 1]][0] + v * uv[idx[3 * t + 2]][0],
                                    w * uv[idx[3 * t]][1] + u * uv[idx[3 * t + 1]][1] + v * uv[idx[3 * t + 2]][1]) <= 0.5f)
                    continue;
            }
            if (std::binary_search(ignore.begin(), ignore.end(), t))
                continue;
            best = Occluders::Hit{dist, t, u, v};
        }
        return best;
    };
    int rays = 0, hits = 0, wrong = 0, wrong_any = 0, through_holes = 0;
    for (int r = 0; r < 4000; ++r)
    {
        const Vec3 o{24.0f * random() - 12.0f, 24.0f * random() - 12.0f, 24.0f * random() - 12.0f};
        Vec3 d{random() - 0.5f, random() - 0.5f, random() - 0.5f};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 0.05f)
            continue;
        for (float &x : d)
            x /= l;
        const float reach = 5.0f + 25.0f * random();
        const std::optional<Occluders::Hit> expected = brute(o, d, reach, {}), found = occluders.Closest(o, d, reach);
        ++rays;
        hits += expected ? 1 : 0;
        // (A ray through a shared edge or a hair from a hole's rim may differ: none of these triangles share edges,
        // and the count below allows a few rim cases.)
        if (expected.has_value() != found.has_value() ||
            (expected && (expected->triangle != found->triangle || !Near(expected->t, found->t, 1e-3f) ||
                          !Near(expected->u, found->u, 1e-3f) || !Near(expected->v, found->v, 1e-3f))))
            ++wrong;
        if (occluders.Occluded(o, d, reach) != expected.has_value())
            ++wrong_any;
        if (expected)
        {
            // The same ray without its nearest triangle: the next one, or nothing.
            const uint32_t ignore[1] = {expected->triangle};
            if (occluders.Occluded(o, d, reach, ignore) != brute(o, d, reach, ignore).has_value())
                ++wrong_any;
        }
    }
    // What a solid version of the same triangles stops and this one lets through its holes.
    const Occluders solid(p, idx);
    for (int r = 0; r < 500; ++r)
    {
        const Vec3 o{20.0f * random() - 10.0f, 12.0f, 20.0f * random() - 10.0f}, down{0.0f, -1.0f, 0.0f};
        const auto with_holes = occluders.Closest(o, down, 30.0f), without = solid.Closest(o, down, 30.0f);
        through_holes += without && (!with_holes || with_holes->triangle != without->triangle) ? 1 : 0;
    }
    std::printf("occluders: %d rays over 400 triangles, %d hit; %d nearest hits and %d any-hits differ from the test of "
                "every triangle; %d of 500 rays pass a hole a solid triangle would stop\n",
                rays, hits, wrong, wrong_any, through_holes);
    CHECK(hits > rays / 10 && wrong <= 2 && wrong_any <= 2 && through_holes > 20);
    CHECK(Occluders({}, {}).Empty() && !Occluders({}, {}).Occluded({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 10.0f));
    // Two triangles 20 km apart, a backdrop island beside a town: no grid to cap, both are hit, nothing between.
    const std::vector<Vec3> far = {{0.0f, 0.0f, 0.0f},           {0.0f, 0.0f, 1.0f},           {1.0f, 0.0f, 0.0f},
                                   {20000.0f, 500.0f, 20000.0f}, {20000.0f, 500.0f, 20001.0f}, {20001.0f, 500.0f, 20000.0f}};
    const std::vector<uint32_t> far_idx = {0, 1, 2, 3, 4, 5};
    const Occluders apart(far, far_idx);
    const Vec3 down{0.0f, -1.0f, 0.0f};
    CHECK(apart.Occluded({0.2f, 5.0f, 0.2f}, down, 10.0f) && apart.Occluded({20000.2f, 505.0f, 20000.2f}, down, 10.0f) &&
          !apart.Occluded({5000.0f, 505.0f, 5000.0f}, down, 600.0f));
}

// The place's probes (fx/place_probes.hpp): one set of rays for the visibility, the near sky and the occlusion lobes.
void CheckPlaceProbes()
{
    // Outdoors: a 40 x 40 m ground; a closed hall (10 x 10 m, its ceiling 12 m up); an awning 3 m over a 6 x 6 m patch.
    std::vector<Vec3> p;
    std::vector<uint32_t> idx;
    AddSolid(p, idx, {-20.0f, -0.5f, -20.0f}, {20.0f, 0.0f, 20.0f});
    AddSolid(p, idx, {5.0f, 0.0f, 5.0f}, {5.3f, 12.0f, 15.0f});
    AddSolid(p, idx, {14.7f, 0.0f, 5.0f}, {15.0f, 12.0f, 15.0f});
    AddSolid(p, idx, {5.0f, 0.0f, 5.0f}, {15.0f, 12.0f, 5.3f});
    AddSolid(p, idx, {5.0f, 0.0f, 14.7f}, {15.0f, 12.0f, 15.0f});
    AddSolid(p, idx, {5.0f, 12.0f, 5.0f}, {15.0f, 12.3f, 15.0f});
    AddSolid(p, idx, {-12.0f, 3.0f, -12.0f}, {-6.0f, 3.1f, -6.0f});
    const Occluders occluders(p, idx);
    ProbeParams params;
    params.cell = 1.0f;
    params.near_sky = 10.0f;
    params.threads = 1;
    const PlaceProbes one = BakePlaceProbes(occluders, p, idx, params);
    params.threads = 4;
    const PlaceProbes four = BakePlaceProbes(occluders, p, idx, params);
    const VisibilityVolume &v = one.visibility;
    const auto open = v.Sample({-15.0f, 1.0f, 12.0f}), hall = v.Sample({10.0f, 1.5f, 10.0f}), awning = v.Sample({-9.0f, 1.0f, -9.0f});
    std::printf("probes: %dx%dx%d cells of %.2f m, %.2f million rays; open ground sky %.2f open %.2f, hall %.2f %.2f, awning "
                "%.2f %.2f\n",
                v.size[0], v.size[1], v.size[2], v.cell, static_cast<double>(one.rays) * 1e-6, open[0], open[1], hall[0],
                hall[1], awning[0], awning[1]);
    // The thread count does not change a byte of it.
    CHECK(!one.Empty() && v.sky_open == four.visibility.sky_open && one.near_sky == four.near_sky && one.ao.Empty());
    CHECK(one.rays == static_cast<uint64_t>(v.Cells()) * kSkyRays); // outdoors: the sky's rays only
    CHECK(open[0] > 0.9f && open[1] > 0.9f);
    CHECK(hall[0] < 0.05f && hall[1] < 0.05f); // a ceiling 12 m up closes it
    // A wall read one cell off it (as ffVisibility does) on either side: the hall's side closed, the street's open.
    const float inside_wall = v.Sample({5.3f + v.cell, 1.5f, 10.0f})[0], outside_wall = v.Sample({5.0f - v.cell, 1.5f, 10.0f})[0];
    std::printf("probes: by the hall's wall inside %.2f, outside %.2f\n", inside_wall, outside_wall);
    CHECK(inside_wall < 0.15f && outside_wall > 0.4f); // outside: the 12 m wall itself hides half the sky
    CHECK(awning[1] < 0.5f && awning[0] < open[0] - 0.3f);
    // The near sky (10 m): the hall's 12 m ceiling is beyond it, its walls are not; the awning closes it as the far one.
    const float near_hall = SampleCells(v, one.near_sky, {10.0f, 1.5f, 10.0f}), near_open = SampleCells(v, one.near_sky, {-15.0f, 1.0f, 12.0f}),
                near_awning = SampleCells(v, one.near_sky, {-9.0f, 1.0f, -9.0f});
    std::printf("probes: near sky in the hall %.2f, open ground %.2f, under the awning %.2f\n", near_hall, near_open, near_awning);
    CHECK(near_hall > hall[0] + 0.2f && near_open > 0.9f && near_awning < near_open - 0.3f);
    CHECK(VisibilityVolume{}.Sample({0.0f, 0.0f, 0.0f})[0] == 1.0f); // none: open
    // The cells stay within the budget and a 3D texture's side.
    ProbeParams coarse = params;
    coarse.cell = 0.05f;
    coarse.max_cells = 32 * 32 * 32;
    const PlaceProbes few = BakePlaceProbes(occluders, p, idx, coarse);
    CHECK(few.visibility.Cells() <= coarse.max_cells && few.visibility.cell > 0.05f);
    CHECK(BakePlaceProbes(occluders, {}, {}, params).Empty()); // nothing to lay them over

    // An interior: a 6 x 3 x 6 m room (its walls face inwards), a table (top at 0.8 m), a barrel 0.7 m from the +x
    // wall, ceiling beams 0.4 m under the ceiling; the occlusion lobes on the room's cells.
    std::vector<Vec3> rp;
    std::vector<uint32_t> ridx;
    AddBox(rp, ridx, {-3.0f, 0.0f, -3.0f}, {3.0f, 3.0f, 3.0f});
    AddSolid(rp, ridx, {-0.6f, 0.76f, -0.6f}, {0.6f, 0.8f, 0.6f});
    AddSolid(rp, ridx, {1.7f, 0.0f, -0.3f}, {2.3f, 0.9f, 0.3f});
    for (float z = -2.5f; z <= 2.5f; z += 1.0f)
        AddSolid(rp, ridx, {-3.0f, 2.6f, z - 0.1f}, {3.0f, 2.8f, z + 0.1f});
    const Occluders room(rp, ridx);
    ProbeParams interior;
    interior.cell = 0.2f;
    interior.max_cells = 1u << 20;
    interior.turned = false;
    interior.ao_radius = 1.5f;
    interior.threads = 1;
    const auto started = std::chrono::steady_clock::now();
    const PlaceProbes probes = BakePlaceProbes(room, rp, ridx, interior);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const AoVolume &volume = probes.ao;
    std::printf("probes, a room: %dx%dx%d cells of %.2f m, %.2f million rays in %.0f ms (1 thread)\n", volume.size[0],
                volume.size[1], volume.size[2], volume.cell, static_cast<double>(probes.rays) * 1e-6, ms);
    CHECK(!volume.Empty() && Near(volume.cell, 0.2f, 1e-6f) && volume.size == probes.visibility.size);
    CHECK(volume.size[0] == 32 && volume.size[1] == 17 && volume.size[2] == 32);
    // The room is closed: no sky, no sun in it.
    const auto in_room = probes.visibility.Sample({-1.5f, 1.0f, -1.5f});
    CHECK(in_room[0] < 0.02f && in_room[1] < 0.02f);
    // Receivers sample one cell off the surface along the normal (as the shader does).
    const Vec3 up{0.0f, 1.0f, 0.0f}, west{-1.0f, 0.0f, 0.0f};
    const float floor = volume.Sample({-1.5f, 0.2f, -1.5f}, up), under_table = volume.Sample({0.0f, 0.2f, 0.0f}, up),
                corner = volume.Sample({2.8f, 0.2f, 2.8f}, up), behind_barrel = volume.Sample({2.65f, 0.2f, 0.0f}, up),
                in_front_of_barrel = volume.Sample({1.0f, 0.2f, 0.0f}, up), mid_air = volume.Sample({-1.5f, 1.6f, -1.5f}, up),
                wall = volume.Sample({2.8f, 1.5f, -1.5f}, west), wall_under_beams = volume.Sample({2.8f, 2.35f, -1.5f}, west);
    std::printf("probes, a room: open floor %.2f, under the table %.2f, corner %.2f, behind / in front of the barrel %.2f / "
                "%.2f, wall %.2f, wall under the beams %.2f, mid-air %.2f\n",
                floor, under_table, corner, behind_barrel, in_front_of_barrel, wall, wall_under_beams, mid_air);
    CHECK(Near(floor, kAoOpenSurface, 0.03f) && Near(wall, kAoOpenSurface, 0.03f)); // the shader's reference
    CHECK(under_table < 0.85f * floor);        // the shader scales the contrast (fx strength)
    CHECK(behind_barrel < 0.75f * floor);      // ... and behind the barrels
    CHECK(behind_barrel < in_front_of_barrel); // ... than in front of them
    CHECK(corner < 0.85f * floor);
    CHECK(wall > corner && wall < mid_air);
    CHECK(mid_air > 0.9f);
    CHECK(wall_under_beams > 0.65f * wall); // beams 0.25 m above shade a wall less than a table top shades the floor
    CHECK(volume.At({0.0f, 50.0f, 0.0f}, up) == 1.0f); // outside the grid: open
    // The winding does not matter: the same room with every triangle turned gives the same volume.
    {
        std::vector<uint32_t> turned = ridx;
        for (size_t t = 0; t + 2 < turned.size(); t += 3)
            std::swap(turned[t + 1], turned[t + 2]);
        const Occluders turned_room(rp, turned);
        interior.threads = 4;
        CHECK(BakePlaceProbes(turned_room, rp, turned, interior).ao.visibility == volume.visibility);
    }
    interior.threads = 4;
    const PlaceProbes threaded = BakePlaceProbes(room, rp, ridx, interior);
    CHECK(threaded.ao.visibility == volume.visibility && threaded.visibility.sky_open == probes.visibility.sky_open);

    // A cramped place, a corridor 1.2 m wide and 2 m high, with its walk points: its floor is as light as an open
    // floor (its baked colours hold how light the place is), a shelf across it still darkens the floor under it.
    {
        std::vector<Vec3> cp;
        std::vector<uint32_t> cidx;
        AddBox(cp, cidx, {-0.6f, 0.0f, -4.0f}, {0.6f, 2.0f, 4.0f});
        AddSolid(cp, cidx, {-0.6f, 0.76f, 2.0f}, {0.6f, 0.8f, 3.0f});
        ProbeParams walked = interior;
        for (float z = -3.5f; z <= 3.5f; z += 0.5f) // one walks along its middle, under the shelf too
            walked.walk.push_back({0.0f, 0.0f, z});
        const Occluders corridor_rays(cp, cidx);
        const AoVolume corridor = BakePlaceProbes(corridor_rays, cp, cidx, walked).ao;
        const float corridor_floor = corridor.Sample({0.0f, 0.2f, -1.0f}, up), under_shelf = corridor.Sample({0.0f, 0.2f, 2.5f}, up);
        std::printf("probes, a corridor: floor %.2f, under the shelf %.2f\n", corridor_floor, under_shelf);
        CHECK(corridor_floor > 0.9f * kAoOpenSurface);
        CHECK(under_shelf < 0.85f * corridor_floor);
        CHECK(PlaceKey({cp, cidx, {}, {}}, walked) != PlaceKey({cp, cidx, {}, {}}, interior)); // the walk points count
    }

    // Under a grating (its bars and holes 25 cm, cut by its texture's alpha) against under a solid plate of the same
    // size, 0.6 m above a floor: the floor under the grating sees clearly more than under the plate, and less than an
    // open floor. The rays pass the holes themselves: no pieces, no cell size between them and the grating.
    {
        const int n = 64;
        AlphaMask checker;
        checker.width = checker.height = n;
        checker.alpha.resize(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                checker.alpha[static_cast<size_t>(y) * n + x] = ((x / 8 + y / 8) & 1) ? 0 : 255;
        const auto mask = std::make_shared<const AlphaMask>(checker);
        const auto floor_visibility = [&](bool plate, bool grating) {
            std::vector<Vec3> gp;
            std::vector<uint32_t> gidx;
            AddSolid(gp, gidx, {-4.0f, -0.1f, -4.0f}, {6.0f, 0.0f, 6.0f});
            if (plate)
                AddSolid(gp, gidx, {0.0f, 0.6f, 0.0f}, {2.0f, 0.62f, 2.0f});
            std::vector<std::array<float, 2>> guv(gp.size(), std::array<float, 2>{0.0f, 0.0f});
            std::vector<Occluders::Cut> gcut(gidx.size() / 3);
            if (grating)
            {
                const auto base = static_cast<uint32_t>(gp.size());
                gp.insert(gp.end(), {Vec3{0.0f, 0.6f, 0.0f}, Vec3{2.0f, 0.6f, 0.0f}, Vec3{2.0f, 0.6f, 2.0f}, Vec3{0.0f, 0.6f, 2.0f}});
                guv.insert(guv.end(), {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}});
                gidx.insert(gidx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                gcut.insert(gcut.end(), 2, Occluders::Cut{mask, 160.0f / 255.0f});
            }
            const Occluders rays(gp, gidx, guv, gcut);
            ProbeParams over = interior;
            over.lo = {-2.0f, -0.1f, -2.0f};
            over.hi = {4.0f, 2.0f, 4.0f};
            return BakePlaceProbes(rays, gp, gidx, over).ao.Sample({1.0f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f});
        };
        const float open_floor = floor_visibility(false, false), under_plate = floor_visibility(true, false),
                    under_grating = floor_visibility(false, true);
        std::printf("probes, cut-out: the floor open %.2f, under a grating %.2f, under a plate %.2f\n", open_floor,
                    under_grating, under_plate);
        CHECK(under_plate < open_floor - 0.15f && under_grating > under_plate + 0.05f && under_grating < open_floor - 0.05f);
    }
}

// The place's cache: one key, one file (fx/place_probes.hpp PlaceKey, PlaceFile).
void CheckPlaceCache()
{
    std::vector<Vec3> p;
    std::vector<uint32_t> idx;
    AddSolid(p, idx, {-4.0f, -0.1f, -4.0f}, {4.0f, 0.0f, 4.0f});
    AddSolid(p, idx, {0.0f, 1.0f, 0.0f}, {2.0f, 1.1f, 2.0f});
    std::vector<std::array<float, 2>> uv(p.size(), std::array<float, 2>{0.25f, 0.25f});
    AlphaMask leaves;
    leaves.width = leaves.height = 8;
    leaves.alpha.assign(64, 255);
    leaves.alpha[9] = 0;
    const auto mask = std::make_shared<const AlphaMask>(leaves);
    std::vector<Occluders::Cut> cut(idx.size() / 3);
    for (size_t t = 12; t < cut.size(); ++t)
        cut[t] = {mask, 0.5f};
    ProbeParams params;
    params.threads = 1;
    const uint64_t key = PlaceKey({p, idx, uv, cut}, params);
    // The same place again: the same key, whatever the threads, whichever copy of the same mask.
    ProbeParams threads = params;
    threads.threads = 7;
    const auto same_mask = std::make_shared<const AlphaMask>(leaves);
    std::vector<Occluders::Cut> same_cut = cut;
    for (size_t t = 12; t < same_cut.size(); ++t)
        same_cut[t].mask = same_mask;
    CHECK(key == PlaceKey({p, idx, uv, cut}, threads) && key == PlaceKey({p, idx, uv, same_cut}, params));
    // The geometry: a vertex a centimetre off, another index.
    std::vector<Vec3> moved = p;
    moved[3][1] += 0.01f;
    std::vector<uint32_t> turned = idx;
    std::swap(turned[0], turned[1]);
    CHECK(key != PlaceKey({moved, idx, uv, cut}, params) && key != PlaceKey({p, turned, uv, cut}, params));
    // The alpha: one texel of a mask, the reference of the material, where the mask lies on a cut triangle, which
    // triangles are cut at all. Each changes what a ray passes: another key.
    AlphaMask torn = leaves;
    torn.alpha[20] = 0;
    const auto torn_mask = std::make_shared<const AlphaMask>(torn);
    std::vector<Occluders::Cut> torn_cut = cut, sharper = cut, fewer = cut;
    for (size_t t = 12; t < cut.size(); ++t)
    {
        torn_cut[t].mask = torn_mask;
        sharper[t].reference = 0.75f;
    }
    fewer[12] = {};
    std::vector<std::array<float, 2>> shifted = uv;
    shifted[idx[3 * 12]][0] += 0.125f;
    CHECK(key != PlaceKey({p, idx, uv, torn_cut}, params) && key != PlaceKey({p, idx, uv, sharper}, params) &&
          key != PlaceKey({p, idx, uv, fewer}, params) && key != PlaceKey({p, idx, shifted, cut}, params) &&
          key != PlaceKey({p, idx, uv, {}}, params));
    // What changes no ray changes no key: the texture coordinates of a solid triangle (a texture moved or swapped on
    // it, its colours: the key does not see them at all).
    std::vector<std::array<float, 2>> repainted = uv;
    repainted[idx[0]] = {0.9f, 0.1f};
    CHECK(key == PlaceKey({p, idx, repainted, cut}, params));
    // The grid and what the probes hold.
    ProbeParams finer = params, near = params, lobes = params, boxed = params;
    finer.cell = 0.25f;
    near.near_sky = 10.0f;
    lobes.ao_radius = 1.5f;
    boxed.lo = {-1.0f, -1.0f, -1.0f};
    boxed.hi = {1.0f, 1.0f, 1.0f};
    for (const ProbeParams &other : {finer, near, lobes, boxed})
        CHECK(key != PlaceKey({p, idx, uv, cut}, other));

    // The probes' section: back as baked; a cut or foreign section is refused.
    const Occluders occluders(p, idx, uv, cut);
    lobes.near_sky = 5.0f;
    lobes.turned = false;
    const PlaceProbes probes = BakePlaceProbes(occluders, p, idx, lobes);
    const std::vector<uint8_t> bytes = EncodeProbes(probes);
    std::string error;
    const std::optional<PlaceProbes> back = DecodeProbes(bytes, error);
    CHECK(back && back->visibility.sky_open == probes.visibility.sky_open && back->visibility.size == probes.visibility.size &&
          back->visibility.origin == probes.visibility.origin && back->visibility.cell == probes.visibility.cell &&
          back->near_sky == probes.near_sky && back->ao.visibility == probes.ao.visibility && back->ao.size == probes.ao.size &&
          back->rays == probes.rays);
    CHECK(!DecodeProbes(std::span(bytes).first(bytes.size() - 1), error) && !error.empty());
    CHECK(!DecodeProbes(std::span(bytes).first(10), error) && !error.empty());
    std::vector<uint8_t> foreign = bytes;
    foreign[0] ^= 0xff;
    CHECK(!DecodeProbes(foreign, error) && !error.empty());

    // The file: sections by kind and key; of a kind the last `keep` stay; another place's file is refused.
    PlaceFile file;
    file.Put(PlaceSection::Probes, key, bytes);
    for (uint64_t set = 1; set <= 4; ++set)
        file.Put(PlaceSection::LightmapSet, set, std::vector<uint8_t>(set, static_cast<uint8_t>(set)), 3);
    file.Put(PlaceSection::LightmapSet, 2, {9, 9}, 3); // written again: it is the newest now
    file.Put(PlaceSection::LightmapSet, 5, {5}, 3);
    const std::vector<uint8_t> encoded = file.Encode(key);
    const std::optional<PlaceFile> read = PlaceFile::Decode(encoded, key, error);
    CHECK(read.has_value());
    if (read)
    {
        const std::span<const uint8_t> kept = read->Find(PlaceSection::Probes, key);
        CHECK(std::equal(kept.begin(), kept.end(), bytes.begin(), bytes.end()));
        CHECK(read->Find(PlaceSection::LightmapSet, 1).empty() && read->Find(PlaceSection::LightmapSet, 3).empty());
        CHECK(read->Find(PlaceSection::LightmapSet, 4).size() == 4 && read->Find(PlaceSection::LightmapSet, 5).size() == 1);
        const std::span<const uint8_t> again = read->Find(PlaceSection::LightmapSet, 2);
        CHECK(again.size() == 2 && again[0] == 9);
        CHECK(read->Find(PlaceSection::Probes, key + 1).empty());
    }
    CHECK(!PlaceFile::Decode(encoded, key + 1, error) && error.find("another place") != std::string::npos);
    CHECK(!PlaceFile::Decode(std::span(encoded).first(encoded.size() - 3), key, error) && !error.empty());
    CHECK(!PlaceFile::Decode(std::span(encoded).first(6), key, error) && !error.empty());
}

// The place's bakes go before the background ones (fx/bakes.hpp BakePriority).
void CheckBakePriority()
{
    // A long background bake (a far LOD) is running, piece by piece. A bake of the place posted now starts at once on
    // its own lane, and while it is queued or runs the background bake takes no further piece.
    std::atomic<int> pieces{0};
    std::atomic<bool> place_running{false}, place_done{false}, release{false};
    std::atomic<int> pieces_at_start{-1}, pieces_while_place{-1};
    BakeTicket background = PostBake(
        "check: background",
        [&] {
            ParallelRanges(4000, 1, [&](size_t, size_t) {
                ++pieces;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }, 1);
        },
        BakePriority::Background);
    CHECK(WaitFor([&] { return pieces.load() >= 3; }));
    const auto posted = std::chrono::steady_clock::now();
    BakeTicket place = PostBake("check: place", [&] {
        place_running = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(60)); // the piece the background bake was in ends
        pieces_at_start = pieces.load();
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pieces_while_place = pieces.load();
        place_done = true;
    });
    CHECK(WaitFor([&] { return place_running.load(); }));
    const double started_ms = MsSince(posted);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    release = true;
    CHECK(WaitFor([&] { return place_done.load(); }));
    const int after_place = pieces.load();
    CHECK(WaitFor([&] { return pieces.load() > after_place + 3; })); // the background bake goes on
    std::printf("bake priority: behind a running background bake a bake of the place started after %.1f ms; the "
                "background bake took %d pieces while it ran, and went on after it\n",
                started_ms, pieces_while_place.load() - pieces_at_start.load());
    CHECK(started_ms < 100.0 && pieces_while_place.load() == pieces_at_start.load());
    background.reset();
    ShutdownBakes();
}

// The bakes' threads by budget and by the device's cores (fx/bakes.hpp BakeCoresFor, CurrentBakeBudget).
void CheckBakeBudget()
{
    // A phone with two prime cores, three big and three small ones (cpu_capacity): behind a loading picture no cap
    // of its own (the shared admission's permits decide); in play two, on the cores below the prime ones.
    const std::vector<unsigned> phone = {1024, 1024, 900, 900, 900, 400, 400, 400};
    const BakeCores loading = BakeCoresFor(BakeBudget::Loading, phone, 8), play = BakeCoresFor(BakeBudget::Play, phone, 8);
    CHECK(loading.threads == 8 && loading.allowed.empty());
    CHECK(play.threads == 2 && play.allowed == std::vector<unsigned>({2, 3, 4, 5, 6, 7}));
    // One fast core and one slower: one thread in play, on the slower.
    const std::vector<unsigned> two = {1024, 512};
    CHECK(BakeCoresFor(BakeBudget::Play, two, 2).threads == 1 && BakeCoresFor(BakeBudget::Play, two, 2).allowed == std::vector<unsigned>({1}));
    // No capacities (a desktop), or all the same: no core is told from another; two threads in play all the same.
    const BakeCores desktop = BakeCoresFor(BakeBudget::Play, {}, 16);
    const std::vector<unsigned> even(4, 1024);
    CHECK(desktop.threads == 2 && desktop.allowed.empty());
    CHECK(BakeCoresFor(BakeBudget::Play, even, 4).threads == 2 && BakeCoresFor(BakeBudget::Play, even, 4).allowed.empty());
    CHECK(BakeCoresFor(BakeBudget::Play, {}, 1).threads == 1);

    // The budget follows the shared admission: Loading in its opaque phase (a loading picture covers the scene),
    // Play otherwise. In play a bake's pieces run on no more than kPlayBakeThreads threads, whatever it asks for; when
    // the loading picture goes while it runs, the threads beyond the budget wait at their next piece.
    CpuAdmission &admission = SharedCpuAdmission();
    const uint64_t generation = 77;
    // Counted, not timed: a thread is "late" on its second piece begun after the picture went, which proves it asked
    // the budget again since (its first may be the one it had already taken). With four cores or more the shrinking
    // bake first waits until more than kPlayBakeThreads threads run pieces, then lets the picture go.
    const bool wide = std::thread::hardware_concurrency() >= 4;
    const auto threads_of = [&](unsigned asked, bool shrink) {
        std::mutex mutex;
        std::condition_variable wider;
        std::set<std::thread::id> seen, late;
        std::map<std::thread::id, int> after;
        bool gone = false;
        ParallelRanges(4000, asked, [&](size_t, size_t) {
            {
                std::unique_lock lock(mutex);
                const auto id = std::this_thread::get_id();
                seen.insert(id);
                if (gone && ++after[id] >= 2)
                    late.insert(id);
                if (shrink && !gone)
                {
                    wider.notify_all();
                    if (wide)
                        wider.wait(lock, [&] { return gone || seen.size() > kPlayBakeThreads; });
                    if (!gone)
                    {
                        admission.end(generation);
                        gone = true;
                        wider.notify_all();
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300)); // pieces long enough for helpers to join
        }, 10);
        return std::pair<size_t, size_t>(seen.size(), late.size());
    };
    admission.end(generation);
    CHECK(CurrentBakeBudget() == BakeBudget::Play);
    const auto in_play = threads_of(8, false);
    admission.begin(generation, true);
    admission.opaque(generation, true);
    CHECK(CurrentBakeBudget() == BakeBudget::Loading);
    const auto shrunk = threads_of(8, true);
    CHECK(CurrentBakeBudget() == BakeBudget::Play);
    std::printf("bake budget: in play %u threads; a bake asking for 8 ran on %zu in play; one that began on %zu behind a "
                "loading picture ended on %zu after it went\n",
                kPlayBakeThreads, in_play.first, shrunk.first, shrunk.second);
    CHECK(in_play.first <= kPlayBakeThreads && shrunk.second <= kPlayBakeThreads && shrunk.second >= 1);
    CHECK(!wide || shrunk.first > kPlayBakeThreads); // it did start wider
}

// fx::SeaWaves' steps (WaveClock): game time, all of it while a frame holds at most kMaxSteps * kMaxStep; 60 Hz
// steps while the frames are short, longer ones (never past kMaxStep) when accelerated time packs more into a frame.
void CheckSeaWaves()
{
    WaveClock clock;
    float step = 0.0f;
    bool dropped = false;
    CHECK(clock.Due(1.0f / 60.0f, step, dropped) == 1 && std::fabs(step - WaveClock::kStep) < 1e-5f && !dropped);
    CHECK(clock.Due(0.0f, step, dropped) == 0);
    // 30 Hz at x1, x2 and x4: every game second is stepped, in steps no longer than kMaxStep.
    for (const float scale : {1.0f, 2.0f, 4.0f})
    {
        WaveClock c;
        float stepped = 0.0f;
        for (int i = 0; i < 300; ++i)
        {
            const int n = c.Due(scale / 30.0f, step, dropped);
            CHECK(!dropped && n <= WaveClock::kMaxSteps && step <= WaveClock::kMaxStep + 1e-6f);
            stepped += static_cast<float>(n) * step;
        }
        CHECK(std::fabs(stepped - scale * 10.0f) < 0.05f);
    }
    // A frame longer than the simulation keeps up with: kMaxSteps steps of kMaxStep, the rest dropped and said so.
    CHECK(clock.Due(1.0f, step, dropped) == WaveClock::kMaxSteps && dropped &&
          std::fabs(step - WaveClock::kMaxStep) < 1e-5f);
    CHECK(clock.Due(1.0f / 120.0f, step, dropped) == 0); // the dropped time is not made up later

    // Uniform field (laplacian 0): a constant displacement rate supplies rate * elapsed height, at every
    // time scale. The external flux must not persist as free-wave velocity in the second-order recurrence.
    float reference = 0.0f;
    for (const int scale : {1, 2, 4})
    {
        WaveClock c;
        float height = 0.0f, previous = 0.0f, last_step = WaveClock::kStep;
        for (int frame = 0; frame < 300 / scale; ++frame)
        {
            const int n = c.Due(static_cast<float>(scale) / 30.0f, step, dropped);
            CHECK(!dropped);
            for (int i = 0; i < n; ++i)
            {
                const float next = height + (height - previous) * (step / last_step) *
                                                std::pow(0.9985f, step / WaveClock::kStep) +
                                   1e-5f * step;
                previous = height + 1e-5f * step;
                height = next;
                last_step = step;
            }
        }
        if (scale == 1)
            reference = height;
        CHECK(reference > 0.0f && std::fabs(height / reference - 1.0f) < 0.01f);
        CHECK(std::fabs(height - 1e-4f) < 1e-7f);
    }
}

// Gloss maps (gloss_map.hpp): the class table on TEHO's names, the class order on one texture, the texel cues.
void CheckGlossMaps()
{
    const auto gloss = [](const char *path) { return ClassifyGlossName(NormalizeTexturePath(path)).gloss; };
    CHECK(gloss("Resource\\Textures\\cannon_T1.tga.tx") == GlossClass::Metal);
    CHECK(gloss("Resource\\Textures\\dm_guns.tga.tx") == GlossClass::Metal);
    CHECK(gloss("Resource\\Textures\\Gundeck_1.tga.tx") == GlossClass::DryWood);
    CHECK(gloss("Resource\\Textures\\Ships\\Brig1\\Hull1\\brighull1.tga.tx") == GlossClass::PaintedWood);
    CHECK(gloss("Resource\\Textures\\Ships\\Brig1\\Hull1\\deck_brig.tga.tx") == GlossClass::DryWood);
    CHECK(gloss("Resource\\Textures\\Ships\\parus_common.tga.tx") == GlossClass::Matte);
    CHECK(gloss("Resource\\Textures\\GlassWindow.tga.tx") == GlossClass::Glass);
    CHECK(gloss("Resource\\Textures\\windtavern2ins.tga.tx") == GlossClass::Glass);
    CHECK(gloss("Resource\\Textures\\windmill_walls.tga.tx") == GlossClass::Building);
    CHECK(gloss("Resource\\Textures\\tavern2ins.tga.tx") == GlossClass::Interior);
    CHECK(gloss("Resource\\Textures\\BigHouse01.tga.tx") == GlossClass::Building);
    CHECK(gloss("Resource\\Textures\\furnitureU1.tga.tx") == GlossClass::Lacquered);
    CHECK(gloss("Resource\\Textures\\bag2.tga.tx") == GlossClass::Leather);
    CHECK(gloss("Resource\\Textures\\063pierres.tga.tx") == GlossClass::Stone);
    CHECK(gloss("Resource\\Textures\\Barn2_bump.tga.tx") == GlossClass::None);
    CHECK(gloss("Resource\\Textures\\Characters\\blaze.tga.tx") == GlossClass::Character);
    CHECK(gloss("Resource\\Textures\\antigua.tga.tx") == GlossClass::None);

    // The roughness network's output corrected (no model here: fields made up): GGX's roughness as the highlight's power,
    // the grooves of the height, dark non-metal texels, the metal mask, the class caps.
    constexpr int kSize = 64;
    constexpr size_t kTexels = size_t(kSize) * kSize;
    std::vector<uint8_t> grey, rusty;
    for (size_t i = 0; i < kTexels; ++i)
    {
        const uint8_t n = static_cast<uint8_t>((i * 7919) % 23), grain = static_cast<uint8_t>((i * 104729) % 41);
        rusty.insert(rusty.end(), {static_cast<uint8_t>(40 + grain), static_cast<uint8_t>(18 + grain / 2), static_cast<uint8_t>(6), 255});
        grey.insert(grey.end(), {static_cast<uint8_t>(95 + n), static_cast<uint8_t>(100 + n), static_cast<uint8_t>(105 + n), 255});
    }
    const std::vector<float> flat(kTexels, 0.5f);
    const auto mean = [&](const GlossField &f) {
        double sum = 0.0;
        for (const float g : f.gloss)
            sum += g;
        return f.gloss.size() == kTexels ? sum / static_cast<double>(kTexels) : -1.0;
    };
    const auto at = [&](float r, GlossClass c, const std::vector<uint8_t> &rgba = {}) {
        return mean(GlossOfRoughness(std::vector<float>(kTexels, r), flat, rgba.empty() ? grey : rgba, kSize, kSize, c));
    };
    // r 0.3: alpha 0.09, power 2/0.0081 - 2 = 245, g = (log2 245 - 3) / 4.5 = 1.1 -> 1; r 0.5: power 30, 0.42; r 0.67 and
    // up: matte.
    std::printf("gloss of roughness 0.3 %.2f, 0.5 %.2f, 0.7 %.2f\n", at(0.3f, GlossClass::Glass), at(0.5f, GlossClass::Glass),
                at(0.7f, GlossClass::Glass));
    CHECK(at(0.3f, GlossClass::Glass) > 0.99 && std::abs(at(0.5f, GlossClass::Glass) - 0.424) < 0.02 &&
          at(0.7f, GlossClass::Glass) == 0.0);
    // Caps: cloth and plaster 0.1, stone 0.35.
    CHECK(std::abs(at(0.3f, GlossClass::Matte) - 0.1) < 1e-6 && std::abs(at(0.3f, GlossClass::Stone) - 0.35) < 1e-6);
    // Dark non-metal texels lose the gloss; metal keeps it, and polished metal at least half of it whatever the network.
    const std::vector<uint8_t> black(kTexels * 4, 8);
    CHECK(at(0.5f, GlossClass::Glass, black) < 0.01 && at(0.5f, GlossClass::Metal, black) > 0.4);
    const std::vector<float> mask = GlossMetalMask(grey, kSize, kSize), rust_mask = GlossMetalMask(rusty, kSize, kSize);
    std::printf("metal: grey %.2f (mask %.2f), rust %.2f (mask %.2f)\n", at(0.9f, GlossClass::Metal), mask[100],
                at(0.9f, GlossClass::Metal, rusty), rust_mask[100]);
    CHECK(at(0.9f, GlossClass::Metal) > 0.45 && at(0.9f, GlossClass::Metal, rusty) + 0.1 < at(0.9f, GlossClass::Metal));
    CHECK(mask.size() == kTexels && mask[100] > 0.95f && rust_mask[100] < 0.8f);
    CHECK(GlossOfRoughness(flat, flat, grey, kSize, kSize, GlossClass::None).gloss.empty());
    CHECK(GlossOfRoughness(flat, {}, grey, kSize, kSize, GlossClass::Stone).gloss.empty());
    // A groove in the height keeps a quarter of the gloss.
    std::vector<float> groove = flat;
    for (int y = 0; y < kSize; ++y)
        for (int x = 30; x < 33; ++x)
            groove[size_t(y) * kSize + x] = 0.1f;
    const GlossField grooved = GlossOfRoughness(std::vector<float>(kTexels, 0.4f), groove, grey, kSize, kSize, GlossClass::Glass);
    CHECK(grooved.gloss[31] < 0.4f * grooved.gloss[10]);
    // The gloss map file's texture: RG8, all mips, R the metal mask, G the gloss.
    const GlossField metal = GlossOfRoughness(flat, flat, grey, kSize, kSize, GlossClass::Metal);
    const NormalMap material = GlossMaterial(metal);
    CHECK(material.format == NormalMapFormat::Rg8 && material.width == kSize && material.mip_count == 7 &&
          material.rg.size() == NormalMapBytes(kSize, kSize, NormalMapFormat::Rg8) && material.rg[200] > 240 &&
          std::abs(material.rg[201] - std::lround(metal.gloss[100] * 255.0f)) <= 1);
}

// The parallax trust (parallax_trust.hpp): plain relief keeps it, a dark pane set into a wall loses it, a texture of
// nothing but panes is below the coverage a texture needs; the exclusion lists (parallax_exclusions.hpp).
// The parallax probe's CPU side (fx_pom_probe): the centre-ray candidates, the pick from the occlusion queries, the view
// order that runs the probe view after the scene, the toggle's edge.
void CheckParallaxProbe()
{
    // Camera at the origin looking down +z (identity view), symmetric perspective.
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float proj[16] = {1.5f, 0, 0, 0, 0, 2.0f, 0, 0, 0, 0, 1.001f, 1, 0, 0, -0.1f, 0};
    const float ahead[3] = {0.3f, -0.2f, 10.0f}, aside[3] = {3.0f, 0.0f, 10.0f}, behind[3] = {0.0f, 0.0f, -5.0f};
    CHECK(OnCentreRay(identity, proj, ahead, 0.5f));
    CHECK(!OnCentreRay(identity, proj, aside, 0.5f));
    CHECK(OnCentreRay(identity, proj, aside, 3.5f));
    CHECK(!OnCentreRay(identity, proj, behind, 1.0f));
    CHECK(OnCentreRay(identity, proj, behind, 6.0f)); // the camera inside the sphere
    // A view that moves the world 10 units back along z: a sphere at the origin is then ahead.
    const float moved[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 10, 1};
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    CHECK(OnCentreRay(moved, proj, origin, 0.1f));
    // An off-centre projection: its centre ray leans by -_31 / _11.
    const float skewed[16] = {1.0f, 0, 0, 0, 0, 1.0f, 0, 0, -0.5f, 0, 1.001f, 1, 0, 0, -0.1f, 0};
    const float leaning[3] = {5.0f, 0.0f, 10.0f};
    CHECK(OnCentreRay(identity, skewed, leaning, 0.2f));
    CHECK(!OnCentreRay(identity, proj, leaning, 0.2f));

    const int32_t none[] = {0, 0};
    const int32_t wall_and_decal[] = {4, 0, 2};
    const int32_t pending[] = {4, kProbeNoResult};
    CHECK(PickProbeTarget(none).conclusive && PickProbeTarget(none).index == -1);
    CHECK(PickProbeTarget(wall_and_decal).conclusive && PickProbeTarget(wall_and_decal).index == 2);
    CHECK(!PickProbeTarget(pending).conclusive);
    CHECK(PickProbeTarget({}).conclusive && PickProbeTarget({}).index == -1);

    // bgfx::setViewOrder's swaps (bgfx_p.h), from identity: the probe view runs right after `last`, the rest keep
    // their order; a reset (null order) restores identity.
    constexpr int kViews = 40;
    uint16_t remap[kViews], position[kViews];
    const auto set_order = [&](uint16_t first, uint16_t count, const uint16_t *order) {
        for (uint16_t i = 0; i < count; ++i)
        {
            const uint16_t pos = first + i, id = order ? order[i] : pos;
            const uint16_t prev_pos = position[id], prev_id = remap[pos];
            remap[pos] = id;
            position[id] = pos;
            remap[prev_pos] = prev_id;
            position[prev_id] = prev_pos;
        }
    };
    for (uint16_t i = 0; i < kViews; ++i)
        remap[i] = position[i] = i;
    for (const uint16_t last : {uint16_t(12), uint16_t(30)})
    {
        set_order(0, kViews, nullptr);
        const std::vector<uint16_t> order = ProbeViewOrder(7, last);
        CHECK(order.size() == static_cast<size_t>(last - 7 + 1));
        set_order(7, static_cast<uint16_t>(order.size()), order.data());
        bool ok = position[7] == position[last] + 1 && position[last + 1] == position[7] + 1;
        for (uint16_t id = 0; id + 1 < kViews; ++id)
            if (id != 7 && id + 1 != 7)
                ok = ok && position[id] < position[id + 1];
        CHECK(ok);
    }
    set_order(0, kViews, nullptr);
    bool identity_again = true;
    for (uint16_t i = 0; i < kViews; ++i)
        identity_again = identity_again && remap[i] == i;
    CHECK(identity_again);
    CHECK(ProbeViewOrder(7, 7).empty());

    EdgeTrigger toggle;
    CHECK(!toggle.Apply(-1)); // the stored value at the start
    CHECK(!toggle.Apply(-1)); // re-applied unchanged
    CHECK(toggle.Apply(1));
    CHECK(!toggle.Apply(1));
    CHECK(toggle.Apply(-1));
}

void CheckParallaxTrust()
{
    const int n = 256;
    const size_t texels = static_cast<size_t>(n) * n;
    std::vector<uint8_t> rgba(texels * 4, 255);
    std::vector<float> height(texels);
    uint32_t seed = 12345;
    const auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    // Plain stone: mid-grey colour with grain, a height of grain with thin joints.
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const size_t t = static_cast<size_t>(y) * n + x;
            const uint8_t grey = static_cast<uint8_t>(120 + noise() * 60.0f);
            rgba[t * 4] = rgba[t * 4 + 1] = rgba[t * 4 + 2] = grey;
            height[t] = 0.6f + noise() * 0.1f - ((x % 32 < 2 || y % 32 < 2) ? 0.3f : 0.0f);
        }
    const std::vector<float> plain = ParallaxTrust(rgba, height, n, n);
    CHECK(plain.size() == texels && TrustCoverage(plain) > 0.95f);
    // A window: a 40 x 56 texel pane, dark and flat, its height deep.
    std::vector<uint8_t> walled = rgba;
    std::vector<float> recessed = height;
    for (int y = 100; y < 156; ++y)
        for (int x = 100; x < 140; ++x)
        {
            const size_t t = static_cast<size_t>(y) * n + x;
            walled[t * 4] = walled[t * 4 + 1] = walled[t * 4 + 2] = 20;
            recessed[t] = 0.05f;
        }
    const std::vector<float> with_window = ParallaxTrust(walled, recessed, n, n);
    CHECK(with_window[128 * n + 120] < 0.05f);                      // the pane: no parallax
    CHECK(with_window[20 * n + 20] > 0.95f && with_window[200 * n + 200] > 0.95f); // the wall far from it keeps it
    CHECK(TrustCoverage(with_window) > 0.85f && TrustCoverage(with_window) < 0.97f);
    CHECK(TrustThumbnail(with_window, n, n).size() == static_cast<size_t>(TrustThumbnailSize(n)) * TrustThumbnailSize(n));
    // Nothing but dark flat glass: no trusted texel to speak of.
    std::vector<uint8_t> glass(texels * 4, 15);
    CHECK(TrustCoverage(ParallaxTrust(glass, std::vector<float>(texels, 0.5f), n, n)) < kParallaxMinCoverage);
    CHECK(ParallaxTrust(rgba, std::vector<float>(10), n, n).empty());

    // In the material: the pane's height goes to the top (1), the coverage and the mask are kept; a texture below the
    // coverage keeps its height whole.
    NormalMap map;
    map.width = map.height = n;
    map.format = NormalMapFormat::Rg8;
    for (int w = n; w >= 1; w /= 2)
        ++map.mip_count;
    map.rg.assign(NormalMapBytes(n, n), 128);
    std::vector<float> varied = recessed;
    for (size_t t = 0; t < texels; ++t)
        varied[t] += 0.3f * static_cast<float>((t / n / 8 + t % n / 8) & 1);
    CHECK(AddReliefMaterial(map, ReliefClass::Stone, varied, {}, walled));
    CHECK(map.trust_coverage > 0.85f && map.trust.size() == static_cast<size_t>(TrustThumbnailSize(n)) * TrustThumbnailSize(n));
    CHECK(map.material[(128 * n + 120) * 2] > 250); // the pane: the top
    const auto masked_material = map.material, masked_trust = map.trust;
    const float coverage = map.trust_coverage;
    // An existing cache keeps the zero-displacement window mask when the upload refreshes its masonry depth.
    map.depth = 3.0f * static_cast<float>(n) / kReliefMapSize;
    RefreshReliefDepth(map, ReliefClass::Stone);
    CHECK(map.depth == 6.0f * static_cast<float>(n) / kReliefMapSize && map.material == masked_material &&
          map.trust == masked_trust && map.trust_coverage == coverage);
    NormalMap dark = map;
    dark.material.clear();
    CHECK(AddReliefMaterial(dark, ReliefClass::Stone, varied, {}, glass));
    CHECK(dark.trust_coverage < kParallaxMinCoverage);

    // The lists.
    ParallaxExclusions lists;
    std::vector<std::string> problems;
    lists.LoadShipped("# comment\nexclude signboardsa  # a sign\n\nbad name\ninclude locations/wall\n", &problems);
    CHECK(lists.shipped_count() == 2 && problems.size() == 1);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "storm_fx_pom_user_check" / "user.txt";
    std::filesystem::remove_all(file.parent_path());
    CHECK(lists.LoadUser(file) && lists.user_count() == 0);
    CHECK(lists.Of("signboardsa") == ParallaxExclusions::Verdict::Excluded && lists.Of("locations/wall") == ParallaxExclusions::Verdict::Included &&
          lists.Of("tavern2") == ParallaxExclusions::Verdict::Default);
    // Masonry takes the parallax by default, nothing else does.
    CHECK(ReliefTakesParallax(ReliefClass::Stone) && ReliefTakesParallax(ReliefClass::Building) &&
          ReliefTakesParallax(ReliefClass::Character) && !ReliefTakesParallax(ReliefClass::Wood) &&
          !ReliefTakesParallax(ReliefClass::Prop) && !ReliefTakesParallax(ReliefClass::Plaster) && !ReliefTakesParallax(ReliefClass::Metal) &&
          !ReliefTakesParallax(ReliefClass::HeightMap) && !ReliefTakesParallax(ReliefClass::None));
    CHECK(lists.SetUser("signboardsa", true) && lists.SetUser("tavern2", false) &&
          lists.Of("signboardsa") == ParallaxExclusions::Verdict::Included && lists.Of("tavern2") == ParallaxExclusions::Verdict::Excluded);
    ParallaxExclusions again;
    again.LoadShipped(ParallaxExclusions::ShippedText());
    CHECK(again.shipped_count() > 0 && again.Of("pierwood1") == ParallaxExclusions::Verdict::Included);
    CHECK(again.LoadUser(file) && again.user_count() == 2 && again.shipped_count() > 0 &&
          again.Of("signboardsa") == ParallaxExclusions::Verdict::Included && again.Of("tavern2") == ParallaxExclusions::Verdict::Excluded);
    std::filesystem::remove_all(file.parent_path());
}

void CheckUpscale()
{
    // Which textures get a ×2, and how their tiles are padded.
    CHECK(ClassifyUpscale("locations/cobblem1", ReliefClass::Stone, false) == UpscaleTarget::Surface);
    CHECK(ClassifyUpscale("locations/cobblem1", ReliefClass::Stone, true) == UpscaleTarget::None); // alpha-cut
    CHECK(ClassifyUpscale("characters/blaze", ReliefClass::None, false) == UpscaleTarget::Character);
    CHECK(ClassifyUpscale("weather/sky/sky1", ReliefClass::None, false) == UpscaleTarget::None);
    CHECK(ClassifyUpscale("bump/wallbump", ReliefClass::HeightMap, false) == UpscaleTarget::None);
    CHECK(UpscaleWraps(UpscaleTarget::Surface, ReliefClass::Stone) && !UpscaleWraps(UpscaleTarget::Surface, ReliefClass::Building) &&
          !UpscaleWraps(UpscaleTarget::Character, ReliefClass::None));
    CHECK(ReliefMapsFromUpscale(512, 256) && !ReliefMapsFromUpscale(1024, 512));
    CHECK(UpscaleSize(1024, 512) && !UpscaleSize(2048, 2048) && !UpscaleSize(0, 64));
    CHECK(UpscaleMethod(false) != UpscaleMethod(true));
    // The budget from the RAM.
    CHECK(UpscaleBudgetBytes(2ull << 30) == 64ull << 20 && UpscaleBudgetBytes(8ull << 30) == 256ull << 20 &&
          UpscaleBudgetBytes(16ull << 30) == 384ull << 20);
    // Residency: nearest first within the budget; a resident one is kept until kUpscaleFar; not drawn = released.
    {
        const uint64_t mb = 1ull << 20;
        const UpscaleCandidate c[] = {{5.0f, 6 * mb, false}, {2.0f, 6 * mb, false}, {30.0f, 1 * mb, true},
                                      {30.0f, 1 * mb, false}, {-1.0f, 1 * mb, true}, {10.0f, 6 * mb, false},
                                      {40.0f, 1 * mb, true}, {20.0f, 1 * mb, false}};
        const UpscalePlan plan = PlanUpscaleResidency(c, 14 * mb);
        CHECK(plan.keep[1] && plan.keep[0] && !plan.keep[5] && plan.keep[2] && !plan.keep[3] && !plan.keep[4] &&
              !plan.keep[6] && plan.keep[7]);
        CHECK(plan.bytes == 14 * mb && plan.limited == 1 && plan.nearest_limited == 10.0f);
        CHECK(PlanUpscaleResidency(c, 64 * mb).limited == 0);
    }
    // The cache: written atomically, read back, a damaged file deleted, the least recently used trimmed first.
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "storm_fx_upscale_check";
        std::filesystem::remove_all(dir);
        std::vector<uint8_t> ktx(5000);
        for (size_t i = 0; i < ktx.size(); ++i)
            ktx[i] = static_cast<uint8_t>(i * 7);
        CHECK(!HasUpscaled(dir, 1) && WriteUpscaled(dir, 1, ktx) && HasUpscaled(dir, 1));
        const std::optional<std::vector<uint8_t>> back = ReadUpscaled(dir, 1);
        CHECK(back && *back == ktx);
        CHECK(!ReadUpscaled(dir, 2));
        {
            std::fstream f(UpscaleFile(dir, 1), std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(100);
            f.put(static_cast<char>(0x5a));
        }
        std::string error;
        CHECK(!ReadUpscaled(dir, 1, &error) && !error.empty() && !HasUpscaled(dir, 1));
        CHECK(WriteUpscaled(dir, 3, ktx) && WriteUpscaled(dir, 4, ktx));
        std::filesystem::last_write_time(UpscaleFile(dir, 3), std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
        CHECK(TrimUpscaled(dir, 6000) <= 6000 && !HasUpscaled(dir, 3) && HasUpscaled(dir, 4));
        std::filesystem::remove_all(dir);
    }
}

void CheckWetness()
{
    // The first weather frame starts at the rain's level (a place entered in the rain is wet already).
    Wetness w;
    w.Update(0.8f, 0.016f);
    CHECK(Near(w.Level(), 0.8f, 1e-6f));
    // The rain stops: drying is slow, kWetDrySeconds from soaked to dry.
    for (int i = 0; i < 60; ++i)
        w.Update(0.0f, 1.0f);
    CHECK(Near(w.Level(), 0.8f - 60.0f / kWetDrySeconds, 1e-4f));
    for (int i = 0; i < 400; ++i)
        w.Update(0.0f, 1.0f);
    CHECK(w.Level() == 0.0f);
    // The rain starts: a full rain soaks the dry place in kWetRiseSeconds (30..60 s), never overshooting.
    float t = 0.0f;
    while (w.Level() < 1.0f && t < 200.0f)
    {
        w.Update(1.0f, 0.1f);
        t += 0.1f;
    }
    CHECK(t >= 30.0f && t <= 60.0f && Near(t, kWetRiseSeconds, 0.2f) && w.Level() == 1.0f);
    CHECK(kWetDrySeconds > 2.0f * kWetRiseSeconds);
    // A lighter rain: the level settles at it, from above as from below.
    w.Update(0.3f, 1000.0f);
    CHECK(Near(w.Level(), 0.3f, 1e-6f));
    w.Update(0.5f, 1000.0f);
    CHECK(Near(w.Level(), 0.5f, 1e-6f));
    // A gap (an interior, a loading screen): the next weather frame jumps to its rain, dry or wet.
    w.Interrupt();
    w.Update(0.0f, 0.016f);
    CHECK(w.Level() == 0.0f);
    w.Interrupt();
    w.Update(1.0f, 0.016f);
    CHECK(w.Level() == 1.0f);
}

int main(int argc, char **argv)
{
    CheckParallaxGeometry();
    if (argc == 2 && std::string_view(argv[1]) == "--parallax-geometry-only")
    {
        std::printf("POM geometry checks: %s\n", failures ? "FAILED" : "passed");
        return failures ? 1 : 0;
    }
    CheckSettings();
    CheckLut();
    CheckToneCurve();
    CheckNoise();
    CheckSky();
    CheckLighting();
    CheckSupport();
    CheckCapsProfiles();
    CheckLightFalloff();
    CheckLights();
    CheckShadows();
    CheckNormalMaps();
    CheckCheapUpgrades();
    CheckHeightMapSizes();
    CheckReliefModel();
    CheckReliefMaterial();
    CheckReliefRules();
    CheckFixtures();
    CheckLampShadows();
    CheckCutout();
    CheckSunFit();
    CheckTownLight();
    CheckOccluders();
    CheckPlaceProbes();
    CheckPlaceCache();
    CheckBakeThermal();
    CheckBakeCancel();
    CheckBakeLoads();
    CheckBakePriority();
    CheckBakeBudget();
    CheckParallaxTrust();
    CheckParallaxProbe();
    CheckSeaWaves();
    CheckGlossMaps();
    CheckUpscale();
    CheckWetness();
    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("fx core checks passed\n");
    return 0;
}
