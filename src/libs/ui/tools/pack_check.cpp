// The pack contract check (UI-2): the real UI pack drawn with the real screen and HUD adapters
// on the recorded models and states, without a GL context and without game data.
//
//   storm_ui_pack_check --pack DIR --fixtures DIR [--only NAME]
//
// The pack (the application's mobile-ui/design, built by its build.py) and the adapters (this
// library) live in two repositories; this is where they meet before a device does. --fixtures is
// tests/ of this library: every models/*.json interface model is a case, and pack-contract.json
// adds the cases that need state channels (dialogue, the HUD scenes) and lists the ported screens
// without a fixture, each with its reason. A case fails on any error line of the host (an
// unknown element, prop or icon, a missing string, an unbound event, a rejected admission, a
// screen that threw) and on any element the runtime marked `ui-error`. Every ported screen must
// be drawn by a case or be listed as uncovered; a listed screen that a case draws is an error too
// (the list only shrinks). Exit 0 when every case is clean, 1 otherwise, 2 for bad arguments.
#include "../tests/runtime_stub.h"
#include "../src/screens/screen.h"
#include "storm_ui/ui.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace storm::ui;

namespace
{

struct Case
{
    std::string name;
    std::vector<std::pair<std::string, fs::path>> states; // channel, file
    fs::path interface;                                    // empty = none
    std::vector<std::string> covers;                       // screens drawn besides the interface's own
};

// Elements the runtime marked as broken (components.cpp: an unknown prop, a failed render).
int MarkedBroken(std::vector<std::string> &where)
{
    int count = 0;
    for (int c = 0; c < Rml::GetNumContexts(); ++c)
    {
        Rml::Context *context = Rml::GetContext(c);
        for (int d = 0; context && d < context->GetNumDocuments(); ++d)
        {
            Rml::ElementList marked;
            context->GetDocument(d)->QuerySelectorAll(marked, ".ui-error");
            for (Rml::Element *element : marked)
            {
                ++count;
                where.push_back(element->GetTagName() + (element->GetId().empty() ? std::string() : "#" + element->GetId()));
            }
        }
    }
    return count;
}

// Runs one case on a host of its own; prints what is wrong and returns false then.
bool Run(const fs::path &pack, const Case &c)
{
    std::unique_ptr<FileSource> files = DirectoryFiles(pack.string());
    NoBackend backend;
    double clock = 0.0;
    std::vector<std::string> errors, warnings;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 2400; // a common phone in landscape: 914 x 411 dp
    config.height = 1080;
    config.density = 2.625f;
    // No frame prediction check (Config::verify_unchanged) here: it compares draw commands, which a
    // renderer that draws nothing cannot make as the real one does. Captures check it through the
    // real backend (storm_ui_shot), the runtime checks in tests/frame_cost_check.cpp.
    config.clock = [&clock] { return clock; };
    config.on_log = [&](LogLevel level, const std::string &line) {
        if (level == LogLevel::Error)
            errors.push_back(line);
        else if (level == LogLevel::Warning)
            warnings.push_back(line);
    };
    std::string error;
    std::unique_ptr<Host> host = Host::Create(config, &error);
    if (!host)
    {
        std::printf("FAIL %s: the UI host does not start with this pack: %s\n", c.name.c_str(), error.c_str());
        return false;
    }
    const auto frames = [&](double seconds) {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0)
        {
            clock += 1.0 / 60.0;
            host->Frame();
        }
    };
    bool fed = true;
    for (const auto &[channel, file] : c.states)
    {
        if (!host->SetState(channel, Read(file)))
        {
            std::printf("FAIL %s: state %s=%s not accepted\n", c.name.c_str(), channel.c_str(), file.string().c_str());
            fed = false;
        }
        frames(0.05);
    }
    if (!c.interface.empty())
    {
        host->ExpectInterface();
        if (!host->SetInterface(Read(c.interface)))
        {
            std::printf("FAIL %s: interface model %s not accepted\n", c.name.c_str(), c.interface.string().c_str());
            fed = false;
        }
    }
    // Past the router's settle time (a model its screen rejects closes the interface, reported,
    // one second after the opening) and the toasts' first frames.
    frames(1.5);
    std::vector<std::string> marked;
    const int broken = MarkedBroken(marked);
    host.reset();
    const bool ok = fed && errors.empty() && broken == 0;
    std::printf("%s %s", ok ? "PASS" : "FAIL", c.name.c_str());
    if (!warnings.empty())
        std::printf(" (%zu warning%s)", warnings.size(), warnings.size() == 1 ? "" : "s");
    std::printf("\n");
    for (const std::string &line : errors)
        std::printf("  error: %s\n", line.c_str());
    for (const std::string &line : marked)
        std::printf("  marked ui-error: %s\n", line.c_str());
    if (!ok)
        for (const std::string &line : warnings)
            std::printf("  warning: %s\n", line.c_str());
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    fs::path pack, fixtures;
    std::string only;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--pack")
            pack = value;
        else if (key == "--fixtures")
            fixtures = value;
        else if (key == "--only")
            only = value;
        else
        {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }
    if (pack.empty() || fixtures.empty() || argc % 2 == 0)
    {
        std::fprintf(stderr, "usage: storm_ui_pack_check --pack DIR --fixtures DIR [--only NAME]\n");
        return 2;
    }
    const Json manifest = Json::parse(Read(fixtures / "pack-contract.json"), nullptr, false);
    if (!manifest.is_object() || !manifest.contains("cases") || !manifest.contains("uncovered"))
    {
        std::fprintf(stderr, "%s: expected {cases, uncovered}\n", (fixtures / "pack-contract.json").c_str());
        return 2;
    }

