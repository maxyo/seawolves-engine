#include "controls.h"

#include "../hud/hud.h"

#include <algorithm>
#include <exception>
#include <string_view>
#include <unordered_set>

namespace storm::ui
{

std::string EngineCommand(const Action &action)
{
    if (action.screen.empty())
        return {};
    if (action.screen == "sound") // the interface click of a press that started (Services::PressStarted)
        return "uisound:" + action.verb;
    if (action.screen == "hud") // HUD input: the verb names the engine entry point
        return action.verb + ":" + action.value;
    if (action.screen == "dialogue") // reply: "<generation>:<index>"; edit: the typed reply text; no back
        return action.verb == "back" ? std::string()
             : action.verb == "edit" ? "dialogue:" + std::to_string(action.opening) + ":" + std::to_string(action.index) + ":" + action.value
                                     : "dialogue:" + action.value;
    if (action.screen == "mainmenu") // the title screen's commands: "main:New", "main:Load", …
        return action.verb == "back" ? "back" : "main:" + action.node;
    if (action.verb == "quick" && action.node.empty()) // Bindings::QuickGame
        return "quick:" + action.value;
    if (action.node.empty())
        return action.verb == "back" ? "back" : std::string();
    return "widget:" + action.screen + "\t" + action.node + "\t" + std::to_string(action.index) + "\t" + WidgetWire(action);
}

bool AnsweredAction(const Action &action)
{
    return !action.screen.empty() && !action.node.empty() && action.screen != "sound" && action.screen != "hud" &&
           action.screen != "dialogue" && action.screen != "mainmenu" && action.screen != "game_over" &&
           action.screen.rfind("app:", 0) != 0; // the launcher's screens: the application's own host
}

std::string WidgetWire(const Action &action)
{
    const std::string stamp = "@" + std::to_string(action.opening) + ":";
    std::string wire;
    if (action.verb == "toggle" || action.verb == "edit")
        wire = action.value;
    else if (action.verb == "set" || !action.value.empty())
        wire = action.verb + stamp + action.value;
    else
        wire = action.verb;
    if (!action.guard.empty())
        wire = "guard" + stamp + action.guard + "|" + wire;
    return wire;
}

} // namespace storm::ui

namespace storm::ui::screens
{

namespace
{

constexpr double kPendingSeconds = 1.5; // a press that sent no widget action (back, a dialogue reply) stops looking busy
constexpr double kAnswerSeconds = 5.0;  // a widget action the engine has not answered by then is an error
constexpr double kFlushSeconds = 0.12;  // drags reach the engine at most this often
constexpr double kIdle = 3600.0;
// The game publishes an interface while its script is still filling it (the store's first
// revision has no goods yet). For this long after an opening a rejected model is not drawn
// and not reported; a newer revision that passes admission shows the ported screen.
constexpr double kSettle = 1.0;

bool Contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

bool Matches(const NodeRule &rule, const Widget &widget)
{
    const std::string_view pattern = rule.node;
    const bool prefix = !pattern.empty() && pattern.back() == '*';
    const std::string_view name = prefix ? pattern.substr(0, pattern.size() - 1) : pattern;
    if (prefix ? !(widget.node.size() >= name.size() && SameNode(std::string_view(widget.node).substr(0, name.size()), name))
               : !SameNode(widget.node, name))
        return false;
    return true;
}

bool TypeFits(const NodeRule &rule, const Widget &widget)
{
    std::string_view types = rule.type;
    while (!types.empty())
    {
        const size_t bar = types.find('|');
        const std::string_view one = types.substr(0, bar);
        if (one == "*" || one == widget.type)
            return true;
        types = bar == std::string_view::npos ? std::string_view() : types.substr(bar + 1);
    }
    return false;
}

} // namespace

bool Hidden(const Widget &widget)
{
    const std::string node = Upper(widget.node);
    const bool has_verbs = !widget.actions.empty();
    if (node.rfind("DEBUG", 0) == 0 || (node.size() > 4 && node.compare(node.size() - 4, 4, "_DBG") == 0))
        return true;
    if (Contains(node, "SCROLL") && !has_verbs && widget.type != "choice" && widget.type != "text")
        return true;
    if (widget.type == "image")
        return widget.label.empty() || !widget.Offers("press"); // pictures are drawn only by screens that know them
    if (widget.type == "edit" || widget.type == "slider" || widget.type == "stepper")
        return false;
    if (widget.type == "choice" && (!widget.cells.empty() || !widget.images.empty()))
        return false;
    if (widget.label.empty() && widget.images.empty())
        return true;
    if (widget.type == "button" && !widget.enabled && widget.unavailable_reason.empty() && !has_verbs && widget.label.empty())
        return true;
    // Form-specific types mean something only to their form; elsewhere they need a verb.
    if (widget.type == "save" || widget.type.rfind("nation_help", 0) == 0)
        return !has_verbs;
    return false;
}

const Widget *Admission::One(std::string_view node) const
{
    for (const Widget *widget : known)
        if (SameNode(widget->node, node))
            return widget;
    return nullptr;
}

std::vector<const Widget *> Admission::All(std::string_view node) const
{
    std::vector<const Widget *> out;
    for (const Widget *widget : known)
        if (SameNode(widget->node, node))
            out.push_back(widget);
    return out;
}

std::vector<const Widget *> Admission::Rows(std::string_view node) const
{
    std::vector<const Widget *> out;
    for (const Widget *widget : known)
        if (SameNode(widget->node, node) && widget->type == "choice")
            out.push_back(widget);
    return out;
}

std::string Admission::Label(std::string_view node) const
{
    const Widget *widget = One(node);
    return widget ? widget->label : std::string();
}

Admission Admit(const InterfaceModel &model, std::span<const NodeRule> rules)
{
    Admission out;
    out.model = &model;
    std::vector<bool> met(rules.size(), false);
    for (const Widget &widget : model.widgets)
    {
        bool known = false, named = false;
        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (!Matches(rules[i], widget))
                continue;
            named = true;
            if (TypeFits(rules[i], widget))
            {
                known = true;
                met[i] = true;
                break;
            }
        }
        if (known)
            out.known.push_back(&widget);
        else if (!Hidden(widget))
        {
            // An alias ("NAME_2") of a node the screen draws is not an extra.
            const std::string node = Upper(widget.node);
            const bool alias = named || (node.size() > 2 && node.compare(node.size() - 2, 2, "_2") == 0 &&
                                         std::any_of(rules.begin(), rules.end(), [&](const NodeRule &rule) {
                                             return SameNode(rule.node, std::string_view(node).substr(0, node.size() - 2));
                                         }));
            if (!alias || named)
                out.extras.push_back(&widget);
        }
    }
    out.accepted = true;
    for (size_t i = 0; i < rules.size(); ++i)
        if (rules[i].required && !met[i])
        {
            out.accepted = false;
            out.missing += (out.missing.empty() ? "" : ", ") + std::string(rules[i].node) + "(" + rules[i].type + ")";
        }
    return out;
}

