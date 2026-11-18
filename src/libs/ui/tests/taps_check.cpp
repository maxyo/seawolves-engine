// Host check: every tap a game interface's screen draws is one its screen maps (Host CheckTaps over
// Behavior::Taps and Router::Maps). An unmapped tap is reported when it is drawn, once, not first when the
// player makes it (and then only as "no mapping for event", the tap doing nothing).
// The case is the shipyard's captain picker (shipyard-captain.json): its rows were drawn with one fixed id
// and the row index as their `action` (an icon name: the crossed "missing icon" box), so no tap on a
// candidate reached the screen's candidate-N bindings and no captain could be chosen. With the test pack
// plus <s-shipyard> reduced to that sheet; no GL context, no game data.
#include "runtime_stub.h"
#include "../src/screens/screen.h"
#include "storm_ui/ui.h"

#include <RmlUi/Core.h>

#include <cstdio>
#include <unistd.h>

using namespace storm::ui;
namespace fs = std::filesystem;

static int failures = 0;

static void Expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

static std::string List(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &line : lines)
        out += (out.empty() ? "" : " | ") + line;
    return "[" + out + "]";
}

// The test pack plus a list row (press, as C-ListRow: the whole row, an action part when `action` names an
// icon) and the shipyard screen drawing its sheet's candidates with `row` as each row's attributes.
static fs::path MakePack(const fs::path &base, const fs::path &font, const std::string &row)
{
    const fs::path pack = fs::temp_directory_path() / ("storm-ui-taps-" + std::to_string(::getpid()));
    fs::remove_all(pack);
    fs::copy(base, pack, fs::copy_options::recursive);
    fs::copy_file(font, pack / "font.ttf");
    Json manifest = Json::parse(Read(pack / "pack.json"));
    manifest["fonts"] = Json::array({"font.ttf"});
    std::ofstream(pack / "pack.json") << manifest.dump();
    Json components = Json::parse(Read(pack / "components.json"));
    components["components"].push_back(Json::parse(R"({
        "tag": "c-row", "behaviors": ["press"],
        "props": {"title": {"type": "text", "default": ""}, "action": {"type": "text", "default": ""}},
        "template": "<div style=\"display: block; height: 40dp;\">{{title}}<t-if test=\"action\"><div data-action=\"action\" data-value=\"{{action}}\" style=\"display: block; width: 40dp; height: 40dp;\"></div></t-if></div>"})"));
    Json screen = {{"tag", "s-shipyard"}, {"props", Json::object()},
                   {"template", "<div style=\"position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-family: LatoLatin;\">"
                                "<t-if test=\"opt(sheet)\"><t-for each=\"sheet.candidates\" as=\"o\"><c-row " + row +
                                    " title=\"{{o.title}}\" style=\"display: block; width: 600dp; height: 40dp;\"/></t-for></t-if>"
                                // A row that only shows something (pointer-events: none) offers no tap.
                                "<c-row title=\"fact\" style=\"display: block; pointer-events: none;\"/></div>"}};
    // Every key of the shipyard's view is a prop of the screen (json, as the pack declares them).
    for (const char *key : {"title", "subject", "stats", "extras", "fleet", "offers", "noOffers", "detail", "empty", "sheet"})
        screen["props"][key] = {{"type", "json"}, {"default", nullptr}};
    components["components"].push_back(screen);
    std::ofstream(pack / "components.json") << components.dump();
    return pack;
}

struct Result
{
    std::vector<std::string> errors, actions, taps;
};

