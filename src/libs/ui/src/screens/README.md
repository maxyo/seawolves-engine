# Screens: game interfaces in the portable UI

The engine describes every open game interface as an `InterfaceModel`
(`include/storm_ui/interface_model.h`): the interface id and its widgets in reading order.
`Host::SetInterface` routes the model to a *screen*: C++ code here that turns the widgets
into the props of a pack screen (`<s-…>`, application repository
`mobile-ui/design/screens/<name>/`) and component events back into engine actions.
Interfaces without a ported screen (`kPorted` in `registry.cpp`) get the generic layout (`generic.cpp`).

```
engine ──InterfaceModel──▶ Router ──admission──▶ ported Screen ─┐
                                   └─(unknown id)─▶ generic layout ─┤─▶ props ─▶ <s-…> in the pack
engine ◀──────Action {screen, opening, node, index, verb, value}──┘◀─ events (id, verb, value)
```

## Adding a screen

1. **Pack.** `mobile-ui/design/screens/<name>/spec.json` (tag `s-<name>`, one prop per
   part of the view, plus `"extras": {"type": "json"}`), `template.rml` built only from
   shared `<c-…>` components and the screen parts (`s-extras`, `s-note`, `s-control`,
   `s-block`, `s-actions`), `style.rcss` with layout only (flex, sizes in dp or %, colours
   as tokens). The root is `<c-screen id="header" key="header" … actions="{{ json(extras.actions) }}">`
   and ends with `<s-extras id="extras" extras="{{ json(extras) }}"/>`; put
   `<s-note id="extras-note" text="{{extras.note}}"/>` under the main block. Nothing may
   assume a fixed size: the viewport is the real dp size of the display.
2. **C++.** A class derived from `Screen` (`screen.h`) in `src/screens/<zone>_<name>.cpp`:
   - `Component()` → `"s-<name>"`;
   - `Nodes()` → every node the design draws with the type it expects (`"a|b"` alternatives,
     `NAME*` prefix); `required = true` marks REQUIRED. REQUIRED names only what the game always
     sends: a table with no rows comes as one `table` widget, so a table is required as
     `"choice|table"` and its rows are read with `admitted.Rows(node)`;
   - `Build(admitted, bindings)` → the props (JSON). Read widgets with `admitted.One/All/Rows/Label`;
     bind every interactive element by its id: `bindings.Forward(id, widget, "press")`,
     `bindings.On(id, verb, handler)` and inside a handler `bindings.Send`, `SendLatest` (drags),
     `Back(exit)`, `EditText` / `AskText`, `Read`, `Rebuild()`, `SendHost(node, value)` (a setting of
     the application). Declare every modal the view shows with `bindings.Modal(id, cancel)`, topmost
     last; `cancel` must do what the modal's ✕ / «Отмена» does, `nullptr` makes it mandatory (scrim
     tap and back key are refused and logged). Never bind `dismiss` by hand. Something else the back
     key closes first returns true from `OwnsBack()` and closes it in `Back()`. Mark expected events
     that need nothing with `bindings.Ignore(id, verb)`.
   Add the file to `CMakeLists.txt`.
3. **Registry.** Add `{"<interface id>", Create…}` to `kPorted` in `registry.cpp`.
4. **Tests.** Add a synthetic model to `tests/models/`, extend `tests/screens_check.cpp`
   (admission, the props that matter, every action mapping) and add every modal of the screen
   to the walk in `tests/modals_check.cpp`.

Element ids: an event carries the id of the nearest ancestor that has one. Components
inside lists report under the list's id with their own verb (`goods row 3`,
`goods change 150`); header parts report under `header` (`section ship`, `subject-next`),
its buttons as `header-back`, `header-close`. A subject switch with a list is `SubjectSwitch`
(controls.h); its sheet rows report as `header-subject-<i>`.

Verbs and what the engine receives (`EngineCommand`, queue line
`widget:<screen>\t<node>\t<index>\t<wire>`):

| verb | value | wire |
|---|---|---|
| `press`, `left`, `right`, any other | empty | the verb |
| `select`, `activate`, `open`, … | row identity | `verb@<opening>:<row id>` |
| `toggle` | `1` / `0` | the value |
| `edit` | the text | the value |
| `set` | slider position | `set@<opening>:<position>` |
| any, with `guard` | — | `guard@<opening>:<guard row>\|<wire>` |
| `back` without a node | — | `back` |

## What the router does for every screen

- **Admission.** All REQUIRED nodes present → the ported screen. A model missing one is held
  hidden for the first second of the opening; after that it is an error (missing nodes logged,
  a toast in debug builds) and the interface closes through the engine's back. Mark nodes
  REQUIRED only when every way the game opens the interface has them.
