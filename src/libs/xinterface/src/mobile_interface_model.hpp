#pragma once
// The open interface's collected widgets as the UI's model (storm::ui::InterfaceModel): the UI
// host lives in this process and takes the struct itself (platform/android_ui_sink.hpp), so the
// collected list is moved into it field by field; nothing is serialised. XINTERFACE builds one
// only when the collected widgets differ from the last ones (MobileWidget::operator==).
#include "mobile_widget.hpp"
#include "storm_ui/interface_model.h"
#include "utf8.h"

#include <string>
#include <utility>
#include <vector>

namespace storm::mobile
{
// Text the game gave in a broken UTF-8 encoding: every byte that does not start a valid sequence
// becomes U+FFFD. True when it changed something (the caller reports it: never a quiet repair).
inline bool RepairUtf8(std::string &text)
{
    if (utf8::IsValidUtf8(text))
        return false;
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (length && i + length <= text.size() && utf8::IsValidUtf8(text.substr(i, length)))
        {
            out.append(text, i, length);
            i += length;
        }
        else
        {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    text = std::move(out);
    return true;
}

// The UI's image reference. A source whose kind lacks what that kind needs (an atlas cell
// without group or name, a battle icon without its atlas, ...) is not a source: the image is then
// only its handle, as the picture registry has it.
inline storm::ui::ImageRef MobileModelImage(const ImageRef &image)
{
    storm::ui::ImageRef out;
    out.handle = static_cast<int64_t>(image.handle);
    out.revision = static_cast<int64_t>(image.imageRevision);
    out.width = static_cast<int>(image.width);
    out.height = static_cast<int>(image.height);
    out.tint_argb = image.tintArgb;
    const auto &source = image.source;
    if (source.kind == "atlas" && !source.group.empty() && !source.image.empty())
    {
        out.kind = source.kind;
        out.group = source.group;
        out.image = source.image;
    }
    else if (source.kind == "portrait" && !source.image.empty())
    {
        out.kind = source.kind;
        out.image = source.image;
    }
    else if (source.kind == "battle_icon" && !source.mode.empty() && !source.atlas.empty() && source.pic >= 0)
    {
        out.kind = source.kind;
        out.mode = source.mode;
        out.atlas = source.atlas;
        out.pic = source.pic;
    }
    else if (source.kind == "texture" && !source.image.empty())
    {
        out.kind = source.kind;
        out.image = source.image;
        for (int i = 0; i < 4; ++i)
            out.uv[i] = source.uv[i];
    }
    return out;
}

inline std::vector<storm::ui::ImageRef> MobileModelImages(const std::vector<ImageRef> &images)
{
    std::vector<storm::ui::ImageRef> out;
    out.reserve(images.size());
    for (const ImageRef &image : images)
        out.push_back(MobileModelImage(image));
    return out;
}

inline std::vector<storm::ui::Cell> MobileModelCells(std::vector<MobileCell> &&cells)
{
    std::vector<storm::ui::Cell> out;
    out.reserve(cells.size());
    for (MobileCell &cell : cells)
        out.push_back({cell.column, std::move(cell.text), cell.colorArgb, MobileModelImages(cell.images)});
    return out;
}

// The model of the open interface `screen` (opening `opening`) from its collected widgets in
// reading order: `order` is the position. Slider bounds belong to sliders and `exclusive` to
// checks only (other types keep the defaults). Nodes with text in a broken encoding are added to
// `repaired` (their text repaired, see RepairUtf8).
inline storm::ui::InterfaceModel MobileInterfaceModel(std::string screen, std::string language, uint64_t opening, uint64_t revision,
                                                      std::vector<MobileWidget> widgets, std::vector<std::string> *repaired = nullptr)
{
    storm::ui::InterfaceModel model;
    model.screen = std::move(screen);
    model.language = std::move(language);
    model.opening = opening;
    model.revision = revision;
    model.widgets.reserve(widgets.size());
    for (MobileWidget &from : widgets)
    {
        storm::ui::Widget to;
        to.node = std::move(from.node);
        to.type = std::move(from.type);
        to.label = std::move(from.label);
        to.value = std::move(from.value);
        to.index = from.index;
        to.order = static_cast<int>(model.widgets.size());
        to.enabled = from.enabled;
        to.row_id = std::move(from.rowId);
        to.cells = MobileModelCells(std::move(from.cells));
        to.images = MobileModelImages(from.images);
        to.actions = std::move(from.actions);
        if (to.type == "slider")
        {
            to.min = from.min;
            to.max = from.max;
            to.step = from.step;
        }
        if (to.type == "check")
            to.exclusive = from.exclusive;
        to.unavailable_reason = std::move(from.unavailableReason);
        to.column_headers = std::move(from.columnHeaders);
        if (from.headerCells)
            to.header_cells = MobileModelCells(std::move(*from.headerCells));
        to.context = std::move(from.context);
        to.text_color_argb = from.textColorArgb;
        bool broken = false;
        const auto repair = [&broken](std::string &text) { broken = RepairUtf8(text) || broken; };
        for (std::string *text : {&to.label, &to.value, &to.row_id, &to.unavailable_reason})
            repair(*text);
        for (std::string &header : to.column_headers)
            repair(header);
        for (auto &[key, value] : to.context)
            repair(value);
        for (storm::ui::Cell &cell : to.cells)
            repair(cell.text);
        if (to.header_cells)
            for (storm::ui::Cell &cell : *to.header_cells)
                repair(cell.text);
        if (broken && repaired)
            repaired->push_back(to.node);
        model.widgets.push_back(std::move(to));
    }
    return model;
}
} // namespace storm::mobile
