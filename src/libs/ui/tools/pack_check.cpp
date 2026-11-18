// The pack contract check: the real UI pack drawn with the real screen and HUD adapters
// on the recorded models and states, without a GL context and without game data.
//
//   storm_ui_pack_check --pack DIR --fixtures DIR [--only NAME] [--crawl 0|1] [--shard K/N] [--actions FILE]
//                       [--app-contract FILE]
//
// The pack (the application's mobile-ui/design, built by its build.py) and the adapters (this
// library) live in two repositories; this is where they meet before a device does. --fixtures is
// tests/ of this library: every models/*.json interface model is a case, and pack-contract.json
// adds the cases that need state channels (dialogue, the HUD scenes, the launcher's screens, the death screen) or
// the game's answer to what a screen asks for on opening (`answer`), and lists the ported screens
// without a fixture, each with its reason. A case fails on any error line of the host (an
// unknown element, prop or icon, a missing string, an unbound event, a rejected admission, a
// screen that threw) and on any element the runtime marked `ui-error`. Every ported screen must
// be drawn by a case or be listed as uncovered; a listed screen that a case draws is an error too
// (the list only shrinks).
//
// The crawl (--crawl 1, the default) then makes every tap of the screen with a real touch, from the recorded state
// and from every state those taps open without the game (tabs, sheets, modals, pickers), and fails the case on
// - any host error in any of those states (a template branch never drawn before, an unbound tap);
// - a widget action that names a node the model does not have, or a verb the engine does not offer for it;
// - an engine command tools/android/app_contract.json does not list (--app-contract, default the engine's);
// - a tap that does nothing at all: no action, no change of what is drawn (a pressable that is bound to
//   nothing useful; a row that only shows something is drawn inert or with pointer-events: none);
// - a disabled control whose tap still sends something or opens something.
// --actions writes every action the taps sent (one JSON object a line) for the application's check that
// they end in the game's scripts and the application's Java (tests/ui_pack_contract/game_bindings.py).
// Exit 0 when every case is clean, 1 otherwise, 2 for bad arguments.
#include "../tests/runtime_stub.h"
#include "../src/components.h"
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
    fs::path answer;                                       // the game's next model of it, empty = none
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

// What a host did while a case ran: its error and warning lines, the actions its screens sent, the text
// editors they asked for.
struct Observed
{
    std::vector<std::string> errors, warnings;
    std::vector<Action> actions;
    std::vector<TextRequest> texts;
};

// One host with one case fed to it (states, interface, answer), settled.
class Session
{
  public:
    // `settle`: seconds drawn after the case is fed. The recorded state waits out the router's settle time (a model
    // its screen rejects closes the interface one second after the opening); the crawl's hosts replay a state that
    // was accepted already.
    Session(const fs::path &pack, const Case &c, Observed &observed, double settle = 1.5) : case_(c), observed_(observed)
    {
        files_ = DirectoryFiles(pack.string());
        Config config;
        config.strict_templates = true; // a template path that names nothing, a drawn tap that maps to nothing: errors
        config.files = files_.get();
        config.backend = &backend_;
        config.width = 2400; // a common phone in landscape: 914 x 411 dp
        config.height = 1080;
        config.density = 2.625f;
        // No frame prediction check (Config::verify_unchanged) here: it compares draw commands, which a
        // renderer that draws nothing cannot make as the real one does. Captures check it through the
        // real backend (storm_ui_shot), the runtime checks in tests/frame_cost_check.cpp.
        config.clock = [this] { return clock_; };
        config.on_log = [this](LogLevel level, const std::string &line) {
            // A danger notice the screen shows (a failure the player is told about) is logged at the error level
            // for the device's error log (Host Shown); here it is what the case shows, not a defect.
            if (level == LogLevel::Error && line.rfind("shown danger ", 0) != 0)
                observed_.errors.push_back(line);
            else if (level == LogLevel::Warning)
                observed_.warnings.push_back(line);
        };
        config.on_action = [this](const Action &action) { observed_.actions.push_back(action); };
        config.on_text_request = [this](const TextRequest &request) { observed_.texts.push_back(request); };
        host_ = Host::Create(config, &error);
        if (host_)
            Feed(settle);
    }

