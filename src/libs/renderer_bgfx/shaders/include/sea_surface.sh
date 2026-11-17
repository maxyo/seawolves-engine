// The sea's surface in its vertex ports (vs_port_sea2, vs_port_sea3) and in
// the sea's self-check (vs_sea_check.sc): the position, normal and bump texture coordinates the CPU computed for every
// vertex of every frame, from the flat grid the sea uploads when its blocks change (src/libs/sea/src/sea.cpp) and its
// height field (src/libs/sea/include/sea_surface_gpu.hpp: the layout of the texels and registers). Every step is
// SEA::WaveXZ's: two waves, each two frames blended texel by texel and sampled bilinearly with wrap.
// c8.w = 0: the CPU displaced the vertices itself (SEA's fallback when the self-check fails); they are used as given.
#ifndef SEA_SURFACE_SH
#define SEA_SURFACE_SH

// The height field: a 1024 x 1024 RGBA32F atlas of the 64 frames (8 x 8 tiles of 128 x 128), read texel by texel at
// the vertex texture unit, as the skinned draws read their bone palettes (ff_skin.sh). D3DVERTEXTEXTURESAMPLER0.
SAMPLER2D(s_vtex, 13);

// Texel xy of `frame`: x the height (0..1), yz the normal's x and z.
vec3 seaFieldTexel(ivec2 xy, int frame)
{
    int row = frame / 8;
    ivec2 tile = ivec2(frame - row * 8, row) * 128;
    return texelFetch(s_vtex, tile + xy, 0).xyz;
}

// Texel xy blended between two frames, as SEA::FrameBlend blends them for WaveXZ.
vec3 seaBlendedTexel(ivec2 xy, int frame1, int frame2, float blend)
{
    vec3 a = seaFieldTexel(xy, frame1);
    vec3 b = seaFieldTexel(xy, frame2);
    return a + (b - a) * blend;
}

// One wave at the mesh position `local`: x its height in metres, yz the bilinear normal (SEA::WaveXZ nX, nZ).
vec3 seaWave(vec2 local, vec4 wave, float frame)
{
    vec2 t = local * wave.x + wave.yz;
    vec2 i = floor(t);
    vec2 f = t - i;
    ivec2 i0 = ivec2(mod(i, 128.0));
    ivec2 i1 = ivec2(mod(i + 1.0, 128.0));
    float first = floor(frame);
    int frame1 = int(mod(first, 64.0));
    int frame2 = int(mod(first + 1.0, 64.0));
    float blend = frame - first;
    vec3 a1 = seaBlendedTexel(i0, frame1, frame2, blend);
    vec3 a2 = seaBlendedTexel(ivec2(i1.x, i0.y), frame1, frame2, blend);
    vec3 a3 = seaBlendedTexel(ivec2(i0.x, i1.y), frame1, frame2, blend);
    vec3 a4 = seaBlendedTexel(i1, frame1, frame2, blend);
    vec3 s = a1 + f.x * (a2 - a1) + f.y * (a3 - a1) + f.x * f.y * (a4 + a1 - a2 - a3);
    return vec3(s.x * wave.w, s.yz);
}

// The surface at the mesh position `local` (metres from the mesh's origin, x and z): `pos` as the sea's vertex
// position (world minus the sea's vertex offset), `normal` and `uv` the bump texture coordinates.
void seaSurface(vec2 local, out vec4 pos, out vec3 normal, out vec2 uv)
{
    vec2 toCamera = local - u_vsc[8].xy;
    float height = 0.0;
    normal = vec3(0.0, 1.0, 0.0);
    if (dot(toCamera, toCamera) <= u_vsc[8].z)
    {
        vec3 w1 = seaWave(local, u_vsc[4], u_vsc[6].x);
        vec3 w2 = seaWave(local, u_vsc[5], u_vsc[6].y);
        height = w1.x + w2.x;
        float y = sqrt(1.0 - dot(w1.yz, w1.yz)) + sqrt(1.0 - dot(w2.yz, w2.yz));
        vec2 xz = (u_vsc[4].x * u_vsc[4].w) * w1.yz + (u_vsc[5].x * u_vsc[5].w) * w2.yz;
        normal = normalize(vec3(xz.x, y, xz.y));
    }
    uv = (local - normal.xz * 5.3) * u_vsc[6].w + u_vsc[7].zw;
    vec2 xz = local - normal.xz * u_vsc[6].z + u_vsc[7].xy;
    pos = vec4(xz.x, height, xz.y, 1.0);
}

// The port's vertex registers v0 (position), v1 (normal) and v2 (texture coordinates) from its stage inputs (which only
// main() can name, storm_common.sh): displaced here from the flat grid, or as the CPU wrote them (c8.w = 0).
void seaVertex(vec4 position, vec3 inNormal, vec4 texcoord, out vec4 v0, out vec4 v1, out vec4 v2)
{
    if (u_vsc[8].w != 0.0)
    {
        vec3 normal;
        vec2 uv;
        seaSurface(position.xz, v0, normal, uv);
        v1 = vec4(normal, 1.0);
        v2 = vec4(uv, 0.0, 1.0);
    }
    else
    {
        v0 = position;
        v1 = vec4(inNormal, 1.0);
        v2 = texcoord;
    }
}

#endif
