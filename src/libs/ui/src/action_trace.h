// The flicker check: what the screen showed between a player's action and the state it settled in.
//
// A tap that the game answers in several steps (it selects a row, then opens its amount window a loop
// later; it closes one interface and opens the next) can make the UI present a frame of each step: a
// state that is neither what the player tapped on nor what the tap leads to, seen for a frame or more.
// That is flicker. The tracer records, from the player's action on, the content of every frame the
// host presents (each visible element's place in the tree, its tag, classes, attributes and text, and
// its box) until the content stops changing, and then counts the transient frames: frames whose content
// differs from both the start and the settled state. Their time on the screen and the share of the
// screen they changed say how visible each was (Host log "UI flicker", ActionTrace in ui.h).
//
// Elements that change on more than kLiveChanges frames of one action (a running timer, a meter) are
// live content, not flicker, and are left out. Content includes each box's place in its parent and its size (3 px).
// Interaction feedback is not content: the pressed and shake classes of a press part, and its busy mark (the
// class, the spinner, and how the part's inside looks while the mark is shown): the press was taken and waits
// for the game.
#pragma once

#include "storm_ui/ui.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Rectangle.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace storm::ui
{

// What one presented frame showed.
struct FrameContent
{
    struct Part
    {
        uint64_t key = 0;     // the element's place: its parent's key, tag, id and index among its siblings
        uint64_t content = 0; // tag, classes, attributes (not style), text
        Rml::Rectanglef box;  // on the screen, clipped by the boxes that clip it (physical pixels)
        std::string label;    // "tag#id 'text'" for the report
        bool feedback = false; // inside a press part that shows its busy mark: interaction feedback, not judged
    };
    std::vector<Part> parts; // sorted by key
    std::string shown;       // FrameStats::shown of the frame
    double time = 0.0;       // host clock when it was presented
    uint64_t hash = 0;       // of every part's key and content
};

// The visible elements of `documents` (hidden documents are skipped), within a viewport of `width` x `height`.
FrameContent CaptureContent(std::span<Rml::ElementDocument *const> documents, float width, float height, std::string shown, double time);

class ActionTracer
{
  public:
    // An element that changes on more frames than this within one action is live content.
    static constexpr int kLiveChanges = 3;
    // The content settled when no presented frame changed it for this long (seconds).
    static constexpr double kSettleSeconds = 0.6;
    // An action whose frames keep changing longer than this is reported unsettled.
    static constexpr double kMaxSeconds = 6.0;

    // A player's action starts: `start` is the content on the screen when it happened. One still open is finished first
    // (interrupted: its last frame is its end).
    std::optional<ActionTrace> Begin(std::string what, FrameContent start, float width, float height);
    // The event the action caused, for its name ("goods row", "header-back click"); the first one names it.
    void Named(const std::string &event);
    // The screen predicted the action's result: its own frame must show it (ActionTrace::late).
    void Predicted()
    {
        predicted_ = predicted_ || active_;
    }
    bool Active() const
    {
        return active_;
    }
    // The action is not one to judge (a HUD control held and moved: the game's live states are what changes on
    // the screen then, not an answer to a tap).
    void Drop()
    {
        active_ = false;
        frames_.clear();
    }
    // A frame was presented while an action is open.
    void Presented(FrameContent frame);
    // Called on every host pass: the action that settled (or ran out of time) is finished and returned. `waiting`: the
    // host still waits for the game (an interface it opened has no frame yet, a widget action is not answered): the
    // action has not settled however long its content stayed.
    std::optional<ActionTrace> Poll(double now, bool waiting);
    // The action is over now (a new touch went down, the host closes): its last presented frame is its end.
    std::optional<ActionTrace> Finish(bool interrupted);

    // The analysis itself, for checks: frames[0] is the start, the last one the settled state.
    static ActionTrace Analyse(const std::string &action, const std::vector<FrameContent> &frames, float width, float height);

  private:
    bool active_ = false;
    bool predicted_ = false;
    std::string what_;
    std::string event_;
    std::vector<FrameContent> frames_;
    double begun_ = 0.0;
    double changed_ = 0.0; // when the last frame that changed the content was presented
    float width_ = 0.0f, height_ = 0.0f;
};

// One line for the log: "UI flicker: …" with the transient frames, or "UI action: …" without.
std::string DescribeTrace(const ActionTrace &trace);

} // namespace storm::ui
