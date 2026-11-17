#!/usr/bin/env python3
"""The engine side of the Android application contract (tools/android/app_contract.json): the engine sources
accept, send, raise, export and look up exactly what the manifest says. The application's
tests/engine_contract checks its Java and interface scripts against the same file.

Read from the sources:
- src/apps/engine/src/main.cpp: the command dispatcher (command.starts_with / command == literals);
- src/libs/ui/src: the HUD's Send verbs, the screens' EngineCommand literals (router.cpp), SendHost nodes,
  state channel consumers, the settings screen (menu_settings.cpp: Segment, DebugSegment, the host facts it erases);
- src/libs/ui_host/src/engine_ui.cpp: the HUD verbs handed to the activity;
- src/libs/util/include/platform/android_shortcuts.hpp: the shortcuts the native queue takes, their screens;
- src/ and android/: script events raised (Event("Android..."), JNI exports (Java_org_stormengine_port_*),
  JNI upcalls (GetMethodID / GetStaticMethodID / CallActivity).
No compiler, no Android: a text check, seconds.
"""
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / 'tools/android/app_contract.json').read_text())
problems = []

# Methods of the Android framework and java.lang the engine looks up; not the application's.
PLATFORM_LOOKUPS = {'getClassLoader', 'loadClass', 'getAssets', 'getApplicationInfo', 'getWindowManager',
                    'getDefaultDisplay', 'getRefreshRate', '<init>'}


def text(path):
    return (root / path).read_text(encoding='utf-8', errors='replace')


def literal(value):
    return value.encode('utf-8').decode('unicode_escape')


def sources(*parts, suffixes=('.cpp', '.hpp', '.h', '.c', '.inc')):
    for part in parts:
        for path in sorted((root / part).rglob('*')):
            if path.suffix in suffixes and path.is_file():
                yield path


def same(label, found, declared):
    found, declared = set(found), set(declared)
    for item in sorted(found - declared, key=str):
        problems.append(f'{label}: {item!r} is in the sources but not in tools/android/app_contract.json')
    for item in sorted(declared - found, key=str):
        problems.append(f'{label}: {item!r} is in tools/android/app_contract.json but not in the sources')


# Commands: the dispatcher accepts exactly the manifest's commands.
main = text('src/apps/engine/src/main.cpp')
dispatched = {literal(v) for v in re.findall(r'command\.starts_with\("([^"]+)"\)', main)}
dispatched |= {literal(v) for v in re.findall(r'command == "([^"]+)"', main)}
commands = manifest['engine_commands']
same('engine command (main.cpp dispatcher)', dispatched, commands)
if 'unknown engine command' not in main:
    problems.append('main.cpp: the command dispatcher has no error for an unknown command')

# What the UI library sends: HUD verbs become "<verb>:<value>" (EngineCommand), except the ones handed to the
# activity; the screens' commands are literals of EngineCommand (router.cpp).
ui_dir = root / 'src/libs/ui/src'
hud_verbs = set()
for path in sources('src/libs/ui/src/hud'):
    hud_verbs |= set(re.findall(r'\bSend\("([a-z-]+)"', path.read_text(encoding='utf-8')))
host_input = manifest['hud_host_input']
router = text('src/libs/ui/src/screens/router.cpp')
engine_command = router[router.index('std::string EngineCommand('):router.index('bool AnsweredAction(')]
ui_sent = {verb + ':' for verb in hud_verbs - set(host_input)}
ui_sent |= {v for v in re.findall(r'"([a-z-]+:?)"', engine_command) if v.endswith(':') or v == 'back'}
for sent in sorted(ui_sent):
    if not any(sent.startswith(c) or c.startswith(sent) for c in commands):
        problems.append(f'the UI library sends {sent!r}, which no engine command of the manifest takes')
for command, sender in sorted(commands.items()):
    by_ui = any(command.startswith(s) or s.startswith(command) for s in ui_sent)
    if ('ui' in sender.split('+')) != by_ui:
        problems.append(f'engine command {command!r}: the manifest says sent by {sender!r}, the UI library '
                        f'{"sends" if by_ui else "does not send"} it')

# HUD input handed to the activity, and its values.
engine_ui = text('src/libs/ui_host/src/engine_ui.cpp')
handed = re.search(r'if \(action\.screen == "hud" && \(([^)]*)\)\)', engine_ui)
same('HUD input handed to the activity (engine_ui.cpp)', re.findall(r'action\.verb == "(\w+)"', handed[1]) if handed else [],
     host_input)
