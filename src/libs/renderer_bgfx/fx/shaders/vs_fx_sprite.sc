$input a_position, a_normal, a_texcoord0
$output v_wpos, v_misc
// Procedural sprites: lens flare elements (clip-space quads). a_normal carries the colour,
// a_texcoord0 = (u, v in -1..1, shape).
#include "fx_common.sh"

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_wpos = a_normal;
    v_misc = vec4(a_texcoord0, 0.0);
}
