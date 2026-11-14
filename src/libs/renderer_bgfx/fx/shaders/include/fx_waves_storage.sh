// Scalar arithmetic shared with the CPU storage regression. All 16-bit codes fit exactly in highp float.
#ifndef FX_WAVES_STORAGE_SH
#define FX_WAVES_STORAGE_SH

float fxWavesEncode(float value, float range)
{
    // Signed fixed point around the exactly representable zero code 32768; code 0 is unused.
    return floor(clamp(value / range, -1.0f, 1.0f) * 32767.0f + 32768.5f);
}

float fxWavesDecode(float code, float range)
{
    return (code - 32768.0f) * (range / 32767.0f);
}

float fxWavesEncodeHeight(float height, float range)
{
    // A source supplies millimetres over a step; linear +-2 m height codes lose that quiet wave to rounding.
    // Point-sampled state permits companding: preserve the full range, with finer precision near still water.
    // Twenty binary decades give <0.022% relative rounding error away from zero and sub-nanometre steps at zero.
    return fxWavesEncode(sign(height) * log2(1.0f + abs(height) * (1048575.0f / range)) / 20.0f, 1.0f);
}

float fxWavesDecodeHeight(float code, float range)
{
    float value = fxWavesDecode(code, 1.0f);
    return sign(value) * (exp2(abs(value) * 20.0f) - 1.0f) * (range / 1048575.0f);
}

float fxWavesEncodeVelocity(float displacement)
{
    // Reference-step displacement, not a second absolute height. Concentrate precision near still water;
    // retain +-4 m, the full difference of two clamped +-2 m heights (dt is never below the reference step).
    return fxWavesEncodeHeight(displacement, 4.0f);
}

float fxWavesDecodeVelocity(float code)
{
    return fxWavesDecodeHeight(code, 4.0f);
}
#endif