// --- Bindings ---------------------------------------------------------------------------

void Bindings::On(const std::string &id, const std::string &verb, Handler handler)
{
    // A scrim tap is a modal's, and goes through Modal (one path for every modal).
    if (verb == "dismiss")
        router_.surface_.Error("screen " + router_.using_ + ": " + id + " dismiss bound by hand; declare the modal with Bindings::Modal");
    handlers_[id + '\x1f' + verb] = std::move(handler);
}

void Bindings::Modal(const std::string &id, Handler cancel)
{
    if (!cancel)
    {
        Router &router = router_;
        cancel = [&router, id](const Event &event) {
            router.surface_.Log("screen " + router.using_ + ": modal " + id + " must be answered by its buttons; " +
                                (event.verb == "back" ? "the back key" : "a tap beside it") + " refused");
            router.surface_.Refuse(id);
        };
        router_.modals_.emplace_back(id, nullptr);
    }
    else
        router_.modals_.emplace_back(id, cancel);
    handlers_[id + '\x1f' + "dismiss"] = cancel;
    handlers_[id + '\x1f' + "back"] = std::move(cancel);
}

void Bindings::Predict(const std::string &id, const std::string &verb, Prediction predict)
{
    predictions_[id + '\x1f' + verb] = std::move(predict);
}

bool Bindings::Patch(Json &view, const std::string &id, const char *key, const Json &value)
{
    bool found = false;
    if (view.is_object())
    {
        if (const auto own = view.find("id"); own != view.end() && own->is_string() && own->get_ref<const std::string &>() == id)
        {
            view[key] = value;
            found = true;
        }
        for (auto &[name, child] : view.items())
            if (child.is_structured())
                found = Patch(child, id, key, value) || found;
    }
    else if (view.is_array())
        for (Json &child : view)
            if (child.is_structured())
                found = Patch(child, id, key, value) || found;
    return found;
}

void Bindings::SelectOnly(Json &entries, size_t chosen)
{
    if (!entries.is_array() || chosen >= entries.size())
        return;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].is_object())
            entries[i]["selected"] = i == chosen;
}

void Bindings::Ignore(const std::string &id, const std::string &verb)
{
    On(id, verb, [](const Event &) {});
}

void Bindings::Forward(const std::string &id, const Widget &widget, const std::string &verb)
{
    const Widget *target = &widget;
    On(id, "", [this, target, verb](const Event &) { Send(*target, verb); });
}

void Bindings::Send(const Widget &widget, const std::string &verb, const std::string &value, const std::string &guard)
{
    router_.Emit(router_.Make(widget, verb, value, guard));
}

