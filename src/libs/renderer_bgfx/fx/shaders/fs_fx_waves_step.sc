$input v_uv
// One step of fx::SeaWaves: the discrete 2D wave equation (explicit leapfrog of the classic ripple scheme) with
// velocity damping, a sponge along the border and this frame's sources.
#include "fx_waves.sh"

#define kWavesSources 16
#define kWavesVectors 8 // fx::SeaWaves::kSourceVectors
// Per source: [0] xy position (metres from the centre), zw forward; [1] x half length, y half width (metres; a hull:
// half its waterline length, its largest half beam), z speed (m/s); [2] x kind (0 body, 1 hull),
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

    // Bodies push the water (momentum forcing); a hull holds the water inside its waterline still, so that the
    // ripples reflect off it. A hull's own waves are its wake (fx::SeaWakes): a field of one wave speed cannot make
    // the dispersive Kelvin pattern, and pushing it with a moving hull made a Mach cone, two straight fronts sliding
    // apart.
    vec2 p = (v_uv - 0.5) * u_wavesStep[2].w;
    float body_force = 0.0;
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
            // A body wading or swimming at speed U pushes the water up ahead of it and draws it down behind (a
            // dipole along its way, as strong as the drag, U^2: no water is added or taken away), and swimming
            // strokes at about 0.9 Hz. A walk (1.6 m/s) makes ripples of about 2 cm at a metre.
            vec2 q = vec2(along / b.x, side / b.y);
            float e = dot(q, q);
            if (e > 4.0)
                continue;
            float g = exp(-2.5 * e);
            body_force += g * (0.0625 * b.z * abs(b.z) * q.x - 0.06 * k.y * sin(5.65 * time + k.z));
        }
        else
        {
            if (abs(along) > b.x * 1.6 + 8.0 || abs(side) > b.y * 2.0 + 8.0)
                continue;
            float dist = fxWavesHull(i, along, side);
            float skin = max(2.0 * u_wavesStep[1].y, 0.25 * b.y);
            hold *= smoothstep(-1.2 * skin, -0.2 * skin, dist);
        }
    }
    h += body_force * u_wavesStep[3].x;
    h *= hold;
    // Sponge: the outer 8 % of the field absorbs what reaches it.
    vec2 edge = min(v_uv, 1.0 - v_uv);
    h *= mix(pow(0.94, dt * 60.0), 1.0, smoothstep(0.0, 0.08, min(edge.x, edge.y)));
    h = clamp(h, -0.95 * kWavesRange, 0.95 * kWavesRange);
    // Preserve hold/sponge/clamp effects in the velocity, before height storage rounds it.
    float base = c.x * hold;
    float history = u_wavesStep[2].z > 0.5 ? (h - base) * u_wavesStep[3].w : base;
    // Idle fade scales position AND velocity/history: no artificial impulse and no hidden state on reentry.
    gl_FragColor = fxWavesOut(vec2(h, history) * u_wavesStep[3].y);
}
