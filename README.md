**English** | [Русский](README.ru.md)

# Seawolves Engine

An Android fork of [Storm Engine](https://github.com/storm-devs/storm-engine), the engine of the Sea Dogs games.
It powers Seawolves, an app for playing Sea Dogs: To Each His Own («Корсары: Каждому своё») on a phone with
your own copy of the game. Licensed under GPL-3.0.

**No game files are included, and none ever will be.** You need your own legally obtained copy. The app copies
it through the system file picker: an installed game folder, an archive or an installer. The GitHub build can
also fetch your own torrent or magnet link; the app ships no links to the game.

## How it differs from upstream

- A [bgfx](https://github.com/bkaradzic/bgfx) renderer (OpenGL ES 3.0 or Vulkan) instead of DirectX 9; the old
  code draws through a D3D9-shaped compatibility layer.
- A touch interface built on RmlUi in place of the original screens.
- Android lifecycle: pausing and resuming, an autosave when the game goes to the background, logs and diagnostics.
- Graphics: relief, normal and gloss maps made on the phone from the game's files by the PBRify networks;
  parallax on stone, wood and plaster; reworked lighting and shadows; waves around ships and characters;
  wet surfaces in the rain; far LODs for islands, ships and locations; a neural ×2 texture upscale.
- Performance work for phones: background resource preparation with a cache, thermal awareness, quality presets.

## What was tested

Only Sea Dogs: To Each His Own. Other games on this engine (Caribbean Legend, older Sea Dogs games) are not
supported and were not tested.

## Requirements

Android 7.0 (API 24) or newer, 64-bit ARM (arm64-v8a), a GPU with OpenGL ES 3.0. Vulkan 1.1 is used by default
where the device supports what the game needs and passes a start-up self-test; otherwise, and after a Vulkan
crash or failure on that driver, the game runs on OpenGL ES (logged, shown in the graphics settings).
`render_backend = gles | vulkan` in engine.ini forces a backend. Texture formats and effects follow the GPU's
capabilities, and anything turned off is logged.

## Building

This repository builds the native engine library only, not an APK:
`bash tools/android/ci_dependencies.sh`, then `bash tools/android/ci_engine.sh`
(details in [tools/android/README.md](tools/android/README.md)). For the Linux host development build, see
`cmake/host.cmake`. The Android app, Seawolves, lives in a separate repository that is not public yet.

## Reporting bugs

In the game, open About → Share log and attach the log to your report. If the bug involves a save, use
Send a save on the same screen. Bugs go to this repository's
[Issues](https://github.com/maxyo/seawolves-engine/issues) with the log attached.

## License and credits

The code is GPL-3.0 ([LICENSE.txt](LICENSE.txt)). Licences of third-party libraries and models are in
[third_party/licenses](third_party/licenses).

Thanks to the original Storm Engine authors for opening the engine, and to the
[storm-devs](https://github.com/storm-devs/storm-engine) community that maintains it: this port is a fork of their work.
