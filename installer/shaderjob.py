"""The launcher's background shader preparation, not started since 2026-11-19 (gui_oot.py RekarinaApp.shader_wanted):
every setup run prepares every variant in the foreground (installer/oot.py shader_mode). What follows is how 1.2.1 to
1.2.3 worked.

A first install prepared the shader variants play met (installer/oot.py, mode 'met'), which is all a player needs at
once. The rest of the variants (five of six) are prepared by the launcher itself, while it is open and the game is not
running: this module runs the setup (`python -m installer.oot --all-shaders ...`: every finished step is skipped, the
shader step prepares the variants that are missing, the package step puts them into the game folder) as a child at
below-normal priority on all cores but one, and reads its JSON-lines events (events.py) for the counts.

    job = ShaderJob(command, cwd)
    job.start()            # a child at low priority; states: idle, running, paused, done, failed
    job.poll()             # drain the child's events; True when what the window shows changed
    job.stop()             # cleanly: the whole tree is ended, what was translated and built stays (the preparation resumes)
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

    def __init__(self, command, cwd, env=None, clock=time.monotonic, popen=subprocess.Popen):
        self.command, self.cwd, self.env, self.clock, self.popen = command, cwd, env, clock, popen
        self.state = 'idle'
        self.proc = None
        self.events = queue.Queue()
        self.reset_counts()
        self.error = None      # the setup's error id when it failed
        self.paused = False    # the player's Pause: until the next start of the launcher

    def reset_counts(self):
        self.phase = self.done = self.total = None
        self.started = None
        self.fraction = 0.0
        self.first_fraction = None

    @property
    def pid(self):
        return self.proc.pid if self.proc else None

    @property
    def active(self):
        return self.state == 'running'

    def start(self):
        """Run the setup as a child. False when one is running or the job is over (done, or failed: not retried by itself)."""
        if self.state in ('running', 'done', 'failed') or self.paused:
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
            if kind == 'progress' and e.get('step') == 'oot_prepare':
                self.phase, self.done, self.total = e.get('phase'), e.get('done'), e.get('total')
                f = eta.step_fraction('oot_prepare', self.phase, self.done, self.total, [])
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
        """End the child's whole tree and wait for it. The job can start again (start(): it continues)."""
        proc = self.proc
        if self.state == 'running' and proc:
            self.state = 'idle'
            kill_tree(proc)
            try:
                proc.wait(timeout=30)
            except (subprocess.TimeoutExpired, OSError):
                pass
        elif self.state == 'running':
            self.state = 'idle'

    def pause(self):
        self.paused = True
        self.stop()

    def resume(self):
        self.paused = False

    def remaining(self, n_cores=None):
        """Seconds the job should take yet: from its progress so far and the time it has taken since, else the
        estimate of the step; None when nothing is known."""
        if self.state != 'running':
            return None
        f = self.fraction
        base = eta.estimate('oot_prepare', 'all', n_cores)[0]
        if self.first_fraction and f - self.first_fraction[0] >= 0.03:
            f0, t0 = self.first_fraction
            return max(0.0, (self.clock() - t0) * (1 - f) / (f - f0))
        return base * (1 - f)