- **Extras.** Widgets a ported screen does not know become rows of the «Прочее» sheet (header «⋯»);
  texts become one note.
- **Busy until answered.** An event that sent something marks its element
  (`bindings.Pending(id)`) until the engine answers; no answer in 5 s is an error.
- **Incremental update.** Every revision patches the mounted screen in place; scroll offsets and
  pressed parts survive.
- **Nothing is dropped quietly.** Malformed models, a missing pack screen, an event without a
  binding and a state channel without a consumer are error lines and, with `Config::show_errors`,
  toasts.

## UI strings

Screens never contain player text of their own. Texts the game sends pass through; everything
else is a key of the pack's string table (`mobile-ui/design/strings/<lang>.json`):

- C++: `bindings.Text("trade.price")`; a key kept in a table goes through
  `StringKey("trade.store")` so the pack build still sees it;
- templates: `{{ tr('common.next') }}`.

Add a key to every `strings/<lang>.json` (the design build fails otherwise) with its zone prefix
(`trade.`, `crew.`, `common.`). The word lists in `controls.cpp` (`kExitLabels`, `kConfirmLabels`,
`kDangerWords`) recognise button roles in all six game languages; add a language's words there with
its string table, and a case to `Roles` in `tests/screens_check.cpp`.

## Game pictures in rows

`PictureOf(image)` (controls.h) turns a `pictures.ini` reference into a row image
(`atlas:GROUP/Picture`); `TextureOf(image)` gives `{texture, uv}` for
`<ui-game-image texture uv [fit="contain|cover"]>`, `MapViewOf` the props of `c-map-view`.
Pictures are resolved from the game root at run time; nothing of the game is bundled.

## State channels

`Host::SetState(channel, json)` carries game state outside interfaces. Implement
`StateConsumer` (`Channel()`, `OnState(json, router)`) and register it in
`CreateStateConsumers()`. The HUD channels are consumed in `../hud/states.cpp`; channel names
are listed in the engine's `tools/android/app_contract.json`.

## The HUD

`src/hud` draws the in-game HUD from state channels:

```
engine ──state JSON──▶ consumer (states.cpp) ──typed States──▶ Scene::Build ──View──▶ mount (ui_host.cpp)
engine ◀──"<verb>:<value>" (Action, screen "hud")── Hud::Send ◀── Input handlers ◀── component events
```

A scene builds a `View`: the layout scene of the pack's `hud-layout.json`, the props of its
elements by id (an element left out is not mounted), an optional sheet and elements to dim.
`Input::On(id, verb, handler)` binds component events. A layout element may add
`"behavior": "hold"` (reports `down` / `up`) or `"swipe"` (`data` → `data-swipe-*`);
`"toastsBelow": true` keeps toasts below a top status.

Adding a scene:

1. Add the channel structs to `States` and a strict consumer per channel to `states.cpp`.
2. Write `Scene` in `src/hud/<zone>.cpp`: `Build` sets `view.layout`, props and bindings; timed
   work in `Tick`, coverage acknowledgements in `Drawn` (only a drawn frame may hide the
   engine's own indicators), `Released` / `Suspended` for cancelled input and a lost screen,
   `Holds` when the engine reports no mode while something of the scene is open.
3. Add `{"<name>", modes, Create…}` to `kScenes` in `hud.cpp`.
4. Pack: elements in `hud-layout.json`, sheets as `screens/hud-…` parts.
5. Tests: synthetic states in `tests/states`, a scene check like `tests/hud_land_check.cpp`.

```sh
build/ui-host/storm_ui_shot --pack <app>/build/ui-pack --size 915x412 --density 2 --out land.png \
    --state gameplay=tests/states/gameplay-explore.json --state combat=tests/states/combat-explore.json \
    --state vitals=tests/states/vitals.json --script "wants 96 329; down 1 96 329; move 1 96 280; wait 0.3"
```

## Trying a screen on the build host

```sh
python3 <app>/mobile-ui/design/build.py                       # the pack
cmake --build build/ui-host && ctest --test-dir build/ui-host --output-on-failure
build/ui-host/storm_ui_shot --pack <app>/build/ui-pack \
    --interface src/libs/ui/tests/models/store-deal.json --size 915x412 --density 2 \
    --out store.png --script "down 1 600 216; up 1 600 216; wait 0.3; shot row.png"
```

`--state dialogue=file.json` feeds a state channel instead; script commands
`interface <json>` (the next revision), `text <answer>` (accept the editor request).
Engine actions are printed as `command widget:store<TAB>TABLE_LIST<TAB>1<TAB>activate@7:…`.
Render at 780×360, 824×412, 961×412, 1024×768 and 1280×800 (`--density 1`) before
calling a layout done. Models in `tests/models` are synthetic; never commit a model
recorded from a game with its texts unless it holds nothing but interface strings.
