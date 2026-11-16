// The sea's surface in its vertex ports (vs_port_sea2, vs_port_sea2foam, vs_port_sea2sunroad, vs_port_sea3): the
// position, normal and bump texture coordinates the CPU once computed for every vertex of every frame, from the flat
// grid the sea uploads when its blocks change (src/libs/sea/src/sea.cpp) and its height field
// (src/libs/sea/include/sea_surface_gpu.hpp: the layout of the texels and registers). Every step is SEA::WaveXZ's:
// two waves, each two frames blended texel by texel and sampled bilinearly with wrap, the normal of each texel from
// its frame's heights, quantised as SEA::BuildVolumeTexture stores it.
#ifndef SEA_SURFACE_SH
#define SEA_SURFACE_SH

// The height field (sea_gpu::pack_field): 128 x 128 x 64 frames, RGBA8, read texel by texel. D3DVERTEXTEXTURESAMPLER0.
SAMPLER3D(s_vtex0, 13);

// The texel's frame-blended height (x, 0..1: the bytes / 255) and normal (yz: x and z), blended as
// SEA::FrameBlend blends them for WaveXZ.
vec3 seaFieldTexel(ivec2 xy, int frame1, int frame2, float blend)
{
    vec4 a = floor(texelFetch(s_vtex0, ivec3(xy, frame1), 0) * 255.0 + 0.5);
    vec4 b = floor(texelFetch(s_vtex0, ivec3(xy, frame2), 0) * 255.0 + 0.5);
    // SEA::BuildVolumeTexture: normalize(2 (left - right), 4, 2 (top - bottom)) of the heights / 255, its x and z
    // stored as shorts (truncated) of x 32767.5.
    vec2 da = a.gb + vec2(mod(a.a, 2.0), floor(a.a / 2.0)) * 256.0 - 255.0;
    vec2 db = b.gb + vec2(mod(b.a, 2.0), floor(b.a / 2.0)) * 256.0 - 255.0;
    vec3 na = normalize(vec3(da.x * (2.0 / 255.0), 4.0, da.y * (2.0 / 255.0)));
    vec3 nb = normalize(vec3(db.x * (2.0 / 255.0), 4.0, db.y * (2.0 / 255.0)));
    vec2 qa = trunc(na.xz * 32767.5) / 32767.5;
    vec2 qb = trunc(nb.xz * 32767.5) / 32767.5;
    float ha = a.r / 255.0, hb = b.r / 255.0;
    return vec3(ha + (hb - ha) * blend, qa + (qb - qa) * blend);
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
    vec3 a1 = seaFieldTexel(i0, frame1, frame2, blend);
    vec3 a2 = seaFieldTexel(ivec2(i1.x, i0.y), frame1, frame2, blend);
    vec3 a3 = seaFieldTexel(ivec2(i0.x, i1.y), frame1, frame2, blend);
    vec3 a4 = seaFieldTexel(i1, frame1, frame2, blend);
    vec3 s = a1 + f.x * (a2 - a1) + f.y * (a3 - a1) + f.x * f.y * (a4 + a1 - a2 - a3);
    return vec3(s.x * wave.w, s.yz);
}

// The vertex at the mesh position `local` (metres from the mesh's origin, x and z): `pos` as the sea's vertex
// position (world minus the sea's vertex offset), `normal` (unnormalised, as the CPU stored it) and `uv` the bump
// texture coordinates.
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

#endif
