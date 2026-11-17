#include "ports.hpp"
#include "device.hpp"

namespace storm::shim
{
namespace
{
// The 11 vs_1_1 / ps_1_x shaders referenced by src/techniques (see shaders/ports) and the coast foam's pair
// (technique CoastFoam, src/libs/island/src/foam.cpp). vsc_count must match the
// port's PORT_VSC_COUNT (tests/renderer_bgfx/run.py checks it).
const ShaderPort ports[] = {
    {"grass_main", true, "vs_port_grass", 0, 82},
    {"particles_vertex_shader", true, "vs_port_particles", 0, 14},
    {"particles_pixel_shader", false, "fs_port_particles", 4, 0},
    {"postprocess_shader", false, "fs_port_postprocess", 4, 0},
    {"caustic_vertex_shader", true, "vs_port_caustic", 0, 16},
    {"caustic_pixel_shader", false, "fs_port_caustic", 3, 0, true},
    {"sea_sea2_vertex_shader", true, "vs_port_sea2", 0, 59, false, true, true},
    {"sea_sea2_pixel_shader", false, "fs_port_sea2", 4, 0, false, true},
    {"sea_sea3_vertex_shader", true, "vs_port_sea3", 0, 59, false, false, true},
    {"sea_sea3_pixel_shader", false, "fs_port_sea3", 4, 0},
    {"worldmap_shader", true, "vs_port_worldmap", 0, 7},
    {"coastfoam_foam_vertex_shader", true, "vs_port_coastfoam", 0, 72},
    {"coastfoam_foam_pixel_shader", false, "fs_port_coastfoam", 2, 0},
};
} // namespace

uint32_t ShaderPort::shader_id() const
{
    return shader_id_by_name(shader_name);
}

const ShaderPort *find_port(std::string_view key, bool vertex)
{
    for (const ShaderPort &port : ports)
        if (port.key == key && port.vertex == vertex)
            return &port;
    return nullptr;
}
} // namespace storm::shim
