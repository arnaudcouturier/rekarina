"""The launcher's background work after the setup (installer/oot.py background_main, since 1.2.8): the driver's build
of every prepared shader variant and the texture cache, which the setup's package step did itself until 1.2.7 while
the player waited (20 minutes to 1.5 hours on some PCs). The launcher does them while it is open and the game is not
running: this module runs `python -m installer.oot --dest <game folder> --background` as a child at below-normal
priority on all cores but one, and reads its JSON-lines events (events.py) for the counts (step "background", phases
"libraries" and "textures"). 1.2.1 to 1.2.3 ran the setup's own shader step this way for the rarer variants.

    job = ShaderJob(command, cwd)
    job.start()            # a child at low priority; states: idle, running, paused, done, failed
    job.poll()             # drain the child's events; True when what the window shows changed
    job.stop()             # the stop file first (what was built stays), after a moment the whole tree is ended
    job.pause() / job.resume()

A stopped job starts again with start(): the setup's preparation counts what is built already, so the new run goes on from
there. Nothing here runs while the game runs: the window asks game_running() before it starts a job and stops one
when a game appears (a game that someone else started counts as well).
"""
import json
import os
import queue
import signal
import subprocess
import threading
import time

from . import eta

WIN = os.name == 'nt'
BELOW_NORMAL = 0x00004000
CREATE_NO_WINDOW = 0x08000000


def process_table():
    """{pid: (parent pid, executable file name)} of the processes of this machine."""
    table = {}
    if WIN:
        import ctypes
        from ctypes import wintypes

        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                        ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                        ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG), ('dwFlags', wintypes.DWORD),
                        ('szExeFile', wintypes.WCHAR * 260)]
        k = ctypes.windll.kernel32
        k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        snap = k.CreateToolhelp32Snapshot(0x2, 0)
        if snap in (None, wintypes.HANDLE(-1).value):
            return table
        try:
            e = Entry()
            e.dwSize = ctypes.sizeof(e)
            ok = k.Process32FirstW(snap, ctypes.byref(e))
            while ok:
                table[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile)
                ok = k.Process32NextW(snap, ctypes.byref(e))
        finally:
            k.CloseHandle(snap)
        return table
    try:
        out = subprocess.run(['ps', '-e', '-o', 'pid=,ppid=,comm='], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return table
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            table[int(parts[0])] = (int(parts[1]), parts[2])
    return table


def descendants(table, pid):
    """The pids of the processes below pid (children, their children ...)."""
    found, todo = set(), [pid]
    while todo:
        top = todo.pop()
        for p, (parent, _name) in table.items():
            if parent == top and p not in found:
                found.add(p)
                todo.append(p)
    return found


def games_running(name, ignore_below=None, table=None):
    """The pids of processes named `name` (the game's file name) that are not part of the job at `ignore_below`: the
    preparation itself runs the game's exe under the same name (--prepare-shaders, --warm-shaders), and that is no game
    for the player."""
    table = process_table() if table is None else table
    skip = descendants(table, ignore_below) | {ignore_below} if ignore_below else set()
    return sorted(p for p, (_parent, exe) in table.items() if exe.lower() == name.lower() and p not in skip)


def lower(proc):
    """Below-normal priority and all cores but one for a child and, by inheritance, everything it starts."""
    try:
        if WIN:
            import ctypes
            bind_to_launcher(proc)
            k = ctypes.windll.kernel32
            handle = int(proc._handle)
            have, system = ctypes.c_size_t(), ctypes.c_size_t()
            if k.GetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.byref(have), ctypes.byref(system)) and bin(have.value).count('1') > 1:
                k.SetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.c_size_t(have.value & ~(1 << (have.value.bit_length() - 1))))
        else:
            cpus = sorted(os.sched_getaffinity(proc.pid))
            if len(cpus) > 1:
                os.sched_setaffinity(proc.pid, set(cpus[:-1]))
    except (OSError, AttributeError, ValueError):
        pass   # a machine that refuses costs the spare core, the low priority still holds


_kill_job = None


def bind_to_launcher(proc):
    """Windows: the child (and what it starts) goes into a job object that ends with the launcher, so a launcher that is
    killed or crashes leaves no preparation running behind it."""
    global _kill_job
    import ctypes
    from ctypes import wintypes
    k = ctypes.windll.kernel32

    class Basic(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                    ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ('r', 'w', 'o', 'rb', 'wb', 'ob')]

    class Extended(ctypes.Structure):
        _fields_ = [('Basic', Basic), ('Io', IoCounters), ('ProcessMemoryLimit', ctypes.c_size_t),
                    ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                    ('PeakJobMemoryUsed', ctypes.c_size_t)]
    if _kill_job is None:
        k.CreateJobObjectW.restype = wintypes.HANDLE
        job = k.CreateJobObjectW(None, None)
        info = Extended()
        info.Basic.LimitFlags = 0x2000   # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not job or not k.SetInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info)):
            return
        _kill_job = job
    k.AssignProcessToJobObject(wintypes.HANDLE(_kill_job), wintypes.HANDLE(int(proc._handle)))


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if WIN:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True, creationflags=CREATE_NO_WINDOW)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            proc.kill()


