// The perk tree: layout invariants on the stock perk graphs (perk_tree.hpp) and the Hero screen's
// event boundary (selection, «Требует» / «Открывает» jumps across groups, explicit stock learning).
// No game, no GPU.
#include "../src/screens/perk_tree.hpp"
#include "../src/screens/controls.h"
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace storm::ui;
using namespace storm::ui::screens;
namespace tree = storm::ui::screens::perk_tree;
static int failures = 0;
static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// The stock groups (PROGRAM/interface/perks/perks_init.c: IDs and condition attributes only, in the
// game's order); the personal group's Trustworthy needs the ship perk AdvancedCommerce.
static const std::vector<std::string> kSelf{"BasicDefense", "AdvancedDefense", "CriticalHit", "HPPlus", "Tireless", "EnergyPlus", "Sliding",
                                            "SwordplayProfessional", "HardHitter", "BladeDancer", "Gunman", "GunProfessional", "Grus", "IronWill",
                                            "ShipEscape", "SharedExperience", "Medic", "Trustworthy", "Alchemy", "AdvancedCommerce", "BasicCommerce"};
static const std::vector<tree::Edge> kSelfEdges{
    {"BasicDefense", "AdvancedDefense"}, {"AdvancedDefense", "HPPlus"},     {"Tireless", "EnergyPlus"},
    {"AdvancedDefense", "Sliding"},      {"CriticalHit", "SwordplayProfessional"}, {"AdvancedDefense", "SwordplayProfessional"},
    {"Tireless", "SwordplayProfessional"}, {"SwordplayProfessional", "HardHitter"}, {"SwordplayProfessional", "BladeDancer"},
    {"Gunman", "GunProfessional"},       {"IronWill", "ShipEscape"},     {"AdvancedDefense", "Medic"},
    {"AdvancedCommerce", "Trustworthy"}, {"IronWill", "Trustworthy"},    {"BasicCommerce", "AdvancedCommerce"}};
static const std::vector<std::string> kShip{
    "Brander", "Troopers", "LongRangeGrappling", "GrapplingProfessional", "MusketsShoot", "BasicCommerce", "AdvancedCommerce",
    "ProfessionalCommerce", "FastReload", "ImmediateReload", "HullDamageUp", "SailsDamageUp", "CrewDamageUp", "CriticalShoot",
    "LongRangeShoot", "CannonProfessional", "Carpenter", "LightRepair", "Builder", "BasicBattleState", "AdvancedBattleState",
    "InstantRepair", "SelfRepair", "ShipDefenseProfessional", "ShipSpeedUp", "ShipTurnRateUp", "StormProfessional", "WindCatcher",
    "SailsMan", "Turn180", "SailingProfessional", "Doctor1", "Doctor2", "SeaDogProfessional", "FlagPir", "FlagEng", "FlagFra", "FlagSpa",
    "FlagHol"};
static const std::vector<tree::Edge> kShipEdges{
    {"LongRangeGrappling", "GrapplingProfessional"}, {"MusketsShoot", "GrapplingProfessional"}, {"CrewDamageUp", "MusketsShoot"},
    {"BasicCommerce", "AdvancedCommerce"}, {"BasicCommerce", "ProfessionalCommerce"}, {"AdvancedCommerce", "ProfessionalCommerce"},
    {"FastReload", "ImmediateReload"}, {"HullDamageUp", "CriticalShoot"}, {"SailsDamageUp", "CriticalShoot"},
    {"CrewDamageUp", "CriticalShoot"}, {"CriticalShoot", "CannonProfessional"}, {"LongRangeShoot", "CannonProfessional"},
    {"FastReload", "CannonProfessional"}, {"Carpenter", "LightRepair"}, {"LightRepair", "Builder"},
    {"BasicBattleState", "AdvancedBattleState"}, {"BasicBattleState", "InstantRepair"}, {"Carpenter", "InstantRepair"},
    {"Builder", "SelfRepair"}, {"AdvancedBattleState", "SelfRepair"}, {"AdvancedBattleState", "ShipDefenseProfessional"},
    {"ShipSpeedUp", "Turn180"}, {"ShipTurnRateUp", "Turn180"}, {"WindCatcher", "Turn180"}, {"SailsMan", "SailingProfessional"},
    {"StormProfessional", "SailingProfessional"}, {"Turn180", "SailingProfessional"}, {"Doctor1", "Doctor2"},
    {"SailingProfessional", "SeaDogProfessional"}};

