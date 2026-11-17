// The texture names a model file (.gm, rdf.h) lists, read from its bytes without building it: what a model loaded in
// the background (geom_deferred.cpp) reads ahead before it is built.
#pragma once

#include "rdf.h"

#include <cstring>
#include <string>
#include <vector>

namespace storm::geometry
{
// The texture names of a model file (RDF_HEAD, the names, their indices, the textures' name offsets); empty when the
// file is too short for what its header says (the build reports it).
inline std::vector<std::string> TextureNames(const std::vector<char> &file)
{
    std::vector<std::string> result;
    RDF_HEAD head;
    if (file.size() < sizeof(head))
        return result;
    std::memcpy(&head, file.data(), sizeof(head));
    if (head.version != RDF_VERSION || head.name_size <= 0 || head.names < 0 || head.ntextures <= 0)
        return result;
    const size_t names = sizeof(head);
    const size_t offsets = names + size_t(head.name_size) + size_t(head.names) * sizeof(int32_t);
    if (file.size() < offsets + size_t(head.ntextures) * sizeof(int32_t))
        return result;
    const char *strings = file.data() + names;
    for (int32_t t = 0; t < head.ntextures; t++)
    {
        int32_t offset;
        std::memcpy(&offset, file.data() + offsets + size_t(t) * sizeof(int32_t), sizeof(offset));
        if (offset < 0 || offset >= head.name_size)
            continue;
        const size_t length = strnlen(strings + offset, size_t(head.name_size - offset));
        if (length > 0 && length < size_t(head.name_size - offset))
            result.emplace_back(strings + offset, length);
    }
    return result;
}
} // namespace storm::geometry