    // The crawl's way back to the case's state on the same host (a host starts slowly: fonts, the pack): the
    // interface closes and opens again as a new opening of the same model. A case drawn only from state channels
    // (a dialogue, the launcher) keeps its consumers' state; false then, and a fresh host is needed.
    bool Reopen(double settle)
    {
        if (case_.interface.empty())
            return false;
        host_->CloseInterface();
        Frames(1.0 / 60.0);
        ++reopened_;
        Feed(settle);
        return true;
    }

  private:
    InterfaceModel Opening(const fs::path &file)
    {
        InterfaceModel parsed;
        std::string why;
        if (!ParseInterfaceModel(Read(file), parsed, &why))
            failures.push_back("interface model " + file.string() + " not accepted: " + why);
        parsed.opening += reopened_ * 1000; // every reopening is an opening of its own
        return parsed;
    }

    void Feed(double settle)
    {
        if (!reopened_)
            for (const auto &[channel, file] : case_.states)
            {
                if (!host_->SetState(channel, Read(file)))
                    failures.push_back("state " + channel + "=" + file.string() + " not accepted");
                Frames(0.05);
            }
        if (!case_.interface.empty())
        {
            host_->ExpectInterface();
            model = Opening(case_.interface);
            host_->SetInterface(model);
            // A screen that asks the game for something on opening (a shop's selected row asks for its
            // deal) gets the model the game answers with, within the screen's hold.
            if (!case_.answer.empty())
            {
                Frames(0.05);
                model = Opening(case_.answer);
                host_->SetInterface(model);
            }
        }
        Frames(settle);
        AnswerAll();
    }

  public:
    ~Session()
    {
        host_.reset(); // before the files and the backend it uses
    }

    Host *operator->()
    {
        return host_.get();
    }
    explicit operator bool() const
    {
        return host_ != nullptr;
    }

    void Frames(double seconds)
    {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0)
        {
            clock_ += 1.0 / 60.0;
            host_->Frame();
        }
    }
    double Clock() const
    {
        return clock_;
    }

    // There is no game here: every widget action is answered as the engine answers one it took (applied, then the
    // model again as a new revision: a capability of the old one is used up), with nothing in it changed; every
    // text editor is cancelled.
    void AnswerAll()
    {
        bool applied = false;
        for (; answered_ < observed_.actions.size(); ++answered_)
            if (observed_.actions[answered_].request && observed_.actions[answered_].screen == model.screen)
            {
                host_->Answer(observed_.actions[answered_].request, true, {});
                applied = true;
            }
        if (applied)
        {
            ++model.revision;
            host_->SetInterface(model);
        }
        for (; cancelled_ < observed_.texts.size(); ++cancelled_)
            host_->SubmitText(observed_.texts[cancelled_].token, {}, false);
        Frames(0.1);
    }

    // Lets what a tap started finish (a drag's delayed send, a step toward an amount, the screen's timed rebuilds)
    // and answers it, so that the next tap is the only thing its result comes from.
    void Settle()
    {
        AnswerAll();
        Frames(1.2);
        AnswerAll();
    }

    std::string error;
    std::vector<std::string> failures; // what of the case was not accepted
    InterfaceModel model;              // the newest interface model fed

  private:
    const Case &case_;
    int reopened_ = 0;
    Observed &observed_;
    std::unique_ptr<FileSource> files_;
    NoBackend backend_;
    std::unique_ptr<Host> host_;
    double clock_ = 0.0;
    size_t answered_ = 0, cancelled_ = 0;
};

// --- The crawl -------------------------------------------------------------------------------------------------
//
// Every state of a screen that taps reach without the game (tabs, sheets, modals, pickers) is drawn and checked
// like the recorded one: from the case's state each tap the screen draws is made with a real touch, the screen
// documents before and after it are compared, and a tap that changed what the screen offers opens a state of its
// own, crawled the same way (to kMaxDepth taps from the recorded state, kMaxStates states per case). There is no
// game: the widget actions taps send are recorded, checked against the model the screen was built from (the node
// exists, the engine offers the verb) and refused at once.

