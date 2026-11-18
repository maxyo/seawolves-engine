// The ripple state of fx::SeaWaves: RG32F holds current/previous heights; RGBA8 holds a 16-bit current height
// and an independent 16-bit reference-step displacement. Read with point sampling at texel centres only.
#ifndef FX_WAVES_SH
#define FX_WAVES_SH
#include "fx_common.sh"
#include "fx_waves_storage.sh"

#define kWavesRange 2.0

SAMPLER2D(s_wavesState, 0);
// [0] xy: uv offset of the previous state (the centre moved), z: (c dt / dx)^2 (dt + dt_last) / (2 dt),
//     w: velocity kept over this step times dt / dt_last (the steps are unequal: WaveClock)
// [1] x: 1 / size, y: metres per texel, z: dt, w: sources
// [2] x: time (s), y: 1 = write still water, z: 1 = packed RGBA8 state, w: metres the field covers
// [3] x: body momentum forcing integrated over this and the preceding step
//     y: step pass: stored-state retention; surface pass: 1 = RGBA8 surface
//     z: velocity kept times dt / reference_dt; w: reference_dt / dt (packed state only)
uniform vec4 u_wavesStep[4];

float fxWavesCode(vec2 hl)
{
    return dot(floor(hl * 255.0 + 0.5), vec2(256.0, 1.0));
}

vec2 fxWavesBytes(float code)
{
    float hi = floor(code / 256.0);
    return vec2(hi, code - hi * 256.0) / 255.0;
}

// (height, previous height) for float; (height, reference-step displacement) for packed. Zero outside the field.
vec2 fxWavesState(vec2 uv)
{
    vec4 t = texture2D(s_wavesState, uv);
    vec2 h = u_wavesStep[2].z > 0.5 ?
        vec2(fxWavesDecodeHeight(fxWavesCode(t.rg), kWavesRange), fxWavesDecodeVelocity(fxWavesCode(t.ba))) : t.rg;
    vec2 inside = step(vec2_splat(0.0), uv) * step(uv, vec2_splat(1.0));
    return h * (inside.x * inside.y);
}

vec4 fxWavesOut(vec2 h)
{
    return u_wavesStep[2].z > 0.5 ?
        vec4(fxWavesBytes(fxWavesEncodeHeight(h.x, kWavesRange)), fxWavesBytes(fxWavesEncodeVelocity(h.y))) : vec4(h, 0.0, 1.0);
}
#endif
