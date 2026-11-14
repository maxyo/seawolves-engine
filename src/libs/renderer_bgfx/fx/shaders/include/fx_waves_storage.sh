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

float fxWavesEncodeVelocity(float displacement)
{
    // Reference-step displacement, not a second absolute height. Concentrate precision near still water;
    // retain +-4 m, the full difference of two clamped +-2 m heights (dt is never below the reference step).
    return fxWavesEncode(sign(displacement) * sqrt(abs(displacement) * 0.25f), 1.0f);
}

float fxWavesDecodeVelocity(float code)
{
    float value = fxWavesDecode(code, 1.0f);
    return value * abs(value) * 4.0f;
}
#endif
