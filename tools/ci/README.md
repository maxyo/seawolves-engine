# Host CI, the push gate and the land queue

| Tool | What | When |
| --- | --- | --- |
| `run_host_tests.py` | every `tests/*/run.py` (no NDK, device or game data) | CI on develop pushes and PRs; `--only`, `--changed-since REF` |
| `affected.py` | what a change can affect: host checks, game-data checks, the UI host and Android builds, the boot stage | used by the two below; `affected.py --base REF` prints the plan, its docstring has the rules |
| `gate.py` | the push gate: builds and checks what the change affects, in parallel | run by `land.py`; alone for a local check |
| `boot_stage.py` | the engine boot stage: an ASan+UBSan host engine boots, loads a save and three scenes | `gate.py --boot auto/on`, or alone |
| `land.py` | the merge queue: queued commits rebased and gated together, then pushed | the only way to land on develop |

## The land queue (`land.py`)

```sh
tools/ci/land.py                 # queue HEAD's commits and wait until they landed (exit 0) or failed (exit 1)
tools/ci/land.py --tests NAME    # with extra host checks
tools/ci/land.py --status        # queue, last results; runner log in <git common dir>/storm-land/runner.log
```

Queue everything that is ready at once. All worktrees share one queue (`<git common dir>/storm-land`); the first
agent without a running runner becomes the runner. The runner works in `<workspace>/storm-engine-land` (warm build
directories), rebases every entry onto origin/develop and the entries before it, gates the stack once with
`--boot auto` and pushes it. A failing batch is gated entry by entry: passing entries land, each failing entry
gets its reason and gate logs in `<git common dir>/storm-land/logs/<id>/`; a conflicting entry fails with the
conflict. After landing, `git fetch && git rebase origin/develop` in your worktree.

The gate runs in the runner's environment, updated by `<git common dir>/storm-land/env` (`NAME=VALUE` lines,
`#` comments). `STORM_VIRTUALGL` (an unpacked VirtualGL) must be set there or in the environment, or `land.py`
refuses to queue:

```
echo "STORM_VIRTUALGL=$WORKSPACE/local-testing/gpu-sweep/vgl" >> "$(git rev-parse --git-common-dir)/storm-land/env"
```

## The gate (`gate.py`)

```sh
tools/ci/gate.py --rebase            # rebase onto origin/develop, plan, run; no push
tools/ci/gate.py --tests renderer_bgfx save_dangling_references   # add host checks by name
tools/ci/gate.py --full --boot on    # every stage, including one boot session: before a release build
```

It plans from `git diff origin/develop...HEAD` (`--base` to change it) and prints one line per stage, `RUN` or
`SKIP` with the reason (skipped host checks are listed in `build/gate/plan.txt`). Stages run in parallel, each
into `build/gate/<stage>.log`:

- **host_checks**: the affected `tests/*/run.py`, `--jobs` at once (default: the free cores, 3 to 8);
- **game_checks**: the affected `tools/game_checks/*` on the local game (`$STORM_GAME_DIR`, default
  `<workspace>/local-testing/extracted/app`; a missing game fails the gate). A pass is cached in
  `build/game-checks/results/<name>.json` by the digest of its inputs and printed as `CACHED PASS`;
- **ui_host**: `build/ui-host` (src/libs/ui) build + ctest;
- **android**: `build/android-engine`, arm64 Release (LTO) `engine`, SDK/NDK from `$STORM_CI_WORK`;
- **boot** (`--boot auto|on|off`, off by default locally, auto in the land queue): `boot_stage.py`.

Each check runs with its own `TMPDIR` in the build tree (`check_tmp.py`), removed after a pass and kept after a
failure for diagnosis.

## The engine boot stage (`boot_stage.py`)

`build/boot-asan` (RelWithDebInfo, `-fsanitize=address,undefined`, `-DSTORM_SCENE_SWEEP=ON`) runs the scene sweep
with one engine on OpenGL ES through VirtualGL: main menu → the save loaded from it → `sea:Martinique` → the town →
`loc:FortFrance_tavern`. It fails on a crash, ASan or UBSan report, fatal engine error, script errors, missing
resources, a black or HUD-less frame, or the time budget (`--budget`, `$STORM_BOOT_BUDGET`, default 300 s). Known
game-data defects (`tools/scene_sweep/known_defects.json`) stay KNOWN; UBSan suppressions with their causes are in
`ubsan.supp`.

Needs, else it fails loudly: `STORM_VIRTUALGL`, the game (`$STORM_GAME_DIR`) and the save (`$STORM_BOOT_SAVE`,
default `<workspace>/local-testing/boot-stage/saint-pierre-streets`). Log: `build/gate/boot.log`, the sweep's
report in `build/gate/boot/`.

## Tiers

- **Before landing**: `land.py` (what the change affects).
- **After a push**: CI (`.github/workflows/host.yml`) runs all host checks on develop.
- **Before a release build**: `gate.py --full --boot on`, plus the on-demand scene sweep (`tools/scene_sweep`) for
  engine/renderer changes.
