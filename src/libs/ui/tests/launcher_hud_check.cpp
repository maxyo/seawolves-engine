// A standalone application host uses the same pack as the engine, but has no gameplay
// presenter. Preparing its first frame must not send HUD ownership to its listener.
// The default engine host still declares every presenter exactly once.
#include "runtime_stub.h"
#include "storm_ui/ui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace storm::ui;

static int Check(const char *pack, bool standalone)
{
    auto files = DirectoryFiles(pack);
    NoBackend backend;
    std::vector<std::string> commands;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 800;
    config.height = 400;
    if (standalone)
        config.game_hud = false; // StormUi's JNI host; engine_ui keeps the default.
    config.on_action = [&](const Action &action) { commands.push_back(EngineCommand(action)); };
    std::string error;
    auto host = Host::Create(config, &error);
    if (!host)
    {
        std::fprintf(stderr, "FAIL: host creation: %s\n", error.c_str());
        return 1;
    }
    // Android settles the runtime before delivering actions, before the first frame.
    double delay = 0;
    host->Prepare(delay);
    host->Frame();
    host->Prepare(delay);
    host->Frame();
    if (standalone)
    {
        if (!commands.empty())
        {
            std::fprintf(stderr, "FAIL: standalone host sent %s\n", commands.front().c_str());
            return 1;
        }
    }
    else
        for (const char *command : {"hudowner:land", "hudowner:sea", "hudowner:worldmap", "hudowner:spyglass"})
            if (std::count(commands.begin(), commands.end(), command) != 1)
            {
                std::fprintf(stderr, "FAIL: engine host must declare %s exactly once\n", command);
                return 1;
            }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const int failures = Check(argv[1], true) + Check(argv[1], false);
    if (!failures)
        std::puts("Standalone application and engine HUD declarations: PASS");
    return failures ? 1 : 0;
}
