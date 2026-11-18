#!/usr/bin/env python3
"""Device crawl of the game's touch UI: tap what a screen draws and say what each tap did.

The app logs its tap map when `log.tag.StormUiTaps` is DEBUG (storm_ui Config::log_taps): one "taps" block in
logcat (tag StormUi) each time what can be tapped changes, a line per touch region: where (pack screen, "hud",
"app:<screen>"), component, element id, verb, box, the point a tap uses ("at"), whether a tap there reaches it,
disabled, text. This tool reads that map, taps through adb and reports, per tap, what the map became and the
errors logged meanwhile (StormUi errors such as "no mapping", missing icons, script and engine errors).

  ui_crawl.py -s emulator-5906 taps                     current map
  ui_crawl.py -s emulator-5906 tap ID[#N] | "~text" | X,Y   tap one region (N: the N-th region with that id)
  ui_crawl.py -s emulator-5906 hold SELECTOR                  press and hold (hold-to-confirm)
  ui_crawl.py -s emulator-5906 back                     hardware Back
  ui_crawl.py -s emulator-5906 crawl --out DIR [--skip REGEX] [--only REGEX] [--no-back]
        tap every reachable enabled region of the current screen in turn; after a tap that leaves the screen
        or covers what is still to tap, Back must return to it (else the crawl stops there). --each ID: per tab. One JSON line per tap in DIR/crawl.jsonl,
        a screenshot per new state.
  ui_crawl.py -s emulator-5906 shot FILE.png
  ui_crawl.py -s emulator-5906 swipe X1,Y1 X2,Y2              drag (scroll), then the map
  ui_crawl.py -s emulator-5906 wait REGEX [--timeout S]   until the screen name matches
  ui_crawl.py -s emulator-5906 flicker DIR...            every tap of the crawls in DIR/crawl.jsonl that showed a
        transient frame, ranked by how visible it was (time on the screen x share of the screen)

The flicker check (storm_ui Config::trace_actions, on with the tap map and in debug builds): each tap's report carries
the host's "UI flicker" lines (frames that showed neither the state before the tap nor the one it settled in) and
"UI action" lines (the tap settled without one); `tap` and `crawl` print them, crawl.jsonl keeps them.

Set the property before the app starts:  adb shell setprop log.tag.StormUiTaps DEBUG
Taps change the game: crawl on a throwaway device or emulator overlay (emulator -read-only).
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

LINE = re.compile(r'^(?P<where>\S+) (?P<tag>\S+) (?P<id>\S+) (?P<verb>\S+) box=(?P<box>-?\d+,-?\d+,\d+,\d+) at=(?P<at>\S+) '
                  r'reach=(?P<reach>[01]) dis=(?P<dis>[01]) hold=(?P<hold>[01]) st=(?P<st>[0-9a-f]+) "(?P<text>[^"]*)"$')
# Logged problems worth a look after a tap. StormUi errors are the UI's own (no mapping, missing icon, broken
# component); script errors come through the engine log.
PROBLEM = re.compile(r'no mapping|missing icon|is drawn with no mapping|script error|COMPILE ERROR|Undefined|'
                     r'not found|threw|failed|FATAL|crash', re.I)
# The flicker check's lines (storm_ui action_trace.cpp DescribeTrace).
TRACE = re.compile(r'UI (?P<kind>flicker|action): (?P<action>.*?): (?:(?P<transient>\d+) transient frames, (?P<ms>[\d.]+) ms, '
                   r'(?P<area>[\d.]+)% of the screen|no transient frames) \((?P<before>\S+(?: \S+)?) -> (?P<after>.*?), settled in '
                   r'(?P<settle>[\d.]+) ms, (?P<frames>\d+) frames(?P<flags>[^)]*)\)(?P<details>.*)$')
NOISE = re.compile(r'frame pacing: the display stayed|force finish cuj|hid_init threw|Failed to open rendernode|Choreographer|OpenGLRenderer|EGL_emulation|goldfish|gralloc|ashmem|libprocessgroup|BpBinder|'
                   r'chatty|HostConnection|vulkan|AudioTrack|AudioFlinger|ziparchive|nativeloader', re.I)


class Device:
    def __init__(self, serial, package):
        self.serial, self.package = serial, package

    def adb(self, *args, check=True, binary=False):
        out = subprocess.run(['adb', '-s', self.serial, *args], capture_output=True, check=check)
        return out.stdout if binary else out.stdout.decode('utf-8', 'replace')

    def logcat(self):
        return self.adb('logcat', '-d', '-v', 'threadtime').splitlines()

    def clear(self):
        self.adb('logcat', '-c')

    def pids(self):
        out = self.adb('shell', 'pidof', self.package, self.package + ':game', check=False).split()
        return set(out)

    def size(self):
        if not hasattr(self, '_size'):
            match = re.search(r'(\d+)x(\d+)', self.adb('shell', 'wm', 'size').strip().splitlines()[-1])
            w, h = int(match.group(1)), int(match.group(2))
            self._size = (max(w, h), min(w, h))  # landscape
        return self._size

    def top_pid(self):
        """The process of the resumed activity: the game's screens are in :game, the launcher's in the main one."""
        top = self.adb('shell', 'dumpsys', 'activity', 'activities', check=False)
        match = re.search(r'topResumedActivity=\S+ \S+ (\S+)', top)
        name = self.package + (':game' if match and 'GameActivity' in match.group(1) else '')
        pid = self.adb('shell', 'pidof', name, check=False).split()
        return pid[0] if pid else None

    def tap(self, x, y):
        self.adb('shell', 'input', 'tap', str(x), str(y))

    def hold(self, x, y, ms=1600):
        self.adb('shell', 'input', 'swipe', str(x), str(y), str(x), str(y), str(ms))

    def back(self):
        self.adb('shell', 'input', 'keyevent', '4')

    def shot(self, path):
        Path(path).write_bytes(self.adb('exec-out', 'screencap', '-p', binary=True))


def blocks(lines, pid=None):
    """Complete taps blocks of process `pid` (any when None) in log order: list of (block lines as dicts)."""
    found, current = [], None
    for raw in lines:
        at = raw.find(' StormUi : taps ')
        if at < 0 or (pid is not None and raw.split(None, 3)[2:3] != [pid]):
            continue
        body = raw[at + len(' StormUi : taps '):]
        if body.startswith('begin '):
            current = []
        elif body.startswith('end '):
            if current is not None:
                found.append(current)
            current = None
        elif current is not None:
            match = LINE.match(body.split(' ', 1)[1].lstrip() if ' ' in body else '')
            if match:
                region = match.groupdict()
                region['box'] = [int(v) for v in region['box'].split(',')]
                region['at'] = None if region['at'] == '-' else [int(v) for v in region['at'].split(',')]
                region['reach'] = region['reach'] == '1'
                region['dis'] = region['dis'] == '1'
                region['hold'] = region['hold'] == '1'
                current.append(region)
    for regions in found:
        seen = {}
        for region in regions:
            seen[region['id']] = seen.get(region['id'], 0) + 1
            region['n'] = seen[region['id']]
            region['name'] = region['id'] + ('#%d' % region['n'] if region['n'] > 1 else '')
    return found


def screen_of(regions):
    names = sorted({r['where'] for r in regions if r['where'] not in ('hud', 'layer')})  # layer: toasts over it
    return '+'.join(names) if names else ('hud' if regions else 'nothing')


def signature(regions):
    return tuple((r['where'], r['tag'], r['id'], r['verb'], r['text'], r['st'], r['dis']) for r in regions)


def problems(lines, pids):
    out = []
    for raw in lines:
        parts = raw.split(None, 6)
        if len(parts) < 7 or (pids and parts[2] not in pids) or NOISE.search(raw):
            continue
        level, message = parts[4], parts[6]
        if ' taps ' in message:
            continue
        if level in ('E', 'F') or PROBLEM.search(message):
            out.append(level + ' ' + message.strip())
    return out


def traces(lines, pids):
    """The flicker check's reports in `lines` (the host's "UI flicker" / "UI action" lines)."""
    out = []
    for raw in lines:
        parts = raw.split(None, 6)
        if len(parts) < 7 or (pids and parts[2] not in pids):
            continue
        at = parts[6].find('UI flicker: ')
        at = at if at >= 0 else parts[6].find('UI action: ')
        match = TRACE.match(parts[6][at:].strip()) if at >= 0 else None
        if not match:
            continue
        out.append({'action': match['action'], 'transient': int(match['transient'] or 0), 'ms': float(match['ms'] or 0),
                    'area': float(match['area'] or 0) / 100.0, 'before': match['before'], 'after': match['after'],
                    'settle_ms': float(match['settle']), 'frames': int(match['frames']), 'flags': match['flags'].strip(', '),
                    'details': [d.strip() for d in match['details'].split(' | ') if d.strip()]})
    return out


def settle(device, last_block_count=0, quiet=1.5, timeout=20.0):
    """Wait until no new taps block arrived for `quiet` s; return (log lines, newest block or None)."""
    start = time.time()
    seen, changed_at = -1, time.time()
    while True:
        lines = device.logcat()
        found = blocks(lines, device.top_pid())
        if len(found) != seen:
            seen, changed_at = len(found), time.time()
        if time.time() - changed_at >= quiet or time.time() - start >= timeout:
            return lines, (found[-1] if found else None)
        time.sleep(0.3)


def describe(region):
    return '%-10s %-18s %-20s %-8s at=%-9s %s%s "%s"' % (
        region['where'][:10], region['tag'], region['name'], region['verb'],
        '-' if not region['at'] else '%d,%d' % tuple(region['at']), ('' if region['reach'] else 'COVERED ') + ('HOLD ' if region.get('hold') else ''),
        'DISABLED ' if region['dis'] else '', region['text'].strip())


def remembered(device, regions=None):
    """The newest map, kept beside logcat (a tap clears logcat; a map that did not change is not logged again)."""
    path = Path(tempfile.gettempdir()) / ('ui_crawl-%s-%s.json' % (device.serial, device.top_pid()))
    if regions is not None:
        path.write_text(json.dumps(regions, ensure_ascii=False))
        return regions
    return json.loads(path.read_text()) if path.exists() else None


def current(device, lines=None, required=True):
    pid = device.top_pid()
    if pid is None and not required:
        return []
    found = blocks(device.logcat() if lines is None else lines, pid)
    regions = remembered(device, found[-1]) if found else remembered(device)
    if regions is None and not required:
        return []
    if regions is None:
        sys.exit('no taps block in logcat: is log.tag.StormUiTaps DEBUG set and the app (re)started since?')
    return regions


def pick(regions, selector):
    if re.fullmatch(r'\d+,\d+', selector):
        x, y = map(int, selector.split(','))
        return {'name': selector, 'at': [x, y], 'where': '?', 'tag': '?', 'verb': '?', 'text': '', 'reach': True, 'dis': False, 'id': selector}
    if selector.startswith('~'):
        hits = [r for r in regions if selector[1:].lower() in r['text'].lower()]
    else:
        hits = [r for r in regions if r['name'] == selector or (r['id'] == selector and r['n'] == 1)]
    if not hits:
        sys.exit('no region ' + selector)
    return hits[0]


def into_view(device, regions, name, tries=8):
    """The region `name`, scrolled into view when its list hides it: drag the list by the region's distance from the
    middle of the screen (at the region's x), then read the map again."""
    for _ in range(tries):
        region = next((r for r in regions if r['name'] == name), None)
        if region is None or (region['at'] and region['reach']):
            return regions, region
        # Off its list's view, or covered (maybe by the list's own sticky header): drag; covered by something that
        # does not scroll away (a modal) leaves the box where it was.
        x, y, w, h = region['box']
        middle = y + h // 2
        height = int(device.size()[1])
        span = max(-height // 4, min(height // 4, middle - height // 2))
        if abs(span) < 10:
            return regions, None
        cx = max(10, min(int(device.size()[0]) - 10, x + w // 2))
        device.clear()
        device.adb('shell', 'input', 'swipe', str(cx), str(height // 2), str(cx), str(height // 2 - span), '500')
        _, block = settle(device, quiet=1.0)
        if block is None:
            return regions, None
        regions = remembered(device, block)
        moved = next((r for r in regions if r['name'] == name), None)
        if moved is not None and moved['box'] == region['box'] and not moved['reach']:
            return regions, None
    return regions, None


def act(device, regions, region, back=False, hold=False):
    """Tap, hold (or Back) and report what the map became and what was logged."""
    pids = device.pids()
    device.clear()
    if back:
        device.back()
    elif hold or region.get('hold'):
        device.hold(*region['at'])  # hold-to-confirm: a tap only shakes it
    else:
        device.tap(*region['at'])
    lines, block = settle(device)
    after = remembered(device, block) if block is not None else current(device, lines)
    result = {
        'tapped': 'BACK' if back else region['name'], 'where': '' if back else region['where'], 'verb': '' if back else region['verb'],
        'text': '' if back else region['text'].strip(), 'before': screen_of(regions), 'after': screen_of(after),
        'changed': block is not None and signature(block) != signature(regions), 'problems': problems(lines, pids | device.pids()),
        'toasts': [r['text'].strip() for r in after if r['where'] == 'layer' and r['text'].strip()],
        'traces': traces(lines, pids | device.pids()),
    }
    return result, after


def report(result):
    status = 'ERR ' if result['problems'] else ('->  ' if result['changed'] else 'same')
    print('%s %-28s %s -> %s %s' % (status, result['tapped'], result['before'], result['after'], result['text'][:30]))
    for toast in result.get('toasts', []):
        print('       toast: ' + toast)
    for line in result['problems'][:8]:
        print('       ' + line[:220])
    for trace in result.get('traces', []):
        if trace['transient']:
            print('       FLICKER %s: %d frames, %.0f ms, %.0f%% of the screen' % (trace['action'], trace['transient'], trace['ms'], trace['area'] * 100))
            for detail in trace['details'][:4]:
                print('         ' + detail[:200])


def flicker_list(dirs):
    """Every tap of the crawls that showed a transient frame, most visible first (ms on the screen x share of it)."""
    rows = []
    for folder in dirs:
        path = Path(folder) / 'crawl.jsonl'
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            result = json.loads(line)
            for trace in result.get('traces', []):
                rows.append((result, trace))
    flickering = [(r, t) for r, t in rows if t['transient']]
    flickering.sort(key=lambda rt: -(rt[1]['ms'] * max(rt[1]['area'], 0.01)))
    print('%d taps traced, %d with transient frames' % (len(rows), len(flickering)))
    for result, trace in flickering:
        print('%5.0f ms %3.0f%% %d fr  %-14s %-28s %s' % (trace['ms'], trace['area'] * 100, trace['transient'], result['before'][:14],
                                                       result['tapped'][:28], trace['action']))
        for detail in trace['details'][:3]:
            print('                         ' + detail[:180])
    return 1 if flickering else 0


def crawl(device, args, each=None):
    """With --each ID (repeatable, outer first): tap each region of that id (a tab, a section) in turn and crawl
    what it shows; the ids of every --each level are not tapped by the inner crawls."""
    each = list(args.each or []) if each is None else each
    if each:
        level, inner = each[0], each[1:]
        tabs = [r['name'] for r in current(device) if re.fullmatch(level, r['id']) and r['at'] and r['reach'] and not r['dis']
                and not (args.skip and re.search(args.skip, r['where'] + ' ' + r['name'] + ' ' + r['text']))]
        print('each %s: %s' % (level, ', '.join(tabs) or '(none here)'))
        if not tabs:
            return crawl(device, args, inner)
        status = 0
        for tab in tabs:
            regions = current(device)
            region = next((r for r in regions if r['name'] == tab and r['at'] and r['reach']), None)
            if region is None:
                print('gone %s' % tab)
                continue
            result, _ = act(device, regions, region)
            report(result)
            status |= crawl(device, args, inner)
        return status
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    log = (out / 'crawl.jsonl').open('a')
    home = current(device)
    home_screen = screen_of(home)
    shots = 0
    names = [r['name'] for r in home if (r['reach'] or not r['at']) and not r['dis']
             and (not args.only or re.search(args.only, r['where'] + ' ' + r['name'] + ' ' + r['text']))
             and not (args.skip and re.search(args.skip, r['where'] + ' ' + r['name'] + ' ' + r['text']))
             and not any(re.fullmatch(level, r['id']) for level in (args.each or []))
             and (args.hud or r['where'] != 'hud') and (args.holds or not r.get('hold'))]
    print('crawl %s: %d regions' % (home_screen, len(names)))
    state = home

    home_modals = {r['id'] for r in home if r['verb'] in ('dismiss', 'close')}

    def reachable(regions, rest):
        """Nothing still to tap is covered by something drawn over it (a modal, a detail view), and no modal opened
        (a scrim that dismisses it). Gone (the screen shows other content now) or scrolled out of its list is fine:
        the crawl reports it when it gets there."""
        by_name = {r['name']: r for r in regions}
        if any(r['verb'] in ('dismiss', 'close') and r['id'] not in home_modals for r in regions):
            return False
        return not any(n in by_name and by_name[n]['at'] and not by_name[n]['reach'] for n in rest)

    for k, name in enumerate(names):
        state, region = into_view(device, state, name)
        if region is None:
            print('gone %s (the screen changed under the crawl, or it cannot be scrolled into view)' % name)
            continue
        result, state = act(device, state, region)
        report(result)
        if result['changed'] or result['problems']:
            shots += 1
            result['shot'] = '%s-%02d-%s.png' % (re.sub(r'\W+', '_', home_screen), shots, re.sub(r'\W+', '_', name))
            device.shot(out / result['shot'])
        log.write(json.dumps(result, ensure_ascii=False) + '\n')
        # Left the screen, or something now covers what is still to tap (a modal, a detail view): Back must undo it.
        rest = names[k + 1:] or names
        if (result['after'] != home_screen or not reachable(state, rest)) and not args.no_back:
            result, state = act(device, state, None, back=True)
            report(result)
            log.write(json.dumps(result, ensure_ascii=False) + '\n')
            if screen_of(state) != home_screen or not reachable(state, rest):
                here = {r['name'] for r in state if r['at'] and r['reach']}
                print('STOP: Back did not return to %s (now %s; not reachable: %s)' % (
                    home_screen, screen_of(state), ', '.join(n for n in rest if n not in here)))
                return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-s', '--serial', required=True)
    parser.add_argument('-p', '--package', default='org.stormengine.port.game')
    sub = parser.add_subparsers(dest='command', required=True)
    taps = sub.add_parser('taps')
    taps.add_argument('--all', action='store_true', help='also covered, disabled and off-screen regions')
    swipe = sub.add_parser('swipe', help='drag from X1,Y1 to X2,Y2 (scroll a list)')
    swipe.add_argument('start')
    swipe.add_argument('end')
    one = sub.add_parser('tap')
    one.add_argument('selector')
    press = sub.add_parser('hold', help='press and hold (hold-to-confirm buttons)')
    press.add_argument('selector')
    sub.add_parser('back')
    wait = sub.add_parser('wait', help='until the screen (as `taps` names it) matches REGEX')
    wait.add_argument('regex')
    wait.add_argument('--timeout', type=float, default=180)
    shot = sub.add_parser('shot')
    shot.add_argument('file')
    walk = sub.add_parser('crawl')
    walk.add_argument('--out', required=True)
    walk.add_argument('--skip')
    walk.add_argument('--only')
    walk.add_argument('--hud', action='store_true', help='also the HUD regions')
    walk.add_argument('--no-back', action='store_true')
    walk.add_argument('--holds', action='store_true', help='also hold-to-confirm controls (they sell, buy, destroy: held)')
    walk.add_argument('--each', action='append', help='tap each region whose id matches this regex and crawl under it (repeat: nested)')
    ranked = sub.add_parser('flicker', help='the taps of crawls that showed transient frames, most visible first')
    ranked.add_argument('dirs', nargs='+')
    args = parser.parse_args()
    device = Device(args.serial, args.package)
    if args.command == 'flicker':
        return flicker_list(args.dirs)
    if args.command == 'taps':
        regions = current(device)
        print(screen_of(regions))
        for region in regions:
            if args.all or (region['at'] and region['reach'] and not region['dis']):
                print('  ' + describe(region))
    elif args.command == 'tap':
        regions = current(device)
        region = pick(regions, args.selector)
        if not region['at'] or not region['reach']:
            regions, region = into_view(device, regions, region['name'])
            if region is None:
                sys.exit('%s: not in view and cannot be scrolled into view (or covered)' % args.selector)
        result, _ = act(device, regions, region)
        report(result)
    elif args.command == 'hold':
        regions = current(device)
        result, _ = act(device, regions, pick(regions, args.selector), hold=True)
        report(result)
    elif args.command == 'back':
        result, _ = act(device, current(device), None, back=True)
        report(result)
    elif args.command == 'swipe':
        regions = current(device)
        device.clear()
        device.adb('shell', 'input', 'swipe', *args.start.split(','), *args.end.split(','), '600')
        lines, block = settle(device)
        regions = remembered(device, block) if block is not None else regions
        print(screen_of(regions))
        for region in regions:
            if region['at'] and region['reach'] and not region['dis']:
                print('  ' + describe(region))
        for line in problems(lines, device.pids()):
            print('  ' + line[:220])
    elif args.command == 'wait':
        start = time.time()
        while time.time() - start < args.timeout:
            regions = current(device, required=False)
            if regions and re.search(args.regex, screen_of(regions)):
                print(screen_of(regions))
                return 0
            time.sleep(1)
        print('timeout; screen ' + screen_of(current(device, required=False)))
        return 1
    elif args.command == 'shot':
        device.shot(args.file)
    elif args.command == 'crawl':
        return crawl(device, args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
