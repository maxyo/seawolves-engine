// The bakes of the current location: one pass of probe rays over what the place draws (fx/place_probes.hpp) gives its
// visibility volume, in a town the sky on its lightmaps' texels and in an interior its ambient-occlusion volume; one
// ray structure (fx::Occluders) serves that pass and the town light's; one cache file holds all of it.
//
// render::SetEffectsLocationGeometry hands over the still models as they are drawn, render::SetEffectsLightmaps a
// town's lightmapped ground (the cells then cover it and the air above it instead of the whole island). At the end of
// that frame the place's key is hashed and its file read on the bakes' load thread; what the file lacks is baked on
// the bake thread (fx::PostBake) and written. The volumes are uploaded when they arrive (effects/visibility.hpp,
// effects/ao_volume.hpp) and the town light gets the place (PlaceReady). Nothing is baked or kept while nothing reads
// it (Низкая: no per-pixel lighting).
//
// The probes are a quality product: the place opens without them and takes them when they arrive (the scene's
// reveal never waits for them); a failure is logged once per place and the place then counts as under open sky.
#pragma once

#include "effects/ao_volume.hpp"
#include "effects/place.hpp"
#include "effects/visibility.hpp"

#include "storm/fx/bakes.hpp"
#include "storm/render/effects.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace storm::effects
{

class PlaceBake
{
  public:
    enum class State
    {
        None,     // no location geometry, or nothing reads the probes
        Building, // on the load or the bake thread
        Ready,
        Failed,   // logged once per location
    };
    // Main thread. The place's still geometry (no triangles: no place).
    void set_geometry(const render::EffectsLocationGeometry &geometry);
    // Main thread. A town's lightmapped ground (absolute world box; lo > hi: none).
    void set_focus(std::string_view name, const std::array<float, 3> &lo, const std::array<float, 3> &hi);
    // Main thread, once per frame, after the frame's reports: starts the place's load, takes in what arrived.
    // `wanted`: something reads the probes; `keep_rays`: a bake of the place still needs its ray structure.
    void update(bool wanted, bool keep_rays);
    State state() const { return state_; }
    const Visibility::Bound *visibility() const { return state_ == State::Ready ? visibility_.bound() : nullptr; }
    const AoVolumes::Bound *ao_volume() const { return state_ == State::Ready ? ao_.bound() : nullptr; }
    // The place once its probes are there (null before, and for a place that failed).
    const std::shared_ptr<const PlaceReady> &ready() const { return ready_; }

    void shutdown();

  private:
    struct Job
    {
        std::atomic<int> state{0}; // 0 pending, 1 ready, 2 nothing to bake
        std::shared_ptr<PlaceStore> store;
        fx::PlaceProbes probes;
        std::string how;
    };
    void rebuild();    // the place or its grid changed: everything of it goes
    void start();
    void take(Job &job);
    fx::ProbeParams params_for(bool interior, bool room, const std::array<float, 3> &room_lo,
                               const std::array<float, 3> &room_hi) const;

    std::shared_ptr<Place> place_; // null: no place
    bool room_ = false;
    std::array<float, 3> room_lo_{}, room_hi_{};
    std::vector<fx::Vec3> walk_;
    std::string focus_name_;
    std::array<float, 3> focus_lo_{1.0f, 1.0f, 1.0f}, focus_hi_{-1.0f, -1.0f, -1.0f};
    std::shared_ptr<PlaceRays> rays_;
    fx::Bake<Job> job_;
    State state_ = State::None;
    bool wanted_ = false;
    std::shared_ptr<const PlaceReady> ready_;
    Visibility visibility_;
    AoVolumes ao_;
};

} // namespace storm::effects
