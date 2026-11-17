$input v_uv
// One of the towns' light ratio maps (fx/sun_relight.hpp): per texel the lighting of the hour's sun over the lighting
// the game's lightmap was baked with (fx/town_light.hpp: a + b sun + c sky), log-encoded (encodeRatio); the
// lightmapped draws multiply the game's own lightmap by it (ff_fragment.sh ffTownRatio). The sun on the texel is read
// from the place's sun map: four hardware-filtered taps over the texel's footprint. The bake's sun on it was read the
// same way once, from the place map drawn along the bake's direction (u_fxRelight[5].z = 1: this pass writes that
// visibility), so a caster the map cannot resolve (a railing, a pole) is missed by both and its painted shadow stays.
// The inputs (position, normal, sky) are at the texels the lighting was fitted at, interpolated here.
#include "fx_common.sh"

SAMPLER2D(s_fxRelightPositionHi, 0); // xyz the position's high bytes within the bounds, w the sky's visibility
SAMPLER2D(s_fxRelightPositionLo, 1); // xyz its low bytes, w 1 = a texel of the lightmap's triangles (or their gutter)
SAMPLER2D(s_fxRelightBaked, 2);      // r the bake's sun on the texel (this pass with u_fxRelight[5].z = 1)
SAMPLER2D(s_fxRelightNormal, 3);     // xy the unit normal on the octahedron, z the game lightmap's luminance
SAMPLER2DSHADOW(s_fxRelightSun, 5);
uniform mat4 u_fxRelightMtx; // world -> the place map's uv and depth
// [0] the tone a, b, c; w: 1 = debug view (the hour's sun visibility instead of the ratio)
// [1] xyz towards the sun (world), w: 1 = the sun is up
// [2], [3] the game's sky per bin of ours
// [4] x: the footprint's radius, y: the offset off the surface, z: depth bias
// [5] xy: 1 / the target's size, z: 1 = write the sun's visibility (the bake's) instead of the ratio
// [6] xy: the size of the inputs (the game lightmap's level the lighting was fitted at), zw: 1 / it
// [7] xyz: the positions' bounds, low corner; [8] xyz: their extent
// [9] xyz: towards the sun the game baked with, w: how far the light may rise above the game's (fx::RelightLift)
uniform vec4 u_fxRelight[10];

float gameSky(float k)
{
    float x = clamp(k * 8.0 - 0.5, 0.0, 7.0);
    float i = min(floor(x), 6.0);
    int at = int(i);
    float lo = at < 4 ? u_fxRelight[2][at] : u_fxRelight[3][at - 4];
    float hi = at + 1 < 4 ? u_fxRelight[2][at + 1] : u_fxRelight[3][at - 3];
    return max(k, mix(lo, hi, x - i));
}

float sunTap(vec3 world)
{
    vec3 c = mul(u_fxRelightMtx, vec4(world, 1.0)).xyz;
    return shadow2D(s_fxRelightSun, vec3(c.xy, c.z - u_fxRelight[4].z));
}

vec3 octDecode(vec2 e)
{
    vec2 f = e * 2.0 - vec2_splat(1.0);
    float y = 1.0 - abs(f.x) - abs(f.y);
    if (y < 0.0)
        f = (vec2_splat(1.0) - abs(f.yx)) * vec2(f.x >= 0.0 ? 1.0 : -1.0, f.y >= 0.0 ? 1.0 : -1.0);
    return normalize(vec3(f.x, y, f.y));
}

// The ratio map's texel for `ratio` (fx/sun_relight.hpp: 3^((255 e - 127) / 127)).
float encodeRatio(float ratio)
{
    return (127.0 + 127.0 * log2(ratio) / 1.5849625) / 255.0;
}