constexpr int kMaxDepth = 4;
constexpr size_t kMaxStates = 40;
constexpr double kReplaySettle = 0.1;
// Taps of one element with one verb (the rows of a list, the cells of a grid) share their binding: the first
// kSampleTaps of them and the last are made in each state.
constexpr int kSampleTaps = 3;
// Longer than any screen waits for the game's answer to a step of its own (the hero screens: 3 s).
constexpr double kStepTimeout = 5.5;

struct TapAt
{
    std::string key;   // tag#id verb value [#n]: the same tap in a fresh host with the same path
    std::string group; // tag#id verb: taps that share a binding
    Rml::Element *part = nullptr;
    ComponentElement *component = nullptr;
    bool hold = false;
    bool disabled = false; // drawn disabled: a tap may shake it or show why, and must send nothing
};

Rml::Element *ScreenElement()
{
    for (int c = 0; c < Rml::GetNumContexts(); ++c)
        if (Rml::Context *context = Rml::GetContext(c))
            for (int d = 0; d < context->GetNumDocuments(); ++d)
                if (Rml::Element *screen = context->GetDocument(d)->GetElementById("screen"); screen && context->GetDocument(d)->IsVisible())
                    return screen;
    return nullptr;
}

std::string EventId(Rml::Element *element)
{
    for (; element; element = element->GetParentNode())
        if (!element->GetId().empty())
            return element->GetId();
    return {};
}

void CollectTaps(Rml::Element *element, std::vector<TapAt> &out, std::map<std::string, int> &seen)
{
    if (auto *component = dynamic_cast<ComponentElement *>(element); component && component->HasBehaviors())
    {
        std::vector<Tap> taps;
        component->Taps(taps);
        for (const Tap &tap : taps)
        {
            const std::string group = component->GetTagName() + "#" + EventId(component) + " " + tap.verb;
            std::string key = group + (tap.value.empty() ? "" : " " + tap.value);
            if (const int n = seen[key]++)
                key += " #" + std::to_string(n + 1);
            out.push_back({key, group, tap.part, component, tap.hold});
        }
        // The disabled ones of a press component: the component itself and its parts marked data-disabled.
        const auto &behaviors = component->Spec().behaviors;
        const bool untouchable = component->GetComputedValues().pointer_events() == Rml::Style::PointerEvents::None;
        if (std::find(behaviors.begin(), behaviors.end(), "press") != behaviors.end() && !untouchable && !component->Flag("busy"))
        {
            std::vector<std::pair<Rml::Element *, std::string>> off;
            if (component->Flag("disabled") && !component->Flag("inert") && component->Spec().constants.value("pressRoot", true))
                off.emplace_back(component, "click");
            Rml::ElementList parts;
            component->QuerySelectorAll(parts, "[data-action][data-disabled]");
            for (Rml::Element *part : parts)
            {
                Rml::Element *owner = part->GetParentNode();
                while (owner && !dynamic_cast<ComponentElement *>(owner))
                    owner = owner->GetParentNode();
                if (owner == component && part->GetComputedValues().pointer_events() != Rml::Style::PointerEvents::None)
                    off.emplace_back(part, part->GetAttribute<Rml::String>("data-action", "click"));
            }
            for (const auto &[part, verb] : off)
            {
                const std::string group = component->GetTagName() + "#" + EventId(component) + " " + verb + " (disabled)";
                const std::string value = part->GetAttribute<Rml::String>("data-value", "");
                std::string key = group + (value.empty() ? "" : " " + value);
                if (const int n = seen[key]++)
                    key += " #" + std::to_string(n + 1);
                out.push_back({key, group, part, component, false, true});
            }
        }
    }
    for (int i = 0; i < element->GetNumChildren(); ++i)
        CollectTaps(element->GetChild(i), out, seen);
}

std::vector<TapAt> Taps()
{
    std::vector<TapAt> taps;
    std::map<std::string, int> seen;
    if (Rml::Element *screen = ScreenElement())
        CollectTaps(screen, taps, seen);
    return taps;
}

// What the screen offers: its taps, in order. Two states offering the same taps are one state of the crawl.
std::string Signature(const std::vector<TapAt> &taps)
{
    std::string out;
    for (const TapAt &tap : taps)
        if (!tap.disabled)
            out += tap.key + "\n";
    return out;
}