// What every layout must hold: icons in their own boxes on the canvas, prerequisites above their
// dependents, links that start under their source, end above their target at a point of their own,
// and never run down the same lane as another source's link or through an icon.
static void Invariants(const tree::Layout &layout, const char *name)
{
    char what[256];
    const auto expect = [&](bool ok, const char *rule) {
        std::snprintf(what, sizeof what, "%s: %s", name, rule);
        Expect(ok, what);
    };
    expect(layout.valid, "laid out");
    for (const auto &[id, place] : layout.nodes)
    {
        expect(place.left >= 0 && place.left + tree::Layout::slot <= tree::Layout::width && place.top >= 0 &&
                   place.top + tree::Layout::box <= layout.height,
               "every touch box on the canvas");
        for (const auto &[other, next] : layout.nodes)
            if (id != other)
                expect(place.left + tree::Layout::slot <= next.left || next.left + tree::Layout::slot <= place.left ||
                           place.top + tree::Layout::box <= next.top || next.top + tree::Layout::box <= place.top,
                       "touch boxes never overlap");
    }
    std::map<std::string, std::set<int>> arrivals;
    for (const auto &link : layout.links)
    {
        const auto &edge = layout.edges[link.edge];
        const auto &s = layout.nodes.at(edge.from), &t = layout.nodes.at(edge.to);
        expect(s.top < t.top, "a prerequisite stands above its dependent");
        expect(link.points.size() >= 2 && link.points.front().y >= s.top + tree::Layout::icon_top + tree::Layout::icon &&
                   link.points.back().y <= t.top + tree::Layout::icon_top,
               "a link leaves under its source icon and arrives above its target icon");
        expect(arrivals[edge.to].insert(link.points.back().x).second, "each link arrives at its own point");
        // A lane (two points straight down) passes no icon of the rows between.
        for (size_t i = 0; i + 1 < link.points.size(); ++i)
        {
            const auto a = link.points[i], b = link.points[i + 1];
            if (a.x != b.x || i == 0 || i + 2 == link.points.size())
                continue;
            for (const auto &[id, v] : layout.nodes)
                if (v.top + tree::Layout::box > a.y && v.top < b.y)
                    expect(a.x <= v.left + (tree::Layout::slot - tree::Layout::icon) / 2 || a.x >= v.left + (tree::Layout::slot + tree::Layout::icon) / 2,
                           "a lane passes between icons");
            for (const auto &other : layout.links)
                if (layout.edges[other.edge].from != edge.from)
                    for (size_t j = 1; j + 2 < other.points.size(); ++j)
                    {
                        const auto c = other.points[j], d = other.points[j + 1];
                        if (c.x == a.x && d.x == c.x)
                            expect(b.y <= c.y || d.y <= a.y, "lanes of different sources never overlap");
                    }
        }
    }
}

