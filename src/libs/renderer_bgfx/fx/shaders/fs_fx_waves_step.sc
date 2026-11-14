$input v_uv
// One step of fx::SeaWaves: the discrete 2D wave equation (explicit leapfrog of the classic ripple scheme) with
// velocity damping, a sponge along the border and this frame's sources.
#include "fx_waves.sh"

#define kWavesSources 16
#define kWavesVectors 8 // fx::SeaWaves::kSourceVectors
// Per source: [0] xy position (metres from the centre), zw forward; [1] x half length, y half width (metres; a hull:
// half its waterline length, its largest half beam), z speed (m/s), w heave (m/s up); [2] x kind (0 body, 1 hull),
// y stroke (0..1), z phase (radians); a hull's waterline: [3] x stern, y stem (metres along forward), [4..5] the half
// beam and [6..7] the centre's offset to the side at 8 stations evenly spaced from stern to stem.
uniform vec4 u_wavesSrc[kWavesSources * kWavesVectors];

// Signed distance (metres, negative inside) from (along, side) to the waterline of hull source `i`.
float fxWavesHull(int i, float along, float side)
{
    vec4 ends = u_wavesSrc[i * kWavesVectors + 3];
    float s = clamp((along - ends.x) / max(ends.y - ends.x, 0.1), 0.0, 1.0) * 7.0;
    // Linear between the stations: hat weights over the eight of them.
    vec4 w0 = max(1.0 - abs(vec4(s, s, s, s) - vec4(0.0, 1.0, 2.0, 3.0)), 0.0);
    vec4 w1 = max(1.0 - abs(vec4(s, s, s, s) - vec4(4.0, 5.0, 6.0, 7.0)), 0.0);
    float half_beam = dot(w0, u_wavesSrc[i * kWavesVectors + 4]) + dot(w1, u_wavesSrc[i * kWavesVectors + 5]);
    float centre = dot(w0, u_wavesSrc[i * kWavesVectors + 6]) + dot(w1, u_wavesSrc[i * kWavesVectors + 7]);
    float lateral = abs(side - centre) - half_beam;
    float beyond = max(ends.x - along, along - ends.y);
    return beyond > 0.0 ? length(vec2(beyond, max(lateral, 0.0))) : lateral;
}

void main()
{
    if (u_wavesStep[2].y > 0.5)
    {
        gl_FragColor = fxWavesOut(vec2_splat(0.0));
        return;
    }
    float texel = u_wavesStep[1].x;
    float dt = u_wavesStep[1].z;
    vec2 uv = v_uv + u_wavesStep[0].xy;
    vec2 c = fxWavesState(uv);
    float lap = fxWavesState(uv - vec2(texel, 0.0)).x + fxWavesState(uv + vec2(texel, 0.0)).x +
                fxWavesState(uv - vec2(0.0, texel)).x + fxWavesState(uv + vec2(0.0, texel)).x - 4.0 * c.x;
    // Packed heights must not be differenced: each rounding error would become persistent velocity.
    float displacement = u_wavesStep[2].z > 0.5 ? c.y * u_wavesStep[3].z : (c.x - c.y) * u_wavesStep[0].w;
    float h = c.x + displacement + u_wavesStep[0].z * lap;

    // Sources: displacement rates calibrated at 60 Hz, and the still water inside the hulls. The source term
    // uses the same second-order time integration as the Laplacian, scaled to retain that reference calibration.
    vec2 p = (v_uv - 0.5) * u_wavesStep[2].w;
    float rate = 0.0;
    float hold = 1.0;
    float time = u_wavesStep[2].x;
    for (int i = 0; i < kWavesSources; ++i)
    {
        if (float(i) >= u_wavesStep[1].w)
            break;
        vec4 a = u_wavesSrc[i * kWavesVectors];
        vec4 b = u_wavesSrc[i * kWavesVectors + 1];
        vec4 k = u_wavesSrc[i * kWavesVectors + 2];
        vec2 d = p - a.xy;
        float along = dot(d, a.zw);
        float side = dot(d, vec2(-a.w, a.z));
        if (k.x < 0.5)
        {
            // A body: pushes down where it moves and, swimming, strokes at about 0.9 Hz.
            vec2 q = vec2(along / b.x, side / b.y);
            float e = dot(q, q);
            if (e > 4.0)
                continue;
            float g = exp(-2.5 * e);
            rate -= g * (0.10 * abs(b.z) + 0.30 * k.y * sin(5.65 * time + k.z));
        }
        else
        {
            // A hull: the water inside its waterline stays still (incoming waves reflect off it); along the
            // waterline, in a band as wide as a quarter of the half beam, it is pushed by the hull's speed, up at
            // the stem and down along the run aft, and by its heave.
            if (abs(along) > b.x * 1.6 + 8.0 || abs(side) > b.y * 2.0 + 8.0)
                continue;
            float dist = fxWavesHull(i, along, side);
            float skin = max(2.0 * u_wavesStep[1].y, 0.25 * b.y);
            if (dist > 3.0 * skin)
                continue;
            hold *= smoothstep(-1.2 * skin, -0.2 * skin, dist);
            float ring = exp(-(dist / skin) * (dist / skin));
            vec4 ends = u_wavesSrc[i * kWavesVectors + 3];
            float bow = clamp((along - ends.x) / max(ends.y - ends.x, 0.1), 0.0, 1.0);
            // How much wave a hull makes for its speed grows with its Froude number U / sqrt(g L): a heavy ship
            // under sail (Fr about 0.2) pushes a modest bow wave, a light boat at the same speed (Fr 0.4 and more)
            // a full one.
            float froude = abs(b.z) * inversesqrt(9.81 * max(2.0 * b.x, 1.0));
            float make = clamp(2.5 * froude, 0.25, 1.0);
            // Heave only from the hull's own bobbing beyond the swell's (a storm lifts every hull metres a second):
            // a small push, clamped, or every hull rings like a bell.
            rate += ring * (abs(b.z) * make * (0.09 * bow * bow * bow - 0.018) - 0.03 * clamp(b.w, -1.0, 1.0));
        }
    }
    h += rate * u_wavesStep[3].x;
    h *= hold;
    // Sponge: the outer 8 % of the field absorbs what reaches it.
    vec2 edge = min(v_uv, 1.0 - v_uv);
    h *= mix(pow(0.94, dt * 60.0), 1.0, smoothstep(0.0, 0.08, min(edge.x, edge.y)));
    h = clamp(h, -0.95 * kWavesRange, 0.95 * kWavesRange);
    // Preserve hold/sponge/clamp effects in the velocity, before height storage rounds it.
    float history = u_wavesStep[2].z > 0.5 ? (h - c.x * hold) * u_wavesStep[3].w : c.x * hold;
    // Idle fade scales position AND velocity/history: no artificial impulse and no hidden state on reentry.
    gl_FragColor = fxWavesOut(vec2(h, history) * u_wavesStep[3].y);
}
