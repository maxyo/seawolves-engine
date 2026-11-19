#include "action_trace.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace storm::ui
{

namespace
{

constexpr uint64_t kOffset = 1469598103934665603ull;
constexpr uint64_t kPrime = 1099511628211ull;

uint64_t Mix(uint64_t hash, std::string_view text)
{
    for (const char c : text)
        hash = (hash ^ static_cast<unsigned char>(c)) * kPrime;
    return (hash ^ 0xffu) * kPrime; // a separator: ("ab", "c") is not ("a", "bc")
}

uint64_t Mix(uint64_t hash, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        hash = (hash ^ ((value >> (i * 8)) & 0xffu)) * kPrime;
    return hash;
}

// Interaction feedback: a press part held down or shaken (behaviors.cpp), a modal that refuses a tap beside it
// (ComponentElement::Refuse), a press that was taken and waits for the game (the busy mark of a press part, the
// pack's .busy and its .spinner): what the finger does, not what the screen says.
bool Feedback(const std::string &name)
{
    return name == "pressed" || name == "shake" || name == "refused" || name == "busy";
}

std::string Clip(std::string text, size_t most)
{
    if (text.size() <= most)
        return text;
    size_t cut = most;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut; // not inside a UTF-8 sequence
    return text.substr(0, cut) + "…";
}

struct Walk
{
    std::vector<FrameContent::Part> &out;
    Rml::Rectanglef viewport;

    // `busy`: inside a press part that shows its busy mark (the mark moves the part's label aside: how the things
    // in it look meanwhile is feedback, as the mark is; Part::feedback).
    void Element(Rml::Element *element, uint64_t parent, int index, Rml::Rectanglef clip, float opacity, bool busy = false)
    {
        if (!element->IsVisible())
            return;
        const Rml::ComputedValues &values = element->GetComputedValues();
        opacity *= values.opacity();
        if (opacity < 0.01f)
            return;
        const std::string &tag = element->GetTagName();
        const std::string &id = element->GetId();
        uint64_t key = Mix(Mix(Mix(parent, tag), id), static_cast<uint64_t>(index));
        uint64_t content = Mix(kOffset, tag);
        std::string text;
        if (auto *run = dynamic_cast<Rml::ElementText *>(element))
        {
            text = run->GetText();
            content = Mix(content, text);
        }
        std::vector<std::string> classes;
        Rml::StringList names;
        Rml::StringUtilities::ExpandString(names, element->GetClassNames(), ' ');
        for (const std::string &name : names)
            if (!name.empty() && !Feedback(name) && name != "scroll-below") // scroll-below follows the scroll position, as data-at
                classes.push_back(name);
        std::sort(classes.begin(), classes.end());
        for (const std::string &name : classes)
            content = Mix(content, name);
        // Attributes in any order (an unordered map): each one hashed alone, the hashes combined.
        uint64_t attributes = 0;
        for (const auto &[name, value] : element->GetAttributes())
            // Not content: data-at, where the runtime placed a list's scroll fade (draw_elements.cpp
            // ScrollEdgesChanged): it moves with the scroll.
            if (name != "class" && name != "id" && name != "data-at" && name != "busy")
                attributes += Mix(Mix(kOffset, name), value.Get<Rml::String>());
        content = Mix(content, attributes);
        // Where it sits in its parent and how big it is, to 3 px: a knob or a panel that jumps for a frame is flicker
        // as much as a wrong text. (Relative offsets: a list that scrolls moves its rows, not their place in it.)
        if (text.empty())
        {
            const Rml::Vector2f at = element->GetRelativeOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            for (const float v : {at.x, at.y, size.x, size.y})
                content = Mix(content, static_cast<uint64_t>(static_cast<int64_t>(std::lround(v / 3.0f))));
        }

        Rml::Rectanglef box;
        Rml::Element *boxed = text.empty() ? element : element->GetParentNode(); // a text run: its parent's box
        if (boxed)
        {
            const Rml::Vector2f size = boxed->GetBox().GetSize(Rml::BoxArea::Border);
            box = Rml::Rectanglef::FromPositionSize(boxed->GetAbsoluteOffset(Rml::BoxArea::Border), size);
        }
        box = box.Intersect(clip);
        if (!box.Valid())
            box = Rml::Rectanglef::FromPositionSize(box.p0, {0.0f, 0.0f});
        std::string label = tag + (id.empty() ? "" : "#" + id);
        for (const std::string &name : classes)
            label += "." + name;
        if (!text.empty())
            label += " '" + Clip(text, 40) + "'";
        out.push_back({key, content, box, std::move(label), busy});

        // What a box clips (overflow other than visible), its children are seen only inside it.
        if (values.overflow_x() != Rml::Style::Overflow::Visible || values.overflow_y() != Rml::Style::Overflow::Visible)
        {
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Padding);
            const Rml::Rectanglef inner = Rml::Rectanglef::FromPositionSize(element->GetAbsoluteOffset(Rml::BoxArea::Padding), size);
            clip = clip.Intersect(inner);
            if (!clip.Valid())
                return;
        }
        const int count = element->GetNumChildren();
        busy = busy || element->IsClassSet("busy");
        // The busy mark itself is no part, and takes no place among its siblings (their keys stay what they were).
        for (int i = 0, place = 0; i < count; ++i)
            if (Rml::Element *child = element->GetChild(i); !child->IsClassSet("spinner"))
                Element(child, key, place++, clip, opacity, busy);
    }
};

// The share of the screen the changed parts cover, on a coarse grid (nested boxes count once).
class Coverage
{
  public:
    Coverage(float width, float height) : width_(std::max(1.0f, width)), height_(std::max(1.0f, height)), cells_(kColumns * kRows, false)
    {
    }
    void Add(const Rml::Rectanglef &box)
    {
        if (!box.Valid() || box.Width() <= 0.0f || box.Height() <= 0.0f)
            return;
        const auto column = [&](float x) { return std::clamp(int(x / width_ * kColumns), 0, kColumns - 1); };
        const auto row = [&](float y) { return std::clamp(int(y / height_ * kRows), 0, kRows - 1); };
        for (int y = row(box.Top()); y <= row(box.Bottom() - 0.5f); ++y)
            for (int x = column(box.Left()); x <= column(box.Right() - 0.5f); ++x)
                cells_[size_t(y * kColumns + x)] = true;
    }
    double Share() const
    {
        return double(std::count(cells_.begin(), cells_.end(), true)) / double(cells_.size());
    }

  private:
    static constexpr int kColumns = 48, kRows = 24;
    float width_, height_;
    std::vector<bool> cells_;
};

} // namespace

