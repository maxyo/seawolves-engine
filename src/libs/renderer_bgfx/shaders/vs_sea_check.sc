$input a_position, a_texcoord0
$output v_texcoord0
// The sea's GPU surface self-check (src/shim/sea_surface_check.cpp): the sea ports' surface code (sea_surface.sh) for
// one point of the sea's own height field and registers, its result written to the point's pixel of a float target.
// a_position.xz: the point (metres from the mesh's origin); a_texcoord0.xy: the corner's clip position (a thin
// triangle over the point's pixel).
#define PORT_VSC_COUNT 9 // registers c0..c8: the surface's c4..c8 (sea_surface_gpu.hpp)
#include "port_vs.sh"
#include "sea_surface.sh"

void main()
{
    vec4 pos;
    vec3 normal;
    vec2 uv;
    seaSurface(a_position.xz, pos, normal, uv);
    gl_Position = vec4(a_texcoord0.xy, 0.5, 1.0);
    v_texcoord0 = vec4(pos.y, normal);
}
