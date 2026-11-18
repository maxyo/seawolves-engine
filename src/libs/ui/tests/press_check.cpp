// Host checks for the press behaviour with the whole runtime (a renderer that draws
// nothing, the synthetic pack tests/pack plus one row component and a real font, so text
// is measured): a long press on a row whose title was cut to fit opens the tooltip with
// the full text and sends nothing; on a row whose title fits it is a plain press, also
// when it fits only stepped down to its min-size. The
// panzoom behaviour (c-map-view) reports taps in the picture's units through pan and zoom. A full-screen
// game video (state channel "video") hides the screen and passes every touch to the game.
// A tap on what is not laid out yet waits for its first frame: a document tapped before it was
// drawn, an interface the game opens (ExpectInterface) before its model arrived, and a tap
// made while the game is in no gameplay mode (title load, scene change): the next interface
// gets it, gameplay drops it.
// No GL context, no game data.
#include "runtime_stub.h"
#include "storm_ui/ui.h"

#include <algorithm>
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

#include <nlohmann/json.hpp>
#include "../src/components.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unistd.h>

using namespace storm::ui;
using Json = nlohmann::json;
namespace fs = std::filesystem;

static int failures = 0;
static int probe_ticks = 0;
static bool probe_active = true;

// A second independent owner must keep running after press/reveal/columns finish.
class TickProbe final : public Behavior
{
    void Rebuilt(ComponentElement &self) override { self.RequestTicks(*this, probe_active); }
    void Tick(ComponentElement &self, double) override
    {
        ++probe_ticks;
        if (!probe_active)
            self.RequestTicks(*this, false);
    }
};
static std::unique_ptr<Behavior> MakeTickProbe() { return std::make_unique<TickProbe>(); }


