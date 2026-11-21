"""How long the setup takes: an estimate per step before it runs, a better one while it runs, the real one after.

The data side only, no window code: oot.py (the command line) and gui_oot.py (the window) both read it, and a later
drawing of the step table needs nothing but Tracker.view().

Where an estimate comes from, in rising order of trust:

 1. DEFAULTS: seconds per step, measured on a 16-thread machine, apart for the first time a step runs on a game
    folder ("first") and for a later run ("rerun": the caches of the first are there). The steps that are limited by
    the processor (SCALED) are multiplied once by 16 / logical cores.
 2. This machine's own durations: every finished step is recorded in timings.json in the user's application data
    (%APPDATA%\\Rekarina\\timings.json), per step and case, and the next run prefers them.

        {"format": 1, "cores": 16, "steps": {"oot_compile": {"first": 702.4, "rerun": 163.0}, ...}}

 3. While a step runs, the phase it is in by its own measured pace: a phase with a counter (objects compiled, shaders
    translated, bytes downloaded) takes what its counter has left at the rate the counter moved over the last
    RATE_WINDOW seconds (Rate; up to now, so a counter that stops makes the figure grow), once it has moved for
    RATE_SPAN seconds; the phases after it take their shares of the step's estimate (PHASES). Until then, and in a
    phase without a counter, the clock: the phase's share of the estimate minus the time it has taken, and past that a
    share of the time taken (OVERRUN), so a phase that takes longer than estimated shows a figure that grows instead of
    one that stands still. The shown remaining time moves towards that figure at a bounded speed (SMOOTH).
    Until 1.2.7 the whole step was put at its time so far divided by the share done by the phase table: a translation
    that ran at a steady 8.9 shaders a second for 48 minutes more (a slow disk) was shown as 3.7 to 4.3 hours, rising,
    because the table gave it 15 % of the step and the driver's phase 78 %.

Events (events.py) carry it between the two processes: after "start" the command line emits

    {"type": "eta", "cores": 16, "steps": {"<step>": {"case": "first" | "rerun" | "all", "seconds": 720, "source":
                                                       "default" | "measured"}}}

and Tracker folds "start", "eta", "plan", "step" and "progress" into what a window shows:

    view = tracker.view()
    view["steps"]      one per step, in order: {"id", "state": done | running | pending | skipped | failed,
                       "seconds_taken" (done, running), "seconds_estimated" (the whole step), "remaining" (running,
                       pending), "fraction" (running: 0..1 of the step, None when it has no counter),
                       "counter": (done, total, unit) of the running phase or None}
    view["elapsed"], view["remaining"]   seconds of the whole run
    view["index"], view["count"]         the running step's place in the list (1-based), the number of steps

step_text() and total_text() put a view into words through a text catalog ("eta.*" ids); a window that draws the
table itself uses the view and clock_text() alone.
"""
import collections
import json
import os
import time
from pathlib import Path

