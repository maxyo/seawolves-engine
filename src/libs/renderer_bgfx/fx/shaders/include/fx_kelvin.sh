// Kelvin ship waves (fx::SeaWakes): the stationary-phase solution for a disturbance moving steadily over deep water.
// Scalar arithmetic only: shared with the CPU check (tests/wake_check.cpp).
//
// In the disturbance's frame, x metres behind it along its track and y to its side, the waves it left stand still:
// inside the wedge |y| / x < 1 / sqrt(8) (19.47 degrees) two systems reach each point, the transverse waves (crests
// across the track, the longest: 2 pi U^2 / g) and the divergent waves (crests nearly along the track, ever shorter
// towards it). A system whose waves travel at angle theta to the track has the wavenumber k0 / cos^2 theta
// (k0 = g / U^2), and those reaching (x, y) satisfy tan(alpha) = tan(theta) / (1 + 2 tan^2 theta), alpha the
// point's angle off the track. The phase is k0 (x - |y| tan theta) / cos theta (stationary in theta exactly there);
// its gradient, the wave vector, is k0 sec theta (1, -tan theta). Both systems spread as r^-1/2 and merge on the cusp lines, where the waves are highest
// (r^-1/3, the Airy cusp); outside the wedge they die away within a few wavelengths.
#ifndef FX_KELVIN_SH
#define FX_KELVIN_SH

#define kKelvinCuspTan 0.35355339 // tan(19.47 degrees) = 1 / sqrt(8)

// The waves at (x, y) for unit amplitude: x height, y dh/dx, z dh/dy, w their envelope (about 1 near the source,
// falling off with distance; 0 outside the wedge). `k0` = g / U^2 (1/m); `r0` metres: the size of the disturbance,
// where the spreading starts; `shortest` metres: shorter waves are left out (the band limit of the target);
// `transverse`, `divergent`: the weights of the two systems.
vec4 fxKelvin(float x, float y, float k0, float r0, float shortest, float transverse, float divergent)
{
    if (x <= 0.0)
        return vec4(0.0, 0.0, 0.0, 0.0);
    float ay = abs(y);
    float side = y < 0.0 ? -1.0 : 1.0;
    float t = max(ay / x, 1e-4);
    float D = 1.0 - 8.0 * t * t;
    float q = sqrt(max(D, 0.0));
    float kr = k0 * sqrt(x * x + y * y);
    float spread = sqrt((1.0 + k0 * r0) / (1.0 + kr));
    // Where the two systems merge, the stationary-phase amplitude D^-1/4 gives way to the Airy cusp's (k r)^1/6.
    float near = pow(1.0 + kr, -0.6666667);
    float inside = D >= 0.0 ? 1.0 : 0.0;
    float a = spread * pow(max(D, near), -0.25) * (inside + (1.0 - inside) * exp(-0.6 * (1.0 + kr) * pow(max(-D, 0.0), 1.5)));
    // The two roots of tan(alpha) = tan(theta) / (1 + 2 tan^2 theta); outside the wedge both are the cusp's.
    float tanT = inside > 0.5 ? 2.0 * t / (1.0 + q) : 0.70710678;
    float tanD = inside > 0.5 ? (1.0 + q) / (4.0 * t) : 0.70710678;
    // The stationary-phase offsets (+-pi/4) close to zero on the cusp, where the systems merge.
    float offset = 0.78539816 * min(q * 3.0, 1.0);
    float kmax = 6.2831853 / shortest;

    float secT = sqrt(1.0 + tanT * tanT);
    float wT = a * transverse * (1.0 - smoothstep(0.5 * kmax, kmax, k0 * secT * secT));
    float phT = k0 * (x - ay * tanT) * secT + offset;
    float secD = sqrt(1.0 + tanD * tanD);
    float wD = a * divergent * (1.0 - smoothstep(0.5 * kmax, kmax, k0 * secD * secD));
    float phD = k0 * (x - ay * tanD) * secD - offset;

    float sT = -wT * sin(phT) * k0 * secT;
    float sD = -wD * sin(phD) * k0 * secD;
    return vec4(wT * cos(phT) + wD * cos(phD), sT + sD, -(sT * tanT + sD * tanD) * side, wT + wD);
}

#endif
