$input a_position, a_normal, a_texcoord0
$output v_wpos, v_misc
// The ribbon along a hull's track in the wake target of fx::SeaWakes (wake_path.hpp: WakeVertex): a_position xy clip,
// z how much of the waves is left; a_normal: metres behind the bow along the track, metres to the side, g / U^2;
// a_texcoord0: the unit vector back along the track (world x, z), the waves' height.
#include "fx_common.sh"

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_wpos = a_normal;
    v_misc = vec4(a_texcoord0, a_position.z);
}