REFERENCE_CORES = 16
#          step               first  rerun   (seconds)
DEFAULTS = {
    'oot_check':             (10, 10),
    'oot_extract':           (20, 20),
    'oot_lift':              (100, 100),
    'oot_compile':           (720, 240),    # a rerun compiles what changed and links again
    'oot_components':        (60, 10),
    'oot_prompts':           (60, 60),      # the button prompts and fonts, drawn again when their tools change
    'oot_prepare':           (420, 60),     # (the variants play met, 1.2.1-1.2.3; every run since prepares all: ALL_SHADERS)
    'oot_package':           (90, 30),      # the copy (the driver's build and the textures are the launcher's since 1.2.8)
    'background':            (2400, 600),   # the launcher's work after the setup (installer/oot.py background_main)
    # the launcher's own update (update.py): the download depends on the line, so its counter soon takes over
    'update_download':       (90, 90),
    'update_unpack':         (40, 40),
    'update_restart':        (5, 5),
}
UNKNOWN_STEP = (30, 30)
# The case 'translate' of oot_prepare: every variant of every shader translated, on the reference machine from nothing.
# (Its case 'all' until 1.2.7 also built every variant in the driver: 2116 s on 16 threads; a timing recorded under
# that case is not this step's any more.)
ALL_SHADERS = 900
SCALED = ('oot_lift', 'oot_compile', 'oot_prepare')   # limited by the processor: times 16 / logical cores
SCALE_RANGE = (0.5, 4.0)
# The share of a step's time each of its phases takes, in the order they run. A phase that is not listed (or a step
# without a row) counts by the clock alone.
PHASES = {
    'oot_check': (('zbic', 0.3), ('dump', 0.5), ('space', 0.2)),
    'oot_lift': (('discover', 0.4), ('emit', 0.6)),
    'oot_compile': (('compile', 0.95), ('link', 0.05)),
    'oot_components': (('detect', 0.05), ('fsr', 0.95), ('dlss', 0.95), ('dlssg', 0.95)),   # one byte counter over the three
    'oot_prompts': (('prompts', 1.0),),
    'oot_prepare': (('seed', 0.05), ('translate', 0.9), ('pack', 0.05)),
    'oot_package': (('pack', 0.2), ('copy', 0.8)),
    'background': (('libraries', 0.85), ('textures', 0.15)),
    'update_download': (('download', 1.0),),
    'update_unpack': (('unpack', 1.0),),
}
SHARED_COUNTER = {'oot_components': ('fsr', 'dlss', 'dlssg')}   # phases that continue one counter
RATE_WINDOW = 120.0    # seconds of a counter's history its rate is taken over
RATE_SPAN = 20.0       # a counter's rate counts once it has moved over this many seconds
# Per second of clock the shown remaining time falls at most 4 s and rises at most 2 s, or 2 % and 1 % of itself when
# that is more (half an hour may lose 36 s a second: a wrong first guess is gone within a minute, never in one jump).
SMOOTH = ((4.0, 0.02), (2.0, 0.01))
FLOOR = 5.0            # seconds: a running step never shows less
# A step past its estimate that no counter speaks for: the estimate was wrong, and the clock is all there is. The rest
# is put at this share of the time the step has taken so far, which grows while it runs: a fixed floor showed the same
# couple of minutes for an hour (a shader step estimated as a rerun that prepared every variant).
OVERRUN = 0.25
FORMAT = 2
# Steps whose work changed in timings.json's format 2 (1.2.8): the setup no longer builds the driver's shaders, so their
# times recorded before (an hour and more on some PCs) are not taken.
FORMAT_2_STEPS = ('oot_prepare', 'oot_package')


def cores():
    return os.cpu_count() or 8


def timings_path():
    base = os.environ.get('APPDATA') if os.name == 'nt' else os.environ.get('XDG_CONFIG_HOME')
    return Path(base or Path.home() / '.config') / 'Rekarina' / 'timings.json'


def load_timings(path=None):
    """{step: {case: seconds}} of timings.json; {} when there is none or it cannot be read."""
    try:
        data = json.loads(Path(path or timings_path()).read_text(encoding='utf-8'))
        steps = data.get('steps') if isinstance(data, dict) else None
        old = isinstance(data, dict) and not (isinstance(data.get('format'), int) and data['format'] >= 2)
        return {s: {c: float(v) for c, v in cases.items() if isinstance(v, (int, float)) and v > 0}
                for s, cases in steps.items() if isinstance(cases, dict) and not (old and s in FORMAT_2_STEPS)} \
            if isinstance(steps, dict) else {}
    except (OSError, ValueError, AttributeError):
        return {}