static void Topology()
{
    tree::Cache cache;
    const std::vector<std::string> ids{"Trustworthy", "IronWill", "AdvancedCommerce", "BasicCommerce", "BasicDefense"};
    const std::vector<tree::Edge> edges{{"BasicCommerce", "AdvancedCommerce"}, {"AdvancedCommerce", "Trustworthy"}, {"IronWill", "Trustworthy"}};
    const auto layout = cache.Get(ids, edges);
    Expect(layout.valid && layout.nodes.size() == 5 && layout.links.size() == 3, "real cross-group closure retained");
    Expect(layout.nodes.at("basiccommerce").top < layout.nodes.at("advancedcommerce").top &&
               layout.nodes.at("advancedcommerce").top < layout.nodes.at("trustworthy").top &&
               layout.nodes.at("ironwill").top == layout.nodes.at("advancedcommerce").top,
           "a row per tier, top to bottom; a perk whose dependents stand further down moves next to them");
    Expect(layout.nodes.at("basicdefense").top == layout.nodes.at("basiccommerce").top,
           "a perk without links takes a free slot in the top row, among the others (no caption)");
    cache.Get(ids, edges);
    auto shuffled = edges;
    std::reverse(shuffled.begin(), shuffled.end());
    cache.Get(ids, shuffled);
    Expect(cache.Builds() == 1, "the same topology (in any edge order) is laid out once");
    Invariants(layout, "closure");

    tree::Cache skip_cache;
    const auto &skipped = skip_cache.Get({"a", "b", "c"}, {{"a", "b"}, {"b", "c"}, {"a", "c"}});
    size_t lanes = 0;
    for (const auto &link : skipped.links)
        lanes += link.points.size() == 4;
    Expect(skipped.valid && lanes == 1, "a redundant stock link takes a lane past the middle row");
    Invariants(skipped, "skip");

    tree::Cache self_cache, ship_cache;
    const auto &self = self_cache.Get(kSelf, kSelfEdges);
    Invariants(self, "stock personal group");
    const auto &ship = ship_cache.Get(kShip, kShipEdges);
    Invariants(ship, "stock ship group");
    int top_row = 0;
    for (const auto &[id, place] : ship.nodes)
        top_row += place.top == ship.nodes.at("shipspeedup").top || place.top == ship.nodes.at("fastreload").top;
    Expect(ship.nodes.at("flaghol").top < ship.nodes.at("sailingprofessional").top && top_row > 4,
           "ship: the flags and lone perks fill free slots of the rows, no caption and no rows of their own");
    std::map<int, int> per_row;
    for (const auto &[id, place] : ship.nodes)
        ++per_row[place.top];
    int widest = 0;
    for (const auto &[row, count] : per_row)
        widest = std::max(widest, count);
    Expect(widest <= tree::Layout::slots && tree::Layout::width <= 500, "every stock tier fits one row of a 780 dp phone");

    auto cycle = edges;
    cycle.push_back({"Trustworthy", "BasicCommerce"});
    Expect(!cache.Get(ids, cycle).valid, "cyclic data cannot produce an invented partial graph");
    Expect(!cache.Get({"a"}, {{"missing", "a"}}).valid, "a missing source is not a root");
    Expect(!cache.Get({"A", "a"}, {}).valid, "case alias duplicate rejected");
    Expect(tree::CanLearn(true, true, false, 1, "stock-all-v1", true, {true, true}), "ALL two satisfied plus actual eligibility");
    Expect(!tree::CanLearn(true, true, false, 1, "stock-all-v1", true, {true, false}), "one missing AND prerequisite locks");
    Expect(!tree::CanLearn(true, true, false, 1, "any", true, {true, false}), "unexported OR rule never invented");
    Expect(!tree::CanLearn(true, true, false, 1, "stock-all-v1", false, {true, true}), "stock extra eligibility denies");
    Expect(!tree::CanLearn(false, true, false, 1, "stock-all-v1", true, {}), "external node cannot learn in wrong group");
    Expect(!tree::CanLearn(true, true, false, 0, "stock-all-v1", true, {}), "group points required");
    Expect(!tree::CanLearn(true, true, true, 1, "stock-all-v1", true, {}), "learned node cannot spend again");
}

struct SurfaceProbe final : Surface
{
    Json props;
    std::vector<Action> sent;
    std::vector<std::string> errors;
    void Show(const std::string &, const Json &p) override { props = p; }
    void Hide() override {}
    void Send(const Action &a) override { sent.push_back(a); }
    void RequestText(TextRequest, const std::string &) override {}
    void Toast(const std::string &) override {}
    void Alert(const std::string &) override {}
    void SetLanguage(const std::string &) override {}
    std::string Text(const std::string &key) override { return key; }
    std::vector<std::string> logs;
    void Log(const std::string &line) override { logs.push_back(line); }
    void Shown(const std::string &, const std::string &, const std::string &) const override {}
    void Error(const std::string &e) override { errors.push_back(e); }
    void Refuse(const std::string &) override {}
    double Now() const override { return 1.0; }
};
static Widget Fact(const std::string &id, const std::string &type, bool learned, bool eligible, int requirements = 0)
{
    Widget w;
    w.node = "ANDROID_PERKS";
    w.type = "fact";
    w.row_id = id;
    w.label = id;
    w.context = {{"label", id}, {"text", "Description: " + id}, {"type", type}, {"learned", learned ? "1" : "0"},
                 {"rule", "stock-all-v1"}, {"requirements", std::to_string(requirements)}, {"eligible", eligible ? "1" : "0"}};
    return w;
}
static Widget Edge(const char *from, const char *to, bool met)
{
    Widget w;
    w.node = "ANDROID_PERKEDGES";
    w.type = "fact";
    w.row_id = std::string(from) + "_" + to;
    w.context = {{"source", from}, {"target", to}, {"met", met ? "1" : "0"}, {"rule", "stock-all-v1"}};
    return w;
}
static Json Nodes(const Json &props)
{
    return props["tree"]["nodes"];
}
static Json Node(const Json &props, const std::string &id)
{
    for (const Json &node : Nodes(props))
        if (node["id"] == id)
            return node;
    return nullptr;
}
static std::string Selected(const Json &props)
{
    for (const Json &node : Nodes(props))
        if (node["selected"] == true)
            return node["id"];
    return "";
}

