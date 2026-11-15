// Ray intersection uses the triangle's plane, not its interpolated lighting normal. A smoothed corner normal can
// face away while the triangle is visible, incorrectly suppressing all parallax at the grazing-angle gate.
// Keep this derivative-only helper shared with the CPU geometry checks; no lighting normal or strength is changed.
struct FxParallaxFrame
{
    vec3 gu;
    vec3 gv;
    vec3 normal;
};

FxParallaxFrame fxParallaxFrame(vec3 dpx, vec3 dpy, vec2 dux, vec2 duy, vec3 V)
{
    vec3 face = cross(dpx, dpy);
    float length2 = dot(face, face);
    if (!(length2 > 1e-30))
        return FxParallaxFrame(vec3_splat(0.0), vec3_splat(0.0), vec3_splat(0.0));
    face *= inversesqrt(length2);
    if (dot(face, V) < 0.0) face = -face;
    vec3 px = cross(dpy, face);
    vec3 py = cross(face, dpx);
    float inv = 1.0 / dot(dpx, px);
    return FxParallaxFrame((px * dux[0] + py * duy[0]) * inv,
                           (px * dux[1] + py * duy[1]) * inv, face);
}