FrameContent CaptureContent(std::span<Rml::ElementDocument *const> documents, float width, float height, std::string shown, double time)
{
    FrameContent frame;
    frame.shown = std::move(shown);
    frame.time = time;
    const Rml::Rectanglef viewport = Rml::Rectanglef::FromPositionSize({0.0f, 0.0f}, {width, height});
    Walk walk{frame.parts, viewport};
    for (size_t i = 0; i < documents.size(); ++i)
        if (documents[i] && documents[i]->IsVisible())
            walk.Element(documents[i], Mix(kOffset, uint64_t(i)), 0, viewport, 1.0f);
    std::stable_sort(frame.parts.begin(), frame.parts.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
    uint64_t hash = kOffset;
    for (const FrameContent::Part &part : frame.parts)
        hash = Mix(Mix(hash, part.key), part.content);
    frame.hash = hash;
    return frame;
}

std::optional<ActionTrace> ActionTracer::Begin(std::string what, FrameContent start, float width, float height)
{
    std::optional<ActionTrace> previous = active_ ? Finish(true) : std::nullopt;
    active_ = true;
    predicted_ = false;
    what_ = std::move(what);
    event_.clear();
    width_ = width;
    height_ = height;
    begun_ = changed_ = start.time;
    frames_.clear();
    frames_.push_back(std::move(start));
    return previous;
}

void ActionTracer::Named(const std::string &event)
{
    if (active_ && event_.empty())
        event_ = event;
}

void ActionTracer::Presented(FrameContent frame)
{
    if (!active_)
        return;
    if (frame.hash != frames_.back().hash)
        changed_ = frame.time;
    frames_.push_back(std::move(frame));
}

std::optional<ActionTrace> ActionTracer::Poll(double now, bool waiting)
{
    if (!active_)
        return std::nullopt;
    if (waiting && now - begun_ < kMaxSeconds)
        changed_ = std::max(changed_, now); // it settles kSettleSeconds after the waiting ends at the earliest
    if (now - changed_ < kSettleSeconds && now - begun_ < kMaxSeconds)
        return std::nullopt;
    std::optional<ActionTrace> trace = Finish(false);
    if (trace && now - begun_ >= kMaxSeconds && now - changed_ < kSettleSeconds)
        trace->unsettled = true;
    return trace;
}

std::optional<ActionTrace> ActionTracer::Finish(bool interrupted)
{
    if (!active_)
        return std::nullopt;
    active_ = false;
    ActionTrace trace = Analyse(event_.empty() ? what_ : event_, frames_, width_, height_);
    trace.interrupted = interrupted;
    trace.predicted = predicted_;
    trace.late = predicted_ && trace.feedback_frame > 1;
    frames_.clear();
    return trace;
}

ActionTrace ActionTracer::Analyse(const std::string &action, const std::vector<FrameContent> &frames, float width, float height)
{
    ActionTrace trace;
    trace.action = action;
    if (frames.empty())
        return trace;
    trace.before = frames.front().shown;
    // Frames that repeat the one before them (a frame drawn again for a texture) are one frame on the screen.
    std::vector<const FrameContent *> seen;
    for (const FrameContent &frame : frames)
        if (seen.empty() || frame.hash != seen.back()->hash)
            seen.push_back(&frame);
    trace.frames = int(frames.size()) - 1;
    const FrameContent &start = *seen.front(), &end = *seen.back();
    trace.after = frames.back().shown;
    // The settled state is shown from the first frame that has its content.
    trace.settle_ms = (end.time - frames.front().time) * 1000.0;
    for (size_t f = 1; f < frames.size() && !trace.feedback_frame; ++f)
        if (frames[f].hash != frames.front().hash)
        {
            trace.feedback_frame = int(f);
            trace.feedback_ms = (frames[f].time - frames.front().time) * 1000.0;
        }
    if (seen.size() < 3)
        return trace;

    // Each element's content in each distinct frame (0 = not there), and how often it changed.
    std::unordered_map<uint64_t, std::vector<uint64_t>> contents;
    std::unordered_map<uint64_t, const FrameContent::Part *> any; // a part of that key, for its box and label
    for (size_t f = 0; f < seen.size(); ++f)
        for (const FrameContent::Part &part : seen[f]->parts)
        {
            auto &column = contents[part.key];
            column.resize(seen.size(), 0);
            column[f] = part.content | 1; // never 0
            any.emplace(part.key, &part);
        }
    std::unordered_set<uint64_t> live;
    for (auto &[key, column] : contents)
    {
        column.resize(seen.size(), 0);
        int changes = 0;
        for (size_t f = 1; f < column.size(); ++f)
            changes += column[f] != column[f - 1];
        if (changes > kLiveChanges)
            live.insert(key);
    }
    const auto box_of = [](const FrameContent &frame, uint64_t key) -> const FrameContent::Part * {
        const auto found = std::lower_bound(frame.parts.begin(), frame.parts.end(), key, [](const auto &part, uint64_t k) { return part.key < k; });
        return found != frame.parts.end() && found->key == key ? &*found : nullptr;
    };
    const size_t last = seen.size() - 1;
    for (size_t f = 1; f < last; ++f)
    {
        Coverage coverage(width, height);
        std::vector<std::string> what;
        int changed = 0;
        for (const auto &[key, column] : contents)
        {
            if (live.count(key) || column[f] == column[0] || column[f] == column[last])
                continue;
            // Where it is seen: in this frame, or where the start or the end has it.
            const FrameContent::Part *part = box_of(*seen[f], key);
            if (part && part->feedback)
                continue; // inside a busy press part: the press's own feedback
            const FrameContent::Part *other = part ? nullptr : box_of(end, key);
            if (!part && !other)
                other = box_of(start, key);
            const FrameContent::Part *shown = part ? part : other;
            if (!shown || shown->box.Width() <= 0.0f || shown->box.Height() <= 0.0f)
                continue; // nothing of it on the screen
            coverage.Add(shown->box);
            ++changed;
            if (what.size() < 4)
                what.push_back((part ? "" : "-") + shown->label);
        }
        if (!changed)
            continue;
        const double ms = (seen[f + 1]->time - seen[f]->time) * 1000.0;
        const double share = coverage.Share();
        ++trace.transient;
        trace.transient_ms += ms;
        trace.area = std::max(trace.area, share);
        char head[160];
        std::snprintf(head, sizeof head, "+%.0f ms %.0f ms %.0f%% (%d elements): ", (seen[f]->time - frames.front().time) * 1000.0, ms,
                      share * 100.0, changed);
        std::string line = head + seen[f]->shown + ":";
        for (const std::string &one : what)
            line += " " + one;
        trace.details.push_back(std::move(line));
    }
    return trace;
}

std::string DescribeTrace(const ActionTrace &trace)
{
    char head[1024];
    if (trace.transient)
        std::snprintf(head, sizeof head, "UI flicker: %s: %d transient frames, %.0f ms, %.0f%% of the screen (%s -> %s, settled in %.0f ms, %d frames%s%s)",
                      trace.action.c_str(), trace.transient, trace.transient_ms, trace.area * 100.0, trace.before.c_str(), trace.after.c_str(),
                      trace.settle_ms, trace.frames, trace.interrupted ? ", interrupted" : "", trace.unsettled ? ", unsettled" : "");
    else
        std::snprintf(head, sizeof head, "UI action: %s: no transient frames (%s -> %s, settled in %.0f ms, %d frames%s%s)", trace.action.c_str(),
                      trace.before.c_str(), trace.after.c_str(), trace.settle_ms, trace.frames, trace.interrupted ? ", interrupted" : "",
                      trace.unsettled ? ", unsettled" : "");
    std::string line = head;
    // The latency of the action: when the screen first answered, and whether a predicted result was in the tap's frame.
    char answer[160] = "";
    if (trace.feedback_frame)
        std::snprintf(answer, sizeof answer, "; first change in frame %d, +%.0f ms", trace.feedback_frame, trace.feedback_ms);
    line += answer;
    if (trace.predicted)
        line += trace.late ? "; LATE: its predicted result was not drawn in the tap's frame" : "; predicted";
    for (const std::string &detail : trace.details)
        line += " | " + detail;
    return line;
}

} // namespace storm::ui
