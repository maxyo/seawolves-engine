#pragma once
// Host checks only: the collected widgets (MobileWidget) as JSON lines for the checks' Python
// assertions. The engine hands the UI the struct itself (xinterface mobile_interface_model.hpp);
// this is the wire the checks were written against, kept for their readable output.
#include "mobile_widget.hpp"

#include <string>
#include <vector>

inline std::string MobileJsonString(const std::string &text)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
            result += static_cast<char>(c);
        }
        else if (c < 32)
        {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        }
        else
            result += static_cast<char>(c);
    }
    return result + '"';
}

inline std::string MobileJsonNumber(float value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}
inline std::string MobileImageJson(const storm::mobile::ImageRef &image)
{
    std::string json = "{\"handle\":" + std::to_string(image.handle) + ",\"imageRevision\":" + std::to_string(image.imageRevision) +
           ",\"width\":" + std::to_string(image.width) + ",\"height\":" + std::to_string(image.height) +
           ",\"tintArgb\":" + std::to_string(image.tintArgb);
    const auto &source = image.source;
    if (source.kind == "atlas" && !source.group.empty() && !source.image.empty())
        json += ",\"source\":{\"kind\":\"atlas\",\"group\":" + MobileJsonString(source.group) +
                ",\"image\":" + MobileJsonString(source.image) + '}';
    else if (source.kind == "portrait" && !source.image.empty())
        json += ",\"source\":{\"kind\":\"portrait\",\"image\":" + MobileJsonString(source.image) + '}';
    else if (source.kind == "battle_icon" && !source.mode.empty() && !source.atlas.empty() && source.pic >= 0)
        json += ",\"source\":{\"kind\":\"battle_icon\",\"mode\":" + MobileJsonString(source.mode) +
                ",\"atlas\":" + MobileJsonString(source.atlas) + ",\"pic\":" + std::to_string(source.pic) + '}';
    else if (source.kind == "texture" && !source.image.empty())
        json += ",\"source\":{\"kind\":\"texture\",\"image\":" + MobileJsonString(source.image) + ",\"uv\":[" +
                MobileJsonNumber(source.uv[0]) + ',' + MobileJsonNumber(source.uv[1]) + ',' + MobileJsonNumber(source.uv[2]) + ',' +
                MobileJsonNumber(source.uv[3]) + "]}";
    return json + '}';
}
inline std::string MobileImagesJson(const std::vector<storm::mobile::ImageRef> &images)
{
    std::string out = "[";
    for (const auto &i : images)
    {
        if (out.size() > 1)
            out += ',';
        out += MobileImageJson(i);
    }
    return out + ']';
}
inline std::string MobileCellsJson(const std::vector<MobileCell> &cells)
{
    std::string result = "[";
    bool first = true;
    for (const auto &c : cells)
    {
        if (!first) result += ',';
        first = false;
        result += "{\"column\":" + std::to_string(c.column) + ",\"text\":" + MobileJsonString(c.text) +
                  ",\"colorArgb\":" + std::to_string(c.colorArgb) + ",\"images\":" + MobileImagesJson(c.images) + '}';
    }
    return result + ']';
}
inline std::string MobileWidgetJson(const std::string &screen, const std::vector<MobileWidget> &widgets,
                                    uint64_t screenRevision = 0, uint64_t revision = 0,
                                    const std::string &language = {})
{
    std::string result = "{\"schemaVersion\":2,\"language\":" + MobileJsonString(language) +
                         ",\"revision\":" + std::to_string(revision) +
                         ",\"screenRevision\":" + std::to_string(screenRevision) +
                         ",\"screen\":" + MobileJsonString(screen) + ",\"widgets\":[";
    bool first = true;
    for (const auto &widget : widgets)
    {
        if (!first)
            result += ',';
        first = false;
        result += "{\"node\":" + MobileJsonString(widget.node) + ",\"type\":" + MobileJsonString(widget.type) +
                  ",\"label\":" + MobileJsonString(widget.label) + ",\"value\":" + MobileJsonString(widget.value) +
                  ",\"index\":" + std::to_string(widget.index) + ",\"enabled\":" + (widget.enabled ? "true" : "false") +
                  ",\"rowId\":" + MobileJsonString(widget.rowId) + ",\"images\":" + MobileImagesJson(widget.images) +
                  ",\"actions\":[";
        bool firstAction = true;
        for (const auto &a : widget.actions)
        {
            if (!firstAction)
                result += ',';
            firstAction = false;
            result += MobileJsonString(a);
        }
        result += "]";
        if (widget.type == "slider")
            result += ",\"min\":" + std::to_string(widget.min) + ",\"max\":" + std::to_string(widget.max) +
                      ",\"step\":" + std::to_string(widget.step);
        if (widget.type == "check")
            result += std::string(",\"exclusive\":") + (widget.exclusive ? "true" : "false");
        if (!widget.unavailableReason.empty())
            result += ",\"unavailableReason\":" + MobileJsonString(widget.unavailableReason);
        if (!widget.columnHeaders.empty())
        {
            result += ",\"columnHeaders\":[";
            for (size_t column = 0; column < widget.columnHeaders.size(); ++column)
            {
                if (column) result += ',';
                result += MobileJsonString(widget.columnHeaders[column]);
            }
            result += ']';
        }
        if (widget.headerCells)
            result += ",\"headerCells\":" + MobileCellsJson(*widget.headerCells);
        if(widget.textColorArgb)result+=",\"textColorArgb\":"+std::to_string(*widget.textColorArgb);
        if(!widget.context.empty()){
            result+=",\"context\":{";bool firstEntry=true;
            for(const auto &[key,value]:widget.context){if(!firstEntry)result+=',';firstEntry=false;result+=MobileJsonString(key)+":"+MobileJsonString(value);}result+='}';
        }
        result += ",\"cells\":" + MobileCellsJson(widget.cells) + '}';
    }
    return result + "]}";
}
