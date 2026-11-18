// Fixed-function texture-stage combiner. Included by fs_ff*.sc; STAGEn_SAMPLE
// macros select the sampler type of each stage (2D unless overridden).
//
// Graphics effects variants (src/effects/lighting.cpp picks them; none is used by the «Оригинал» preset):
//   FX_FF_LIT       per-pixel lighting of a lit draw (vs_ff_lit): the diffuse colour the stages read is
//                   re-lit per pixel with fx_lighting.sh (sun, hemisphere, point / spot lights, rim, sunlight through sails,
//                   specular with FX_LIGHT_SPECULAR); features the settings leave off arrive with zero strength.
//   FX_FF_BAKED     geometry with baked vertex colours (lighting off, vs_ff_lit): dynamic point lights are added,
//                   as texture * light.
// Product blocks of the lit and baked families (renderer_bgfx/CMakeLists.txt; lighting.cpp ChooseFamily picks the
// narrowest family that holds every block a draw reads; a block's per-draw switch still turns it off in a family
// that has it):
//   FX_FF_LAMPS     the location's lamps with their shadows (ffLampLight, off per draw by u_fxLamps[0].x = 0)
//                   relight the baked colours and light the lit draws; lit draws of an interior take the room light
//                   as their ambient (ffRoomLight, u_fxLampGrid[2].w).
//   FX_FF_AO        the interior's ambient occlusion volume (ffAoVolume, off per draw by u_fxAoParams.x = 0): darkens
//                   the ambient of lit draws and the baked colours.
//   FX_FF_TOWN_WALL FX_FF_BAKED: a town's walls relit by our sun and sky (u_fxTownWall[0].w = 0: off for the draw).
//   FX_FF_WET       wet surfaces: in the rain, outdoor surfaces where the sky's rain reaches them (the visibility
//                   volume's openness: dry under roofs) are darker and glossier, their relief filled; the highlights
//                   of the sun and the lamps (FX_LIGHT_SPECULAR) take the wet gloss, the overcast sky is reflected;
//                   puddles in the relief's hollows (FX_LIGHT_PARALLAX). ff_wet.sh, off per draw by u_fxWet[0].x = 0.
// Every family reads the visibility volume (ffVisibility).
//   FX_LIGHT_NORMALMAP  with FX_FF_LIT / FX_FF_BAKED: relief from the normal map derived from stage 1's height texture
//                   or stage 0's colour texture (stage 14, that stage's coordinates: u_fxFf.y): lit draws perturb the
//                   normal, baked ones scale their colour.
//   FX_LIGHT_SPECULAR  with FX_FF_LIT / FX_FF_BAKED: highlights. With a gloss map of stage 0's texture (stage 7,
//                   fx/gloss_map.hpp, u_fxGloss.x its share while it fades in) each texel's gloss sets the highlight
//                   (fxGlossSpec), else the material's (fxMaterialSpec). u_fxGloss.w > 0 (Settings::gloss, its value the gloss strength) adds the
//                   lamps' highlights with their shadows (lit and baked) and the sun's on baked surfaces outdoors, where
//                   the bake shows the sun; u_fxGloss.y the texture's metal class (its polished texels: the highlight in
//                   the texture's colour, a little less diffuse light; times the map's metal mask, its R); u_fxGloss.z debug
//                   views: 1 the gloss (magenta: no map), 2 the relief (the normal's tilt by the normal map, grey: none),
//                   3 the open sun the bake shows times the sun's light (yellow), its highlight (blue); lit draws
//                   dark blue; 4 the metal mask (cyan).
//   FX_LIGHT_PARALLAX   with FX_LIGHT_NORMALMAP (a stage 0 map whose material, stage 7, has a height): parallax occlusion mapping;
//                   stage 0's colour and the normal map are read where the eye's ray meets the relief.
//   FX_LIGHT_SHADOW with FX_FF_LIT: the sun's diffuse and specular terms are scaled by the sun shadow maps
//                   (stage 15, src/effects/shadows.hpp; one tap or the soft filter: fx_lighting.sh fxSunShadow); with
//                   FX_FF_BAKED: a town's walls likewise, other baked surfaces take the moving casters' shadows on
//                   the sun's part of their bake (u_fxShadow[3].w).
//   FX_FF_DYN       the plain combiner plus the moving casters' sun shadows (fs_ff_dyn with vs_ff[_unlit]_dyn: the
//                   lightmapped and baked ground under characters and ships; fx_sun_map.sh fxSunGround).
//   FX_FF_COVERAGE  alpha to coverage instead of the alpha-test discard (fx_coverage.sh); in the FX_FF_LIT /
//                   FX_FF_BAKED variants it is switched per draw by u_fxCoverage.z.
#include "ff_finish.sh"
#ifdef FX_FF_DYN
#include "fx_sun_map.sh"
#endif

#if defined(FX_FF_LIT) || defined(FX_FF_BAKED)
#define FX_FF_PIXEL_LIGHTING 1
#include "fx_lighting.sh"
// x: two-sided surface whose back side shows the light passing through (normal flipped towards the viewer)
// y: FX_LIGHT_NORMALMAP: the texture stage (0 or 1) whose coordinates the normal map follows
uniform vec4 u_fxFf;

