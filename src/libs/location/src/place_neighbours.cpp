#include "place_neighbours.h"

#include "core.h"
#include "model_textures.hpp"
#include "string_compare.hpp"
#include "v_file_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace storm::location
{
namespace
{
// The element of a script array of attribute classes whose `id` is `id` (case-insensitive, as the scripts compare).
ATTRIBUTES *FindById(const char *array, const char *id)
{
    auto *data = static_cast<VDATA *>(core.GetScriptVariable(array));
    if (!data)
        return nullptr;
    for (uint32_t i = 0, n = data->GetElementsNum(); i < n; ++i)
        if (ATTRIBUTES *element = data->GetAClass(i))
            if (const char *element_id = element->GetAttribute("id"); element_id && storm::iEquals(std::string_view(element_id), std::string_view(id)))
                return element;
    return nullptr;
}

// A value by its dotted path ("filespath.models"); null when it is not there.
const char *Value(ATTRIBUTES *attributes, const char *path)
{
    ATTRIBUTES *found = attributes->FindAClass(attributes, path);
    return found ? static_cast<const char *>(found->GetThisAttr()) : nullptr;
}

std::string Join(std::string directory, const char *name)
{
    while (!directory.empty() && (directory.back() == '\\' || directory.back() == '/'))
        directory.pop_back();
    return directory.empty() ? name : directory + "\\" + name;
}

// The game file of a texture as the renderer finds it (Renderer::TextureCreate): the texture path inserted before the
// name's file part, then the name as it is.
std::optional<std::filesystem::path> TextureFile(const std::string &textures, const std::string &name)
{
    std::vector<std::string> tries;
    if (!textures.empty())
    {
        const size_t slash = name.find_last_of('\\');
        const size_t split = slash == std::string::npos ? 0 : slash + 1;
        tries.push_back(name.substr(0, split) + textures + name.substr(split));
    }
    tries.push_back(name);
    for (const std::string &t : tries)
    {
        std::filesystem::path file = std::filesystem::u8path(fio->ConvertPathResource(("resource\\textures\\" + t + ".tx").c_str()));
        std::error_code error;
        if (std::filesystem::is_regular_file(file, error))
            return file;
    }
    return std::nullopt;
}
} // namespace

void PlaceNeighbours::Begin(ATTRIBUTES *location, const std::function<std::optional<std::array<float, 3>>(const char *)> &exit)
{
    begun_ = true;
    const char *id = location ? static_cast<const char *>(location->GetAttribute("id")) : nullptr;
    ATTRIBUTES *reload = location ? location->GetAttributeClass("reload") : nullptr;
    if (!id || !reload)
        return; // nothing to make ahead: published empty
    place_ = id;
    for (size_t i = 0; i < reload->GetAttributesNum(); ++i)
    {
        ATTRIBUTES *exit_attributes = reload->GetAttributeClass(static_cast<uint32_t>(i));
        const char *go = exit_attributes ? static_cast<const char *>(exit_attributes->GetAttribute("go")) : nullptr;
        if (!go || !go[0] || storm::iEquals(std::string_view(go), std::string_view(id)))
            continue;
        const char *locator = exit_attributes->GetAttribute("name");
        const std::optional<std::array<float, 3>> position = locator ? exit(locator) : std::nullopt;
        // One neighbour per place it leads to; the first of its exits with a locator places it.
        auto known = std::find_if(neighbours_.begin(), neighbours_.end(),
                                  [&](const render::EffectsNeighbour &n) { return storm::iEquals(n.name, std::string(go)); });
        if (known != neighbours_.end())
        {
            if (!known->position)
                known->position = position;
            continue;
        }
        const size_t index = neighbours_.size();
        if (ATTRIBUTES *target = FindById("Locations", go))
        {
            const char *models = Value(target, "filespath.models");
            if (!models)
                continue;
            const char *textures = Value(target, "filespath.textures");
            for (const char *group : {"models.always", "models.day", "models.night"})
                if (ATTRIBUTES *list = target->FindAClass(target, group))
                    for (size_t m = 0; m < list->GetAttributesNum(); ++m)
                    {
                        const char *key = list->GetAttributeName(m);
                        const char *model = list->GetAttribute(m);
                        if (!model || !model[0] || !key || storm::iEquals(std::string_view(key), std::string_view("grassPatch")) ||
                            storm::iEquals(std::string_view(key), std::string_view("charactersPatch")) ||
                            storm::iEquals(std::string_view(key), std::string_view("jumpPatch")) ||
                            storm::iEquals(std::string_view(key), std::string_view("lights")))
                            continue;
                        models_.push_back({index, Join(models, model), textures ? textures : ""});
                    }
        }
        else if (ATTRIBUTES *island = FindById("Islands", go))
        {
            const char *models = Value(island, "filespath.models"), *model = island->GetAttribute("model");
            const char *texture_path = island->GetAttribute("TexturePath");
            if (!models || !model)
                continue;
            models_.push_back({index, Join(models, model), texture_path ? Join("islands", texture_path) + "\\" : ""});
        }
        else
        {
            spdlog::info("location neighbours: {}: exit {} leads to {}, no location or island of that id", place_,
                         locator ? locator : "?", go);
            continue;
        }
        neighbours_.push_back({go, position, {}});
    }
}

void PlaceNeighbours::Step()
{
    if (!begun_ || published_)
        return;
    if (next_ < models_.size())
    {
        const Model &model = models_[next_++];
        const std::filesystem::path file =
            std::filesystem::u8path(fio->ConvertPathResource(("resource\\models\\" + model.file + ".gm").c_str()));
        if (const auto names = storm::geometry::ModelTextureNames(file))
        {
            auto &files = neighbours_[model.neighbour].files;
            for (const std::string &name : *names)
                if (const auto texture = TextureFile(model.textures, name);
                    texture && std::find(files.begin(), files.end(), *texture) == files.end())
                    files.push_back(*texture);
        }
        else
            ++unreadable_;
        return;
    }
    published_ = true;
    size_t textures = 0;
    for (const auto &n : neighbours_)
        textures += n.files.size();
    if (!place_.empty())
        spdlog::info("location neighbours: {}: {} neighbours, {} models ({} unreadable), {} textures", place_,
                     neighbours_.size(), models_.size(), unreadable_, textures);
    render::SetEffectsNeighbours(place_, std::move(neighbours_));
    neighbours_.clear();
    models_.clear();
}
} // namespace storm::location
