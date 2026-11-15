# Android sea navigator contract checks

Run `python3 tests/android_sea_navigator/run.py` on the host. The ASan/UBSan
check exercises the same projection, charge/damage composition and zoom helpers
used by the native renderer, and the real layout/zoom mailbox parser.

The app prepares a measured circular aperture in Android surface pixels (safe
insets are included in its center), density, cardinal language and layout id. Preparation
is not a draw ACK: the native renderer echoes that id only after drawing the real
map; only then can Android expose the opaque panel and acknowledge visible text.
Matching-aspect internal render scales use a measured uniform conversion. The mailbox
binds that layout to gameplay revision, HUD/Activity revision and native
island/resource generation; it lasts until that owner changes or Android clears it
(no lease: the UI says it once per owner and place, however slow its frames).
Hidden/detached Android views clear it. The native compass is never compiled on
Android; a missing aperture cannot bring it back. A zoom request requires the same current layout and uses the original
native scale bounds. A new viewport must match the measured render size.

Native keeps the actual island render target, ship list and ship-relative world
projection. Four side attributes come from `Cannons.Borts.cannonf/r/b/l`;
`ChargeRatio` is a fraction of the undamaged part, so the ready arc is
`charge * (1 - damage)`. Missing sides draw unknown tracks, not ready cannons.
Unsupported wind display states make the touch navigator unavailable; they never
restore the desktop compass.

The `cannon-loadball-v1` powder adapter implements the verified stock
`AICannon::Cannon_LoadBall` reload prerequisite: `GetCargoGoods(GOOD_POWDER)>0`.
It is published separately from cargo count, with current ship ownership and
optional validity. The UI does not infer warning state. Stock `GetCurrentCharge`
assigns only slots 0–2 of a shared result array; slots 3/4 are not fresh powder
signals and are deliberately never exported. Ammo warning retains its original
`SetEnoughBalls` event, and the full localized ammo caption is read from the
battle interface's refreshable `Ammo` text.

Enable `log.tag.StormInput=DEBUG` to trace native navigator source readiness,
resource generation and touch radar readiness. Any visible legacy compass is a
failed visual acceptance.
Host checks and compilation cannot establish visual acceptance: test the exact
APK at both logical sizes, warning/ordnance states, contacts, zoom guards,
scene/menu/Activity transitions and inspect actual screenshots independently.