static void Expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// The test pack with one more component: a row like C-ListRow (title cut to two lines,
// long press = tooltip) and an overlay document with the tooltip layer.
static fs::path MakePack(const fs::path &base, const fs::path &font)
{
    const fs::path pack = fs::temp_directory_path() / ("storm-ui-press-" + std::to_string(::getpid()));
    fs::remove_all(pack);
    fs::copy(base, pack, fs::copy_options::recursive);
    fs::copy_file(font, pack / "font.ttf");
    Json manifest = Json::parse(Read(pack / "pack.json"));
    manifest["fonts"] = Json::array({"font.ttf"});
    manifest["overlay"] = "overlay.rml";
    manifest["settings"] = {{"toastMs", 500}, {"toastLeave", 0.22}};
    Write(pack / "pack.json", manifest.dump());
    Json components = Json::parse(Read(pack / "components.json"));
    components["components"].push_back(Json::parse(R"({
        "tag": "c-row", "behaviors": ["press"],
        "props": {"title": {"type": "text", "default": ""}, "hold": {"type": "bool", "default": false}, "tip": {"type": "text", "default": ""}},
        "constants": {"holdMode": "tooltip"},
        "template": "<ui-clamp lines=\"2\" text=\"{{title}}\" style=\"display: block; width: 100%;\"/>"})"));
    // A one-line label that fits only when stepped down (ui-clamp min-size), and the same label
    // without a step-down: cut, so its long press is the tooltip.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-fit", "behaviors": ["press"],
        "props": {"title": {"type": "text", "default": ""}, "least": {"type": "int", "default": 0}, "hold": {"type": "bool", "default": false}, "tip": {"type": "text", "default": ""}},
        "constants": {"holdMode": "tooltip"},
        "template": "<ui-clamp lines=\"1\" min-size=\"{{least}}\" text=\"{{title}}\" style=\"display: block; width: 100%; white-space: nowrap;\"/>"})"));
    // The toast the host shows: it comes in and goes out with an animation (as the pack's C-Notice).
    components["components"].push_back(Json::parse(R"({
        "tag": "c-notice", "behaviors": ["toast"],
        "props": {"text": {"type": "text", "default": ""}, "variant": {"type": "text", "default": ""}, "place": {"type": "text", "default": ""},
                  "tone": {"type": "text", "default": ""}, "compact": {"type": "bool", "default": false}, "leaving": {"type": "bool", "default": false}},
        "template": "<div data-toast=\"toast\" style=\"height: 30dp;\">{{text}}</div>"})"));
    components["components"].push_back(Json::parse(R"({
        "tag": "c-pz", "behaviors": ["panzoom"],
        "props": {"base": {"type": "json", "default": [1000, 500]}, "view": {"type": "json", "default": []}},
        "template": "<div data-panzoom=\"1\" style=\"position: absolute; left: 0; top: 0; right: 0; bottom: 0;\"><div data-plane=\"1\" style=\"position: absolute;\"></div><div data-zoom=\"in\" style=\"position: absolute; right: 0; top: 0; width: 40dp; height: 40dp;\"></div></div>"})"));
    // The pause menu's screen component, reduced to its «Продолжить» (id "resume") as a row.
    components["components"].push_back(Json::parse(R"({
        "tag": "s-game-menu",
        "props": {"line": {"type": "json", "default": {}}, "resume": {"type": "json", "default": {}}, "buttons": {"type": "json", "default": []}, "confirm": {"type": "json", "default": {}}, "extras": {"type": "json", "default": []}},
        "template": "<div style=\"font-family: LatoLatin;\"><c-row id=\"resume\" style=\"display: block; position: absolute; left: 0; top: 0; width: 200dp; height: 48dp;\" title=\"Resume\"/></div>"})"));
    // Icon-only table headers: tap sorts, long press explains without sorting.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-head", "behaviors": ["press", "columns", "reveal"], "constants": {"pressRoot": false},
        "props": {"show": {"type": "bool", "default": true}},
        "template": "<div t-if=\"show\" data-action=\"sort\" data-value=\"2\" data-tip=\"Buy price\" style=\"width: 60dp; height: 40dp;\">icon</div><div data-action=\"\" data-tip=\"Our stock\" style=\"width: 60dp; height: 40dp;\">icon</div>"})"));
    // A hold-to-confirm part (C-Setting's «Очистить»): a hold sends, an early release shakes it.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-confirm", "behaviors": ["press"], "constants": {"pressRoot": false},
        "template": "<div id=\"confirm-part\" data-action=\"clear\" data-hold=\"\" style=\"display: block; width: 100dp; height: 48dp;\"></div>"})"));
    // Column sizing must preserve the design minimum even when the headers are tiny icons.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-cols", "behaviors": ["press", "columns", "reveal", "tick-probe"], "constants": {"colMin": 56},
        "template": "<div data-columns-head=\"\" style=\"display: flex;\"><div style=\"flex: 1 1 0;\"></div><div id=\"h0\" data-col=\"0\" style=\"min-width: 56dp; min-height: 44dp; display: flex; align-items: flex-start;\"><span>x</span></div><div id=\"h1\" data-col=\"1\" style=\"min-width: 56dp; min-height: 44dp; display: flex; align-items: flex-start;\"><span>x</span></div></div><div data-columns-body=\"\" style=\"height: 30dp; overflow-y: auto;\"><div style=\"display: flex;\"><div style=\"flex: 1 1 0;\"></div><div id=\"v0\" data-col=\"0\" style=\"min-width: 56dp; display: flex; align-items: flex-start;\"><span>1</span></div><div id=\"v1\" data-col=\"1\" style=\"min-width: 56dp; display: flex; align-items: flex-start;\"><span>2</span></div></div><div style=\"height: 100dp;\"></div></div>"})"));
    Write(pack / "components.json", components.dump());
    // A broken component (an unknown tag) and an icon not in the pack, under the pack's rule for the
    // inline marks (base.rcss: `.ui-debug .ui-error`, the stripes).
    Write(pack / "marks.rml", "<rml><head><style>body { width: 100%; height: 100%; } .ui-unknown { display: block; }"
                              " .ui-debug .ui-error { min-width: 24dp; min-height: 24dp; }</style></head>"
                              "<body><c-nope id=\"broken\"/><ui-icon id=\"lost\" name=\"nothing-here\" size=\"24\"/></body></rml>");
    Write(pack / "overlay.rml", "<rml><head><style>body { font-family: LatoLatin; width: 100%; height: 100%; pointer-events: none; z-index: 1000; } #tooltip { display: none; } #tooltip.open { display: block; position: absolute; }"
                                " #toasts { position: absolute; left: 0; right: 0; top: 0; display: flex; flex-direction: column; }"
                                " @keyframes in { from { opacity: 0; } to { opacity: 1; } } @keyframes out { from { opacity: 1; max-height: 30dp; } to { opacity: 0; max-height: 0dp; } }"
                                " c-notice { display: block; width: 100%; animation: 0.18s cubic-out in; } c-notice [data-toast] { display: block; pointer-events: auto; } c-notice.leaving { opacity: 0; max-height: 0dp; animation: 0.22s cubic-out out; }</style></head>"
                                "<body><div id=\"tooltip\"></div><div id=\"toasts\"></div></body></rml>");
    Write(pack / "rows.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }"
                             " c-row { display: block; position: absolute; left: 0; width: 200dp; height: 48dp; }"
                             " c-fit { display: block; position: absolute; left: 0; width: 72dp; height: 40dp; }</style></head><body>"
                             "<c-cols id=\"columns\" style=\"display: block; position: absolute; left: 220dp; top: 100dp; width: 160dp; height: 100dp;\"/>"
                             "<c-head id=\"headers\" style=\"display: flex; position: absolute; left: 220dp; top: 0; width: 120dp; height: 40dp;\"/>"
                             "<c-row id=\"short\" style=\"top: 0;\" hold=\"1\" title=\"Short\"/>"
                             "<c-row id=\"long\" style=\"top: 100dp;\" hold=\"1\" title=\"A reply far too long to fit in two lines of a narrow column, so it is cut and ends in an ellipsis\"/>"
                             "<c-fit id=\"fit\" style=\"top: 200dp;\" hold=\"1\" least=\"11\" title=\"Mannschaft\"/>"
                             "<c-fit id=\"cut\" style=\"top: 300dp;\" hold=\"1\" title=\"Mannschaft\"/>"
                             "<c-pz id=\"pz\" style=\"display: block; position: absolute; left: 400dp; top: 200dp; width: 400dp; height: 200dp;\"/>"
                             "<c-confirm id=\"confirm\" style=\"display: block; position: absolute; left: 600dp; top: 0; width: 100dp; height: 48dp;\"/>"
                             "</body></rml>");
    Write(pack / "columns-only.rml", "<rml><head><style>body { font-family: LatoLatin; width: 100%; height: 100%; }</style></head>"
          "<body><div style=\"display: flex; width: 100%;\"><c-cols id=\"columns\" style=\"display: block; flex: 1 1 0; min-width: 0; height: 100dp;\"/>"
          "<div id=\"sibling\" style=\"width: 80px; height: 100dp; flex: 0 0 80px;\"/></div></body></rml>");
    Write(pack / "columns-auto.rml", "<rml><head><style>body { font-family: LatoLatin; width: 100%; height: 100%; }</style></head>"
          "<body><c-cols id=\"columns\" style=\"display: block; width: 60%;\"/><div id=\"following\" style=\"height: 32px;\"/></body></rml>");
    // A store-like component: a list of 20 rows, the deal's amount, its total and the buy button.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-store",
        "props": {"rows": {"type": "json", "default": []}, "amount": {"type": "int", "default": 0}, "total": {"type": "text", "default": ""}},
        "template": "<div id=\"list\"><t-for each=\"rows\" as=\"r\"><div class=\"row{{ r.sel ? ' sel' : '' }}\" data-value=\"{{r.id}}\"><span class=\"name\">{{r.name}}</span><span class=\"held\">{{r.held}}</span></div></t-for></div><div id=\"amount\">{{amount}}</div><div id=\"total\">{{total}}</div><c-row id=\"buy\" title=\"{{total}}\" style=\"display: block; width: 200dp; height: 48dp;\"/>"})"));
    // A screen that passes its deal on: the amount goes to the store whole, the label is its own text.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-outer",
        "props": {"deal": {"type": "json", "default": null}, "rows": {"type": "json", "default": []}, "label": {"type": "text", "default": ""}},
        "template": "<div class=\"label\">{{label}}</div><t-if test=\"deal\"><c-store id=\"inner\" rows=\"{{ json(rows) }}\" props=\"{{ json(deal) }}\"/></t-if>"})"));
    // constants.boxSize: the size prop is the box (an icon measured without formatting its content).
    components["components"].push_back(Json::parse(R"({
        "tag": "c-sized", "constants": {"boxSize": "size"},
        "props": {"size": {"type": "int", "default": 24}},
        "template": "<div style=\"display: inline-block; width: 10dp; height: 10dp;\"></div>"})"));
    // A slider (behaviour range): the template places the knob at the value.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-slide", "behaviors": ["range"],
        "props": {"value": {"type": "int", "default": 0}, "min": {"type": "int", "default": 0}, "max": {"type": "int", "default": 100},
                  "active": {"type": "bool", "default": false}},
        "template": "<div data-range=\"value\" style=\"position: absolute; left: 100dp; top: 100dp; width: 400dp; height: 40dp;\"><div id=\"knob\" data-range-knob=\"\" style=\"position: absolute; left: {{ (value - min) / (max - min) * 100 }}%; top: 0; width: 0; height: 40dp;\"></div></div>"})"));
    Write(pack / "slide.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                              "<c-slide id=\"coarse\" max=\"4\"/></body></rml>");
    // A slider whose value's texts and marks are live parts (data-live-*): a drag needs no render
    // of the control until its shape (here: above or below the middle) changes.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-live-slide", "behaviors": ["range"],
        "props": {"value": {"type": "int", "default": 0}, "min": {"type": "int", "default": 0}, "max": {"type": "int", "default": 100},
                  "active": {"type": "bool", "default": false}},
        "template": "<div data-live-shape=\"value > 50 ? 'high' : 'low'\"><div data-range=\"value\" style=\"position: absolute; left: 100dp; top: 100dp; width: 400dp; height: 40dp;\"><div id=\"live-knob\" data-range-knob=\"\" data-live-style=\"'left: ' + value + '%;'\" style=\"position: absolute; left: {{value}}%; top: 0; width: 0; height: 40dp;\"></div></div><div id=\"live-num\" data-live-text=\"value\">{{value}}</div><t-if test=\"value > 50\"><div id=\"live-high\">high</div></t-if></div>"})"));
    Write(pack / "live-slide.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                   "<c-live-slide id=\"live\" value=\"10\"/></body></rml>");
    // The slider on a page that scrolls vertically (a settings page).
    Write(pack / "slide-list.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                   "<div id=\"page\" style=\"display: block; position: absolute; left: 0; top: 0; width: 800dp; height: 300dp; overflow-y: auto;\">"
                                   "<div style=\"display: block; position: relative; height: 1000dp;\"><c-slide id=\"listed-slide\" max=\"4\"/></div></div></body></rml>");
    Write(pack / "slide-fine.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                   "<c-slide id=\"fine\" max=\"4000\"/></body></rml>");
    // A list whose open row shows its amount under itself (C-TradeList): behaviour reveal.
    components["components"].push_back(Json::parse(R"({
        "tag": "c-open-list", "behaviors": ["reveal"],
        "props": {"open": {"type": "int", "default": -1}, "amount": {"type": "int", "default": 0}},
        "template": "<div id=\"olist\" style=\"position: absolute; left: 0; top: 0; width: 400dp; height: 200dp; overflow-y: auto;\"><t-for each=\"range(20)\" as=\"r\"><t-if test=\"r == open\"><div data-reveal=\"{{r}}\" style=\"display: block; height: 108dp;\">{{r}} {{amount}}</div><t-else/><div style=\"display: block; height: 48dp;\">{{r}}</div></t-if></t-for></div>"})"));
    Write(pack / "open-list.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }"
                                  " .scroll-fade { position: absolute; left: 0; right: 0; height: 26dp; display: none; } .scroll-below > .scroll-fade { display: block; }"
                                  "</style></head><body><c-open-list id=\"opener\"/></body></rml>");
    Write(pack / "components.json", components.dump());
    {
        Json rows = Json::array();
        for (int i = 0; i < 20; ++i)
            rows.push_back({{"id", i}, {"name", "Goods " + std::to_string(i)}, {"held", i * 10}, {"sel", i == 3}});
        Write(pack / "store.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                  "<c-store id=\"store\" amount=\"1\" total=\"Buy 1\" rows='" + rows.dump() + "'/>"
                                  "<div style=\"display: flex;\"><c-sized id=\"sized\" size=\"20\"/></div></body></rml>");
        Write(pack / "bar.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                "<div id=\"bar\" rmlui-layout-boundary=\"\" style=\"display: flex; flex-direction: row; width: 800dp; height: 40dp;\">"
                                "<div id=\"stat\" rmlui-layout-boundary=\"\" style=\"flex: 0 0 auto;\">189</div><div id=\"rest\" style=\"flex: 1 1 0;\">rest</div>"
                                "</div></body></rml>");
        Write(pack / "outer.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }</style></head><body>"
                                  "<c-outer id=\"outer\" label=\"Store\" deal='{\"amount\": 1, \"total\": \"Buy 1\"}' rows='" + rows.dump() + "'/></body></rml>");
    }
    // The dialogue panel's layout (C-DialoguePanel): the speech box stops 44 dp above the
    // bottom for the «Дальше ›» line, the replies (data-reserve) reach the bottom and take that
    // line's place on the final page.
    Write(pack / "pages.rml", "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }"
                              " .panel { position: absolute; left: 0; top: 0; width: 400dp; height: 200dp; }"
                              " ui-pages { display: block; position: absolute; left: 0; top: 0; right: 0; bottom: 44dp; line-height: 26dp; white-space: nowrap; overflow: hidden; }"
                              " .replies { position: absolute; left: 0; right: 0; bottom: 0; }"
                              " .replies c-row { display: block; height: 48dp; }"
                              " ui-pages.more ~ .replies { visibility: hidden; pointer-events: none; }"
                              " .next { position: absolute; right: 0; bottom: 0; width: 80dp; height: 44dp; } ui-pages.last ~ .next { display: none; }</style></head><body>"
                              "<div class=\"panel\"><ui-pages text=\"Welcome aboard, captain! The yard has timber, pitch and canvas in plenty, and my men have been idle since the storm. What will it be today?\" page=\"0\" mode=\"paged\"/>"
                              "<div class=\"replies\" data-reserve=\"1\"><c-row id=\"reply\" title=\"Repair my ship\"/><c-row id=\"leave\" title=\"Goodbye\"/></div>"
                              "<c-row id=\"next\" class=\"next\" title=\"Next\"/></div>"
                              "</body></rml>");
    // The dialogue sheet (s-dialogue): it grows (data-grow) up to its max-height before the
    // speech pages; here 200 dp high in a 400 dp screen, at most 88 % (352 dp).
    const std::string sheet =
        "<rml><head><style>body { font-family: LatoLatin; font-size: 16dp; width: 800dp; height: 400dp; }"
        " .sheet { position: absolute; left: 0; bottom: 0; width: 400dp; height: 200dp; max-height: 88%; }"
        " ui-pages { display: block; position: absolute; left: 0; top: 0; right: 0; bottom: 44dp; line-height: 26dp; white-space: nowrap; overflow: hidden; }"
        " .replies { position: absolute; left: 0; right: 0; bottom: 0; } .replies c-row { display: block; height: 48dp; }"
        " ui-pages.more ~ .replies { visibility: hidden; pointer-events: none; }"
        " .next { position: absolute; right: 0; bottom: 0; width: 80dp; height: 44dp; } ui-pages.last ~ .next { display: none; }</style></head><body>"
        "<div class=\"sheet\" data-grow=\"1\"><ui-pages text=\"TEXT\" page=\"0\" mode=\"paged\"/>"
        "<div class=\"replies\" data-reserve=\"1\"><c-row id=\"reply\" title=\"Repair my ship\"/><c-row id=\"leave\" title=\"Goodbye\"/></div>"
        "<c-row id=\"next\" class=\"next\" title=\"Next\"/></div></body></rml>";
    const auto with = [&](const std::string &text) {
        std::string doc = sheet;
        doc.replace(doc.find("TEXT"), 4, text);
        return doc;
    };
    const std::string greeting = "Welcome aboard, captain! The yard has timber, pitch and canvas in plenty, and my men have been idle since "
                                 "the storm. We can mend the hull, change the sails and rigging, or sell you a sloop or a brig that "
                                 "stand at the pier. What will it be today?";
    Write(pack / "grow.rml", with(greeting));
    std::string story;
    for (int i = 0; i < 8; ++i)
        story += greeting + " ";
    Write(pack / "grow-long.rml", with(story));
    return pack;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: storm_ui_press_unit <tests/pack> <font.ttf>\n");
        return 2;
    }
    RegisterBehavior("tick-probe", &MakeTickProbe);
    const fs::path pack = MakePack(argv[1], argv[2]);
    std::unique_ptr<FileSource> files = DirectoryFiles(pack.string());
    NoBackend backend;
    std::vector<std::string> clicks;
    int sounds = 0; // the interface clicks a press start asked for (Action screen "sound")
    double clock = 0.0;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 800;
    config.height = 400;
    config.clock = [&clock] { return clock; };
    std::vector<std::pair<LogLevel, std::string>> logged;
    config.on_log = [&logged](LogLevel level, const std::string &line) { logged.emplace_back(level, line); };
    config.on_action = [&clicks, &sounds](const Action &action) {
        if (action.screen == "sound")
        {
            ++sounds;
            return;
        }
        clicks.push_back(action.node + " " + action.verb + (action.value.empty() ? "" : " " + action.value));
    };
    std::string error;
    std::unique_ptr<Host> host = Host::Create(config, &error);
    Expect(host != nullptr, "the runtime starts: " + error);
    if (!host)
        return 1;
    Expect(host->LoadDocument("rows.rml"), "the rows document loads");
    const auto frames = [&](double seconds) {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0)
        {
            clock += 1.0 / 60.0;
            host->Frame();
        }
    };
    frames(0.2);
    const int before_release = probe_ticks;
    host->Touch(TouchPhase::Down, 1, 230, 110);
    host->Touch(TouchPhase::Up, 1, 230, 110);
    frames(0.1);
    Expect(probe_ticks >= before_release + 5, "press release and column completion cannot stop another tick owner");
    probe_active = false;
    frames(0.05);
    const int stopped = probe_ticks;
    frames(0.05);
    Expect(probe_ticks == stopped, "the last tick owner stops independently");
    const auto hold = [&](float y, float x = 100) {
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, x, y);
        frames(1.0);
        host->Touch(TouchPhase::Up, 1, x, y);
        frames(0.1);
    };
    {
        Rml::Element *part = nullptr;
        Rml::Context *context = Rml::GetContext("storm-ui");
        for (int i = 0; context && i < context->GetNumDocuments(); ++i)
            if (Rml::Element *found = context->GetDocument(i)->GetElementById("confirm-part"))
                part = found;
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, 640, 20);
        frames(0.2);
        host->Touch(TouchPhase::Up, 1, 640, 20);
        frames(0.05);
        Expect(part && clicks.empty() && part->IsClassSet("shake"), "a hold released early sends nothing and shakes its part");
        frames(1.0);
        Expect(part && !part->IsClassSet("shake"), "the shake ends");
        hold(20, 640);
        Expect(clicks.size() == 1 && clicks[0] == "confirm clear", "a full hold sends: " + (clicks.empty() ? std::string() : clicks[0]));
    }
    {
        Rml::Element *table = nullptr;
        Rml::Context *context = Rml::GetContext("storm-ui");
        for (int i = 0; context && i < context->GetNumDocuments(); ++i)
            if (Rml::Element *found = context->GetDocument(i)->GetElementById("columns"))
                table = found;
        const auto column = [table](const char *id) { return table ? table->QuerySelector(std::string("#") + id) : nullptr; };
        for (const auto &[head, value] : {std::pair{"h0", "v0"}, std::pair{"h1", "v1"}})
        {
            Rml::Element *h = column(head), *v = column(value);
            Expect(h && v && h->GetBox().GetSize(Rml::BoxArea::Border).x >= 56.f && h->GetBox().GetSize(Rml::BoxArea::Border).y >= 44.f &&
                       std::abs(h->GetBox().GetSize(Rml::BoxArea::Border).x - v->GetBox().GetSize(Rml::BoxArea::Border).x) < 0.01f &&
                       std::abs(h->GetAbsoluteLeft() - v->GetAbsoluteLeft()) < 0.01f,
                   "icon columns retain 56dp minimum and aligned numeric cells");
        }
    }
    Expect(host->LoadDocument("columns-only.rml"), "isolated columns document loads without unrelated text clamps");
    frames(0.2);
    // Resize the same mounted component: pixel measurements must not survive a
    // viewport/density change, and shrinking must release the old larger minima.
    {
        Rml::Context *context = Rml::GetContext("storm-ui");
        Rml::Element *table = nullptr;
        for (int i = 0; context && i < context->GetNumDocuments(); ++i)
            if (Rml::Element *found = context->GetDocument(i)->GetElementById("columns"))
                table = found;
        Expect(table != nullptr, "resize keeps the mounted columns fixture");
        if (table)
        {
            table->SetProperty("width", "60%");
            Rml::Element *original_head = table->QuerySelector("#h0");
            const auto check = [&](int width, int height, float density) {
                host->Resize(width, height, density);
                host->Frame();
                Expect(table->QuerySelector("#h0") == original_head, "resize preserves column element identity");
                Rml::Element *sibling = table->GetParentNode()->QuerySelector("#sibling");
                Expect(sibling && std::abs(sibling->GetClientWidth() - 80.f) < 0.01f &&
                           std::abs(table->GetClientWidth() + 80.f - table->GetParentNode()->GetClientWidth()) < 0.01f &&
                           std::abs(sibling->GetAbsoluteLeft() - table->GetAbsoluteLeft() -
                                    table->GetBox().GetSize(Rml::BoxArea::Border).x) < 0.01f,
                       "column relayout preserves flex allocation and the sibling box");
                Rml::Element *head = table->QuerySelector("[data-columns-head]");
                Rml::Element *body = table->QuerySelector("[data-columns-body]");
                Expect(head && body && std::abs(head->GetClientWidth() - body->GetClientWidth()) < 0.01f,
                       "resized header matches current scrolling body viewport");
                for (const auto &[h_id, v_id] : {std::pair{"h0", "v0"}, std::pair{"h1", "v1"}})
                {
                    Rml::Element *h = table->QuerySelector(std::string("#") + h_id);
                    Rml::Element *v = table->QuerySelector(std::string("#") + v_id);
                    Expect(h && v && std::abs(h->GetBox().GetSize(Rml::BoxArea::Border).x - 56.f * density) < 0.01f &&
                               std::abs(h->GetAbsoluteLeft() - v->GetAbsoluteLeft()) < 0.01f,
                           "live resize remeasures 56dp columns and keeps header/body alignment");
                }
            };
            check(1280, 720, 1.5f);
            check(780, 360, 1.f);
            check(1280, 720, 1.5f);
            check(1280, 720, 1.f); // density-only, unchanged pixel viewport
            // Natural text can exceed the minimum too: a density-only shrink must
            // not measure the previous frame's larger glyphs and retain that width.
            Rml::Element *value = table->QuerySelector("#v0");
            value->GetChild(0)->SetInnerRML("123456789012345");
            table->SetProperty("height", "100px");
            table->SetProperty("width", "500px");
            frames(0.1);
            for (float density : {1.5f, 1.f})
            {
                host->Resize(1280, 720, density);
                host->Frame();
                const float natural = value->GetChild(0)->GetBox().GetSize(Rml::BoxArea::Border).x;
                Expect(natural > 56.f * density &&
                           std::abs(value->GetBox().GetSize(Rml::BoxArea::Border).x - std::ceil(natural)) < 0.01f,
                       "density-only resize remeasures current natural glyph width");
            }
            value->GetChild(0)->SetInnerRML("1");
            host->Resize(800, 400, 1.f);
            table->SetProperty("width", "160dp");
            table->SetProperty("height", "100dp");
            frames(0.2);
        }
    }
    Expect(host->LoadDocument("columns-auto.rml"), "automatic-height columns document loads");
    frames(0.2);
    for (float density : {1.5f, 1.f})
    {
        host->Resize(800, 400, density);
        host->Frame();
        Rml::Element *table = nullptr, *following = nullptr;
        auto *context = Rml::GetContext("storm-ui");
        for (int i = 0; i < context->GetNumDocuments(); ++i)
        {
            if (auto *found = context->GetDocument(i)->GetElementById("columns")) table = found;
            if (auto *found = context->GetDocument(i)->GetElementById("following")) following = found;
        }
        Expect(table && following && std::abs(following->GetAbsoluteTop() - table->GetAbsoluteTop() -
                   table->GetBox().GetSize(Rml::BoxArea::Border).y) < 0.01f,
               "automatic-height column relayout keeps the following sibling in normal flow on its first frame");
    }
    host->Resize(800, 400, 1.f);
    Expect(host->LoadDocument("rows.rml"), "rows document returns after isolated resize checks");
    frames(0.2);
    hold(124);
    Expect(clicks.empty(), "a long press on a cut title opens its tooltip and sends nothing");
    hold(24);
    Expect(clicks.size() == 1 && clicks[0] == "short click", "a long press on a title that fits is a plain press");
    hold(220, 30);
    Expect(clicks.size() == 1 && clicks[0] == "fit click", "the same label with min-size steps down and fits: a plain press");
    hold(320, 30);
    Expect(clicks.empty(), "a label too wide for its box without a step-down is cut (tooltip)");

    // Panzoom: the 1000 x 500 picture fills the 400 x 200 viewport (0.4 px per unit).
    const auto tap = [&](float x, float y) {
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, x, y);
        host->Touch(TouchPhase::Up, 1, x, y);
        frames(0.05);
    };
    hold(20, 240);
    Expect(clicks.empty(), "holding a sortable icon header explains it without sorting");
    tap(240, 20);
    Expect(clicks.size() == 1 && clicks[0] == "headers sort 2", "a short tap still sorts the icon header");
    hold(20, 300);
    Expect(clicks.empty(), "holding a non-sortable icon header sends no action");
    tap(300, 20);
    Expect(clicks.empty(), "a tooltip-only header has no tap action");
    // A list rebuild can remove the tooltip anchor while the finger is still down.
    const auto tooltip = [&]() -> Rml::Element * {
        Rml::Context *context = Rml::GetContext("storm-ui");
        for (int i = 0; context && i < context->GetNumDocuments(); ++i)
            if (auto *tip = context->GetDocument(i)->GetElementById("tooltip"))
                return tip;
        return nullptr;
    };
    for (double held : {0.6, 0.1})
    {
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, 240, 20);
        frames(held);
        Expect(tooltip() && tooltip()->IsClassSet("open") == (held > 0.4), "tooltip opens only after its delay");
        Expect(host->SetProps("headers", R"({"show": false})"), "rebuild removes the pressed tooltip anchor");
        frames(0.6);
        Expect(tooltip() && !tooltip()->IsClassSet("open"), "rebuild closes an open tooltip and cancels a pending tooltip");
        host->Touch(TouchPhase::Up, 1, 240, 20);
        Expect(clicks.empty(), "removed tooltip anchor cannot activate on release");
        Expect(host->SetProps("headers", R"({"show": true})"), "tooltip anchor can be mounted again");
        frames(0.1);
    }
    tap(500, 300);
    Expect(clicks.size() == 1 && clicks[0] == "pz point 250.0,250.0", "a tap reports the picture point under it");
    tap(780, 220); // the zoom-in part: ×1.6 around the centre, nothing sent
    Expect(clicks.empty(), "the zoom part zooms locally and sends nothing");
    tap(600, 300);
    Expect(clicks.size() == 1 && clicks[0] == "pz point 500.0,250.0", "the centre stays under the centre after zooming");
    clicks.clear();
    host->Touch(TouchPhase::Down, 1, 600, 300);
    host->Touch(TouchPhase::Move, 1, 650, 300);
    host->Touch(TouchPhase::Move, 1, 664, 300);
    host->Touch(TouchPhase::Up, 1, 664, 300);
    frames(0.05);
    Expect(clicks.empty(), "a drag pans and sends nothing");
    tap(600, 300);
    Expect(clicks.size() == 1 && clicks[0] == "pz point 400.0,250.0", "after a 64 px pan the centre shows a point 100 units left");
    // A quick pan (1200 px/s, a sample every 1/120 s, a frame every second one): the picture
    // follows 1:1 with nothing laid out, then coasts on, frame by frame, to the right edge
    // of the zoomed picture (640 px wide in the 400 px viewport).
    {
        double t = clock;
        float x = 700.0f;
        int layouts = 0, coasting = 0;
        host->Touch(TouchPhase::Down, 1, x, 300, t);
        for (int i = 1; i <= 12; ++i)
        {
            t += 1.0 / 120.0;
            x -= 10.0f;
            host->Touch(TouchPhase::Move, 1, x, 300, t);
            if (i % 2 == 0)
            {
                clock = t;
                host->Frame();
                layouts += i > 2 ? host->LastFrameStats().layout_passes : 0;
            }
        }
        host->Touch(TouchPhase::Up, 1, x, 300, t);
        double next = 1.0;
        while (coasting < 120 && host->Prepare(next) && next == 0.0)
        {
            clock += 1.0 / 60.0;
            host->Frame();
            layouts += host->LastFrameStats().layout_passes;
            ++coasting;
        }
        Expect(layouts == 0, "a pan and its coast lay nothing out (" + std::to_string(layouts) + " layout passes)");
        Expect(coasting >= 3 && coasting < 120, "the released picture coasts on frame by frame and stops (" + std::to_string(coasting) + " frames)");
        tap(600, 300);
        Expect(clicks.size() == 1 && clicks[0] == "pz point 687.5,250.0", "it came to rest at the right edge of the picture (" +
                                                                               (clicks.empty() ? std::string("-") : clicks[0]) + ")");
    }
    // A full-screen game video (channel "video") hides what the UI shows and gives every touch
    // to the game; the screen comes back as it was when the video ends.
    Expect(host->WantsTouch(100, 24) && host->InterfaceActive(), "a document takes the touches");
    Expect(host->SetState("video", R"({"fullscreen":true})"), "the video state is accepted");
    frames(0.05);
    Expect(!host->WantsTouch(100, 24) && !host->InterfaceActive() && !host->WantsBack(),
           "during a full-screen video the touches and the back key go to the game");
    hold(24);
    Expect(clicks.empty(), "nothing of the hidden screen is pressed during the video");
    Expect(host->SetState("video", "null"), "the video end is accepted");
    frames(0.05);
    Expect(host->WantsTouch(100, 24) && host->InterfaceActive(), "after the video the screen takes the touches again");
    Expect(!host->SetState("video", R"({"fullscreen":"yes"})"), "a malformed video state is refused aloud");
    // A toast animates in, stays (hud::ToastSeconds: 3 s for a short one), animates out (toastLeave) and goes: frames keep
    // coming while an animation runs, and stop once everything has settled.
    frames(0.5);
    double delay = 0.0;
    Expect(!host->Prepare(delay) && delay > 0.2, "an idle screen asks for no frame");
    const auto drawing = [&](double from, double to) { // every Prepare in [from, to) s asks for a frame at once
        bool all = true;
        for (double t = from; t < to; t += 1.0 / 60.0)
        {
            double next = 1.0;
            all = host->Prepare(next) && next == 0.0 && all;
            host->Frame();
            clock += 1.0 / 60.0;
        }
        return all;
    };
    host->Toast("Saved", "ok");
    Expect(drawing(0.0, 0.15), "the toast's entrance keeps frames coming");
    frames(0.2);
    Expect(!host->Prepare(delay) && delay > 2.5 && delay < 2.7, "a shown toast asks for no frame, only a wake-up when it is due");
    // a finger on it stops its time; lifting it gives back what was left
    Expect(host->WantsTouch(10, 8), "the toast takes a finger");
    host->Touch(TouchPhase::Down, 1, 10, 8);
    frames(5.0);
    Expect(!host->Prepare(delay) && delay > 3.0, "held: no wake-up is due while the finger stays: " + std::to_string(delay));
    host->Touch(TouchPhase::Up, 1, 10, 8);
    frames(0.05);
    Expect(!host->Prepare(delay) && delay > 2.4 && delay < 2.7, "released: the time it had left runs on: " + std::to_string(delay));
    frames(2.6);
    Expect(drawing(0.0, 0.18), "its exit keeps frames coming");
    frames(0.2);
    Expect(!host->Prepare(delay) && delay > 0.2, "after the exit has run the toast is gone and frames stop");

    // Nothing reaches the screen without a line in the log (Config::on_log: the engine's log files), with where
    // it came from; what is shown as an error is logged as one (error.log).
    const auto wasLogged = [&logged](LogLevel level, const std::string &line) {
        return std::find(logged.begin(), logged.end(), std::make_pair(level, line)) != logged.end();
    };
    Expect(wasLogged(LogLevel::Info, "shown ok (toast): Saved"), "a toast is logged");
    host->Toast("Cannot save", "danger");
    host->Toast("Gone", "warn");
    host->GameMessage("Gold +100", "items");
    host->AlertString("press.alert");
    Expect(wasLogged(LogLevel::Error, "shown danger (toast): Cannot save"), "a danger toast is an error line");
    Expect(wasLogged(LogLevel::Warning, "shown warn (toast): Gone"), "a warning toast is a warning line");
    Expect(wasLogged(LogLevel::Info, "shown info (game log, opens items): Gold +100"), "a game log line, with where a tap leads");
    Expect(wasLogged(LogLevel::Error, "shown danger (alert press.alert): press.alert"), "an alert of a string names its key");
    frames(8.0);

    // A document opened by the platform and tapped before any frame laid it out: the tap
    // waits for that layout and then presses what is under it.
    Expect(host->LoadDocument("rows.rml"), "the rows document loads again");
    tap(100, 24);
    Expect(clicks.size() == 1 && clicks[0] == "short click", "a tap before the document's first frame presses it after that frame");

    // A greeting that fits beside the replies stays on one page: the replies show at once (the
    // pager measures the part of the speech box they cover, not their whole height).
    Expect(host->LoadDocument("pages.rml"), "the paged speech loads");
    frames(0.2);
    tap(40, 130);
    Expect(clicks.size() == 1 && clicks[0] == "reply click", "a three-line greeting and two replies: one page, the replies answer at once: " +
                                                                  (clicks.empty() ? std::string("nothing") : clicks[0]));
    tap(360, 180);
    Expect(clicks.size() == 1 && clicks[0] == "leave click", "the last reply sits where «Дальше» would be, and it is not there");

    // A greeting too long for the designed sheet beside its replies: the sheet grows and it
    // stays one page; a speech longer than even the tallest sheet pages.
    Expect(host->LoadDocument("grow.rml"), "the growing sheet loads");
    frames(0.3);
    tap(40, 330);
    Expect(clicks.size() == 1 && clicks[0] == "reply click", "the sheet grew: the greeting and its replies on one page: " +
                                                                  (clicks.empty() ? std::string("nothing") : clicks[0]));
    Expect(host->LoadDocument("grow-long.rml"), "the long speech loads");
    frames(0.3);
    tap(360, 380);
    Expect(clicks.size() == 1 && clicks[0] == "next click", "a speech beyond the tallest sheet pages: «Дальше» is there");
    tap(40, 330);
    Expect(clicks.empty(), "… and the replies wait");

    host.reset();
    host = Host::Create(config, &error);
    Expect(host != nullptr, "a second runtime starts: " + error);
    if (!host)
        return 1;
    frames(0.05);
    clicks.clear();
    const fs::path states = fs::path(argv[1]) / "../states";
    const std::string explore = Read(states / "gameplay-explore.json");
    Json blocked = Json::parse(explore);
    blocked["mode"] = "unknown"; // what the engine publishes while it loads or changes scene
    blocked["blocked"] = "loading";
    blocked["revision"] = 8;
    blocked["capabilities"] = Json::array();

    // Before the game is in any gameplay mode (the title load, a scene change) nothing on the
    // screen can be used: the UI owns the touches and holds them. Gameplay coming next drops them.
    Expect(!host->WantsTouch(700, 350), "before any gameplay state nothing is decided: the game keeps the touches");
    Expect(host->SetState("gameplay", blocked.dump()), "the engine loads (mode unknown, blocked loading)");
    Expect(host->WantsTouch(700, 350), "while the game loads the touches are the UI's");
    tap(700, 350);
    frames(0.5);
    Expect(host->SetState("gameplay", explore), "gameplay starts");
    frames(0.05);
    Expect(clicks.empty() && !host->WantsTouch(700, 350), "gameplay drops the held tap and the game gets the touches again");

    // A scene change (no gameplay mode again), then the interface the game opens: a tap made
    // in between waits for it and presses it.
    blocked["revision"] = 9;
    Expect(host->SetState("gameplay", blocked.dump()), "the game leaves gameplay");
    Expect(host->WantsTouch(700, 350), "between scenes the touches are the UI's again");
    tap(100, 24);
    frames(0.5);
    Expect(clicks.empty(), "nothing receives the tap between scenes");
    host->ExpectInterface();
    const std::string first_menu = Read(fs::path(argv[1]) / "../models/game-menu.json");
    Expect(host->SetInterface(first_menu), "the game menu model arrives after the scene change");
    frames(0.05);
    Expect(clicks.size() == 1 && clicks[0] == " back",
           "the tap made between scenes presses the menu that came next: " + (clicks.empty() ? std::string("nothing") : clicks[0]));
    clicks.clear();
    host->CloseInterface();
    Expect(host->SetState("gameplay", explore), "gameplay again");
    frames(0.05);

    Expect(!host->WantsTouch(700, 350), "nothing of the UI over the game: its touches go to the game");
    host->ExpectInterface();
    Expect(host->WantsTouch(700, 350) && host->WantsTouch(100, 24), "an interface being opened owns every touch at once");
    tap(100, 24);
    frames(0.5);
    Expect(clicks.empty(), "the tap waits while the model is still to come");
    const std::string menu = Read(fs::path(argv[1]) / "../models/game-menu.json");
    Expect(host->SetInterface(menu), "the game menu model arrives");
    frames(0.05);
    // «Продолжить» resumes through the engine's back (menu_game.cpp).
    Expect(clicks.size() == 1 && clicks[0] == " back",
           "then the held tap presses the screen's «Продолжить»: " + (clicks.empty() ? std::string("nothing") : clicks[0]));
    clicks.clear();
    host->CloseInterface();
    frames(0.05);
    host->ExpectInterface();
    tap(100, 24);
    host->CloseInterface();
    frames(0.05);
    Expect(clicks.empty() && !host->WantsTouch(700, 350), "an opening withdrawn before its model drops its held tap and gives the touches back");

    // One tap whose finger trembles (the platform delivers every sample with its time, the
    // historical ones of a batched move too): one press, one click, one action. On a game
    // interface the press start asks for the interface's click; a plain document (the
    // launcher's screens) has none.
    const auto trembling_tap = [&](float x, float y, int samples) {
        clicks.clear();
        sounds = 0;
        double t = clock;
        host->Touch(TouchPhase::Down, 1, x, y, t);
        for (int i = 0; i < samples; ++i)
            host->Touch(TouchPhase::Move, 1, x + float(i % 3), y + float(i % 2), t += 0.004);
        frames(1.0 / 60.0);
        host->Touch(TouchPhase::Up, 1, x + 1, y, t + 0.004);
        frames(0.05);
    };
    host->ExpectInterface();
    Expect(host->SetInterface(menu), "the game menu opens again");
    frames(0.05);
    trembling_tap(100, 24, 12);
    Expect(sounds == 1 && clicks.size() == 1 && clicks[0] == " back",
           "a tap with 12 move samples on a game interface: one click, one action (" + std::to_string(sounds) + " clicks, " +
               std::to_string(clicks.size()) + " actions)");
    host->CloseInterface();
    frames(0.05);
    Expect(host->LoadDocument("rows.rml"), "the rows document loads for the trembling tap");
    frames(0.05);
    trembling_tap(100, 24, 12);
    Expect(sounds == 0 && clicks.size() == 1 && clicks[0] == "short click",
           "a tap with 12 move samples on a plain document: one action, no click (" + std::to_string(clicks.size()) + " actions)");


    // One value changes, only what shows it changes: a new amount sets the amount's text, the
    // total's text and the button's title (which rebuilds the button), not one of the 20 rows.
    Expect(host->LoadDocument("store.rml"), "the store document loads");
    frames(0.1);
    {
        Rml::Element *list = nullptr;
        std::vector<Rml::Element *> rows_before;
        const auto rows_of = [&] {
            std::vector<Rml::Element *> rows;
            if (list)
                for (int i = 0; i < list->GetNumChildren(); ++i)
                    rows.push_back(list->GetChild(i));
            return rows;
        };
        const auto find = [](const char *id) -> Rml::Element * {
            Rml::Context *context = Rml::GetContext("storm-ui");
            for (int i = 0; context && i < context->GetNumDocuments(); ++i)
                if (Rml::Element *found = context->GetDocument(i)->GetElementById(id))
                    return found;
            return nullptr;
        };
        list = find("list");
        Expect(list && list->GetNumChildren() == 20, "the store lists 20 rows");
        rows_before = rows_of();
        Expect(host->SetProps("store", R"({"amount": 2, "total": "Buy 2"})"), "the amount changes");
        clock += 1.0 / 60.0;
        host->Frame();
        const FrameStats changed = host->LastFrameStats();
        Expect(changed.touched > 0 && changed.touched <= 4 && changed.patch_fallbacks == 0,
               "a new amount touches the amount, the total and the button only (" + std::to_string(changed.touched) + " elements, " +
                   std::to_string(changed.patch_fallbacks) + " whole levels)");
        Expect(find("list") == list && rows_of() == rows_before, "the rows are the same elements");
        Json rows = Json::array();
        for (int i = 0; i < 20; ++i)
            rows.push_back({{"id", i}, {"name", "Goods " + std::to_string(i)}, {"held", i == 3 ? 32 : i * 10}, {"sel", i == 3}});
        Expect(host->SetProps("store", Json{{"rows", rows}}.dump()), "one row's hold changes");
        clock += 1.0 / 60.0;
        host->Frame();
        const FrameStats row = host->LastFrameStats();
        Expect(row.touched == 1 && rows_of() == rows_before, "one row's figure touches that figure only (" + std::to_string(row.touched) + " elements)");
        // The same values again: the render is the shown one, nothing to draw.
        Expect(host->SetProps("store", R"({"amount": 2, "total": "Buy 2"})"), "the same amount again");
        double delay = 0.0;
        clock += 1.0 / 60.0;
        host->Prepare(delay);
        host->Frame();
        Expect(host->LastFrameStats().touched == 0 && host->LastFrameStats().laid_out == 0, "the same values touch and lay out nothing");
        Rml::Element *sized = find("sized");
        Expect(sized && std::abs(sized->GetBox().GetSize(Rml::BoxArea::Border).x - 20.0f) < 0.01f &&
                   std::abs(sized->GetBox().GetSize(Rml::BoxArea::Border).y - 20.0f) < 0.01f,
               "constants.boxSize makes the size prop the box: 20 x 20");
        Expect(host->SetProps("sized", R"({"size": 32})"), "the size changes");
        frames(0.05);
        Expect(std::abs(sized->GetBox().GetSize(Rml::BoxArea::Border).x - 32.0f) < 0.01f, "… and the box follows: 32");
    }

    // A boundary sized by its contents in a flex row (a header's figure): a text of the same width
    // lays out only it; a wider one changes the row around it, which is laid out again.
    Expect(host->LoadDocument("bar.rml"), "the bar document loads");
    frames(0.1);
    {
        Rml::Context *context = Rml::GetContext("storm-ui");
        const auto find = [context](const char *id) -> Rml::Element * {
            for (int i = 0; context && i < context->GetNumDocuments(); ++i)
                if (Rml::Element *found = context->GetDocument(i)->GetElementById(id))
                    return found;
            return nullptr;
        };
        Rml::Element *stat = find("stat"), *rest = find("rest");
        const float narrow = stat ? stat->GetBox().GetSize().x : 0.f, rest_at = rest ? rest->GetAbsoluteLeft() : 0.f;
        Expect(stat && rest && narrow > 0.f, "the bar is laid out");
        if (stat && rest)
        {
            stat->SetInnerRML("WWWWWWW 1 514");
            frames(0.05);
            Expect(stat->GetBox().GetSize().x > narrow + 10.f && rest->GetAbsoluteLeft() > rest_at + 10.f,
                   "a wider figure widens its box and moves what follows (" + std::to_string(stat->GetBox().GetSize().x) + ")");
            stat->SetInnerRML("189");
            frames(0.05);
            Expect(std::abs(stat->GetBox().GetSize().x - narrow) < 0.01f && std::abs(rest->GetAbsoluteLeft() - rest_at) < 0.01f,
                   "back to the first figure: back to the first layout");
        }
    }

    // A value a screen only passes on (a drag of the deal's amount): the screen does not render its
    // template, the store renders once and touches the amount, the total and the button only.
    Expect(host->LoadDocument("outer.rml"), "the outer document loads");
    frames(0.1);
    {
        Expect(host->SetProps("outer", R"({"deal": {"amount": 2, "total": "Buy 2"}})"), "the deal's amount changes");
        clock += 1.0 / 60.0;
        host->Frame();
        const FrameStats moved = host->LastFrameStats();
        Expect(moved.forwarded >= 1 && moved.rebuilds == 1 && moved.touched <= 5 && moved.laid_out <= 30 && moved.patch_fallbacks == 0,
               "a passed-on value: the screen forwards it (" + std::to_string(moved.forwarded) + "), only the store renders (" +
                   std::to_string(moved.rebuilds) + " renders), " + std::to_string(moved.touched) + " elements touched, " +
                   std::to_string(moved.laid_out) + " boxes");
        Expect(host->SetProps("outer", R"({"label": "Shop"})"), "the screen's own text changes");
        clock += 1.0 / 60.0;
        host->Frame();
        const FrameStats own = host->LastFrameStats();
        Expect(own.forwarded == 0 && own.rebuilds == 1, "its own text: the screen renders, the store does not");
        Expect(host->SetProps("outer", R"({"deal": {"amount": 3, "total": "Buy 3"}})"), "the amount after the screen rendered");
        clock += 1.0 / 60.0;
        host->Frame();
        Rml::Context *context = Rml::GetContext("storm-ui");
        Rml::Element *amount = nullptr;
        for (int i = 0; context && i < context->GetNumDocuments() && !amount; ++i)
            amount = context->GetDocument(i)->GetElementById("amount");
        Expect(amount && amount->GetInnerRML() == "3", "the store shows the amount passed on last");
        Expect(host->SetProps("outer", R"({"deal": null})"), "the deal closes");
        frames(0.05);
        amount = nullptr;
        for (int i = 0; context && i < context->GetNumDocuments() && !amount; ++i)
            amount = context->GetDocument(i)->GetElementById("amount");
        Expect(!amount, "a deal whose truth changed renders the screen: the store is gone");
    }

    // A slider's knob stays under the finger: the value has steps (0…4 over 400 px: 100 px each),
    // the finger has none. The template places the knob at the value; the behaviour moves it on
    // to the finger with a transform and takes that away on release.
    // The live slider: the drag moves change its number and knob without a render; the middle
    // (its shape) renders once; each move does a counted, small amount of work.
    Expect(host->LoadDocument("live-slide.rml"), "the live slider loads");
    frames(0.1);
    {
        Rml::Context *context = Rml::GetContext("storm-ui");
        const auto find = [context](const char *id) -> Rml::Element * {
            for (int i = 0; context && i < context->GetNumDocuments(); ++i)
                if (Rml::Element *found = context->GetDocument(i)->GetElementById(id))
                    return found;
            return nullptr;
        };
        // 400 px track at 100..500 (density 1): value 10 at 140, 30 at 220.
        host->Touch(TouchPhase::Down, 1, 140, 120, clock);
        frames(1.0 / 60.0);
        int renders = 0, refreshed = 0;
        double worst = 0.0;
        int moves = 0, touched = 0, fallbacks = 0, most_laid_out = 0, most_passes = 0, most_geometry = 0;
        for (float x = 150; x <= 300; x += 10) // values 12 … 50: below the middle
        {
            host->Touch(TouchPhase::Move, 1, x, 120, clock);
            clock += 1.0 / 60.0;
            host->Frame();
            const FrameStats moved = host->LastFrameStats();
            ++moves;
            renders += moved.rebuilds;
            refreshed += moved.refreshed;
            touched += moved.touched;
            fallbacks += moved.patch_fallbacks;
            most_laid_out = std::max(most_laid_out, moved.laid_out);
            most_passes = std::max(most_passes, moved.layout_passes);
            most_geometry = std::max(most_geometry, moved.geometry);
            worst = std::max(worst, moved.update_ms);
        }
        Rml::Element *num = find("live-num");
        Expect(renders == 0 && refreshed >= 10, "a drag below the middle renders the control " + std::to_string(renders) + " times (" +
                                                    std::to_string(refreshed) + " live refreshes)");
        Expect(num && num->GetInnerRML() == "50", "the number follows the finger: " + (num ? std::string(num->GetInnerRML()) : std::string("none")));
        // The cost of a move is counted, not timed (a loaded machine stretches milliseconds, not work): two live
        // elements changed (the number and the knob), no morph, one box pair laid out in one pass, one mesh.
        Expect(touched <= 2 * moves && fallbacks == 0 && most_laid_out <= 2 && most_passes <= 1 && most_geometry <= 1,
               "a drag move changes the number and the knob only: " + std::to_string(touched) + " elements changed in " +
                   std::to_string(moves) + " moves, " + std::to_string(fallbacks) + " morphs, at most " +
                   std::to_string(most_laid_out) + " boxes in " + std::to_string(most_passes) + " layout passes, " +
                   std::to_string(most_geometry) + " meshes per move");
        std::printf("a drag move's update: worst %.3f ms (reported, not checked)\n", worst);
        host->Touch(TouchPhase::Move, 1, 340, 120, clock); // 60: above the middle
        clock += 1.0 / 60.0;
        host->Frame();
        Expect(host->LastFrameStats().rebuilds == 1 && find("live-high") != nullptr, "crossing the middle renders once: its branch shows");
        host->Touch(TouchPhase::Up, 1, 340, 120, clock);
        frames(0.05);
        num = find("live-num");
        Expect(num && num->GetInnerRML() == "60", "after the release the render shows the value");
    }

    Expect(host->LoadDocument("slide.rml"), "the slider document loads");
    frames(0.1);
    {
        const auto find = [](const char *id) -> Rml::Element * {
            Rml::Context *context = Rml::GetContext("storm-ui");
            for (int i = 0; context && i < context->GetNumDocuments(); ++i)
                if (Rml::Element *found = context->GetDocument(i)->GetElementById(id))
                    return found;
            return nullptr;
        };
        // Where the knob is drawn: its laid-out place plus its transform.
        const auto knob_x = [&]() -> float {
            Rml::Element *knob = find("knob");
            if (!knob)
                return -1.0f;
            float x = knob->GetAbsoluteOffset(Rml::BoxArea::Border).x;
            if (const Rml::Property *property = knob->GetLocalProperty(Rml::PropertyId::Transform))
                if (const Rml::TransformPtr transform = property->Get<Rml::TransformPtr>())
                    for (const Rml::TransformPrimitive &primitive : transform->GetPrimitives())
                        if (primitive.type == Rml::TransformPrimitive::TRANSLATEX)
                            x += primitive.translate_x.values[0].number;
            return x;
        };
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, 100, 120);
        frames(0.05);
        float worst = 0.0f;
        int layouts = 0;
        for (float x = 101.0f; x <= 500.0f; x += 7.0f)
        {
            host->Touch(TouchPhase::Move, 1, x, 120);
            clock += 1.0 / 60.0;
            host->Frame();
            worst = std::max(worst, std::abs(knob_x() - x));
            // A frame in which the value did not change moves the knob without a rebuild or a layout.
            if (host->LastFrameStats().rebuilds == 0)
                layouts += host->LastFrameStats().layout_passes;
        }
        Expect(worst < 0.01f, "the knob is drawn under the finger in every frame of the drag (worst " + std::to_string(worst) + " px off)");
        Expect(clicks.size() == 4, "while the value still changes in steps: 4 changes over the drag (" + std::to_string(clicks.size()) + ")");
        Expect(layouts == 0, "a move between two values lays nothing out");
        host->Touch(TouchPhase::Up, 1, 493, 120);
        frames(0.05);
        Expect(std::abs(knob_x() - 500.0f) < 0.01f, "released, the knob is at its value (4 of 4: 500 px), not where the finger left it");
    }

    // On a page that scrolls vertically the first movement decides: up or down scrolls the page
    // and leaves the slider alone, sideways drags the slider; a tap sets it.
    Expect(host->LoadDocument("slide-list.rml"), "the slider on a scrolling page loads");
    frames(0.1);
    {
        Rml::Element *page = nullptr;
        Rml::Context *context = Rml::GetContext("storm-ui");
        for (int i = 0; context && i < context->GetNumDocuments(); ++i)
            if (Rml::Element *found = context->GetDocument(i)->GetElementById("page"))
                page = found;
        const auto swipe = [&](float x0, float y0, float x1, float y1) {
            clicks.clear();
            host->Touch(TouchPhase::Down, 1, x0, y0);
            frames(1.0 / 60.0);
            for (int i = 1; i <= 10; ++i)
            {
                host->Touch(TouchPhase::Move, 1, x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
                frames(1.0 / 60.0);
            }
            host->Touch(TouchPhase::Up, 1, x1, y1);
            frames(0.05);
        };
        swipe(300, 120, 305, 40);
        Expect(page && page->GetScrollTop() > 40 && clicks.empty(),
               "a vertical swipe that starts on the slider scrolls the page and changes nothing (" + std::to_string(clicks.size()) + " actions)");
        frames(1.0);
        if (page)
            page->SetScrollTop(0);
        frames(0.1);
        swipe(120, 120, 420, 125);
        Expect(page && page->GetScrollTop() == 0 && !clicks.empty() && clicks.back().find("commit") != std::string::npos,
               "a sideways drag on the slider moves it and not the page");
        clicks.clear();
        host->Touch(TouchPhase::Down, 1, 500, 120);
        frames(0.05);
        host->Touch(TouchPhase::Up, 1, 500, 120);
        frames(0.05);
        Expect(!clicks.empty() && clicks.back() == "listed-slide commit 4", "a tap sets the slider (" + (clicks.empty() ? std::string("nothing") : clicks.back()) + ")");
    }

    // A platform that gives the touches once per frame sends the moves as samples and has each
    // pointer placed for the frame's time (Host::PlaceTouches). A finger moving evenly (1000 px/s,
    // a sample every 7 ms) under frames every 1/120 s: placed at its newest sample the slider
    // moved 7 or 14 px per frame; placed for the frame's time it moves the same 8.33 px in each.
    Expect(host->LoadDocument("slide-fine.rml"), "the fine slider loads");
    frames(0.1);
    {
        const auto drag = [&](bool placed) {
            clicks.clear();
            const double start = clock;
            double sample = start;
            host->Touch(TouchPhase::Down, 1, 120, 120, start);
            std::vector<float> values;
            for (int frame = 1; frame <= 30; ++frame)
            {
                clock = start + frame / 120.0;
                while (sample + 0.007 <= clock)
                {
                    sample += 0.007;
                    host->Touch(placed ? TouchPhase::Sample : TouchPhase::Move, 1, 120.0f + float((sample - start) * 1000.0), 120, sample);
                }
                if (placed)
                    host->PlaceTouches(clock);
                host->Frame();
                if (!clicks.empty())
                    values.push_back(std::stof(clicks.back().substr(clicks.back().rfind(' ') + 1)) / 10.0f); // 10 units per px
                else
                    values.push_back(values.empty() ? 20.0f : values.back());
            }
            host->Touch(TouchPhase::Up, 1, 400, 120, clock);
            frames(0.05);
            float least = 1e9f, most = 0.0f;
            for (size_t i = 3; i < values.size(); ++i) // from the third frame: two samples are there
            {
                least = std::min(least, values[i] - values[i - 1]);
                most = std::max(most, values[i] - values[i - 1]);
            }
            return std::pair<float, float>(least, most);
        };
        const auto [raw_least, raw_most] = drag(false);
        Expect(raw_most - raw_least > 6.0f, "at its newest sample the slider moves in uneven steps (" + std::to_string(raw_least) + " to " +
                                                std::to_string(raw_most) + " px per frame): the check measures something");
        const auto [least, most] = drag(true);
        Expect(least > 8.1f && most < 8.6f, "placed for the frame's time it moves evenly: 8.33 px in every frame (" + std::to_string(least) + " to " +
                                                std::to_string(most) + ")");
    }

    // A row that opens is scrolled into view whole, clear of the bottom fade while more rows follow
    // (20 rows of 48 dp in a 200 dp list; the open one is 108 dp); a row that stays open is left
    // where the player scrolled it.
    Expect(host->LoadDocument("open-list.rml"), "the open-list document loads");
    frames(0.1);
    {
        Rml::Element *list = nullptr;
        if (Rml::Context *context = Rml::GetContext("storm-ui"))
            for (int i = 0; i < context->GetNumDocuments() && !list; ++i)
                list = context->GetDocument(i)->GetElementById("olist");
        Expect(list != nullptr, "the open list is there");
        const auto open = [&](int row, int amount = 0) {
            Expect(host->SetProps("opener", Json{{"open", row}, {"amount", amount}}.dump()), "row " + std::to_string(row) + " opens");
            frames(1.0);
            return list ? list->GetScrollTop() : -1.0f;
        };
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0f; };
        float top = open(1);
        Expect(near(top, 0.0f), "a row that opens inside the view does not move the list (" + std::to_string(top) + ")");
        top = open(3);
        Expect(near(top, 78.0f), "a row that opens past the bottom edge comes up whole, 26 dp of fade under it (" + std::to_string(top) + ", want 78)");
        list->SetScrollTop(0.0f);
        frames(0.1);
        top = open(3, 7);
        Expect(near(top, 0.0f), "the open row's amount changes: the list stays where the player scrolled it (" + std::to_string(top) + ")");
        top = open(15);
        Expect(near(top, 654.0f), "a row far below opens: the least scroll that shows it (" + std::to_string(top) + ", want 654)");
        top = open(19);
        Expect(near(top, 820.0f), "the last row opens: the list goes to its end, no fade to clear (" + std::to_string(top) + ", want 820)");
        top = open(0);
        Expect(near(top, 0.0f), "a row above the view opens: its top comes to the top edge (" + std::to_string(top) + ")");
    }

    // Preserved white space (patches/rmlui-preserved-line-breaks.patch): a blank line between
    // paragraphs keeps its height, and a space that does not fit at the end of a pre-wrap line
    // hangs there instead of starting the next line.
    if (Rml::Context *context = Rml::GetContext("storm-ui"))
    {
        // The pre-wrap text at every width from 80 to 200 px: at some of them a space meets the line end.
        std::string wraps;
        for (int width = 80; width <= 200; ++width)
            wraps += "<div class=\"wrap\" style=\"white-space: pre-wrap; width: " + std::to_string(width) + "px;\">aaaa bbbb cccc dddd eeee ffff gggg</div>";
        Rml::ElementDocument *doc = context->LoadDocumentFromMemory(
            "<rml><head><style>body { font-family: LatoLatin; font-size: 20px; line-height: 20px; }"
            " div { display: block; width: 300px; }</style></head><body>"
            "<div id=\"one\" style=\"white-space: pre-line;\">a\nb</div>"
            "<div id=\"two\" style=\"white-space: pre-line;\">a\n\nb</div>" +
            wraps + "</body></rml>");
        Expect(doc != nullptr, "the white-space document loads");
        if (doc)
        {
            doc->Show();
            context->Update();
            const float one = doc->GetElementById("one")->GetClientHeight(), two = doc->GetElementById("two")->GetClientHeight();
            Expect(two > one + 15, "a blank line between paragraphs keeps its height (" + std::to_string(one) + " vs " + std::to_string(two) + ")");
            Rml::ElementList wrapped;
            doc->GetElementsByClassName(wrapped, "wrap");
            int leading = 0;
            for (Rml::Element *element : wrapped)
                if (auto *text = rmlui_dynamic_cast<Rml::ElementText *>(element->GetFirstChild()))
                    for (size_t i = 1; i < text->GetLines().size(); ++i)
                        leading += text->GetLines()[i].text.rfind(' ', 0) == 0;
            Expect(wrapped.size() == 121, "the pre-wrap texts are laid out");
            Expect(leading == 0, "no wrapped pre-wrap line starts with a space (" + std::to_string(leading) + " do)");
            doc->Close();
            context->Update();
        }
    }

    // Release and debug builds (Config::show_errors, from the debuggable APK flag): a UI error is an error line in
    // both; only a debug build also puts it on the screen as a toast (its entrance keeps frames coming).
    for (const bool debug : {false, true})
    {
        host.reset();
        logged.clear();
        config.show_errors = debug;
        host = Host::Create(config, &error);
        Expect(host != nullptr, "a runtime starts for the build mode: " + error);
        if (!host)
            return 1;
        frames(0.5);
        const std::string mode = debug ? "debug: " : "release: ";
        Expect(!host->Prepare(delay), mode + "an idle host asks for no frame");
        Expect(!host->SetState("gameplay", "{not json"), mode + "malformed state is refused");
        Expect(std::count(logged.begin(), logged.end(), std::make_pair(LogLevel::Error, std::string("state channel gameplay: malformed JSON"))) == 1,
               mode + "the error is logged");
        Expect(drawing(0.0, 0.15) == debug, mode + (debug ? "the error is shown as a toast" : "nothing is shown"));
        frames(8.0);

        // The inline marks of a broken component: logged in both, drawn in debug builds only (the
        // document root's ui-debug class; release draws nothing in their place).
        logged.clear();
        const int markers = backend.Renderer().marker_textures;
        Expect(host->LoadDocument("marks.rml"), mode + "the document with a broken component loads");
        frames(0.1);
        Rml::Context *context = Rml::GetContext("storm-ui");
        Rml::ElementDocument *marks = nullptr;
        for (int d = 0; context && d < context->GetNumDocuments(); ++d)
            if (context->GetDocument(d)->GetElementById("broken"))
                marks = context->GetDocument(d);
        Expect(marks != nullptr, mode + "the marks document is open");
        if (!marks)
            continue;
        Rml::Element *broken = marks->GetElementById("broken");
        Expect(marks->IsClassSet("ui-debug") == debug, mode + (debug ? "the root is marked ui-debug" : "the root has no ui-debug class"));
        Expect(broken->IsClassSet("ui-error"), mode + "the broken component is marked ui-error");
        Expect(std::any_of(logged.begin(), logged.end(),
                           [](const auto &line) { return line.first == LogLevel::Error && line.second.find("Unknown component <c-nope>") != std::string::npos; }),
               mode + "the broken component is logged");
        Expect((broken->GetBox().GetSize().y >= 24.0f) == debug, mode + (debug ? "the stripes are drawn" : "nothing is drawn in its place"));
        Expect(std::any_of(logged.begin(), logged.end(),
                           [](const auto &line) { return line.first == LogLevel::Error && line.second.find("Icon 'i-nothing-here' is not in the icon pack") != std::string::npos; }),
               mode + "the missing icon is logged");
        Expect((backend.Renderer().marker_textures > markers) == debug, mode + (debug ? "the crossed icon is drawn" : "no crossed icon is drawn"));
    }

    host.reset();
    fs::remove_all(pack);
    if (failures)
        std::fprintf(stderr, "%d press check(s) failed\n", failures);
    else
        std::printf("press checks passed\n");
    return failures ? 1 : 0;
}
