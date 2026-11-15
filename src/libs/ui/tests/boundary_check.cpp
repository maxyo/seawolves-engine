// Host checks of the UI exception boundary (UI-1): a screen, a screen's handler, a state consumer
// or a HUD scene that throws must not end the game session. Each throw is an error line naming
// the screen or component and the exception, a danger toast for the player, and a logged
// fallback: the ported screen closes for the opening (the generic layout draws the interface),
// the generic layout throwing closes the interface, a HUD scene goes off for the session.
// Fake screens and scenes only; no GL context, no pack, no game data.
#include "../src/hud/hud.h"

#include <cstdio>
#include <stdexcept>

using namespace storm::ui;
using namespace storm::ui::screens;
using namespace storm::ui::hud;

static int failures = 0;

static void Expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

static bool Has(const std::vector<std::string> &lines, const std::string &part)
{
    for (const std::string &line : lines)
        if (line.find(part) != std::string::npos)
            return true;
    return false;
}

struct FakeSurface final : Surface
{
    std::string tag;
    Json props;
    std::vector<std::string> commands, errors, alerts;
    int hides = 0;

    void Show(const std::string &shown, const Json &view) override
    {
        tag = shown;
        props = view;
    }
    void Hide() override
    {
        tag.clear();
        ++hides;
    }
    void Send(const Action &action) override
    {
        commands.push_back(EngineCommand(action));
    }
    void RequestText(TextRequest, const std::string &) override
    {
    }
    void Toast(const std::string &) override
    {
    }
    void Alert(const std::string &text) override
    {
        alerts.push_back(text);
    }
    void SetLanguage(const std::string &) override
    {
    }
    std::string Text(const std::string &key) override
    {
        return "<" + key + ">";
    }
    void Log(const std::string &) override
    {
    }
    void Shown(const std::string &, const std::string &, const std::string &) const override
    {
    }
    void Error(const std::string &line) override
    {
        errors.push_back(line);
    }
    void Refuse(const std::string &) override
    {
    }
    double Now() const override
    {
        return 5.0;
    }
};

// What the fake screens do, set by each case.
static bool throw_in_build = false;
static bool throw_in_handler = false;
static int builds = 0;

class ThrowingScreen final : public Screen
{
  public:
    const char *Component() const override
    {
        return "s-throwing";
    }
    std::span<const NodeRule> Nodes() const override
    {
        static const NodeRule rules[] = {{"OK", "button", true}};
        return rules;
    }
    Json Build(const Admission &admitted, Bindings &bindings) override
    {
        ++builds;
        if (throw_in_build)
            throw std::runtime_error("bad widget in Build");
        bindings.On("ok", "", [](const Event &) {
            if (throw_in_handler)
                throw std::out_of_range("bad row in a handler");
        });
        return Json{{"label", admitted.Label("OK")}};
    }
};

static const ScreenEntry kScreens[] = {{"throwing", [] { return std::unique_ptr<Screen>(std::make_unique<ThrowingScreen>()); }}};

static InterfaceModel Model(uint64_t opening, uint64_t revision)
{
    InterfaceModel model;
    model.screen = "throwing";
    model.language = "english";
    model.opening = opening;
    model.revision = revision;
    Widget ok;
    ok.node = "OK";
    ok.type = "button";
    ok.label = "Fine";
    model.widgets.push_back(ok);
    return model;
}

static void CheckScreenBuild()
{
    FakeSurface surface;
    Router router(surface, kScreens);
    throw_in_build = true;
    router.SetInterface(Model(1, 1));
    Expect(router.Using() == "generic" && surface.tag == "s-generic", "a screen that throws in Build closes; the generic layout draws the interface");
    Expect(Has(surface.errors, "screen throwing (interface throwing #1 revision 1) threw in Build: bad widget in Build"),
           "the error line names the screen, the interface, the call and the exception");
    Expect(surface.alerts.size() == 1 && surface.alerts[0] == "<ui.error.screen>", "the player gets the localized danger toast");
    // Later revisions of the same opening stay on the generic layout: no throw, no new toast.
    const int before = builds;
    router.SetInterface(Model(1, 2));
    Expect(builds == before && router.Using() == "generic" && surface.alerts.size() == 1, "the failed screen stays closed for the opening");
    // A new opening tries the ported screen again.
    throw_in_build = false;
    router.SetInterface(Model(2, 1));
    Expect(router.Using() == "throwing" && surface.tag == "s-throwing", "a new opening draws the ported screen again");
}

