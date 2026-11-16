#!/usr/bin/env python3
"""Queue boot selection: once on a passing stack; own diff only during failure isolation.
No builds, game data, network or queue mutations: run the real orchestration with mocked stages.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/ci'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, root / f'tools/ci/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


land, gate = load('land'), load('gate')


class QueueBoot(unittest.TestCase):
    def batch(self, entries, outcomes, conflict=None, moved=False):
        commands, finished, pushes = [], [], []
        tip = 'base'

        def apply(entry):
            nonlocal tip
            if entry['id'] == conflict:
                return False, 'conflict'
            tip += '-' + entry['id']
            return True, ''

        def prepare():
            nonlocal tip
            tip = 'base'
            return tip

        def git(*args, **kwargs):
            nonlocal tip
            if args[:2] == ('rev-parse', 'HEAD'):
                return types.SimpleNamespace(stdout=tip)
            if args[0] == 'checkout':
                tip = args[-1]
                return
            raise AssertionError(args)

        def run(command, **kwargs):
            commands.append(command)
            return types.SimpleNamespace(returncode=outcomes[len(commands) - 1], stdout='gate result\n', stderr='')

        with tempfile.TemporaryDirectory() as temp, \
                patch.object(land, 'log_root', Path(temp)), patch.object(land, 'land_tree', Path(temp)), \
                patch.object(land, 'say'), patch.object(land, 'prepare_tree', side_effect=prepare), \
                patch.object(land, 'apply', side_effect=apply), patch.object(land, 'git', side_effect=git), \
                patch.object(land, 'finish', side_effect=lambda e, *a, **k: finished.append(e['id'])), \
                patch.object(land, 'push', side_effect=lambda e, b: pushes.append([x['id'] for x in e]) or
                             ('moved' if moved else 'pushed')), \
                patch.object(land, 'gate_env', return_value={}), patch.object(land.subprocess, 'run', side_effect=run):
            land.run_batch(entries)
        self.assertTrue(all(c[c.index('--boot') + 1] == 'auto' for c in commands))
        return commands, finished, pushes

    def test_boot_directory_logs_are_copied_without_game_work(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            tree, logs = temp / 'tree', temp / 'logs'
            boot = tree / 'build/gate/boot'
            (boot / 'work').mkdir(parents=True)
            (boot / 'report.json').write_text('report')
            (boot / 'work' / 'disposable-game').write_text('game')
            (tree / 'build/gate/boot.log').write_text('boot passed')
            result = types.SimpleNamespace(returncode=0, stdout='passed', stderr='')
            with patch.object(land, 'land_tree', tree), patch.object(land, 'log_root', logs), \
                    patch.object(land, 'say'), patch.object(land, 'gate_env', return_value={}), \
                    patch.object(land.subprocess, 'run', return_value=result):
                self.assertTrue(land.gate(ENTRIES, 'batch')[0])
            for entry in ENTRIES:
                target = logs / entry['id'] / 'batch'
                self.assertEqual((target / 'boot/report.json').read_text(), 'report')
                self.assertEqual((target / 'boot.log').read_text(), 'boot passed')
                self.assertFalse((target / 'boot/work').exists())

    def test_passing_batch_gates_once(self):
        commands, failed, pushes = self.batch(ENTRIES, [0])
        self.assertEqual(len(commands), 1)
        self.assertNotIn('--boot-base', commands[0])
        self.assertEqual(failed, [])
        self.assertEqual(pushes, [['runtime', 'docs']])
        self.assertEqual(commands[0][-3:], ['--tests', 'extra_a', 'extra_b'])

    def test_failed_batch_uses_each_entry_diff(self):
        commands, failed, pushes = self.batch(ENTRIES, [1, 0, 0])
        self.assertEqual(len(commands), 3)
        self.assertEqual([c[c.index('--boot-base') + 1] for c in commands[1:]], ['base', 'base-runtime'])
        self.assertEqual(pushes, [['runtime', 'docs']])
        self.assertEqual(failed, [])

    def test_failed_entry_is_removed_from_next_tip(self):
        commands, failed, pushes = self.batch(ENTRIES, [1, 1, 0])
        self.assertEqual([c[c.index('--boot-base') + 1] for c in commands[1:]], ['base', 'base'])
        self.assertEqual(failed, ['runtime'])
        self.assertEqual(pushes, [['docs']])

    def test_single_failed_entry_does_not_retry(self):
        commands, failed, pushes = self.batch(ENTRIES[:1], [1])
        self.assertEqual(len(commands), 1)
        self.assertEqual(failed, ['runtime'])
        self.assertEqual(pushes, [])

    def test_conflicting_entry_never_gated(self):
        commands, failed, pushes = self.batch(ENTRIES, [0], conflict='runtime')
        self.assertEqual(len(commands), 1)
        self.assertEqual(failed, ['runtime'])
        self.assertEqual(pushes, [['docs']])

    def test_moved_base_returns_for_new_batch(self):
        commands, _, _ = self.batch(ENTRIES, [0], moved=True)
        self.assertEqual(len(commands), 1)


class QueueEnv(unittest.TestCase):
    def env(self, text, environ=None):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            vgl = temp / 'vgl'
            (vgl / 'usr/lib').mkdir(parents=True)
            (vgl / 'usr/lib/libvglfaker.so').touch()
            env_file = temp / 'env'
            if text is not None:
                env_file.write_text(text.replace('VGL', str(vgl)))
            with patch.object(land, 'env_file', env_file), patch.dict('os.environ', environ or {}, clear=True):
                return land.gate_env(), str(vgl)

    def test_file_sets_virtualgl(self):
        env, vgl = self.env('# queue settings\n\nSTORM_VIRTUALGL=VGL\n')
        self.assertEqual(env['STORM_VIRTUALGL'], vgl)

    def test_file_overrides_runner(self):
        env, vgl = self.env('STORM_VIRTUALGL=VGL\n', {'STORM_VIRTUALGL': '/elsewhere', 'KEEP': '1'})
        self.assertEqual((env['STORM_VIRTUALGL'], env['KEEP']), (vgl, '1'))

    def test_missing_virtualgl_is_refused(self):
        for text, environ in ((None, {}), ('OTHER=1\n', {}), ('STORM_VIRTUALGL=/nonexistent\n', {})):
            with self.subTest(text=text), self.assertRaisesRegex(RuntimeError, 'STORM_VIRTUALGL'):
                self.env(text, environ)

    def test_malformed_line_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'expected NAME=VALUE'):
            self.env('STORM_VIRTUALGL=VGL\nnonsense\n')


ENTRIES = [dict(id='runtime', label='runtime', sha='abc', tests=['extra_a']),
           dict(id='docs', label='docs', sha='def', tests=['extra_b'])]


class GateBoot(unittest.TestCase):
    def run_gate(self, flags, own_runtime=False, batch_runtime=True):
        calls, boots = [], []

        def changed(base):
            calls.append(base)
            runtime = batch_runtime if base == 'origin/develop' else own_runtime
            return {'runtime': 'M'} if runtime else {}

        def plan(files, *args):
            return dict(host_checks={}, game_checks={}, ui_host=(False, 'test'), android=(False, 'test'),
                        boot=(bool(files), 'runtime' if files else 'no runtime changes'))

        with tempfile.TemporaryDirectory() as temp, patch.object(gate, 'logs', Path(temp)), \
                patch.object(sys, 'argv', ['gate.py', '--no-fetch', *flags]), \
                patch.object(gate, 'git', return_value='abc'), patch.object(gate.affected, 'changed_files', side_effect=changed), \
                patch.object(gate.affected, 'plan', side_effect=plan), \
                patch.object(gate, 'stage_boot', side_effect=lambda: boots.append(1) or True), \
                patch.dict('os.environ', {'STORM_VIRTUALGL': '/test'}), contextlib.redirect_stdout(io.StringIO()):
            gate.main()
        return calls, boots

    def test_local_default_off_even_with_full(self):
        for flags in ([], ['--full']):
            self.assertEqual(self.run_gate(flags)[1], [])

    def test_explicit_on(self):
        self.assertEqual(self.run_gate(['--boot', 'on'])[1], [1])

    def test_auto_batch(self):
        self.assertEqual(self.run_gate(['--boot', 'auto']), (['origin/develop'], [1]))

    def test_auto_documentation_batch_skips_boot(self):
        self.assertEqual(self.run_gate(['--boot', 'auto'], batch_runtime=False)[1], [])

    def test_auto_own_diff_docs_after_runtime_does_not_boot(self):
        self.assertEqual(self.run_gate(['--boot', 'auto', '--boot-base', 'previous']),
                         (['origin/develop', 'previous'], []))

    def test_boot_base_only_narrows_auto(self):
        for flags in (['--boot', 'on'], ['--boot', 'off'], ['--boot', 'auto', '--full']):
            with self.subTest(flags=flags), self.assertRaises(SystemExit), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.run_gate([*flags, '--boot-base', 'previous'])

    def test_auto_own_runtime_diff_boots(self):
        self.assertEqual(self.run_gate(['--boot', 'auto', '--boot-base', 'previous'], True)[1], [1])


if __name__ == '__main__':
    unittest.main()
