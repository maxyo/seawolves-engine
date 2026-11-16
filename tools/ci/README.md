# Host CI, the push gate and the land queue

| Tool | What | When |
| --- | --- | --- |
| `run_host_tests.py` | every `tests/*/run.py` (no NDK, device or game data) | CI on develop pushes and PRs; `--only`, `--changed-since REF` |
| `affected.py` | what a change can affect: host checks, game-data checks, the UI host and Android builds | used by the two below; `affected.py --base REF` prints the plan |
| `gate.py` | the push gate: builds and checks what the change affects, in parallel | before every push to develop (`--push`) |
| `boot_stage.py` | the engine boot stage: an ASan+UBSan host engine boots, loads a save and three scenes | `gate.py --boot auto/on`, or alone |
| `land.py` | the merge queue: several agents' commits rebased and gated together, then pushed | instead of `gate.py --push` when other agents land too |

## The gate (`gate.py`)

```sh
tools/ci/gate.py --rebase            # rebase onto origin/develop, plan, run; no push
tools/ci/gate.py --rebase --push     # ... and push HEAD to develop if everything passed and develop did not move
tools/ci/gate.py --tests renderer_bgfx save_dangling_references   # add host checks by name
tools/ci/gate.py --full --boot on    # every stage, including one boot session: before a release build
```

It plans from `git diff origin/develop...HEAD` and prints one line per stage, `RUN` with the reason or
`SKIP` with the reason; skipped host checks are counted on the console and listed with their reasons in
`build/gate/plan.txt`. Stages run in parallel, each into `build/gate/<stage>.log`:

