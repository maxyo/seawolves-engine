// RGBA8 surface height: signed square-root codes retain small ripples without reducing the +-0.5 m range.
// This non-linear channel must be decoded at texel centres BEFORE interpolation. GB slopes and A foam stay linear.
#ifndef FX_WAVES_SURFACE_STORAGE_SH
#define FX_WAVES_SURFACE_STORAGE_SH
float fxWavesEncodeSurfaceHeight(float height)
{
    return (sign(height) * sqrt(clamp(abs(height) * 2.0f, 0.0f, 1.0f)) * 127.0f + 128.0f) / 255.0f;
}

float fxWavesDecodeSurfaceHeight(float channel)
{
    // Inputs are point samples. Recover the exact byte before subtraction: UNORM conversion and fused
    // multiply-add must not turn the zero code into a nonzero height.
    float value = (floor(channel * 255.0f + 0.5f) - 128.0f) / 127.0f;
    return value * abs(value) * 0.5f;
}
#endif
