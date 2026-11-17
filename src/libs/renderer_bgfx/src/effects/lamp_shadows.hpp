// The lamps of a location relight it per pixel, each with its own shadow maps (fx/lamp_shadows.hpp,
// Settings::lamp_shadows, option fx_lamp_shadows).
//
// The location reports its lamps every frame (render::SetEffectsLamps: the static lights of lights.ini placed at
// its locators, with their flicker) and once its still geometry with its baked vertex colours
// (render::SetEffectsLocationGeometry). Candles of one fixture are merged; every lamp lights within its shape's radius
// (fx/light_falloff.hpp, the same falloff as every light). Its own fixture (fx::FindFixtures: candlestick, lantern,
// post, chandelier) casts no shadow of it. The lamps nearest to the camera, up to Settings::lamp_shadows, take a slot:
//   * a row of six cube faces in two depth atlases (D24, else D32F: Support::lamp_depth) holding the hardware depth of
//     each face's own perspective projection (fx::LampFaceDepth; no caster writes its depth, early depth testing
//     stays on): `static` with the still geometry around the lamp, each triangle only in the faces it reaches,
//     drawn once when the lamp takes the slot (a few slots per frame; all of them in the first frame of a location);
//     `dynamic` with what moves (the characters and the other lit draws that reach a face within the lamp's reach,
//     on screen or not: the faces are published as shadow volumes, effects/shadow_casters.hpp), drawn only in frames
//     that have such a caster (no clear, no pass otherwise). The faces that hold a caster this frame are a frame
//     uniform (u_fxLampFaces, set before the frame is handed to bgfx: before_frame): the receivers read the dynamic
//     atlas only there;
//   * its number in the lamp grid (a small 3D texture: which slots reach which cell of the place).
// A lamp counts only as far as its light can show (fx::LampReach, fx::kLampNegligible): the grid lists it there,
// still triangles and characters cast into its faces from there; a receiver where its light is under that level
// takes no shadow taps for it. The strongest lamp of a pixel takes the 5x5 filter (Settings::lamp_soft) or the 3x3, up to
// Settings::lamp_filtered of its lamps in all take a filter (the others the 3x3), the rest one hardware-filtered tap;
// at most Settings::lamp_per_pixel grid lamps light one pixel.
// A lamp fades in once its faces are drawn and fades out before its slot goes to another lamp.
// The last Settings::lamp_moving slots are for the lights that move, the nearest to the camera; the receivers take
// them without the grid, and they only add light (no bake holds theirs), as far and as strong as the game's point
// light they stand for (its shape, as fxPointLight lights it):
//   * carried lanterns and torches (render::SetEffectsDynamicLights): their still faces are drawn again from the
//     location's triangles around them whenever they move (or their radius changes), each triangle only into the
//     faces it reaches; their colour, intensity and core follow the light every frame;
//   * the ships' lanterns (render::SetEffectsShipLights): a lantern stands still on its ship, so its faces are in
//     the ship's space (the receivers and the casters turn into it): the still faces hold the ship itself but the
//     lantern's own fixture (render::SetEffectsShipGeometry, drawn once and when the ship changes), the dynamic ones
//     the crew, the sails and other ships.
// Receivers (the lit and baked variants into the game picture, ff_fragment.sh ffLampLight) look the lamps of their
// cell up, evaluate each with its shape and compare against both atlases. A lamp without a slot
// lights nothing per pixel: no light without its shadow.
// Interiors also get the room light (fx::RoomLight, a 3D texture of the baked colours around every point): the
// ambient of the lit models that stand in the room.
// All positions here are absolute world. The textures are always bound (1x1 open ones when there is nothing), since
// GL refuses a draw that leaves a shadow or 3D sampler on a unit of another type.
#pragma once

#include "effects/casters.hpp"
#include "effects/shadow_casters.hpp"
#include "storm/fx/lamp_shadows.hpp"
#include "storm/fx/settings.hpp"
#include "storm/render/effects.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace storm::shim
{
struct Device;
struct TexStore;
} // namespace storm::shim

namespace storm::fx
{
struct AlphaMask;
}