// What is drawn: every visible document's tree (a toast, a selection, a changed detail all show here).
std::string Drawn()
{
    std::string out;
    for (int c = 0; c < Rml::GetNumContexts(); ++c)
        if (Rml::Context *context = Rml::GetContext(c))
            for (int d = 0; d < context->GetNumDocuments(); ++d)
                if (context->GetDocument(d)->IsVisible())
                    out += context->GetDocument(d)->GetInnerRML();
    return out;
}

bool Within(Rml::Element *element, Rml::Element *ancestor)
{
    for (; element; element = element->GetParentNode())
        if (element == ancestor)
            return true;
    return false;
}

// A point of the tap's part where a finger reaches it now: the topmost element there is the part or inside
// it, and not inside another press part of it. False when the part is covered (a modal over it) or off screen.
bool Reach(Session &session, const TapAt &tap, Rml::Vector2f &at)
{
    tap.part->ScrollIntoView(false);
    session.Frames(2.0 / 60.0);
    Rml::Context *context = tap.part->GetContext();
    const Rml::Vector2f origin = tap.part->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = tap.part->GetBox().GetSize(Rml::BoxArea::Border);
    if (!context || size.x <= 0.0f || size.y <= 0.0f)
        return false;
    for (const float fx : {0.5f, 0.15f, 0.85f})
        for (const float fy : {0.5f, 0.25f, 0.75f})
        {
            at = origin + Rml::Vector2f(size.x * fx, size.y * fy);
            if (at.x < 0 || at.y < 0 || at.x >= context->GetDimensions().x || at.y >= context->GetDimensions().y)
                continue;
            Rml::Element *hit = context->GetElementAtPoint(at);
            if (!Within(hit, tap.part))
                continue;
            bool other = false;
            for (Rml::Element *at_hit = hit; at_hit && at_hit != tap.part && !other; at_hit = at_hit->GetParentNode())
                other = at_hit->HasAttribute("data-action") || dynamic_cast<ComponentElement *>(at_hit);
            if (!other)
                return true;
        }
    return false;
}

void Press(Session &session, const TapAt &tap, Rml::Vector2f at)
{
    session->Touch(TouchPhase::Down, 1, at.x, at.y, session.Clock());
    session.Frames(tap.hold ? 1.5 : 1.0 / 60.0);
    session->Touch(TouchPhase::Up, 1, at.x, at.y, session.Clock());
    session.Frames(0.1);
}

// A game interface's widget action against the model its screen was built from.
std::string CheckAction(const InterfaceModel &model, const Action &action)
{
    if (action.screen != model.screen)
        return "sent to the interface " + action.screen + " while " + model.screen + " is open";
    const Widget *node = nullptr, *exact = nullptr;
    for (const Widget &widget : model.widgets)
        if (SameNode(widget.node, action.node))
        {
            node = node ? node : &widget;
            if (widget.index == action.index)
                exact = exact ? exact : &widget;
        }
    if (!node)
        return "sent to the node " + action.node + ", which the model does not have";
    if (!exact)
        return "sent to " + action.node + "[" + std::to_string(action.index) + "], a row the model does not have";
    if (!exact->actions.empty())
    {
        Action bare = action;
        bare.guard.clear();
        const std::string wire = WidgetWire(bare);
        for (const std::string &offer : exact->actions)
            if (wire == offer || action.verb == offer || wire.rfind(offer + "=", 0) == 0)
                return {};
        std::string offers;
        for (const std::string &offer : exact->actions)
            offers += (offers.empty() ? "" : ", ") + offer;
        return "sends " + wire + " to " + action.node + "[" + std::to_string(action.index) + "], which offers only " + offers;
    }
    static const std::set<std::string> kStandard = {"button", "choice", "check", "edit", "stepper"};
    if (kStandard.count(exact->type) && !exact->Offers(action.verb))
        return "sends " + action.verb + " to the " + (exact->enabled ? "" : "disabled ") + exact->type + " " + action.node + "[" +
               std::to_string(action.index) + "], which does not take it";
    return {};
}