static void CheckScreen(const std::string &directory)
{
    std::ifstream file(directory + "/hero.json");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    InterfaceModel model;
    std::string error;
    if (!ParseInterfaceModel(text, model, &error))
    {
        Expect(false, "parse existing Hero model");
        return;
    }
    // The personal group: basicdefense and trustworthy in the table; trustworthy needs the ship perk
    // advancedcommerce (not learned) and the personal ironwill (learned).
    for (auto &w : model.widgets)
        if (w.node == "TABLE_PERKS" && w.index == 1)
        {
            w.row_id = "trustworthy";
            w.label = "Trustworthy";
            w.cells[1].text = "Trustworthy";
        }
    std::erase_if(model.widgets, [](const Widget &w) { return w.node == "ANDROID_PERKS" || w.node == "ANDROID_PERKEDGES"; });
    model.widgets.push_back(Fact("basicdefense", "self", false, true));
    model.widgets.push_back(Fact("trustworthy", "self", false, false, 2));
    auto iron = Fact("ironwill", "self", true, false);
    iron.context["label"] = "Iron, will";
    model.widgets.push_back(iron);
    model.widgets.push_back(Fact("advancedcommerce", "ship", false, true, 1));
    model.widgets.push_back(Fact("basiccommerce", "ship", true, false));
    model.widgets.push_back(Edge("basiccommerce", "advancedcommerce", true));
    model.widgets.push_back(Edge("advancedcommerce", "trustworthy", false));
    model.widgets.push_back(Edge("ironwill", "trustworthy", true));
    SurfaceProbe surface;
    Router router(surface);
    router.SetInterface(model);
    router.OnEvent({"header-tabs", "select", "perks1"});
    Expect(Nodes(surface.props).size() == 5 && surface.errors.empty(), "tree includes both real external ancestors");
    Expect(Selected(surface.props) == "basicdefense" && Node(surface.props, "basicdefense")["reveal"] == "self:basicdefense",
           "the group's first perk in reading order (top row, from the left) is selected on opening and brought into view");
    Expect(Node(surface.props, "advancedcommerce")["other"] == true && Node(surface.props, "advancedcommerce")["groupIcon"] == "ship" &&
               Node(surface.props, "advancedcommerce")["name"] == "advancedcommerce" && Node(surface.props, "ironwill")["other"] == false,
           "an other-group perk carries its group's glyph; the name is its long-press tooltip");
    const Json initial = surface.props["tree"];
    router.OnEvent({"perks", "row", "trustworthy"});
    Expect(surface.sent.empty(), "node selection sends no stock command or learn action");
    router.OnEvent({"perks", "row", "basicdefense"});
    Expect(Selected(surface.props) == "basicdefense" && Node(surface.props, "basicdefense")["reveal"] == "self:basicdefense" &&
               Node(surface.props, "trustworthy")["reveal"] == "",
           "a new selection is brought into view");
    router.OnEvent({"perks", "row", "trustworthy"});
    for (size_t i = 0; i < initial["nodes"].size(); ++i)
        Expect(initial["nodes"][i]["top"] == surface.props["tree"]["nodes"][i]["top"] &&
                   initial["nodes"][i]["left"] == surface.props["tree"]["nodes"][i]["left"],
               "selection preserves every graph position");
    {
        const Json &detail = surface.props["detail"];
        Expect(detail["requires"].size() == 2 && detail["learn"]["disabled"] == true, "detail lists every actual requirement");
        Expect(detail["requires"][0]["perk"] == "advancedcommerce" && detail["requires"][0]["met"] == false &&
                   detail["requires"][0]["groupIcon"] == "ship" && detail["requires"][1]["perk"] == "ironwill" &&
                   detail["requires"][1]["met"] == true && detail["requires"][1]["groupIcon"] == "",
               "missing requirements first; a row of the other group carries its glyph");
        Expect(!detail.contains("blocked") && detail["learn"]["label"] == "hero.learn" && detail["learn"]["disabled"] == true &&
                   !detail["learn"].contains("reason"),
               "a locked perk: the button disabled with no reason line or toast (what it needs is in its description icons)");
        Expect(surface.props["perkInfo"] == "hero.free_points 1 · hero.next_point 340 / 1\xC2\xA0" "000",
               "free points and the progress to the next one stay over the tree");
        const std::string links = surface.props["tree"]["links"];
        Expect(links.find("2 ") != std::string::npos && (links.find("0 ") != std::string::npos || links.find("1 ") != std::string::npos),
               "the selected perk's links are highlighted, the others drawn faint");
    }
    router.OnEvent({"perk-learn", "click", ""});
    Expect(surface.props["dialog"].is_null() && surface.sent.empty(), "a locked perk's hold learns nothing");
    router.OnEvent({"perk-learn", "refused", ""});
    Expect(surface.props["detail"]["requires"][0]["nudge"] == true && surface.props["detail"]["requires"][1]["nudge"] == false &&
               surface.props["perkInfoNudge"] == false && surface.sent.empty(),
           "a tap on the disabled «Изучить» makes the missing prerequisite pulse, not the met one");
    // «Требует» in the same group selects that perk.
    router.OnEvent({"perk-detail", "go", "ironwill"});
    Expect(surface.sent.empty() && Selected(surface.props) == "ironwill" && surface.props["detail"]["opens"].size() == 1 &&
               surface.props["detail"]["opens"][0]["perk"] == "trustworthy",
           "a requirement row jumps to it; its panel lists what it opens");
    router.OnEvent({"perk-detail", "go", "trustworthy"});
    Expect(Selected(surface.props) == "trustworthy", "an «Открывает» row jumps back");
    // An other-group node is read in place; its own tab learns it.
    router.OnEvent({"perks", "row", "advancedcommerce"});
    Expect(surface.props["detail"]["title"] == "advancedcommerce" && surface.props["detail"]["learn"].is_null() &&
               surface.props["detail"]["go"]["id"] == "perk-open-group" && surface.sent.empty(),
           "other-group node is readable but cannot learn using personal points");
    // «Требует» across groups: the game switches to the other group, where the perk is selected.
    router.OnEvent({"perks", "row", "trustworthy"});
    router.OnEvent({"perk-detail", "go", "advancedcommerce"});
    Expect(surface.sent.size() == 1 && EngineCommand(surface.sent.back()).find("TABBTN_2") != std::string::npos,
           "a requirement of the other group switches the game to that group");
    Expect(Selected(surface.props) == "advancedcommerce" && surface.props["tabs"][1]["selected"] == true &&
               surface.props["tabs"][2]["selected"] == false,
           "the perk is selected at once; the tab marker moves with the game's group, in the same frame as the tree");
    auto ship = model;
    ship.revision++;
    std::erase_if(ship.widgets, [](const Widget &w) { // the adapter exports the ship table's perks and their prerequisites
        return (w.node == "TABLE_PERKS" && w.index == 1) || (w.node == "ANDROID_PERKEDGES" && w.context.at("target") == "trustworthy") ||
               (w.node == "ANDROID_PERKS" && w.row_id != "advancedcommerce" && w.row_id != "basiccommerce");
    });
    for (auto &w : ship.widgets)
    {
        if (w.node == "TABBTN_1")
            w.value = "0";
        if (w.node == "TABBTN_2")
            w.value = "1";
        if (w.node == "TABLE_PERKS")
        {
            w.row_id = "AdvancedCommerce";
            w.label = "Advanced commerce";
            w.cells[1].text = "Advanced commerce";
        }
    }
    router.SetInterface(ship);
    Expect(Selected(surface.props) == "advancedcommerce" && Node(surface.props, "advancedcommerce")["other"] == false &&
               Node(surface.props, "advancedcommerce")["reveal"] == "ship:advancedcommerce" && !surface.props["detail"]["learn"].is_null() &&
               surface.props["tabs"][2]["selected"] == true &&
               Node(surface.props, "advancedcommerce")["revealTop"].get<int>() < Node(surface.props, "advancedcommerce")["top"].get<int>(),
           "in its own group the perk is selected, brought into view and learnable there");
    router.SetInterface(model);

    {
        // Without free points a reachable perk still reads open; only its learn button says why not.
        auto poor = model;
        for (auto &w : poor.widgets)
            if (w.node == "ANDROID_SUBJECT")
                w.context["freeself"] = "0";
        SurfaceProbe poor_surface;
        Router poor_router(poor_surface);
        poor_router.SetInterface(poor);
        poor_router.OnEvent({"header-tabs", "select", "perks1"});
        poor_router.OnEvent({"perks", "row", "basicdefense"});
        Expect(Node(poor_surface.props, "basicdefense")["state"] == "open" && Node(poor_surface.props, "trustworthy")["state"] == "locked" &&
                   poor_surface.props["detail"]["learn"]["disabled"] == true && poor_surface.props["detail"]["learn"]["label"] == "hero.learn" && !poor_surface.props["detail"].contains("blocked"),
               "no free points: reachable perks stay open, the button is disabled");
        poor_router.OnEvent({"perk-learn", "refused", ""});
        Expect(poor_surface.props["perkInfoNudge"] == true && poor_surface.sent.empty(), "without free points a refused tap makes the points line pulse");
    }
    // Incomplete facts: shown, never learnable.
    auto incomplete = model;
    incomplete.revision += 10;
    std::erase_if(incomplete.widgets, [](const Widget &w) { return w.node == "ANDROID_PERKEDGES" && w.context.at("source") == "advancedcommerce"; });
    for (auto &w : incomplete.widgets)
        if (w.node == "ANDROID_PERKS" && w.row_id == "trustworthy")
            w.context["eligible"] = "1";
    SurfaceProbe partial_surface;
    Router partial(partial_surface);
    partial.SetInterface(incomplete);
    partial.OnEvent({"header-tabs", "select", "perks1"});
    partial.OnEvent({"perks", "row", "trustworthy"});
    Expect(partial_surface.props["detail"]["learn"]["disabled"] == true && !partial_surface.errors.empty(),
           "truncated edge facts cannot unlock a perk, and say so in the log");
    for (int variant = 0; variant != 2; ++variant)
    {
        auto damaged = model;
        if (variant == 0)
            std::erase_if(damaged.widgets, [](const Widget &w) { return w.node == "ANDROID_PERKS" && w.row_id == "basicdefense"; });
        else
            for (auto &w : damaged.widgets)
                if (w.node == "ANDROID_PERKS" && w.row_id == "basicdefense")
                    w.context.erase("rule");
        SurfaceProbe damaged_surface;
        Router damaged_router(damaged_surface);
        damaged_router.SetInterface(damaged);
        damaged_router.OnEvent({"header-tabs", "select", "perks1"});
        damaged_router.OnEvent({"perks", "row", "basicdefense"});
        Expect(Node(damaged_surface.props, "basicdefense")["state"] == "unavailable",
               "incomplete structured node stays readable and is not mislabeled open/locked");
        Expect(damaged_surface.props["detail"]["title"] == "Basic defense", "missing fact preserves stock row title");
        Expect(damaged_surface.props["detail"]["learn"]["disabled"] == true && !damaged_surface.errors.empty(), "missing fact/rule disables learning loudly");
        damaged_router.OnEvent({"perk-learn", "click", ""});
        Expect(damaged_surface.props["dialog"].is_null() && damaged_surface.sent.empty(), "incomplete snapshot cannot enter confirmation");
    }
    {
        auto cyclic = model;
        cyclic.widgets.push_back(Edge("trustworthy", "basiccommerce", false));
        SurfaceProbe cyclic_surface;
        Router cyclic_router(cyclic_surface);
        cyclic_router.SetInterface(cyclic);
        cyclic_router.OnEvent({"header-tabs", "select", "perks1"});
        Expect(cyclic_surface.props["tree"].is_null() && cyclic_surface.props["detail"].is_null() && !cyclic_surface.errors.empty(),
               "a cyclic graph shows no invented tree and is logged");
    }
    // A delayed stock window with the same perk ID must not spend another person's
    // points or switch from personal to ship abilities underneath an old confirmation.
    for (int change = 0; change != 2; ++change)
    {
        SurfaceProbe stale_surface;
        Router stale(stale_surface);
        stale.SetInterface(model);
        stale.OnEvent({"header-tabs", "select", "perks1"});
        stale.OnEvent({"perks", "row", "basicdefense"});
        stale.OnEvent({"perk-learn", "click", ""});
        Expect(stale_surface.sent.size() == 1, "context control starts a genuine pending learn");
        auto reply = model;
        reply.revision++;
        for (auto &w : reply.widgets)
        {
            if (change == 0 && w.node == "ANDROID_SUBJECT")
                w.context["character"] = "9";
            if (change == 1 && w.node == "TABBTN_2")
                w.value = "1";
        }
        Widget offered;
        offered.node = "PERK_ACCEPT";
        offered.type = "button";
        offered.actions = {"press"};
        reply.widgets.push_back(offered);
        stale.SetInterface(reply);
        Expect(stale_surface.sent.size() == 1, "same perk in changed subject/group cannot consume delayed PERK_ACCEPT");
        std::erase_if(reply.widgets, [](const Widget &w) { return w.node == "PERK_ACCEPT"; });
        reply.revision++;
        if (change == 1)
            for (auto &w : reply.widgets)
                if (w.node == "ANDROID_SUBJECT")
                    w.context["freeship"] = "1";
        stale.SetInterface(reply);
        stale.OnEvent({"perks", "row", "basicdefense"});
        stale.OnEvent({"perk-learn", "click", ""});
        Expect(stale_surface.sent.size() == 2, "fresh explicit learn remains possible in new context");
    }
    // The completed hold on «Изучить» starts the stock learn at once (no card), and the log says so.
    router.OnEvent({"perks", "row", "basicdefense"});
    const size_t before_learn = surface.sent.size();
    router.OnEvent({"perk-learn", "click", ""});
    Expect(surface.sent.size() == before_learn + 1 && EngineCommand(surface.sent.back()).find("TABLE_PERKS\t0\tactivate") != std::string::npos &&
               surface.props["dialog"].is_null() && surface.props["detail"]["learn"]["busy"] == true,
           "the hold enters the stock perk window at once; no card, the button busy");
    bool logged = false;
    for (const auto &line : surface.logs)
        logged |= line.find("perk learn: basicdefense") != std::string::npos;
    Expect(logged, "the learn action is logged");
    router.OnEvent({"perk-learn", "click", ""});
    Expect(surface.sent.size() == before_learn + 1, "a second hold while the game learns sends nothing");
    model.revision += 100;
    Widget accept;
    accept.node = "PERK_ACCEPT";
    accept.type = "button";
    accept.label = "Learn";
    accept.enabled = false; // stock refusal
    Widget cancel;
    cancel.node = "PERK_CANCEL";
    cancel.type = "button";
    cancel.actions = {"press"};
    model.widgets.push_back(accept);
    model.widgets.push_back(cancel);
    router.SetInterface(model);
    for (const auto &a : surface.sent)
        Expect(EngineCommand(a).find("PERK_ACCEPT") == std::string::npos, "stock refusal is never bypassed");
}

int main(int argc, char **argv)
{
    Topology();
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: perk_tree_check <existing tests/models>\n");
        return 2;
    }
    CheckScreen(argv[1]);
    if (!failures)
        std::puts("PASS: perk tree layout, cross-group jumps, details and explicit stock learn boundary");
    return failures ? 1 : 0;
}