def record(step, case, seconds, path=None):
    """Keep a finished step's duration for the next estimate. A profile that cannot be written costs only that."""
    if case not in ('first', 'rerun', 'all', 'translate') or not seconds or seconds <= 0:
        return
    path = Path(path or timings_path())
    steps = load_timings(path)
    steps.setdefault(step, {})[case] = round(float(seconds), 1)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + '.tmp')
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'format': FORMAT, 'cores': cores(), 'steps': steps}, f, indent=1)
            f.write('\n')
        os.replace(tmp, path)
    except OSError:
        pass


def scale(n_cores):
    return max(SCALE_RANGE[0], min(SCALE_RANGE[1], REFERENCE_CORES / max(1, n_cores)))


def estimate(step, case, n_cores=None, timings=None):
    """(seconds, 'measured' | 'default') for a step that is about to run."""
    measured = (timings or {}).get(step, {}).get(case)
    if measured:
        return float(measured), 'measured'
    first, rerun = DEFAULTS.get(step, UNKNOWN_STEP)
    seconds = float(ALL_SHADERS if step == 'oot_prepare' and case in ('all', 'translate') else
                    first if case == 'first' else rerun)
    if step in SCALED:
        seconds *= scale(n_cores or cores())
    return seconds, 'default'


def estimates(cases, n_cores=None, timings=None):
    """The fields of the "eta" event for {step: case}: the machine's cores and each step's estimate."""
    n_cores = n_cores or cores()
    timings = load_timings() if timings is None else timings
    steps = {}
    for step, case in cases.items():
        seconds, source = estimate(step, case, n_cores, timings)
        steps[step] = {'case': case, 'seconds': round(seconds, 1), 'source': source}
    return {'cores': n_cores, 'steps': steps}


def step_fraction(step, phase, done, total, seen):
    """The share of a step that is behind it, from the phase it is in and that phase's counter; None when neither the
    phase table nor a counter says anything. seen: the phases the step has reported so far."""
    table = PHASES.get(step)
    inside = done / total if total and done is not None else None
    if inside is not None:
        inside = max(0.0, min(1.0, inside))
    if not table:
        return inside
    names = [n for n, _ in table]
    if phase not in names:
        return None
    shared = SHARED_COUNTER.get(step, ())
    before = 0.0
    for name, share in table:
        if name == phase:
            break
        if not (name in shared and phase in shared):   # phases of one counter are not behind each other
            before += share
    share = dict(table)[phase]
    if inside is None:
        return before if before > 0 else None
    return min(1.0, before + share * inside)


class Rate:
    """A counter's pace: units per second over its last RATE_WINDOW seconds, measured to now (a counter that stops
    slows it down); None until the counter has moved over RATE_SPAN seconds. A counter that goes back (a phase that
    counts again from zero) starts a new history."""

    def __init__(self):
        self.samples = collections.deque()

    def reset(self):
        self.samples.clear()

    def feed(self, now, done):
        if done is None:
            return
        if self.samples and done < self.samples[-1][1]:
            self.samples.clear()
        self.samples.append((now, done))
        while len(self.samples) > 2 and self.samples[1][0] <= now - RATE_WINDOW:
            self.samples.popleft()

    def rate(self, now):
        if len(self.samples) < 2:
            return None
        (t0, d0), (t1, d1) = self.samples[0], self.samples[-1]
        if t1 - t0 < RATE_SPAN or d1 <= d0:
            return None
        return (d1 - d0) / max(now - t0, t1 - t0)


def phase_left(step, phase, done, total, rate, estimate, phase_elapsed):
    """Seconds left of the phase a step is in (the module text, 3) and of the phases after it: (this phase, after it).
    None for this phase when the step has a phase table that does not know the phase."""
    table = PHASES.get(step) or ((phase, 1.0),)
    names = [n for n, _ in table]
    if phase not in names:
        return None, 0.0
    at = names.index(phase)
    shared = SHARED_COUNTER.get(step, ())
    after = sum(share for name, share in table[at + 1:] if not (name in shared and phase in shared))
    mine = estimate * table[at][1]
    if total and done is not None:
        left = max(0, total - done)
        if rate:
            return left / rate, estimate * after
        by_share = mine * left / total
    else:
        by_share = mine - phase_elapsed
    return max(by_share, OVERRUN * phase_elapsed, 0.0), estimate * after


