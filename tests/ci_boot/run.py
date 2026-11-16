#!/usr/bin/env python3
"""Boot stage launches one sweep and requires fresh complete evidence; budget failure is explicit."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('boot_stage', root / 'tools/ci/boot_stage.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class Boot(unittest.TestCase):
    def run_boot(self, report, code=0, timeout=False, stale=False):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp / 'engine').touch()
            (temp / 'save').touch()
            lib = temp / 'usr/lib'
            lib.mkdir(parents=True)
            for name in ('libdlfaker.so', 'libvglfaker.so'):
                (lib / name).touch()
            out = temp / 'out'
            out.mkdir()
            if stale:
                (out / 'report.json').write_text(json.dumps(COMPLETE))
            process = Mock(pid=123)
            process.wait.side_effect = [subprocess.TimeoutExpired('sweep', 1), 0] if timeout else [code]

            def launch(command, **kwargs):
                self.assertIn('--single-session', command)
                self.assertEqual(command[command.index('--jobs') + 1], '1')
                self.assertEqual(command[command.index('--only') + 1:command.index('--jobs')], list(boot.SCENES))
                self.assertFalse((out / 'report.json').exists(), 'stale report was reused')
                if report is not None:
                    (out / 'report.json').write_text(json.dumps(report))
                return process

            argv = ['boot_stage.py', '--no-build', '--build', str(temp), '--out', str(out),
                    '--game', str(temp), '--save', str(temp / 'save'), '--budget', '1']
            with patch.object(sys, 'argv', argv), patch.dict('os.environ', {'STORM_VIRTUALGL': str(temp)}), \
                    patch.object(boot.subprocess, 'Popen', side_effect=launch) as popen, \
                    patch.object(boot.os, 'killpg') as kill, contextlib.redirect_stdout(io.StringIO()):
                try:
                    boot.main()
                finally:
                    self.assertEqual(popen.call_count, 1)
                    if timeout:
                        kill.assert_called_once_with(123, boot.signal.SIGTERM)

    def test_complete(self):
        self.run_boot(COMPLETE)

    def test_no_report_even_with_stale_success(self):
        with self.assertRaisesRegex(SystemExit, 'no report'):
            self.run_boot(None, stale=True)

    def test_missing_scene(self):
        with self.assertRaisesRegex(SystemExit, 'required scenes'):
            self.run_boot({'scenes': {'menu:main': ENTRY}, 'meta': COMPLETE['meta']})

    def test_session_count_required(self):
        for count in (None, 0, 2):
            with self.subTest(count=count), self.assertRaisesRegex(SystemExit, 'exactly one engine session'):
                self.run_boot({**COMPLETE, 'meta': {'engine_sessions': count}})

    def test_nonzero_exit(self):
        with self.assertRaisesRegex(SystemExit, 'exit 1'):
            self.run_boot(COMPLETE, code=1)

    def test_budget(self):
        with self.assertRaisesRegex(SystemExit, 'time budget'):
            self.run_boot(None, timeout=True)


ENTRY = dict(status='pass', reasons=[])
COMPLETE = {'meta': {'engine_sessions': 1}, 'scenes': {name: ENTRY for name in ('menu:main', 'save:boot', *boot.SCENES)}}
if __name__ == '__main__':
    unittest.main()