class ShaderJob:
    """The setup's preparation of every shader variant as a child of the launcher (module text)."""

    def __init__(self, command, cwd, env=None, clock=time.monotonic, popen=subprocess.Popen, stopper=None, asker=None):
        """stopper(proc): asks the child to end and returns once it has ended or was killed (installer/oot.py
        stop_background); without one stop() kills the tree at once. asker(): asks it to end and returns at once (the
        stop file alone), for stop_begin."""
        self.command, self.cwd, self.env, self.clock, self.popen = command, cwd, env, clock, popen
        self.stopper, self.asker = stopper, asker
        self.stop_since = None
        self.state = 'idle'
        self.proc = None
        self.events = queue.Queue()
        self.reset_counts()
        self.error = None      # the setup's error id when it failed
        self.paused = False    # the player's Pause: until the next start of the launcher

    def reset_counts(self):
        self.phase = self.done = self.total = None
        self.step = None
        self.started = None
        self.fraction = 0.0
        self.first_fraction = None
        self.rate = eta.Rate()   # the running phase's pace: the time left follows it (remaining)

    @property
    def pid(self):
        return self.proc.pid if self.proc else None

    @property
    def active(self):
        return self.state == 'running'

    @property
    def stopping(self):
        return self.state == 'stopping'

    def start(self):
        """Run the setup as a child. False when one is running or the job is over (done, or failed: not retried by itself)."""
        if self.state in ('running', 'stopping', 'done', 'failed') or self.paused:
            return False
        self.reset_counts()
        self.error = None
        env = dict(self.env if self.env is not None else os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        kwargs = dict(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=self.cwd, env=env, text=True, encoding='utf-8',
                      errors='replace', bufsize=1)
        if WIN:
            kwargs['creationflags'] = BELOW_NORMAL | CREATE_NO_WINDOW
        else:
            kwargs.update(start_new_session=True, preexec_fn=lambda: os.nice(10))
        self.proc = self.popen(self.command, **kwargs)
        lower(self.proc)
        self.events = queue.Queue()
        self.started = self.clock()
        self.state = 'running'
        threading.Thread(target=self.read, args=(self.proc, self.events), daemon=True).start()
        return True

    def read(self, proc, out):
        try:
            for line in proc.stdout:
                line = line.strip()
                if line.startswith('{'):
                    try:
                        out.put(json.loads(line))
                    except ValueError:
                        pass
            proc.wait()
        except (OSError, ValueError):
            pass
        try:
            proc.stdout.close()
        except (OSError, ValueError):
            pass
        out.put({'type': '_exit', 'code': proc.poll()})

    def poll(self):
        """Take in the child's events; True when the state or the counts changed."""
        changed = False
        while True:
            try:
                e = self.events.get_nowait()
            except queue.Empty:
                return changed
            if self.state != 'running':
                continue   # an event of a child that was stopped meanwhile
            kind = e.get('type')
            if kind == 'progress' and e.get('step') in ('background', 'oot_prepare'):
                if e.get('phase') != self.phase or e.get('total') != self.total:
                    self.rate.reset()
                self.phase, self.done, self.total = e.get('phase'), e.get('done'), e.get('total')
                self.step = e.get('step')
                if self.total:
                    self.rate.feed(self.clock(), self.done)
                f = eta.step_fraction(self.step, self.phase, self.done, self.total, [])
                if f is not None:   # the latest event's: a phase may count from zero again (a second pass of the translation)
                    self.fraction = f
                    if self.first_fraction is None:
                        self.first_fraction = (f, self.clock())
                changed = True
            elif kind == 'error':
                self.error = e.get('id', 'internal')
                self.error_params = e.get('params', {})
                changed = True
            elif kind == 'end':
                self.state = 'done' if e.get('result') == 'ok' else 'failed'
                changed = True
            elif kind == '_exit':
                if self.state == 'running':
                    self.state = 'done' if e.get('code') == 0 else 'failed'
                    changed = True

    def stop(self):
        """End the child (the stopper, else its whole tree at once) and wait for it. The job can start again (start():
        it continues)."""
        proc = self.proc
        if self.state == 'running' and proc:
            self.state = 'idle'
            if self.stopper is not None:
                try:
                    self.stopper(proc)
                except Exception:   # a stopper that fails must not keep the child alive
                    kill_tree(proc)
            else:
                kill_tree(proc)
            try:
                proc.wait(timeout=30)
            except (subprocess.TimeoutExpired, OSError):
                pass
        elif self.state == 'running':
            self.state = 'idle'

    def stop_begin(self):
        """Ask the child to end and return at once; stop_poll() says when it has. Play and Pause use it: the libraries
        in the driver's hands are finished while the window stays responsive (a build killed while it writes the
        driver's cache may leave that damaged, and the game starts on it next). Without an asker: stop() at once."""
        if self.state != 'running' or not self.proc:
            return
        if self.asker is None:
            self.stop()
            return
        try:
            self.asker()
        except Exception:   # an asker that fails: the hard way
            self.stop()
            return
        self.state, self.stop_since = 'stopping', self.clock()

    def stop_poll(self, wait):
        """True once the child a stop_begin asked has ended; one still there after wait seconds is stopped the hard way
        (stop(): the stopper's grace, then its tree is killed)."""
        if self.state != 'stopping':
            return True
        if self.proc.poll() is None and self.clock() - self.stop_since < wait:
            return False
        self.state = 'running'   # (stop() ends a running job: the stopper returns at once when the child has ended)
        self.stop()
        return True

    def pause(self):
        self.paused = True
        self.stop_begin()

    def resume(self):
        self.paused = False

    def remaining(self, n_cores=None):
        """Seconds the job should take yet: the running phase's count left at its own measured pace (eta.Rate) and
        the phases after it by their shares of the step's estimate (eta.phase_left), else that estimate by the share
        done; None when nothing is known."""
        if self.state != 'running':
            return None
        step = self.step or 'background'
        base = eta.estimate(step, 'first', n_cores)[0]
        if self.phase is not None:
            mine, after = eta.phase_left(step, self.phase, self.done, self.total, self.rate.rate(self.clock()), base, 0.0)
            if mine is not None:
                return mine + after
        return base * (1 - self.fraction)
