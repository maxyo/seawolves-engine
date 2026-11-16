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

// Pieces of one source perk may share its lane; pieces of different sources never run along each other.
static bool Disjoint(const tree::Layout &layout, const std::vector<int> &a, const std::vector<int> &b)
{
    for (int x : a)
        for (int y : b)
            if (layout.edges[x].from == layout.edges[y].from)
                return false;
    return true;
}

// What every layout must hold: nodes on their own rows, links distinct, lines inside the canvas.
static void Invariants(const tree::Layout &layout, const char *name)
{
    char what[256];
    const auto expect = [&](bool ok, const char *rule) {
        std::snprintf(what, sizeof what, "%s: %s", name, rule);
        Expect(ok, what);
    };
    expect(layout.valid, "laid out");
    expect(layout.columns.size() <= 4, "at most four columns: fits a 780 dp phone beside the panel");
    for (const auto &column : layout.columns)
        for (const auto &a : column.nodes)
            for (const auto &b : column.nodes)
                if (a != b)
                    expect(std::abs(layout.nodes.at(a).top - layout.nodes.at(b).top) >= tree::Layout::node_height,
                           "nodes of a column never overlap");
    for (const auto &edge : layout.edges)
        expect(layout.nodes.at(edge.from).column < layout.nodes.at(edge.to).column, "a prerequisite stands left of its dependent");
    std::map<int, int> arrows;
    for (size_t g = 0; g < layout.gutters.size(); ++g)
    {
        const auto &gutter = layout.gutters[g];
        for (const auto &arrow : gutter.arrows)
            ++arrows[arrow.edge];
        for (const auto &line : gutter.lines)
            expect(line.x >= 0 && line.x + line.width <= gutter.width && line.y >= 0 && line.y + line.height <= layout.height,
                   "gutter lines stay inside the gutter and the canvas");
        // Distinct links: two pieces of different sources never run along each other.
        for (size_t i = 0; i < gutter.lines.size(); ++i)
            for (size_t j = i + 1; j < gutter.lines.size(); ++j)
            {
                const auto &a = gutter.lines[i], &b = gutter.lines[j];
                if (!Disjoint(layout, a.edges, b.edges))
                    continue;
                const bool flat_a = a.height == tree::Layout::thickness && a.width > a.height, flat_b = b.height == tree::Layout::thickness && b.width > b.height;
                if (flat_a && flat_b && a.y == b.y)
                    expect(a.x + a.width <= b.x || b.x + b.width <= a.x, "horizontal pieces of different links never overlap");
                if (!flat_a && !flat_b && a.x == b.x)
                    expect(a.y + a.height <= b.y || b.y + b.height <= a.y, "vertical pieces of different links never share a lane");
            }
    }
    for (size_t e = 0; e < layout.edges.size(); ++e)
        expect(arrows[static_cast<int>(e)] == 1, "every link ends in one arrowhead");
    // Arrivals on a target's edge are each at their own height.
    std::map<std::string, std::set<int>> arrivals;
    for (const auto &gutter : layout.gutters)
        for (const auto &arrow : gutter.arrows)
            expect(arrivals[layout.edges[arrow.edge].to].insert(arrow.y).second, "each link arrives at its own height");
    // Crossings of skipped columns run between node rows, never through a node.
    for (size_t c = 0; c < layout.columns.size(); ++c)
        for (const auto &line : layout.columns[c].lines)
            for (const auto &id : layout.columns[c].nodes)
            {
                const int top = layout.nodes.at(id).top;
                expect(line.full && (line.y + line.height <= top || top + tree::Layout::node_height <= line.y), "a crossing passes between nodes");
            }
}