class Tracker:
    """Folds the setup's events into the step table with times (the module text). clock: seconds, monotonic."""

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.reset()

    def reset(self):
        self.steps = []          # ids in order
        self.info = {}           # id -> dict(state, estimate, started, taken, phase, phase_at, done, total, unit, seen, shown,
                                 #             shown_at)
        self.started = None
        self.ended = None
        self.cores = None

    def blank(self, step):
        return {'state': 'pending', 'estimate': estimate(step, 'first', self.cores)[0], 'started': None, 'taken': None,
                'phase': None, 'phase_at': None, 'done': None, 'total': None, 'unit': None, 'seen': [], 'shown': None,
                'shown_at': None, 'rate': Rate()}

    def step(self, step):
        if step not in self.info:
            self.steps.append(step)
            self.info[step] = self.blank(step)
        return self.info[step]

    def feed(self, event, now=None):
        now = self.clock() if now is None else now
        kind = event.get('type')
        if kind == 'start':
            self.reset()
            self.started = now
            for s in event.get('steps') or []:
                self.step(s)
        elif kind == 'eta':
            self.cores = event.get('cores') or self.cores
            for s, e in (event.get('steps') or {}).items():
                if isinstance(e, dict) and isinstance(e.get('seconds'), (int, float)):
                    self.step(s)['estimate'] = float(e['seconds'])
        elif kind == 'plan':
            for s in event.get('skip') or {}:
                if self.step(s)['state'] == 'pending':
                    self.info[s]['state'] = 'skipped'
        elif kind == 'step':
            i, state = self.step(event.get('step')), event.get('state')
            if state == 'started':
                i.update(state='running', started=now, phase=None, phase_at=None, done=None, total=None, unit=None, seen=[],
                         shown=None)
                i['rate'].reset()
            elif state == 'done':
                taken = event.get('seconds')
                i.update(state='done', taken=float(taken) if isinstance(taken, (int, float))
                         else (now - i['started'] if i['started'] is not None else 0.0))
            elif state == 'skipped':
                i['state'] = 'skipped'
        elif kind == 'progress':
            i = self.step(event.get('step'))
            if i['state'] == 'pending':
                i.update(state='running', started=now)
            phase = event.get('phase')
            if phase and phase not in i['seen']:
                i['seen'].append(phase)
            if phase != i['phase'] or event.get('total') != i['total']:
                i['phase_at'] = now
                i['rate'].reset()
            i.update(phase=phase, done=event.get('done'), total=event.get('total'), unit=event.get('unit'))
            if i['total']:
                i['rate'].feed(now, i['done'])
        elif kind == 'error':
            if event.get('step') in self.info:
                self.info[event['step']]['state'] = 'failed'
        elif kind == 'end':
            self.ended = now
            for i in self.info.values():
                if i['state'] == 'running':
                    i['state'] = 'failed' if event.get('result') == 'failed' else 'pending'

    def running_remaining(self, i, step, now):
        """Seconds left of the running step (the module text, 3), and the share of it that is done (for the bar)."""
        elapsed = max(0.0, now - i['started'])
        est = i['estimate']
        fraction = step_fraction(step, i['phase'], i['done'], i['total'], i['seen'])
        mine, after = (None, 0.0)
        if i['phase'] is not None and elapsed >= 3:
            phase_elapsed = max(0.0, now - (i['phase_at'] if i['phase_at'] is not None else i['started']))
            mine, after = phase_left(step, i['phase'], i['done'], i['total'], i['rate'].rate(now), est, phase_elapsed)
        if mine is None:   # no phase yet, or one the table does not know: the step's clock
            target = max(est - elapsed, OVERRUN * elapsed)
        else:
            target = mine + after
        target = max(target, FLOOR)
        if i['shown'] is not None:
            dt = max(0.0, now - i['shown_at'])
            fall = max(SMOOTH[0][0], SMOOTH[0][1] * i['shown']) * dt
            rise = max(SMOOTH[1][0], SMOOTH[1][1] * i['shown']) * dt
            target = max(i['shown'] - fall, min(i['shown'] + rise, target))
        i['shown'], i['shown_at'] = target, now
        return target, fraction

    def view(self, now=None):
        now = self.clock() if now is None else now
        rows, remaining, index = [], 0.0, 0
        for n, step in enumerate(self.steps, 1):
            i = self.info[step]
            row = {'id': step, 'state': i['state'], 'seconds_taken': None, 'seconds_estimated': i['estimate'],
                   'remaining': None, 'fraction': None, 'counter': None}
            if i['state'] == 'done':
                row['seconds_taken'] = i['taken']
                index = max(index, n)
            elif i['state'] == 'running':
                left, fraction = self.running_remaining(i, step, now)
                row.update(seconds_taken=max(0.0, now - i['started']), remaining=left, fraction=fraction)
                if i['total']:
                    row['counter'] = (i['done'] or 0, i['total'], i['unit'])
                remaining += left
                index = n
            elif i['state'] == 'pending' and self.ended is None:
                row['remaining'] = i['estimate']
                remaining += i['estimate']
            rows.append(row)
        end = self.ended if self.ended is not None else now
        return {'steps': rows, 'elapsed': max(0.0, end - self.started) if self.started is not None else 0.0,
                'remaining': remaining if self.ended is None else 0.0, 'index': index, 'count': len(self.steps),
                'running': next((r['id'] for r in rows if r['state'] == 'running'), None)}


