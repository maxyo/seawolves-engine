#include "model_textures.hpp"

#include "rdf.h"

#include <cstring>
#include <fstream>

namespace storm::geometry
{
std::optional<std::vector<std::string>> ModelTextureNames(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    RDF_HEAD head{};
    if (!in.read(reinterpret_cast<char *>(&head), sizeof(head)) || head.version != RDF_VERSION || head.name_size < 0 ||
        head.names < 0 || head.ntextures < 0 || head.name_size > (64 << 20) || head.ntextures > 65536)
        return std::nullopt;
    std::vector<char> names(static_cast<size_t>(head.name_size));
    std::vector<int32_t> textures(static_cast<size_t>(head.ntextures));
    in.read(names.data(), head.name_size);
    in.seekg(static_cast<std::streamoff>(head.names) * sizeof(int32_t), std::ios::cur); // the other names' offsets
    in.read(reinterpret_cast<char *>(textures.data()), static_cast<std::streamsize>(textures.size() * sizeof(int32_t)));
    if (!in)
        return std::nullopt;
    std::vector<std::string> result;
    for (const int32_t offset : textures)
    {
        if (offset < 0 || offset >= head.name_size)
            return std::nullopt;
        const char *name = names.data() + offset;
        result.emplace_back(name, ::strnlen(name, static_cast<size_t>(head.name_size - offset)));
    }
    return result;
}
} // namespace storm::geometry