shortcuts = text('src/libs/util/include/platform/android_shortcuts.hpp')
same('shortcut the native queue takes (android_shortcuts.hpp)', re.findall(r'name == "(\w+)"', shortcuts), host_input['shortcut'])
sent_shortcuts = set()
presence = set()
for path in sources('src/libs/ui/src'):
    body = path.read_text(encoding='utf-8')
    sent_shortcuts |= set(re.findall(r'\bSend\("shortcut", "(\w+)"', body))
    for line in re.findall(r'\bSend\("presence", ([^;]*)\);', body):
        presence |= set(re.findall(r'"(\w+)"', line))
for name in sorted(sent_shortcuts - set(host_input['shortcut'])):
    problems.append(f'the HUD sends shortcut {name!r}, which tools/android/app_contract.json does not list')
same('HUD presence value', presence, host_input['presence'])

# Host actions and state channels.
host_actions = set()
channels = set()
for path in sources('src/libs/ui/src'):
    body = path.read_text(encoding='utf-8')
    host_actions |= set(re.findall(r'\bSendHost\("([\w-]+)"', body))
    channels |= set(re.findall(r'Channel\(\) const override\s*\{\s*return "(\w+)";', body))
same('host action (SendHost)', host_actions, manifest['host_actions'])
for host, written in manifest['state_channels'].items():
    for channel in written:
        if channel not in channels:
            problems.append(f'state channel {channel!r} ({host} host): no UI consumer has Channel() == {channel!r}')

# The settings screen.
settings_screen = text('src/libs/ui/src/screens/menu_settings.cpp')
settings = manifest['settings']
same('application setting (menu_settings.cpp Segment, Toggle)', re.findall(r'\b(?:Segment|Toggle)\(bindings, \w+, "(\w+)"', settings_screen),
     settings['keys'])
same('host fact (menu_settings.cpp, erased before «Готово» sends)', re.findall(r'settings\.erase\("(\w+)"\)', settings_screen),
     settings['host_facts'])
debug = {key: [int(v) for v in values.split(',')]
         for key, values in re.findall(r'DebugSegment\(bindings,\s*debug,\s*"(\w+)",\s*"[^"]*",\s*\{([^}]*)\}', settings_screen)}
same('debug override (menu_settings.cpp DebugSegment)', debug, settings['debug'])
for key, values in sorted(debug.items()):
    if key in settings['debug'] and settings['debug'][key] != values:
        problems.append(f'debug override {key!r}: the screen offers {values}, the manifest says {settings["debug"][key]}')
outside_ui = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in sources('src')
                       if not p.is_relative_to(root / 'src/libs/ui'))
for key in sorted(set(settings['debug']) | set(settings['keys'])):
    if f'"{key}"' not in outside_ui:
        problems.append(f'setting {key!r}: the settings screen offers it but no engine code outside the UI reads it')

# Script events the application's interface scripts handle.
raised = set()
for path in sources('src'):
    raised |= set(re.findall(r'\bEvent\(\s*"(Android\w*)"', path.read_text(encoding='utf-8', errors='replace')))
same('script event raised for the application (Event("Android..."))', raised, manifest['script_events'])
screen_function = shortcuts[shortcuts.index('inline const char *shortcutScreen('):shortcuts.index('class GameplayShortcuts')]
same('AndroidOpenScreen argument (shortcutScreen)', re.findall(r'return "(\w+)";', screen_function),
     manifest['script_events'].get('AndroidOpenScreen', []))

# JNI: exports and upcalls.
exports, lookups = set(), set()
for path in sources('src', 'android'):
    body = path.read_text(encoding='utf-8', errors='replace')
    exports |= set(re.findall(r'\bJava_org_stormengine_port_([A-Za-z0-9]+)_([A-Za-z0-9]+)\s*\(', body))
    lookups |= set(re.findall(r'Get(?:Static)?MethodID\(\s*[^,()]+,\s*"([\w<>]+)",\s*"([^"]+)"\s*\)', body))
    lookups |= set(re.findall(r'\bCallActivity\("(\w+)",\s*"([^"]+)"', body))
same('JNI export', exports, {(cls, name) for cls, names in manifest['jni']['natives'].items() for name in names})
same('JNI upcall', {lookup for lookup in lookups if lookup[0] not in PLATFORM_LOOKUPS},
     {(upcall[1], upcall[2]) for upcall in manifest['jni']['upcalls']})

if problems:
    sys.exit('APP CONTRACT: the engine sources and tools/android/app_contract.json differ:\n  ' + '\n  '.join(problems))
print(f'PASS: app contract: {len(commands)} engine commands, {len(host_input["shortcut"])} shortcuts, '
      f'{len(settings["debug"])} debug overrides, {len(manifest["script_events"])} script events, '
      f'{sum(map(len, manifest["jni"]["natives"].values()))} JNI exports, {len(manifest["jni"]["upcalls"])} upcalls '
      'match the engine sources')
