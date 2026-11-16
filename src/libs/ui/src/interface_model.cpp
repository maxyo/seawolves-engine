#include "storm_ui/interface_model.h"

#include <nlohmann/json.hpp>

namespace storm::ui
{

namespace
{

using Json = nlohmann::json;

// Reads the recorded model. A field that is absent takes its default; one present with another
// type than the format's is a defect of the recording, reported (the first one) by the parse.
class Reader
{
  public:
    std::string wrong; // "widgets[3].index: expected a number"

    std::string Text(const Json &object, const char *key, const std::string &where)
    {
        const auto found = object.find(key);
        if (found == object.end())
            return {};
        if (found->is_string())
            return found->get<std::string>();
        Wrong(where, key, "a string");
        return {};
    }

    template <typename Number> Number Num(const Json &object, const char *key, Number fallback, const std::string &where)
    {
        const auto found = object.find(key);
        if (found == object.end())
            return fallback;
        if (found->is_number())
            return found->get<Number>();
        Wrong(where, key, "a number");
        return fallback;
    }

    bool Flag(const Json &object, const char *key, bool fallback, const std::string &where)
    {
        const auto found = object.find(key);
        if (found == object.end())
            return fallback;
        if (found->is_boolean())
            return found->get<bool>();
        Wrong(where, key, "a boolean");
        return fallback;
    }

    // The array or object under `key`, nullptr when absent (or of another type: reported).
    const Json *Find(const Json &object, const char *key, bool array, const std::string &where)
    {
        const auto found = object.find(key);
        if (found == object.end())
            return nullptr;
        if (array ? found->is_array() : found->is_object())
            return &*found;
        Wrong(where, key, array ? "an array" : "an object");
        return nullptr;
    }

    void Wrong(const std::string &where, const std::string &key, const char *expected)
    {
        if (wrong.empty())
            wrong = where + (where.empty() || key.empty() ? "" : ".") + key + ": expected " + expected;
    }

    std::vector<ImageRef> Images(const Json &object, const std::string &where)
    {
        std::vector<ImageRef> out;
        const Json *list = Find(object, "images", true, where);
        if (!list)
            return out;
        for (size_t i = 0; i < list->size(); ++i)
        {
            const Json &entry = (*list)[i];
            const std::string at = where + ".images[" + std::to_string(i) + "]";
            if (!entry.is_object())
            {
                Wrong(at, "", "an object");
                continue;
            }
            ImageRef image;
            image.handle = Num<int64_t>(entry, "handle", 0, at);
            image.revision = Num<int64_t>(entry, "imageRevision", 0, at);
            image.width = Num<int>(entry, "width", 0, at);
            image.height = Num<int>(entry, "height", 0, at);
            image.tint_argb = Num<uint32_t>(entry, "tintArgb", 0xffffffffu, at);
            if (const Json *source = Find(entry, "source", false, at))
            {
                const std::string in = at + ".source";
                image.kind = Text(*source, "kind", in);
                image.group = Text(*source, "group", in);
                image.image = Text(*source, "image", in);
                image.mode = Text(*source, "mode", in);
                image.atlas = Text(*source, "atlas", in);
                image.pic = Num<int>(*source, "pic", -1, in);
                if (const Json *uv = Find(*source, "uv", true, in))
                {
                    if (uv->size() != 4)
                        Wrong(in, "uv", "4 numbers");
                    for (size_t k = 0; k < 4 && k < uv->size(); ++k)
                        if ((*uv)[k].is_number())
                            image.uv[k] = (*uv)[k].get<float>();
                        else
                            Wrong(in, "uv", "4 numbers");
                }
            }
            out.push_back(std::move(image));
        }
        return out;
    }