void Bindings::SendLatest(const Widget &widget, const std::string &verb, const std::string &value, const std::string &guard,
                          const std::optional<std::string> &lead)
{
    Action action = router_.Make(widget, verb, value, guard);
    std::optional<Action> first;
    if (lead)
        first = router_.Make(widget, verb, *lead, guard);
    // A send that waits for its turn still changed what the screen shows (the amount the
    // player just set): the view is built again now, not when it goes out.
    router_.rebuild_ = true;
    for (Router::Waiting &waiting : router_.waiting_)
        if (waiting.action.node == action.node && waiting.action.index == action.index && waiting.action.verb == action.verb)
        {
            waiting.action = std::move(action);
            waiting.lead = std::move(first);
            return;
        }
    const double now = router_.surface_.Now();
    if (now - router_.last_flush_ >= kFlushSeconds)
    {
        router_.last_flush_ = now;
        if (first)
            router_.Emit(std::move(*first));
        router_.Emit(std::move(action));
        return;
    }
    router_.waiting_.push_back({std::move(action), router_.last_flush_ + kFlushSeconds, std::move(first)});
}

void Bindings::SendAfter(double seconds, const Widget &widget, const std::string &verb, const std::string &value, const std::string &guard,
                         const std::optional<std::string> &lead)
{
    Action action = router_.Make(widget, verb, value, guard);
    std::optional<Action> first;
    if (lead)
        first = router_.Make(widget, verb, *lead, guard);
    const double due = router_.surface_.Now() + seconds;
    router_.rebuild_ = true; // as SendLatest: the view shows what was asked at once
    for (Router::Waiting &waiting : router_.waiting_)
        if (waiting.action.node == action.node && waiting.action.index == action.index && waiting.action.verb == action.verb)
        {
            waiting.action = std::move(action);
            waiting.lead = std::move(first);
            waiting.due = due;
            return;
        }
    router_.waiting_.push_back({std::move(action), due, std::move(first)});
}

bool Bindings::Waiting(const Widget &widget, const std::string &verb) const
{
    for (const Router::Waiting &waiting : router_.waiting_)
        if (waiting.action.node == widget.node && waiting.action.index == widget.index && waiting.action.verb == verb)
            return true;
    return false;
}

void Bindings::SendHost(const std::string &node, const std::string &value)
{
    Action action;
    action.node = node;
    action.verb = "set";
    action.value = value;
    router_.Emit(std::move(action));
}

void Bindings::Back(const Widget *exit_button)
{
    if (exit_button && exit_button->Offers("press"))
    {
        Send(*exit_button, "press");
        return;
    }
    Action action;
    action.screen = router_.model_.screen;
    action.opening = router_.model_.opening;
    action.verb = "back";
    router_.Emit(std::move(action));
}

void Bindings::QuickGame(const std::string &which)
{
    Action action;
    action.screen = router_.model_.screen;
    action.opening = router_.model_.opening;
    action.verb = "quick";
    action.value = which;
    router_.Emit(std::move(action));
}

void Bindings::EditText(const Event &event, const Widget &widget, const std::string &title, bool numeric, bool secret)
{
    TextRequest request;
    request.token = std::to_string(++router_.edit_serial_);
    request.text = widget.value;
    request.title = title;
    request.numeric = numeric;
    request.secret = secret;
    router_.edit_token_ = request.token;
    router_.edit_action_ = router_.Make(widget, "edit", {}, {});
    router_.edit_handler_ = nullptr;
    router_.surface_.RequestText(std::move(request), event.id);
}

void Bindings::AskText(const Event &event, const std::string &text, const std::string &title, bool numeric,
                       std::function<void(const std::string &)> accepted)
{
    TextRequest request;
    request.token = std::to_string(++router_.edit_serial_);
    request.text = text;
    request.title = title;
    request.numeric = numeric;
    router_.edit_token_ = request.token;
    router_.edit_action_ = Action();
    router_.edit_action_.screen = router_.model_.screen;
    router_.edit_action_.opening = router_.model_.opening;
    router_.edit_handler_ = std::move(accepted);
    router_.surface_.RequestText(std::move(request), event.id);
}

void Bindings::Read(const std::string &title, const std::string &text)
{
    router_.reading_title_ = title;
    router_.reading_text_ = text;
    router_.rebuild_ = true;
}

void Bindings::Toast(const std::string &text)
{
    router_.surface_.Toast(text);
}

Json Bindings::Notice(Json notice)
{
    const std::string tone = notice.value("tone", std::string("info"));
    std::string text = notice.value("text", std::string());
    if (notice.contains("lead") && notice["lead"].is_string())
        text = notice["lead"].get<std::string>() + " " + text;
    Shown(tone, text);
    return notice;
}

void Bindings::Shown(const std::string &tone, const std::string &text)
{
    const std::string key = router_.using_ + "\n" + tone + "\n" + text;
    if (!text.empty() && router_.notices_building_.insert(key).second && !router_.notices_shown_.count(key))
        router_.surface_.Shown("notice on screen " + router_.using_, tone, text);
}

