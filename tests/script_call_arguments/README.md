# Calls with a wrong argument count

```sh
python3 tools/ci/run_host_tests.py --only script_call_arguments
```

A call inside an expression was never checked against the function's argument count (the statement form refuses
it; the expression check is commented out in the original engine), and TEHO has such calls, e.g.
`Link.l1 = RandPhraseSimple("...", "...", "...")` (`Enc_Officer_dialog.c`, two declared). At run time the script
function's frame starts at its own last arguments, the call ends with `function 'X' stack error`, and the first
arguments stayed on the stack under the result, so the caller's expression read the wrong operands
(`"a" + F(x, y, z)` adds the result to x). Too few arguments take the caller's own stack values.

- `COMPILER::CheckCallArguments` writes `COMPILE WARNING in <file>(<line>): function 'X' declared with N arguments is
  called with M` to the error log for such a call (script functions, fixed-count internal ones; not imported
  functions or functions of unloaded segments) and compiles it as before.
- `COMPILER::DropStrayStackValues` drops the values a call left above its result after the stack error.

The runner extracts both verbatim from `compiler.cpp` and runs them under ASan/UBSan with the real `FuncInfo`,
`S_STACK` and `DATA`.