# ---- words --------------------------------------------------------------------------------------------------------

def clock_text(seconds):
    """'0:21', '7:12', '1:02:03'."""
    s = int(round(max(0.0, seconds or 0)))
    return f'{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


def about_text(cat, seconds, total=False):
    """'about 5 min', 'under a minute'; the bottom line's 'about 37 minutes left' with total."""
    minutes = int(round((seconds or 0) / 60))
    if minutes < 1:
        return cat('eta.left_under' if total else 'eta.about_under')
    if minutes == 1 and total:
        return cat('eta.left_one')
    if minutes >= 90:
        return cat('eta.left_hours' if total else 'eta.about_hours', n=f'{minutes / 60:.1f}')
    return cat('eta.left_min' if total else 'eta.about_min', n=minutes)


def counter_text(cat, counter):
    """'412 of 681'; bytes as megabytes."""
    done, total, unit = counter
    if unit == 'bytes':
        return cat('eta.counter_mb', done=f'{done / 1e6:.0f}', total=f'{total / 1e6:.0f}')
    return cat('eta.counter', done=done, total=total)


def step_text(cat, row):
    """The time column of one step: '0:21' when done, '412 of 681 · about 5 min' while it runs, 'about 1 min' before."""
    if row['state'] == 'done':
        return clock_text(row['seconds_taken'])
    if row['state'] == 'running':
        about = about_text(cat, row['remaining'])
        return f'{counter_text(cat, row["counter"])} · {about}' if row['counter'] else about
    if row['state'] == 'pending' and row['remaining'] is not None:
        return about_text(cat, row['remaining'])
    return ''


def total_text(cat, view):
    """'Step 4 of 7 · about 37 minutes left · 7:12 elapsed'."""
    parts = []
    if view['index'] and view['running']:
        parts.append(cat('eta.step_of', index=view['index'], count=view['count']))
    if view['running']:
        parts.append(about_text(cat, view['remaining'], total=True))
    parts.append(cat('eta.elapsed', time=clock_text(view['elapsed'])))
    return ' · '.join(parts)
