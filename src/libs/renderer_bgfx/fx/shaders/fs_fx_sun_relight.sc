$input v_uv
// One of our relit town lightmaps (fx/sun_relight.hpp), composed where it is sampled from: per texel the game's
// paint times the lighting of the hour's sun over the lighting it was baked with (fx/town_light.hpp:
// a + b sun + c sky). The sun on the texel is read from the place's sun map: four hardware-filtered taps over the
// texel's footprint. The bake's sun on it was read the same way once, from the place map drawn along the bake's
// direction (u_fxRelight[5].z = 1: this pass writes that visibility), so a caster the map cannot resolve (a railing,
// a pole) is missed by both and its painted shadow stays. Alpha: the game's.
// The game's paint is read from its own lightmap at the target's resolution (u_fxRelight[7].x: its level): only the
// lighting's inputs (position, normal, sky, the bake's sun) are interpolated between the texels it was fitted at.
#include "fx_common.sh"

SAMPLER2D(s_fxRelightPosition, 0); // xyz world, w 1 = a texel of the lightmap's triangles (or their gutter)
SAMPLER2D(s_fxRelightNormal, 1);   // xyz the surface normal * 0.5 + 0.5, w the sky's visibility
SAMPLER2D(s_fxRelightBaked, 2);    // r the bake's sun on the texel (this pass with u_fxRelight[5].z = 1)
SAMPLER2D(s_fxRelightGame, 4);     // the game's lightmap texture, as the draws sample it
SAMPLER2DSHADOW(s_fxRelightSun, 5);
uniform mat4 u_fxRelightMtx; // world -> the place map's uv and depth
// [0] the tone a, b, c; w: 1 = debug view (the sun's visibility as grey)
// [1] xyz towards the sun (world), w: 1 = the sun is up
// [2], [3] the game's sky per bin of ours
// [4] x: the footprint's radius, y: the offset off the surface, z: depth bias
// [5] xy: 1 / the target's size, z: 1 = write the sun's visibility (the bake's) instead of the relit paint
// [6] xy: the size of the inputs (the game lightmap's level the lighting was fitted at), zw: 1 / it
// [7] x: the level of the game's lightmap texture with the target's texels
uniform vec4 u_fxRelight[8];

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

void main()
{
    // The target's texel: no dependence on how the backend orients a render target. The target has `scale` times
    // the inputs' texels per side; the inputs are interpolated here (bilinear, over the texels the lightmap's
    // triangles cover: a texel outside them does not pull a position off the surface).
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
    vec4 game = texture2DLod(s_fxRelightGame, uv, u_fxRelight[7].x);
    vec4 position = vec4_splat(0.0);
    vec4 normalSky = vec4_splat(0.0);
    float covered = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec4 p = texture2DLod(s_fxRelightPosition, uvs[i], 0.0);
        float w = bilinear[i] * p.w;
        position += vec4(p.xyz * w, w);
        normalSky += texture2DLod(s_fxRelightNormal, uvs[i], 0.0) * w;
        covered += w;
    }
    if (covered < 1e-4)
    {
        gl_FragColor = game; // outside every triangle: the game's
        return;
    }
    position.xyz /= covered;
    normalSky /= covered;
    vec4 baked = texture2DLod(s_fxRelightBaked, uv, 0.0);
    vec3 n = normalize(normalSky.xyz * 2.0 - vec3_splat(1.0));
    vec3 sun = u_fxRelight[1].xyz;
    float facing = dot(n, sun);
    float visible = 0.0;
    if (u_fxRelight[1].w > 0.5 && facing > 0.02)
    {
        vec3 t = normalize(cross(abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
        vec3 b = cross(n, t);
        vec3 o = position.xyz + n * u_fxRelight[4].y;
        float r = u_fxRelight[4].x;
        // The four directions of fx::SunVisibility's rays over the texel.
        visible = sunTap(o + (t * 0.7071 + b * 0.7071) * r) + sunTap(o + (-t * 0.7071 + b * 0.7071) * r) +
                  sunTap(o + (-t * 0.7071 - b * 0.7071) * r) + sunTap(o + (t * 0.7071 - b * 0.7071) * r);
        visible *= 0.25 * facing;
    }
    if (u_fxRelight[5].z > 0.5)
    {
        gl_FragColor = vec4(visible, visible, visible, 1.0);
        return;
    }
    float k = normalSky.w;
    float a = u_fxRelight[0].x, bs = u_fxRelight[0].y, c = u_fxRelight[0].z;
    float now = a + bs * visible + c * k;
    float then = a + bs * baked.r + c * gameSky(k);
    // The paint under the game's light, lit by ours (a texel the game had in a shadow we do not see, or the other
    // way round, at most 3x brighter / darker).
    float ratio = clamp(now / max(then, 0.02), 0.33, 3.0);
    vec3 color = clamp(game.rgb * ratio, 0.0, 1.0);
    if (u_fxRelight[0].w > 0.5)
        color = vec3_splat(0.125 + 0.75 * visible); // MODULATE2X: 0.25..1.0 of the paint x2
    gl_FragColor = vec4(color, game.a);
}