void Bindings::Log(const std::string &text)
{
    router_.surface_.Log("screen " + router_.using_ + ": " + text);
}

void Bindings::Error(const std::string &text)
{
    router_.surface_.Error("screen " + router_.using_ + ": " + text);
}

void Bindings::ErrorOnce(const std::string &key, const std::string &text)
{
    if (router_.logged_.insert("error:" + router_.using_ + ":" + key).second)
        Error(text);
}

std::optional<SubjectRequest> Bindings::TakeSubject(const std::string &screen)
{
    return router_.hud_ ? router_.hud_->TakeSubject(screen) : std::nullopt;
}

void Bindings::Rebuild()
{
    router_.rebuild_ = true;
}

void Bindings::Await()
{
    router_.awaiting_ = true;
}

void Bindings::RebuildAfter(double seconds)
{
    const double when = router_.surface_.Now() + std::max(0.0, seconds);
    if (router_.rebuild_at_ == 0.0 || when < router_.rebuild_at_)
        router_.rebuild_at_ = when;
}

std::string Bindings::Text(const std::string &key) const
{
    return router_.surface_.Text(key);
}

bool Bindings::Pending(const std::string &id, const std::string &value) const
{
    for (const Router::PendingMark &mark : router_.pending_)
        if (mark.id == id && mark.value == value)
            return true;
    return false;
}

double Bindings::Now() const
{
    return router_.surface_.Now();
}

uint64_t Bindings::RequestFloor() const
{
    return router_.next_request_ + 1;
}

Bindings::Answers Bindings::AnsweredSince(uint64_t floor) const
{
    if (!router_.waiting_.empty())
        return Answers::Waiting; // something of it has not gone out yet
    if (router_.outstanding_.lower_bound(floor) != router_.outstanding_.end())
        return Answers::Waiting;
    bool any = false;
    for (auto it = router_.answers_.lower_bound(floor); it != router_.answers_.end(); ++it)
    {
        if (!it->second.applied)
            return Answers::Refused;
        if (!it->second.model_after)
            return Answers::Waiting;
        any = true;
    }
    return any ? Answers::Applied : Answers::Waiting;
}

// --- Router -----------------------------------------------------------------------------

namespace
{

// Runs `call`; what it threw, or nothing.
template <class Call> std::optional<std::string> Caught(Call &&call)
{
    try
    {
        call();
        return std::nullopt;
    }
    catch (const std::exception &error)
    {
        return std::string(error.what());
    }
    catch (...)
    {
        return std::string("an exception that is not a std::exception");
    }
}

} // namespace

template <class Call> bool Router::Guarded(const std::string &where, Call &&call)
{
    const std::optional<std::string> what = Caught(std::forward<Call>(call));
    if (what)
        Failed(where, *what);
    return !what;
}

bool Router::Consume(const std::string &channel, const char *what, const std::function<void()> &call)
{
    const std::optional<std::string> error = Caught(call);
    if (error)
        surface_.Error("state channel " + channel + ": its consumer threw in " + what + ": " + *error + "; the state is dropped");
    return !error;
}

void Router::Failed(const std::string &where, const std::string &what)
{
    const std::string failed = using_.empty() ? model_.screen : using_;
    CloseOnDefect("screen " + failed + " (interface " + (model_.screen.empty() ? std::string("-") : model_.screen) + " #" +
                      std::to_string(model_.opening) + " revision " + std::to_string(model_.revision) + ") threw in " + where + ": " + what +
                      "; the interface is closed (the engine's back)",
                  false);
}

void Router::CloseOnDefect(const std::string &line, bool logged)
{
    if (!logged)
        surface_.Error(line);
    // Nothing of the failed build stays: its bindings may point into state the throw left half made.
    bindings_.handlers_.clear();
    bindings_.predictions_.clear();
    modals_.clear();
    screen_.reset();
    using_.clear();
    waiting_.clear();
    pending_.clear();
    predicting_.clear();
    edit_token_.clear();
    edit_handler_ = nullptr;
    extras_open_ = false;
    reading_title_.clear();
    reading_text_.clear();
    current_ = nullptr;
    rebuild_ = false;
    rebuild_at_ = 0.0;
    settle_until_ = 0.0;
    Action back;
    back.screen = model_.screen;
    back.opening = model_.opening;
    back.verb = "back";
    rejection_.clear();
    model_ = InterfaceModel();
    surface_.Hide();
    surface_.Send(back);
}

Router::Router(Surface &surface, std::span<const ScreenEntry> screens, std::unique_ptr<Screen> (*generic)())
    : surface_(surface), screens_(screens), generic_(generic), bindings_(*this), consumers_(CreateStateConsumers())
{
}

Router::~Router() = default;