struct Crawl
{
    int states = 0, taps = 0, unreachable = 0;
    std::vector<std::string> findings; // errors: each with the path of taps that leads to it
    std::vector<std::string> inert;    // taps that did nothing at all
    std::vector<std::string> notes;    // states the crawl could not open again the same way (timed screen state): not crawled further
};

std::string PathText(const std::vector<std::string> &path)
{
    std::string out;
    for (const std::string &key : path)
        out += (out.empty() ? "" : " > ") + key;
    return out.empty() ? "(recorded state)" : out;
}

// Makes the taps of `path` in a fresh session; false when one is no longer offered or reachable.
bool Replay(Session &session, const std::vector<std::string> &path)
{
    for (const std::string &key : path)
    {
        const std::vector<TapAt> taps = Taps();
        const auto found = std::find_if(taps.begin(), taps.end(), [&](const TapAt &tap) { return tap.key == key; });
        Rml::Vector2f at;
        if (found == taps.end() || !Reach(session, *found, at))
            return false;
        Press(session, *found, at);
        session.Settle();
    }
    return true;
}

void RunCrawl(const fs::path &pack, const Case &c, Crawl &crawl, std::vector<std::string> &records, const Json &commands)
{
    std::vector<std::vector<std::string>> queue = {{}};
    std::set<std::string> seen;
    std::set<std::string> reported;
    const auto finding = [&](const std::vector<std::string> &path, const std::string &what) {
        if (reported.insert(what).second)
            crawl.findings.push_back(what + "\n      via " + PathText(path));
    };
    Observed observed;
    std::unique_ptr<Session> session;
    // The case's state again: the same host reopened, or a fresh one.
    const auto restart = [&]() {
        if (session && session->Reopen(kReplaySettle))
            return true;
        session.reset();
        session = std::make_unique<Session>(pack, c, observed, kReplaySettle);
        return static_cast<bool>(*session);
    };
    while (!queue.empty() && seen.size() < kMaxStates)
    {
        const std::vector<std::string> path = queue.front();
        queue.erase(queue.begin());
        const size_t errors_before = observed.errors.size();
        if (!restart())
            return;
        if (!Replay(*session, path))
        {
            crawl.notes.push_back("the path no longer replays (a tap of it is gone or covered when the state is opened again): " + PathText(path));
            continue;
        }
        const std::string signature = Signature(Taps());
        if (!seen.insert(signature).second)
            continue;
        ++crawl.states;
        for (size_t e = errors_before; e < observed.errors.size(); ++e)
            finding(path, observed.errors[e]);
        std::vector<size_t> sampled; // indexes of the taps made in this state
        {
            const std::vector<TapAt> all = Taps();
            std::map<std::string, std::vector<size_t>> groups;
            for (size_t i = 0; i < all.size(); ++i)
                groups[all[i].group].push_back(i);
            for (size_t i = 0; i < all.size(); ++i)
            {
                const std::vector<size_t> &group = groups[all[i].group];
                const size_t at = std::find(group.begin(), group.end(), i) - group.begin();
                if (at < kSampleTaps || at + 1 == group.size())
                    sampled.push_back(i);
            }
        }
        bool dirty = false;
        for (const size_t i : sampled)
        {
            if (dirty)
            {
                if (!restart() || !Replay(*session, path))
                    break;
                dirty = false;
            }
            std::vector<TapAt> taps = Taps();
            if (i < taps.size() && taps[i].disabled)
            {
                // Nothing left over from the taps before may look like this one's: the screen's timed work (a drag's
                // delayed send, a step's timeout) is let run out first. If that changed the state, the tap is not made.
                session->Settle();
                session->Frames(kStepTimeout);
                session->AnswerAll();
                taps = Taps();
                if (Signature(taps) != signature)
                {
                    dirty = true;
                    continue;
                }
            }
            if (i >= taps.size() || Signature(taps) != signature)
            {
                crawl.notes.push_back("the state does not come back the same when it is opened again: " + PathText(path));
                break;
            }
            const TapAt &tap = taps[i];
            Rml::Vector2f at;
            if (!Reach(*session, tap, at))
            {
                ++crawl.unreachable;
                continue;
            }
            ++crawl.taps;
            // Choosing what is chosen already (a selected row or tab) rightly does nothing.
            bool chosen = tap.component->Flag("selected");
            for (Rml::Element *at_part = tap.part; at_part && at_part != tap.component && !chosen; at_part = at_part->GetParentNode())
                chosen = at_part->IsClassSet("sel") || at_part->IsClassSet("selected");
            const std::string before = Drawn();
            const size_t errors = observed.errors.size(), actions = observed.actions.size(), texts = observed.texts.size();
            Press(*session, tap, at);
            std::vector<std::string> next = path;
            next.push_back(tap.key);
            bool acted = observed.texts.size() > texts;
            for (size_t a = actions; a < observed.actions.size(); ++a)
            {
                const Action &action = observed.actions[a];
                if (action.screen == "sound")
                    continue;
                acted = true;
                const std::string command = EngineCommand(action);
                if (tap.disabled)
                    finding(next, "screen " + action.screen + ": " + tap.key + " is drawn disabled and still sends " +
                                      (command.empty() ? action.node + " " + action.verb : command));
                Json record = {{"case", c.name}, {"screen", action.screen}, {"node", action.node}, {"index", action.index},
                               {"verb", action.verb}, {"value", action.value}, {"command", command}, {"tap", tap.key},
                               {"path", PathText(next)}};
                if (AnsweredAction(action))
                {
                    record["wire"] = WidgetWire(action);
                    const Widget *widget = session->model.Find(action.node);
                    record["type"] = widget ? widget->type : std::string();
                    if (const std::string wrong = CheckAction(session->model, action); !wrong.empty())
                        finding(next, "screen " + action.screen + ": " + tap.key + " " + wrong);
                }
                else if (!command.empty() && action.screen.rfind("app:", 0) != 0) // the application's screens: its Java (game_bindings.py)
                {
                    bool known = false;
                    for (const auto &[prefix, owner] : commands.items())
                        known = known || command == prefix || (prefix.back() == ':' || prefix.back() == '\t') && command.rfind(prefix, 0) == 0;
                    if (!known)
                        finding(next, "screen " + action.screen + ": " + tap.key + " sends the engine command '" + command +
                                          "', which tools/android/app_contract.json does not list");
                }
                records.push_back(record.dump());
            }
            session->AnswerAll();
            for (size_t e = errors; e < observed.errors.size(); ++e)
                finding(next, observed.errors[e]);
            const std::string after_signature = Signature(Taps());
            const bool moved = after_signature != signature;
            if (tap.disabled)
            {
                // It may shake or show why; it may not open anything.
                if (moved)
                    finding(next, "screen " + session->model.screen + ": " + tap.key + " is drawn disabled and still changes what the screen offers");
                dirty = dirty || acted || moved || Drawn() != before;
                continue;
            }
            if (!acted && !moved && !chosen && Drawn() == before && observed.errors.size() == errors)
                crawl.inert.push_back(tap.key + " via " + PathText(path));
            if (moved)
            {
                dirty = true;
                if (static_cast<int>(next.size()) <= kMaxDepth && !seen.count(after_signature))
                    queue.push_back(next);
            }
            else if (acted)
                dirty = true; // what the screen does after it sent something waits for the game's answer: a fresh host
        }
    }
}

