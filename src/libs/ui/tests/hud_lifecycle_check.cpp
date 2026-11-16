// Full runtime regression: removing held land controls must cancel them while their scene
// and its bindings still exist. Synthetic pack/states, no graphics context or game files.
#include "hud_bench.h"

#include <filesystem>
#include <unistd.h>

static void ChangeScene(const std::string &pack, bool combat)
{
    auto files = DirectoryFiles(pack);
    NoBackend backend;
    std::vector<std::string> errors, commands;
    double clock = 0.0;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 800;
    config.height = 400;
    config.clock = [&] { return clock; };
    config.on_log = [&](LogLevel level, const std::string &line) {
        if (level == LogLevel::Error)
            errors.push_back(line);
    };
    config.on_action = [&](const Action &action) { commands.push_back(EngineCommand(action)); };
    std::string error;
    auto host = Host::Create(config, &error);
    Expect(host != nullptr, "runtime starts: " + error);
    if (!host)
        return;
    const auto feed = [&](const char *channel, const char *file) { Expect(host->SetState(channel, Text(file)), file); };
    const auto frame = [&] { clock += 1.0 / 60.0; host->Frame(); };
    feed("presentation", "presentation-game");
    feed("gameplay", combat ? "gameplay-melee" : "gameplay-explore");
    feed("combat", combat ? "combat-melee" : "combat-explore");
    feed("vitals", "vitals");
    feed("worldInfo", "world-info");
    feed("contextAction", "context-door");
    feed("landCommands", "land-commands-idle");
    frame();
    frame();
    errors.clear();
    if (combat)
    {
        host->Touch(TouchPhase::Down, 1, 110, 132); // block
        host->Touch(TouchPhase::Down, 2, 672, 282); // strike gesture
    }
    else
    {
        host->Touch(TouchPhase::Down, 1, 90, 320);
        host->Touch(TouchPhase::Move, 1, 130, 320); // movement held
    }
    const auto held = commands.size();
    feed("gameplay", "gameplay-orders"); // unknown mode, only game-menu button remains
    frame();
    frame();
    Expect(errors.empty(), std::string(combat ? "combat" : "movement") + " cancelled through its original scene without orphan events" +
           (errors.empty() ? "" : ": " + errors.back()));
    Expect(!host->WantsTouch(90, 320) && host->WantsTouch(766, 34), "only the menu button remains");
    if (!combat)
        Expect(commands.size() > held && std::any_of(commands.begin() + held, commands.end(), [](const std::string &command) {
            return command.rfind("move:", 0) == 0 && command.ends_with("\t0\t0\t0");
        }), "leaving the movement scene sends the native stop");
    const auto ended = commands.size();
    host->Touch(TouchPhase::Up, 1, 130, 320);
    if (combat)
        host->Touch(TouchPhase::Up, 2, 672, 282);
    frame();
    Expect(commands.size() == ended && errors.empty(), "late finger releases neither act nor report stale controls");
}

// «Под левую руку»: the HUD mounts the pack's mirrored placement; touches follow the elements, and back.
static void LeftHanded(const std::string &pack)
{
    auto files = DirectoryFiles(pack);
    NoBackend backend;
    std::vector<std::string> errors;
    double clock = 0.0;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 800;
    config.height = 400;
    config.clock = [&] { return clock; };
    config.on_log = [&](LogLevel level, const std::string &line) {
        if (level == LogLevel::Error)
            errors.push_back(line);
    };
    config.on_action = [](const Action &) {};
    std::string error;
    auto host = Host::Create(config, &error);
    Expect(host != nullptr, "runtime starts: " + error);
    if (!host)
        return;
    const auto feed = [&](const char *channel, const char *file) { Expect(host->SetState(channel, Text(file)), file); };
    const auto frame = [&] {
        for (int i = 0; i < 4; ++i)
        {
            clock += 1.0 / 60.0;
            host->Frame();
        }
    };
    feed("presentation", "presentation-game");
    feed("gameplay", "gameplay-explore");
    feed("combat", "combat-explore");
    feed("vitals", "vitals");
    feed("worldInfo", "world-info");
    feed("contextAction", "context-door");
    feed("landCommands", "land-commands-idle");
    frame();
    Expect(host->WantsTouch(90, 320) && host->WantsTouch(766, 34), "right-handed: the move zone left, the menu button top right");
    host->SetDisplayOptions(1.0f, 1.0f, true);
    frame();
    Expect(host->WantsTouch(800 - 90, 320) && host->WantsTouch(34, 34) && !host->WantsTouch(90, 320),
           "left-handed: the move zone right, the menu button top left");
    host->SetDisplayOptions(1.0f, 1.0f, false);
    frame();
    Expect(host->WantsTouch(90, 320) && host->WantsTouch(766, 34), "back to right-handed");
    Expect(errors.empty(), "no errors" + (errors.empty() ? std::string() : ": " + errors.back()));
}

int main(int argc, char **argv)
{
    states_directory = argc > 1 ? argv[1] : "tests/states";
    const std::filesystem::path base = argc > 2 ? argv[2] : "tests/pack";
    const auto pack = std::filesystem::temp_directory_path() / ("storm-ui-lifecycle-" + std::to_string(::getpid()));
    std::filesystem::copy(base, pack, std::filesystem::copy_options::recursive);
    // The runtime's fallback menu uses c-hud-button; the small test pack names its plain
    // button c-button. Give the fallback the same synthetic implementation.
    {
        std::ifstream file(pack / "components.json");
        Json components = Json::parse(file);
        Json button;
        for (const auto &entry : components["components"])
            if (entry["tag"] == "c-button")
                button = entry;
        button["tag"] = "c-hud-button";
        components["components"].push_back(button);
        std::ofstream(pack / "components.json") << components.dump();
    }
    LeftHanded(pack.string());
    ChangeScene(pack.string(), false);
    ChangeScene(pack.string(), true);
    std::filesystem::remove_all(pack);
    if (!failures)
        std::printf("HUD lifecycle checks passed\n");
    return failures ? 1 : 0;
}
