# Window and application events

`os_window.hpp` is the window abstraction (SDL2 implementation in `src/sdl_window.cpp`). `app_events.hpp` is the
engine-wide subscription point for window and application lifecycle events; its contract (events, threading,
early subscribers, Android event order, queue bounds) is documented at the top of that header.

```cpp
const int id = storm::AppEvents::instance().subscribe([](const storm::AppEvent &e) {
    if (e.type == storm::AppEventType::WindowResized) resize(e.width, e.height);
});
// Early subscriber (second argument true): runs inside the SDL call that raised the event, before SDL gets
// control back, e.g. to release the surface on WillEnterBackground.
storm::AppEvents::instance().unsubscribe(id);
```

Scripts receive `AppWillEnterBackground`, `AppDidEnterForeground`, `AppLowMemory` and
`AppWindowResized(width, height)` as ordinary script events (`apps/engine/src/main.cpp`).
`AppWillEnterBackground` handlers run synchronously on the game thread (the application's overlay autosaves
there); their time is logged.

Tests: `tests/app_events/run.py --sdl-source <SDL checkout>` (real host SDL, ASan/UBSan and TSan) and
`tests/android_window/run.py` for the window mailbox.
