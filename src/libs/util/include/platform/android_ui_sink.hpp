// The engine's side of the portable UI on Android: the UI host lives in this process and
// draws inside the engine frame (src/libs/ui_host). Engine code publishes its states and
// interface models here, on the game thread, in-process; nothing of it crosses JNI.
// Implemented by src/libs/ui_host/src/engine_ui.cpp.
#pragma once
#ifdef __ANDROID__
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace storm::ui
{
struct InterfaceModel; // storm_ui/interface_model.h
}

namespace storm::android::ui
{
// A state channel of the portable UI: JSON as the UI host's SetState takes it ("null" =
// nothing). Kept until the host exists, then delivered in order.
void state(std::string_view channel, std::string json);
// The open interface opened (`screen` not empty, `revision` of the opening) or closed. An
// opening says how long the game took to make it: releasing the old interface and loading
// the new one's INI with its nodes (milliseconds, logged with the opening).
void screen(const std::string &screen, uint64_t revision, double release_ms = 0.0, double load_ms = 0.0);
// The interface that closed is followed by no other: the UI drops it now. Until this is said (or another interface
// opens, or a load starts) the UI keeps drawing the closed one: the scripts close one menu some frames before they
// open the next, and the game must not flash between them. Said every frame while no interface is open and none
// is scheduled (xinterface); a state of the game, not a time.
void interfaceGone();
// The interface model of the open screen (xinterface), in the frame it changed; the newest
// replaces older ones. Taken as the struct itself: nothing is serialised.
void interfaceModel(storm::ui::InterfaceModel &&model);
// The answer to a widget action of the open interface (UiWidgetAction::request): applied (its
// script events have run) or refused with the reason. Request 0 is nobody's: ignored.
void answer(uint64_t request, bool applied, std::string reason);
// The engine's facts of state channel "presentation", in the frame they change: a loading screen
// is up, a cinematic plays (the hero is an actor, a video runs), a dialogue covers the game. The
// open interface comes from screen(); only `live` (the game is in front and running) is Android's.
void loading(bool active);
void cinematic(bool active);
void dialogue(bool active);
// A location load (render::GetSceneLoadStatus) blocks the game: shown as loading.
void locationLoad(bool blocks);
// A passing line of the game's log: a toast over the game; `target` is where a tap on it
// leads (Host::GameMessage), "" = nowhere.
void toast(std::string text, std::string target = {});
// Something the player asked for failed: a lasting danger toast of the pack string `string_key`.
void alert(std::string string_key);
// Top of every engine loop iteration, before the queued commands are read: starts the host
// once the renderer is ready and applies what the platform thread sent (touches, presentation).
void tick();
// Engine commands the UI produced since the last call ("widget:…", "hold:…"), oldest first;
// a latest-value slot (stick position) appears once, with its newest command.
std::vector<std::string> takeCommands();
// Increments when a UI host starts (0 before the first): publishers whose commands carry an
// activity generation start a new one, and republish, for every new host.
uint64_t generation();
// Before the renderer goes away at engine exit.
void shutdown();
} // namespace storm::android::ui
#endif
