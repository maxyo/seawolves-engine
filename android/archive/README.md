# Safe game archive import library

`libstorm_archive.so` reads installer/archive **data** into an empty private staging directory. It never
launches an installer, loads an extracted library, evaluates installer scripts or applies registry/run
entries, and there is no external-helper fallback.

The Android app owns document selection, copying SAF documents into private input storage (one directory per
source set) and publishing a validated staging directory. Delete a failed/cancelled staging directory in full.
Never extract into a live game, saved-game directory or an existing game set.

## Build and package

```sh
python3 android/archive/build.py \
  --build-dir /absolute/local-testing/archive-loader/ndk \
  --ndk /absolute/android-ndk-r30 --jobs 4
```

The script fetches only SHA-256-pinned upstream releases and links `output/libstorm_archive.so` for Android
API 24 (16 KiB ELF alignment, shared libc++). Package that `.so`, the engine's matching `libc++_shared.so`
and the notices in `licenses/`. `--sources` reuses a dedicated source cache (not one another build is
patching). Omit `--ndk` for a host library and `storm_archive_cli`. Standalone CMake expects the pinned
sources and prepared prefix from `build.py`.

## Android JNI contract

Class `org.stormengine.port.NativeArchives`, `System.loadLibrary("storm_archive")`.

```java
static native String[] discover(String source, String emptyScratch, String passwordOrNull,
                                String patchLayerOrNull, long[] limits, Progress callbackOrNull);
static native String[] patchLayer(String patchLayer);
static native String[] extractRoot(String source, String scratch, String passwordOrNull,
                                   String rootId, String emptyPrivateStaging,
                                   long[] limits, Progress callbackOrNull);
interface Progress {
    boolean onProgress(int stage, String path, long bytes, long totalBytes, long filesCreated);
}
```

Calls are synchronous (use a worker thread). The callback runs on that thread and returns **true to
cancel**; it may fire with an empty path, and `filesCreated` counts opened output files. Results are
`[decimalStatus, format, error, json]`; success is only status 0 from `extractRoot`. C consumers use
`storm_archive.h`.

| Status | Meaning |
|---|---|
| 0 | Success |
| 1 | Input/output failure |
| 2 | Unsupported archive, installer version, or feature |
| 3 | Unsupported executable installer; no code was run |
| 4 | Encrypted input (see passwords) |
| 5 | Invalid/corrupt input, unsafe path/link, duplicate, conflicting roots, or source changed since discovery |
| 6 | Entry, byte, memory, ratio or time limit |
| 7 | Cancelled |
| 8 | Missing Inno sibling volume |
| 9 | Required game structure missing |
| 10 | Not enough free storage |

Statuses 6, 7 and 10 end the call; other container failures during discovery become `issues`.

### discover

Finds **every complete game root** (a folder holding `engine.ini`, `PROGRAM/` and `RESOURCE/`, ASCII
case-insensitive). `source` is a directory (searched recursively) or one file in a directory (only that file
and its own volumes are read). It recognises loose folders, ZIP/7z/RAR/tar(.gz/.bz2/.xz)/CAB/ISO archives by
signature, Inno Setup installers with sibling volumes, multi-volume sets (`game.7z.001`, `game.part1.rar`,
`game.rar` + `game.r00`; split ZIP `.z01` is unsupported) and containers inside containers up to `maxDepth`
levels (members written to `scratch/<n>/`). Each root is a separate candidate; roots are never merged.

```json
{"version":2,
 "candidates":[{"id":"…","kind":"archive","format":"zip","location":["download.zip","repack.7z"],
   "root":"Game/","title":"…","version":"…","languages":["english","russian"],
   "default_language":"russian","file_count":46418,"unpacked_bytes":12734518486,
   "patch_markers":["Patch_1.3_readme.txt"],"ready":true,"missing_volumes":[],
   "identified":true,"patch_set":{"id":"teho-1.7.3-x64","version":"ver. 1.7.3 (26.11.21) x64"}}],
 "patch_sets":[{"id":"teho-1.7.3-x64","game":"…","version":"ver. 1.7.3 (26.11.21) x64",
   "identify":["program/_lsc_on_off.h"]}],
 "issues":[{"location":["menu.exe"],"status":3,"error":"…","missing_volumes":[]}],
 "partial_roots":[{"location":["half.zip"],"root":"Game/","has_engine_ini":false,
   "has_program":true,"has_resource":true}],
 "scratch_bytes":3624000000}
```

`kind` is `folder`, `archive` or `inno`; an Inno root with missing volumes is `ready:false` and names them.
`file_count`/`unpacked_bytes` are exactly what `extractRoot` writes.

