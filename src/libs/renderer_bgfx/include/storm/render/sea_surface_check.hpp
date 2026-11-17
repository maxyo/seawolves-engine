// The sea's GPU surface self-check (SEA, src/libs/sea/src/sea.cpp). The sea's vertex ports displace its flat grid from
// a height field read in the vertex shader (shaders/include/sea_surface.sh); a device whose vertex stage reads that
// texture wrong would draw a flat or broken sea. Before the sea uses that path, it runs the same shader code on the
// device for a few points of its real field (vs_sea_check.sc: the results written to a float target and read back)
// and compares them with its CPU formula (SEA::WaveXZ); on a mismatch it keeps the surface on the CPU and says so.
// Game thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace storm::shim
{
struct BaseTexture;
}

namespace storm::render
{

inline constexpr uint32_t kSeaSurfaceCheckMaxPoints = 64;

// One point's surface as the GPU computed it: height (metres) and unit normal.
struct SeaSurfaceSample
{
    float height = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
};

// Starts a check: `count` points (`points`: x, z pairs, metres from the mesh's origin) through sea_surface.sh with
// `field` at D3DVERTEXTEXTURESAMPLER0 and the surface registers c4..c8 (`registers`: 5 x 4 floats, sea_surface_gpu.hpp),
// in the frame being recorded. Returns its ticket for PollSeaSurfaceCheck, or 0 with `why` set: `retry` when it may run
// in a later frame (no device ready yet, an earlier check still in flight, no transient memory left in this frame),
// otherwise the device cannot run it (no float render target, no vertex texture units).
uint32_t StartSeaSurfaceCheck(shim::BaseTexture *field, const float *registers, const float *points, uint32_t count,
                              std::string &why, bool &retry);

enum class SeaSurfaceCheckState
{
    Pending, // the frame with the read-back has not come back yet
    Done,    // `samples` holds the results, one per point
    Lost,    // the ticket is not the latest check, or its resources went away with the device: start another
};
SeaSurfaceCheckState PollSeaSurfaceCheck(uint32_t ticket, std::vector<SeaSurfaceSample> &samples);

// The device for a log line: backend, GPU and driver (vendor, renderer, version) and its vertex texture units.
std::string SeaSurfaceCheckDevice();

} // namespace storm::render
