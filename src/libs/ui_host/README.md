# ui_host — the game's portable UI inside the engine (Android)

One `storm::ui::Host` lives in the engine process, on the game thread, and draws through the bgfx backend
(`storm_ui_bgfx`) in the frame owner's UI views, after the game picture, at native resolution. It receives state
channels, interface models (`xinterface`), toasts and presentation from the engine, touches and settings from
`GameActivity` through the `EngineUi` natives (applied by `ui::tick()` at the top of the next loop iteration), and
sends widget actions back to `xinterface` and host actions to the activity over JNI. A host that cannot start is an
error shown to the player (`uiFailed`).

Every screen switch logs one line (`StormUi` INFO): `UI switch <screen>: first frame N ms after the action …`.

## Debug logging

Set before the game starts (`adb shell setprop log.tag.<tag> DEBUG`):

- `StormUiInput`: per loop iteration every touch sample, action, model and frame with its CPU by stage;
- `StormUiState`: every state channel update;
- `StormUiModels`: writes the newest model of each screen to `<external files>/ui-models/`;
- `StormUiModelCheck`: collects the interface model every frame and logs a model that went stale.
