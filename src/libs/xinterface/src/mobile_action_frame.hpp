#pragma once

#include <string>

namespace storm::mobile
{
// What became of one widget action of the touch UI (XINTERFACE::MobileApplyAction). The UI gets
// the answer: applied once the action's script events have run (the model that follows shows its
// result), refused with the reason at once. A deferred action is answered when it is retried.
struct ActionOutcome
{
    enum State
    {
        Applied,
        Refused,
        Deferred,
    } state = Applied;
    std::string reason;
};

// The touch UI's widget actions of one interface frame (XINTERFACE::DoControl) are applied in the
// order the player made them, and the script event an action queued runs before the next action
// is applied. A node raises its script event through XINTERFACE's event list, which Execute runs
// newest first, so two actions applied one after the other with their events left for the next
// Execute ran their scripts in reverse: in the store, closing the open deal (QTY_CANCEL_BUTTON:
// exitCancel) and opening the next row (TABLE_LIST: ShowItemInfo) ran as ShowItemInfo, which
// closed the deal, and then exitCancel, which with no deal open leaves the store. An edit box's
// text and the event that reads it are the same case (the trade's "0" then "277"). Running each
// action's events at once keeps the order and answers the tap in the frame it arrived in: the
// model that frame publishes shows its result (tests/mobile_action_frame).

// The interface's click (the scripts' ISOUND_EVENT on a node's first press) belongs to the
// player's gesture: the touch UI asks for it when a press starts on an interactive element
// (engine command "uisound:press", main.cpp). The widget actions the UI sends for it afterwards
// (a tap's close-the-deal-and-open-the-row pair, a quantity drag's texts, a re-selection) are
// programmatic: a node's first press inside one is silent.
class UiAction
{
  public:
    // Scope of one widget action of the touch UI (XINTERFACE::DoControl).
    class Scope
    {
      public:
        explicit Scope(UiAction &action) : action_(action)
        {
            action_.active_ = true;
        }
        ~Scope()
        {
            action_.active_ = false;
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

      private:
        UiAction &action_;
    };
    // True while a widget action of the touch UI runs: CINODE::DoAction does not click.
    bool Active() const
    {
        return active_;
    }

  private:
    bool active_ = false;
};
inline UiAction uiAction;
} // namespace storm::mobile