static void CheckHandler()
{
    FakeSurface surface;
    Router router(surface, kScreens);
    throw_in_build = false;
    throw_in_handler = true;
    router.SetInterface(Model(1, 1));
    Expect(router.Using() == "throwing", "the ported screen is in use");
    Expect(router.OnEvent({"ok", "click", ""}), "the event was handled (by the boundary)");
    Expect(router.Using() == "generic" && Has(surface.errors, "threw in the handler of ok click: bad row in a handler"),
           "a handler that throws closes the screen, the error line names the event");
    Expect(surface.alerts.size() == 1, "one toast");
    throw_in_handler = false;
}

class ThrowingGeneric final : public Screen
{
  public:
    const char *Component() const override
    {
        return "s-generic";
    }
    std::span<const NodeRule> Nodes() const override
    {
        return {};
    }
    Json Build(const Admission &, Bindings &) override
    {
        throw std::logic_error("generic failed");
    }
};

// The ported screen throws and the generic layout that takes over throws too: nothing is left to
// draw the interface, so it closes through the engine's back.
static void CheckGenericFails()
{
    FakeSurface surface;
    Router router(surface, kScreens, [] { return std::unique_ptr<Screen>(std::make_unique<ThrowingGeneric>()); });
    throw_in_build = true;
    router.SetInterface(Model(3, 1));
    throw_in_build = false;
    Expect(!router.Active() && surface.tag.empty() && surface.hides > 0, "both layouts threw: the interface is closed");
    Expect(!surface.commands.empty() && surface.commands.back() == "back", "the game is asked to close it (the engine's back)");
    Expect(Has(surface.errors, "threw in Build: bad widget in Build; the screen is closed") &&
               Has(surface.errors, "screen generic (interface throwing #3 revision 1) threw in Build: generic failed; the interface is closed"),
           "both throws are error lines");
    Expect(surface.alerts.size() == 2, "a toast for each");
    // An interface no ported screen draws: the generic layout is its screen, a throw closes it at once.
    FakeSurface plain_surface;
    Router plain(plain_surface, kScreens, [] { return std::unique_ptr<Screen>(std::make_unique<ThrowingGeneric>()); });
    InterfaceModel other = Model(4, 1);
    other.screen = "unported";
    plain.SetInterface(other);
    Expect(!plain.Active() && plain_surface.commands.size() == 1 && plain_surface.alerts.size() == 1, "the generic layout alone: closed at once");
}

// Bad states of real channels: whatever a consumer does with them, the screen in use stays.
static void CheckConsumers()
{
    FakeSurface surface;
    Router router(surface, kScreens);
    throw_in_build = false;
    router.SetInterface(Model(1, 1));
    for (const char *channel : {"gameOver", "dialogue", "launcher", "titleIntent", "loadFailure", "appSettings", "gameplay", "ship", "worldMap"})
    {
        router.SetState(channel, Json::array({1, "two", nullptr}));
        router.SetState(channel, Json("not an object"));
    }
    Expect(router.Using() == "throwing" && surface.tag == "s-throwing", "a channel's bad state leaves the screen in use alone");
}

// --- HUD -------------------------------------------------------------------------------

struct FakeMount final : Mount
{
    View shown;
    bool visible = false;
    std::vector<std::string> commands, errors, alerts;
    double now = 0.0;