    std::vector<Case> cases;
    std::set<fs::path> used; // fixtures a manifest case names
    for (const Json &entry : manifest["cases"])
    {
        Case c;
        c.name = entry.value("name", std::string());
        for (const Json &pair : entry.value("states", Json::array()))
        {
            const std::string text = pair.get<std::string>();
            const size_t equals = text.find('=');
            if (equals == std::string::npos)
            {
                std::fprintf(stderr, "case %s: state '%s' is not channel=file\n", c.name.c_str(), text.c_str());
                return 2;
            }
            c.states.emplace_back(text.substr(0, equals), fixtures / text.substr(equals + 1));
            used.insert(c.states.back().second);
        }
        if (entry.contains("interface"))
        {
            c.interface = fixtures / entry["interface"].get<std::string>();
            used.insert(c.interface);
        }
        for (const Json &screen : entry.value("covers", Json::array()))
            c.covers.push_back(screen.get<std::string>());
        cases.push_back(std::move(c));
    }
    std::vector<fs::path> models;
    for (const fs::directory_entry &file : fs::directory_iterator(fixtures / "models"))
        if (file.path().extension() == ".json")
            models.push_back(file.path());
    std::sort(models.begin(), models.end());
    int problems = 0;
    for (const fs::path &model : models)
    {
        if (used.count(model))
            continue;
        InterfaceModel parsed;
        std::string error;
        if (!ParseInterfaceModel(Read(model), parsed, &error))
        {
            std::printf("FAIL %s: neither an interface model nor named by a case of pack-contract.json (%s)\n", model.filename().c_str(), error.c_str());
            ++problems;
            continue;
        }
        Case c;
        c.name = model.stem().string();
        c.interface = model;
        cases.push_back(std::move(c));
    }

    // Coverage: every ported screen is drawn by a case, or listed with its reason.
    std::set<std::string> covered;
    for (const Case &c : cases)
    {
        covered.insert(c.covers.begin(), c.covers.end());
        if (!c.interface.empty())
        {
            InterfaceModel parsed;
            if (ParseInterfaceModel(Read(c.interface), parsed))
                covered.insert(parsed.screen);
        }
    }
    const Json &uncovered = manifest["uncovered"];
    for (const screens::ScreenEntry &entry : screens::PortedScreens())
    {
        const bool listed = uncovered.contains(entry.id);
        if (covered.count(entry.id) && listed)
        {
            std::printf("FAIL coverage: %s is drawn by a case and still listed as uncovered in pack-contract.json\n", entry.id);
            ++problems;
        }
        else if (!covered.count(entry.id) && !listed)
        {
            std::printf("FAIL coverage: the ported screen %s has no case (add a recorded model, or list it as uncovered with the reason)\n", entry.id);
            ++problems;
        }
        else if (listed)
            std::printf("UNCOVERED %s: %s\n", entry.id, uncovered[entry.id].get<std::string>().c_str());
    }

    int failed = 0, ran = 0;
    for (const Case &c : cases)
    {
        if (!only.empty() && c.name != only)
            continue;
        ++ran;
        if (!Run(pack, c))
            ++failed;
    }
    std::printf("pack contract: %d of %d cases clean, %d coverage/manifest problem%s\n", ran - failed, ran, problems, problems == 1 ? "" : "s");
    return failed || problems ? 1 : 0;
}
