$input a_position, a_texcoord0, a_color0
$output v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog
// The coast foam strands of src/libs/island/src/foam.cpp (CoastFoam, technique CoastFoam of
// src/techniques/weather/CoastFoam.fx), drawn from the static buffer CoastFoam::BuildBuffers fills once per foam file.
// What CoastFoam::ExecuteFoamType2 once computed for every vertex of every frame on the CPU is done here: the
// strand's place between the work part's two ends for its move, its alpha, its texture coordinates, and the sea's
// height under it (SEA::WaveXZ: two animated height frames, each sampled bilinearly with wrap).
//
// Vertex (CoastFoam::GpuVertex, 24 bytes):
//   a_position: the work part's ends (v[0].x, v[0].z, v[1].x, v[1].z)
//   a_texcoord0.x: the work part's tu
//   a_color0 (UBYTE4N): x row 0..7 of the strand, y the edge alpha * 4 (0..4), zw the foam's index (low, high byte)
//
// Registers (CoastFoam::Realize):
//   c0..c3   view * projection, as columns
//   c4, c5   wave 1, 2: x the height uv per metre, yz the uv at x = z = 0, w the amplitude
//   c6       x, y the frames of waves 1, 2 (0..64), z the foam's lift above the sea, w the first foam of the draw
//   c7       x, z the camera, z the sea's reach squared (beyond it the sea is flat, as WaveXZ returns 0), w 0
//   c8 + i   foam (c6.w + i)'s strand of this draw: x move, y alpha, z texture offset, w 1 when drawn
//
// s_vol1 (stage 1): the sea's 64 height frames, four to a texel (frame 4 s + c in slice s, channel c), 128 x 128 x 16.
#define PORT_VSC_COUNT 72 // c0..c71 (CoastFoam kFoamsPerDraw = 64)
#include "port_vs.sh"

SAMPLER3D(s_vol1, 9);

// One frame of the sea's heights at uv, 0..1 as SEA::CalculateHeightMap scales the bytes (1 / 255).
float seaFrame(vec2 uv, float frame)
{
    float slice = floor(frame / 4.0);
    vec4 texel = texture3DLod(s_vol1, vec3(uv, (slice + 0.5) / 16.0), 0.0);
    float channel = frame - slice * 4.0;
    return dot(texel, vec4(equal(vec4_splat(channel), vec4(0.0, 1.0, 2.0, 3.0))));
}

// SEA::CalculateHeightMap: the frame blended with the next one (frames wrap), the blend linear like the bilinear
// sample, so blending two bilinear samples is the same surface.
float seaHeight(vec2 uv, float frame)
{
    float first = floor(frame);
    float next = first + 1.0 >= 64.0 ? 0.0 : first + 1.0;
    return mix(seaFrame(uv, first), seaFrame(uv, next), frame - first);
}

void main()
{
    float foam = floor(a_color0.z * 255.0 + 0.5) + floor(a_color0.w * 255.0 + 0.5) * 256.0;
    vec4 strand = u_vsc[8 + int(foam - u_vsc[6].w)];
    float row = floor(a_color0.x * 255.0 + 0.5);
    float dy = row / 7.0;
    vec4 oPos = vec4(0.0, 0.0, 2.0, 1.0); // a strand not drawn: every corner here, no area
    vec4 oD0 = vec4_splat(0.0);
    vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0);
    if (strand.w > 0.5)
    {
        vec2 xz = a_position.xy + (strand.x * dy) * (a_position.zw - a_position.xy);
        float height = 0.0;
        vec2 d = xz - u_vsc[7].xy;
        if (dot(d, d) <= u_vsc[7].z)
            height = u_vsc[4].w * seaHeight(xz * u_vsc[4].x + u_vsc[4].yz, u_vsc[6].x) +
                     u_vsc[5].w * seaHeight(xz * u_vsc[5].x + u_vsc[5].yz, u_vsc[6].y);
        vec3 pos = vec3(xz.x, u_vsc[6].z + height, xz.y);
        oPos = u_vsc[0] * pos.x + u_vsc[1] * pos.y + u_vsc[2] * pos.z + u_vsc[3];
        float edge = floor(a_color0.y * 255.0 + 0.5) * 0.25;
        oD0 = vec4(1.0, 1.0, 1.0, strand.y * clamp(2.5 * (1.0 - dy) * dy, 0.0, 1.0) * edge);
        oT0.xy = vec2(strand.z + a_texcoord0.x, 1.0 - dy);
    }
    vec4 none = vec4(0.0, 0.0, 0.0, 1.0);
    PORT_VS_OUTPUT(oPos, oD0, vec4_splat(0.0), oT0, none, none, none, none, 1.0);
}
