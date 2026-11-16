$input v_uv
// One of our relit town lightmaps (fx/sun_relight.hpp), composed where it is sampled from: per texel the game's
// paint times the lighting of the hour's sun over the lighting it was baked with (fx/town_light.hpp:
// a + b sun + c sky). The sun on the texel is read from the place's sun map: four hardware-filtered taps over the
// texel's footprint. The bake's sun on it was read the same way once, from the place map drawn along the bake's
// direction (u_fxRelight[5].z = 1: this pass writes that visibility), so a caster the map cannot resolve (a railing,
// a pole) is missed by both and its painted shadow stays. Alpha: the game's.
#include "fx_common.sh"

SAMPLER2D(s_fxRelightPosition, 0); // xyz world, w 1 = a texel of the lightmap's triangles (or their gutter)
SAMPLER2D(s_fxRelightNormal, 1);   // xyz the surface normal * 0.5 + 0.5, w the sky's visibility
SAMPLER2D(s_fxRelightBaked, 2);    // r the bake's sun on the texel (this pass with u_fxRelight[5].z = 1)
SAMPLER2D(s_fxRelightGame, 4);     // the game's lightmap
SAMPLER2DSHADOW(s_fxRelightSun, 5);
uniform mat4 u_fxRelightMtx; // world -> the place map's uv and depth
// [0] the tone a, b, c; w: 1 = debug view (the sun's visibility as grey)
// [1] xyz towards the sun (world), w: 1 = the sun is up
// [2], [3] the game's sky per bin of ours
// [4] x: the footprint's radius, y: the offset off the surface, z: depth bias
// [5] xy: 1 / the lightmap's size, z: 1 = write the sun's visibility (the bake's) instead of the relit paint
uniform vec4 u_fxRelight[6];

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
    // The target's texel is the lightmap's: no dependence on how the backend orients a render target.
    vec2 uv = gl_FragCoord.xy * u_fxRelight[5].xy;
    vec4 game = texture2DLod(s_fxRelightGame, uv, 0.0);
    vec4 position = texture2DLod(s_fxRelightPosition, uv, 0.0);
    if (position.w < 0.5)
    {
        gl_FragColor = game; // outside every triangle: the game's
        return;
    }
    vec4 normalSky = texture2DLod(s_fxRelightNormal, uv, 0.0);
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
