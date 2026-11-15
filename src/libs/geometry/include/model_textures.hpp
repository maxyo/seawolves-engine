#pragma once
// The texture names a model file (.gm, src/rdf.h) uses, as they are written in it (the game resolves them like
// GEOM_SERVICE::TextureFile: the texture path, then the name). Reads the header, the name block and the texture table
// only (a few KB): the location reads its neighbours' models with it (location place_neighbours.h).
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace storm::geometry
{
// Nothing when the file cannot be read or is no model of this format.
std::optional<std::vector<std::string>> ModelTextureNames(const std::filesystem::path &file);
} // namespace storm::geometry
