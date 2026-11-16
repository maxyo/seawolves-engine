#!/usr/bin/env python3
"""Boot sweep orchestration checks, without starting an engine or a display server."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import threading
import types

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('boot_sweep', root / 'tools/scene_sweep/run.py')
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


def check_selection():
    sea = sweep.inventory.Scene('sea:Martinique', 'sea')
    tavern = sweep.inventory.Scene('loc:FortFrance_tavern', 'tavern')
    arguments = types.SimpleNamespace(only=[sea.id, tavern.id], single_session=True,
                                     smoke=False, failed=False, changed=False)
    assert sweep.select([tavern, sea], arguments, None, None) == [sea, tavern]


def check_no_retry():
    calls, recorded = [], []
    shard = types.SimpleNamespace(queue=['sea:Martinique', 'loc:FortFrance_tavern'],
                                  context={'stop': threading.Event()},
                                  arguments=types.SimpleNamespace(single_session=True), boot_failures=0)

    def run_engine():
        calls.append('engine')
        shard.queue.pop(0)  # an engine that dies during the sea scene

    shard.run_engine = run_engine
    shard.record = lambda scene, reasons: recorded.append((scene, reasons))
    sweep.Shard.sweep(shard)
    assert len(calls) == 1
    assert not shard.queue
    assert recorded[0][0] == 'loc:FortFrance_tavern'
    assert sweep.status_of(*recorded[0]) == 'fail'


def check_failure_logs():
    for filename, line, reason in (
            ('stdout', 'ERROR: AddressSanitizer: heap-use-after-free', 'sanitizer'),
            ('stdout', 'runtime error: signed integer overflow', 'sanitizer'),
            ('stdout', 'Engine stopped: fatal', 'fatal engine error'),
            ('error.log', 'RUNTIME ERROR in test.c(42)', 'script error'),
            ('system.log', "Can't load texture resource\\textures\\missing.tx", 'missing texture'),
            ('system.log', "Can't load model missing.gm", 'missing model'),
            ('system.log', 'Cannot open animation file missing.ani', 'missing resource'),
            ('system.log', 'Warning! Can`t open ini file resource/test.ini', 'missing resource'),
            ('system.log', "Audio decode failed 'missing.wav'", 'missing resource')):
        reasons = sweep.log_reasons([(filename, line)])
        assert reasons and reasons[0].startswith(reason), (line, reasons)
        assert sweep.status_of('menu:main', reasons) == 'fail'


def check_final_logs():
    with tempfile.TemporaryDirectory(prefix='ci-boot-sweep-') as temporary:
        directory = Path(temporary)
        control = directory / 'control'
        control.mkdir()
        (control / 'events.log').write_text('1 done\n')
        stdout = directory / 'stdout.log'
        # Exit 0 with a fatal message after the last capture, including no trailing newline.
        stdout.write_text('Engine stopped: final failure')
        recorded = []
        shard = types.SimpleNamespace(arguments=types.SimpleNamespace(save=True),
                                      display_server=types.SimpleNamespace(poll=lambda: None),
                                      record=lambda scene, reasons: recorded.append((scene, reasons)))
        engine = types.SimpleNamespace(poll=lambda: 0, returncode=0)
        sweep.Shard.watch(shard, engine, directory, control, stdout)
        assert recorded[0][0] == 'exit:shutdown'
        assert sweep.status_of(*recorded[0]) == 'fail'
        assert 'fatal engine error' in recorded[0][1][0]
        engine.returncode = -11
        recorded.clear()
        sweep.Shard.watch(shard, engine, directory, control, stdout)
        assert any('crash at engine shutdown: killed by signal SIGSEGV' in reason for reason in recorded[0][1])
        engine.returncode = 0
        stdout.write_text('runtime error: final undefined behaviour')
        recorded.clear()
        sweep.Shard.watch(shard, engine, directory, control, stdout)
        assert sweep.status_of(*recorded[0]) == 'fail'
        assert 'sanitizer' in recorded[0][1][0]


check_selection()
check_no_retry()
check_final_logs()
check_failure_logs()
print('PASS boot sweep order, single session, and final fatal/sanitizer logs')
