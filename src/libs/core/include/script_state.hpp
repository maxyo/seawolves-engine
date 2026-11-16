#pragma once

#include <cstdint>

namespace storm
{
// A count of the changes to the script state the native caches of the application's interface
// adapters depend on (xinterface MobileCollectFacts, MobileNationFlags: values computed by
// scripts from attributes).
//
// Counted: every script segment loaded or unloaded (compiler.cpp), and every attribute write
// that changes something (a value, a name, the set of children; attributes.cpp) in an attribute
// tree an adapter read since the last ScriptStateForget (ScriptStateReads). Writes to other trees
// do not count: the game rewrites some every frame (weather.c Whr_OnWindChange moves Weather.Wind
// on every "frame" event, an open interface included) and no adapter reads them. Writes made while
// an adapter runs do not count either: they are its own output (GameInterface.AndroidFacts), never
// game state (the adapters change no game rules). Between a ScriptStateForget (an interface opened
// or was released) and the end of the next ScriptStateReads no attribute change counts: no cached
// facts exist then (the caches are keyed by the interface opening), and gameplay without an open
// interface pays one flag test per attribute write. Attributes are read and written on the game
// thread only.
uint64_t ScriptStateGeneration() noexcept;
void ScriptStateChanged() noexcept;

// While one exists on the game thread (an adapter runs), every attribute read notes the tree
// (root object) it reads, and changes are not counted; when it ends, the trees it noted are added
// to the ones whose changes count. One made while another exists is part of it (the interface
// model's collection runs the facts adapter): the outermost one adds the trees.
class ScriptStateReads
{
  public:
    ScriptStateReads();
    ~ScriptStateReads();
    ScriptStateReads(const ScriptStateReads &) = delete;
    ScriptStateReads &operator=(const ScriptStateReads &) = delete;

  private:
    bool nested_;
};

// An interface opened or was released: the trees read so far stop counting until an adapter reads
// them again.
void ScriptStateForget();

// What engine code asks the script state by name while no script runs (the VM's "script work:" line, compiler.cpp):
// attributes looked up by a name or a path, of them inside a ScriptStateReads (an interface adapter) and in the
// frame's HUD phase (the Android HUD adapters, frame_work::Phase::Hud), and global variables found by name, of them in
// the HUD phase.
struct ScriptNameLookups
{
    uint64_t attributes = 0;
    uint64_t adapterAttributes = 0;
    uint64_t hudAttributes = 0;
    uint64_t variables = 0;
    uint64_t hudVariables = 0;
};
ScriptNameLookups &NameLookups() noexcept;
// Scripts are running (the VM's own lookups are not the engine's).
void ScriptRunning(bool running) noexcept;

// Advances whenever the game state can have changed: at every core frame (CoreImpl::Run: scripts, entities, loading)
// and whenever engine code starts a script outside one (an event raised by input or a HUD command). Native readers
// of the game state (the Android HUD adapters) read it once per value of this and reuse what they read until it
// advances. Game thread only.
uint64_t ScriptStateEpoch() noexcept;
void ScriptStateMayChange() noexcept;
void CountVariableLookup() noexcept;
} // namespace storm
