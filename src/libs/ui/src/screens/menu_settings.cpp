// Settings (interface "option_screen"): three pages on a rail, each in columns of C-Setting rows. «Игра»: «Частота пиратов на карте» (the app's pirate_frequency, platform/pirate_frequency.hpp),
// the game's Директсейл, advanced navigation, boarding autosave and its marks. «Управление»: one «Чувствительность»
// for the game's two mouse sensitivities (the touch camera drag goes through them), «Инверсия по вертикали», the sea
// camera, the camera turn at doors. «Звук и графика»: the game's volumes; the quality preset «Низкая / Средняя /
// Высокая» (engine renderer_bgfx fx_preset 0/1/2, applied at once; «Низкая» chosen by the automatic preset at a lower
// scene height shows that height under it), which also decides the game's grass and sea detail (renderer
// scene_detail.hpp), the frame rate «Частота кадров» among the rates the host reports for the display, and the
// texture quality (at the next start). A debuggable app adds «Отладка», its single fx overrides. The application's
// options come from the platform host on the state channel "appSettings", are kept as a draft and go back with
// «Готово» (Bindings::SendHost). The game applies its own controls as they change and keeps or drops them with
// BTN_OK / BTN_CANCEL: «Готово» is BTN_OK, ← is BTN_CANCEL, and there is no unsaved-changes question.
// Fills the pack screen <s-settings>.
#include "controls.h"

#include <platform/display_options.hpp>
#include <platform/pirate_frequency.hpp>

namespace storm::ui::screens
{

namespace
{

constexpr NodeRule kNodes[] = {
    {"BTN_OK", "button", true},
    {"BTN_CANCEL", "button", true},
    {"MUSIC_SLIDE", "slider"},
    {"SOUND_SLIDE", "slider"},
    {"DIALOG_SLIDE", "slider"},
    {"VMOUSE_SENSITIVITY_SLIDE", "slider"},
    {"HMOUSE_SENSITIVITY_SLIDE", "slider"},
    {"SEACAMPERSP_SLIDE", "slider"},
    {"AUTOSAVE_CHECKBOX", "check"},
    {"DIRECTSAIL_CHECKBOX", "check"},
    {"ENHANCEDSAILING_CHECKBOX", "check"},
    {"INVERT_MOUSE_CHECKBOX", "check"},
    {"CAMERASWING_CHECKBOX", "check"},
    {"QUESTMARK_CHECKBOX", "check"},
    {"SHIPMARK_CHECKBOX", "check"},
    {"BATTLE_MODE_CHECKBOX", "check"},
    // Not offered: colour correction (unavailable in the Android renderer), keyboard
    // bindings and their defaults (mobile only), the game's effects the quality preset decides (glow, sky
    // rotation, dynamic lights, simple sea, crew on deck, grass and sea detail: renderer scene_detail.hpp),
    // «Всегда бегом» (the move stick's travel decides walk or run), the game's HUD scale (the portable HUD
    // replaces the game's), the game's own captions and pictures of its layout.
    {"HERB_CHECKBOX", "check"},
    {"SEA_DETAILS_SLIDE", "slider"},
    {"ALWAYS_RUN_CHECKBOX", "check"},
    {"HUD_SLIDE", "slider"},
    {"HUD_DESCRIP_TEXT", "text"},
    {"BTN_CONTROLS_DEFAULT", "button"},
    {"GLOW_SLIDE", "slider"},
    {"ROTATESKY_CHECKBOX", "check"},
    {"DYNAMICLIGHTS_CHECKBOX", "check"},
    {"SIMPLESEA_CHECKBOX", "check"},
    {"CREWONDECK_CHECKBOX", "check"},
    {"GAMMA_SLIDE", "slider"},
    {"BRIGHT_SLIDE", "slider"},
    {"CONTRAST_SLIDE", "slider"},
    {"CONTROLS_LIST", "choice"},
    {"CHANGEKEY_*", "*"},
    {"KEY_CHOOSER", "*"},
    {"TABBTN_*", "image|button"},
    {"TABSTR_*", "text"},
    {"TITLE", "heading|text"},
    {"TITLES_STR", "text"},
    {"TITLES_IMAGES", "image"},
    {"HARDWARE_SETTINGS", "text"},
    {"SOFTWARE_SETTINGS", "text"},
    {"MAIN_BACK", "image"},
};

// «Игра», «Управление», «Звук и графика»; a debuggable app adds the untranslated «Отладка» (TabCount).
const char *const kTabs[] = {StringKey("settings.tab_game"), StringKey("settings.tab_controls"), StringKey("settings.tab_sound_graphics")};

// The application's renderer options as the host last reported them.
Json &AppSettings()
{
    static Json settings;
    return settings;
}

// What the renderer reports about the device (UI channel "renderCaps"): {"upscale": "supported" | "unsupported" |
// "unknown"}, the texture quality «Ультра».
Json &RenderCaps()
{
    static Json caps = Json::object();
    return caps;
}

class RenderCapsConsumer final : public StateConsumer
{
  public:
    const char *Channel() const override
    {
        return "renderCaps";
    }
    void OnState(const Json &state, Router &) override
    {
        RenderCaps() = state.is_object() ? state : Json::object();
    }
};

class AppSettingsConsumer final : public StateConsumer
{
  public:
    const char *Channel() const override
    {
        return "appSettings";
    }
    void OnState(const Json &state, Router &router) override
    {
        AppSettings() = state;
        router.RebuildActive(); // the cache facts of an open settings screen are the host's latest
    }
};

struct Column
{
    std::string title;
    Json rows = Json::array();
};

class SettingsScreen final : public Screen
{
  public:
    const char *Component() const override
    {
        return "s-settings";
    }
    std::span<const NodeRule> Nodes() const override
    {
        return kNodes;
    }

