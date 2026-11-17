# A deleted attribute behind a script aref

```sh
python3 tools/ci/run_host_tests.py --only script_dangling_aref
```

An `aref` is a pointer to an attribute; scripts delete attributes behind arefs (`DeleteAttribute`, an object
replaced by a copy, a local object at the end of its function). reload.c keeps the quest reload's fake locator
in the global `aref reload_locator_ref`; `DoDeleteFakeLocation` deletes it when the location has loaded. When a
second quest reload starts before that (the scene sweep forces one: NEW-PRE-SWEEP-UAF), its `ReloadEndFade` read
the freed locator: an intermittent SIGSEGV, a heap-use-after-free under ASan.

Every attribute holds a slot of a generation table (`ATTRIBUTES::Handle`); destroying it advances the slot's
generation. An aref keeps the handle of its attribute, so a use of an aref whose attribute is gone is a script error
naming it and the aref reads as empty, whatever now occupies the memory or the slot. A posted event checks its
attribute arguments the same way when its time comes, and an entity the attributes it was created with (the boarding
fader of AIAbordage.c is created on a local `object`: `GetEventData()` of its fade events gave an aref to the freed
object; now an empty aref with the entity id, one warning per entity). A save writes such an aref empty with one warning and clears
it (`tests/save_dangling_references`).

The check runs the real `DATA`, `ATTRIBUTES`, `MESSAGE` and the compiler's `AddPostEvent` /
`ForgetDeletedAttributes` (verbatim from `compiler.cpp`) under ASan/UBSan: the reload.c case with the memory reused,
copies of a dangling aref, a branch replaced by a copy, a destroyed object, a posted event; live arefs unchanged.