Action Router::Make(const Widget &widget, const std::string &verb, const std::string &value, const std::string &guard) const
{
    Action action;
    action.screen = model_.screen;
    action.opening = model_.opening;
    action.node = widget.node;
    action.index = widget.index;
    action.verb = verb;
    action.value = value;
    action.guard = guard;
    return action;
}

void Router::Emit(Action action)
{
    if (current_)
        sent_in_event_ = true;
    if (AnsweredAction(action))
    {
        action.request = ++next_request_;
        outstanding_[action.request] = {current_ ? current_->id : std::string(), current_ ? current_->value : std::string(), action.node,
                                        current_ != nullptr};
    }
    surface_.Send(action);
}

bool Router::MarkOpen(const std::string &id, const std::string &value) const
{
    for (const auto &[number, request] : outstanding_)
        if (request.marked && request.id == id && request.value == value)
            return true;
    return false;
}

void Router::Answer(uint64_t request, bool applied, const std::string &reason)
{
    const auto found = outstanding_.find(request);
    if (found == outstanding_.end())
    {
        // An action of an opening that is gone (its marks went with it).
        surface_.Log("answer to request " + std::to_string(request) + " of an earlier opening: " + (applied ? "applied" : "refused: " + reason));
        return;
    }
    const Request sent = found->second;
    outstanding_.erase(found);
    answers_[request] = {applied, false};
    while (answers_.size() > 256)
        answers_.erase(answers_.begin());
    if (!applied)
    {
        surface_.Log("screen " + using_ + ": the game refused " + sent.node + " (" + sent.id + "): " + reason);
        if (sent.marked)
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                          [&](const PendingMark &mark) { return mark.id == sent.id && mark.value == sent.value; }),
                           pending_.end());
        if (screen_)
            Build(); // the screen shows the game's state again at once (an amount draft goes back)
        return;
    }
    if (sent.marked && !MarkOpen(sent.id, sent.value))
        for (PendingMark &mark : pending_)
            if (mark.id == sent.id && mark.value == sent.value)
                mark.answered = true;
}

void Router::SetInterface(const InterfaceModel &model)
{
    const bool reopened = (!screen_ && settle_until_ == 0.0) || model.screen != model_.screen || model.opening != model_.opening;
    if (model.language != model_.language || !screen_)
        surface_.SetLanguage(model.language);
    const bool changed = model.revision != model_.revision;
    model_ = model;
    // The engine sends the model after every widget action it answered: the answers before it are
    // now shown, and the presses they answered stop looking busy.
    for (auto &[number, answer] : answers_)
        answer.model_after = true;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const PendingMark &mark) { return mark.answered; }), pending_.end());
    if (reopened)
    {
        opened_at_ = surface_.Now();
        screen_.reset();
        extras_open_ = false;
        reading_title_.clear();
        reading_text_.clear();
        pending_.clear();
        predicting_.clear();
        waiting_.clear();
        outstanding_.clear();
        edit_token_.clear();
        edit_handler_ = nullptr;
    }
    else if (changed)
        pending_.clear();
    Build();
}

void Router::CloseInterface()
{
    if (!screen_ && settle_until_ == 0.0)
        return;
    settle_until_ = 0.0;
    screen_.reset();
    using_.clear();
    rejection_.clear();
    bindings_.handlers_.clear();
    modals_.clear();
    pending_.clear();
    predicting_.clear();
    waiting_.clear();
    outstanding_.clear();
    edit_token_.clear();
    edit_handler_ = nullptr;
    model_ = InterfaceModel();
    surface_.Hide();
}

void Router::CloseGameInterface(const std::string &screen)
{
    if (!model_.screen.empty() && model_.screen != screen)
    {
        surface_.Log("interface " + (screen.empty() ? std::string("(not modelled yet)") : screen) + " closed under the " + model_.screen +
                     " screen: that screen stays");
        return;
    }
    CloseInterface();
    for (const auto &consumer : consumers_)
        Consume(consumer->Channel(), "Resume", [&] { consumer->Resume(*this); });
}

void Router::SetState(const std::string &channel, const Json &state)
{
    bool consumed = false;
    for (const auto &consumer : consumers_)
        if (channel == consumer->Channel())
        {
            // A consumer that throws: its state is dropped and reported (a screen it builds has
            // the screen boundary of Build).
            Consume(channel, "OnState", [&] { consumer->OnState(state, *this); });
            consumed = true;
        }
    if (!consumed)
        surface_.Error("state channel " + channel + " has no consumer");
}

void Router::RebuildActive()
{
    if (screen_)
        Build();
}

void Router::ReportError(const std::string &line)
{
    surface_.Error(line);
}

void Router::Alert(const std::string &key, const std::string &subject)
{
    surface_.Alert(subject.empty() ? surface_.Text(key) : surface_.Text(key) + ": " + subject);
}

void Router::Build()
{
    Guarded("Build", [this] { BuildView(); });
}