- **host_checks**: the affected `tests/*/run.py` (`run_host_tests.py --only ...`, longest first), as many at once
  as `--jobs` (default 0: the free cores, cores minus the 1-minute load average, 3 to 8; the same number goes to
  renderer_bgfx's own parallel checks as `STORM_CHECK_JOBS`);
- **game_checks**: the affected `tools/game_checks/*` on the local game (`$STORM_GAME_DIR`, default
  `<workspace>/local-testing/extracted/app`; missing game = the gate fails, it is not skipped), in parallel. A
  pass is remembered in `build/game-checks/results/<name>.json` with the digest of its inputs: its own files, every
  file its compiles read (a CMake check: its Ninja graph; a direct one: `tools/ccache/cxx` `STORM_CXX_DEPS`), the
  compiler and the game (path, size and time of every file). The same inputs again print `CACHED PASS` with the
  time of that pass instead of running (far_lod: 6-10 minutes). A failure is never remembered;
- **ui_host**: `build/ui-host` (src/libs/ui) build + ctest;
- **android**: `build/android-engine`, arm64 Release (LTO) `engine`, SDK/NDK from `$STORM_CI_WORK`;
- **boot** (`--boot auto|on|off`, off by default locally; the land queue uses auto): `boot_stage.py`, below.

## The engine boot stage (`boot_stage.py`)

The host checks replace the engine's seams (script VM, entity lifetimes, renderer, techniques, save loader) with
stubs; this stage starts the real engine. `build/boot-asan` (RelWithDebInfo, `-fsanitize=address,undefined`,
`-DSTORM_SCENE_SWEEP=ON`, kept warm through ccache) runs the scene sweep with one engine on OpenGL ES on the host
GPU through VirtualGL: main menu → the canonical save loaded from it (`save:boot`, the town Сен-Пьер) → `sea:Martinique`
→ the town again → `loc:FortFrance_tavern` → exit. `--single-session` forbids engine retries,
including after a crash; any remaining scenes fail. The stage requires a fresh report covering the menu,
save, sea and tavern, and rejects incomplete results or a session count other than one. Existing known
game-data defects (`tools/scene_sweep/known_defects.json`) remain reported as KNOWN; existing UBSan
suppressions are listed with causes in `tools/ci/ubsan.supp`. All other matching errors fail the run:
crash, ASan report, UBSan `runtime error:`, fatal engine error, script errors, missing resources, a black
or HUD-less frame, or its time budget (`--budget`,
`$STORM_BOOT_BUDGET`, default 300 s for the sweep; the build is logged separately). Three scenes from a save, selected
only by changes that can affect the engine at run time; not the all-scenes sweep (`tools/scene_sweep/README.md`).

Needs, else it fails loudly: `STORM_VIRTUALGL` (an unpacked VirtualGL, e.g. `<workspace>/local-testing/gpu-sweep/vgl`),
the game (`$STORM_GAME_DIR`) and the save (`$STORM_BOOT_SAVE`, default `<workspace>/local-testing/boot-stage/saint-pierre-streets`,
the Test Lab scenario 3 save «Сен-Пьер улицы»). Log: `build/gate/boot.log` (per scene: status, seconds, load time;
build, sweep and total time), the sweep's report in `build/gate/boot/`.

Rules of `affected.py` (also in its docstring):

- `*.md` affects nothing; `.github/` (run by GitHub after the push) affects nothing here. `tools/ccache/` and
  `tests/<file>` affect everything; `tools/ci/run_host_tests.py` and what it fetches for every check
  (`cmake/glm.cmake`, `tools/android/engine_sources.py`, `tools/android/ci_dependencies.sh`) every host check.
  The rest of `tools/ci/` (the checks naming it: `tests/ci_*`), `cmake/` and the top-level `CMakeLists.txt` (the
  builds that read them as configure inputs) are files like any other.
- A host check is affected when its directory changed or its files name the changed file, its directory or
  its component (`src/libs/<lib>/`, `src/apps/<app>/`, `<top>/<name>/`). A CMake check already built in
  `build/host-ci` is decided by its Ninja graph: only changed files the build reads count. A check that compiles
  through `$CXX` itself is decided by what its last passing run compiled (`build/host-ci/deps/<name>.txt`, written
  by `run_host_tests.py` from `tools/ccache/cxx`): a changed file on that list, its exact path spelled by the
  check, or its directory spelled by a script that scans it (or any spelling, for a file that is not C/C++).
  Spelling `-I src/libs/renderer_bgfx/src` no longer selects a check for every renderer change. Without a
  trusted list (never passed here, or its scripts call `c++`/`cmake` directly) the name rule above applies.
- A build is affected when a changed file is in its Ninja graph (dependency log, inputs, configure inputs)
  or is a new file in a directory the build reads; without a built directory it always runs.
- A game-data check is affected when a header its `check.cpp` includes changed (compiler `-MM`), or a file of a
  library it links: its graph in `build/game-checks/<name>` once built, else what its files name.
- Names match with or without the final slash (`src/libs/relief"`), never as the start of a longer name.
- The boot stage: an input of `build/boot-asan` (not built yet: any change under `src/`, `android/`), `src/techniques`
  (read at run time by no build), `tools/scene_sweep`, `boot_stage.py`, `ubsan.supp`. A source file that selects no host or
  game-data check selects the boot stage ("owned by no check"): no change selects nothing (`tests/ci_affected`).

Examples of what still runs (2026-11-14): a change of `src/libs/core/src/attributes.cpp` or `compiler.cpp`
runs 28 checks including `script_state_generation` (ATTRIBUTES re-parenting) and `save_dangling_references`
(ASan); `src/libs/location/src/grass_pack.hpp` runs the `grass_pack` host check and the `grass_pack` game check
on every map of the game; any `src/libs/renderer_bgfx` change runs `renderer_bgfx` (`preset_switch_check`:
the pause black frame, on OpenGL ES and Vulkan) and the Android build.

## The land queue (`land.py`)

```sh
tools/ci/land.py                 # queue HEAD's commits and wait until they landed (exit 0) or failed (exit 1)
tools/ci/land.py --tests NAME    # with extra host checks
tools/ci/land.py --status        # queue, last results; runner log in <git common dir>/storm-land/runner.log
```

All worktrees share one queue (`<git common dir>/storm-land`). The first agent without a running runner
becomes the runner; the others wait. The runner works in `<workspace>/storm-engine-land` (warm build
directories), rebases every queued entry onto origin/develop and the entries before it, gates the stack
once with `--boot auto` and pushes it. Runtime-affecting changes selected by `affected.py` run one boot
smoke for the entire stacked tree; a passing batch never runs boot again per entry. Local developer/agent
`gate.py` runs default to `--boot off`; use `--boot on` for an explicit local session. A failing batch is
gated entry by entry; the passing entries land, each failing entry gets its reason and its gate logs (`<git common dir>/storm-land/logs/<id>/`). A conflicting entry fails
with the conflict. During entry-by-entry isolation, other checks still cover the kept stack, but
`--boot-base <previous-stack-tip>` selects boot from only the current entry’s changes. An earlier runtime
entry therefore does not trigger boot again for a later documentation entry. If develop moves, the new
stack must be gated again. Nobody rebases under a moving develop any more: the queue is the only local
pusher. After landing, `git fetch && git rebase origin/develop` in your worktree.

The gate's logs and the boot report (`report.json`, `summary.txt`, pictures; not the sweep's game copies) are
copied into each entry's log directory; `gate.py` empties `build/gate` at start, so only that gate's results are
there. The gate's environment is the runner's, updated by `<git common dir>/storm-land/env` (`NAME=VALUE`
lines, `#` comments), so it does not depend on which agent became the runner. `STORM_VIRTUALGL` (the VirtualGL
directory) must be set there or in the environment, or `land.py` refuses to queue:

```
echo "STORM_VIRTUALGL=$WORKSPACE/local-testing/gpu-sweep/vgl" >> "$(git rev-parse --git-common-dir)/storm-land/env"
```

## Tiers

- **Before every push**: `gate.py` / `land.py` (what the change affects).
- **After a push**: CI (`.github/workflows/host.yml`) runs all host checks on develop.
- **Before a release build**: `gate.py --full --boot on`, plus the on-demand engine suites (`tools/scene_sweep`, full
  parity) only for engine/renderer changes; heavy runtime suites (lifecycle, resize) only when their code changed.
