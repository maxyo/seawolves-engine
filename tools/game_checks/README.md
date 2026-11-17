# Game-data checks

Checks that need an installed game (its RESOURCE folder), so they are not host checks. Run them before a release;
the gate runs the affected ones (`tools/ci/README.md`). Each `run.py` prints its usage without arguments.

- `grass_pack/run.py <game directory>`: every grass map (`.grs`) packs into the grass vertices
  (`src/libs/location/src/grass_pack.hpp`).
- `far_lod/run.py <game directory>`: the far LODs (`src/libs/geometry/far_lod`) of every model that gets them.
- `skin_bones/run.py <game directory>`: every skeleton has its skin bones (head/neck, hands); animals have none.
- `character_skin/run.py <game directory> [--list N] [--model NAME]...`: skin reference and torso skin share of every
  character model atlas; outliers are flagged for a look.