// A character's own skin colour (fx/skin_tone.hpp, the same test as SkinLikeness: keep both in step): [0] its mean
// chromaticity r, g, its luminance's median and 95th percentile; [1] the inverse covariance of the chromaticity rr, rg,
// gg, w the lod the test reads. [0].x 0: explicitly disabled; -1: unknown colour, conservatively mask the torso.
uniform vec4 u_fxSkinTone[2];
float ffSmooth(float a, float b, float x)
{
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float ffSkinLikeness(vec3 c)
{
    float sum = max(c.r + c.g + c.b, 1e-4);
    vec2 d = c.rg / sum - u_fxSkinTone[0].xy;
    float d2 = d.x * d.x * u_fxSkinTone[1].x + 2.0 * d.x * d.y * u_fxSkinTone[1].y + d.y * d.y * u_fxSkinTone[1].z;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float inRange = ffSmooth(u_fxSkinTone[0].z * 0.45, u_fxSkinTone[0].z * 0.65, lum) *
                    (1.0 - ffSmooth(u_fxSkinTone[0].w * 1.1, u_fxSkinTone[0].w * 1.3 + 0.02, lum));
    float satTone = length(u_fxSkinTone[0].xy - vec2_splat(1.0 / 3.0));
    float sat = length(c.rg / sum - vec2_splat(1.0 / 3.0));
    float saturated = ffSmooth(0.55, 0.8, sat / max(satTone, 1e-3));
    return (1.0 - ffSmooth(2.25, 6.25, d2)) * inRange * saturated;
}

#ifdef FX_LIGHT_SPECULAR
uniform vec4 u_fxGloss; // the gloss of the material texture (s_fxMaterial, fx_lighting.sh)
#endif

// Ambient-occlusion volume of the interior (src/effects/ao_volume.hpp; stage 12 = fx::kAoVolumeStage).
// u_fxAoVolume: view space -> volume uvw. u_fxAoParams: x strength (0 = no volume for this draw; times fx_ao_gain), y contrast,
// z normal offset in metres (one cell: every cell the filter mixes lies before the surface), w 1 / what an open surface sees.
// The volume holds four cosine lobes (fx::AoLobeAxes, RGBA); Nw is the world-space normal.
#ifdef FX_FF_AO
SAMPLER3D(s_fxAoVolume, 12);
uniform mat4 u_fxAoVolume;
uniform vec4 u_fxAoParams;
float ffAoVolume(vec3 P, vec3 Ng, vec3 Nw)
{
    if (u_fxAoParams.x <= 0.0)
        return 1.0;
    vec3 uvw = mul(u_fxAoVolume, vec4(P + Ng * u_fxAoParams.z, 1.0)).xyz;
    vec4 lobes = texture3D(s_fxAoVolume, uvw);
    const float k = 0.57735027;
    vec4 w = max(vec4(dot(Nw, vec3(k, k, k)), dot(Nw, vec3(k, -k, -k)), dot(Nw, vec3(-k, k, -k)), dot(Nw, vec3(-k, -k, k))), 0.0);
    float seen = dot(lobes, w) / max(dot(w, vec4_splat(1.0)), 1e-4);
    float ao = clamp(seen * u_fxAoParams.w, 0.0, 1.0);
    return max(mix(1.0, pow(ao, u_fxAoParams.y), u_fxAoParams.x), 0.0); // (the strength may pass 1: fx_ao_gain)
}
#endif

// How open to the sky the pixel's place is (src/effects/visibility.hpp, fx/visibility.hpp; stage 5 =
// fx::kVisibilityStage, the stage 0 cube map's unit, which no lit or baked variant has): x sky (cosine-weighted, to the
// place's bounds), y open (the share of directions more than 15° up that reach out). P and the geometric normal Ng in
// view space: the volume is read off the surface along Ng, on the side the surface faces (a wall's own cells would
// mix in the other side's). u_fxVisibility: view space -> volume uvw; u_fxVisibilityParams: x 1 = this place has a
// volume (0: everything counts as under open sky; 2: the debug view fx_visibility_debug), y that offset in metres, z the
// volume's share while it fades in after it arrived (lighting.cpp kVolumeFade; open sky for the rest).
// Read once per pixel: every term that needs it takes the value.
// A stage 0 cube map takes the unit: such a draw has no lit or baked variant, and its place counts as open.
#ifndef STAGE0_CUBE
SAMPLER3D(s_fxVisibility, 5);
uniform mat4 u_fxVisibility;
uniform vec4 u_fxVisibilityParams;
#endif
vec2 ffVisibility(vec3 P, vec3 Ng)
{
#ifdef STAGE0_CUBE
    return vec2(1.0, 1.0);
#else
    if (u_fxVisibilityParams.x <= 0.0)
        return vec2(1.0, 1.0);
    vec2 seen = texture3D(s_fxVisibility, mul(u_fxVisibility, vec4(P + Ng * u_fxVisibilityParams.y, 1.0)).xyz).xy;
    return mix(vec2(1.0, 1.0), seen, u_fxVisibilityParams.z);
#endif
}
// Whether the sun can reach the pixel at all, from the volume's openness: closed under a roof (an interior, an
// arcade), open as soon as a share of the sky above 15° is. It gates the sun; it does not dim it: where the sun
// comes through, its shadow map alone says how much (the two multiplied darkened a sunlit alley twice).
float ffSunReach(float open)
{
    return smoothstep(0.0, 0.2, open);
}

// The town walls' relight (src/effects/town_light.hpp, lighting.cpp bind_town_wall): [0] the lightmaps' tone a, b, c and
// w its share for this draw (0 off; it fades in 0 -> 1 after it came on, TownLight::wall_weight); [1] towards the live sun (view space); [2] towards the sun the game baked with, w how far the light may rise above the game's (fx::RelightLift); [3], [4] the
// game's sky per bin of ours (bin centres (i + 0.5) / 8).
#ifdef FX_FF_TOWN_WALL
uniform vec4 u_fxTownWall[5];
float ffWallSky(float k)
{
    float x = clamp(k * 8.0 - 0.5, 0.0, 7.0);
    float i = min(floor(x), 6.0);
    int at = int(i);
    float lo = at < 4 ? u_fxTownWall[3][at] : u_fxTownWall[4][at - 4];
    float hi = at + 1 < 4 ? u_fxTownWall[3][at + 1] : u_fxTownWall[4][at - 3];
    return max(k, mix(lo, hi, x - i));
}
#endif
#ifdef FX_FF_WET
#include "ff_wet.sh"
#endif

#ifdef FX_FF_LAMPS
// The lamps of a location, per pixel and with their shadows (src/effects/lamp_shadows.hpp; fx/lamp_shadows.hpp for
// the face convention). All of it in absolute world space.
// u_fxLampView: view space -> world.
// u_fxLamps[0]: x lamp slots (0 = no lamps for this draw), y 1 = GL texture origin (bottom-left), z the lamps' gain
//               on baked colours, w the part of the lamp light the baked colours already hold
// u_fxLamps[1]: x 1 / faces per atlas row, y 1 / atlas rows, z static face size in texels, w dynamic face size
// per slot i at 2 + 3i: xyz position, w radius | rgb colour (with the slot's fade), w 2 = a light that moves (a
//               carried lantern: no bake holds its light, it only adds), else 0 | intensity, core², unused, unused
// u_fxLampGrid[0]: xyz the lamp grid's corner, w debug view (1 each slot in its colour, 2 the shadow mask, 3 the
//                  shadow taps per pixel, 4 the lamps that reach the pixel)
// u_fxLampGrid[1]: xyz 1 / the grid's extent, w cells along x
// u_fxLampGrid[2]: xyz the room light's corner, w > 0 = this draw takes its ambient from the room light, times w
// u_fxLampGrid[3]: xyz 1 / the room light's extent, w 1 = 5x5 filter on the static faces of the strongest lamp (else 3x3)
// u_fxLampGrid[4]: x the first slot of the lights that move, y how many such slots (they are not in the grid),
//                  z the share of a lamp's own colour in what it adds to baked surfaces (the rest: the bake's tone),
//                  w the light under which a lamp is not filtered against its shadow (Settings::lamp_shadow_light
//                  over the strongest gain, gamma space)
// u_fxLampGrid[5..7], [8..10]: per such slot, the rows of the turn from the world into the space of its faces
// u_fxLampGrid[11]: x the most grid lamps per pixel (Settings::lamp_per_pixel), y the faces' near plane
//                   (fx::kLampShadowNear), z the most lamps per pixel whose shadows are filtered (Settings::lamp_filtered;
//                   the others take one tap), w the light (gamma space) under which the strongest lamp takes the 3x3
//                   filter instead of the 5x5 (Settings::lamp_soft_light over the strongest gain)
// u_fxLampFaces[i].x (a frame uniform, the value at the end of the frame for every draw of it): per slot, the faces
//                   (bit f = face f) of its dynamic atlas that hold a caster this frame; the others are not read.
uniform vec4 u_fxLamps[50];
uniform vec4 u_fxLampGrid[12];
uniform vec4 u_fxLampFaces[16];
SAMPLER3D(s_fxRoomLight, 8);
SAMPLER3D(s_fxLampGrid, 9);
SAMPLER2DSHADOW(s_fxLampDynamic, 10);
SAMPLER2DSHADOW(s_fxLampStatic, 11);

// Shadow compare taps of this pixel (the debug view u_fxLampGrid[0].w = 3) and the lamps that reach it (4); globals
// of the shader (HLSL, which the SPIR-V path compiles, reads a global without `static` as a uniform).
#if BGFX_SHADER_LANGUAGE_GLSL
float ffLampTaps = 0.0;
float ffLampsReached = 0.0;
#else
static float ffLampTaps = 0.0;
static float ffLampsReached = 0.0;
#endif

// 3x3 percentage-closer filter in four hardware-filtered compare taps: the optimised PCF of The Witness, after
// MJP's Shadows sample (github.com/TheRealMJP/Shadows, MIT). uv: atlas coordinates, size: the atlas in texels.
// (The macros' own names carry a prefix: their arguments are expressions of the caller's variables.)
#define FF_LAMP_PCF3(result, map, uv, size, depth) \
    { \
        vec2 pcf_texel = (uv) * (size); \
        vec2 pcf_base = floor(pcf_texel + vec2_splat(0.5)); \
        vec2 pcf_st = pcf_texel + vec2_splat(0.5) - pcf_base; \
        pcf_base = (pcf_base - vec2_splat(0.5)) / (size); \
        vec2 pcf_w0 = vec2_splat(3.0) - 2.0 * pcf_st; \
        vec2 pcf_w1 = vec2_splat(1.0) + 2.0 * pcf_st; \
        vec2 pcf_o0 = ((vec2_splat(2.0) - pcf_st) / pcf_w0 - vec2_splat(1.0)) / (size); \
        vec2 pcf_o1 = (pcf_st / pcf_w1 + vec2_splat(1.0)) / (size); \
        result = (pcf_w0.x * pcf_w0.y * shadow2D(map, vec3(pcf_base.x + pcf_o0.x, pcf_base.y + pcf_o0.y, depth)) + \
                  pcf_w1.x * pcf_w0.y * shadow2D(map, vec3(pcf_base.x + pcf_o1.x, pcf_base.y + pcf_o0.y, depth)) + \
                  pcf_w0.x * pcf_w1.y * shadow2D(map, vec3(pcf_base.x + pcf_o0.x, pcf_base.y + pcf_o1.y, depth)) + \
                  pcf_w1.x * pcf_w1.y * shadow2D(map, vec3(pcf_base.x + pcf_o1.x, pcf_base.y + pcf_o1.y, depth))) / 16.0; \
        ffLampTaps += 4.0; \
    }

// The same filter over 5x5 texels in nine taps.
#define FF_LAMP_PCF5(result, map, uv, size, depth) \
    { \
        vec2 pcf_texel = (uv) * (size); \
        vec2 pcf_base = floor(pcf_texel + vec2_splat(0.5)); \
        vec2 pcf_st = pcf_texel + vec2_splat(0.5) - pcf_base; \
        pcf_base = (pcf_base - vec2_splat(0.5)) / (size); \
        vec2 pcf_w0 = vec2_splat(4.0) - 3.0 * pcf_st; \
        vec2 pcf_w2 = vec2_splat(1.0) + 3.0 * pcf_st; \
        vec2 pcf_o0 = ((vec2_splat(3.0) - 2.0 * pcf_st) / pcf_w0 - vec2_splat(2.0)) / (size); \
        vec2 pcf_o1 = ((vec2_splat(3.0) + pcf_st) / 7.0) / (size); \
        vec2 pcf_o2 = (pcf_st / pcf_w2 + vec2_splat(2.0)) / (size); \
        result = (pcf_w0.x * pcf_w0.y * shadow2D(map, vec3(pcf_base.x + pcf_o0.x, pcf_base.y + pcf_o0.y, depth)) + \
                  7.0 * pcf_w0.y * shadow2D(map, vec3(pcf_base.x + pcf_o1.x, pcf_base.y + pcf_o0.y, depth)) + \
                  pcf_w2.x * pcf_w0.y * shadow2D(map, vec3(pcf_base.x + pcf_o2.x, pcf_base.y + pcf_o0.y, depth)) + \
                  pcf_w0.x * 7.0 * shadow2D(map, vec3(pcf_base.x + pcf_o0.x, pcf_base.y + pcf_o1.y, depth)) + \
                  49.0 * shadow2D(map, vec3(pcf_base.x + pcf_o1.x, pcf_base.y + pcf_o1.y, depth)) + \
                  pcf_w2.x * 7.0 * shadow2D(map, vec3(pcf_base.x + pcf_o2.x, pcf_base.y + pcf_o1.y, depth)) + \
                  pcf_w0.x * pcf_w2.y * shadow2D(map, vec3(pcf_base.x + pcf_o0.x, pcf_base.y + pcf_o2.y, depth)) + \
                  7.0 * pcf_w2.y * shadow2D(map, vec3(pcf_base.x + pcf_o1.x, pcf_base.y + pcf_o2.y, depth)) + \
                  pcf_w2.x * pcf_w2.y * shadow2D(map, vec3(pcf_base.x + pcf_o2.x, pcf_base.y + pcf_o2.y, depth))) / 144.0; \
        ffLampTaps += 9.0; \
    }

// The depth a face stores for a point `major` metres along its axis (fx::LampFaceDepth).
float ffLampFaceDepth(float major, float radius)
{
    float near = u_fxLampGrid[11].y;
    return clamp((1.0 - near / max(major, 1e-4)) * radius / (radius - near), 0.0, 1.0);
}

// 1 = the lamp of slot `slot` reaches the point at L from it (unit geometric normal Ng, facing the lamp by ngl),
// 0 = the location or a character stands between them. radius: the lamp's; filtering: 2 = the 5x5 filter on its still
// faces (the strongest lamp of the pixel with Settings::lamp_soft), 1 = 3x3 (the lamps of the pixel's filtered
// budget, Settings::lamp_filtered), 0 = one hardware-filtered tap (the pixel's other lamps); the dynamic faces take
// the 3x3 filter or the one tap alike. Its dynamic faces are read only where they hold a caster.
float ffLampVisibility(float slot, vec3 L, float radius, vec3 Ng, float ngl, float filtering)
{
    float soft = filtering > 1.5 ? 1.0 : 0.0;
    // One texel of a face at this distance, in metres: the receiver steps that far off its surface (further at grazing
    // light), and the compared point is pulled towards the lamp by most of a texel.
    float texel = 2.1 * length(L) / u_fxLamps[1].z;
    vec3 Lo = L + Ng * (texel * (1.0 + soft + 1.5 * (1.0 - clamp(ngl, 0.0, 1.0))) + 0.004);
    vec3 a = abs(Lo);
    vec3 F;
    vec3 R;
    vec3 U;
    float face;
    if (a.x >= a.y && a.x >= a.z)
    {
        if (Lo.x >= 0.0) { face = 0.0; F = vec3(1.0, 0.0, 0.0); R = vec3(0.0, 0.0, -1.0); U = vec3(0.0, 1.0, 0.0); }
        else { face = 1.0; F = vec3(-1.0, 0.0, 0.0); R = vec3(0.0, 0.0, 1.0); U = vec3(0.0, 1.0, 0.0); }
    }
    else if (a.y >= a.z)
    {
        if (Lo.y >= 0.0) { face = 2.0; F = vec3(0.0, 1.0, 0.0); R = vec3(1.0, 0.0, 0.0); U = vec3(0.0, 0.0, -1.0); }
        else { face = 3.0; F = vec3(0.0, -1.0, 0.0); R = vec3(1.0, 0.0, 0.0); U = vec3(0.0, 0.0, 1.0); }
    }
    else
    {
        if (Lo.z >= 0.0) { face = 4.0; F = vec3(0.0, 0.0, 1.0); R = vec3(1.0, 0.0, 0.0); U = vec3(0.0, 1.0, 0.0); }
        else { face = 5.0; F = vec3(0.0, 0.0, -1.0); R = vec3(-1.0, 0.0, 0.0); U = vec3(0.0, 1.0, 0.0); }
    }
    float major = max(dot(Lo, F), 1e-4);
    float s = 1.0 / 1.05; // fx::kLampFaceGuard
    vec2 uv = vec2((face + 0.5 + 0.5 * s * dot(Lo, R) / major) * u_fxLamps[1].x,
                   (slot + 0.5 - 0.5 * s * dot(Lo, U) / major) * u_fxLamps[1].y);
    if (u_fxLamps[0].y > 0.5)
        uv.y = 1.0 - uv.y;
    // Along the ray to the lamp the stored depth only grows: the pull in metres becomes a shorter axis distance.
    float len = max(length(Lo), 1e-4);
    float depth = ffLampFaceDepth(major * (len - 0.75 * texel) / len, radius);
    vec2 faces = vec2(1.0 / u_fxLamps[1].x, 1.0 / u_fxLamps[1].y);
    float lit;
    if (filtering > 1.5)
    {
        FF_LAMP_PCF5(lit, s_fxLampStatic, uv, faces * u_fxLamps[1].z, depth)
    }
    else if (filtering > 0.5)
    {
        FF_LAMP_PCF3(lit, s_fxLampStatic, uv, faces * u_fxLamps[1].z, depth)
    }
    else
    {
        lit = shadow2D(s_fxLampStatic, vec3(uv, depth));
        ffLampTaps += 1.0;
    }
    if (mod(floor(u_fxLampFaces[int(slot + 0.5)].x / exp2(face) + 0.001), 2.0) > 0.5)
    {
        // The characters' faces are coarser by the ratio of the face sizes.
        float pull = 0.75 * texel + texel * (u_fxLamps[1].z / u_fxLamps[1].w - 1.0);
        float movingDepth = ffLampFaceDepth(major * (len - pull) / len, radius);
        float moving;
        if (filtering > 0.5)
        {
            FF_LAMP_PCF3(moving, s_fxLampDynamic, uv, faces * u_fxLamps[1].w, movingDepth)
        }
        else
        {
            moving = shadow2D(s_fxLampDynamic, vec3(uv, movingDepth));
            ffLampTaps += 1.0;
        }
        lit = min(lit, moving);
    }
    return lit;
}

vec3 ffLampDebugColor(float slot)
{
    vec3 hue = abs(fract(vec3_splat(slot * 0.381966) + vec3(0.0, 0.3333, 0.6667)) * 6.0 - vec3_splat(3.0)) - vec3_splat(1.0);
    return clamp(hue, 0.0, 1.0);
}

// The light of the lamp in slot `slot` at Pw without its shadow and its surface's turn: its strongest channel times
// its falloff (which lamp of a pixel is the strongest).
float ffLampShown(float slot, vec3 Pw)
{
    int base = 2 + 3 * int(slot + 0.5);
    vec4 lamp = u_fxLamps[base];
    float reach = length(Pw - lamp.xyz);
    if (reach >= lamp.w)
        return 0.0;
    vec3 c = u_fxLamps[base + 1].rgb;
    vec4 shape = u_fxLamps[base + 2];
    return max(c.r, max(c.g, c.b)) * fxLightFalloff(reach, lamp.w, shape.x, shape.y);
}

// Adds the lamp with grid number `id` (slot + 1, 0 = none) at Pw: sum.rgb its light with its shadow, sum.a the
// luminance of its light without the shadow. Ng: geometric normal (shadow bias, no light from behind the
// surface), N: shading normal. fxLightFalloff: the lamp's shape, zero at its radius. strongest: the slot of the
// pixel's strongest lamp (the 5x5 filter with Settings::lamp_soft, else 3x3), -1 = none. filtered: how many more of
// the pixel's lamps take the 3x3 filter (Settings::lamp_filtered less the strongest; the first lamps that look their
// shadow up, in the loop's order: the lights that move, then the grid's strongest first); the others take one tap.
// specV, spec: the world-space unit vector to the eye and the pixel's highlight (fxGlossSpec / fxMaterialSpec); the
// lamp's highlight with its shadow is added to lampSpec.
// shown: the lamp's light at the pixel (gamma space): the 5x5 filter only where it is bright enough for its wider
// penumbra to show (u_fxLampGrid[11].w, Settings::lamp_soft_light), the 3x3 below.
float ffLampFilter(float slot, float strongest, float shown, inout float filtered)
{
    if (abs(slot - strongest) < 0.5)
        return u_fxLampGrid[3].w > 0.5 && shown >= u_fxLampGrid[11].w ? 2.0 : 1.0;
    if (filtered < 0.5)
        return 0.0;
    filtered -= 1.0;
    return 1.0;
}
void ffLampAdd(float id, vec3 Pw, vec3 Ng, vec3 N, vec3 specV, vec2 spec, float strongest, inout float filtered,
               inout vec4 sum, inout vec3 lampSpec)
{
    if (id < 0.5)
        return;
    float slot = id - 1.0;
    int base = 2 + 3 * int(slot);
    vec4 lamp = u_fxLamps[base];
    vec3 L = Pw - lamp.xyz;
    float reach = length(L);
    if (reach >= lamp.w)
        return;
    ffLampsReached += 1.0;
    vec4 aggregateShape = u_fxLamps[base+2];
    if (aggregateShape.z > 0.5)
    {
        vec4 diffuse = vec4_splat(0.0);
        vec3 highlight = vec3_splat(0.0);
        fxAggregateAdd(aggregateShape.z,Pw,Ng,N,specV,spec,aggregateShape.w,1.0,diffuse,highlight);
        // Exactly one visibility, including grazing-center normals. At the center the lookup direction is undefined.
        float seen = 1.0;
        if (reach > 1e-4)
            seen = ffLampVisibility(slot, L, lamp.w, Ng, dot(Ng, -L / reach), ffLampFilter(slot, strongest,
                                                                                    fxLinear() ? sqrt(diffuse.a) : diffuse.a, filtered));
        if (u_fxLampGrid[0].w > 0.5)
            diffuse.rgb = ffLampDebugColor(slot) * diffuse.a;
        sum.rgb += diffuse.rgb * seen;
        sum.a += diffuse.a;
        lampSpec += highlight * seen;
        return;
    }
    vec3 toLamp = -L / max(reach, 1e-4);
    float ngl = dot(Ng, toLamp);
    float ndl = dot(N, toLamp);
    if (ngl <= 0.0 || ndl <= 0.0)
        return;
    vec4 color = u_fxLamps[base + 1];
    vec4 shape = u_fxLamps[base + 2];
    float moving = color.w > 1.5 ? 1.0 : 0.0;
    // Every light of the place, still or moving, with the one falloff of all lights (fx/light_falloff.hpp): its
    // radius is where it ends.
    float falloff = fxLightFalloff(reach, lamp.w, shape.x, shape.y);
    float light = ndl * falloff;
    vec3 rgb = fxLightColor(color.rgb);
    if (u_fxLampGrid[0].w > 0.5)
        rgb = ffLampDebugColor(slot) * dot(rgb, vec3(0.299, 0.587, 0.114));
    // Light the picture cannot show (under half a step, fx::kLampNegligible) is not filtered against its shadow.
    float shown = max(rgb.r, max(rgb.g, rgb.b)) * light;
    if (fxLinear())
        shown = sqrt(shown);
    float seen = 1.0;
    if (shown >= u_fxLampGrid[4].w)
    {
        // The faces of a ship's lantern are in the ship's space: the direction and the normal turn into it.
        vec3 Lf = L;
        vec3 Nf = Ng;
        if (moving > 0.5)
        {
            int turn = 5 + 3 * int(slot - u_fxLampGrid[4].x + 0.5);
            Lf = vec3(dot(L, u_fxLampGrid[turn].xyz), dot(L, u_fxLampGrid[turn + 1].xyz), dot(L, u_fxLampGrid[turn + 2].xyz));
            Nf = vec3(dot(Ng, u_fxLampGrid[turn].xyz), dot(Ng, u_fxLampGrid[turn + 1].xyz), dot(Ng, u_fxLampGrid[turn + 2].xyz));
        }
        seen = ffLampVisibility(slot, Lf, lamp.w, Nf, ngl, ffLampFilter(slot, strongest, shown, filtered));
    }
    sum.rgb += rgb * (light * seen);
    sum.a += dot(rgb, vec3(0.299, 0.587, 0.114)) * light;
#ifdef FX_LIGHT_SPECULAR
    if (spec.y > 0.0)
    {
        lampSpec += rgb * (fxSpecLobe(N, specV, toLamp, spec) * clamp(ndl * 4.0, 0.0, 1.0) * falloff * seen);
    }
#endif
}

// How much of the lamps' part `lamp` a baked colour takes over its lamp-free part `base` (both in the space they add
// in: energy in linear light), one scale for all channels so the lamps' hue stays. The scene is 8-bit and the
// composite gives it its highlights back (fxExpandLdr): a channel past 1 is clipped, so near a lamp the relit colour
// burnt out to a flat yellow-white without its texture. Up to half of the headroom h that `base` leaves in the lamps'
// direction their light is added as it is; above, a shoulder with the same slope where it starts, x -> h - h*h/(4x),
// reaches h only at infinite light: the pool stays bright, its texture and colour stay.
float ffLampRoll(vec3 base, vec3 lamp)
{
    float peak = max(lamp.r, max(lamp.g, lamp.b));
    if (peak <= 1e-4)
        return 1.0;
    vec3 room = max(vec3_splat(1.0) - base, vec3_splat(0.0)) * peak;
    vec3 along = max(lamp, vec3_splat(1e-3 * peak));
    float h = min(room.r / along.r, min(room.g / along.g, room.b / along.b));
    if (peak <= 0.5 * h)
        return 1.0;
    return h * (1.0 - h / (4.0 * peak)) / peak;
}

// The light of the lights that move (a carried lantern, a ship's lanterns) at the pixel, with their shadows. The
// bake holds nothing of them: it is plain added light, as the game's own point lights, not a part of the relit sum:
// `lampMoved` of ffLampLight.

// The lamps that reach Pw: the lights that move, and from the lamp grid up to u_fxLampGrid[11].x of the eight of its
// cell, strongest first; `lampSpec` their highlights (ffLampAdd).
vec4 ffLampLight(vec3 Pw, vec3 Ng, vec3 N, vec3 specV, vec2 spec, out vec3 lampMoved, out vec3 lampSpec)
{
    vec4 sum = vec4_splat(0.0);
    lampMoved = vec3_splat(0.0);
    lampSpec = vec3_splat(0.0);
    if (u_fxLamps[0].x < 0.5)
        return sum;
    float cap = u_fxLampGrid[11].x;
    vec4 near = vec4_splat(0.0);
    vec4 more = vec4_splat(0.0);
    vec3 cell = (Pw - u_fxLampGrid[0].xyz) * u_fxLampGrid[1].xyz;
    if (min(min(cell.x, cell.y), cell.z) >= 0.0 && max(max(cell.x, cell.y), cell.z) <= 1.0)
    {
        float half_texel = 0.5 / u_fxLampGrid[1].w;
        float u = 0.5 * clamp(cell.x, half_texel, 1.0 - half_texel);
        near = floor(texture3D(s_fxLampGrid, vec3(u, cell.y, cell.z)) * 255.0 + vec4_splat(0.5));
        if (near.w > 0.5 && cap > 4.5)
            more = floor(texture3D(s_fxLampGrid, vec3(u + 0.5, cell.y, cell.z)) * 255.0 + vec4_splat(0.5));
    }
    // The strongest lamp of the pixel takes the best filter (ffLampFilter): the first of its cell (the grid lists the
    // strongest first) or a light that moves, whichever is stronger here.
    float strongest = -1.0;
    {
        float best = 0.0;
        if (near.x > 0.5)
        {
            strongest = near.x - 1.0;
            best = ffLampShown(strongest, Pw);
        }
        for (int i = 0; i < 2; ++i)
        {
            float slot = u_fxLampGrid[4].x + float(i);
            if (u_fxLampGrid[4].y > float(i) + 0.5)
            {
                float s = ffLampShown(slot, Pw);
                if (s > best)
                {
                    best = s;
                    strongest = slot;
                }
            }
        }
    }
    float filtered = u_fxLampGrid[11].z - 1.0;
    // One loop over the pixel's lights, so the program holds ffLampAdd (its filters, its aggregate loop) once: ten
    // inlined copies made the lamp programs four times the size of the others, and their links as slow. First the
    // lights that move (at most two: few and anywhere, every pixel asks them, without the grid), which leave the sum
    // as `lampMoved`, then the lamps of the grid's cell, the first always, the others up to the cap.
    for (int i = 0; i < 10; ++i)
    {
        float id = 0.0;
        if (i < 2)
        {
            if (u_fxLampGrid[4].y > float(i) + 0.5)
                id = u_fxLampGrid[4].x + float(i) + 1.0;
        }
        else
        {
            if (i == 2)
            {
                // The debug views show every light the same way: the grid's lamps add to the moving ones.
                if (u_fxLampGrid[0].w <= 0.5)
                {
                    lampMoved = sum.rgb;
                    sum = vec4_splat(0.0);
                }
            }
            int k = i - 2;
            if (k > 0 && cap < float(k) + 0.5)
                break;
            vec4 cellIds = k < 4 ? near : more;
            int c = k < 4 ? k : k - 4;
            id = c == 0 ? cellIds.x : c == 1 ? cellIds.y : c == 2 ? cellIds.z : cellIds.w;
        }
        ffLampAdd(id, Pw, Ng, N, specV, spec, strongest, filtered, sum, lampSpec);
    }
    return sum;
}

// The light of the place around Pw (fx::RoomLight: its baked colours), in the baked colours' own units; Nw: the
// surface's normal (the light is taken half a metre in front of it).
vec3 ffRoomLight(vec3 Pw, vec3 Nw)
{
    vec3 uvw = (Pw + Nw * 0.5 - u_fxLampGrid[2].xyz) * u_fxLampGrid[3].xyz;
    return texture3D(s_fxRoomLight, clamp(uvw, 0.0, 1.0)).rgb;
}
#endif // FX_FF_LAMPS
#define FF_DIFFUSE ffDiffuse
#else
#define FF_DIFFUSE v_color0
#endif

// per stage i: [3i] colour op, arg1, arg2, arg0   [3i+1] alpha op, arg1, arg2, arg0
//              [3i+2] x: result goes to TEMP  y: projected  z: texture bound  w: point sprite
uniform vec4 u_ffStage[15];
SAMPLER2D(s_tex0, 0);
SAMPLER2D(s_tex1, 1);
SAMPLER2D(s_tex2, 2);
SAMPLER2D(s_tex3, 3);
SAMPLER2D(s_tex4, 4);
#ifdef STAGE0_CUBE
SAMPLERCUBE(s_cube0, 5);
#endif
#ifdef STAGE1_CUBE
SAMPLERCUBE(s_cube1, 6);
#endif
#ifdef STAGE2_CUBE
SAMPLERCUBE(s_cube2, 7);
#endif

// The town light (src/effects/town_light.hpp): the stage that samples a town's lightmap multiplies it by the hour's sun
// over the bake's, a light ratio map (fx/sun_relight.hpp) the shim binds at a fixed-function unit the draw leaves
// free, read with the lightmap's own coordinates. x: the lightmap's stage (-1: none), y: the ratio's unit (2, 3, 4),
// z: its weight (0 -> 1 while it fades in), w: 1 = debug view (the map holds the hour's sun visibility: grey).
uniform vec4 u_ffTownRatio;
// The ratio map's texel at the lightmap's coordinates: read once per pixel in main() (FF_TOWN_RATIO_READ), not in every
// stage. Expanded in each of the five stages it was 15 texture reads in branches of the fixed-function fragment
// programs, the largest the game has.
float ffTownRatioTexel(vec2 uv)
{
    if (u_ffTownRatio.y < 2.5)
        return texture2D(s_tex2, uv).r;
    if (u_ffTownRatio.y < 3.5)
        return texture2D(s_tex3, uv).r;
    return texture2D(s_tex4, uv).r;
}
vec3 ffTownRatio(vec3 lightmap, float e)
{
    if (u_ffTownRatio.w > 0.5)
        return mix(lightmap, vec3_splat(0.125 + 0.75 * e), u_ffTownRatio.z); // MODULATE2X: 0.25..1.0 of the paint x2
    // fx/sun_relight.hpp: 3^((255 e - 127) / 127).
    float ratio = exp2((e * 255.0 - 127.0) * (1.5849625 / 127.0));
    return min(lightmap * mix(1.0, ratio, u_ffTownRatio.z), vec3_splat(1.0));
}

// diffuse: FF_DIFFUSE; specular: v_color1 (stage inputs, passed in: storm_common.sh).
vec4 ffArg(float arg, vec4 current, vec4 tex, vec4 temp, vec4 diffuse, vec4 specular)
{
    float flags = floor(arg / 16.0);
    float sel = arg - flags * 16.0;
    vec4 r;
    if (sel < 0.5) r = diffuse;
    else if (sel < 1.5) r = current;
    else if (sel < 2.5) r = tex;
    else if (sel < 3.5) r = u_ffFs[0];
    else if (sel < 4.5) r = specular;
    else r = temp;
    float complement = flags - 2.0 * floor(flags / 2.0);
    if (complement > 0.5) r = vec4_splat(1.0) - r;
    if (flags > 1.5) r = r.aaaa;
    return r;
}

vec4 ffCombine(float op, vec4 a1, vec4 a2, vec4 a0, vec4 current, vec4 tex, vec4 diffuse)
{
    if (op < 2.5) return a1;
    if (op < 3.5) return a2;
    if (op < 4.5) return a1 * a2;
    if (op < 5.5) return a1 * a2 * 2.0;
    if (op < 6.5) return a1 * a2 * 4.0;
    if (op < 7.5) return a1 + a2;
    if (op < 8.5) return a1 + a2 - vec4_splat(0.5);
    if (op < 9.5) return (a1 + a2 - vec4_splat(0.5)) * 2.0;
    if (op < 10.5) return a1 - a2;
    if (op < 11.5) return a1 + a2 - a1 * a2;
    if (op < 12.5) return mix(a2, a1, diffuse.a);
    if (op < 13.5) return mix(a2, a1, tex.a);
    if (op < 14.5) return mix(a2, a1, u_ffFs[0].a);
    if (op < 15.5) return a1 + a2 * (1.0 - tex.a);
    if (op < 16.5) return mix(a2, a1, current.a);
    if (op < 17.5) return a1;
    if (op < 18.5) return vec4(a1.rgb + a1.a * a2.rgb, a1.a);
    if (op < 19.5) return vec4(a1.rgb * a2.rgb + a1.aaa, a1.a);
    if (op < 20.5) return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
    if (op < 21.5) return vec4((vec3_splat(1.0) - a1.rgb) * a2.rgb + a1.aaa, a1.a);
    if (op < 23.5) return a1;
    if (op < 24.5) return vec4_splat(clamp(4.0 * dot(a1.rgb - vec3_splat(0.5), a2.rgb - vec3_splat(0.5)), 0.0, 1.0));
    if (op < 25.5) return a0 + a1 * a2;
    return mix(a2, a1, a0);
}

#define FF_ARG(arg) ffArg(arg, current, tex, temp, FF_DIFFUSE, v_color1)
// ms.z above 1: the stage's texture was swapped a moment ago (a finished transcode, its x2: src/shim/texture_fade.hpp)
// and the previous one, bound at the first unit the draw leaves free, still shows by ms.z - 1.
#define FF_FADE(tex, ms, PREV) if (ms.z > 1.0) { tex = mix(tex, PREV, ms.z - 1.0); }
#define FF_NO_FADE(tex, ms, PREV)
// The town ratio's texel (ffTownRatioTexel) with the coordinates of the lightmap's stage u_ffTownRatio.x; stage inputs:
// only main() can name them in the SPIR-V build (storm_common.sh).
#define FF_TOWN_RATIO_READ(e) \
    float e = 1.0; \
    if (u_ffTownRatio.x > -0.5) \
    { \
        float k = u_ffTownRatio.x; \
        vec4 ttc = k < 0.5 ? v_texcoord0 : k < 1.5 ? v_texcoord1 : k < 2.5 ? v_texcoord2 : k < 3.5 ? v_texcoord3 : v_texcoord4; \
        vec4 tms = k < 0.5 ? u_ffStage[2] : k < 1.5 ? u_ffStage[5] : k < 2.5 ? u_ffStage[8] : k < 3.5 ? u_ffStage[11] : u_ffStage[14]; \
        e = ffTownRatioTexel(FF_UV(ttc, tms)); \
    }
#define FF_STAGE(i, SAMPLE, FADE, PREV) \
    if (running && float(i) < u_ffFs[3].z) \
    { \
        vec4 cs = u_ffStage[3 * i]; \
        vec4 as = u_ffStage[3 * i + 1]; \
        vec4 ms = u_ffStage[3 * i + 2]; \
        vec4 tex = vec4(0.0, 0.0, 0.0, 1.0); \
        if (ms.z > 0.5) \
        { \
            tex = SAMPLE; \
            FADE(tex, ms, PREV) \
            if (float(i) == u_ffTownRatio.x) tex.rgb = ffTownRatio(tex.rgb, townTexel); \
        } \
        FF_KEEP_TEX(i, tex) \
        vec4 c = ffCombine(cs.x, FF_ARG(cs.y), FF_ARG(cs.z), FF_ARG(cs.w), current, tex, FF_DIFFUSE); \
        vec4 a = as.x < 1.5 ? current : ffCombine(as.x, FF_ARG(as.y), FF_ARG(as.z), FF_ARG(as.w), current, tex, FF_DIFFUSE); \
        vec4 r = clamp(vec4(c.rgb, cs.x > 23.5 && cs.x < 24.5 ? c.a : a.a), 0.0, 1.0); \
        if (ms.x > 0.5) temp = r; else current = r; \
    } \
    else { running = false; }

#ifdef FX_FF_PIXEL_LIGHTING
#define FF_KEEP_TEX(i, tex) if (i == 0) tex0 = tex;
#else
#define FF_KEEP_TEX(i, tex)
#endif

// ms.w: point-sprite draw (D3DRS_POINTSPRITEENABLE on a point list only). The vertex shader supplies the
// window-space centre and pixel size for every stage. Reconstruct the generated coordinates because bgfx's
// SPIR-V/HLSL profile does not expose gl_PointCoord. The centre includes inset/scaled viewport placement.
#define FF_POINT_UV(tc) ((gl_FragCoord.xy - (tc).xy) / max((tc).z, 1e-6) + vec2_splat(0.5))
#define FF_UV(tc, ms) ((ms).w > 0.5 ? FF_POINT_UV(tc) : (ms).y > 0.5 ? (tc).xy / (tc).w : (tc).xy)

#ifdef FX_LIGHT_PARALLAX
// The parallax variant is only chosen for a relief stage without projected coordinates (effects/lighting.cpp): that
// stage (u_fxFf.y) is read where the eye's ray meets the relief, the other as it is.
#ifndef STAGE0_SAMPLE
#define STAGE0_SAMPLE (u_fxFf.y > 0.5 ? texture2D(s_tex0, FF_UV(v_texcoord0, ms)) : texture2DGrad(s_tex0, ffReliefUv, ffReliefDx, ffReliefDy))
#endif
#ifndef STAGE1_SAMPLE
#define STAGE1_SAMPLE (u_fxFf.y > 0.5 ? texture2DGrad(s_tex1, ffReliefUv, ffReliefDx, ffReliefDy) : texture2D(s_tex1, FF_UV(v_texcoord1, ms)))
#endif
#else
#ifndef STAGE0_SAMPLE
#define STAGE0_SAMPLE texture2D(s_tex0, FF_UV(v_texcoord0, ms))
#endif
#ifndef STAGE1_SAMPLE
#define STAGE1_SAMPLE texture2D(s_tex1, FF_UV(v_texcoord1, ms))
#endif
#endif
#ifndef STAGE2_SAMPLE
#define STAGE2_SAMPLE texture2D(s_tex2, FF_UV(v_texcoord2, ms))
#endif
// The previous texture of a fading stage 0 / 1 (FF_FADE): stage 0's at unit 4, stage 1's at unit 3, read as the stage.
#ifdef FX_LIGHT_PARALLAX
#define STAGE0_PREV (u_fxFf.y > 0.5 ? texture2D(s_tex4, FF_UV(v_texcoord0, ms)) : texture2DGrad(s_tex4, ffReliefUv, ffReliefDx, ffReliefDy))
#define STAGE1_PREV (u_fxFf.y > 0.5 ? texture2DGrad(s_tex3, ffReliefUv, ffReliefDx, ffReliefDy) : texture2D(s_tex3, FF_UV(v_texcoord1, ms)))
#else
#define STAGE0_PREV texture2D(s_tex4, FF_UV(v_texcoord0, ms))
#define STAGE1_PREV texture2D(s_tex3, FF_UV(v_texcoord1, ms))
#endif
#define STAGE3_SAMPLE texture2D(s_tex3, FF_UV(v_texcoord3, ms))
#define STAGE4_SAMPLE texture2D(s_tex4, FF_UV(v_texcoord4, ms))

void main()
{
#ifdef FX_FF_PIXEL_LIGHTING
    vec3 P = v_position.xyz;
    vec3 V = normalize(-P);
    vec3 N = normalize(v_normal.xyz);
#ifdef FX_FF_LIT
    if (u_fxFf.x > 0.5)
    {
        // Two-sided thin surface (sails, flags): light the side the viewer sees; the light from behind comes
        // through as translucency. The facing comes from the triangle itself, so the row order of the target
        // does not matter.
        vec3 face = cross(dFdx(P), dFdy(P));
        if (dot(face, V) < 0.0) face = -face;
        if (dot(N, face) < 0.0) N = -N;
    }
#endif
    vec3 Ng = N;
    // The skin weight of a character's pixel (ff_vertex.sh: the share of the head, neck and hand bones; 0 on every draw
    // that is not skinned): its relief, parallax, cavity and gloss fade out on faces, hands and hair, whose painted
    // shading the relief's models read as bumps.
    // When in doubt the relief goes: half a bone's share of skin (a collar, a cuff at the wrist)
    // already takes all of it.
    float ffSkin = clamp(2.0 * v_normal.w, 0.0, 1.0);
    // A character's torso and arms: skin where the atlas there has the character's own skin colour (fx/skin_tone.hpp,
    // u_fxSkinTone; read at about 64 texels across, so the grain of leather and dark cloth averages into its own colour).
    // This also protects generated gloss in specular-only variants, before normal maps become ready.
    float ffTorso = clamp(v_position.w, 0.0, 1.0);
    if (ffTorso > 0.004)
    {
        if (u_fxSkinTone[0].x < 0.0)
            ffSkin = max(ffSkin, clamp(2.0 * ffTorso, 0.0, 1.0));
        else if (u_fxSkinTone[0].x > 0.0)
            ffSkin = max(ffSkin, clamp(2.0 * ffTorso * ffSkinLikeness(texture2DLod(s_tex0, v_texcoord0.xy, u_fxSkinTone[1].w).rgb), 0.0, 1.0));
    }
#ifdef FX_LIGHT_NORMALMAP
    // The relief follows the coordinates of its stage (u_fxFf.y); with the parallax that stage's texture is then read
    // where the relief's map is (ffReliefUv, with the derivatives of the unshifted coordinates).
    vec2 ffReliefUv = u_fxFf.y > 0.5 ? v_texcoord1.xy : v_texcoord0.xy;
    vec2 ffReliefDx = dFdx(ffReliefUv);
    vec2 ffReliefDy = dFdy(ffReliefUv);
    vec3 reliefGu;
    vec3 reliefGv;
    fxReliefFrame(Ng, P, ffReliefDx, ffReliefDy, reliefGu, reliefGv);
#ifdef FX_LIGHT_PARALLAX
    FxParallaxFrame ffPomFrame = fxParallaxFrame(dFdx(P), dFdy(P), ffReliefDx, ffReliefDy, V);
    vec3 ffPomFound;
    vec2 ffReliefUv0 = ffReliefUv;
    ffPomFound = vec3_splat(0.0);
    if (ffSkin < 0.999)
        ffReliefUv = mix(fxParallax(ffReliefUv, ffReliefDx, ffReliefDy, ffPomFrame.gu, ffPomFrame.gv,
                                  ffPomFrame.normal, V, length(P), ffPomFound),
                         ffReliefUv0, ffSkin);
    N = fxPerturbNormal(Ng, reliefGu, reliefGv, texture2DGrad(s_fxNormalMap, ffReliefUv, ffReliefDx, ffReliefDy).rg);
#else
    N = fxPerturbNormal(Ng, reliefGu, reliefGv, texture2D(s_fxNormalMap, ffReliefUv).rg);
#endif
    N = normalize(mix(N, Ng, ffSkin));
#endif
    // The highlight of this pixel: its gloss texel (stage 0's coordinates; the G of the material texture) or the
    // material's. ffGloss -1: no gloss map (ff_wet.sh may read it).
    float ffGloss = -1.0;
    vec2 ffSpec = fxMaterialSpec();
    // The metal of the pixel (0..1): a metal texture's polished texels; their highlight takes the texture's colour and
    // their diffuse light is a little less (subtle: no chrome look). Rust, carriages and grime are less glossy, so no
    // metal.
    float ffMetal = 0.0;
#ifdef FX_LIGHT_SPECULAR
    if (u_fxGloss.x > 0.0)
    {
        // G the gloss; R a gloss map's metal mask (fx::GlossMaterial, u_fxGloss.y > 0) or a relief's height (u_fxGloss.y
        // < 0 on metal: its polished texels by their gloss, which the metal mask floored at 0.5).
        vec2 ffMaterial = texture2D(s_fxMaterial, v_texcoord0.xy).rg;
        ffGloss = ffMaterial.g;
        float glossShare = u_fxGloss.x * (1.0 - ffSkin); // skin keeps the material's highlight
        ffMetal = (u_fxGloss.y > 0.0 ? u_fxGloss.y * ffMaterial.r : -u_fxGloss.y * smoothstep(0.4, 0.55, ffGloss)) * glossShare;
        // Metal reflects several times what lacquer or glazing do.
        ffSpec = mix(ffSpec, fxGlossSpec(ffGloss) * vec2(1.0, (1.0 + 2.5 * ffMetal) * u_fxGloss.w), glossShare);
    }
#endif
    vec3 ffSpecV = normalize(mul(u_fxLampView, vec4(V, 0.0)).xyz);
    vec3 Pw = mul(u_fxLampView, vec4(P, 1.0)).xyz;
    vec3 Nw = normalize(mul(u_fxLampView, vec4(Ng, 0.0)).xyz);
    // How open the pixel is to the sky (the visibility volume): x the sky it sees, y whether the sun can reach it.
    // Under a roof the sun and the weather's ambient do not arrive and the room's own occlusion counts in full; in
    // a doorway, by a window or under a hatch each arrives by its share.
    vec2 skySeen = ffVisibility(P, Ng);
    float sunReach = ffSunReach(skySeen.y);
    // The rain (ff_wet.sh): where it reaches (the openness above 15°: dry under roofs), the surface is wet, its relief
    // filled with water and its gloss raised, before the lamps and the sun take them; puddles in the relief's hollows.
#ifdef FX_FF_WET
    float ffHeight = 1.0;
#ifdef FX_LIGHT_PARALLAX
    if (u_fxWet[0].y > 0.5)
        ffHeight = ffPomFound.x > 0.0 ? 1.0 - ffPomFound.y : texture2DLod(s_fxMaterial, ffReliefUv, ffPomFound.z).r;
#endif
    vec4 ffWet = ffWetPrepare(Nw, skySeen.y, ffGloss, ffMetal, ffHeight);
    if (ffWet.x > 0.0)
    {
        N = ffWetNormal(N, Ng, ffWet);
        ffSpec = ffWetSpec(ffSpec, ffWet, ffMetal);
    }
#else
    vec4 ffWet = vec4(0.0, 0.0, 0.0, 1.0); // dry (ffWetPrepare's)
#endif
    // The lamps' highlights: with the gloss on (u_fxGloss.w), or wet.
    vec2 ffLampSpecOf = ffSpec;
#ifdef FX_LIGHT_SPECULAR
    if (u_fxGloss.w <= 0.0 && ffWet.x <= 0.0)
        ffLampSpecOf.y = 0.0;
#endif
    vec3 ffPointDiffuse, ffPointSpecular;
    fxPointTerms(Ng,N,P,V,ffSpec,ffPointDiffuse,ffPointSpecular);
    // The place's lamps with their shadows (nothing where there are none).
#ifdef FX_FF_LAMPS
    vec3 ffLampMoved;
    vec3 ffLampSpec;
    vec4 lamps = ffLampLight(Pw, Nw, normalize(mul(u_fxLampView, vec4(N, 0.0)).xyz), ffSpecV, ffLampSpecOf, ffLampMoved, ffLampSpec);
#else
    vec3 ffLampMoved = vec3_splat(0.0);
    vec3 ffLampSpec = vec3_splat(0.0);
    vec4 lamps = vec4_splat(0.0);
#endif
#ifdef FX_FF_AO
    float aoVolume = mix(ffAoVolume(P, Ng, Nw), 1.0, skySeen.x);
#else
    float aoVolume = 1.0;
#endif
    // The relief's cavity: an occlusion at the texture's scale, under the light as the volume's is (ambient
    // in full, half of the direct light); a puddle's water fills it.
    float cavity = 1.0;
#ifdef FX_LIGHT_PARALLAX
    cavity = mix(fxReliefCavity(ffReliefUv, ffPomFound), 1.0, max(ffWet.y, ffSkin));
#endif
    float cavityDirect = mix(1.0, cavity, 0.5);
#ifdef FX_FF_LIT
    // The volume occludes the ambient part (the per-vertex sum carries the global ambient and the lights not lit per
    // pixel); the per-pixel sun and the lamps keep their own visibility.
    float sunShadow = fxSunShadow(P, Ng) * sunReach * cavityDirect;
#ifdef FX_LIGHT_PARALLAX
    sunShadow *= mix(fxParallaxShadow(ffReliefUv, ffPomFound, ffPomFrame.gu, ffPomFrame.gv,
                                    ffPomFrame.normal, u_fxLight[0].xyz), 1.0, ffSkin);
#endif
    vec3 ambient = fxAmbient(N);
    float lampSeen = dot(lamps.rgb, vec3(0.299, 0.587, 0.114));
#ifdef FX_FF_LAMPS
    if (u_fxLampGrid[2].w > 0.5)
    {
        // Indoors the model stands in the room's own light: the baked colours around it are its ambient (the global
        // ambient is the weather's, of the street). Where lamps light the pixel directly, the part of them the bake
        // holds leaves the ambient, as on the baked surfaces.
        vec3 room = ffRoomLight(Pw, Nw);
        float level = max(dot(room, vec3(0.299, 0.587, 0.114)), 0.03);
        // The room's level, the models' lift over it and the lamps' gain are scales of light: squared in linear light,
        // as the baked colours' relight below takes them, so models and the room around them stay as bright as each
        // other in either space.
        float lift = u_fxLampGrid[2].w;
        float lampGain = u_fxLamps[0].z;
        if (fxLinear())
        {
            level *= level;
            lift *= lift;
            lampGain *= lampGain;
        }
        vec3 roomAmbient = fxLightColor(room) * (lift * level / (level + u_fxLamps[0].w * lampGain * lamps.a));
        // The sky's light where the sky is seen: fxAmbient is the weather's ambient, the hemisphere's or a flat one
        // (src/effects/lighting.cpp), since the vertex sum's is switched off here.
        ambient = mix(roomAmbient, ambient, skySeen.x);
    }
#endif
    // A lamp's light on a model is at most that of a fully lit surface: above half of it the sum is compressed
    // towards 1 (a model at a candle's flame is lit, not burnt out).
    float lampShown = lampSeen <= 0.5 ? lampSeen : 0.5 + (lampSeen - 0.5) / (1.0 + 2.0 * (lampSeen - 0.5));
    vec3 lampLight = lamps.rgb * (lampShown / max(lampSeen, 1e-4)) * cavityDirect;
    // The occlusion of the ambient, as on the vertex sum (squared with it in linear light: fxLit).
    float ambientShade = aoVolume * cavity;
    if (fxLinear())
        ambientShade *= ambientShade;
    vec4 ffDiffuse = vec4(clamp(fxLit(v_color0.rgb * (aoVolume * cavity), v_matDiffuse.rgb,
                                 fxSunDiffuse(N, V) * sunShadow + (ffPointDiffuse + ffLampMoved) * cavityDirect +
                                     lampLight,
                                 v_matAmbient.rgb, ambient * ambientShade), 0.0, 1.0), v_color0.a);
#else
    vec4 ffDiffuse = v_color0;
#endif
    vec4 tex0 = vec4_splat(0.0); // stage 0's texel: the baked relight, the tint of a metal highlight
#endif
    vec4 current = FF_DIFFUSE;
    vec4 temp = vec4_splat(0.0);
    FF_TOWN_RATIO_READ(townTexel)
    bool running = true;
    FF_STAGE(0, STAGE0_SAMPLE, FF_FADE, STAGE0_PREV)
    FF_STAGE(1, STAGE1_SAMPLE, FF_FADE, STAGE1_PREV)
    FF_STAGE(2, STAGE2_SAMPLE, FF_NO_FADE, tex)
    FF_STAGE(3, STAGE3_SAMPLE, FF_NO_FADE, tex)
    FF_STAGE(4, STAGE4_SAMPLE, FF_NO_FADE, tex)
#ifdef FX_FF_DYN
    current.rgb *= fxSunGround(v_shadow.xyz, v_shadow.w);
#endif
#ifdef FX_FF_PIXEL_LIGHTING
    // The rain's water in the pores (ff_wet.sh): the surface's colour darker, before any highlight is added.
    current.rgb *= ffWet.w;
    tex0.rgb *= ffWet.w;
    // A metal's highlight takes the colour of its texture; the rest is white (the fixed-function material has no
    // usable specular colour).
    vec3 specTint = vec3_splat(1.0);
#ifdef FX_LIGHT_SPECULAR
    specTint = mix(specTint, tex0.rgb / max(max(tex0.r, max(tex0.g, tex0.b)), 0.04), ffMetal);
    // Metal scatters less of its light diffusely: 15 % less at full metal (subtle: the game's textures hold its shading).
    current.rgb *= 1.0 - 0.15 * ffMetal;
#endif
#endif
#ifdef FX_FF_LIT
    // Highlights and the rim light.
    current.rgb = fxAddSpecular(current.rgb, specTint * (fxSunSpecular(N, V, ffSpec) * sunShadow + ffPointSpecular +
                                                         ffLampSpec) + fxRim(N, V));
#ifdef FX_LIGHT_TRANSLUCENT
    // Sails and flags with the sun behind them: its light comes through the canvas (fx_lighting.sh). The visibility
    // is the sun's on the sail's sunlit side, so a sail does not shadow itself; a sail behind another one still gets
    // part of it (canvas passes light on, the shadow map cannot tell canvas from wood).
    if (u_fxLight[2].w > 0.0)
        current.rgb = fxAddSpecular(current.rgb, fxSunTransmission(Ng, V, dot(tex0.rgb, vec3(0.299, 0.587, 0.114))) *
                                                     mix(0.4, 1.0, fxSunShadow(P, -Ng)));
#endif
#endif
#ifdef FX_FF_BAKED
    // How lit the bake shows this texel: its stages' result against the texture alone (with a 2x modulate or a
    // light map alike). The sun's highlight shows where that is the light of the open sun, not in the shade; the
    // relief's sun-side shading and its own shadow likewise: in a shadow the game painted (and in the shade of a
    // wall) no sun lights the relief, so the painted dark stays as deep as the game's.
    float bakedSun = smoothstep(0.55, 0.9, dot(current.rgb, vec3(0.299, 0.587, 0.114)) / (1.0 - 0.15 * ffMetal) /
                                               max(dot(tex0.rgb, vec3(0.299, 0.587, 0.114)), 0.04));
#ifdef FX_LIGHT_NORMALMAP
    current.rgb *= mix(1.0, fxBakedRelief(Ng, N, V), bakedSun);
#ifdef FX_LIGHT_PARALLAX
    // The baked colour holds the sun and the sky: the relief's own shadow takes the sun's part (about half of it).
    current.rgb *= 1.0 - 0.5 * bakedSun * (1.0 - fxParallaxShadow(ffReliefUv, ffPomFound, ffPomFrame.gu, ffPomFrame.gv,
                                                                  ffPomFrame.normal, u_fxLight[0].xyz));
#endif
#endif
#ifdef FX_FF_AO
    if (u_fxAoParams.x > 0.0)
    {
        // The interior's occlusion on a baked surface: only what the bake lacks. The volume's occlusion here relative to
        // the open room before the surface, the brighter of half a metre and a metre and a half out (above a table or
        // a shelf): an open wall sees what the room before it sees and takes nothing (the bake's level holds the
        // room's enclosure); a corner, the floor under a table take their own. Over the darkening the bake already
        // shows against the room's baked light there (ffRoomLight, with the lamps): a corner the game painted dark
        // keeps its paint and takes nothing more; one it painted as bright as the room takes the volume's.
        float aoOpen = max(ffAoVolume(P + Ng * 0.5, Ng, Nw), ffAoVolume(P + Ng * 1.5, Ng, Nw));
        float aoRel = clamp(ffAoVolume(P, Ng, Nw) / max(aoOpen, 1e-3), 0.0, 1.0);
        float held = 1.0;
#ifdef FX_FF_LAMPS
        if (u_fxLampGrid[2].w > 0.5)
            held = clamp(dot(v_color0.rgb, vec3(0.299, 0.587, 0.114)) /
                             max(dot(ffRoomLight(Pw, Nw), vec3(0.299, 0.587, 0.114)), 0.03), 0.0, 1.0);
#endif
        aoVolume = mix(aoRel / max(held, aoRel), 1.0, skySeen.x);
    }
#endif
    // The baked colour holds the sky and the sun together: three quarters of the cavity.
    aoVolume *= mix(1.0, cavity, 0.75);
    // The moving casters' shadows (characters, ships) take the sun's part of the bake, not the lamps' light.
#ifdef FX_LIGHT_SHADOW
    float movingShade = fxSunGroundAt(P, Ng);
#else
    float movingShade = 1.0;
#endif
#ifdef FX_FF_LAMPS
    // The place's lamps relight the baked colour b: b * (b + G * lit) / (b + k * G * unshadowed). Where no lamp
    // reaches, the colour is the bake; the side towards a lamp gets its light, what the lamp cannot see loses the
    // part of that light the bake is taken to hold (k). The AO volume occludes all of it: the room's own look stays.
    float baked = max(dot(v_color0.rgb, vec3(0.299, 0.587, 0.114)), 0.03);
    float gain = u_fxLamps[0].z;
    // The baked colours already carry the tone the game gave its lamp light on these surfaces: the relight adds
    // and takes light in that tone, with only a share of the lamp's own, far more saturated colour
    // (u_fxLampGrid[4].z; with all of it a candle-lit wall goes orange-red where the game's is warm white).
    vec3 lampTone = mix(vec3_splat(dot(lamps.rgb, vec3(0.299, 0.587, 0.114))), lamps.rgb, u_fxLampGrid[4].z);
    // The lamp-free part and the lamps' part add, the latter within the picture's headroom (ffLampRoll).
    vec3 surface = current.rgb * aoVolume;
    // The bake as it is (without the relight). Where it is darker than the share of the lamps it is taken to hold
    // (k G unshadowed), the game painted no lamp light there (a painted shadow, dark paint): the relight may darken it
    // (the lamp's shadow) but not lighten it above the bake.
    vec3 bakeOnly = surface * movingShade;
    float holds;
    if (fxLinear())
    {
        float share = u_fxLamps[0].w * gain * gain * lamps.a;
        holds = smoothstep(0.5, 1.0, baked * baked / max(share, 1e-4));
        surface *= surface / (baked * baked + share);
        vec3 base = surface * (baked * baked * movingShade * movingShade);
        vec3 lamp = surface * (gain * gain * lampTone);
        current.rgb = sqrt(base + lamp * ffLampRoll(base, lamp));
    }
    else
    {
        float share = u_fxLamps[0].w * gain * lamps.a;
        holds = smoothstep(0.5, 1.0, baked / max(share, 1e-4));
        surface /= baked + share;
        vec3 base = surface * (baked * movingShade);
        vec3 lamp = surface * (gain * lampTone);
        current.rgb = base + lamp * ffLampRoll(base, lamp);
    }
    current.rgb = mix(min(current.rgb, bakeOnly), current.rgb, holds);
    // The lamps' highlights on the baked colours, at the lamps' gain on them.
    vec3 ffLampBakedSpec = ffLampSpec * (fxLinear() ? gain * gain : gain);
#else
    // Without the lamps the relight leaves the bake: only the moving casters' shadow takes its sun.
    current.rgb *= aoVolume * movingShade;
    vec3 ffLampBakedSpec = vec3_splat(0.0);
#endif
#ifdef FX_FF_TOWN_WALL
    if (u_fxTownWall[0].w > 0.0)
    {
        // A town's wall (src/effects/town_light.hpp TownWallTone): its baked colour carries the game's sun along the
        // direction the game baked with and the game's sky, which takes no account of alleys and gaps between
        // houses. Relit as the lightmaps are (the town light's ratio, fx/sun_relight.hpp RelightTone): a + b sun + c sky
        // with our sun's shadow (where the sun reaches at all) at the bake's cosine and our sky now, over the game's sun
        // and sky then. A face turned more to the hour's sun than to the bake's gets more light only by the lift.
        float sunThen = max(0.0, dot(N, u_fxTownWall[2].xyz));
        float facingNow = sunThen + max(dot(N, u_fxTownWall[1].xyz) - sunThen, 0.0) * u_fxTownWall[2].w;
        // A face turned away from the hour's sun is in its own shadow (the lightmaps' rays: no sun below 0.02 of it).
        float sunNow = facingNow * fxSunShadow(P, Ng) * sunReach * clamp(dot(Ng, u_fxTownWall[1].xyz) * 50.0, 0.0, 1.0);
        float now = u_fxTownWall[0].x + u_fxTownWall[0].y * sunNow + u_fxTownWall[0].z * skySeen.x;
        float then = u_fxTownWall[0].x + u_fxTownWall[0].y * sunThen + u_fxTownWall[0].z * ffWallSky(skySeen.x);
        current.rgb *= mix(1.0, clamp(now / max(then, 0.02), 0.33, 3.0), u_fxTownWall[0].w);
    }
#endif
    current.rgb = fxLit(current.rgb, tex0.rgb, (ffPointDiffuse + ffLampMoved) * cavityDirect, vec3_splat(0.0),
                        vec3_splat(0.0));
#ifdef FX_LIGHT_SPECULAR
    // Highlights the bake cannot hold: the sun's (outdoors: u_fxLight[1] is black indoors), the lamps' with their
    // shadows (at the lamps' gain on baked colours), the dynamic lights'; with a gloss map, or wet in the rain.
    if (u_fxGloss.w > 0.0 || ffWet.x > 0.0)
        current.rgb = fxAddSpecular(current.rgb, specTint * (fxSunSpecular(N, V, ffSpec) * bakedSun * sunReach + ffLampBakedSpec +
                                                             ffPointSpecular));
#endif
#endif
#if defined(FX_FF_PIXEL_LIGHTING) && defined(FX_LIGHT_SPECULAR)
    if (u_fxGloss.z > 0.5)
    {
        // Debug views: the gloss as grey (magenta: no gloss map), or the relief: how far the normal map tilts the
        // normal, 0.5 grey where it does not (no map, or still fading in).
        if (u_fxGloss.z < 1.5)
            current.rgb = ffGloss >= 0.0 ? vec3_splat(ffGloss) : vec3(0.6, 0.0, 0.6);
        else if (u_fxGloss.z < 2.5)
            current.rgb = clamp(vec3_splat(0.5) + 2.5 * (N - Ng), 0.0, 1.0);
        else if (u_fxGloss.z > 4.5)
            current.rgb = mix(vec3(0.2, 0.6, 0.2), vec3(0.9, 0.2, 0.2), ffSkin) * (0.4 + 0.6 * dot(current.rgb, vec3(0.299, 0.587, 0.114))); // the skin weight: red skin, green relief
        else if (u_fxGloss.z > 3.5)
            current.rgb = mix(vec3_splat(0.15), vec3(0.2, 0.85, 1.0), ffMetal); // the metal mask
        else
        {
#ifdef FX_FF_BAKED
            // Yellow: the open sun the bake shows, times the sun's light; blue: the sun's highlight there.
            float sunLight = bakedSun * clamp(2.0 * dot(u_fxLight[1].rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
            current.rgb = vec3(sunLight, sunLight, clamp(fxSunSpecular(N, V, ffSpec).g * bakedSun, 0.0, 1.0));
#else
            current.rgb = vec3(0.0, 0.0, 0.5); // lit draws: the sun shadow map decides
#endif
        }
    }
#endif
#ifdef FX_FF_PIXEL_LIGHTING
#ifdef FX_FF_WET
    // The overcast sky in the wet surfaces outdoors, on the lit colour.
    current.rgb = ffWetFinish(current.rgb, N, V, mul(u_fxLampView, vec4(reflect(-V, N), 0.0)).y, ffWet);
#endif
#ifdef FX_FF_LAMPS
    if (u_fxLamps[0].x > 0.5 && u_fxLampGrid[0].w > 0.5)
    {
        // Debug views: the lamps' light in the slots' colours over a grey picture, or what the shadows remove.
        float grey = dot(current.rgb, vec3(0.299, 0.587, 0.114));
        if (u_fxLampGrid[0].w < 1.5)
            current.rgb = vec3_splat(0.35 * grey) + lamps.rgb * u_fxLamps[0].z;
        else if (u_fxLampGrid[0].w < 2.5)
            current.rgb = vec3_splat(lamps.a > 1e-4 ? dot(lamps.rgb, vec3(0.299, 0.587, 0.114)) / lamps.a : 0.5);
        else if (u_fxLampGrid[0].w < 3.5)
            current.rgb = vec3_splat(ffLampTaps * 2.0 / 255.0); // two steps of 8 bits per tap
        else
            current.rgb = vec3_splat(ffLampsReached * 16.0 / 255.0); // sixteen steps per lamp
    }
#endif
#ifndef STAGE0_CUBE
    if (u_fxVisibilityParams.x > 1.5)
    {
        // Debug view (fx_visibility_debug): red the sky, green the openness above 15°, over a grey picture.
        current.rgb = vec3(skySeen.x, skySeen.y, 0.15) * (0.35 + 0.65 * dot(current.rgb, vec3(0.299, 0.587, 0.114)));
    }
#endif
#endif
#ifdef FX_LIGHT_PARALLAX
    if (u_fxParallax[1].y > 0.5)
    {
        // Debug view (fx_parallax_debug): red the uv shift in texels of the height's top level (full red at 8), green
        // the relief's height where the eye's ray met it, blue 0.3 on every parallax draw (other draws keep their colour).
        float shifted = length((ffReliefUv - ffReliefUv0) * exp2(u_fxParallax[0].w));
        current.rgb = vec3(clamp(shifted / 8.0, 0.0, 1.0), ffPomFound.x > 0.0 ? 1.0 - ffPomFound.y : 0.0, 0.3);
    }
#endif
    gl_FragColor = ffFinish(current);
    FF_SLOPE_DEPTH();
}
