#pragma once
#include "mobile_image_cache.hpp"
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <limits>
#include <optional>
#include <map>

struct MobileCell
{
    int column = 0;
    std::string text;
    uint32_t colorArgb = 0xffffffff;
    std::vector<storm::mobile::ImageRef> images;
    bool operator==(const MobileCell &) const = default;
};

// What a node's MobileCollect reports for one control (or table row). XINTERFACE turns the
// collected list into the UI's storm::ui::InterfaceModel (mobile_interface_model.hpp).
struct MobileWidget
{
    std::string node, type, label, value;
    int index = -1;
    int order = 0;
    bool enabled = true;
    std::string rowId;
    std::vector<MobileCell> cells;
    std::vector<storm::mobile::ImageRef> images;
    std::vector<std::string> actions;
    // Optional schema-2 capabilities, serialized only for their control type.
    int min = 0, max = 0, step = 1;
    bool exclusive = false;
    std::string unavailableReason;
    // Table hr.tdN.str, indexed by MobileCell::column; absent without hr.
    std::vector<std::string> columnHeaders;
    // Present only with hr; existing tdN cells retain their explicit column, including gaps.
    std::optional<std::vector<MobileCell>> headerCells;
    // Optional bounded interface context; values are native facts, not app rules.
    std::map<std::string,std::string> context;
    std::optional<uint32_t> textColorArgb;
    bool operator==(const MobileWidget &) const = default;
};

inline bool MobileSliderPosition(const std::string &text, int minimum, int maximum, int &position)
{
    if (text.empty() || minimum < 0 || maximum < minimum) return false;
    int parsed = 0;
    for (const unsigned char ch : text)
    {
        if (ch < '0' || ch > '9' || parsed > (std::numeric_limits<int>::max() - (ch - '0')) / 10)
            return false;
        parsed = parsed * 10 + (ch - '0');
    }
    if (parsed < minimum || parsed > maximum) return false;
    position = parsed;
    return true;
}

inline bool MobileSliderSetPosition(const std::string &action, uint64_t screenRevision, int &position)
{
    const std::string prefix = "set@" + std::to_string(screenRevision) + ":";
    return action.compare(0, prefix.size(), prefix) == 0 &&
           MobileSliderPosition(action.substr(prefix.size()), 0, std::numeric_limits<int>::max(), position);
}

// The face of a game portrait texture ("interfaces\\portraits\\256\\face_12.tga" → "12"), "" for
// any other picture: the portable UI draws portraits by face, not by path.
inline std::string MobilePortraitFace(const std::string &path)
{
    std::string lower;
    for (const char c : path) lower += c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t portraits = lower.find("portraits/"), face = lower.rfind("/face_");
    if (portraits == std::string::npos || face == std::string::npos || face < portraits) return {};
    std::string id;
    for (size_t i = face + 6; i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i])); ++i) id += lower[i];
    return id;
}
// The texture-namespace name of a game texture ("Interfaces\\Maps\\Kayman.tga" →
// "interfaces/maps/kayman"): no resource prefix, no extension; "" for a path outside the namespace.
inline std::string MobileTextureName(const std::string &path)
{
    std::string name = storm::mobile::TexturePath(path);
    const std::string prefix = "resource/textures/";
    if (name.compare(0, prefix.size(), prefix) != 0)
        return {};
    name.erase(0, prefix.size());
    if (name.rfind("textures/", 0) == 0) // a path given from the resource root ("Textures\\…")
        name.erase(0, 9);
    for (const char *extension : {".tx", ".tga"})
    {
        const size_t length = std::char_traits<char>::length(extension);
        if (name.size() > length && name.compare(name.size() - length, length, extension) == 0)
            name.erase(name.size() - length);
    }
    return name;
}
inline std::string MobileNumber(float value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}
