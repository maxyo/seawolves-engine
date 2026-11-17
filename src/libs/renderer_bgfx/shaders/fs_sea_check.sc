$input v_texcoord0
// The sea's GPU surface self-check (vs_sea_check.sc): the point's height and normal as computed in its vertices.
#include "storm_common.sh"

void main()
{
    gl_FragColor = v_texcoord0;
}
