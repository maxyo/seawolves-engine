# Script VM checks

The script VM (`src/libs/core`: the compiler and interpreter, `DATA`, `ATTRIBUTES`, the name codec) runs the game's own
scripts, which we never change. Its optimisations must therefore be invisible to them and to the saves: the same
values, the same attribute trees in the same order, the same name codes in a save.

Three things hold that:

- **Checks inside the engine** (`-DSTORM_VM_VERIFY=ON`, a host option like `STORM_SCENE_SWEEP`; never shipped builds).
  Every fast path that replaced a plain computation also runs the plain one and compares
  (`src/libs/core/include/vm_verify.hpp` lists them): a difference logs `vm verify: MISMATCH: ...` and aborts. The
  engine prints the number of checks of each kind when it exits. A new fast path gets its check with it.
- **Sessions of the real game** on such an engine, `tools/vm_verify/run.py`: a new game with its opening dialogue, a
  town, interiors, land combat, the sea, a sea battle, a boarding, the world map, a storm, and with `--save FILE` the
  same from a loaded save. The run fails on a mismatch, on an engine that did not end normally and on a kind of check
  that never ran. On demand, like the scene sweep it drives: it needs the player's game files and a GPU (or
  `-- --render llvmpipe`), and takes a few minutes.
- **Host checks** in the gate, without game data: `tests/script_vm_names` (the codec's numbering against the codec the
  saves were numbered with; `ATTRIBUTES` against a plain tree, under random operations).

```sh
cmake -S . -B build/host-verify -G Ninja -DSTORM_SCENE_SWEEP=ON -DSTORM_VM_VERIFY=ON ...   # as build/host otherwise
ninja -C build/host-verify engine
STORM_VIRTUALGL=~/vgl python3 tools/vm_verify/run.py --game <game dir> --engine build/host-verify/engine --out <dir>
```

Only `src/libs/core` is compiled differently with the option, so a second build directory shares the compiler cache
with the plain one for everything else.