// Opens the captain picker with rows drawn as `row`, then taps the second candidate.
static Result Run(const fs::path &base, const fs::path &font, const fs::path &models, const std::string &row)
{
    Result result;
    const fs::path pack = MakePack(base, font, row);
    std::unique_ptr<FileSource> files = DirectoryFiles(pack.string());
    NoBackend backend;
    double clock = 0.0;
    Config config;
    config.strict_templates = true; // the contract checks: a template path that names nothing, a tap that maps to nothing
    config.files = files.get();
    config.backend = &backend;
    config.width = 800;
    config.height = 400;
    config.clock = [&clock] { return clock; };
    config.log_taps = true; // the tap map of device crawls (tools/ui_crawl)
    config.on_log = [&result](LogLevel level, const std::string &line) {
        if (level == LogLevel::Error && line.rfind("screen ", 0) == 0) // the reduced pack has no strings of its own
            result.errors.push_back(line);
        if (line.rfind("taps ", 0) == 0)
            result.taps.push_back(line);
    };
    config.on_action = [&result](const Action &action) {
        if (action.screen == "shipyard") // the game's widgets (not the click sound, not the host's own)
            result.actions.push_back(action.node + " " + std::to_string(action.index) + " " + action.verb);
    };
    std::string error;
    std::unique_ptr<Host> host = Host::Create(config, &error);
    Expect(host != nullptr, "the runtime starts: " + error);
    if (!host)
        return result;
    const auto frames = [&](double seconds) {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0)
        {
            clock += 1.0 / 60.0;
            host->Frame();
        }
    };
    host->ExpectInterface();
    Expect(host->SetInterface(Read(models / "shipyard-captain.json")), "the captain picker is shown");
    frames(0.2);
    Rml::ElementList rows;
    if (Rml::Context *context = Rml::GetContext(0))
        context->GetRootElement()->QuerySelectorAll(rows, "c-row[id]");
    Expect(rows.size() == 3, "three candidates are drawn: " + std::to_string(rows.size()));
    if (rows.size() >= 2)
    {
        const Rml::Vector2f at = rows[1]->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(100.0f, 20.0f);
        host->Touch(TouchPhase::Down, 1, at.x, at.y, clock);
        frames(1.0 / 60.0);
        host->Touch(TouchPhase::Up, 1, at.x, at.y, clock);
        frames(0.1);
    }
    host.reset();
    fs::remove_all(pack);
    return result;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: storm_ui_taps_unit <tests/models> <tests/pack> <font.ttf>\n");
        return 2;
    }
    const fs::path models = argv[1], base = argv[2], font = argv[3];

    // The rows as the pack drew them: one id for all, the index as the action icon.
    const Result broken = Run(base, font, models, "id=\"candidates\" action=\"{{o_i}}\"");
    const std::vector<std::string> expected = {
        "screen shipyard: c-row candidates action is drawn with no mapping: a tap on it does nothing",
        "screen shipyard: c-row candidates click is drawn with no mapping: a tap on it does nothing",
        "screen shipyard: no mapping for event candidates click "};
    Expect(broken.errors == expected, "a row whose taps map to nothing is reported when drawn, once: " + List(broken.errors));
    Expect(broken.actions.empty(), "and its tap reaches nothing: " + List(broken.actions));

    // Each row with the id the screen binds (candidate-N): drawn without an error, the tap selects that candidate.
    const Result bound = Run(base, font, models, "id=\"{{o.id}}\"");
    Expect(bound.errors.empty(), "rows whose taps are mapped: no errors: " + List(bound.errors));
    Expect(bound.actions == std::vector<std::string>{"PASSENGERSLIST 1 yard@7:40:select"}, "a tap on the second candidate selects it in the game's list: " + List(bound.actions));

    // The tap map (Config::log_taps): one block, a line per candidate row with the point a tap there uses, reached
    // by that row; the row that only shows something is not in it.
    int rows = 0, begins = 0;
    for (const std::string &line : bound.taps)
    {
        begins += line.rfind("taps begin ", 0) == 0;
        if (line.find(" shipyard c-row ") != std::string::npos && line.find(" click ") != std::string::npos)
            rows += line.find(" reach=1 dis=0 ") != std::string::npos && line.find(" at=-") == std::string::npos;
        Expect(line.find("\"fact") == std::string::npos, "a row with pointer-events: none is not in the tap map: " + line);
    }
    Expect(begins >= 1 && rows == 3, "the tap map lists the three candidate rows, reachable: " + List(bound.taps));

    if (failures)
    {
        std::fprintf(stderr, "%d tap check(s) failed\n", failures);
        return 1;
    }
    std::printf("taps: a screen's drawn tap with no mapping is reported when drawn; the captain picker's rows select their candidate\n");
    return 0;
}