// Runs one case: the recorded state on a host of its own, then the crawl; prints what is wrong and returns false then.
bool Run(const fs::path &pack, const Case &c, std::vector<std::string> &records, const Json &commands, bool crawl_states)
{
    Observed observed;
    std::vector<std::string> marked;
    int broken = 0;
    std::vector<std::string> failures;
    bool started = true;
    std::string error;
    {
        Session session(pack, c, observed);
        if (!session)
        {
            started = false;
            error = session.error;
        }
        else
        {
            broken = MarkedBroken(marked);
            failures = session.failures;
        }
    }
    if (!started)
    {
        std::printf("FAIL %s: the UI host does not start with this pack: %s\n", c.name.c_str(), error.c_str());
        return false;
    }
    Crawl crawl;
    if (crawl_states)
        RunCrawl(pack, c, crawl, records, commands);
    const bool ok = failures.empty() && observed.errors.empty() && broken == 0 && crawl.findings.empty() && crawl.inert.empty();
    std::printf("%s %s", ok ? "PASS" : "FAIL", c.name.c_str());
    if (crawl_states)
        std::printf(" (%d state%s, %d tap%s", crawl.states, crawl.states == 1 ? "" : "s", crawl.taps, crawl.taps == 1 ? "" : "s");
    if (!observed.warnings.empty())
        std::printf("%s%zu warning%s", crawl_states ? ", " : " (", observed.warnings.size(), observed.warnings.size() == 1 ? "" : "s");
    if (crawl_states || !observed.warnings.empty())
        std::printf(")");
    std::printf("\n");
    for (const std::string &line : failures)
        std::printf("  failed: %s\n", line.c_str());
    for (const std::string &line : observed.errors)
        std::printf("  error: %s\n", line.c_str());
    for (const std::string &line : marked)
        std::printf("  marked ui-error: %s\n", line.c_str());
    for (const std::string &line : crawl.findings)
        std::printf("  crawl: %s\n", line.c_str());
    for (const std::string &line : crawl.inert)
        std::printf("  inert tap (does nothing at all): %s\n", line.c_str());
    for (const std::string &line : crawl.notes)
        std::printf("  note: %s\n", line.c_str());
    if (!ok)
        for (const std::string &line : observed.warnings)
            std::printf("  warning: %s\n", line.c_str());
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    fs::path pack, fixtures, actions_out, contract;
    std::string only;
    bool crawl = true;
    int shard = 0, shards = 1; // --shard K/N: the cases K, K+N, …; the coverage check in shard 0 (run them in parallel)
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--pack")
            pack = value;
        else if (key == "--fixtures")
            fixtures = value;
        else if (key == "--only")
            only = value;
        else if (key == "--actions")
            actions_out = value;
        else if (key == "--app-contract")
            contract = value;
        else if (key == "--crawl")
            crawl = value != "0";
        else if (key == "--shard")
        {
            if (std::sscanf(value.c_str(), "%d/%d", &shard, &shards) != 2 || shards < 1 || shard < 0 || shard >= shards)
            {
                std::fprintf(stderr, "--shard takes K/N with 0 <= K < N\n");
                return 2;
            }
        }
        else
        {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }
    if (pack.empty() || fixtures.empty() || argc % 2 == 0)
    {
        std::fprintf(stderr, "usage: storm_ui_pack_check --pack DIR --fixtures DIR [--only NAME] [--crawl 0|1] [--shard K/N] [--actions FILE] [--app-contract FILE]\n");
        return 2;
    }
    // The engine commands the application takes (engine tools/android/app_contract.json): a screen that sends
    // another one sends it to nothing.
    if (contract.empty())
        contract = fixtures / "../../../../tools/android/app_contract.json";
    const Json app_contract = Json::parse(Read(contract), nullptr, false);
    if (!app_contract.is_object() || !app_contract.contains("engine_commands"))
    {
        std::fprintf(stderr, "%s: no engine_commands (pass the engine's tools/android/app_contract.json with --app-contract)\n", contract.c_str());
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
        if (entry.contains("answer"))
        {
            c.answer = fixtures / entry["answer"].get<std::string>();
            used.insert(c.answer);
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
    for (const screens::ScreenEntry &entry : shard == 0 ? screens::PortedScreens() : std::span<const screens::ScreenEntry>())
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

    int failed = 0, ran = 0, index = -1;
    std::vector<std::string> records; // every action the crawl's taps sent, one JSON object a line (--actions)
    for (const Case &c : cases)
    {
        if (!only.empty() && c.name != only)
            continue;
        if (++index % shards != shard)
            continue;
        ++ran;
        if (!Run(pack, c, records, app_contract["engine_commands"], crawl))
            ++failed;
    }
    if (!actions_out.empty())
    {
        std::ofstream out(actions_out);
        for (const std::string &line : records)
            out << line << "\n";
        if (!out)
        {
            std::fprintf(stderr, "cannot write %s\n", actions_out.c_str());
            return 2;
        }
    }
    std::printf("pack contract: %d of %d cases clean, %d coverage/manifest problem%s\n", ran - failed, ran, problems, problems == 1 ? "" : "s");
    return failed || problems ? 1 : 0;
}