namespace storm::effects
{

class LampShadows
{
  public:
    void configure(const fx::Settings &settings, fx::LampDepth depth);
    bool enabled() const { return slots_ > 0; }
    // An interior is loaded (its still geometry was reported): the weather's sun and ambient are not its light.
    bool interior() const { return interior_ && !positions_.empty(); }
    // Main thread, every frame the location draws: its lamps (empty when it has none).
    void set_lamps(const std::vector<fx::Lamp> &lamps);
    void set_lamps(const std::vector<fx::Lamp> &components, std::span<const render::EffectsLampFlame> flames);
    // The place's lamps as the relight lights them (merged fixtures, range = the radius it windows them to) and their
    // fixtures' sizes.
    const std::vector<fx::Lamp> &lamps() const { return lamps_; }
    const std::vector<float> &holders() const { return holder_; }
    // Main thread, every frame the location draws: the lights that move (carried lanterns and torches).
    void set_moving(std::vector<fx::Lamp> lamps)
    {
        moving_ = std::move(lamps);
        moving_this_frame_ = true;
    }
    // ... and the lanterns of the ships at sea with their ships (render::SetEffectsShipLights).
    struct ShipLight
    {
        fx::Lamp lamp;     // absolute world
        uint64_t ship = 0;
        fx::Vec3 local{};  // in the ship's space
    };
    void set_ship_lights(std::vector<render::EffectsShip> ships, std::vector<ShipLight> lights)
    {
        ships_ = std::move(ships);
        ship_lights_ = std::move(lights);
        ship_lights_this_frame_ = true;
    }
    // render::EffectsShipGeometryWanted / SetEffectsShipGeometry.
    bool ship_geometry_wanted(uint64_t ship, uint64_t signature) const;
    void set_ship_geometry(uint64_t ship, uint64_t signature, const render::EffectsShipGeometry &geometry);
    // The still geometry of the location (render::EffectsLocationGeometry); an empty one: none (the sea).
    void set_geometry(const render::EffectsLocationGeometry &geometry);
    // The first receiver of a frame: gives the lamps around the camera at `eye` their slots, renders static faces
    // that are due, opens the dynamic atlas and publishes the dynamic faces of the ready slots to `volumes`
    // (effects/shadow_casters.hpp): the shim's caster entry draws into them what moves and reaches them (a
    // character, a prop, a ship's sails, another ship; not the still geometry the slot's static faces hold). True
    // when this frame's receivers take lamps or the room light.
    bool prepare(shim::Device &device, const std::array<float, 3> &eye, Casters &casters, ShadowCasters &volumes);
    // A point light at `position` is one of the place's lamps, or a moving light, that has a slot this frame: the
    // receivers light it per pixel with its shadow, so it leaves the draw's own light sums.
    bool relit(const fx::Vec3 &position) const;
    bool relit(const fx::AggregateRef &ref) const;
    std::array<fx::AggregateRef, fx::kMaxShadowLamps> aggregate_refs() const;
    // This frame's lit models take their ambient from the room light.
    bool room_light() const { return active_ && room_ready_; }
    // u_fxLampView (view -> absolute world) for every lit or baked draw with the camera `view` (row-vector absolute
    // world -> view): fx_lighting.sh and ff_fragment.sh place every such pixel with it, lamps or not.
    void bind_view(const std::array<float, 16> &view);
    // Receiver uniforms and textures of a draw whose program has the lamps (FX_FF_LAMPS), after bind_view();
    // `active` false: the open textures and no lamps. `room`: the draw's ambient is the room light.
    void bind(bool active, bool room, const std::array<float, fx::kMaxShadowLamps> &aggregate = {},
              const std::array<fx::AggregateRef, fx::kMaxShadowLamps> &overrides = {});
    int initial_state() const {
        if (!enabled()) return 0;
        if (!prepared_) return 1;
        if (room_dirty_) return 1;
        if (!room_.rgba.empty() && !bgfx::isValid(room_map_)) return 2;
        if ((lamps_.empty() || positions_.empty()) && moving_.empty() && ship_lights_.empty() && room_.rgba.empty()) return 0;
        if (!active_ || !bgfx::isValid(static_map_) || !bgfx::isValid(dynamic_map_) ||
            !bgfx::isValid(static_fb_) || !bgfx::isValid(dynamic_fb_)) return 2;
        if (layout_dirty_ || grid_dirty_ || room_dirty_) return 1;
        if (!grid_.rgba.empty() && !bgfx::isValid(grid_map_)) return 2;
        for (const auto &slot : slot_) if (slot.lamp != -1 && !slot.ready) return 1;
        return 0;
    }
    // Right before every bgfx::frame() (a present, or a flush for a read-back that ends the bgfx frame early): the
    // faces that took a caster this frame (the published volumes' counts) into u_fxLampFaces, which every draw of
    // that bgfx frame reads.
    void before_frame();
    void end_frame();
    // Lamps with a slot and static faces drawn in the last frame (effects profile). Face draws and atlas clears are
    // renderer counters (render::PerfCounters).
    int last_slots() const { return last_slots_; }
    // A slot is fading in or out (kFadeSeconds of the steady clock): what it lights is not yet its final light.
    bool fading() const;
    int last_static_faces() const { return last_static_faces_; }

    void shutdown();

