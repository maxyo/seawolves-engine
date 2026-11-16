$input v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog, v_shadow
// fs_ff_dyn with alpha to coverage instead of the alpha-test discard (fs_ff_coverage's twin for the surfaces under
// moving casters: the game draws its location geometry with the alpha test on).
#define FX_FF_DYN 1
#define FX_FF_COVERAGE 1
#include "ff_fragment.sh"
