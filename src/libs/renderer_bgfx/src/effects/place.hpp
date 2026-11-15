// What a place's bakes share (effects/place_bake.hpp makes them, the town light reads them): one copy of its still
// geometry, one ray structure over it, one cache file.
#pragma once

#include "storm/fx/place_probes.hpp"
#include "storm/fx/sun_fit.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace storm::effects
{

// The place's still geometry as it is drawn (render::EffectsLocationGeometry), kept once.
struct Place
{
    std::string name;
    bool interior = false;
    std::vector<fx::Vec3> positions;
    std::vector<std::array<float, 2>> uvs; // per position
    std::vector<uint32_t> indices;
    std::vector<fx::Occluders::Cut> cut;   // per triangle: where its texture's alpha cuts holes
    std::vector<uint32_t> colors;          // per position: the baked colours (the town's walls are calibrated on them)
};

// The place's rays: built by the first bake that needs them (none does when the cache holds everything) and let go of
// when no bake is left (release): nothing keeps a ray structure while one plays.
class PlaceRays
{
  public:
    explicit PlaceRays(std::shared_ptr<const Place> place) : place_(std::move(place)) {}
    // Any thread. `build_ms` (may be null): how long the build took, 0 when it was there.
    std::shared_ptr<const fx::Occluders> get(double *build_ms = nullptr);
    void release();

  private:
    std::shared_ptr<const Place> place_;
    std::mutex mutex_;
    std::shared_ptr<const fx::Occluders> occluders_;
};

// The place's cache file (fx::PlaceFile), read once by the place's load and written whole when a section is added.
class PlaceStore
{
  public:
    PlaceStore(std::filesystem::path directory, uint64_t key) : directory_(std::move(directory)), key_(key) {}
    uint64_t key() const { return key_; }
    // Load thread: reads the file. Empty: no error; else why the file was refused (it is then rewritten).
    std::string read();
    // Any thread: a copy of the section, empty when it is not there.
    std::vector<uint8_t> find(fx::PlaceSection kind, uint64_t key);
    // Any thread: adds the section and writes the file. Empty: written; else why it is baked again next time.
    std::string put(fx::PlaceSection kind, uint64_t key, std::vector<uint8_t> bytes, size_t keep = 1);

  private:
    std::filesystem::path directory_;
    uint64_t key_;
    std::mutex mutex_;
    fx::PlaceFile file_;
};

// A place whose probes are there.
struct PlaceReady
{
    std::shared_ptr<const Place> place;
    std::shared_ptr<PlaceRays> rays;
    std::shared_ptr<PlaceStore> store;
    std::shared_ptr<const fx::VisibilityVolume> volume;        // outdoors (null in an interior: only its texture is kept)
    std::shared_ptr<const std::vector<uint8_t>> near_sky;      // per cell of `volume` (fx::PlaceProbes::near_sky)
};

} // namespace storm::effects
