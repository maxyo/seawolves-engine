#pragma once

#include "cannon_trace.h"
#include "storm/render/effects.hpp"

// The base height field SEA_BASE::WaveXZ samples (its heights; no ripples, no wakes): two waves of the same animated
// frames, each sampled bilinearly with wrap at (x + move.x) * scale, (z + move.z) * scale.
struct SeaHeightField
{
    const uint8_t *const *frames = nullptr; // frameCount frames of size x size bytes (255 = 1)
    int32_t frameCount = 0, size = 0;
    float frame[2]{};      // each wave's frame, 0..frameCount (blended with the next, which wraps)
    float scale[2]{};      // frame texels per metre
    float amplitude[2]{};  // metres for 1
    glm::vec3 move[2]{};   // added to x and z before the scale
    glm::vec3 camera{};    // beyond `reach` metres of it (in x, z) the sea is flat: WaveXZ returns 0
    float reach = 0.0f;
};

class SEA_BASE : public CANNON_TRACE_BASE
{
  public:
    virtual float WaveXZ(float x, float z, glm::vec3 *vNormal = nullptr) = 0;
    // What WaveXZ computes its heights from this frame (the coast foam samples it on the GPU: CoastFoam); false
    // when there is none. An implementation whose WaveXZ is more than this field must change both.
    virtual bool HeightField(SeaHeightField &field) const = 0;
    // Something on or in the water this frame (a hull, a swimmer, a wader): the sea hands this frame's sources to
    // the renderer's waves from objects when it draws (storm::render::SetEffectsSeaFrame). Execute stage.
    virtual void AddWaveSource(const storm::render::EffectsWaveSource &source) = 0;
};