**Script patch identification.** `patchLayer` is the engine's script patch layer directory (Android:
`<filesDir>/script-patches/current`, engine `src/libs/util/include/platform/script_patches.hpp`,
`tools/script_patches/README.md`). Discovery loads it as the engine does and identifies every copy with the
engine's own rule, `script_patches::identify`, over the copy's identify files (whole files up to 1 MiB, keyed
as the engine's resource index keys the installed file): the launcher's verdict and the set the engine
applies at start-up cannot differ. Per candidate, `identified` says the identify files were read (loose
folders and random-access archives: ZIP, ISO, CAB) and `patch_set` is the set that identifies the copy, or
`null` when no set does (the game would run unpatched). Copies inside installers and solid archives (7z, RAR,
tar) are not read before extraction: `identified:false`, `patch_set:null` (unknown, not "untested"); an
identify file over 1 MiB is unknown as well. `patch_sets` lists the layer's sets (what the launcher names as
the tested versions). A `null` layer means the caller does not identify copies (the archive host checks, and
the app's download-library check «does this set hold a game»): every candidate is `identified:false` and
`patch_sets` is `null`. A layer path that is not a directory fails with status 1, a damaged layer (or two
sets identifying one copy) with status 5; neither is ever reported as "no set".

`patchLayer(path)` returns `[status, "", error, {"patch_sets":[…]}]` (the same entries) for a caller that
copies a copy's identify files itself before discovery (the app's picked-folder import).

### extractRoot

Writes exactly the root named by the opaque `id` (from the same source and scratch) into an empty staging
directory as `RESOURCE/`, `PROGRAM/`, `engine.ini`, `options`; wrappers and other files are skipped,
executable/library/shell extensions are excluded. The caller deletes scratch afterwards.

### Limits, space, passwords

`long[8]`, all positive: `[totalBytes, fileBytes, entries, seconds, scratchBytes, maxDepth, maxRatio,
reserveBytes]`; defaults 64 GiB, 4 GiB, 400000, 3600, 32 GiB, 3, 100, 64 MiB. Decoded bytes of one container
may not exceed `maxRatio` × its input size + 1 MiB. Free space must cover the data plus `reserveBytes`;
status 10 returns `{"no_space":{"step":"nested"|"install","needed_bytes":N,"available_bytes":M}}`.

Encrypted input returns status 4 with
`{"encrypted":{"location":[…],"password_supported":bool,"password_given":bool}}`. Only ZIP traditional
(ZipCrypto) encryption can be opened: pass the password to both calls. Archive/codec memory is capped at
768 MiB per instance; some installers use a 512 MiB LZMA dictionary, so expect memory pressure on low-memory
phones (allocation failure is status 6).

## Formats

* Inno Setup via innoextract 1.9 (Unicode versions through 6.0.5; ANSI and unknown versions fail). Only
  literal `{app}` paths; no GOG Galaxy reassembly, external file entries, component selection or multipart
  logical files. All `.bin` volumes must sit beside `setup.exe` with their exact names.
* ZIP/ZIP64, 7z, tar variants, Microsoft CAB, RAR/RAR5, ISO9660 via libarchive 3.8.10 (codecs: zlib, bzip2,
  liblzma, built-ins). Not every RAR/CAB/7z variant is supported.
* Generic EXE/SFX, NSIS, MSI, InstallShield CAB and unknown packers are unsupported.

Paths must be relative, without traversal, control characters, drive/ADS colons, empty components or
trailing dots/spaces (1024 bytes, 32 components, 255 bytes per component). Links, special files, existing
outputs and ASCII case collisions are rejected.

## Tests

```sh
python3 android/archive/tests/test_archives.py --library /absolute/build/reader/libstorm_archive.so -v
# discovery/nested extraction; host library built with -DSTORM_ARCHIVE_TESTING=ON (simulated free space)
python3 android/archive/tests/test_discovery.py --library /absolute/build/reader/libstorm_archive.so \
  [--libarchive-tests /absolute/sources/libarchive-3.8.10/libarchive/test] [--inno-installer /absolute/setup.exe] -v
# allocation failures (STORM_ARCHIVE_TESTING=ON, host only)
python3 android/archive/tests/test_oom.py --library /absolute/build/reader/libstorm_archive.so \
  --report /absolute/local-testing/archive-oom.json
```

`--inno-installer` adds a local reference Inno 5.6 installer (metadata, missing volumes, cancellation); no
game assets are in the tests. Host JNI check:

```sh
cmake -S android/archive -B /absolute/build/reader -DSTORM_ARCHIVE_HOST_JNI=ON
cmake --build /absolute/build/reader --parallel 4
javac -d /absolute/build/jni-test android/archive/tests/NativeArchives.java
java -Xcheck:jni -Djava.library.path=/absolute/build/reader \
  -cp /absolute/build/jni-test org.stormengine.port.NativeArchives
```

## Upstream and licenses

Versions, URLs and SHA-256 digests are pinned in `build.py`; the original notices are in `licenses/`.

| Dependency | License | Upstream |
|---|---|---|
| innoextract 1.9 | zlib | https://constexpr.org/innoextract/ |
| libarchive 3.8.10 | BSD notices in COPYING | https://www.libarchive.org/ |
| XZ liblzma 5.8.4 | 0BSD | https://tukaani.org/xz/ |
| bzip2 1.0.8 | bzip2 BSD-style license | https://sourceware.org/bzip2/ |
| Boost 1.90.0 filesystem/iostreams/headers | BSL-1.0 | https://www.boost.org/ |
| zlib 1.3.2 | zlib | https://zlib.net/ |