    std::vector<Cell> Cells(const Json &list, const std::string &where)
    {
        std::vector<Cell> out;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Json &entry = list[i];
            const std::string at = where + "[" + std::to_string(i) + "]";
            if (!entry.is_object())
            {
                Wrong(at, "", "an object");
                continue;
            }
            Cell cell;
            cell.column = Num<int>(entry, "column", static_cast<int>(out.size()), at);
            cell.text = Text(entry, "text", at);
            cell.color_argb = Num<uint32_t>(entry, "colorArgb", 0xffffffffu, at);
            cell.images = Images(entry, at);
            out.push_back(std::move(cell));
        }
        return out;
    }
};

Json ImagesJson(const std::vector<ImageRef> &images)
{
    Json out = Json::array();
    for (const ImageRef &image : images)
    {
        Json entry{{"handle", image.handle},
                   {"imageRevision", image.revision},
                   {"width", image.width},
                   {"height", image.height},
                   {"tintArgb", image.tint_argb}};
        if (!image.kind.empty())
        {
            // The fields of its kind (what the engine sends), and any other one that is set.
            Json source{{"kind", image.kind}};
            const bool texture = image.kind == "texture", battle = image.kind == "battle_icon";
            if (!image.group.empty() || image.kind == "atlas")
                source["group"] = image.group;
            if (!image.image.empty() || image.kind == "atlas" || image.kind == "portrait" || texture)
                source["image"] = image.image;
            if (!image.mode.empty() || battle)
                source["mode"] = image.mode;
            if (!image.atlas.empty() || battle)
                source["atlas"] = image.atlas;
            if (image.pic != -1 || battle)
                source["pic"] = image.pic;
            if (texture || image.uv[0] != 0.0f || image.uv[1] != 0.0f || image.uv[2] != 1.0f || image.uv[3] != 1.0f)
                source["uv"] = {image.uv[0], image.uv[1], image.uv[2], image.uv[3]};
            entry["source"] = std::move(source);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

Json CellsJson(const std::vector<Cell> &cells)
{
    Json out = Json::array();
    for (const Cell &cell : cells)
        out.push_back({{"column", cell.column}, {"text", cell.text}, {"colorArgb", cell.color_argb}, {"images", ImagesJson(cell.images)}});
    return out;
}

} // namespace

std::string InterfaceModelJson(const InterfaceModel &model)
{
    Json widgets = Json::array();
    for (const Widget &widget : model.widgets)
    {
        Json entry{{"node", widget.node},   {"type", widget.type},       {"label", widget.label},
                   {"value", widget.value}, {"index", widget.index},     {"enabled", widget.enabled},
                   {"rowId", widget.row_id}, {"images", ImagesJson(widget.images)}, {"actions", widget.actions}};
        if (widget.type == "slider" || widget.min != 0 || widget.max != 0 || widget.step != 1)
        {
            entry["min"] = widget.min;
            entry["max"] = widget.max;
            entry["step"] = widget.step;
        }
        if (widget.type == "check" || widget.exclusive)
            entry["exclusive"] = widget.exclusive;
        if (!widget.unavailable_reason.empty())
            entry["unavailableReason"] = widget.unavailable_reason;
        if (!widget.column_headers.empty())
            entry["columnHeaders"] = widget.column_headers;
        if (widget.header_cells)
            entry["headerCells"] = CellsJson(*widget.header_cells);
        if (widget.text_color_argb)
            entry["textColorArgb"] = *widget.text_color_argb;
        if (!widget.context.empty())
            entry["context"] = widget.context;
        entry["cells"] = CellsJson(widget.cells);
        widgets.push_back(std::move(entry));
    }
    const Json root{{"schemaVersion", 2},          {"language", model.language}, {"revision", model.revision},
                    {"screenRevision", model.opening}, {"screen", model.screen},   {"widgets", std::move(widgets)}};
    // Text the game gave in a broken encoding is written with U+FFFD, never dropped or thrown on.
    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool SameNode(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

bool Widget::Offers(std::string_view verb) const
{
    if (!enabled)
        return false;
    if (!actions.empty())
    {
        for (const std::string &action : actions)
            if (action == verb)
                return true;
        return false;
    }
    // Exporters that predate the verb list.
    if (type == "button")
        return verb == "press";
    if (type == "choice")
        return verb == "select";
    if (type == "check")
        return verb == "toggle";
    if (type == "edit")
        return verb == "edit";
    if (type == "stepper")
        return verb == "left" || verb == "right";
    return false;
}

std::string Widget::CellText(int column) const
{
    for (const Cell &cell : cells)
        if (cell.column == column)
            return cell.text;
    size_t at = 0;
    for (int i = 0; i < column; ++i)
    {
        at = label.find('\t', at);
        if (at == std::string::npos)
            return {};
        ++at;
    }
    const size_t end = label.find('\t', at);
    return label.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

const ImageRef *Widget::FirstImage() const
{
    if (!images.empty())
        return &images.front();
    for (const Cell &cell : cells)
        if (!cell.images.empty())
            return &cell.images.front();
    return nullptr;
}

const Widget *InterfaceModel::Find(std::string_view node, std::string_view type) const
{
    for (const Widget &widget : widgets)
        if (SameNode(widget.node, node) && (type.empty() || widget.type == type))
            return &widget;
    return nullptr;
}

bool ParseInterfaceModel(std::string_view json, InterfaceModel &out, std::string *error)
{
    const Json root = Json::parse(json.begin(), json.end(), nullptr, false);
    const auto widgets = root.is_object() ? root.find("widgets") : root.end();
    if (!root.is_object() || widgets == root.end() || !widgets->is_array())
    {
        if (error)
            *error = "interface model: not an object with widgets";
        return false;
    }
    Reader read;
    out = InterfaceModel();
    out.screen = read.Text(root, "screen", "");
    out.language = read.Text(root, "language", "");
    out.opening = read.Num<uint64_t>(root, "screenRevision", 0, "");
    out.revision = read.Num<uint64_t>(root, "revision", 0, "");
    for (size_t i = 0; i < widgets->size(); ++i)
    {
        const Json &entry = (*widgets)[i];
        const std::string at = "widgets[" + std::to_string(i) + "]";
        if (!entry.is_object())
        {
            read.Wrong(at, "", "an object");
            continue;
        }
        Widget widget;
        widget.node = read.Text(entry, "node", at);
        widget.type = read.Text(entry, "type", at);
        widget.label = read.Text(entry, "label", at);
        widget.value = read.Text(entry, "value", at);
        widget.index = read.Num<int>(entry, "index", -1, at);
        widget.order = static_cast<int>(out.widgets.size()); // the engine sends them sorted
        widget.enabled = read.Flag(entry, "enabled", true, at);
        widget.row_id = read.Text(entry, "rowId", at);
        widget.images = read.Images(entry, at);
        if (const Json *cells = read.Find(entry, "cells", true, at))
            widget.cells = read.Cells(*cells, at + ".cells");
        if (const Json *actions = read.Find(entry, "actions", true, at))
            for (const Json &verb : *actions)
                if (verb.is_string())
                    widget.actions.push_back(verb.get<std::string>());
                else
                    read.Wrong(at, "actions", "strings");
        widget.min = read.Num<int>(entry, "min", 0, at);
        widget.max = read.Num<int>(entry, "max", 0, at);
        widget.step = read.Num<int>(entry, "step", 1, at);
        widget.exclusive = read.Flag(entry, "exclusive", false, at);
        widget.unavailable_reason = read.Text(entry, "unavailableReason", at);
        if (const Json *headers = read.Find(entry, "columnHeaders", true, at))
            for (const Json &header : *headers)
                if (header.is_string())
                    widget.column_headers.push_back(header.get<std::string>());
                else
                    read.Wrong(at, "columnHeaders", "strings");
        if (const Json *cells = read.Find(entry, "headerCells", true, at))
            widget.header_cells = read.Cells(*cells, at + ".headerCells");
        if (const Json *context = read.Find(entry, "context", false, at))
            for (const auto &item : context->items())
                if (item.value().is_string())
                    widget.context[item.key()] = item.value().get<std::string>();
                else
                    read.Wrong(at + ".context", item.key(), "a string");
        if (const auto color = entry.find("textColorArgb"); color != entry.end())
        {
            if (color->is_number())
                widget.text_color_argb = color->get<uint32_t>();
            else
                read.Wrong(at, "textColorArgb", "a number");
        }
        out.widgets.push_back(std::move(widget));
    }
    if (!read.wrong.empty())
    {
        if (error)
            *error = "interface model: " + read.wrong;
        return false;
    }
    return true;
}

} // namespace storm::ui