void Router::BuildView()
{
    notices_shown_ = std::move(notices_building_);
    notices_building_.clear();
    rebuild_ = false;
    awaiting_ = false;
    // A deadline still ahead stays (Bindings::RebuildAfter from an event handler: the build that
    // follows the event must not drop it); one that came is spent.
    if (rebuild_at_ > 0.0 && rebuild_at_ <= surface_.Now())
        rebuild_at_ = 0.0;
    bindings_.handlers_.clear();
    bindings_.predictions_.clear();
    modals_.clear();
    // The ported screen registered for the id draws it when the model passes its admission.
    // If its REQUIRED nodes are missing or mistyped once the first model settled, the screen
    // cannot draw the interface: that is a defect, reported, and the interface closes like
    // one whose screen threw (CloseOnDefect). Only an interface with no ported screen at all
    // is the generic layout's.
    const ScreenEntry *ported = nullptr;
    for (const ScreenEntry &entry : screens_)
        if (model_.screen == entry.id)
            ported = &entry;
    // The model's distinct nodes, "a,b,c": the key of the log-once lines below, built only for them.
    const auto nodes = [this] {
        std::string list;
        std::unordered_set<std::string_view> seen;
        for (const Widget &widget : model_.widgets)
            if (seen.insert(widget.node).second)
                list += (list.empty() ? "" : ",") + widget.node;
        return list;
    };
    Admission admitted;
    if (ported)
    {
        if (using_ != ported->id || !screen_)
        {
            screen_ = ported->create();
            using_ = ported->id;
        }
        admitted = Admit(model_, screen_->Nodes());
        if (!admitted.accepted)
        {
            screen_.reset();
            if (surface_.Now() - opened_at_ < kSettle)
            {
                settle_until_ = opened_at_ + kSettle; // Tick builds again then
                using_.clear();
                surface_.Hide();
                return;
            }
            const std::string rejection = admitted.missing;
            const bool logged = !logged_.insert("rejected:" + model_.screen + ":" + nodes()).second;
            CloseOnDefect("screen " + model_.screen + " (interface #" + std::to_string(model_.opening) + " revision " +
                              std::to_string(model_.revision) + "): required nodes missing or mistyped: " + rejection +
                              "; the interface is closed (the engine's back)",
                          logged);
            rejection_ = rejection;
            return;
        }
    }
    settle_until_ = 0.0;
    rejection_.clear();
    if (!ported)
    {
        if (using_ != "generic" || !screen_)
        {
            screen_ = generic_();
            using_ = "generic";
        }
        admitted = Admit(model_, screen_->Nodes());
        if (const std::string list = nodes(); logged_.insert("generic:" + model_.screen + ":" + list).second)
            surface_.Log("screen " + model_.screen + ": no ported screen, generic layout; nodes=" + (list.empty() ? std::string("none") : list));
    }
    Json view = screen_->Build(admitted, bindings_);
    // The generic layout places every widget itself and hands over only its overflow rows.
    Json rows = view.contains("more") ? view["more"] : Json::array();
    view.erase("more");
    view["extras"] = ExtrasView(admitted, std::move(rows));
    Predict(view);
    surface_.Show(screen_->Component(), view);
}

bool Router::AnsweredFrom(uint64_t floor) const
{
    if (awaiting_ || !waiting_.empty() || outstanding_.lower_bound(floor) != outstanding_.end())
        return false;
    for (auto it = answers_.lower_bound(floor); it != answers_.end(); ++it)
        if (it->second.applied && !it->second.model_after)
            return false;
    return true;
}

namespace
{
// Where two views first differ, as a path ("rows/3/selected"), for the correction's log line.
std::string FirstDifference(const Json &predicted, const Json &game, const std::string &path)
{
    if (predicted.type() == game.type() && predicted.is_object())
    {
        for (const auto &[key, value] : predicted.items())
            if (!game.contains(key))
                return path + "/" + key + ": predicted " + value.dump() + ", the game has none";
            else if (value != game[key])
                return FirstDifference(value, game[key], path + "/" + key);
        for (const auto &[key, value] : game.items())
            if (!predicted.contains(key))
                return path + "/" + key + ": predicted none, the game has " + value.dump();
    }
    if (predicted.type() == game.type() && predicted.is_array() && predicted.size() == game.size())
        for (size_t i = 0; i < predicted.size(); ++i)
            if (predicted[i] != game[i])
                return FirstDifference(predicted[i], game[i], path + "/" + std::to_string(i));
    std::string was = predicted.dump(), is = game.dump();
    if (was.size() > 120)
        was = was.substr(0, 120) + "…";
    if (is.size() > 120)
        is = is.substr(0, 120) + "…";
    return path + ": predicted " + was + ", the game has " + is;
}
} // namespace

