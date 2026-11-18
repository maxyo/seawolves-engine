$input v_wpos, v_misc
// The Kelvin waves of one hull on its ribbon (fx::SeaWakes, include/fx_kelvin.sh): the bow's wave system and,
// opposite in sign, the stern's, band limited to the target's texels, turned from the track's axes into the world's.
// Out: R, G slope in world x, z, B height (encoded around 128/255), A the weight this hull's waves are blended over
// the others' with (its fade, and their envelope: the ribbon's edges leave the other hulls' waves alone).
#include "fx_common.sh"
#include "fx_kelvin.sh"

// [0] x waterline length (m), y half beam (m), z metres per texel, w unused
// [1] x slope range, y height range of the encoding (fx::SeaWakes::kSlope, kHeight), zw unused
uniform vec4 u_wake[2];

float fxWakeEncode(float value, float range)
{
    return clamp(128.0 / 255.0 + value / range * (127.0 / 255.0), 0.0, 1.0);
}

void main()
{
    float along = v_wpos.x;
    float side = v_wpos.y;
    float k0 = v_wpos.z;
    float hull_length = u_wake[0].x;
    // What the target holds (3.5 texels), and what a hull makes: its divergent waves come from the bow and shoulders,
    // those much shorter than the transverse waves cancel along its length; the visible ones run from the cusp
    // (1.5 k0) out to about 60 degrees (4 k0). The transverse waves are lower than the divergent.
    float shortest = max(3.5 * u_wake[0].z, 6.2831853 / (4.0 * k0));
    vec4 bow = fxKelvin(along, side, k0, 0.5 * hull_length, shortest, 0.45, 1.0);
    vec4 stern = fxKelvin(along - hull_length, side, k0, 0.5 * hull_length, shortest, 0.45, 1.0);
    float amplitude = v_misc.z;
    vec3 wave = (bow.xyz - 0.6 * stern.xyz) * amplitude;
    // Track axes (back along the track, to its side) to world x, z.
    vec2 back = v_misc.xy;
    vec2 across = vec2(-back.y, back.x);
    vec2 slope = wave.y * back + wave.z * across;
    float weight = v_misc.w * smoothstep(0.0, 0.15, bow.w);
    gl_FragColor = vec4(fxWakeEncode(slope.x, u_wake[1].x), fxWakeEncode(slope.y, u_wake[1].x),
                        fxWakeEncode(wave.x, u_wake[1].y), weight);
}