    Json Build(const Admission &admitted, Bindings &bindings) override
    {
        if (!draft_.is_object())
        {
            draft_ = AppSettings();
            bindings.SendHost("app-cache-measure", ""); // the cache grows while the game runs: its size as of now
        }

        // One game control (or the radio group of a node) as a row; nothing when absent.
        const auto game = [&admitted, &bindings](Column &column, const char *node, const char *label = nullptr) {
            const std::vector<const Widget *> widgets = admitted.All(node);
            if (widgets.empty())
                return;
            const std::vector<std::string> labels = label ? std::vector<std::string>{bindings.Text(label)} : std::vector<std::string>();
            ControlRows(bindings, widgets, std::string(node) + "-", labels, column.rows);
        };

        std::vector<std::pair<std::string, std::vector<Column>>> sections;
        const int tabs_count = TabCount();
        tab_ = std::clamp(tab_, 0, tabs_count - 1);
        if (tab_ == 0)
        {
            // «Игра»: the game's rules at sea and on the map, and its marks.
            Column sea{bindings.Text("settings.sea_and_map")}, markers{bindings.Text("settings.markers")};
            // «Частота пиратов на карте» (platform/pirate_frequency.hpp): an app setting the game's encounter script
            // asks for at every random encounter, so it counts from the next one after «Готово».
            Segment(bindings, sea, "pirate_frequency", "settings.pirate_frequency",
                    std::vector<int>(std::begin(storm::game::kPirateFrequencies), std::end(storm::game::kPirateFrequencies)),
                    {bindings.Text("settings.pirates_rare"), bindings.Text("settings.pirates_sometimes"), bindings.Text("settings.pirates_often")},
                    false);
            game(sea, "DIRECTSAIL_CHECKBOX");
            game(sea, "ENHANCEDSAILING_CHECKBOX");
            game(sea, "AUTOSAVE_CHECKBOX");
            game(markers, "QUESTMARK_CHECKBOX");
            game(markers, "SHIPMARK_CHECKBOX");
            game(markers, "BATTLE_MODE_CHECKBOX");
            sections.push_back({"", {sea, markers}});
        }
        else if (tab_ == 1)
        {
            // «Управление»: the camera (the touch drag goes through the game's mouse sensitivity and inversion).
            Column camera{bindings.Text("settings.camera")};
            Sensitivity(admitted, bindings, camera);
            game(camera, "INVERT_MOUSE_CHECKBOX");
            if (!camera.rows.empty() && camera.rows.back()["id"] == "INVERT_MOUSE_CHECKBOX-0")
                camera.rows.back()["label"] = bindings.Text("settings.invert_vertical");
            game(camera, "SEACAMPERSP_SLIDE", "settings.sea_camera");
            game(camera, "CAMERASWING_CHECKBOX");
            if (!camera.rows.empty() && camera.rows.back()["id"] == "CAMERASWING_CHECKBOX-0")
                camera.rows.back()["label"] = bindings.Text("settings.door_camera_turn"); // the game's «Поворот камеры» reads as any turn
            // «Экран и касания»: the app's display options (platform/display_options.hpp), applied with «Готово». The UI
            // sizes are the ones the host found the display can fit (ui_scale_options: the designed layout height left).
            Column screen{bindings.Text("settings.screen_touch")};
            std::vector<int> scales;
            Json scaleLabels = Json::array(), transparencies = Json::array();
            if (draft_.is_object() && draft_.contains("ui_scale_options") && draft_["ui_scale_options"].is_array())
                for (const Json &scale : draft_["ui_scale_options"])
                    if (scale.is_number_integer() && storm::android::OneOf(storm::android::kUiScales, scale.get<int>()))
                    {
                        scales.push_back(scale.get<int>());
                        scaleLabels.push_back(std::to_string(scales.back()) + "%");
                    }
            for (const int share : storm::android::kHudTransparencies)
                transparencies.push_back(share == 0 ? bindings.Text("settings.transparency_none") : std::to_string(share) + "%");
            Segment(bindings, screen, "ui_scale", "settings.ui_scale", scales, scaleLabels, false);
            // A display with room for no larger size than 100 %: the row stays, disabled, and a tap says why.
            if (scales.size() < 2 && Known("ui_scale"))
            {
                screen.rows.back()["disabled"] = true;
                screen.rows.back()["reason"] = bindings.Text("settings.ui_scale_no_room");
            }
            Segment(bindings, screen, "hud_transparency", "settings.hud_transparency",
                    std::vector<int>(std::begin(storm::android::kHudTransparencies), std::end(storm::android::kHudTransparencies)),
                    transparencies, false);
            Toggle(bindings, screen, "left_handed", "settings.left_handed"); // the HUD mirrored left to right
            Toggle(bindings, screen, "haptics", "settings.haptics"); // «Вибрация»: hits, broadsides, boarding (android_haptics.hpp)
            sections.push_back({"", {camera, screen}});
        }
        else if (tab_ == 2)
        {
            // «Звук и графика». The graphics choice is the preset; it also decides the game's grass and sea detail
            // (renderer scene_detail.hpp). The graphics API in use is shown in «О программе».
            Column sound{bindings.Text("settings.volume")}, graphics{bindings.Text("settings.graphics")};
            game(sound, "MUSIC_SLIDE", "settings.music");
            game(sound, "SOUND_SLIDE", "settings.sound_effects");
            game(sound, "DIALOG_SLIDE", "settings.voices");
            // The preset's three names stand right under the column's title «Графика»: an empty label (every row
            // carries every field the template reads; strict host renders refuse a missing one).
            Segment(bindings, graphics, "fx_preset", "settings.graphics", {0, 1, 2},
                    {bindings.Text("settings.fx_low"), bindings.Text("settings.fx_medium"), bindings.Text("settings.fx_high")}, false);
            graphics.rows.back()["label"] = "";
            // The automatic preset's rung below «Низкая»: «Низкая» with the scene at a lower height, which only the
            // measurement chooses (fx_auto_render_height, while that choice stands); its height under the row.
            if (Known("fx_auto_render_height") && draft_["fx_auto_render_height"].get<int>() > 0 && Known("fx_preset") &&
                draft_["fx_preset"].get<int>() == 0)
                graphics.rows.back()["hint"] =
                    Fill(bindings.Text("settings.fx_scene_height"), {std::to_string(draft_["fx_auto_render_height"].get<int>())});
            // «Частота кадров»: the rates the host found the display can show (frame_rate_options: whole divisors
            // of its refresh rates), applied at once with «Готово» (the engine's frame pacing).
            std::vector<int> rates;
            Json rateLabels = Json::array();
            if (draft_.is_object() && draft_.contains("frame_rate_options") && draft_["frame_rate_options"].is_array())
                for (const Json &rate : draft_["frame_rate_options"])
                    if (rate.is_number_integer())
                    {
                        rates.push_back(rate.get<int>());
                        rateLabels.push_back(std::to_string(rates.back()));
                    }
            Segment(bindings, graphics, "frame_rate", "settings.frame_rate", rates, rateLabels, false);
            // «Качество текстур» (fx::TextureQuality): Низкое / Среднее / Высокое change the textures' resolution at the
            // next start (marked only when the choice changes it); «Ультра» is Высокое with the neural ×2, at once,
            // offered where the renderer reports the device can show it (channel "renderCaps"), else disabled with why.
            Json levels = Json::array({bindings.Text("settings.texture_low"), bindings.Text("settings.texture_medium"),
                                       bindings.Text("settings.texture_high")});
            const std::string upscale = RenderCaps().value("upscale", std::string("unknown"));
            if (upscale == "supported")
                levels.push_back(bindings.Text("settings.texture_ultra"));
            else
                levels.push_back(Json{{"key", ""}, // an option object names its text by key or, empty, by label
                                      {"label", bindings.Text("settings.texture_ultra")},
                                      {"disabled", true},
                                      {"reason", bindings.Text(upscale == "unsupported" ? "settings.texture_ultra_unsupported"
                                                                                        : "settings.texture_ultra_unknown")}});
            const auto resolution = [](int level) { return level >= 2 ? 0 : 2 - level; }; // fx::TextureDegradation
            const bool restart = Known("texture_quality") && AppSettings().is_object() && AppSettings().contains("texture_quality") &&
                                 resolution(draft_["texture_quality"].get<int>()) != resolution(AppSettings()["texture_quality"].get<int>());
            Segment(bindings, graphics, "texture_quality", "settings.texture_quality", {0, 1, 2, 3}, levels, restart);
            CacheRow(bindings, graphics);
            sections.push_back({"", {sound, graphics}});
        }
        else
        {
            // «Отладка», only when a debuggable app reports single fx overrides (GraphicsSettings.DEBUG_KEYS): rows to
            // bisect rendering problems on a phone, applied with «Готово» like the preset. Development only, so the
            // texts are not translated.
            Column debug{"Отладка эффектов"};
            const Json onOff = Json::array({"пресет", "выкл", "вкл"});
            DebugSegment(bindings, debug, "fx_msaa", "MSAA", {-1, 1, 2, 4}, {"пресет", "выкл", "2x", "4x"});
            DebugSegment(bindings, debug, "fx_bloom", "Свечение (bloom)", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_grade", "Цветокоррекция", {-1, 0, 1, 2}, {"пресет", "выкл", "Карибы", "Пасмурно"});
            DebugSegment(bindings, debug, "fx_dither", "Дизеринг", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_shadows", "Тени солнца", {-1, 0, 1, 2}, {"пресет", "выкл", "низк", "мягк"});
            DebugSegment(bindings, debug, "fx_sea_band_limit", "Море: мягкая даль", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_sea_soft_edges", "Море: мягкие края", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_sea_specular", "Море: блик солнца", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_light_per_pixel", "Освещение попиксельно", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_light_point", "Точечные огни", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_light_specular", "Блики", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_light_normal_map", "Карты нормалей", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_alpha_to_coverage", "Alpha to coverage", {-1, 0, 1}, onOff);
            DebugSegment(bindings, debug, "fx_anisotropy", "Анизотропия", {-1, 1, 4, 8}, {"пресет", "выкл", "4x", "8x"});
            DebugSegment(bindings, debug, "fx_ao_volume", "Затенение (объём)", {-1, 0, 1, 2, 3}, {"пресет", "выкл", "игра", "умер", "сильн"});
            // FSR 1 and SMAA: the scene's internal height (native on displays not taller),
            // the post AA, RCAS;
            // «Замер GPU» logs GPU timer averages, which «Поделиться логом» carries out.
            if (Known("fx_render_height"))
            {
                DebugSegment(bindings, debug, "fx_render_height", "Высота сцены", {-1, 720, 900, 1000, 1100, 0},
                             {"пресет", "720", "900", "1000", "1100", "экран"});
                DebugSegment(bindings, debug, "fx_smaa", "SMAA", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "fx_sharpen", "Резкость (RCAS)", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "fx_gpu_profile", "Замер GPU", {-1, 1}, {"выкл", "вкл"});
            }
            // Far LODs of islands, ships and outdoor locations: the pixels a simplified level may differ by
            // (src/libs/geometry/far_lod), in tenths of a pixel; «выкл» draws every model whole.
            if (Known("fx_far_lod"))
                DebugSegment(bindings, debug, "fx_far_lod", "Дальние LOD", {-1, 0, 10, 20}, {"пресет", "выкл", "1 px", "2 px"});
            // The lamps' relight of locations: how many lamps cast shadows, the
            // shadow filter, how much brighter characters indoors are than the room, the lamp views.
            if (Known("fx_lamp_shadows"))
            {
                DebugSegment(bindings, debug, "fx_lamp_shadows", "Тени ламп", {-1, 0, 8, 16}, {"пресет", "выкл", "8", "16"});
                DebugSegment(bindings, debug, "fx_lamp_soft", "Фильтр теней ламп", {-1, 0, 1}, {"пресет", "3x3", "5x5"});
                DebugSegment(bindings, debug, "fx_lamp_model_lift", "Персонажи ярче комнаты", {-1, 0, 20, 50, 100},
                             {"пресет", "0%", "20%", "50%", "100%"});
                DebugSegment(bindings, debug, "fx_lamp_debug", "Лампы: отладка", {-1, 0, 1, 2}, {"пресет", "выкл", "цвета", "маска"});
            }
            // The share of a lamp's own colour in the light it adds to baked surfaces.
            if (Known("fx_lamp_tint"))
                DebugSegment(bindings, debug, "fx_lamp_tint", "Цвет ламп на стенах", {-1, 0, 25, 50, 100},
                             {"пресет", "0%", "25%", "50%", "100%"});
            // The one falloff of every light (fx/light_falloff.hpp): how far it reaches, its soft core, its level.
            if (Known("fx_light_reach"))
            {
                DebugSegment(bindings, debug, "fx_light_reach", "Огни: дальность", {-1, 100, 160, 220, 300},
                             {"пресет", "100%", "160%", "220%", "300%"});
                DebugSegment(bindings, debug, "fx_light_core", "Огни: мягкость у огня", {-1, 10, 40, 80},
                             {"пресет", "10 см", "40 см", "80 см"});
                DebugSegment(bindings, debug, "fx_light_level", "Огни: яркость", {-1, 50, 70, 85, 100},
                             {"пресет", "50%", "70%", "85%", "100%"});
            }
            // Sunlight through the sails: its strength.
            if (Known("fx_sail_light"))
                DebugSegment(bindings, debug, "fx_sail_light", "Свет сквозь паруса", {-1, 0, 100, 200, 300},
                             {"пресет", "выкл", "100%", "200%", "300%"});
            // Our town lightmaps for the live sun (effects/town_light.hpp) against the game's, and what they hold.
            if (Known("fx_town_light"))
            {
                DebugSegment(bindings, debug, "fx_town_light", "Свет городов", {-1, 0, 1}, {"пресет", "игра", "наш"});
                DebugSegment(bindings, debug, "fx_town_light_debug", "Свет городов: отладка", {-1, 0, 1}, onOff);
            }
            // The town walls relit per pixel.
            if (Known("fx_town_walls"))
                DebugSegment(bindings, debug, "fx_town_walls", "Стены городов", {-1, 0, 1}, {"пресет", "игра", "наши"});
            // Gloss maps (Высокая): per-texel highlights of the sun and lamps, metal in its own colour.
            if (Known("fx_gloss"))
            {
                DebugSegment(bindings, debug, "fx_gloss", "Блеск", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "fx_gloss_strength", "Сила блеска", {-1, 50, 100, 200, 300},
                             {"пресет", "50%", "100%", "200%", "300%"});
                DebugSegment(bindings, debug, "fx_gloss_debug", "Блеск: отладка", {-1, 0, 1, 2, 3, 4, 5},
                             {"пресет", "выкл", "блеск", "нормали", "солнце", "металл", "кожа"});
            }
            // The characters' relief (feat/char-relief): off, normal maps, with the parallax; its strength.
            if (Known("fx_character_relief"))
            {
                DebugSegment(bindings, debug, "fx_character_relief", "Рельеф персонажей", {-1, 0, 1, 2},
                             {"пресет", "выкл", "нормали", "параллакс"});
                DebugSegment(bindings, debug, "fx_character_relief_strength", "Сила рельефа персонажей",
                             {-1, 25, 50, 75, 100}, {"пресет", "25%", "50%", "75%", "100%"});
            }
            // Waves from objects: ripples, their lift of the surface, their views.
            if (Known("fx_sea_waves"))
            {
                DebugSegment(bindings, debug, "fx_sea_waves", "Море: рябь от объектов", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "fx_sea_waves_lift", "Море: рябь поднимает воду", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "fx_sea_waves_debug", "Море: волны, отладка", {-1, 0, 1, 2},
                             {"пресет", "выкл", "слои", "наклон"});
            }
            // The parallax probe (engine effects/parallax_probe.hpp): the relief texture under the screen centre, its
            // parallax and trust mask drawn over the picture; the second row switches that texture's parallax.
            // Settings reach the engine as stored values applied with «Готово», not as events: every change of
            // fx_pom_toggle's value is one press (the engine switches on the change, never on the value itself).
            if (Known("fx_pom_probe"))
            {
                DebugSegment(bindings, debug, "fx_pom_probe", "Прицел: текстура и параллакс", {-1, 1}, {"выкл", "вкл"});
                DebugSegment(bindings, debug, "fx_pom_toggle", "Прицел: переключить параллакс", {-1, 1}, {"А", "Б"});
            }
            // The renderer's thread, its frame overlap and the graphics API (read at the game's start).
            if (Known("render_backend"))
            {
                DebugSegment(bindings, debug, "render_backend", "Графический API", {-1, 1, 2}, {"авто", "GLES", "Vulkan"});
                DebugSegment(bindings, debug, "render_thread", "Поток рендера", {-1, 0, 1}, onOff);
                DebugSegment(bindings, debug, "render_overlap", "Кадр вперёд", {-1, 0, 1}, {"по кадру", "никогда", "всегда"});
            }
            sections.push_back({"", {debug}});
        }

        Json view = Json::object();
        view["title"] = bindings.Text("settings.title");
        Json tabs = Json::array();
        for (int i = 0; i < tabs_count; ++i)
            tabs.push_back(Json{{"label", i < static_cast<int>(std::size(kTabs)) ? bindings.Text(kTabs[i]) : std::string("Отладка")},
                                {"selected", i == tab_}});
        view["tabs"] = tabs;
        bindings.On("tabs", "select", [this, &bindings](const Event &event) {
            tab_ = std::clamp(std::atoi(event.value.c_str()), 0, TabCount() - 1);
            bindings.Rebuild();
        });
        Json pages = Json::array();
        for (const auto &[title, columns] : sections)
        {
            Json cols = Json::array();
            for (const Column &column : columns)
                cols.push_back(Json{{"title", column.title}, {"rows", column.rows}});
            pages.push_back(Json{{"title", title}, {"columns", cols}});
        }
        view["sections"] = pages;

        const Widget *ok = admitted.One("BTN_OK");
        const Widget *cancel = admitted.One("BTN_CANCEL");
        view["done"] = Json{{"label", bindings.Text("settings.done")},
                            {"disabled", !ok->Offers("press")},
                            {"reason", ok->unavailable_reason},
                            {"busy", bindings.Pending("done")}};
        bindings.On("done", "", [this, &bindings, ok](const Event &) {
            // The settings only: what the host reports about the device (frame_rate_options, ui_scale_options) and
            // its cache (cache_bytes, cache_clear_pending), whether the preset
            // was measured (fx_preset_auto: the host's automatic choice) and the measured scene height
            // (fx_auto_render_height) are not ones.
            const auto choices = [](Json settings) {
                settings.erase("frame_rate_options");
                settings.erase("ui_scale_options");
                settings.erase("cache_bytes");
                settings.erase("cache_clear_pending");
                settings.erase("fx_preset_auto");
                settings.erase("fx_auto_render_height");
                return settings;
            };
            if (draft_.is_object() && AppSettings().is_object() && choices(draft_) != choices(AppSettings()))
            {
                const Json settings = choices(draft_);
                bindings.SendHost("app-settings", settings.dump());
                AppSettings().update(settings);
            }
            bindings.Send(*ok, "press");
        });
        bindings.On("header-back", "", [&bindings, cancel](const Event &) { bindings.Back(cancel); });
        return view;
    }

  private:
    // «Кэш»: what the game derived and can make again (the host's cache_bytes: -1 while it measures) and «Очистить»,
    // which the host does at the next start (cache_clear_pending), when nothing of the game holds those files. Both
    // are facts the host reports while the screen is open, not part of the draft.
    void CacheRow(Bindings &bindings, Column &column)
    {
        const Json &facts = AppSettings();
        if (!facts.is_object() || !facts.contains("cache_bytes") || !facts["cache_bytes"].is_number_integer())
            return; // a host without the cache report: no row
        const long long bytes = facts["cache_bytes"].get<long long>();
        const bool pending = facts.value("cache_clear_pending", false);
        constexpr double kGiB = 1073741824.0; // MB below a whole GB: «118,0 МБ», not «0,1 ГБ»
        const std::string size = bytes < 0 ? bindings.Text("settings.cache_measuring")
                                           : Size(bindings, static_cast<double>(bytes), bytes < kGiB ? 0.0 : static_cast<double>(bytes));
        // Pending: the row says so in its normal colours and has no button any more; «Очистить» is pressed and held
        // (destructive actions: the c-setting button confirms by a hold).
        column.rows.push_back(Json{{"kind", "button"},
                                   {"id", "app-cache-clear"},
                                   {"label", Fill(bindings.Text("settings.cache"), {size})},
                                   {"hint", pending ? bindings.Text("settings.cache_pending") : std::string()},
                                   {"display", pending ? std::string() : bindings.Text("settings.cache_clear")},
                                   {"disabled", !pending && bytes >= 0 && bytes < (1 << 20)}, // under a megabyte: nothing to clear
                                   {"reason", bindings.Text("settings.cache_empty")}});
        bindings.On("app-cache-clear", "", [&bindings](const Event &) {
            if (!AppSettings().value("cache_clear_pending", false))
                bindings.SendHost("app-cache-clear", ""); // the host reports the pending clear
        });
    }

    // The debug page exists only when the host reports debug overrides (a debuggable app).
    int TabCount() const
    {
        return static_cast<int>(std::size(kTabs)) + (Known("fx_msaa") ? 1 : 0);
    }

    // «Чувствительность»: one slider for the game's two mouse sensitivities (the touch camera drag goes through
    // them): it shows the vertical one and sets both to the same position.
    void Sensitivity(const Admission &admitted, Bindings &bindings, Column &column)
    {
        const Widget *vertical = admitted.One("VMOUSE_SENSITIVITY_SLIDE");
        const Widget *horizontal = admitted.One("HMOUSE_SENSITIVITY_SLIDE");
        if (!vertical || !horizontal)
            return;
        const auto percent = [](const Widget &slider) {
            const int span = std::max(1, slider.max - slider.min);
            return std::clamp(static_cast<int>(std::lround(100.0 * (std::atoi(slider.value.c_str()) - slider.min) / span)), 0, 100);
        };
        const auto position = [](const Widget &slider, double share) {
            const int span = std::max(1, slider.max - slider.min);
            const int step = std::max(1, slider.step);
            const double wanted = slider.min + share * span;
            const int snapped = slider.min + static_cast<int>(std::lround((wanted - slider.min) / step)) * step;
            return std::to_string(std::clamp(snapped, slider.min, slider.max));
        };
        bindings.Ignore("camera-sensitivity", "change"); // drag progress: the position goes out on release
        bindings.On("camera-sensitivity", "commit", [&bindings, vertical, horizontal, position](const Event &event) {
            const double share = std::clamp(std::atof(event.value.c_str()), 0.0, 100.0) / 100.0;
            bindings.Send(*vertical, "set", position(*vertical, share));
            bindings.Send(*horizontal, "set", position(*horizontal, share));
        });
        column.rows.push_back(Json{{"kind", "slider"},
                                   {"id", "camera-sensitivity"},
                                   {"label", bindings.Text("settings.camera_sensitivity")},
                                   {"value", percent(*vertical)},
                                   {"disabled", !vertical->Offers("set") || !horizontal->Offers("set")},
                                   {"reason", vertical->unavailable_reason}});
    }

    // A renderer option the host reported; without the report the row stays visible but
    // disabled, and a tap says why.
    bool Known(const char *key) const
    {
        return draft_.is_object() && draft_.contains(key) && draft_[key].is_number_integer();
    }

    // `restart`: the option applies at the next start (marked so); `stack`: the options go
    // under the label in a full-width segment.
    void Segment(Bindings &bindings, Column &column, const char *key, const char *label, std::vector<int> values, Json options,
                 bool restart = true, bool stack = true)
    {
        int selected = 0;
        for (size_t i = 0; i < values.size(); ++i)
            if (Known(key) && draft_[key].get<int>() == values[i])
                selected = static_cast<int>(i);
        const std::string id = std::string("app-") + key;
        column.rows.push_back(Json{{"kind", "segment"},
                                   {"id", id},
                                   {"label", bindings.Text(label)},
                                   {"options", options},
                                   {"selected", selected},
                                   {"stack", stack},
                                   {"restart", restart},
                                   {"disabled", !Known(key)},
                                   {"reason", Known(key) ? std::string() : bindings.Text("settings.app_unavailable")}});
        bindings.On(id, "change", [this, &bindings, key, values](const Event &event) {
            const size_t index = static_cast<size_t>(std::atoi(event.value.c_str()));
            if (index < values.size() && Known(key))
                draft_[key] = values[index];
            bindings.Rebuild();
        });
    }

    // An app option that is on (1) or off (0) as a toggle row, a draft applied with «Готово».
    void Toggle(Bindings &bindings, Column &column, const char *key, const char *label)
    {
        const std::string id = std::string("app-") + key;
        column.rows.push_back(Json{{"kind", "toggle"},
                                   {"id", id},
                                   {"label", bindings.Text(label)},
                                   {"on", Known(key) && draft_[key].get<int>() != 0},
                                   {"disabled", !Known(key)},
                                   {"reason", Known(key) ? std::string() : bindings.Text("settings.app_unavailable")}});
        bindings.On(id, "change", [this, &bindings, key](const Event &event) {
            if (Known(key))
                draft_[key] = event.value == "1" ? 1 : 0;
            bindings.Rebuild();
        });
    }

    // A debug override row (raw, untranslated label); applied with «Готово» without a restart.
    void DebugSegment(Bindings &bindings, Column &column, const char *key, const char *label, std::vector<int> values, Json options)
    {
        int selected = 0;
        for (size_t i = 0; i < values.size(); ++i)
            if (draft_[key].get<int>() == values[i])
                selected = static_cast<int>(i);
        const std::string id = std::string("app-") + key;
        column.rows.push_back(Json{{"kind", "segment"}, {"id", id},       {"label", label},     {"options", options},
                                   {"selected", selected}, {"stack", true}, {"restart", false}, {"disabled", false},
                                   {"reason", ""}});
        bindings.On(id, "change", [this, &bindings, key, values](const Event &event) {
            const size_t index = static_cast<size_t>(std::atoi(event.value.c_str()));
            if (index < values.size())
                draft_[key] = values[index];
            bindings.Rebuild();
        });
    }

    int tab_ = 0;
    Json draft_; // the renderer options as the player set them on this opening
};

} // namespace

std::unique_ptr<Screen> CreateSettingsScreen()
{
    return std::make_unique<SettingsScreen>();
}

std::unique_ptr<StateConsumer> CreateAppSettingsConsumer()
{
    return std::make_unique<AppSettingsConsumer>();
}

std::unique_ptr<StateConsumer> CreateRenderCapsConsumer()
{
    return std::make_unique<RenderCapsConsumer>();
}

} // namespace storm::ui::screens
