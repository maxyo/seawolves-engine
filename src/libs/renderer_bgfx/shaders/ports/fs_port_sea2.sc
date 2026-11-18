$input v_color0, v_color1, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_fog, v_foam
// Port of the technique shader sea_sea2_pixel_shader (ps_1_1) of src/techniques/weather/sea.fx.
// Stage 0: animated bump volume texture, stage 1: the game's foam texture (weather\\sea\\pena\\pena.tga; read only by
// the waves from objects), stage 3: environment cube map.
//
// On top of the original (effects/sea.hpp sets u_seaFx; all off reproduces the D3D9 shader except the first point):
// - the reflected ray never points below the horizon: the cube's -Y face is never rendered above water, and the
//   original sampled its black there (dark specks near the horizon); a ray reflected downwards meets the sea again,
//   whose colour at the horizon the side faces hold;
// - band limit: the bump detail fades towards the flat surface as one pixel covers more texels of the bump
//   texture (far and grazing water), so the reflection does not shimmer at the horizon;
// - soft edges: the water fades out where it is shallower than the edge depth in front of the scene behind it
//   (s_seaDepth, the scene's view distance per pixel packed into RGB8 by fs_fx_sea_depth). Where that distance is
//   nearer than the water itself, the pixel belongs to an object in front of the sea (its edge: a rope, a sail, a
//   hull, partly covering a multisampled pixel) and the water is not faded: fading it there showed what lies under
//   the sea as bright fringes along every edge over the water;
// - specular: the sun's glint from the bumped normal: the same reflected ray as the game's sun road, so it lies on it;
// - waves from objects (sea_waves.sh): ripples around swimmers and waders and the Kelvin wakes of the hulls tilt the
//   normal; the reflection follows it, and so does the Fresnel split between the mirrored sky (v0) and the water's
//   own colour and transparency (v1, alpha), recomputed per pixel with the vertex shader's formula for the tilted and
//   the untilted normal: where nothing stirs the water the original look stays exactly. Foam on the ripples' crests
//   is the game's foam texture, thresholded by the foam amount (optional).
#include "port_ps.sh"
#include "sea_waves.sh"
#include "sea_optics.sh"
SAMPLER3D(s_vol0, 8);
SAMPLER2D(s_tex1, 1);
SAMPLERCUBE(s_cube2, 7); // the game's sun road cube (sea.cpp, stage 2): the glint's colour
SAMPLERCUBE(s_cube3, 11);
SAMPLER2D(s_seaDepth, 14);
// [0] x: band limit, y: soft edges, z: specular (each 0 or 1), w: far plane of the packed scene distance
// [1] xyz: towards the sun (world), w: specular power   [2] x: specular strength, w: 1 / edge depth (metres)
// [3] rgb: current game water colour, w: extinction per metre (0 disables turbidity).
// [4] xyz: actual shim camera forward (row-vector view third column).
uniform vec4 u_seaFx[5];