    bool Measure(const std::string &, const std::string &, Placement &) const override
    {
        return false;
    }
    bool ShowHud(const View &view) override
    {
        shown = view;
        visible = true;
        return true;
    }
    void HideHud() override
    {
        visible = false;
    }
    void Send(const Action &action) override
    {
        commands.push_back(EngineCommand(action));
    }
    void SetLanguage(const std::string &) override
    {
    }
    std::string Text(const std::string &key) override
    {
        return "<" + key + ">";
    }
    void Log(const std::string &) override
    {
    }
    void Error(const std::string &line) override
    {
        errors.push_back(line);
    }
    double Now() const override
    {
        return now;
    }
    bool InterfaceActive() const override
    {
        return false;
    }
    bool ScreenMounted() const override
    {
        return false;
    }
    void Alert(const std::string &text) override
    {
        alerts.push_back(text);
    }
};

static bool scene_throws_in_build = false, scene_throws_in_tick = false, scene_throws_in_handler = false;
static int scenes_created = 0;

class ThrowingScene final : public Scene
{
  public:
    ThrowingScene()
    {
        ++scenes_created;
    }
    void Build(const States &, View &view, Input &input) override
    {
        if (scene_throws_in_build)
            throw std::runtime_error("scene build failed");
        view.layout = "probe";
        view.elements["button"] = Json::object();
        input.On("button", "click", [](const Event &) {
            if (scene_throws_in_handler)
                throw std::runtime_error("scene handler failed");
        });
    }
    double Tick(const States &, double) override
    {
        if (scene_throws_in_tick)
            throw std::runtime_error("scene tick failed");
        return 3600.0;
    }
};

static const char *const kModes[] = {"explore"};
static const SceneEntry kScenes[] = {{"probe", kModes, [](Hud &) -> std::unique_ptr<Scene> { return std::make_unique<ThrowingScene>(); }}};

struct HudBench
{
    FakeMount mount;
    Hud hud{mount, kScenes};
    HudBench()
    {
        hud.Edit().gameplay.mode = "explore";
        hud.Edit().gameplay.revision = 1;
        Presentation &shown = hud.Edit().presentation;
        shown.known = true;
        shown.live = true;
        hud.Tick();
    }
};

static void CheckHudComponent(const char *where, bool &flag, bool event)
{
    HudBench bench;
    Expect(bench.hud.Visible() && !bench.hud.MenuOnly() && bench.mount.shown.layout == "probe", std::string(where) + ": the scene draws the HUD");
    flag = true;
    if (event)
        bench.hud.OnEvent({"button", "click", ""});
    else
        bench.hud.Rebuild();
    bench.hud.Tick();
    flag = false;
    Expect(Has(bench.mount.errors, std::string("HUD probe threw in ")) && Has(bench.mount.errors, "it is off for the rest of the session"),
           std::string(where) + ": the error line names the component, the call and the exception");
    Expect(bench.mount.alerts.size() == 1 && bench.mount.alerts[0] == "<ui.error.hud>", std::string(where) + ": the player gets the localized danger toast");
    Expect(bench.hud.MenuOnly() && bench.mount.shown.layout == "menu", std::string(where) + ": the game-menu button stays");
    // Off for the session: later states never make the scene again.
    const int created = scenes_created;
    bench.hud.Edit().gameplay.revision = 2;
    bench.hud.Tick();
    Expect(scenes_created == created && bench.hud.MenuOnly() && bench.mount.alerts.size() == 1, std::string(where) + ": the scene stays off");
}

int main()
{
    CheckScreenBuild();
    CheckHandler();
    CheckGenericFails();
    CheckConsumers();
    CheckHudComponent("Build", scene_throws_in_build, false);
    CheckHudComponent("Tick", scene_throws_in_tick, false);
    CheckHudComponent("event handler", scene_throws_in_handler, true);
    if (failures)
    {
        std::fprintf(stderr, "%d UI boundary check(s) failed\n", failures);
        return 1;
    }
    std::printf("UI boundary checks passed\n");
    return 0;
}
