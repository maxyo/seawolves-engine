// Waves from objects on the Sea2 passes (effects::Host::prepare_sea sets everything here):
// - ripples: the surface of fx::SeaWaves (fx/include/storm/fx/sea_waves.hpp) around the camera (R height, GB slope in
//   x / z, A crest foam; RGBA16F as they are, or RGBA8 around 128/255), in the normals, the Fresnel, the crest foam
//   and (lift) the vertices. The pixel shaders read it as a cubic B-spline (four linear taps): the slopes, and with
//   them the reflection, are smooth across the 0.5 m texels instead of showing their bilinear facets.
// - wakes: the target of fx::SeaWakes (fx/include/storm/fx/sea_wakes.hpp) around the camera, the Kelvin waves behind
//   every hull under way (R, G slope in x / z, B height, around 128/255), in the normals and (lift) the vertices.
//   Its waves are at least 3.5 texels long: they fade out where a texel gets smaller than half a pixel (no shimmer).
// Positions are those of the sea's vertices (a_position: world minus the sea's own camera offset, small numbers).
#ifndef SEA_WAVES_SH
#define SEA_WAVES_SH
#include "fx_waves_surface_storage.sh"

SAMPLER2D(s_seaWaves, 15); // unit of fx::kShadowMapStage: never bound on a Sea2 draw
SAMPLER2D(s_seaWake, 12);  // the shim's stage-3 volume unit: Sea2's stage 3 is a cube (unit 11)
// [0] xy: field uv at the sea's vertex origin, z: 1 / metres the field covers (uv = pos.xz * z + xy), w: lift (0 off)
// [1] x: ripple normals (0 off, else their strength), y: crest foam strength, z: 0, w: metres per field texel
// [2] x: debug view (fx_sea_waves_debug), y: 1 = RGBA8 surface, z: the sea's relative refractive index (its
//     vertex Fresnel's Sea2.Frenel), w: field texels per side
// [3] xy: foam texture uv at the sea's vertex origin, z: foam texture repeats per metre, w: 0
uniform vec4 u_seaWaves[4];
// [0] xy: wake target uv at the sea's vertex origin, z: 1 / metres it covers, w: 1 = wakes drawn this frame
// [1] x: lift (0 off), y: slope gain, z: 0, w: texels per side
uniform vec4 u_seaWake[2];

#define kSeaWakeSlope 0.5  // fx::SeaWakes::kSlope
#define kSeaWakeHeight 1.0 // fx::SeaWakes::kHeight

vec2 seaWakeUv(vec2 pos)
{
    return pos * u_seaWake[0].z + u_seaWake[0].xy;
}

float seaWakeDecode(float channel, float range)
{
    return (channel - 128.0 / 255.0) * (255.0 / 127.0) * range;
}

vec2 seaWavesUv(vec2 pos)
{
    return pos * u_seaWaves[0].z + u_seaWaves[0].xy;
}

float seaWavesInside(vec2 uv)
{
    vec2 inside = step(vec2_splat(0.0), uv) * step(uv, vec2_splat(1.0));
    return inside.x * inside.y;
}

// A surface texel as metres of height (x), slope (yz) and foam (w).
vec4 seaWavesDecode(vec4 s)
{
    if (u_seaWaves[2].y > 0.5)
        // Fragment consumers use only the linear slopes/foam; height is decoded separately in seaWavesLift.
        return vec4(0.0, (s.gb - 128.0 / 255.0) * 2.0, s.a);
    return s;
}

#if BGFX_SHADER_TYPE_VERTEX
// Metres the ripples raise the sea's vertex at `pos`.
float seaWavesLift(vec2 pos)
{
    if (u_seaWaves[0].w <= 0.0)
        return 0.0;
    vec2 uv = seaWavesUv(pos);
    float height;
    if (u_seaWaves[2].y > 0.5)
    {
        // The fallback's companded height cannot be filtered in encoded space. Sample its four texel centres;
        // slopes and foam in the pixel shaders remain linear and retain the existing four-tap cubic filter.
        float size = u_seaWaves[2].w;
        vec2 st = uv * size - 0.5;
        vec2 base = (floor(st) + 0.5) / size;
        vec2 f = fract(st);
        float a = fxWavesDecodeSurfaceHeight(texture2DLod(s_seaWaves, base, 0.0).r);
        float b = fxWavesDecodeSurfaceHeight(texture2DLod(s_seaWaves, base + vec2(1.0 / size, 0.0), 0.0).r);
        float c = fxWavesDecodeSurfaceHeight(texture2DLod(s_seaWaves, base + vec2(0.0, 1.0 / size), 0.0).r);
        float d = fxWavesDecodeSurfaceHeight(texture2DLod(s_seaWaves, base + vec2_splat(1.0 / size), 0.0).r);
        height = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }
    else
        height = texture2DLod(s_seaWaves, uv, 0.0).r;
    return height * seaWavesInside(uv) * u_seaWaves[0].w;
}

