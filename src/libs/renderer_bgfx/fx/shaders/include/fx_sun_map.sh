// The sun's shadow maps (fx/shadows.hpp): one D16 array on sampler stage 15 (fx::kShadowMapStage), layer 0 the
// place's still geometry (drawn when it changes), layer 1 the moving casters (every frame). One projection: the
// dynamic layer's uv is the static one's scaled and shifted. Included by fx_lighting.sh (FX_LIGHT_SHADOW: lit
// receivers read both layers) and by the fixed-function ground variant (fs_ff_dyn: the moving casters only).
#ifndef FX_SUN_MAP_SH
#define FX_SUN_MAP_SH

SAMPLER2DARRAYSHADOW(s_fxShadowMap, 15);
uniform mat4 u_fxShadowMtx; // view space -> the static layer's uv and depth
// [0] x: one texel in uv, y: shadow strength, z: the static layer's normal offset in view units, w: its depth bias
// [1] x, yz: the dynamic layer's uv = static uv * x + yz, w: its depth bias for receivers that cast into it
// [2] x: the dynamic layer's normal offset, y: the static layer holds casters, z: a moving caster's shadow can
//     reach this draw, w: facing scale (the light-space depth of a unit normal times it: 1 facing the sun)
// [3] ground receivers. x: how far over the surface (in depth) the place's own shade must start to count,
//     y: the surface's light without the sun over its light with it, z: depth step of the dynamic layer,
//     w: 1 = this draw is a ground receiver (baked variants)
uniform vec4 u_fxShadow[4];

// Shadows fade out over the last tenth of the dynamic layer's square (the shadowed region) and of the depth range.
float fxSunInside(vec2 uvDynamic, float depth)
{
    vec2 edge = abs(uvDynamic - vec2_splat(0.5)) * 2.0;
    return clamp((1.0 - max(max(edge.x, edge.y), depth)) * 10.0, 0.0, 1.0);
}

// A surface that holds its own static light (a lightmap, baked colours) under the moving casters: 1 = untouched.
// `c`: its static layer uv and depth (no offset: it is not in the dynamic layer, so it cannot shadow itself);
// `facing`: 0 turned away from the sun .. 1. The shadow takes the sun's share of the surface's light
// (u_fxShadow[3].y: what is left), and only where the static layer does not have the surface in the place's shade
// already: a character's shadow does not fall through a house, nor darken a shaded street twice.
float fxSunGround(vec3 c, float facing)
{
    // The receiver's plane in the map: the static tap's bias follows its slope (no normal needed).
    vec3 dx = dFdx(c);
    vec3 dy = dFdy(c);
    if (u_fxShadow[2].z < 0.5)
        return 1.0;
    vec2 uvDynamic = c.xy * u_fxShadow[1].x + u_fxShadow[1].yz;
    float dark = 1.0 - shadow2DArray(s_fxShadowMap, vec4(uvDynamic, 1.0, c.z - u_fxShadow[3].z));
    dark *= fxSunInside(uvDynamic, c.z) * u_fxShadow[0].y * facing;
    // (Read for every pixel of the draw, not only where the moving layer shades: a compare tap inside a branch on
    // a per-pixel value has no defined derivatives; on NVIDIA's Vulkan it read "shaded" there.)
    if (u_fxShadow[2].y > 0.5)
    {
        float det = dx.x * dy.y - dx.y * dy.x;
        vec2 slope = abs(det) > 1e-12 ? vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : vec2_splat(0.0);
        // Shade from what stands over the surface (houses, trees, awnings), not from what lies on it: the still
        // geometry has ground covers, planks and paths a few centimetres over one another.
        float bias = min((abs(slope.x) + abs(slope.y)) * 1.5 * u_fxShadow[0].x, 8.0 * u_fxShadow[0].w) + u_fxShadow[3].x;
        dark *= shadow2DArray(s_fxShadowMap, vec4(c.xy, 0.0, c.z - bias));
    }
    return 1.0 - dark * (1.0 - u_fxShadow[3].y);
}

#endif
