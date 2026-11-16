$input v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog, v_normal, v_position, v_matDiffuse, v_matAmbient
// Geometry with baked vertex colours (lighting off) under the per-pixel lighting presets: dynamic point lights,
// normal-map relief, the AO volume and the location's lamps with their shadows (src/effects/lighting.cpp). Compiled
// only as the baked families' variants (renderer_bgfx/CMakeLists.txt: product blocks FX_FF_*, FX_LIGHT_SPECULAR the
// gloss highlights of the sun, the lamps and the dynamic lights, FX_LIGHT_NORMALMAP / FX_LIGHT_PARALLAX, FX_LIGHT_SHADOW).
// The inputs are those of vs_ff_lit (bgfx links only programs whose varyings match exactly).
#define FX_FF_BAKED 1
#define FX_FF_COVERAGE 1
#define FX_LIGHT_POINT 1
#define FX_LIGHT_POINT_MAX 8
#include "ff_fragment.sh"
