$input v_uv
// The surface of fx::SeaWaves for the sea shaders (shaders/include/sea_waves.sh), sampled with linear filtering:
// R height (m), GB slope in x / z, A crest foam (0..1). RGBA16F as they are; RGBA8 as height
// signed-sqrt height +-0.5 m and linear slopes +-1 around 128/255. Fades to flat water at the border.
#include "fx_waves.sh"
#include "fx_waves_surface_storage.sh"

void main()
{
    float texel = u_wavesStep[1].x;
    float h = fxWavesState(v_uv).x;
    float l = fxWavesState(v_uv - vec2(texel, 0.0)).x;
    float r = fxWavesState(v_uv + vec2(texel, 0.0)).x;
    float d = fxWavesState(v_uv - vec2(0.0, texel)).x;
    float u = fxWavesState(v_uv + vec2(0.0, texel)).x;
    vec2 slope = vec2(r - l, u - d) / (2.0 * u_wavesStep[1].y);
    // Foam where a crest is high and sharp (it would break), from the curvature of the field.
    float curvature = (4.0 * h - l - r - d - u) / (u_wavesStep[1].y * u_wavesStep[1].y);
    // Only a thin crest where the bow wave breaks: high and sharp, at half strength (the wake's foam is the ship's
    // own trail behind the stern).
    float foam = 0.5 * clamp((h - 0.15) * 6.0, 0.0, 1.0) * clamp(curvature, 0.0, 1.0);
    vec2 edge = min(v_uv, 1.0 - v_uv);
    float fade = smoothstep(0.02, 0.15, min(edge.x, edge.y));
    if (u_wavesStep[3].y > 0.5)
        // Zero is 128/255, exact in 8 bits (0.5 is not: a flat field would decode to a slight tilt everywhere).
        gl_FragColor = vec4(fxWavesEncodeSurfaceHeight(h * fade),
                            clamp(slope * fade * 0.5 + 128.0 / 255.0, 0.0, 1.0), foam * fade);
    else
        gl_FragColor = vec4(h * fade, slope * fade, foam * fade);
}
