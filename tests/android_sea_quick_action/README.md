# Sea quick action contract checks

Run `python3 tests/android_sea_quick_action/run.py` on the host (ASan/UBSan).

`ILogAndActions` offers one quick action at sea: Map, Moor, Board, LandTroops or Reload.
`readSeaQuickAction` (battle_interface) binds it to the main ship and to the identity of
its target; `SeaQuickActionState` publishes `{schemaVersion, activity, revision, available,
action, label}` (state channel `seaQuickAction`) and takes
`seaquick:1\t<activity>\t<revision>\tack\t<0|1>` and `…\tactivate\t<action>`.

Android never draws the legacy sea HUD, including before a presenter declaration or
following `hudowner:lost`. Runtime failures are logged as errors; the touch HUD must
recover itself. Ownership and revision answers still gate touch actions: a tap is
accepted once per revision for the drawn action only. Another target, owner or Activity
makes a new revision, so duplicate and stale taps do nothing.
