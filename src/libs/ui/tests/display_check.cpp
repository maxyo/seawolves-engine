// Host checks for the player's display options (Host::SetDisplayOptions) with the whole runtime (a renderer that
// draws nothing, the synthetic pack tests/pack plus one component): «Размер интерфейса» grows every dp of the
// layout, the safe-area insets given in the display's dp shrink by it, and «Прозрачность кнопок» marks the HUD.
#include "runtime_stub.h"
#include "storm_ui/ui.h"

#include <nlohmann/json.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unistd.h>

using namespace storm::ui;
using Json = nlohmann::json;
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

static fs::path MakePack(const fs::path &base)
{
    const fs::path pack = fs::temp_directory_path() / ("storm-ui-display-" + std::to_string(::getpid()));
    fs::remove_all(pack);
    fs::copy(base, pack, fs::copy_options::recursive);
    Json components = Json::parse(Read(pack / "components.json"));
    components["components"].push_back(Json::parse(R"({
        "tag": "c-safe-probe", "props": {},
        "template": "<div id=\"safe\" data-left=\"{{safe.left}}\"></div>"})"));
    Write(pack / "components.json", components.dump());
    Write(pack / "display.rml",
          "<rml><head></head><body><div id=\"box\" style=\"display: block; width: 100dp; height: 10dp;\"></div>"
          "<c-safe-probe id=\"probe\"/></body></rml>");
    return pack;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: storm_ui_display_unit <tests/pack>\n");
        return 2;
    }
    const fs::path pack = MakePack(argv[1]);
    std::unique_ptr<FileSource> files = DirectoryFiles(pack.string());
    NoBackend backend;
    double clock = 0.0;
    Config config;
    config.strict_templates = true; // a template path that names nothing is an error
    config.files = files.get();
    config.backend = &backend;
    config.width = 1600;
    config.height = 720;
    config.density = 2.0f;
    config.clock = [&clock] { return clock; };
    std::string error;
    std::unique_ptr<Host> host = Host::Create(config, &error);
    Expect(host != nullptr, "the runtime starts: " + error);
    if (!host)
        return 1;
    Expect(host->LoadDocument("display.rml"), "the document loads");
    const auto frame = [&] {
        clock += 1.0 / 60.0;
        host->Frame();
        host->Frame();
    };
    Rml::ElementDocument *document = Rml::GetContext("storm-ui")->GetDocument(0);
    const auto width = [&] { return document->GetElementById("box")->GetBox().GetSize().x; };
    const auto safe_left = [&] {
        Rml::Element *safe = document->GetElementById("safe");
        return safe ? std::atof(safe->GetAttribute<Rml::String>("data-left", "-1").c_str()) : -1.0;
    };
    host->SetSafeArea(26.0f, 0.0f, 0.0f, 0.0f);
    frame();
    Expect(std::fabs(width() - 200.0f) < 0.5f, "100 dp at density 2 are 200 px: " + std::to_string(width()));
    Expect(std::fabs(safe_left() - 26.0) < 0.01, "the inset in dp as given: " + std::to_string(safe_left()));
    host->SetDisplayOptions(1.3f, 1.0f, false);
    frame();
    Expect(std::fabs(width() - 260.0f) < 0.5f, "«Размер интерфейса» 130 %: 100 dp are 260 px: " + std::to_string(width()));
    Expect(std::fabs(safe_left() - 20.0) < 0.01, "the inset given in the display's dp shrinks by the UI scale: " + std::to_string(safe_left()));
    host->Resize(1600, 720, 3.0f);
    frame();
    Expect(std::fabs(width() - 390.0f) < 0.5f, "a density change keeps the UI scale: " + std::to_string(width()));
    host->SetDisplayOptions(1.15f, 1.0f, false);
    frame();
    Expect(std::fabs(width() - 345.0f) < 0.5f, "«Размер интерфейса» 115 %: " + std::to_string(width()));
    host->SetDisplayOptions(1.0f, 0.5f, false);
    frame();
    Expect(std::fabs(width() - 300.0f) < 0.5f, "back to 100 %, the HUD buttons' opacity changes nothing in the layout");
    host.reset();
    fs::remove_all(pack);
    if (failures)
        std::fprintf(stderr, "%d display check(s) failed\n", failures);
    else
        std::printf("display checks passed\n");
    return failures ? 1 : 0;
}
