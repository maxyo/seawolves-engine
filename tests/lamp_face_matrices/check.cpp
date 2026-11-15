// fuse.py supplies the actual production fragments. Only bgfx submission is recorded, never executed.
#include "storm/fx/lamp_shadows.hpp"
#include <bx/math.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fx = storm::fx;
namespace storm::fx {
// FX_MATH
}
using Matrix = std::array<float, 16>;
struct Draw { Matrix transform; std::array<float, 4> uniform; std::array<uint16_t, 4> scissor; };
namespace bgfx {
struct ProgramHandle { bool valid = true; };
struct Caps { bool homogeneousDepth = false; } caps;
Caps *getCaps() { return &caps; }
bool isValid(ProgramHandle p) { return p.valid; }
Draw current{};
std::vector<Draw> draws;
void setScissor(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { current.scissor = {x,y,w,h}; }
void setUniform(int, const float *p) { std::copy_n(p, 4, current.uniform.begin()); }
void setStencil(int) {}
void setTransform(const float *p) { std::copy_n(p, 16, current.transform.begin()); }
void setState(int) {}
void submit(int, ProgramHandle, int, int) { draws.push_back(current); }
}
namespace shim { struct Device {}; }
struct CasterMaterial {};
struct Casters {
    enum class Pass { Lamp };
    bool valid = true;
    bgfx::ProgramHandle program(shim::Device &, Pass, bool, const CasterMaterial &) { return {valid}; }
};
constexpr int kCasterState = 1, kLampDynamicView = 1, BGFX_STENCIL_NONE = 0, BGFX_DISCARD_NONE = 0;
constexpr float kLanternHolder = 0.3f;
size_t matrix_builds = 0;
class LampShadows {
public:
    struct Slot {
        int lamp = 0;
        bool ready = true;
        fx::Lamp at{};
        uint64_t ship = 0;
        fx::Vec3 local{};
        Matrix to_local{};
        float holder = 0;
        uint32_t casters = 0;
        // CACHE_FIELDS
    };
    std::vector<Slot> slot_ = std::vector<Slot>(16);
    int slots_ = 16, columns_ = 6, rows_ = 16, dynamic_face_size_ = 128, u_caster_ = 0;
    bool active_ = true, drawn_dynamic_ship_node_ = false;
    uint64_t drawn_ship_ = 0;
    void reset() { // RESET
    }
    const Matrix &dynamic_face_matrix(int, int, const fx::Lamp &);
    void submit_caster(shim::Device &, const float *, bool, const std::array<float,3> &, float, Casters &, const CasterMaterial &);
    void submit_uncached(shim::Device &, const float *, bool, const std::array<float,3> &, float, Casters &, const CasterMaterial &);
};
// PRODUCTION
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
Matrix transform(float angle, float x, float z) {
    Matrix m{};
    m[0] = m[10] = std::cos(angle); m[2] = std::sin(angle); m[8] = -m[2];
    m[5] = m[15] = 1; m[12] = x; m[14] = z;
    return m;
}
bool equal(const Draw &a, const Draw &b) {
    return std::memcmp(a.transform.data(), b.transform.data(), sizeof(Matrix)) == 0 &&
        a.uniform == b.uniform && a.scissor == b.scissor;
}
int main(int argc, char **argv) try {
    const std::string mutation = argc > 1 ? argv[1] : "";
    LampShadows lamps;
    shim::Device device; Casters casters; CasterMaterial material;
    size_t cached_total = 0, original_total = 0, draws_total = 0;
    // Changes between frames exercise relocation, range, atlas resize, GL depth and ship-local lanterns.
    for (int frame = 0; frame < 8; ++frame) {
        bgfx::caps.homogeneousDepth = frame % 2;
        lamps.rows_ = 16 + frame; lamps.columns_ = frame % 3 ? 6 : 8;
        for (int s = 0; s < lamps.slots_; ++s) {
            auto &slot = lamps.slot_[s];
            // Slot 13 leaves for a frame then is recycled to another source/coordinate system.
            if (s == 13 && frame == 4) slot = LampShadows::Slot{};
            slot.lamp = s == 14 || (s == 13 && frame == 3) ? -1 : s + frame * 16;
            slot.ready = s != 15;
            slot.at.position = {float(s) * .15f + frame * .2f, .5f, -.75f};
            slot.at.range = 5.f + frame;
            slot.ship = (s == 13 && frame >= 4) ? 0 : (s % 2 ? 42 : 0);
            slot.local = {.4f + .03f * frame, 1.f, -.2f};
            slot.to_local = transform(.13f * frame, -float(frame), float(frame));
        }
        if (mutation != "no-reset") lamps.reset();
        matrix_builds = 0;
        std::vector<Draw> actual;
        auto draw_batch = [&](bool uncached) {
            bgfx::draws.clear();
            for (int draw = 0; draw < 48; ++draw) {
                lamps.drawn_ship_ = draw % 3 ? 0 : 42;
                lamps.drawn_dynamic_ship_node_ = draw % 2;
                lamps.active_ = draw != 45;
                casters.valid = draw != 46;
                const auto world = transform(.07f * draw, float(draw) * .1f, -float(draw));
                // Near casters, a far rejected caster and a lamp-centred sphere spanning all six faces.
                const fx::Vec3 center = draw == 47 ? fx::Vec3{10000.f,0,-10000.f} :
                    fx::Vec3{float(draw % 6) - 2.f, float(draw % 4) - 1.f, float(draw % 5) - 2.f};
                const float radius = draw == 0 ? 8.f : .3f;
                if (uncached) lamps.submit_uncached(device, world.data(), draw % 2, center, radius, casters, material);
                else lamps.submit_caster(device, world.data(), draw % 2, center, radius, casters, material);
            }
        };
        draw_batch(mutation == "uncached");
        actual = bgfx::draws;
        const size_t cached = matrix_builds;
        matrix_builds = 0;
        draw_batch(true);
        require(actual.size() == bgfx::draws.size(), "caster/face admission changed");
        for (size_t i = 0; i < actual.size(); ++i)
            require(equal(actual[i], bgfx::draws[i]), "cached transform/uniform/scissor differs from original");
        require(cached <= 6u * 14u, "projection builds exceed one per live slot/face/frame");
        require(matrix_builds > cached * 3, "fixture did not eliminate repeated projection work");
        cached_total += cached; original_total += matrix_builds; draws_total += actual.size();
    }
    std::cout << "PASS bit-exact draws=" << draws_total << " projection_builds=" << cached_total
              << " original=" << original_total << " frames=8 slots=16 depth=GL/Vulkan ship-local/live-world\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