void Router::Predict(Json &view)
{
    for (auto it = predicting_.begin(); it != predicting_.end();)
    {
        Json predicted = view;
        try
        {
            it->predict(it->event, predicted);
        }
        catch (const std::exception &error)
        {
            surface_.Error("screen " + using_ + ": the prediction of " + it->event.id + " " + it->event.verb + " threw (" + error.what() +
                           "); the game's state is shown");
            it = predicting_.erase(it);
            continue;
        }
        if (!AnsweredFrom(it->floor))
        {
            view = std::move(predicted); // the game is still answering: its steps do not change what was predicted
            ++it;
            continue;
        }
        // The game has answered: its state is shown, and it must be what was drawn at the tap.
        if (predicted != view)
            surface_.Error("UI prediction corrected: screen " + using_ + " " + it->event.id + " " + it->event.verb + " " + it->event.value +
                           ": the game answered otherwise after " + std::to_string(int((surface_.Now() - it->since) * 1000.0)) + " ms; " +
                           FirstDifference(predicted, view, ""));
        it = predicting_.erase(it);
    }
}

Json Router::ExtrasView(const Admission &admitted, Json rows)
{
    std::string note, nodes;
    std::vector<const Widget *> controls;
    const auto flush = [&] {
        if (!controls.empty())
            ControlRows(bindings_, controls, "extra-c" + std::to_string(rows.size()) + "-", std::vector<std::string>(), rows);
        controls.clear();
    };
    const bool generic = screen_->Nodes().empty();
    for (const Widget *widget : admitted.extras)
    {
        if (generic)
            break;
        nodes += (nodes.empty() ? "" : ",") + widget->node;
        if (widget->type == "text" || widget->type == "heading")
        {
            note += (note.empty() ? "" : "\n") + widget->label;
            continue;
        }
        if (widget->type == "check" || widget->type == "edit" || widget->type == "slider" || widget->type == "stepper")
        {
            controls.push_back(widget);
            continue;
        }
        flush();
        if (widget->type == "button" || widget->type == "choice" || widget->type == "image")
            rows.push_back(ActionRow(bindings_, *widget, "extra-a" + std::to_string(rows.size())));
    }
    flush();
    if (!nodes.empty() && logged_.insert("extras:" + model_.screen + ":" + nodes).second)
        surface_.Log("screen " + model_.screen + " extras=" + nodes);

    Json view = Json::object();
    view["rows"] = rows;
    view["open"] = extras_open_ && !rows.empty();
    view["actions"] = rows.empty() ? Json::array() : Json::array({{{"id", "extras-more"}, {"icon", "more"}, {"label", surface_.Text("common.other")}}});
    view["title"] = surface_.Text("common.other");
    view["note"] = note;
    view["reading"] = !reading_text_.empty();
    view["readingTitle"] = reading_title_;
    view["readingText"] = reading_text_;
    bindings_.On("extras-more", "", [this](const Event &) {
        extras_open_ = !extras_open_;
        rebuild_ = true;
    });
    // Both are drawn over the screen: declared after its own modals, the reading sheet on top.
    if (view["open"] == true)
        bindings_.Modal("extras", [this](const Event &) {
            extras_open_ = false;
            rebuild_ = true;
        });
    if (!reading_text_.empty())
    {
        const auto close = [this](const Event &) {
            reading_text_.clear();
            rebuild_ = true;
        };
        bindings_.Modal("extras-reading", close);
        bindings_.On("extras-reading-close", "", close);
    }
    bindings_.On("extras-note", "", [this, note](const Event &) {
        reading_title_.clear();
        reading_text_ = note;
        rebuild_ = true;
    });
    return view;
}

// A disabled control's tap reports "refused" (it shook already; a screen may bind it to point at what is missing).
// Only a handler bound to that verb takes it: a handler of every verb of the element is its action, which a
// disabled control must not run; unbound, the shake was the whole answer.
bool Router::Maps(const std::string &id, const std::string &verb) const
{
    return screen_ && (bindings_.handlers_.count(id + '\x1f' + verb) || (verb != "refused" && bindings_.handlers_.count(id + '\x1f')));
}