static void Topology()
{
    tree::Cache cache;
    const std::vector<std::string> ids{"Trustworthy", "IronWill", "AdvancedCommerce", "BasicCommerce", "BasicDefense"};
    const std::vector<tree::Edge> edges{{"BasicCommerce", "AdvancedCommerce"}, {"AdvancedCommerce", "Trustworthy"}, {"IronWill", "Trustworthy"}};
    const auto layout = cache.Get(ids, edges);
    Expect(layout.valid && layout.nodes.size() == 5 && layout.edges.size() == 3, "real cross-group closure retained");
    Expect(layout.nodes.at("basiccommerce").column == 0 && layout.nodes.at("advancedcommerce").column == 1 &&
               layout.nodes.at("trustworthy").column == 2 && layout.nodes.at("ironwill").column == 1,
           "a column per tier; a perk whose dependents stand further right moves next to them");
    Expect(layout.caption_top > 0 && layout.nodes.at("basicdefense").top > layout.caption_top, "unlinked perks below the blocks, under a caption");
    cache.Get(ids, edges);
    auto shuffled = edges;
    std::reverse(shuffled.begin(), shuffled.end());
    cache.Get(ids, shuffled);
    Expect(cache.Builds() == 1, "the same topology (in any edge order) is laid out once");
    Invariants(layout, "closure");

    tree::Cache skip_cache;
    const auto skipped = skip_cache.Get({"a", "b", "c"}, {{"a", "b"}, {"b", "c"}, {"a", "c"}});
    Expect(skipped.valid && skipped.columns[1].lines.size() == 1, "a redundant stock link crosses the middle column once");
    Invariants(skipped, "skip");

    tree::Cache self_cache, ship_cache;
    const auto &self = self_cache.Get(kSelf, kSelfEdges);
    Invariants(self, "stock personal group");
    Expect(self.columns.size() == 4 && self.nodes.at("hardhitter").column == 3, "personal: four tiers");
    const auto &ship = ship_cache.Get(kShip, kShipEdges);
    Invariants(ship, "stock ship group");
    Expect(ship.columns.size() == 4 && ship.caption_top > 0, "ship: four tiers, the flags and lone perks packed under a caption");
    int width = 0;
    for (const auto &gutter : ship.gutters)
        width += gutter.width;
    Expect(width <= 72, "the gutters leave the columns their room");

    auto cycle = edges;
    cycle.push_back({"Trustworthy", "BasicCommerce"});
    Expect(!cache.Get(ids, cycle).valid, "cyclic data cannot produce an invented partial graph");
    Expect(!cache.Get({"a"}, {{"missing", "a"}}).valid, "a missing source is not a root");
    Expect(!cache.Get({"A", "a"}, {}).valid, "case alias duplicate rejected");
    Expect(tree::LongestWords({"Профессиональный фехтовальщик", "Тяжёлая рука"}, 2) ==
               std::vector<std::string>({"Профессиональный", "фехтовальщик"}),
           "the longest words by letters size a column");
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
    void Log(const std::string &) override {}
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
    Json nodes = Json::array();
    for (const Json &column : props["tree"]["columns"])
        for (const Json &node : column["nodes"])
            nodes.push_back(node);
    return nodes;
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
    Expect(Selected(surface.props) == "trustworthy" && Node(surface.props, "trustworthy")["reveal"] == "self:trustworthy",
           "the group's first perk in reading order is selected on opening and brought into view");
    Expect(Node(surface.props, "advancedcommerce")["external"] == true && Node(surface.props, "advancedcommerce")["group"] == "Ship" &&
               Node(surface.props, "advancedcommerce")["groupIcon"] == "ship" && Node(surface.props, "advancedcommerce")["name"] == "advancedcommerce",
           "an other-group perk is marked by its group, apart from its name");
    const Json initial = surface.props["tree"];
    router.OnEvent({"perks", "row", "trustworthy"});
    Expect(surface.sent.empty(), "node selection sends no stock command or learn action");
    router.OnEvent({"perks", "row", "basicdefense"});
    Expect(Selected(surface.props) == "basicdefense" && Node(surface.props, "basicdefense")["reveal"] == "self:basicdefense" &&
               Node(surface.props, "trustworthy")["reveal"] == "",
           "a new selection is brought into view");
    router.OnEvent({"perks", "row", "trustworthy"});
    for (size_t c = 0; c < initial["columns"].size(); ++c)
        for (size_t i = 0; i < initial["columns"][c]["nodes"].size(); ++i)
            Expect(initial["columns"][c]["nodes"][i]["top"] == surface.props["tree"]["columns"][c]["nodes"][i]["top"],
                   "selection preserves every graph position");
    {
        const Json &detail = surface.props["detail"];
        Expect(detail["requires"].size() == 2 && detail["learn"]["disabled"] == true, "detail lists every actual requirement");
        Expect(detail["requires"][0]["id"] == "perk-go-advancedcommerce" && detail["requires"][0]["met"] == false &&
                   detail["requires"][0]["meta"] == "hero.not_learned" && detail["requires"][0]["groupHead"] == "Ship" &&
                   detail["requires"][0]["groupIcon"] == "ship" && detail["requires"][1]["id"] == "perk-go-ironwill" &&
                   detail["requires"][1]["meta"] == "hero.learned" && detail["requires"][1]["groupHead"] == "Personal",
               "missing requirements first; a caption with glyph and name wherever the group changes");
        Expect(detail["missing"] == "hero.learn_first advancedcommerce", "a locked perk says what is missing");
        Expect(detail["learn"]["sub"] == "hero.cost 1 · hero.free_points 1" && detail["progress"] == "hero.next_point 340 / 1\xC2\xA0" "000",
               "cost and free points are separate");
        bool focus_in = false, dim = false;
        for (const Json &gutter : surface.props["tree"]["gutters"])
            for (const Json &arrow : gutter["arrows"])
                (arrow["cls"].get<std::string>().find("focus") == 0 ? focus_in : dim) = true;
        Expect(focus_in && dim, "the selected perk's links are highlighted, the others dimmed");
    }
    router.OnEvent({"perk-learn", "click", ""});
    Expect(surface.props["dialog"].is_null(), "locked perk cannot enter learn confirmation");
    // «Требует» in the same group selects that perk.
    router.OnEvent({"perk-go-ironwill", "click", ""});
    Expect(surface.sent.empty() && Selected(surface.props) == "ironwill" && surface.props["detail"]["opens"].size() == 1 &&
               surface.props["detail"]["opens"][0]["id"] == "perk-go-trustworthy",
           "a requirement row jumps to it; its panel lists what it opens");
    router.OnEvent({"perk-go-trustworthy", "click", ""});
    Expect(Selected(surface.props) == "trustworthy", "an «Открывает» row jumps back");
    // An other-group node is read in place; its own tab learns it.
    router.OnEvent({"perks", "row", "advancedcommerce"});
    Expect(surface.props["detail"]["title"] == "advancedcommerce" && surface.props["detail"]["learn"].is_null() &&
               surface.props["detail"]["go"]["id"] == "perk-open-group" && surface.sent.empty(),
           "other-group node is readable but cannot learn using personal points");
    // «Требует» across groups: the game switches to the other group, where the perk is selected.
    router.OnEvent({"perks", "row", "trustworthy"});
    router.OnEvent({"perk-go-advancedcommerce", "click", ""});
    Expect(surface.sent.size() == 1 && EngineCommand(surface.sent.back()).find("TABBTN_2") != std::string::npos,
           "a requirement of the other group switches the game to that group");
    Expect(Selected(surface.props) == "advancedcommerce" && surface.props["tabs"][2]["selected"] == true,
           "the asked group shows as chosen and the perk selected while the game switches");
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
    Expect(Selected(surface.props) == "advancedcommerce" && Node(surface.props, "advancedcommerce")["external"] == false &&
               Node(surface.props, "advancedcommerce")["reveal"] == "ship:advancedcommerce" && !surface.props["detail"]["learn"].is_null(),
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
                   poor_surface.props["detail"]["learn"]["disabled"] == true && poor_surface.props["detail"]["missing"] == "hero.no_points",
               "no free points: reachable perks stay open, the button and the panel say why it cannot be learned");
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
        stale.OnEvent({"learn-ok", "click", ""});
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
        stale.OnEvent({"learn-ok", "click", ""});
        Expect(stale_surface.sent.size() == 2, "fresh explicit learn remains possible in new context");
    }
    {
        SurfaceProbe card_surface;
        Router card(card_surface);
        card.SetInterface(model);
        card.OnEvent({"header-tabs", "select", "perks1"});
        card.OnEvent({"perks", "row", "basicdefense"});
        card.OnEvent({"perk-learn", "click", ""});
        Expect(card_surface.props["dialog"]["kind"] == "learn", "an open perk enters the confirmation card");
        auto changed = model;
        changed.revision++;
        for (auto &w : changed.widgets)
            if (w.node == "ANDROID_SUBJECT")
                w.context["character"] = "9";
        card.SetInterface(changed);
        Expect(card_surface.props["dialog"].is_null(), "subject change closes old confirmation even before an intent exists");
    }
    router.OnEvent({"perks", "row", "basicdefense"});
    router.OnEvent({"perk-learn", "click", ""});
    Expect(surface.sent.size() == 1 && surface.props["dialog"]["kind"] == "learn", "only explicit Learn opens existing confirmation");
    router.OnEvent({"learn-cancel", "click", ""});
    Expect(surface.sent.size() == 1, "cancel spends nothing");
    router.OnEvent({"perk-learn", "click", ""});
    router.OnEvent({"learn-ok", "click", ""});
    Expect(surface.sent.size() == 2 && EngineCommand(surface.sent.back()).find("TABLE_PERKS\t0\tactivate") != std::string::npos,
           "confirmation still enters stock perk window; cannot bypass PERK_ACCEPT");
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
