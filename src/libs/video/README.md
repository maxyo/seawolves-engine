# video

FFmpeg-based playback for the script-facing `CAviPlayer` entity (`xinterface/aviplayer`) and the
`VIDEO` interface node (`xi_video_rect`, which embeds a `CAviPlayer`). The FFmpeg build:
[tools/ffmpeg/README.md](../../../tools/ffmpeg/README.md).

## Decoder policy

Fixed per codec in `kPolicy` (`src/player.cpp`) and logged for every video:

| codec | Android | host |
| --- | --- | --- |
| WMV3, VC-1 | software (FFmpeg) | software |
| VP9 | MediaCodec `vp9_mediacodec`, then software | software |
| VP8 | MediaCodec `vp8_mediacodec`, then software | software |
| WMA v2, Vorbis, Opus | software | software |

A missing hardware VP8/VP9 decoder is the one deliberate fallback: logged as a warning, decoded in software
(late frames are dropped against the audio clock). A codec outside the table, an unreadable container or a
decode error ends the video with an error in the log, and the scripts receive `ievntEndVideo`.

## Script interface

- `CreateEntity(&aviVideoObj, "CAviPlayer")`, `SendMessage(&aviVideoObj, "ls", MSG_SET_VIDEO_PLAY,
  "<name>.wmv")`: plays `resource\videos\<name>` full screen, letterboxed.
- `MSG_SET_VIDEO_FLAGS` bit 0: loop.
- `ievntEndVideo` is sent once when the video has played out (or could not be played); the scripts delete the
  entity in their handler. Deleting it earlier stops playback.
- A tap sends `Control Activation`/`Control Deactivation` of `ICancel`; the scripts' `IVideoBreak` decides
  whether the video may be skipped.
- The soundtrack plays at the music volume.

## Tests

`tests/video/run.py --dependency-sources <dir>` (host): generated WebM and ASF clips through the real pipeline
with ASan/UBSan; `--smoke FILE...` plays local media in real time.