  private:
    // Every handle invalid and every slot, grid and room to be built again (shutdown, after the destroys).
    void forget_gpu_objects();
    // Index lists of a slot's still faces (fx::LampFaceLists): one buffer, list k = face * 2 + near the holder.
    struct FaceBuffer
    {
        bgfx::IndexBufferHandle handle = BGFX_INVALID_HANDLE;
        std::array<uint32_t, 13> first{};
    };
    struct Slot
    {
        int lamp = -1;          // index into lamps_, -1: free (a moving slot: kMovingLamp while it holds a light)
        bool moving = false;    // one of the slots kept for lights that move: its still faces follow the light
        fx::Lamp at{};          // the lamp its faces were drawn for (range = its radius)
        float reach = 0.0f;     // metres its light counts (fx::LampReach with the flicker's headroom)
        float holder = 0.0f;    // metres around it in which the still geometry casts nothing
        bool ready = false;     // its static faces are drawn
        bool redraw = false;    // a moving light has moved, a ship's geometry came: its still faces are drawn again
        uint64_t ship = 0;      // a ship's lantern: the ship (its faces are in the ship's space)
        fx::Vec3 local{};       // ... the lantern in that space
        std::array<float, 16> to_local{}; // ... absolute world -> that space (row vectors), this frame
        bool leaving = false;   // fading out: the slot goes to another lamp
        float weight = 0.0f;    // 0..1, the fade
        FaceBuffer triangles;   // the solid still geometry within its reach
        std::vector<std::pair<std::shared_ptr<shim::TexStore>, FaceBuffer>> cut; // ... and of each cut-out texture
        std::array<ShadowCasters::Id, 6> volume{}; // this frame's dynamic faces (while `published`)
        bool published = false;
    };

    void ensure_targets();
    // The faces' index buffer of `triangles` (index triples into `positions`) for a lamp at `lamp` (an invalid
    // handle when none reaches a face).
    static FaceBuffer face_buffer(std::span<const fx::Vec3> positions, std::span<const uint32_t> triangles, fx::Vec3 lamp,
                                  float holder);
    static void free_faces(FaceBuffer &faces);
    float reach_of(const fx::Lamp &lamp) const;
    void choose_slots(const std::array<float, 3> &eye);
    void free_slot(Slot &slot);
    void draw_static(shim::Device &device, bool all, Casters &casters);
    void choose_moving(const std::array<float, 3> &eye);
    // The six still faces of `slot` (its row cleared first) from `triangles` / `cut` index buffers.
    void draw_faces(shim::Device &device, Casters &casters, int slot);
    static constexpr int kMovingLamp = -2;
    int static_slots() const { return slots_ - moving_slots_; }
    void upload_grid();
    void upload_room();