bool Router::OnEvent(const Event &event)
{
    if (!screen_)
        return false;
    auto found = bindings_.handlers_.find(event.id + '\x1f' + event.verb);
    if (found == bindings_.handlers_.end() && event.verb == "refused")
        return true; // the control's shake answered it (Maps)
    if (found == bindings_.handlers_.end())
        found = bindings_.handlers_.find(event.id + '\x1f');
    if (found == bindings_.handlers_.end())
    {
        surface_.Error("screen " + using_ + ": no mapping for event " + event.id + " " + event.verb + " " + event.value);
        return false;
    }
    const Bindings::Handler handler = found->second; // a rebuild replaces the table
    Bindings::Prediction predict;
    if (const auto predicted = bindings_.predictions_.find(event.id + '\x1f' + event.verb); predicted != bindings_.predictions_.end())
        predict = predicted->second;
    const uint64_t floor = next_request_ + 1;
    current_ = &event;
    sent_in_event_ = false;
    if (!Guarded("the handler of " + event.id + " " + event.verb, [&] { handler(event); }))
        return true; // Failed closed the interface
    current_ = nullptr;
    if (sent_in_event_)
    {
        // An action row of the «Прочее» sheet closes it, as a menu does.
        if (event.id.rfind("extra-a", 0) == 0)
            extras_open_ = false;
        const bool answers = MarkOpen(event.id, event.value);
        pending_.push_back({event.id, event.value, model_.revision, surface_.Now() + (answers ? kAnswerSeconds : kPendingSeconds), answers});
        rebuild_ = true;
    }
    // What the tap leads to is drawn now (the build below); the game's answer to what the handler sent ends it. A
    // handler that asked the game nothing predicted nothing: the screen's own state is the result already.
    if (predict && screen_ && (next_request_ >= floor || !waiting_.empty()))
    {
        predicting_.push_back({event, std::move(predict), floor, surface_.Now()});
        ++predicted_count_;
        rebuild_ = true;
    }
    if (rebuild_ && screen_)
        Build();
    return true;
}

void Router::Back()
{
    Action engine_back;
    engine_back.screen = model_.screen;
    engine_back.opening = model_.opening;
    engine_back.verb = "back";
    if (!screen_)
    {
        surface_.Send(engine_back);
        return;
    }
    if (!modals_.empty())
    {
        OnEvent({modals_.back().first, "back", ""});
        return;
    }
    if (screen_->OwnsBack())
    {
        if (!Guarded("Back", [this] { screen_->Back(bindings_); }))
            return;
        if (rebuild_ && screen_)
            Build();
        return;
    }
    if (bindings_.handlers_.count(std::string("header-back") + '\x1f'))
    {
        OnEvent({"header-back", "click", ""});
        return;
    }
    surface_.Send(engine_back);
}

void Router::SubmitText(const std::string &token, const std::string &text, bool accepted)
{
    if (token.empty() || token != edit_token_)
        return;
    edit_token_.clear();
    auto handler = std::move(edit_handler_);
    edit_handler_ = nullptr;
    if (!accepted || !screen_ || edit_action_.screen != model_.screen || edit_action_.opening != model_.opening)
        return;
    if (handler)
    {
        if (!Guarded("the text editor's answer", [&] { handler(text); }))
            return;
        if (rebuild_ && screen_)
            Build();
        return;
    }
    Action action = edit_action_;
    action.value = text;
    Emit(std::move(action));
}

bool Router::Answering() const
{
    if ((screen_ && awaiting_) || !outstanding_.empty() || !waiting_.empty())
        return true;
    return std::any_of(answers_.begin(), answers_.end(), [](const auto &entry) { return entry.second.applied && !entry.second.model_after; });
}

bool Router::WantsBack() const
{
    return screen_ && (!modals_.empty() || screen_->OwnsBack());
}

std::vector<std::pair<std::string, bool>> Router::Modals() const
{
    std::vector<std::pair<std::string, bool>> out;
    for (const auto &[id, cancel] : modals_)
        out.emplace_back(id, cancel != nullptr);
    return out;
}


double Router::Tick()
{
    const double now = surface_.Now();
    if (settle_until_ > 0.0)
    {
        if (now < settle_until_)
            return settle_until_ - now;
        Build();
    }
    if (!screen_)
        return kIdle;
    if (rebuild_at_ > 0.0 && now >= rebuild_at_)
        Build();
    double next = rebuild_at_ > 0.0 ? rebuild_at_ - now : kIdle;
    // In the order they were asked for: an edit goes out before the press that confirms it.
    for (size_t i = 0; i < waiting_.size();)
    {
        if (now >= waiting_[i].due)
        {
            last_flush_ = now;
            if (waiting_[i].lead)
                Emit(*waiting_[i].lead);
            Emit(waiting_[i].action);
            waiting_.erase(waiting_.begin() + static_cast<long>(i));
        }
        else
        {
            next = std::min(next, waiting_[i].due - now);
            ++i;
        }
    }
    const size_t before = pending_.size();
    for (const PendingMark &mark : pending_)
        if (now >= mark.until && mark.answers && MarkOpen(mark.id, mark.value))
        {
            surface_.Error("screen " + using_ + ": the game did not answer " + mark.id + " in " + std::to_string(int(kAnswerSeconds)) + " s");
            for (auto it = outstanding_.begin(); it != outstanding_.end();)
                it = it->second.marked && it->second.id == mark.id && it->second.value == mark.value ? outstanding_.erase(it) : std::next(it);
        }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const PendingMark &mark) { return now >= mark.until; }),
                   pending_.end());
    for (const PendingMark &mark : pending_)
        next = std::min(next, mark.until - now);
    if (pending_.size() != before)
        Build();
    return next;
}

} // namespace storm::ui::screens