void main()
{
    vec4 v0 = v_color0;
    vec4 v1 = v_color1;
    vec4 r0 = vec4_splat(0.0);

    vec4 t0 = texture3D(s_vol0, v_texcoord0.xyz);                           // tex t0
    vec3 n = PS_BX2(t0.rgb);                                                //   t0_bx2
    // Texels of the bump texture's top level under one pixel: 1 close up, many at the horizon.
    vec2 footprint = fwidth(v_texcoord0.xy) * 128.0;
    float detail = 1.0 - u_seaFx[0].x * clamp(log2(max(max(footprint.x, footprint.y), 1.0)) * 0.3, 0.0, 0.85);
    n = mix(vec3(0.0, 1.0, 0.0), n, detail);
    float m1 = dot(v_texcoord1.xyz, n);                                     // texm3x3pad t1, t0_bx2
    float m2 = dot(v_texcoord2.xyz, n);                                     // texm3x3pad t2, t0_bx2
    float m3 = dot(v_texcoord3.xyz, n);                                     // texm3x3vspec t3, t0_bx2
    vec3 eye = vec3(v_texcoord1.w, v_texcoord2.w, v_texcoord3.w);           //   eye vector from the w components
    float foam;
    vec3 debug;
    // The foam texture in world metres (the sea's vertex origin moves with the camera; u_seaWaves[3] holds its uv).
    vec2 foam_uv = v_texcoord4.zw * u_seaWaves[3].z + u_seaWaves[3].xy;
    vec3 n0 = normalize(vec3(m1, m2, m3));
    vec2 wake = seaWakeSlope(v_texcoord4.zw, length(fwidth(v_texcoord4.zw)));
    vec2 slope = seaWavesSlope(v_texcoord4, length(fwidth(v_texcoord4.zw)), foam, debug) + wake;
    debug.r += length(wake) * 4.0;
    vec3 N = seaWavesTilt(n0, slope);
    vec3 R = portReflect(N, eye);
    // The waves' tilt changes how much sky the water mirrors: the vertex shader's Fresnel split, per pixel, for the
    // tilted normal over that for the sea's own.
    vec3 e = normalize(eye);
    float f0 = seaFresnel(dot(e, n0), u_seaWaves[2].z);
    float f1 = seaFresnel(dot(e, N), u_seaWaves[2].z);
    float water = (1.0 - f1) / max(1.0 - f0, 1e-3);
    v0.rgb *= f1 / max(f0, 1e-3);
    v0.a *= water;
    v1.rgb *= water;
    R.y = max(R.y, 0.0);
    vec4 t3 = textureCube(s_cube3, R);
    r0.rgb = PS11(v0.rgb * t3.rgb + v1.rgb);                                // mad r0.xyz, v0, t3, v1
    r0.a = v0.a;                                                            // mov r0.w, v0.wwww
    if (u_seaFx[0].z > 0.5)
    {
        // The sun's glint: the same reflected ray as the game's sun road, so it lies on it, in the colour of the sun
        // the game draws into that road's cube (warm at dawn and dusk, none where the game shows no sun).
        vec3 sun = textureCube(s_cube2, u_seaFx[1].xyz).rgb;
        r0.rgb += pow(max(dot(normalize(R), u_seaFx[1].xyz), 0.0), u_seaFx[1].w) * u_seaFx[2].x * sun;
    }
    float gap = -1.0;
    if (u_seaFx[0].y > 0.5 || u_seaFx[3].w > 0.0)
    {
        vec3 packed = texture2D(s_seaDepth, gl_FragCoord.xy * u_viewTexel.xy).rgb;
        float behind = dot(packed, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0)) * u_seaFx[0].w;
        gap = behind - v_fog.z;
    }
    if (u_seaFx[3].w > 0.0 && gap >= 0.0 && u_seaWaves[2].x < 0.5)
    {
        // View-z distance to ray distance; neither surface normal nor wave strength changes.
        float rayCos = seaViewRayCos(e, u_seaFx[4].xyz);
        float transmission = seaOpticalTransmission(gap, rayCos, u_seaFx[3].w);
        // Sea2 blends source + destination*alpha. Only the transmitted fraction
        // becomes water-colour scattering; reflection/Fresnel remain as calculated.
        float scattered = r0.a * (1.0 - transmission);
        r0.rgb += clamp(u_seaFx[3].rgb, 0.0, 1.0) * scattered;
        r0.a *= transmission;
    }
    if (foam > 0.0)
    {
        // The foam texture's bright streaks (its luminance: 0.15 dark water .. 0.75 foam), more of them the more foam:
        // thin lace where a crest just breaks, a white sheet in the churned water behind a hull.
        vec3 lace = texture2D(s_tex1, foam_uv).rgb;
        float streak = clamp((dot(lace, vec3(0.3, 0.5, 0.2)) - 0.15) / 0.6, 0.0, 1.0);
        float cover = smoothstep(-0.1, 0.5, streak - (1.0 - 1.15 * foam)) * min(foam * 2.5, 0.95);
        // Lit like the water: the sky straight up, over the sea's own colour; the streaks' own shading on it.
        vec3 sky = textureCube(s_cube3, vec3(0.0, 1.0, 0.0)).rgb;
        vec3 white = min(vec3_splat(dot(sky, vec3(0.3, 0.5, 0.2)) * 1.1) + sky * 0.15 + v1.rgb, vec3_splat(1.0));
        white *= 0.8 + 0.25 * streak;
        r0.rgb = mix(r0.rgb, white, cover);
        r0.a *= 1.0 - cover;
    }

    if (u_seaWaves[2].x > 1.5)
        r0 = vec4((N.x - n0.x) * 2.0 + 0.5, 0.5, (N.z - n0.z) * 2.0 + 0.5, 0.0); // the tilt the waves add
    else if (u_seaWaves[2].x > 0.5)
        r0 = vec4(min(debug, vec3_splat(1.0)), 0.0);
    gl_FragColor = ffFinish(clamp(r0, 0.0, 1.0));
    if (u_seaFx[0].y > 0.5)
    {
        // Blended as Sea2: source + destination * alpha; fading to the scene keeps destination * 1.
        float edge = gap < 0.0 ? 1.0 : clamp(gap * u_seaFx[2].w, 0.0, 1.0);
        gl_FragColor.rgb *= edge;
        gl_FragColor.a = 1.0 - edge * (1.0 - gl_FragColor.a);
    }

    // The game's two layers over the sea, drawn by its technique as two more passes over the whole sea and folded in
    // here: both add to what this draw leaves (Sea2_Foam: source * source alpha + destination; Sea2_SunRoad:
    // source + destination), so their colours add to this draw's source, which the destination * alpha keeps. Each is
    // saturated as its own pass's output was; the sum saturates in the target as the passes' blends did. Not faded by
    // the soft edges (they never were). The target's alpha is no longer their sum: nothing reads the scene's alpha.
    if (u_psc[0].x > 0.5)
    {
        // Sea2_Foam (sea_sea2foam_pixel_shader): the foam texture (stage 1 here) at the vertex shader's uv, the
        // layer's alpha from the height (v_foam), fogged as this draw (both techniques leave the fog to the state).
        vec4 lace = ffFinish(clamp(vec4(texture2D(s_tex1, v_foam.xy).rgb, v_foam.z), 0.0, 1.0));
        gl_FragColor.rgb += lace.rgb * lace.a;
    }
    if (u_seaWaves[2].x < 0.5)
    {
        // Sea2_SunRoad (sea_sea2sunroad_pixel_shader, no fog): the sun road cube (stage 2 here) in the ray reflected
        // about the unfiltered bump normal (no band limit) with the ripples.
        vec3 bump = PS_BX2(t0.rgb);
        vec3 road = seaWavesTilt(vec3(dot(v_texcoord1.xyz, bump), dot(v_texcoord2.xyz, bump), dot(v_texcoord3.xyz, bump)),
                                 slope);
        gl_FragColor.rgb += clamp(textureCube(s_cube2, portReflect(road, eye)).rgb, 0.0, 1.0);
    }
}
