// Hero section «Персонаж» (interface lscchar_all; mockups V2-Hero-Skills, -SkillPoints, -Perks,
// -Companions, -OfficerAssign, -OfficerActions) and the read-only card of another character
// (interface character, V2-Hero-Candidate). Fills the pack screen <s-hero>.
//
// Tabs of our own: «Умения» (the PIRATES column, personal and ship skills, ‹ › while points
// are free), the game's two perk groups as a tab each with its free points («Личные»,
// «Корабельные»: the group's perks as a tree that scrolls vertically, a column per tier with the
// links between them (perk_tree.hpp), learned / open / locked at a glance; beside it the selected
// perk with what it needs and what it opens, each a row that jumps to that perk, across groups too;
// learning is confirmed on a parchment card with the perk's full text, what it needs and what it
// costs) and «Офицеры» (posts, free posts,
// passengers). The stock interface keeps its rules:
// every change goes through its own controls, and actions that need several of them (a skill
// arrow sits on the selected row only, a perk is learned in its window, a post is changed in
// the post chooser) are intents the screen walks one engine step per model revision, each
// step taken only from the state the engine confirmed.
//
// Facts the stock interface does not draw come from the interface adapter android_hero.c
// (ANDROID_SUBJECT, ANDROID_POSTS, ANDROID_PASSENGERS, ANDROID_CANDIDATES, ANDROID_PERKS);
// the hidden PIRATES table is exported read-only by the engine (xinterface mobile_facts.inc).
#include "hero.h"
#include "perk_tree.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace storm::ui::screens
{

using namespace hero;

namespace
{

constexpr NodeRule kNodes[] = {
    {"HERO_NAME", "text", true},
    {"TABLE_SKILL_1", "choice", true},
    {"TABLE_SKILL_2", "choice", true},
    {"ANDROID_SUBJECT", "fact"}, // the adapter's facts: CheckAdapter reports them missing
    {"ANDROID_POSTS", "fact"},
    {"ANDROID_PASSENGERS", "fact"},
    {"ANDROID_CANDIDATES", "fact"},
    {"ANDROID_PERKS", "fact"},
    {"ANDROID_PERKEDGES", "fact"}, // structured runtime prerequisite IDs (including other-group ancestors)
    {"ANDROID_SKILLHELP", "fact"}, // stock RPGDescribe text, keyed by table and skill ID
    {"TITLE", "heading"},
    {"CHARACTER_BIG_PICTURE", "image"},
    {"CHARACTER_NATION_PICTURE", "image"},
    {"CHARACTER_DRUNK_PICTURE", "image"},
    {"MAIN_BACK", "image"},
    {"TABLE_SPECIAL", "choice"},
    {"TABLE_OTHER", "choice"},
    {"TABLE_PERKS", "choice"},
    {"TABBTN_1", "image"},
    {"TABBTN_2", "image"},
    {"TABSTR_1", "text"},
    {"TABSTR_2", "text"},
    {"STR_1", "text"}, // table captions: the columns carry their own headings
    {"STR_2", "text"},
    {"STR_3", "text"},
    {"STR_4", "text"},
    {"REPUTATION_STR", "text"},
    {"LOYALITY_STR", "text"},
    {"FREE_SKILL", "text"},
    {"FREE_SPECIAL", "text"},
    {"B_SKILLUP", "button"},
    {"B_SKILLDOWN", "button"},
    {"CHARACTERS_SCROLL", "choice"},
    {"UP_BUTTON", "button"},
    {"DOWN_BUTTON", "button"},
    {"EXIT_BTN", "button"},
    {"I_CHARACTER", "button"},
    {"I_SHIP", "button"},
    {"I_QUESTBOOK", "button"},
    {"I_TRADEBOOK", "button"},
    {"I_NATIONS", "button"},
    {"I_ITEMS", "button"},
    {"STR_BUTTONS", "text"}, // captions of the section buttons: the header icons replace them
    // The game's windows: the perk window is walked by the learn intent, the post chooser and
    // the remove window are drawn as our sheets, the message window as a dialog.
    {"PERK_*", "text|button|image|choice"},
    {"REMOVE_*", "text|button"},
    {"OFFICER*", "text|button"},
    {"PASSENGERSLIST", "choice"},
    {"LEFTSCROLLBUTTON", "button"},
    {"RIGHTSCROLLBUTTON", "button"},
    {"TABLE_SMALLSKILL", "choice"},
    {"TABLE_SMALLOTHER", "choice"},
    {"ACCEPT_ADD_OFFICER", "button"},
    {"CANCEL_ADD_OFFICER", "button"},
    {"SPECIAL_*", "text|button"},
    {"GAME_ACCEPT", "button"},
    {"GAME_CANCEL", "button"},
    {"MSG_*", "text|button"},
};

constexpr int kRevealMargin = 16;   // dp kept above and below a perk node scrolled into view
constexpr double kStepTimeout = 3.0; // an intent whose step got no answer from the engine is dropped

// The caption of a perk group button: "Личные 1" → {"Личные", 1}.
std::pair<std::string, int> GroupCaption(const std::string &label)
{
    const std::string text = Trim(label);
    const size_t space = text.find_last_of(' ');
    if (space == std::string::npos || text.find_first_not_of("0123456789", space + 1) != std::string::npos)
        return {text, 0};
    return {Trim(text.substr(0, space)), std::atoi(text.c_str() + space + 1)};
}

// Our bare icon of a skill (pictures.ini ICONS_SPEC rows carry the skill id as row id).
const char *SkillIcon(const std::string &id)
{
    static const char *const icons[][2] = {
        {"Leadership", "leadership"}, {"FencingL", "rapier"},     {"FencingS", "sabre"},    {"FencingH", "axe"},
        {"Pistol", "pistol"},         {"Fortune", "fortune"},     {"Sneak", "sneak"},       {"Sailing", "wheel"},
        {"Accuracy", "accuracy"},     {"Cannons", "cannon"},      {"Grappling", "grapple"}, {"Defence", "shield"},
        {"Repair", "repair"},         {"Commerce", "commerce"}};
    for (const auto &pair : icons)
        if (id == pair[0])
            return pair[1];
    return "";
}

// The icon of a free post (GetOfficerTypeByNum).
const char *PostIcon(const std::string &post, int slot)
{
    static const char *const icons[][2] = {{"navigator", "wheel"}, {"boatswain", "grapple"}, {"cannoner", "cannon"},
                                           {"doctor", "medicine"}, {"treasurer", "commerce"}, {"carpenter", "repair"}};
    for (const auto &pair : icons)
        if (post == pair[0])
            return pair[1];
    return slot % 3 == 1 ? "rapier" : slot % 3 == 2 ? "sabre" : "axe";
}

// Perks drawn with our enamel icon; the others keep the game's picture.
bool HasPerkIcon(const std::string &perk)
{
    static const char *const perks[] = {"advanceddefense", "alchemy", "basiccommerce", "basicdefense", "brander", "carpenter",
                                        "crewdamageup", "criticalhit", "energyplus", "fastreload", "grapplingprofessional",
                                        "gunman", "gunprofessional", "hardhitter", "hpplus", "hulldamageup", "instantrepair",
                                        "ironwill", "longrangeshoot", "professionalcommerce", "sailsdamageup", "shipescape",
                                        "swordplayprofessional", "tireless"};
    std::string id;
    for (const char c : perk)
        id += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    for (const char *name : perks)
        if (id == name)
            return true;
    return false;
}

std::string Lowercase(const std::string &text)
{
    std::string out;
    for (const char c : text)
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

// "45 / 140" → {value: percent, text: "45"}.
Json BarOf(const std::string &text)
{
    const size_t slash = text.find('/');
    const int current = Number(text);
    const int maximum = slash == std::string::npos ? 0 : Number(text.substr(slash + 1));
    return Json{{"value", maximum > 0 ? std::clamp(current * 100 / maximum, 0, 100) : 100}, {"text", std::to_string(current)}};
}

enum class Tab
{
    Skills,
    Perks,
    Officers
};

class HeroScreen final : public Screen
{
  public:
    const char *Component() const override
    {
        return "s-hero";
    }
    std::span<const NodeRule> Nodes() const override
    {
        return kNodes;
    }

    Json Build(const Admission &admitted, Bindings &bindings) override
    {
        // Handlers run after Build returns: keep the admission (pointers into the router's model,
        // valid until the next revision rebuilds the view).
        admission_ = admitted;
        a_ = &admission_;
        revision_ = admitted.model->revision;
        CheckAdapter(admitted, bindings, "ANDROID_SUBJECT", checked_opening_);
        const bool full = admitted.model->screen == "lscchar_all";
        const Widget *subject = admitted.One("ANDROID_SUBJECT");
        // A HUD tap that opened this screen (a party portrait, an event line that leads to the
        // abilities): taken once; its tab at once, its person once the screen can switch.
        if (auto request = bindings.TakeSubject("character"))
        {
            if (request->tab == "perks")
                tab_ = Tab::Perks;
            subject_request_ = std::move(request);
        }
        if (!full && tab_ == Tab::Officers)
            tab_ = Tab::Skills;

        Json view = Json::object();
        view["title"] = Trim(admitted.Label("TITLE"));
        view["sections"] = full ? "hero" : "";

        // The game's windows that are open now.
        window_ = admitted.One("PERK_ACCEPT") || admitted.One("PERK_CANCEL")                ? Window::Perk
                  : admitted.One("REMOVE_ACCEPT_OFFICER") || admitted.One("REMOVE_CANCEL_OFFICER") ? Window::Remove
                  : admitted.One("ACCEPT_ADD_OFFICER") || admitted.One("CANCEL_ADD_OFFICER")         ? Window::Assign
                  : admitted.One("MSG_OK")                                                           ? Window::Message
                                                                                                     : Window::None;
        Walk(bindings);
        if (full && subject_request_ && intent_.kind == Intent::None && window_ == Window::None)
            OpenOnSubject(bindings);

        // Header: tabs, the officer switch, the sections.
        const int free_self = Number(Field(subject, "freeself"));
        const int free_ship = Number(Field(subject, "freeship"));
        const int free_skill = Number(admitted.Label("FREE_SKILL"));
        Json tabs = Json::array();
        tabs.push_back(Json{{"id", "skills"}, {"label", bindings.Text("hero.tab_skills")}, {"selected", tab_ == Tab::Skills},
                            {"badge", free_skill > 0 ? Json(free_skill) : Json("")}});
        // The game's two perk groups (TABBTN_n with TABSTR_n "Личные 1"; the active one shows value 1) are a
        // tab each. The marker follows the group the game shows, so it moves in the same frame as the tree
        // (wanted_ship_ only keeps a second tap from pressing the group button again while the game switches).
        const Widget *group1 = admitted.One("TABBTN_1");
        const Widget *group2 = admitted.One("TABBTN_2");
        const bool ship = group2 && group2->value == "1";
        if (wanted_ship_ == static_cast<int>(ship) || intent_.kind != Intent::None)
            wanted_ship_ = -1;
        const std::string caption1 = GroupCaption(admitted.Label("TABSTR_1")).first, caption2 = GroupCaption(admitted.Label("TABSTR_2")).first;
        tabs.push_back(Json{{"id", "perks1"}, {"label", caption1.empty() ? bindings.Text("hero.tab_perks") : caption1},
                            {"selected", tab_ == Tab::Perks && !ship}, {"badge", free_self > 0 ? Json(free_self) : Json("")}});
        if (group2 && !caption2.empty())
            tabs.push_back(Json{{"id", "perks2"}, {"label", caption2}, {"selected", tab_ == Tab::Perks && ship},
                                {"badge", free_ship > 0 ? Json(free_ship) : Json("")}});
        if (full)
            tabs.push_back(Json{{"id", "officers"}, {"label", bindings.Text("hero.tab_officers")}, {"selected", tab_ == Tab::Officers}});
        view["tabs"] = tabs;
        bindings.On("header-tabs", "select", [this, &bindings, group1, group2, ship](const Event &event) {
            const bool perks = event.value == "perks1" || event.value == "perks2";
            const Tab next = perks ? Tab::Perks : event.value == "officers" ? Tab::Officers : Tab::Skills;
            const bool want_ship = event.value == "perks2";
            if (next == tab_ && (!perks || want_ship == (wanted_ship_ >= 0 ? wanted_ship_ == 1 : ship)))
                return;
            tab_ = next;
            if (intent_.kind == Intent::Learn) intent_ = {};
            skill_help_ = nullptr;
            sent_revision_ = 0; // a player action: its first step may go at once
            if (window_ != Window::None && window_ != Window::Message)
                CloseWindow(bindings);
            else if (perks && want_ship != ship)
            {
                perk_.clear();
                wanted_ship_ = want_ship ? 1 : 0;
                Step(bindings, want_ship ? group2 : group1, "press");
            }
            bindings.Rebuild();
        });
        view["tab"] = tab_ == Tab::Skills ? "skills" : tab_ == Tab::Perks ? "perks" : "officers";
        view["subject"] = full ? hero::Subject(admitted, bindings, subjects_, [this, &bindings](const Widget &row) {
            if (intent_.kind == Intent::Learn) intent_ = {};
            sent_revision_ = 0; // a player action: its first step may go at once
            Step(bindings, &row, "select", row.row_id);
        })
                               : Json(nullptr);
        BindSections(bindings);

        view["card"] = CardView(bindings, subject, free_skill);
        if (tab_ == Tab::Skills)
            SkillsView(view, bindings, free_skill);
        else if (tab_ == Tab::Perks)
            PerksView(view, bindings, subject, full, ship, ship ? free_ship : free_self);
        else
            OfficersView(view, bindings);
        view["dialog"] = DialogView(bindings);

        bindings.On("header-back", "", [this, &bindings](const Event &) {
            sent_revision_ = 0; // a player action: its first step may go at once
            if (!skill_help_.is_null())
            {
                skill_help_ = nullptr;
                bindings.Rebuild();
            }
            else if (window_ != Window::None)
                CloseWindow(bindings);
            else
                bindings.Back(nullptr);
        });
        return view;
    }

    // Android back closes the skill help or the game's window first (Bindings::Modal does it for
    // those shown as a dialog; a window of the game is open a moment before its dialog is).
    bool OwnsBack() const override
    {
        return !skill_help_.is_null() || window_ != Window::None;
    }
    void Back(Bindings &bindings) override
    {
        if (!skill_help_.is_null())
        {
            skill_help_ = nullptr;
            return bindings.Rebuild();
        }
        sent_revision_ = 0; // a player action: its first step may go at once
        CloseWindow(bindings);
    }

  private:
    enum class Window
    {
        None,
        Perk,
        Remove,
        Assign,
        Message
    };
    // What the player asked for that takes several engine steps.
    struct Intent
    {
        enum Kind
        {
            None,
            Arrow,   // table, row id, verb up|down
            Learn,   // perk id
            Post,    // slot: select, then activate (the post chooser or the remove window)
            Assign,  // candidate row of PASSENGERSLIST: select, then accept
            Section, // a section button once no window is open
        } kind = None;
        std::string node, row, verb;
        std::string candidate_character; // Assign: identity shown at the tap, not a reusable row index
        int candidate_post = -1;
        std::string learn_character; // Learn: actual subject identity, never a reusable slot
        bool learn_ship = false;
        double since = 0.0;
    };

    Admission admission_;
    const Admission *a_ = nullptr;
    uint64_t revision_ = 0;
    uint64_t sent_revision_ = 0;
    uint64_t checked_opening_ = 0;
    SubjectSwitch subjects_;
    Tab tab_ = Tab::Skills;
    std::optional<SubjectRequest> subject_request_; // the HUD's subject, until the screen can switch to it
    Window window_ = Window::None;
    Intent intent_;
    std::string perk_;                  // the perk in the detail panel
    perk_tree::Cache perk_tree_;        // topology only; selection/state changes do not relayout
    Json skill_help_ = nullptr;         // presentation-only stock skill description, never a game selection
    int wanted_ship_ = -1;              // the perk group the player asked for until the game shows it (0 personal, 1 ship)
    std::map<std::string, int> added_;  // points the player put into a skill during this visit

    bool SameLearnContext(const std::string &character, bool ship) const
    {
        const Widget *group = a_->One("TABBTN_2");
        return !character.empty() && character == Field(a_->One("ANDROID_SUBJECT"), "character") &&
            ship == (group && group->value == "1");
    }

    // One engine step per model revision: a step is sent only after the engine answered the last.
    bool Step(Bindings &bindings, const Widget *widget, const std::string &verb, const std::string &value = {})
    {
        if (!widget || sent_revision_ == revision_)
            return false;
        sent_revision_ = revision_;
        bindings.Send(*widget, verb, value);
        return true;
    }

    const Widget *Row(const char *node, const std::string &row_id) const
    {
        for (const Widget *row : a_->All(node))
            if (row->row_id == row_id)
                return row;
        return nullptr;
    }

    const Widget *Selected(const char *node) const
    {
        for (const Widget *row : a_->All(node))
            if (row->value == "1")
                return row;
        return nullptr;
    }

    void Start(Bindings &bindings, Intent intent)
    {
        sent_revision_ = 0; // a player action: its first step may go at once
        intent.since = bindings.Now();
        intent_ = std::move(intent);
        Walk(bindings);
        bindings.RebuildAfter(kStepTimeout);
    }

    void Drop(Bindings &bindings, const std::string &why)
    {
        if (!why.empty())
            bindings.Error("lscchar_all: " + why);
        intent_ = {};
    }

    void CloseWindow(Bindings &bindings)
    {
        const char *const cancels[] = {window_ == Window::Perk     ? "PERK_CANCEL"
                                       : window_ == Window::Remove ? "REMOVE_CANCEL_OFFICER"
                                       : window_ == Window::Assign ? "CANCEL_ADD_OFFICER"
                                                                   : "MSG_OK",
                                       window_ == Window::Perk     ? "PERK_EXIT_BTN"
                                       : window_ == Window::Remove ? "REMOVE_EXIT_BTN"
                                       : window_ == Window::Assign ? "OFFICER_EXIT_BTN"
                                                                   : "MSG_EXIT_BTN"};
        for (const char *node : cancels)
            if (const Widget *button = a_->One(node); button && button->Offers("press"))
            {
                Step(bindings, button, "press");
                return;
            }
    }

    // The next engine step of the intent, from the state of this model.
    void Walk(Bindings &bindings)
    {
        if (intent_.kind == Intent::None)
            return;
        if (bindings.Now() - intent_.since > kStepTimeout)
            return Drop(bindings, "the engine did not answer a step (" + intent_.node + " " + intent_.row + ")");
        switch (intent_.kind)
        {
        case Intent::Arrow: {
            const Widget *row = Row(intent_.node.c_str(), intent_.row);
            if (!row)
                return Drop(bindings, "skill row " + intent_.row + " is gone");
            if (row->value != "1")
            {
                Step(bindings, row, "select", row->row_id);
                return;
            }
            const Widget *arrow = a_->One(intent_.verb == "up" ? "B_SKILLUP" : "B_SKILLDOWN");
            if (!arrow || !arrow->Offers("press"))
                return Drop(bindings, {}); // the game offers no arrow there (limit reached)
            if (Step(bindings, arrow, "press"))
            {
                added_[intent_.row] += intent_.verb == "up" ? 1 : -1;
                intent_ = {};
            }
            return;
        }
        case Intent::Learn: {
            if (!SameLearnContext(intent_.learn_character, intent_.learn_ship))
                return Drop(bindings, "perk learning subject or group changed");
            if (window_ == Window::Perk)
            {
                const Widget *accept = a_->One("PERK_ACCEPT");
                const Widget *selected = Selected("TABLE_PERKS");
                if (selected && selected->row_id == intent_.row && accept && accept->Offers("press"))
                {
                    if (Step(bindings, accept, "press"))
                        intent_ = {};
                    return;
                }
                CloseWindow(bindings); // another perk's window, or the game refuses: close it
                if (selected && selected->row_id == intent_.row)
                    Drop(bindings, {});
                return;
            }
            if (window_ != Window::None)
                return CloseWindow(bindings);
            const Widget *row = Row("TABLE_PERKS", intent_.row);
            if (!row)
                return Drop(bindings, "perk " + intent_.row + " is not in the list");
            Step(bindings, row, row->value == "1" ? "activate" : "select", row->row_id);
            return;
        }
        case Intent::Post: {
            if (window_ == Window::Remove || window_ == Window::Assign)
            {
                intent_ = {};
                return;
            }
            if (window_ != Window::None)
                return CloseWindow(bindings);
            const auto rows = a_->All("CHARACTERS_SCROLL");
            const int slot = std::atoi(intent_.row.c_str());
            const Widget *row = nullptr;
            for (const Widget *r : rows)
                if (r->index == slot)
                    row = r;
            if (!row)
                return Drop(bindings, "officer slot " + intent_.row + " is gone");
            if (row->value != "1")
                Step(bindings, row, "select", row->row_id);
            else if (row->Offers("activate"))
                Step(bindings, row, "activate", row->row_id);
            return;
        }
        case Intent::Assign: {
            if (window_ != Window::Assign)
                return Drop(bindings, {}); // the chooser closed: assigned, or cancelled by the game
            const Widget *post = Selected("CHARACTERS_SCROLL");
            const Widget *candidate = nullptr;
            for (const Widget *fact : a_->All("ANDROID_CANDIDATES"))
                if (Field(fact, "index") == intent_.row)
                    candidate = fact;
            // Selection and acceptance span revisions. A newly filtered list can
            // reuse the same row for someone else; a generic accept must never
            // carry our old tap into that new candidate or another post.
            if (intent_.candidate_character.empty() || intent_.candidate_post <= 0 || !post ||
                post->index != intent_.candidate_post || !candidate ||
                Field(candidate, "character") != intent_.candidate_character)
                return Drop(bindings, "the post chooser candidate or post changed");
            const int index = std::atoi(intent_.row.c_str());
            const Widget *row = nullptr;
            for (const Widget *r : a_->All("PASSENGERSLIST"))
                if (r->index == index)
                    row = r;
            if (!row)
                return Drop(bindings, "candidate " + intent_.row + " is not in the post chooser");
            if (row->value != "1")
            {
                Step(bindings, row, "select", row->row_id);
                return;
            }
            const Widget *accept = a_->One("ACCEPT_ADD_OFFICER");
            if (!accept || !accept->Offers("press"))
                return Drop(bindings, "the post chooser refuses candidate " + intent_.row);
            Step(bindings, accept, "press");
            return;
        }
        case Intent::Section: {
            if (window_ != Window::None)
                return CloseWindow(bindings);
            for (const Widget *widget : a_->known)
                if (widget->type == "button" && SectionOf(*widget) == intent_.row && widget->Offers("press"))
                {
                    if (Step(bindings, widget, "press"))
                        intent_ = {};
                    return;
                }
            return Drop(bindings, "section " + intent_.row + " cannot be opened");
        }
        case Intent::None:
            return;
        }
    }

    // A tap on a land party portrait or an event line opened this screen for that person (hud
    // land.cpp, hud.cpp Open): the header's officer switch selects the post the person holds, as
    // the player's choice does.
    void OpenOnSubject(Bindings &bindings)
    {
        const auto request = std::move(subject_request_);
        subject_request_.reset();
        for (const Widget *post : a_->All("ANDROID_POSTS"))
            if (std::atoi(Field(post, "character").c_str()) == request->character && !Field(post, "character").empty())
            {
                const int slot = std::atoi(Field(post, "slot").c_str());
                for (const Widget *row : a_->All("CHARACTERS_SCROLL"))
                    if (row->index == slot && row->value != "1" && row->Offers("select"))
                    {
                        sent_revision_ = 0;
                        Step(bindings, row, "select", row->row_id);
                    }
                return;
            }
        // An event line about the captain (a level, ability points) names the hero, who holds no
        // post: the captain is shown already.
        if (request->tab.empty())
            bindings.Error("lscchar_all: character " + std::to_string(request->character) + " of the HUD's tap holds no post here; the captain stays");
    }

    void BindSections(Bindings &bindings)
    {
        bindings.On("header", "section", [this, &bindings](const Event &event) {
            if (event.value == "hero")
                return;
            Intent intent;
            intent.kind = Intent::Section;
            intent.row = event.value;
            Start(bindings, intent);
        });
    }

    Json CardView(Bindings &bindings, const Widget *subject, int free_skill)
    {
        std::map<std::string, const Widget *> other;
        for (const Widget *row : a_->All("TABLE_OTHER"))
            other[row->row_id] = row;
        const auto value = [&](const char *id) { return other.count(id) ? Trim(other[id]->CellText(2)) : std::string(); };
        const auto label = [&](const char *id) { return other.count(id) ? Trim(other[id]->CellText(1)) : std::string(); };

        Json card = Json::object();
        card["title"] = Trim(a_->Label("HERO_NAME"));
        std::string sub = label("Rank").empty() ? "" : label("Rank") + " " + value("Rank");
        const Widget *slot = Selected("CHARACTERS_SCROLL");
        for (const Widget *post : a_->All("ANDROID_POSTS"))
            if (slot && Field(post, "slot") == std::to_string(slot->index) && !Field(post, "character").empty())
                sub += (sub.empty() ? "" : " · ") + LowerText(Field(post, "role"));
        card["sub"] = sub;
        const ImageRef *face = nullptr;
        if (const Widget *picture = a_->One("CHARACTER_BIG_PICTURE"))
            face = picture->FirstImage();
        const Json portrait = PictureOf(face);
        card["portrait"] = portrait.is_object() ? portrait.value("texture", "") : std::string();
        card["health"] = BarOf(value("Life"));
        card["energy"] = BarOf(value("Energy"));
        card["free"] = tab_ == Tab::Skills && free_skill > 0 ? std::to_string(free_skill) : "";
        Json lines = Json::array();
        if (!value("Health").empty())
        {
            const std::string word = LowerText(value("Health"));
            const bool bad = other.count("Health") && (other["Health"]->cells.size() > 2) &&
                             (other["Health"]->cells[2].color_argb & 0xFFFFFF) != 0xFFFFFF && (other["Health"]->cells[2].color_argb & 0xFF00) < 0x8000;
            lines.push_back(Json{{"k", label("Health")}, {"v", word}, {"tone", bad ? "danger" : "ok"}});
        }
        // The stock LOYALITY bar: the hero's authority (1 … 100), an officer's loyalty word.
        if (!Field(subject, "authority").empty())
            lines.push_back(Json{{"k", Trim(a_->Label("LOYALITY_STR"))}, {"v", Field(subject, "authority") + " / 100"}});
        else if (!Field(subject, "loyalty").empty())
            lines.push_back(Json{{"k", Trim(a_->Label("LOYALITY_STR"))}, {"v", LowerText(Field(subject, "loyalty"))}});
        if (Field(subject, "hero") != "1" && Number(Field(subject, "salary")) > 0)
            lines.push_back(Json{{"k", bindings.Text("hero.salary")}, {"v", Thousands(Field(subject, "salary"))}, {"tone", "gold"},
                                 {"after", " " + bindings.Text("hero.per_month")}});
        card["lines"] = lines;
        // The portrait opens the rest of the game's facts (TABLE_OTHER: rank, title, money,
        // weight, experience) with the reputation, as the mockup's rail leaves them out.
        std::string facts;
        for (const Widget *row : a_->All("TABLE_OTHER"))
            if (row->row_id != "Life" && row->row_id != "Energy" && row->row_id != "Health" && !Trim(row->CellText(2)).empty())
                facts += Trim(row->CellText(1)) + ": " + Trim(row->CellText(2)) + "\n";
        if (!Trim(a_->Label("REPUTATION_STR")).empty())
            facts += bindings.Text("hero.reputation") + ": " + Trim(a_->Label("REPUTATION_STR")) + "\n";
        const std::string name = Trim(a_->Label("HERO_NAME"));
        bindings.On("card", "portrait", [&bindings, name, facts](const Event &) { bindings.Read(name, facts); });
        // The start-of-game PIRATES choice (interface character with its own OK / cancel).
        Json actions = Json::array();
        if (const Widget *cancel = a_->One("GAME_CANCEL"))
            actions.push_back(ButtonView(bindings, *cancel, "game-cancel", "secondary"));
        if (const Widget *accept = a_->One("GAME_ACCEPT"))
            actions.push_back(ButtonView(bindings, *accept, "game-accept", "primary"));
        card["actions"] = actions;
        return card;
    }

    // One skill table (PIRATES, personal, ship) for a c-table.
    Json SkillTable(Bindings &bindings, const char *node, const std::string &id, const std::string &heading, bool points,
                    bool special)
    {
        Json columns = Json::array();
        columns.push_back(Json{{"label", heading}, {"w", "1fr"}});
        if (points)
            columns.push_back(Json{{"label", ""}, {"w", "112px"}, {"align", "right"}});
        else
        {
            if (!special)
                columns.push_back(Json{{"label", bindings.Text("hero.exp")}, {"w", "40px"}});
            columns.push_back(Json{{"label", ""}, {"w", special ? "32px" : "28px"}, {"align", "right"}});
        }
        Json rows = Json::array();
        std::vector<const Widget *> source = a_->All(node);
        for (const Widget *row : source)
        {
            // TABLE_SKILL_n: icon, name, experience %, bonus, value; TABLE_SPECIAL: letter, name, bonus, value.
            const int value_col = special ? 3 : 4;
            // The game names blade and gun skills by pairs ("Рапиры/Шпаги"): the first word fits
            // the column (mockup V2-Hero-Skills).
            const std::string full = Trim(row->CellText(1));
            const std::string name = special || full.find('/') == std::string::npos ? full : Trim(full.substr(0, full.find('/')));
            const std::string bonus = Trim(row->CellText(special ? 2 : 3));
            const std::string value = Trim(row->CellText(value_col));
            const std::string tone = bonus.find('-') != std::string::npos   ? "danger"
                                     : bonus.find('+') != std::string::npos ? "ok"
                                     : special                              ? ""
                                                                            : "gold";
            // A name that does not fit wraps to two lines and steps down (ui-clamp min-size).
            Json first{{"text", name}, {"lines", 2}};
            if (!special && !points && *SkillIcon(row->row_id))
                first["icon"] = SkillIcon(row->row_id), first["iconSize"] = 24;
            Json cells = Json::array({first});
            if (points)
            {
                const int added = added_.count(row->row_id) ? added_[row->row_id] : 0;
                const bool may_down = added > 0 || (special && a_->One("GAME_ACCEPT"));
                cells.push_back(Json{{"text", value}, {"arrows", true}, {"noLeft", !may_down}, {"tone", added > 0 ? "ok" : "gold"}, {"bold", true}});
            }
            else
            {
                if (!special)
                    cells.push_back(Json{{"bar", std::clamp(Number(row->CellText(2)), 0, 100)}, {"barKind", "xp"}, {"text", ""}});
                Json last{{"text", value}, {"bold", true}};
                if (!tone.empty())
                    last["tone"] = tone;
                cells.push_back(last);
            }
            rows.push_back(Json{{"cells", cells}});
        }
        const auto arrow = [this, &bindings, node, source](const Event &event, const char *verb) {
            const size_t index = static_cast<size_t>(std::atoi(event.value.c_str()));
            if (index >= source.size())
                return;
            Intent intent;
            intent.kind = Intent::Arrow;
            intent.node = node;
            intent.row = source[index]->row_id;
            intent.verb = verb;
            Start(bindings, intent);
        };
        bindings.On(id, "inc", [arrow](const Event &event) { arrow(event, "up"); });
        bindings.On(id, "dec", [arrow](const Event &event) { arrow(event, "down"); });
        bindings.On(id, "row", [this, &bindings, node, source](const Event &event) {
            const size_t index = static_cast<size_t>(std::atoi(event.value.c_str()));
            if (index >= source.size())
                return;
            for (const Widget *fact : a_->All("ANDROID_SKILLHELP"))
                if (Field(fact, "node") == node && Field(fact, "skill") == source[index]->row_id)
                {
                    const std::string text = StockText(Field(fact, "text"));
                    if (text.empty())
                        return;
                    skill_help_ = Json{{"title", Trim(source[index]->CellText(1))}, {"text", text}};
                    bindings.Rebuild();
                    return;
                }
        });
        return Json{{"columns", columns}, {"rows", rows}};
    }

    void SkillsView(Json &view, Bindings &bindings, int free_skill)
    {
        const bool points = free_skill > 0 || !added_.empty();
        const int free_special = Number(a_->Label("FREE_SPECIAL"));
        const bool special_points = (free_special > 0 || a_->One("GAME_ACCEPT")) && !a_->All("TABLE_SPECIAL").empty() &&
                                    a_->All("TABLE_SPECIAL").front()->Offers("select");
        view["special"] = SkillTable(bindings, "TABLE_SPECIAL", "special", bindings.Text("hero.special"), special_points, true);
        view["personal"] = SkillTable(bindings, "TABLE_SKILL_1", "personal", GroupCaption(a_->Label("TABSTR_1")).first, points, false);
        view["ship"] = SkillTable(bindings, "TABLE_SKILL_2", "ship", GroupCaption(a_->Label("TABSTR_2")).first, points, false);
        view["points"] = points;
    }

    // The perks of the snapshot. Rows: the stock table of the group shown (TABLE_PERKS); facts: the
    // adapter's (ANDROID_PERKS: label, text, type self|ship, learned, officer, rule, requirements,
    // eligible) for those rows and for every prerequisite of theirs, also of the other group;
    // edges: ANDROID_PERKEDGES (source, target, met, rule).
    struct PerkGraph
    {
        std::vector<std::string> ids; // the table's order, then other-group prerequisites as first needed
        std::map<std::string, const Widget *> rows, facts;
        std::map<std::string, std::vector<const Widget *>> incoming, outgoing;
        std::vector<perk_tree::Edge> edges;
        bool complete = true; // every perk has its fact with the stock ALL rule and all its edges
    };

    PerkGraph Graph() const
    {
        PerkGraph graph;
        for (const Widget *fact : a_->All("ANDROID_PERKS"))
            graph.facts[perk_tree::Id(fact->row_id)] = fact;
        std::set<std::string> known;
        for (const Widget *row : a_->All("TABLE_PERKS"))
        {
            const std::string id = perk_tree::Id(row->row_id);
            if (known.insert(id).second)
                graph.ids.push_back(id);
            graph.rows[id] = row;
        }
        for (const Widget *edge : a_->All("ANDROID_PERKEDGES"))
        {
            const std::string source = perk_tree::Id(Field(edge, "source")), target = perk_tree::Id(Field(edge, "target"));
            graph.incoming[target].push_back(edge);
            graph.outgoing[source].push_back(edge);
            graph.edges.push_back({source, target});
        }
        for (size_t at = 0; at < graph.ids.size(); ++at) // the prerequisites the shown perks need, breadth first
            for (const Widget *edge : graph.incoming[graph.ids[at]])
                if (const std::string source = perk_tree::Id(Field(edge, "source")); known.insert(source).second)
                    graph.ids.push_back(source);
        // Links among perks that are neither (stale or damaged facts): drawn, so the snapshot stays readable,
        // and the incomplete graph below locks learning. A link to a perk without any fact fails the layout.
        for (const auto &edge : graph.edges)
            for (const auto &end : {edge.from, edge.to})
                if (graph.facts.count(end) && known.insert(end).second)
                    graph.ids.push_back(end);
        for (const auto &id : graph.ids)
        {
            const Widget *fact = graph.facts.count(id) ? graph.facts.at(id) : nullptr;
            graph.complete &= fact && Field(fact, "rule") == "stock-all-v1" && !Field(fact, "requirements").empty() &&
                              Number(Field(fact, "requirements")) == static_cast<int>(graph.incoming[id].size());
            for (const Widget *edge : graph.incoming[id])
                graph.complete &= Field(edge, "rule") == "stock-all-v1";
        }
        return graph;
    }

    static const Widget *FactOf(const PerkGraph &graph, const std::string &id)
    {
        const auto found = graph.facts.find(id);
        return found == graph.facts.end() ? nullptr : found->second;
    }
    static bool Learned(const PerkGraph &graph, const std::string &id)
    {
        return Field(FactOf(graph, id), "learned") == "1";
    }
    // The stock caption: the table's, else the adapter's label, else the ID itself.
    static std::string PerkName(const PerkGraph &graph, const std::string &id)
    {
        if (graph.rows.count(id))
            if (std::string name = Trim(graph.rows.at(id)->CellText(1)); !name.empty())
                return name;
        const std::string label = Trim(Field(FactOf(graph, id), "label"));
        return label.empty() ? id : label;
    }
    Json PerkPicture(const PerkGraph &graph, const std::string &id, bool dim) const
    {
        if (graph.rows.count(id))
            return PerkImage(*graph.rows.at(id), dim);
        if (HasPerkIcon(id))
            return Json{{"kind", "token"}, {"icon", "perk-" + id}, {"enamel", dim ? "iron" : "red"}};
        return Json{{"kind", "item"}, {"texture", std::string("atlas:") + (dim ? "PERK_DISABLE/" : "PERK_ENABLE/") + id}};
    }
    // The group of a perk (fact type: self or ship) and the caption of its tab.
    static bool ShipPerk(const PerkGraph &graph, const std::string &id)
    {
        return Field(FactOf(graph, id), "type") == "ship";
    }
    std::string GroupName(bool ship) const
    {
        return GroupCaption(a_->Label(ship ? "TABSTR_2" : "TABSTR_1")).first;
    }
    bool Available(const PerkGraph &graph, const std::string &id, bool full, int free) const
    {
        const Widget *fact = FactOf(graph, id);
        std::vector<bool> met;
        for (const Widget *edge : graph.incoming.count(id) ? graph.incoming.at(id) : std::vector<const Widget *>())
            met.push_back(Field(edge, "met") == "1" && Field(edge, "rule") == "stock-all-v1");
        return graph.complete && perk_tree::CanLearn(graph.rows.count(id) > 0, full, Learned(graph, id), free, Field(fact, "rule"),
                                                     Field(fact, "eligible") == "1", met);
    }

    Json PerkImage(const Widget &row, bool locked) const
    {
        if (HasPerkIcon(row.row_id))
            return Json{{"kind", "token"}, {"icon", "perk-" + Lowercase(row.row_id)}, {"enamel", locked ? "iron" : "red"}};
        return PictureOf(row.cells.empty() || row.cells[0].images.empty() ? nullptr : &row.cells[0].images[0]);
    }

    // Selects a perk named in the panel (a prerequisite, a dependent, a node of the other group): in this
    // tree when it is one of the group's perks, else the game switches to its group and it is selected
    // there. Nothing is learned or changed in the game but the shown group.
    void JumpTo(Bindings &bindings, const std::string &id, bool ship_perk, bool local, bool shown_ship)
    {
        perk_ = id;
        if (!local && ship_perk != shown_ship && window_ == Window::None && intent_.kind == Intent::None)
        {
            const Widget *group = a_->One(ship_perk ? "TABBTN_2" : "TABBTN_1");
            if (!group)
                bindings.Error("perk group button " + std::string(ship_perk ? "TABBTN_2" : "TABBTN_1") + " missing: stays in this group");
            else
            {
                wanted_ship_ = ship_perk ? 1 : 0;
                sent_revision_ = 0; // a player action: its first step may go at once
                Step(bindings, group, "press");
            }
        }
        bindings.Rebuild();
    }

    void PerksView(Json &view, Bindings &bindings, const Widget *subject, bool full, bool ship, int free)
    {
        const std::string exp = Field(subject, ship ? "expship" : "expself");
        const std::string rate = Field(subject, ship ? "rateship" : "rateself");
        const std::string progress =
            full && Number(rate) > 0 ? bindings.Text("hero.next_point") + " " + Thousands(exp.empty() ? "0" : exp) + " / " + Thousands(rate) : "";
        view["perksEmpty"] = bindings.Text("hero.no_perks");
        view["perkInfo"] = full ? bindings.Text("hero.free_points") + " " + std::to_string(std::max(free, 0)) + (progress.empty() ? "" : " · " + progress) : "";
        view["tree"] = nullptr;
        view["detail"] = nullptr;
        const PerkGraph graph = Graph();
        if (graph.rows.empty())
        {
            return;
        }
        const size_t builds = perk_tree_.Builds();
        const perk_tree::Layout &layout = perk_tree_.Get(graph.ids, graph.edges);
        const bool built = perk_tree_.Builds() != builds;
        if (!layout.valid)
        {
            // A cyclic graph or an edge to a perk outside the snapshot: no invented partial tree.
            if (built)
                bindings.Error("Perk prerequisite graph is cyclic or names a perk without a fact; the perk tree is not shown");
            view["perksEmpty"] = bindings.Text("items.unavailable");
            return;
        }
        if (built && !graph.complete)
            bindings.Error("Perk facts are incomplete (missing fact, rule or edge); perks are shown as unavailable and cannot be learned");

        std::string selected = perk_tree::Id(perk_);
        if (!layout.nodes.count(selected))
        {
            // The group's first perk in reading order: the top row, from the left.
            selected = graph.ids.front();
            for (const auto &[id, row] : graph.rows)
            {
                const auto &place = layout.nodes.at(id), &best = layout.nodes.at(selected);
                if (std::tie(place.top, place.left) < std::tie(best.top, best.left))
                    selected = id;
            }
        }
        perk_ = graph.rows.count(selected) ? graph.rows.at(selected)->row_id : selected;

        // A node is locked only by a missing prerequisite; free points and the stock window's other checks
        // decide the learn button, so a tree without points still reads which perks are reachable.
        const auto state = [&](const std::string &id) {
            if (Learned(graph, id))
                return "learned";
            if (!graph.complete)
                return "unavailable";
            for (const Widget *edge : graph.incoming.count(id) ? graph.incoming.at(id) : std::vector<const Widget *>())
                if (Field(edge, "met") != "1")
                    return "locked";
            return "open";
        };
        // The links as <ui-links> draws them: 2 the selected perk's (on top), 1 from a learned perk, 0 the others.
        std::string links;
        for (const auto &link : layout.links)
        {
            const auto &edge = layout.edges[link.edge];
            links += (links.empty() ? "" : ";") +
                     std::to_string(edge.from == selected || edge.to == selected ? 2 : Learned(graph, edge.from) ? 1 : 0);
            for (const auto &point : link.points)
                links += " " + std::to_string(point.x) + "," + std::to_string(point.y);
        }
        const std::string reveal_group = ship ? "ship:" : "self:";
        Json nodes = Json::array();
        for (const auto &id : graph.ids)
        {
            const std::string st = state(id);
            const bool other = ShipPerk(graph, id) != ship;
            const auto &place = layout.nodes.at(id);
            nodes.push_back(Json{{"id", id}, {"name", PerkName(graph, id)},
                                 {"image", PerkPicture(graph, id, st != std::string("learned") && st != std::string("open"))},
                                 {"state", st}, {"selected", id == selected}, {"other", other},
                                 {"groupIcon", ShipPerk(graph, id) ? "ship" : "hero"},
                                 {"left", place.left}, {"top", place.top},
                                 {"reveal", id == selected ? reveal_group + id : ""},
                                 {"revealTop", place.top - kRevealMargin}});
        }
        view["tree"] = Json{{"nodes", nodes}, {"links", links}, {"width", perk_tree::Layout::width}, {"height", layout.height},
                            {"caption", layout.caption_top >= 0 ? bindings.Text("hero.perks_unlinked") : ""}, {"captionTop", layout.caption_top}};
        bindings.On("perks", "row", [this, &bindings, ids = graph.ids, rows = graph.rows](const Event &event) {
            if (std::find(ids.begin(), ids.end(), event.value) == ids.end())
                return;
            perk_ = rows.count(event.value) ? rows.at(event.value)->row_id : event.value;
            bindings.Rebuild(); // the description only: no engine command
        });

        // The panel of the selected perk: what it needs and what it opens, each a row that selects that perk.
        const Widget *fact = FactOf(graph, selected);
        const bool learned = Learned(graph, selected), can_learn = Available(graph, selected, full, free);
        const bool external = !graph.rows.count(selected), ship_perk = ShipPerk(graph, selected);
        std::map<std::string, std::pair<bool, bool>> targets; // a row's perk -> (of the ship group, in this table)
        const auto link = [&](const std::string &id, bool met) {
            const bool other = ShipPerk(graph, id) != ship;
            targets[id] = {ShipPerk(graph, id), graph.rows.count(id) > 0};
            return Json{{"perk", id}, {"name", PerkName(graph, id)}, {"image", PerkPicture(graph, id, !Learned(graph, id))},
                        {"met", met}, {"other", other}, {"groupIcon", other ? (ShipPerk(graph, id) ? "ship" : "hero") : ""}};
        };
        // What is still missing first, then this tab's perks before the other group's; a row of the other group
        // carries that group's glyph, so it is never read as one of this tab.
        const auto grouped = [&](Json rows) {
            std::stable_sort(rows.begin(), rows.end(), [](const Json &a, const Json &b) {
                return std::make_pair(a["met"].get<bool>(), a["other"].get<bool>()) < std::make_pair(b["met"].get<bool>(), b["other"].get<bool>());
            });
            return rows;
        };
        Json needs = Json::array(), opens = Json::array();
        std::string missing;
        for (const Widget *edge : graph.incoming.count(selected) ? graph.incoming.at(selected) : std::vector<const Widget *>())
        {
            const std::string source = perk_tree::Id(Field(edge, "source"));
            const bool met = Field(edge, "met") == "1";
            needs.push_back(link(source, met));
            if (!met)
                missing += (missing.empty() ? "" : ", ") + PerkName(graph, source);
        }
        for (const Widget *edge : graph.outgoing.count(selected) ? graph.outgoing.at(selected) : std::vector<const Widget *>())
        {
            const std::string target = perk_tree::Id(Field(edge, "target"));
            opens.push_back(link(target, Learned(graph, target)));
        }
        bindings.On("perk-detail", "go", [this, &bindings, targets, ship](const Event &event) {
            const auto found = targets.find(event.value);
            if (found != targets.end())
                JumpTo(bindings, found->first, found->second.first, found->second.second, ship);
        });
        // Why it cannot be learned now, in the order the stock window checks it (shown above the button).
        std::string reason;
        if (!graph.complete)
            reason = bindings.Text("items.unavailable");
        else if (!missing.empty())
            reason = bindings.Text("hero.learn_first") + " " + missing;
        else if (free <= 0)
            reason = bindings.Text("hero.no_points");
        else if (!can_learn)
            reason = bindings.Text("items.unavailable");
        Json detail{{"id", "detail:" + reveal_group + selected}, // a reveal key of its own: the panel scrolls back to its top
                    {"title", PerkName(graph, selected)},
                    {"text", StockText(Field(fact, "text"))},
                    {"image", PerkPicture(graph, selected, state(selected) != std::string("learned") && state(selected) != std::string("open"))},
                    {"group", external || ship_perk != ship ? GroupName(ship_perk) : ""},
                    {"groupIcon", ship_perk ? "ship" : "hero"},
                    {"blocked", !external && full && !learned && !can_learn ? reason : ""},
                    {"requires", grouped(needs)},
                    {"opens", grouped(opens)},
                    {"note", !external && Field(fact, "officer") == "1" ? bindings.Text("hero.officer_has") : ""},
                    {"learn", nullptr},
                    {"go", nullptr}};
        if (external)
        {
            // A perk of the other group is read here; it is learned in its own tab.
            detail["go"] = Json{{"id", "perk-open-group"}, {"label", bindings.Text("hero.open_group")}}; // the group: the header's sub line
            bindings.On("perk-open-group", "", [this, &bindings, selected, ship_perk, ship](const Event &) {
                JumpTo(bindings, selected, ship_perk, false, ship);
            });
        }
        else if (full)
        {
            // «Изучить», learned by a hold (the owner's rule for what cannot be undone: an early release shakes,
            // nothing is sent). Why a perk cannot be learned stands right above the button (`blocked`), so the
            // disabled button needs no toast. The hold starts the stock learn at once (Intent::Learn: the row's
            // ChoosePerk, then its PERK_ACCEPT, never drawn), so points, flags and unlocks are the stock ones.
            detail["learn"] = Json{{"id", "perk-learn"},
                                   {"label", bindings.Text(learned ? "hero.learned" : "hero.learn")},
                                   {"disabled", !can_learn},
                                   {"busy", intent_.kind == Intent::Learn}};
            bindings.On("perk-learn", "", [this, &bindings, can_learn, id = selected](const Event &) {
                if (!can_learn || intent_.kind != Intent::None)
                    return;
                Intent intent;
                intent.kind = Intent::Learn;
                intent.row = perk_;
                intent.learn_character = Field(a_->One("ANDROID_SUBJECT"), "character");
                const Widget *group = a_->One("TABBTN_2");
                intent.learn_ship = group && group->value == "1";
                bindings.Log("perk learn: " + id + " (character " + intent.learn_character + ", " + (intent.learn_ship ? "ship" : "personal") +
                             " group) by a hold on Изучить");
                sent_revision_ = 0; // a player action: its first step may go at once
                Start(bindings, intent);
                bindings.Rebuild();
            });
        }
        view["detail"] = detail;
    }

    void OfficersView(Json &view, Bindings &bindings)
    {
        Json on = Json::array(), free = Json::array(), passengers = Json::array();
        int posts = 0;
        for (const Widget *post : a_->All("ANDROID_POSTS"))
        {
            const int slot = std::atoi(Field(post, "slot").c_str());
            if (slot == 0)
                continue;
            ++posts;
            if (!Field(post, "character").empty())
                on.push_back(Json{{"id", std::to_string(slot)},
                                  {"name", post->label},
                                  {"meta", LowerText(Field(post, "role"))},
                                  {"image", Portrait(Field(post, "face"))},
                                  {"selected", intent_.kind == Intent::Post && intent_.row == std::to_string(slot)}});
            else
                free.push_back(Json{{"id", std::to_string(slot)}, {"role", Field(post, "role")}, {"icon", PostIcon(Field(post, "post"), slot)},
                                    {"selected", intent_.kind == Intent::Post && intent_.row == std::to_string(slot)}});
        }
        for (const Widget *passenger : a_->All("ANDROID_PASSENGERS"))
            passengers.push_back(Json{{"name", passenger->label},
                                      {"meta", bindings.Text("hero.rank_short") + " " + Field(passenger, "rank")},
                                      {"image", Portrait(Field(passenger, "face"))}});
        view["posts"] = on;
        view["postsCount"] = std::to_string(on.size()) + " " + bindings.Text("common.of") + " " + std::to_string(posts);
        view["freePosts"] = free;
        view["passengers"] = passengers;
        view["passengersCount"] = passengers.empty() ? "" : std::to_string(passengers.size());
        // A tap on a post (element "post-<slot>") opens the game's post chooser or remove window.
        for (const Json &rows : {on, free})
            for (const Json &row : rows)
            {
                const std::string slot = row.value("id", "");
                bindings.On("post-" + slot, "", [this, &bindings, slot](const Event &) {
                    Intent intent;
                    intent.kind = Intent::Post;
                    intent.row = slot;
                    Start(bindings, intent);
                });
            }
    }

    Json DialogView(Bindings &bindings)
    {
        if (!skill_help_.is_null() && window_ == Window::None && tab_ == Tab::Skills)
        {
            const auto close = [this, &bindings](const Event &) {
                skill_help_ = nullptr;
                bindings.Rebuild();
            };
            bindings.Modal("dialog", close);
            bindings.On("dialog-close", "", close);
            bindings.On("skill-help-close", "", close);
            return Json{{"kind", "message"}, {"title", skill_help_["title"]}, {"text", skill_help_["text"]},
                        {"actions", Json::array({Json{{"id", "skill-help-close"}, {"label", bindings.Text("common.close")}, {"variant", "primary"}}})}};
        }
        const Widget *slot = Selected("CHARACTERS_SCROLL");
        const Widget *post = nullptr;
        for (const Widget *p : a_->All("ANDROID_POSTS"))
            if (slot && Field(p, "slot") == std::to_string(slot->index))
                post = p;
        const auto close = [this, &bindings](const Event &) {
            sent_revision_ = 0; // a player action: its first step may go at once
            CloseWindow(bindings);
        };
        bindings.On("dialog-close", "", close);
        if (window_ == Window::Remove && post)
        {
            Json facts = Json::array();
            if (!Field(post, "skill").empty())
                facts.push_back(Json{{"k", Field(post, "skill")}, {"v", Field(post, "skillvalue")}});
            if (!Field(post, "loyalty").empty())
                facts.push_back(Json{{"k", bindings.Text("hero.loyalty")}, {"v", LowerText(Field(post, "loyalty"))}});
            if (Number(Field(post, "salary")) > 0)
                facts.push_back(Json{{"k", bindings.Text("hero.salary")}, {"v", Thousands(Field(post, "salary")) + " " + bindings.Text("hero.per_month")}});
            const Widget *remove = a_->One("REMOVE_ACCEPT_OFFICER");
            Json actions = Json::array();
            if (remove)
                actions.push_back(Json{{"id", "officer-remove"}, {"label", bindings.Text("hero.dismiss")}, {"variant", "secondary"},
                                       {"disabled", !remove->Offers("press")}, {"busy", bindings.Pending("officer-remove")}});
            actions.push_back(Json{{"id", "officer-skills"}, {"label", bindings.Text("hero.tab_skills")}, {"variant", "primary"}});
            bindings.On("officer-remove", "", [this, &bindings, remove](const Event &) {
                sent_revision_ = 0;
                Step(bindings, remove, "press");
            });
            bindings.On("officer-skills", "", [this, &bindings](const Event &) {
                tab_ = Tab::Skills;
                sent_revision_ = 0; // a player action: its first step may go at once
                CloseWindow(bindings);
                bindings.Rebuild();
            });
            bindings.Modal("dialog", close);
            return Json{{"kind", "officer"}, {"title", post->label}, {"subtitle", LowerText(Field(post, "role"))}, {"facts", facts}, {"actions", actions}};
        }
        if (window_ == Window::Assign)
        {
            Json rows = Json::array();
            for (const Widget *candidate : a_->All("ANDROID_CANDIDATES"))
            {
                std::string meta = bindings.Text("hero.rank_short") + " " + Field(candidate, "rank");
                if (Number(Field(candidate, "salary")) > 0)
                    meta += " · " + Thousands(Field(candidate, "salary")) + " " + bindings.Text("hero.per_month");
                rows.push_back(Json{{"id", Field(candidate, "index")},
                                    {"name", candidate->label},
                                    {"meta", meta},
                                    {"value", Field(candidate, "skill") + " " + Field(candidate, "skillvalue")},
                                    {"image", Portrait(Field(candidate, "face"))},
                                    {"busy", intent_.kind == Intent::Assign && intent_.row == Field(candidate, "index")}});
            }
            for (const Json &row : rows)
            {
                const std::string index = row.value("id", "");
                std::string character;
                for (const Widget *candidate : a_->All("ANDROID_CANDIDATES"))
                    if (Field(candidate, "index") == index)
                        character = Field(candidate, "character");
                const int post_index = slot ? slot->index : -1;
                bindings.On("candidate-" + index, "", [this, &bindings, index, character, post_index](const Event &) {
                    Intent intent;
                    intent.kind = Intent::Assign;
                    intent.row = index;
                    intent.candidate_character = character;
                    intent.candidate_post = post_index;
                    Start(bindings, intent);
                });
            }
            bindings.Modal("dialog", close);
            return Json{{"kind", "assign"}, {"title", post ? Field(post, "role") : std::string()}, {"rows", rows}, {"empty", bindings.Text("hero.no_candidates")}};
        }
        if (window_ == Window::Message)
        {
            const Widget *ok = a_->One("MSG_OK");
            Json actions = Json::array({ButtonView(bindings, *ok, "msg-ok", "primary")});
            bindings.Modal("dialog", close);
            return Json{{"kind", "message"}, {"title", Trim(a_->Label("MSG_WINDOW_CAPTION"))}, {"text", Trim(a_->Label("MSG_WINDOW_TEXT"))}, {"actions", actions}};
        }
        return nullptr;
    }
};

} // namespace

std::unique_ptr<Screen> CreateHeroScreen()
{
    return std::make_unique<HeroScreen>();
}

} // namespace storm::ui::screens
