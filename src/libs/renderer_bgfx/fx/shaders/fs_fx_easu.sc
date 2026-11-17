$input v_uv
// FSR 1 EASU: edge-adaptive upscale of the antialiased internal picture to the display size (fx_fsr.sh). GLES 3.0 has
// no textureGather: each 2x2 gather is four texel fetches (the reference's twelve-tap footprint, shared by r, g, b).
#include "fx_fsr.sh"

uniform vec4 u_fxFsrOut; // xy: output size in pixels
#if FX_DEPTH
// Render scale: the scene's depth into the target's (nearest), for the 3D the game draws after the scene boundary on
// the display-size picture (PostChain::Frame::scene_depth).
SAMPLER2D(s_fxSceneDepth, 1);
#endif

// textureGather(p): the 2x2 texels around the corner p, in the order x (-,+) y (+,+) z (+,-) w (-,-). FsrEasuF gathers
// red, green and blue of each corner in turn, so the red call fetches the four texels and the other two reuse them.
#if BGFX_SHADER_LANGUAGE_GLSL
#define FX_STATIC
#else
#define FX_STATIC static
#endif
FX_STATIC AF4 fx_gather_x, fx_gather_y, fx_gather_z, fx_gather_w;
AF4 FsrEasuRF(AF2 p)
{
    ASU2 c = ASU2(floor(p * u_fxFsr[3].zw - 0.5));
    fx_gather_x = fxFsrTexel(c + ASU2(0, 1));
    fx_gather_y = fxFsrTexel(c + ASU2(1, 1));
    fx_gather_z = fxFsrTexel(c + ASU2(1, 0));
    fx_gather_w = fxFsrTexel(c);
    return AF4(fx_gather_x.r, fx_gather_y.r, fx_gather_z.r, fx_gather_w.r);
}
AF4 FsrEasuGF(AF2 p) { return AF4(fx_gather_x.g, fx_gather_y.g, fx_gather_z.g, fx_gather_w.g); }
AF4 FsrEasuBF(AF2 p) { return AF4(fx_gather_x.b, fx_gather_y.b, fx_gather_z.b, fx_gather_w.b); }
#define FSR_EASU_F 1
#include "ffx_fsr1.h"

void main()
{
    AF3 c;
    FsrEasuF(c, fxFsrPixel(v_uv, u_fxFsrOut.xy), fxFsrCon(0), fxFsrCon(1), fxFsrCon(2), fxFsrCon(3));
    gl_FragColor = vec4(c, 1.0);
#if FX_DEPTH
    gl_FragDepth = texture2D(s_fxSceneDepth, v_uv).r;
#endif
}
