#pragma once
// The direct neighbours of a location for the relief maps made ahead (render::SetEffectsNeighbours): the locations its
// reload exits lead to (Locations[] by id: the models of filespath.models, models.always, .day and .night) and the
// island's sea (Islands[] by id: its island model). Read on the game thread a little at a time: Begin takes the
// neighbours from the script attributes, each Step reads the texture names of one model file (geometry
// ModelTextureNames: a few KB), and the last one hands the resolved texture files to the renderer.
#include "storm/render/effects.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ATTRIBUTES;

namespace storm::location
{
class PlaceNeighbours
{
  public:
    // `location`: the location's attributes (Locations[n]). `exit`: the position of a reload locator by its name.
    void Begin(ATTRIBUTES *location, const std::function<std::optional<std::array<float, 3>>(const char *)> &exit);
    // Game thread, every frame after Begin: one model file; when all are read, the neighbours go to the renderer once.
    void Step();
    bool begun() const { return begun_; }

  private:
    struct Model
    {
        size_t neighbour;
        std::string file;     // resource path of the .gm
        std::string textures; // the texture path its textures are looked up under first ("" none)
    };
    std::string place_;
    std::vector<render::EffectsNeighbour> neighbours_;
    std::vector<Model> models_;
    size_t next_ = 0, unreadable_ = 0;
    bool begun_ = false, published_ = false;
};
} // namespace storm::location
