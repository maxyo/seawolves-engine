// Real Router regression for a multi-revision officer assignment. No renderer or game process.
#include "../src/screens/screen.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace storm::ui;
using namespace storm::ui::screens;

namespace
{
void Require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
struct TestSurface final : Surface
{
    Json props;
    std::vector<std::string> commands, errors;
    void Show(const std::string &, const Json &view) override { props = view; }
    void Hide() override {}
    void Send(const Action &action) override { commands.push_back(EngineCommand(action)); }
    void RequestText(TextRequest, const std::string &) override {}
    void Toast(const std::string &) override {}
    void Alert(const std::string &) override {}
    void SetLanguage(const std::string &) override {}
    std::string Text(const std::string &key) override { return key; }
    void Log(const std::string &) override {}
    void Shown(const std::string &, const std::string &, const std::string &) const override {}
    void Error(const std::string &error) override { errors.push_back(error); }
    void Refuse(const std::string &) override {}
    double Now() const override { return 1.0; }
};
Widget &Find(InterfaceModel &model, const char *node, int index = -1)
{
    for (auto &widget : model.widgets)
        if (widget.node == node && (index == -1 || widget.index == index)) return widget;
    throw std::runtime_error(std::string("missing fixture widget ") + node);
}
Widget Make(const char *node, const char *type)
{
    Widget widget;
    widget.node = node;
    widget.type = type;
    widget.enabled = true;
    return widget;
}
InterfaceModel Chooser(const std::string &directory)
{
    std::ifstream file(directory + "/hero.json");
    Require(bool(file), "hero fixture unavailable");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    InterfaceModel model;
    std::string error;
    Require(ParseInterfaceModel(text, model, &error), "hero fixture parse failed");
    for (auto &widget : model.widgets)
        if (widget.node == "CHARACTERS_SCROLL") widget.value = widget.index == 2 ? "1" : "0";
    for (const char *node : {"ACCEPT_ADD_OFFICER", "CANCEL_ADD_OFFICER"})
    {
        auto button = Make(node, "button");
        button.actions = {"press"};
        model.widgets.push_back(button);
    }
    for (int i = 0; i != 2; ++i)
    {
        auto choice = Make("PASSENGERSLIST", "choice");
        choice.index = i;
        choice.row_id = "3:" + std::to_string(i);
        choice.value = i == 0 ? "1" : "0";
        choice.actions = {"select"};
        choice.label = i == 0 ? "Other" : "Officer A";
        model.widgets.push_back(choice);
        auto fact = Make("ANDROID_CANDIDATES", "fact");
        fact.index = i;
        fact.label = choice.label;
        // These are the actual android_hero.c AndroidHeroRow/AndroidHeroCandidates keys.
        fact.context = {{"index", std::to_string(i)}, {"character", std::to_string(10 + i)},
                        {"rank", "7"}, {"skill", "Defence"}, {"skillvalue", "10"}};
        model.widgets.push_back(fact);
    }
    return model;
}
void SelectSecond(InterfaceModel &model)
{
    Find(model, "PASSENGERSLIST", 0).value = "0";
    Find(model, "PASSENGERSLIST", 1).value = "1";
}
void Run(const std::string &directory, const std::string &scenario)
{
    TestSurface surface;
    Router router(surface);
    auto model = Chooser(directory);
    const bool immediate = scenario == "already-selected";
    if (immediate) SelectSecond(model);
    if (scenario == "missing-identity-at-click") Find(model, "ANDROID_CANDIDATES", 1).context.erase("character");
    auto initial = model;
    initial.widgets.erase(std::remove_if(initial.widgets.begin(), initial.widgets.end(), [](const Widget &w) {
        return w.node == "ACCEPT_ADD_OFFICER" || w.node == "CANCEL_ADD_OFFICER" ||
               w.node == "PASSENGERSLIST" || w.node == "ANDROID_CANDIDATES";
    }), initial.widgets.end());
    router.SetInterface(initial);
    router.OnEvent({"header-tabs", "select", "officers"});
    ++model.revision;
    router.SetInterface(model);
    Require(surface.props["dialog"]["kind"] == "assign", "real officer chooser did not mount");
    router.OnEvent({"candidate-1", "click", ""});
    if (scenario == "missing-identity-at-click")
    {
        Require(surface.commands.empty(), "missing stable character must fail before selection");
        return;
    }
    Require(surface.commands.size() == 1, "tap must issue exactly one stock action");
    if (immediate)
    {
        Require(surface.commands.back().find("\tACCEPT_ADD_OFFICER\t") != std::string::npos,
                "already selected eligible officer must retain immediate accept");
        return;
    }
    Require(surface.commands.back().find("\tPASSENGERSLIST\t1\tselect@") != std::string::npos,
            "unselected officer must use the native selection capability");
    SelectSecond(model);
    ++model.revision; // same screen/opening: Router preserves the pending Assign intent
    if (scenario == "replaced-character")
    {
        Find(model, "ANDROID_CANDIDATES", 1).context["character"] = "12";
        Find(model, "ANDROID_CANDIDATES", 1).label = "Officer B";
        Find(model, "PASSENGERSLIST", 1).label = "Officer B";
        Find(model, "PASSENGERSLIST", 1).row_id = "4:1";
    }
    if (scenario == "changed-post")
    {
        Find(model, "CHARACTERS_SCROLL", 2).value = "0";
        Find(model, "CHARACTERS_SCROLL", 1).value = "1";
    }
    if (scenario == "removed-candidate")
        model.widgets.erase(std::remove_if(model.widgets.begin(), model.widgets.end(), [](const Widget &w) {
            return w.node == "ANDROID_CANDIDATES" && w.index == 1;
        }), model.widgets.end());
    if (scenario == "missing-identity") Find(model, "ANDROID_CANDIDATES", 1).context.erase("character");
    if (scenario == "accept-disabled") Find(model, "ACCEPT_ADD_OFFICER").enabled = false;
    router.SetInterface(model);
    if (scenario == "unchanged")
    {
        Require(surface.commands.size() == 2 && surface.commands.back().find("\tACCEPT_ADD_OFFICER\t") != std::string::npos,
                "same character/post must advance from selection to stock accept");
    }
    else
    {
        Require(surface.commands.size() == 1, "stale or forbidden assignment sent an accept");
        Require(!surface.errors.empty(), "refused assignment must report its reason");
        ++model.revision;
        router.SetInterface(model);
        Require(surface.commands.size() == 1, "dropped intent must not revive on another revision");
    }
}
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    try
    {
        for (const char *scenario : {"unchanged", "already-selected", "replaced-character", "changed-post",
                                     "removed-candidate", "missing-identity", "missing-identity-at-click", "accept-disabled"})
        {
            Run(argv[1], scenario);
            std::printf("PASS %s\n", scenario);
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL officer assignment: %s\n", error.what());
        return 1;
    }
}
