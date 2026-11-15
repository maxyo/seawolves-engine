#ifndef STORM_SEA_OPTICS_SH
#define STORM_SEA_OPTICS_SH
// The sea port uses private D3D matrices, not bgfx built-in u_view.
float seaViewRayCos(vec3 ray, vec3 forward)
{
    return dot(normalize(ray), normalize(forward));
}

// Beer-Lambert transmission of the scene behind the water. A negative gap is
// foreground/MSAA coverage, not submerged material; preserve it exactly.
float seaOpticalTransmission(float viewGap, float viewRayCos, float extinction)
{
    if (viewGap <= 0.0 || extinction <= 0.0) return 1.0;
    float distance = viewGap / max(abs(viewRayCos), 0.05);
    return exp(-min(distance * extinction, 80.0));
}
#endif