    int slots_ = 0;
    fx::LampDepth depth_ = fx::LampDepth::None;
    int per_pixel_ = fx::kLampGridCapacity;
    int filtered_ = fx::kLampGridCapacity;
    // prepare(): the dynamic faces were offered their casters in this run of frames under the loading cover, and in
    // how many covered frames since they were not.
    bool covered_offered_ = false;
    uint32_t covered_frames_ = 0;
    float level_ = fx::kLampNegligible; // where a lamp ends (Settings::lamp_reach_light), the receivers' gamma units over the strongest gain
    float shadow_level_ = fx::kLampNegligible; // under it no shadow taps (Settings::lamp_shadow_light), likewise
    int face_size_ = 0, dynamic_face_size_ = 0;
    int columns_ = 0, rows_ = 0;
    float gain_ = 0.5f, baked_ = 0.5f;
    bool soft_ = false;
    int debug_ = 0;
    int moving_slots_ = 0;         // the last slots of the atlas
    std::vector<fx::Lamp> moving_; // the lights that move, as reported this frame
    std::vector<ShipLight> ship_lights_;
    std::vector<render::EffectsShip> ships_;
    // The still geometry of the ships whose lanterns hold a slot, in each ship's space.
    struct ShipGeometry
    {
        uint64_t ship = 0, signature = 0;
        bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
        std::vector<fx::Vec3> positions;           // the ship's space
        std::vector<uint32_t> indices;
        std::vector<int32_t> cut_of;               // per triangle: -1 casts whole, else its texture in `cut`
        std::vector<std::shared_ptr<shim::TexStore>> cut;      // the textures that cut holes
        // Per lantern (its place in the ship's space): its fixture (fx::FindFixtures), found once.
        std::vector<std::pair<fx::Vec3, std::vector<uint32_t>>> fixtures;
        size_t triangles = 0;
    };
    // The still faces' index buffers of a ship's lantern in `slot`: the ship's triangles but its own fixture's.
    void ship_slot_triangles(Slot &slot, ShipGeometry &geometry);
    std::vector<ShipGeometry> ship_geometry_;
    void free_ship_geometry(ShipGeometry &geometry);
    const ShadowCasters *volumes_ = nullptr; // where the slots' faces are published this frame
    bool moving_this_frame_ = false, ship_lights_this_frame_ = false; // reported since the last Present (else: none)
    std::vector<fx::Lamp> raw_;   // as reported this frame
    std::vector<int> raw_lamp_;   // per raw lamp: its merged lamp (index into lamps_), -1: none (no light)
    std::vector<fx::Lamp> lamps_; // merged, this frame's colours; range = the radius it lights
    std::vector<fx::Vec3> placed_; // per merged lamp: where it stood when the slots were laid out
    std::vector<float> holder_;   // per merged lamp: the size of its fixture
    std::vector<Slot> slot_;
    bool layout_dirty_ = true; // the lamps or the geometry changed: every slot is chosen and drawn again at once
    bool grid_dirty_ = true;
    bool prepared_ = false, active_ = false;
    bool geometry_dirty_ = false, room_dirty_ = false, room_ready_ = false;
    bool interior_ = false;
    std::vector<fx::Vec3> positions_;
    std::vector<uint32_t> colors_;
    std::vector<std::array<float, 2>> uvs_;
    std::vector<uint32_t> indices_; // every triangle (the room light)
    std::vector<uint32_t> solid_;   // the triangles that cast whole
    fx::TriangleGrid solid_grid_;   // ... by where they are
    std::vector<uint32_t> seen_;    // scratch of fx::TrianglesInSphere
    struct CutOut                   // the triangles of one texture whose alpha cuts holes
    {
        std::shared_ptr<shim::TexStore> store; // held while these triangles can be drawn
        std::vector<uint32_t> indices;
        fx::TriangleGrid grid;
    };
    std::vector<CutOut> cut_;
    // Surface runs are part of the geometry's identity, including the storage and alpha mask. Equal counts alone
    // do not mean equal shadows: a model reload may replace its texture or move a run to different triangles.
    struct CutSurface
    {
        uint32_t first = 0, count = 0;
        std::shared_ptr<shim::TexStore> store;
        std::shared_ptr<const fx::AlphaMask> mask;
        bool operator==(const CutSurface &) const = default;
    };
    std::vector<CutSurface> cut_surfaces_;
    std::vector<int32_t> list_of_;   // per triangle of indices_: -1 in solid_, else its cut_
    std::vector<uint32_t> local_of_; // ... its number in that list
    // Per merged lamp: the triangles of its own fixture (fx::FindFixtures, ascending): they cast no shadow of it.
    std::vector<std::vector<uint32_t>> fixtures_;
    bool fixtures_dirty_ = true;
    void place_lamps(std::vector<fx::Lamp> merged, std::vector<fx::Lamp> raw, std::vector<int> membership);
    void find_fixtures();
    float model_lift_ = 0.0f;
    float tint_ = 0.0f; // Settings::lamp_tint
    float soft_light_ = 0.0f; // Settings::lamp_soft_light
    fx::Vec3 geometry_lo_{}, geometry_hi_{};
    fx::Vec3 room_lo_{}, room_hi_{};
    fx::LampGrid grid_;
    fx::RoomLight room_;
    std::chrono::steady_clock::time_point last_prepare_{};
    int last_slots_ = 0, last_static_faces_ = 0, static_faces_ = 0;
    bgfx::VertexBufferHandle static_vb_ = BGFX_INVALID_HANDLE, clear_vb_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle static_map_ = BGFX_INVALID_HANDLE, dynamic_map_ = BGFX_INVALID_HANDLE, open_map_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle grid_map_ = BGFX_INVALID_HANDLE, room_map_ = BGFX_INVALID_HANDLE, open_grid_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle static_fb_ = BGFX_INVALID_HANDLE, dynamic_fb_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_static_ = BGFX_INVALID_HANDLE, s_dynamic_ = BGFX_INVALID_HANDLE, s_grid_ = BGFX_INVALID_HANDLE,
                        s_room_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_view_ = BGFX_INVALID_HANDLE, u_lamps_ = BGFX_INVALID_HANDLE, u_grid_ = BGFX_INVALID_HANDLE,
                        u_caster_ = BGFX_INVALID_HANDLE, u_faces_ = BGFX_INVALID_HANDLE;
    std::array<std::array<float, 4>, 2 + 3 * fx::kMaxShadowLamps> uniforms_{};
    std::array<std::array<float, 4>, 12> grid_uniforms_{};
    std::array<std::array<float, 4>, fx::kMaxShadowLamps> faces_uniform_{}; // u_fxLampFaces (a frame uniform)
    std::array<float, 16> bound_view_{}, view_to_world_{};
    bool reported_ = false;
    bool transient_reported_ = false, ship_reported_ = false, carried_reported_ = false;
};

} // namespace storm::effects