void main()
{
    // The target's texel: no dependence on how the backend orients a render target. The inputs are interpolated here
    // (bilinear, over the texels the lightmap's triangles cover: a texel outside them does not pull a position off
    // the surface).
    vec2 uv = gl_FragCoord.xy * u_fxRelight[5].xy;
    vec2 at = uv * u_fxRelight[6].xy - vec2_splat(0.5);
    vec2 base = floor(at);
    vec2 f = at - base;
    vec2 uv00 = clamp((base + vec2_splat(0.5)) * u_fxRelight[6].zw, vec2_splat(0.0), vec2_splat(1.0));
    vec2 uv11 = clamp((base + vec2_splat(1.5)) * u_fxRelight[6].zw, vec2_splat(0.0), vec2_splat(1.0));
    vec2 uvs[4];
    uvs[0] = uv00;
    uvs[1] = vec2(uv11.x, uv00.y);
    uvs[2] = vec2(uv00.x, uv11.y);
    uvs[3] = uv11;
    float bilinear[4];
    bilinear[0] = (1.0 - f.x) * (1.0 - f.y);
    bilinear[1] = f.x * (1.0 - f.y);
    bilinear[2] = (1.0 - f.x) * f.y;
    bilinear[3] = f.x * f.y;
    vec3 position = vec3_splat(0.0);
    vec3 normal = vec3_splat(0.0);
    float k = 0.0;
    float game = 0.0;
    float covered = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec4 hi = texture2DLod(s_fxRelightPositionHi, uvs[i], 0.0);
        vec4 lo = texture2DLod(s_fxRelightPositionLo, uvs[i], 0.0);
        float w = bilinear[i] * lo.w;
        vec3 q = floor(hi.xyz * 255.0 + vec3_splat(0.5)) * 256.0 + floor(lo.xyz * 255.0 + vec3_splat(0.5));
        position += (u_fxRelight[7].xyz + u_fxRelight[8].xyz * (q / 65535.0)) * w;
        vec4 ng = texture2DLod(s_fxRelightNormal, uvs[i], 0.0);
        normal += octDecode(ng.xy) * w;
        game += ng.z * w;
        k += hi.w * w;
        covered += w;
    }
    if (covered < 1e-4)
    {
        // Outside every triangle: the game's light.
        gl_FragColor = vec4_splat(u_fxRelight[5].z > 0.5 ? 0.0 : 127.0 / 255.0);
        return;
    }
    position /= covered;
    k /= covered;
    game /= covered;
    vec3 n = normalize(normal);
    vec4 baked = texture2DLod(s_fxRelightBaked, uv, 0.0);
    vec3 sun = u_fxRelight[1].xyz;
    float facing = dot(n, sun);
    float visible = 0.0;
    if (u_fxRelight[1].w > 0.5 && facing > 0.02)
    {
        vec3 t = normalize(cross(abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
        vec3 b = cross(n, t);
        vec3 o = position + n * u_fxRelight[4].y;
        float r = u_fxRelight[4].x;
        // The four directions of fx::SunVisibility's rays over the texel.
        visible = sunTap(o + (t * 0.7071 + b * 0.7071) * r) + sunTap(o + (-t * 0.7071 + b * 0.7071) * r) +
                  sunTap(o + (-t * 0.7071 - b * 0.7071) * r) + sunTap(o + (t * 0.7071 - b * 0.7071) * r);
        visible *= 0.25;
    }
    if (u_fxRelight[5].z > 0.5 || u_fxRelight[0].w > 0.5)
    {
        gl_FragColor = vec4_splat(visible * max(facing, 0.0)); // the bake's sun (its cosine with it), or the debug view's
        return;
    }
    // fx/sun_relight.hpp RelightTone: the sun term keeps the bake's cosine on the texel (the paint's lit level at every
    // hour), only its shadow follows the hour's sun. Where the hour's sun reaches more than the bake's did (a shadow
    // that moved away, a face turned to the low sun), the light rises by the lift only, and only as far as the game's
    // paint is as dark as our bake's shadow says: a painted shadow our shadows do not explain stays.
    float a = u_fxRelight[0].x, bs = u_fxRelight[0].y, c = u_fxRelight[0].z;
    float lift = u_fxRelight[9].w;
    float facingThen = max(dot(n, u_fxRelight[9].xyz), 0.0);
    float shadowThen = facingThen > 0.02 ? clamp(baked.r / facingThen, 0.0, 1.0) : 0.0;
    float shade = a + c * gameSky(k);
    float lit = shade + bs * facingThen;
    float explained = lit - shade < 0.02 ? 1.0 : clamp((0.5 * (shade + lit) - game) / (0.5 * (lit - shade)), 0.0, 1.0);
    float shadowNow = visible <= shadowThen ? visible : shadowThen + (visible - shadowThen) * lift * explained;
    float facingNow = facingThen + max(facing - facingThen, 0.0) * lift;
    float now = a + bs * shadowNow * facingNow + c * k;
    float then = shade + bs * baked.r;
    // A texel the game had in a shadow we do not see, or the other way round: at most 3x brighter / darker.
    gl_FragColor = vec4_splat(encodeRatio(clamp(now / max(then, 0.02), 1.0 / 3.0, 3.0)));
}
