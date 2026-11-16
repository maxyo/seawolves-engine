$input a_position, a_normal, a_color0, a_color1, a_texcoord0, a_texcoord1, a_texcoord2, a_texcoord3
$output v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog, v_shadow
// vs_ff_unlit for a draw that receives the moving casters' sun shadows (fs_ff_dyn, src/effects/lighting.cpp).
#define FF_UNLIT 1
#define FX_FF_DYN 1
#include "ff_vertex.sh"
