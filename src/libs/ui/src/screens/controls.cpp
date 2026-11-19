#include "controls.h"
#include "hero.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>

namespace storm::ui::screens
{

namespace
{

// Lower case for ASCII, Cyrillic and the Latin-1 letters of de/es/fr/it, enough to compare button captions.
std::string Lower(std::string_view text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 'A' && c <= 'Z')
            out += static_cast<char>(c - 'A' + 'a');
        else if (c == 0xD0 && i + 1 < text.size())
        {
            const auto d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x90 && d <= 0x9F) // А–П → а–п
                out += {static_cast<char>(0xD0), static_cast<char>(d + 0x20)};
            else if (d >= 0xA0 && d <= 0xAF) // Р–Я → р–я
                out += {static_cast<char>(0xD1), static_cast<char>(d - 0x20)};
            else
                out += {static_cast<char>(c), static_cast<char>(d)};
            ++i;
        }
        else if (c == 0xC3 && i + 1 < text.size())
        {
            const auto d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x80 && d <= 0x9E && d != 0x97) // À–Þ → à–þ (Latin-1 letters: é, ü, ô …)
                out += {static_cast<char>(c), static_cast<char>(d + 0x20)};
            else
                out += {static_cast<char>(c), static_cast<char>(d)};
            ++i;
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

bool Starts(const std::string &text, std::string_view prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool Ends(const std::string &text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

template <size_t N> bool OneOf(const std::string &text, const char *const (&list)[N])
{
    return std::find(std::begin(list), std::end(list), text) != std::end(list);
}

} // namespace

std::string Upper(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

size_t Length(const std::string &text)
{
    size_t count = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++count;
    return count;
}

bool Short(const std::string &text, size_t chars)
{
    return !text.empty() && text.find('\n') == std::string::npos && Length(text) <= chars;
}

std::string SectionOf(const Widget &button)
{
    static const char *const pairs[][2] = {{"I_CHARACTER", "hero"}, {"I_SHIP", "ship"},       {"I_QUESTBOOK", "journal"},
                                           {"I_TRADEBOOK", "tradebook"}, {"I_NATIONS", "nations"}, {"I_ITEMS", "items"}};
    std::string node = Upper(button.node);
    if (Ends(node, "_2"))
        node.resize(node.size() - 2);
    for (const auto &pair : pairs)
        if (node == pair[0])
            return pair[1];
    return {};
}

// Button-role matching data, NOT UI text: the captions a button of the game's own screens may carry
// (lower case, compared whole) and the substrings that mark a destructive one, for every shipped
// language (ru, en, de, es, fr, it) plus the glyph "×". Add a language here when its string table is added.
const char *const kExitLabels[] = {
    "назад", "отмена", "выход", "уйти", "закрыть",                  // ru
    "cancel", "back", "exit", "close", "×",                         // en
    "zurück", "abbrechen", "beenden", "verlassen", "schließen",     // de
    "atrás", "cancelar", "salir", "cerrar", "volver",               // es
    "retour", "annuler", "quitter", "fermer", "sortir",             // fr
    "indietro", "annulla", "esci", "uscire", "chiudi"};             // it
const char *const kConfirmLabels[] = {
    "ок", "ok", "oк", "да", "готово", "принять", "применить", "подтвердить", "начать", "взять", "купить", "забрать", // ru (+en ok)
    "ja", "fertig", "annehmen", "anwenden", "bestätigen", "starten", "nehmen", "kaufen",                           // de
    "sí", "listo", "aceptar", "aplicar", "confirmar", "empezar", "comenzar", "tomar", "comprar", "recoger",       // es
    "oui", "terminé", "accepter", "appliquer", "confirmer", "commencer", "prendre", "acheter",                    // fr
    "sì", "fatto", "pronto", "accetta", "applica", "conferma", "inizia", "prendi", "compra"};                     // it
const char *const kDangerWords[] = {
    "delete", "удалить", "сжечь", "kill", "казнить",   // en, ru
    "löschen", "verbrennen", "töten", "hinrichten",    // de
    "eliminar", "borrar", "quemar", "matar",           // es
    "supprimer", "effacer", "brûler", "tuer",          // fr
    "elimina", "cancella", "brucia", "uccidi"};        // it

ButtonRole RoleOf(const Widget &button)
{
    const std::string node = Upper(button.node);
    const std::string label = Lower(button.label);
    if (!SectionOf(button).empty())
        return ButtonRole::Section;
    for (const char *prefix : {"EXIT", "CANCEL", "CLOSE", "BACK"})
        if (Starts(node, prefix))
            return ButtonRole::Exit;
    for (const char *suffix : {"_EXIT", "_CANCEL", "_CLOSE"})
        if (Ends(node, suffix))
            return ButtonRole::Exit;
    if (OneOf(label, kExitLabels))
        return ButtonRole::Exit;
    for (const char *word : kDangerWords)
        if (label.find(word) != std::string::npos || Lower(button.node).find(word) != std::string::npos)
            return ButtonRole::Danger;
    for (const char *prefix : {"OK", "ACCEPT", "YES", "CONFIRM", "DONE", "APPLY", "START"})
        if (Starts(node, prefix))
            return ButtonRole::Confirm;
    for (const char *suffix : {"_OK", "_ACCEPT", "_YES"})
        if (Ends(node, suffix))
            return ButtonRole::Confirm;
    if (OneOf(label, kConfirmLabels))
        return ButtonRole::Confirm;
    return ButtonRole::Other;
}

Json PictureOf(const ImageRef *image)
{
    if (image && image->kind == "portrait" && !image->image.empty())
        return Json{{"kind", "portrait"}, {"texture", "INTERFACES/PORTRAITS/128/face_" + image->image}};
    if (image && image->kind == "texture" && !image->image.empty())
        return Json{{"kind", "item"}, {"texture", image->image}};
    if (!image || image->kind != "atlas" || image->group.empty() || image->image.empty())
        return Json();
    return Json{{"kind", "item"}, {"texture", "atlas:" + image->group + "/" + image->image}};
}

Json TextureOf(const ImageRef *image)
{
    if (image && image->kind == "texture" && !image->image.empty())
    {
        char uv[96];
        std::snprintf(uv, sizeof uv, "%g,%g,%g,%g", image->uv[0], image->uv[1], image->uv[2], image->uv[3]);
        return Json{{"texture", image->image}, {"uv", uv}};
    }
    if (image && image->kind == "atlas" && !image->group.empty() && !image->image.empty())
        return Json{{"texture", "atlas:" + image->group + "/" + image->image}, {"uv", ""}};
    return Json();
}

// The ship skills of the captain's mini table (interface_utils.c SetSPECIALMiniTable), the ones that decide a
// captain: row tr5 names them only by picture (ICONS_SPEC "<skill> skill icon", no text), row tr6 holds the
// values, green where items or officers raise the skill and red where they lower it. {icon, k, v, tone}.
Json ShipSkills(const std::vector<const Widget *> &table, Bindings &bindings, const std::string &screen, bool &reported)
{
    static const char *const names[][2] = {{"Sailing", StringKey("skill.sailing")},     {"Accuracy", StringKey("skill.accuracy")},
                                           {"Cannons", StringKey("skill.cannons")},     {"Grappling", StringKey("skill.grappling")},
                                           {"Defence", StringKey("skill.defence")},     {"Repair", StringKey("skill.repair")},
                                           {"Commerce", StringKey("skill.commerce")}};
    const auto row = [&table](int index) -> const Widget * {
        for (const Widget *widget : table)
            if (widget->index == index)
                return widget;
        return nullptr;
    };
    const Widget *pictures = row(4);
    const Widget *values = row(5);
    Json skills = Json::array();
    if (!pictures || !values)
    {
        if (!table.empty() && !reported)
        {
            reported = true;
            bindings.Error(screen + ": the captain's skill table has no ship skill rows (tr5 pictures, tr6 values)");
        }
        return skills;
    }
    constexpr std::string_view suffix = " skill icon";
    for (const Cell &cell : pictures->cells)
    {
        const ImageRef *picture = cell.images.empty() ? nullptr : &cell.images.front();
        std::string id = picture ? picture->image : std::string();
        if (id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0)
            id.resize(id.size() - suffix.size());
        const char *key = nullptr;
        for (const auto &name : names)
            if (id == name[0])
                key = name[1];
        if (!key)
        {
            if (!reported)
            {
                reported = true;
                bindings.Error(screen + ": the captain's skill table names an unknown ship skill by its picture '" +
                               (picture ? picture->group + "/" + picture->image : std::string()) + "' (column " +
                               std::to_string(cell.column) + ")");
            }
            continue;
        }
        std::string tone;
        for (const Cell &value : values->cells)
            if (value.column == cell.column)
                tone = value.color_argb == 0xffc4ffc4u ? "ok" : value.color_argb == 0xffffc4c4u ? "danger" : "";
        skills.push_back(Json{{"icon", hero::SkillIcon(id)}, {"k", bindings.Text(key)}, {"v", values->CellText(cell.column)}, {"tone", tone}});
    }
    return skills;
}

namespace
{
// "a,b[,c,d]" of the engine's map context as numbers; empty when malformed.
Json Numbers(const Widget &widget, const char *key, size_t count)
{
    const auto found = widget.context.find(key);
    if (found == widget.context.end())
        return Json::array();
    Json out = Json::array();
    std::string_view text = found->second;
    while (!text.empty() && out.size() < count)
    {
        const size_t comma = text.find(',');
        const std::string part(text.substr(0, comma));
        char *end = nullptr;
        const double value = std::strtod(part.c_str(), &end);
        if (part.empty() || end != part.c_str() + part.size())
            return Json::array();
        out.push_back(value);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return out.size() == count && text.empty() ? out : Json::array();
}
double Context(const Widget &widget, const char *key)
{
    const Json value = Numbers(widget, key, 1);
    return value.empty() ? 0.0 : value[0].get<double>();
}
} // namespace

Json MapViewOf(const Widget &map, std::span<const Widget *const> markers)
{
    const Json picture = TextureOf(map.FirstImage());
    Json view{{"texture", picture.is_null() ? "" : picture["texture"]},
              {"uv", picture.is_null() ? "" : picture["uv"]},
              {"base", Numbers(map, "base", 2)},
              {"view", Numbers(map, "view", 4)}};
    Json pins = Json::array();
    for (const Widget *marker : markers)
    {
        const Json texture = TextureOf(marker->FirstImage());
        if (texture.is_null())
            continue;
        pins.push_back(Json{{"texture", texture["texture"]}, {"uv", texture["uv"]}, {"id", marker->row_id},
                            {"x", Context(*marker, "x")}, {"y", Context(*marker, "y")},
                            {"w", Context(*marker, "width")}, {"h", Context(*marker, "height")}});
    }
    view["markers"] = pins;
    return view;
}

std::string NationOf(const ImageRef *image)
{
    if (!image || image->kind != "atlas" || Upper(image->group) != "NATIONS")
        return {};
    static const char *const nations[][2] = {
        {"ENGLAND", "england"}, {"FRANCE", "france"}, {"SPAIN", "spain"}, {"HOLLAND", "holland"}, {"PIRATE", "pirates"}};
    const std::string name = Upper(image->image);
    for (const auto &[game, flag] : nations)
        if (name == game)
            return flag;
    return {};
}

std::string Fill(std::string text, std::initializer_list<std::string> args)
{
    int number = 1;
    for (const std::string &arg : args)
    {
        const std::string mark = "{" + std::to_string(number++) + "}";
        for (size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at + arg.size()))
            text.replace(at, mark.size(), arg);
    }
    return text;
}

// One decimal with the decimal mark of the language ("2,3").
std::string Decimal(const Bindings &bindings, double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    std::string text = buffer;
    const size_t dot = text.find('.');
    if (dot != std::string::npos)
        text.replace(dot, 1, bindings.Text("launcher.decimal_mark"));
    return text;
}

std::string Decimal(const std::string &text, const std::string &language)
{
    if (language == "english")
        return text;
    std::string out = text;
    for (size_t i = 1; i + 1 < out.size(); ++i)
        if (out[i] == '.' && out[i - 1] >= '0' && out[i - 1] <= '9' && out[i + 1] >= '0' && out[i + 1] <= '9')
            out[i] = ',';
    return out;
}

// A size in GB from a tenth of a GB, else in MB; `scale` (when given) picks the unit, so
// that "done of total" share one.
std::string Size(const Bindings &bindings, double bytes, double scale)
{
    constexpr double kMiB = 1048576.0, kGiB = 1073741824.0;
    if ((scale < 0 ? bytes : scale) >= kGiB / 10)
        return Fill(bindings.Text("launcher.gb"), {Decimal(bindings, bytes / kGiB)});
    return Fill(bindings.Text("launcher.mb"), {Decimal(bindings, bytes / kMiB)});
}


std::string OneLine(const std::string &text)
{
    std::string out;
    for (char c : text)
    {
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c != ' ' || (!out.empty() && out.back() != ' '))
            out += c;
    }
    const size_t first = out.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first, out.find_last_not_of(' ') - first + 1);
}

Json ButtonView(Bindings &bindings, const Widget &button, const std::string &id, const char *variant)
{
    bindings.Forward(id, button, "press");
    return Json{{"id", id},
                {"label", button.label},
                {"variant", variant},
                {"disabled", !button.Offers("press")},
                {"reason", button.unavailable_reason},
                {"busy", bindings.Pending(id)}};
}

void AmountDraft::Open(const std::string &key, int origin, int other)
{
    open_ = true;
    key_ = key;
    origin_ = origin;
    other_ = other;
    target_ = origin;
    sent_ = false;
}

void AmountDraft::Follow(const std::string &key, int origin, int other, Bindings &bindings)
{
    if (!open_ || key != key_ || origin != origin_ || other != other_)
        Open(key, origin, other); // a new control, or the game's figures changed (a move went through)
    else if (sent_)
    {
        switch (bindings.AnsweredSince(floor_))
        {
        case Bindings::Answers::Refused: // the engine refused an action of the deal (logged by the router)
        case Bindings::Answers::Applied: // all applied, and the model after them still has the old figures: the game said no
            Open(key, origin, other);
            break;
        case Bindings::Answers::Waiting:
            if (bindings.Now() - sent_at_ >= kNoAnswer)
            {
                bindings.Error("the deal on " + key + " got no answer from the game in 5 s");
                Open(key, origin, other);
            }
            else
                bindings.RebuildAfter(sent_at_ + kNoAnswer - bindings.Now());
            break;
        }
    }
}

void AmountDraft::Close()
{
    open_ = false;
    key_.clear();
    sent_ = false;
}

int AmountDraft::Value(int low, int high) const
{
    return !open_ ? origin_ : std::clamp(target_, low, std::max(low, high));
}

void AmountDraft::Bind(Bindings &bindings, const std::string &id, int low, int high, const std::string &more_reason, int grain)
{
    const auto set = [this, &bindings, low, high = std::max(low, high), more_reason, grain = std::max(1, grain)](const Event &event) {
        const int wanted = std::atoi(event.value.c_str());
        // Past the end the purse or the room allows: the reason, once per push (a drag reports many samples).
        if (wanted > high && !more_reason.empty() && bindings.Now() - reason_at_ > 1.5)
        {
            reason_at_ = bindings.Now();
            bindings.Toast(more_reason);
        }
        if (!sent_)
        {
            int target = std::clamp(wanted, low, high);
            if (grain > 1 && target != low && target != high)
            {
                // the nearest whole number of grains from the origin, the ends where it runs out
                const int grains = static_cast<int>(std::lround(static_cast<double>(target - origin_) / grain));
                target = std::clamp(origin_ + grains * grain, low, high);
            }
            target_ = target;
        }
        bindings.Rebuild();
    };
    bindings.On(id, "change", set);
    bindings.On(id, "commit", set);
}

Json AmountDraft::Buttons(Bindings &bindings, int low, int high, Labels labels, const std::string &sub, bool disabled, const std::string &reason,
                          std::function<void(int)> commit)
{
    const int value = Value(low, high);
    const int delta = value - origin_;
    if (!open_ || (!sent_ && delta == 0))
        return Json();
    const std::string label = bindings.Text(delta > 0 ? labels.more : labels.less) + " " + std::to_string(labels.absolute ? value : std::abs(delta));
    if (sent_)
    {
        // Sent, the game's answer not here yet: the same button waits, and a second press sends nothing (nor
        // what the screen's bar has in its place without a deal, such as a take-all).
        bindings.On("primary", "", [](const Event &) {});
        return Json{{"label", label}, {"sub", sub}, {"disabled", false}, {"reason", ""}, {"busy", true}};
    }
    bindings.On("primary", "", [this, &bindings, value, delta, disabled, commit](const Event &) {
        if (disabled || sent_)
            return;
        Sent(value, bindings);
        commit(delta);
        bindings.Rebuild();
    });
    return Json{{"label", label},
                {"sub", sub},
                {"disabled", disabled},
                {"reason", reason},
                {"busy", bindings.Pending("primary")}};
}

void AmountDraft::CancelAction(Bindings &bindings, Json &actions)
{
    if (!open_ || sent_ || target_ == origin_)
        return;
    actions.push_back(Json{{"id", "deal-cancel"}, {"label", bindings.Text("common.cancel")}, {"variant", "secondary"}, {"disabled", false}, {"reason", ""}, {"busy", false}});
    bindings.On("deal-cancel", "", [this, &bindings](const Event &) {
        target_ = origin_;
        bindings.Rebuild();
    });
}

void AmountDraft::Sent(int value, Bindings &bindings)
{
    target_ = value;
    sent_ = true;
    sent_at_ = bindings.Now();
    floor_ = bindings.RequestFloor();
}

bool AmountDraft::Drop()
{
    if (!open_ || sent_ || target_ == origin_)
        return false;
    target_ = origin_;
    return true;
}

void TapChoice(Bindings &bindings, const Widget &choice)
{
    const char *verb = choice.value == "1" && choice.Offers("activate") ? "activate"
                       : choice.Offers("select")                        ? "select"
                       : choice.Offers("open")                          ? "open"
                       : choice.Offers("activate")                      ? "activate"
                                                                        : nullptr;
    if (!verb)
        return;
    // Verbs the engine lists are bound to the row identity; the legacy plain select is not.
    const bool listed = std::find(choice.actions.begin(), choice.actions.end(), verb) != choice.actions.end();
    bindings.Send(choice, verb, listed ? choice.row_id : std::string());
}

Json ActionRow(Bindings &bindings, const Widget &widget, const std::string &id)
{
    const bool choice = widget.type == "choice";
    const bool usable = choice ? widget.Offers("select") || widget.Offers("activate") || widget.Offers("open") : widget.Offers("press");
    const Widget *target = &widget;
    if (choice)
        bindings.On(id, "", [&bindings, target](const Event &) { TapChoice(bindings, *target); });
    else
        bindings.Forward(id, widget, "press");
    std::string title = widget.label;
    std::replace(title.begin(), title.end(), '\t', ' ');
    return Json{{"kind", "action"},
                {"id", id},
                {"title", title},
                {"disabled", !usable},
                {"reason", widget.unavailable_reason},
                {"selected", choice && widget.value == "1"},
                {"busy", bindings.Pending(id)}};
}

void ControlRows(Bindings &bindings, std::span<const Widget *const> widgets, const std::string &id_prefix,
                 const std::vector<std::string> &labels, Json &rows)
{
    for (size_t i = 0; i < widgets.size(); ++i)
    {
        const Widget *widget = widgets[i];
        const std::string label = i < labels.size() ? labels[i] : std::string();
        const std::string id = id_prefix + std::to_string(i);
        if (widget->type == "check" && widget->exclusive)
        {
            // One radio group: the following options of the same node.
            size_t end = i;
            while (end < widgets.size() && widgets[end]->type == "check" && widgets[end]->exclusive &&
                   SameNode(widgets[end]->node, widget->node))
                ++end;
            const std::vector<const Widget *> group(widgets.begin() + static_cast<long>(i), widgets.begin() + static_cast<long>(end));
            const bool segment = group.size() <= 4 &&
                                 std::all_of(group.begin(), group.end(), [](const Widget *w) { return Short(w->label, 12); });
            if (segment)
            {
                Json options = Json::array();
                int selected = -1;
                bool usable = false;
                for (size_t k = 0; k < group.size(); ++k)
                {
                    options.push_back(group[k]->label);
                    if (group[k]->value == "1")
                        selected = static_cast<int>(k);
                    usable = usable || group[k]->Offers("toggle");
                }
                bindings.On(id, "change", [&bindings, group](const Event &event) {
                    const size_t chosen = static_cast<size_t>(std::atoi(event.value.c_str()));
                    if (chosen < group.size() && group[chosen]->Offers("toggle"))
                        bindings.Send(*group[chosen], "toggle", "1");
                });
                // The chosen option is drawn chosen in the tap's frame (the game's answer confirms it).
                bindings.Predict(id, "change", [id](const Event &event, Json &view) { Bindings::Patch(view, id, "selected", std::atoi(event.value.c_str())); });
                rows.push_back(Json{{"kind", "segment"},  {"id", id},          {"label", label},
                                    {"options", options}, {"selected", selected}, {"disabled", !usable},
                                    {"reason", widget->unavailable_reason}});
            }
            else
                for (size_t k = 0; k < group.size(); ++k)
                {
                    const std::string row_id = id_prefix + std::to_string(i + k);
                    const Widget *option = group[k];
                    bindings.On(row_id, "change", [&bindings, option](const Event &) { bindings.Send(*option, "toggle", "1"); });
                    rows.push_back(Json{{"kind", "radio"},
                                        {"id", row_id},
                                        {"label", option->label},
                                        {"on", option->value == "1"},
                                        {"disabled", !option->Offers("toggle")},
                                        {"reason", option->unavailable_reason}});
                }
            i = end - 1;
        }
        else if (widget->type == "check")
        {
            bindings.On(id, "change", [&bindings, widget](const Event &event) { bindings.Send(*widget, "toggle", event.value == "1" ? "1" : "0"); });
            // The switch is drawn where the finger put it in the tap's frame (the game's answer confirms it).
            bindings.Predict(id, "change", [id](const Event &event, Json &view) { Bindings::Patch(view, id, "on", event.value == "1"); });
            rows.push_back(Json{{"kind", "toggle"},
                                {"id", id},
                                {"label", widget->label},
                                {"on", widget->value == "1"},
                                {"disabled", !widget->Offers("toggle")},
                                {"reason", widget->unavailable_reason}});
        }
        else if (widget->type == "slider")
        {
            const int span = std::max(1, widget->max - widget->min);
            const int position = std::atoi(widget->value.c_str());
            const int percent = static_cast<int>(std::lround(100.0 * (position - widget->min) / span));
            bindings.Ignore(id, "change"); // drag progress: the position goes out on release
            bindings.On(id, "commit", [&bindings, widget, span](const Event &event) {
                const int step = std::max(1, widget->step);
                const double wanted = widget->min + std::atof(event.value.c_str()) * span / 100.0;
                const int snapped = widget->min + static_cast<int>(std::lround((wanted - widget->min) / step)) * step;
                bindings.Send(*widget, "set", std::to_string(std::clamp(snapped, widget->min, widget->max)));
            });
            rows.push_back(Json{{"kind", "slider"},
                                {"id", id},
                                {"label", label.empty() ? widget->label : label},
                                {"value", std::clamp(percent, 0, 100)},
                                {"disabled", !widget->Offers("set")},
                                {"reason", widget->unavailable_reason}});
        }
        else if (widget->type == "stepper")
        {
            bindings.On(id + "-left", "", [&bindings, widget](const Event &) { bindings.Send(*widget, "left"); });
            bindings.On(id + "-right", "", [&bindings, widget](const Event &) { bindings.Send(*widget, "right"); });
            rows.push_back(Json{{"kind", "stepper"},
                                {"id", id},
                                {"label", label},
                                {"text", widget->value.empty() ? widget->label : widget->value},
                                {"left", widget->Offers("left")},
                                {"right", widget->Offers("right")}});
        }
        else if (widget->type == "edit")
        {
            const std::string title = label;
            bindings.On(id, "focus", [&bindings, widget, title](const Event &event) {
                if (widget->Offers("edit"))
                    bindings.EditText(event, *widget, title, false);
            });
            rows.push_back(Json{{"kind", "field"}, {"id", id}, {"label", label}, {"value", widget->value}});
        }
    }
}

Json SubjectSwitch::View(Bindings &bindings, const std::vector<Choice> &choices, size_t current, const std::string &title,
                        std::function<void(size_t)> choose)
{
    const size_t count = choices.size();
    if (current >= count)
    {
        open_ = false;
        return nullptr;
    }
    const bool list = count > 1;
    if (!list)
        open_ = false;
    bindings.On("header", "subject-prev", [choose, current](const Event &) {
        if (current > 0)
            choose(current - 1);
    });
    bindings.On("header", "subject-next", [choose, current, count](const Event &) {
        if (current + 1 < count)
            choose(current + 1);
    });
    if (list)
        bindings.On("header", "subject-list", [this, &bindings](const Event &) {
            open_ = true;
            bindings.Rebuild();
        });
    else
        bindings.Ignore("header", "subject-list");
    Json view{{"label", choices[current].label},
              // «1 из 1» says nothing and takes the line a two-line name needs.
              {"sub", count > 1 ? std::to_string(current + 1) + " " + bindings.Text("common.of") + " " + std::to_string(count) : std::string()},
              {"prev", current > 0},
              {"next", current + 1 < count},
              {"list", list}};
    if (!open_)
        return view;
    Json rows = Json::array();
    for (size_t i = 0; i < count; ++i)
    {
        rows.push_back(Json{{"label", choices[i].label},
                            {"meta", choices[i].meta},
                            {"image", choices[i].image},
                            {"icon", choices[i].icon},
                            {"selected", i == current}});
        bindings.On("header-subject-" + std::to_string(i), "", [this, &bindings, choose, current, i](const Event &) {
            Close(bindings);
            if (i != current)
                choose(i);
        });
    }
    const auto close = [this, &bindings](const Event &) { Close(bindings); };
    bindings.Modal("header-subjects", close);
    bindings.On("header-subjects-close", "", close);
    view["open"] = true;
    view["title"] = title;
    view["rows"] = rows;
    return view;
}

void SubjectSwitch::Close(Bindings &bindings)
{
    open_ = false;
    bindings.Rebuild();
}

} // namespace storm::ui::screens
