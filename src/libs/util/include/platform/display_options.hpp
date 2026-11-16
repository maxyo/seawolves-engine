#pragma once
#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

// The player's display options (settings screen «Управление» → «Экран и касания»), app settings of the "appSettings"
// state channel: «Размер интерфейса» ui_scale (percent of the display's density the UI lays out at) and «Прозрачность
// кнопок» hud_transparency (percent the HUD's buttons are see-through beyond their design). The app's GraphicsSettings
// keeps the same steps and defaults; the engine's UI host (ui_host engine_ui.cpp) applies them (Host::SetDisplayOptions).
namespace storm::android
{
// 100 % is the smallest: the screens are designed to 48 dp targets there.
inline constexpr int kUiScales[] = {100, 115, 130};
inline constexpr int kUiScaleDefault = 100;
inline constexpr int kHudTransparencies[] = {0, 35, 60};
inline constexpr int kHudTransparencyDefault = 0;

template <size_t N> bool OneOf(const int (&steps)[N], int value)
{
    return std::find(std::begin(steps), std::end(steps), value) != std::end(steps);
}

// An integer member of the channel's flat JSON object ("key": 115); nullopt when absent or not an integer.
inline std::optional<int> FlatJsonInt(std::string_view json, std::string_view key)
{
    const std::string quoted = "\"" + std::string(key) + "\"";
    size_t at = json.find(quoted);
    if (at == std::string_view::npos)
        return std::nullopt;
    at += quoted.size();
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])))
        ++at;
    if (at >= json.size() || json[at] != ':')
        return std::nullopt;
    ++at;
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])))
        ++at;
    const size_t start = at;
    if (at < json.size() && json[at] == '-')
        ++at;
    while (at < json.size() && std::isdigit(static_cast<unsigned char>(json[at])))
        ++at;
    if (at == start || (at == start + 1 && json[start] == '-') || (at < json.size() && (json[at] == '.' || json[at] == 'e')))
        return std::nullopt;
    return std::stoi(std::string(json.substr(start, at - start)));
}

struct DisplayOptions
{
    int ui_scale = kUiScaleDefault;
    int hud_transparency = kHudTransparencyDefault;
    std::string problems; // what was missing or invalid and replaced by its default (empty: none)
};

inline DisplayOptions ReadDisplayOptions(std::string_view app_settings)
{
    DisplayOptions options;
    const auto take = [&](const char *key, const auto &steps, int fallback, int &out) {
        const std::optional<int> value = FlatJsonInt(app_settings, key);
        if (value && OneOf(steps, *value))
            out = *value;
        else
        {
            out = fallback;
            options.problems += std::string(options.problems.empty() ? "" : "; ") + key + " " +
                                (value ? std::to_string(*value) + " is not a step" : std::string("is missing")) + ", default " +
                                std::to_string(fallback);
        }
    };
    take("ui_scale", kUiScales, kUiScaleDefault, options.ui_scale);
    take("hud_transparency", kHudTransparencies, kHudTransparencyDefault, options.hud_transparency);
    return options;
}
} // namespace storm::android
