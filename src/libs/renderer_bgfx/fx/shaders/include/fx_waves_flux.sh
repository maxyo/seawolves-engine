// The hull/body source is a displacement rate (m/s), not an acceleration. Shared with the CPU regression.
#ifndef FX_WAVES_FLUX_SH
#define FX_WAVES_FLUX_SH

float fxWavesSourceDisplacement(float rate, float step)
{
    return rate * step;
}

float fxWavesHistoryBase(float height, float source, float hold)
{
    // Exclude externally supplied height from the free-wave velocity. Otherwise every source increment is
    // added again on subsequent steps, turning a constant displacement rate into a continuously accelerating wave.
    return (height + source) * hold;
}
#endif