// Metres the wakes raise the sea's vertex at `pos`.
float seaWakeLift(vec2 pos)
{
    if (u_seaWake[0].w <= 0.0 || u_seaWake[1].x <= 0.0)
        return 0.0;
    vec2 uv = seaWakeUv(pos);
    return seaWakeDecode(texture2DLod(s_seaWake, uv, 0.0).b, kSeaWakeHeight) * seaWavesInside(uv);
}
#endif

#if BGFX_SHADER_TYPE_FRAGMENT
// The surface at `uv` filtered as a uniform cubic B-spline (C2: smooth slopes, no bilinear facets or terraces), from
// four linear taps placed between the texels by the weights (Sigg and Hadwiger, GPU Gems 2, ch. 20).
vec4 seaWavesCubic(vec2 uv)
{
    float size = u_seaWaves[2].w;
    vec2 st = uv * size - 0.5;
    vec2 i = floor(st);
    vec2 f = st - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 p0 = (i - 0.5 + w1 / g0) / size; // the texel centres of i - 1, i blended by w0 : w1
    vec2 p1 = (i + 1.5 + w3 / g1) / size; // those of i + 1, i + 2 by w2 : w3
    vec4 a = texture2D(s_seaWaves, vec2(p0.x, p0.y)) * g0.x + texture2D(s_seaWaves, vec2(p1.x, p0.y)) * g1.x;
    vec4 b = texture2D(s_seaWaves, vec2(p0.x, p1.y)) * g0.x + texture2D(s_seaWaves, vec2(p1.x, p1.y)) * g1.x;
    return a * g0.y + b * g1.y;
}

// The vertex shader's Fresnel term (sea_sea2_vertex_shader: unpolarised Fresnel reflectance for the cosine `c`
// between the eye and the normal and the relative refractive index `eta`), for the pixel's own normal.
float seaFresnel(float c, float eta)
{
    c = clamp(c, 1e-3, 1.0);
    float g = sqrt(max(eta * eta * (c * c - 1.0) + 1.0, 0.0));
    float s = (c - eta * g) / (c + eta * g);
    float p = (eta * c - g) / (eta * c + g);
    return min(0.5 * (s * s + p * p), 1.0);
}

// The ripples at the field uv t4.xy of the Sea2 pixel: their slope in the sea's tangent plane; `foam`: crest foam there
// (0..1). `pixel`: metres per pixel (the ripples fade out where a field texel gets smaller than a pixel).
vec2 seaWavesSlope(vec4 t4, float pixel, out float foam, out vec3 debug)
{
    vec2 slope = vec2_splat(0.0);
    foam = 0.0;
    if (u_seaWaves[1].x > 0.0)
    {
        float keep = seaWavesInside(t4.xy) * (1.0 - smoothstep(1.0, 4.0, pixel / u_seaWaves[1].w));
        if (keep > 0.0)
        {
            vec4 s = seaWavesDecode(seaWavesCubic(t4.xy));
            slope = s.yz * keep * u_seaWaves[1].x;
            foam = min(s.w * keep * u_seaWaves[1].y, 1.0);
        }
    }
    debug = vec3(length(slope) * 4.0, 0.0, foam);
    return slope;
}

// The wakes' slope at `pos` (the sea's vertex position: world minus its vertex origin), in x / z; `pixel`: metres
// per pixel there.
vec2 seaWakeSlope(vec2 pos, float pixel)
{
    if (u_seaWake[0].w <= 0.0)
        return vec2_splat(0.0);
    vec2 uv = seaWakeUv(pos);
    float keep = seaWavesInside(uv) * (1.0 - smoothstep(1.0, 2.0, pixel * u_seaWake[0].z * u_seaWake[1].w));
    if (keep <= 0.0)
        return vec2_splat(0.0);
    vec2 encoded = texture2DLod(s_seaWake, uv, 0.0).rg;
    return vec2(seaWakeDecode(encoded.x, kSeaWakeSlope), seaWakeDecode(encoded.y, kSeaWakeSlope)) *
           (keep * u_seaWake[1].y);
}

// The world normal `n` (the sea's own, bumped) tilted by the ripples' `slope` (seaWavesSlope).
vec3 seaWavesTilt(vec3 n, vec2 slope)
{
    n = normalize(n);
    // The height field's normal (-sx, 1, -sz) on the sea's own: its slope added in the sea's tangent plane.
    return normalize(n + vec3(-slope.x, 0.0, -slope.y) * n.y);
}
#endif

#endif
