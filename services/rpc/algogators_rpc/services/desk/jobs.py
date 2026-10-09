"""Running commands: one FIFO queue and worker thread per portfolio.

Jobs for the same portfolio run one at a time, in the order they were queued; different
portfolios run concurrently. An engine job spawns

    flock <lock_dir>/<portfolio_id>.lock <engine_binary> --<mode> --portfolio-config <dir>
          --date YYYY-MM-DD --audit-id N

in <engine_cwd>. The catch-up scheduler (catchup.py) takes the same lock file for its model runs,
so a desk command and a model run never overlap. The binary records the row's outcome itself
(status, result, message, finished_at); exit 0 = done, 2 = refused, anything else = failed. If
the row is still 'running' after the process exits (a crash, a kill, a timeout), the agent marks
it failed. If even that write fails (the database is down), the row is reported back to the
dispatcher as abandoned, and the recovery task re-drives it (recovery.py).

The process's stdout and stderr go to temporary files, never to memory, so a chatty engine
cannot exhaust the container (R#8); only the last lines are read back for the log.
"""

from __future__ import annotations

import logging
import os
import queue
import signal
import subprocess
import tempfile
import threading
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence

from .command_store import CommandStore
from .config import CommandSettings

log = logging.getLogger("desk.jobs")

TAIL_LINES = 40
# How much of the end of each output file is read back for the tail.
TAIL_BYTES = 64 * 1024
MODES = ("desk", "override", "publish")


@dataclass(frozen=True)
class RunResult:
    rc: Optional[int]
    # The last TAIL_BYTES of each stream (never the whole output).
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    # Set when the process could not be started at all.
    error: Optional[str] = None


class Runner:
    """Spawns a process. Tests inject a fake with the same `run`."""

    def run(self, argv: Sequence[str], cwd: str, timeout: float) -> RunResult:
        raise NotImplementedError


def _read_tail(f, limit: int = TAIL_BYTES) -> str:
    f.flush()
    size = f.seek(0, os.SEEK_END)
    f.seek(max(0, size - limit))
    data = f.read()
    text = data.decode("utf-8", errors="replace")
    if size > limit:  # drop the partial first line
        text = text.split("\n", 1)[-1]
    return text


class SubprocessRunner(Runner):
    """Runs argv in its own session with stdout/stderr in temporary files. `copy_to`, when set,
    is a file object the full stdout+stderr is appended to afterwards (the catch-up log)."""

    def run(self, argv: Sequence[str], cwd: str, timeout: float,
            copy_to=None) -> RunResult:
        posix = os.name == "posix"
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            try:
                # A new session, so a timeout kills flock and the engine under it together.
                proc = subprocess.Popen(list(argv), cwd=cwd, stdout=out, stderr=err,
                                        stdin=subprocess.DEVNULL, start_new_session=posix)
            except OSError as exc:
                return RunResult(rc=None,
                                 error=f"{type(exc).__name__}: {exc.strerror or exc}")
            timed_out = False
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                if posix:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except OSError:
                        proc.kill()
                else:
                    proc.kill()
                proc.wait()
            if copy_to is not None:
                for f in (out, err):
                    f.seek(0)
                    while True:
                        chunk = f.read(1 << 16)
                        if not chunk:
                            break
                        copy_to.write(chunk.decode("utf-8", errors="replace"))
            return RunResult(rc=proc.returncode, stdout=_read_tail(out), stderr=_read_tail(err),
                             timed_out=timed_out)


def tail(text: str, n: int = TAIL_LINES) -> List[str]:
    return list(deque((line for line in (text or "").splitlines() if line.strip()), maxlen=n))


def engine_argv(settings: CommandSettings, mode: str, portfolio_id: str, portfolio_dir: str,
                date: str, audit_id: int) -> List[str]:
    if mode not in MODES:
        raise ValueError(f"unknown engine mode {mode}")
    lock = os.path.join(settings.lock_dir, f"{portfolio_id}.lock")
    return [settings.flock, lock, settings.engine_binary, f"--{mode}",
            "--portfolio-config", portfolio_dir, "--date", date, "--audit-id", str(audit_id)]


@dataclass
class Job:
    """A unit of work for one portfolio's queue. `on_done(abandoned)` runs after the work,
    whatever happened; `abandoned` is True when the row may still be 'running' with nobody
    on it (the outcome could not be recorded)."""

    portfolio_id: str
    audit_id: int
    kind: str
    work: Callable[[], None]
    on_done: Optional[Callable[[bool], None]] = field(default=None, repr=False)

    def run(self) -> None:
        self.work()


class JobManager:
    def __init__(self, store: CommandStore, settings: CommandSettings,
                 runner: Optional[Runner] = None):
        self._store = store
        self._settings = settings
        self._runner = runner or SubprocessRunner()
        self._queues: Dict[str, "queue.Queue[Job]"] = {}
        self._lock = threading.Lock()
        self._abandoned = threading.local()

    # -- queueing ------------------------------------------------------------------------------

    def submit(self, job: Job) -> None:
        with self._lock:
            q = self._queues.get(job.portfolio_id)
            if q is None:
                q = queue.Queue()
                self._queues[job.portfolio_id] = q
                threading.Thread(target=self._worker, args=(job.portfolio_id, q), daemon=True,
                                 name=f"jobs-{job.portfolio_id}").start()
        log.info("job queued", extra={"portfolio_id": job.portfolio_id,
                                      "audit_id": job.audit_id, "kind": job.kind})
        q.put(job)

    def wait_idle(self) -> None:
        """Block until every queued job has finished (tests, shutdown)."""
        with self._lock:
            queues = list(self._queues.values())
        for q in queues:
            q.join()

    def _worker(self, portfolio_id: str, q: "queue.Queue[Job]") -> None:
        while True:
            job = q.get()
            self._abandoned.value = False
            try:
                job.run()
            except Exception as exc:
                log.error("job crashed", exc_info=True,
                          extra={"portfolio_id": portfolio_id, "audit_id": job.audit_id,
                                 "kind": job.kind, "error": type(exc).__name__})
                self._fail_if_running(job.audit_id, f"agent error running the job: "
                                                    f"{type(exc).__name__}")
            finally:
                abandoned = bool(getattr(self._abandoned, "value", False))
                if job.on_done is not None:
                    try:
                        job.on_done(abandoned)
                    except Exception:  # never let bookkeeping kill the worker
                        log.error("job completion hook failed", exc_info=True,
                                  extra={"audit_id": job.audit_id})
                q.task_done()

    def mark_abandoned(self) -> None:
        """Called from inside a job: its outcome could not be recorded."""
        self._abandoned.value = True

    # -- engine jobs ---------------------------------------------------------------------------

    def engine_job(self, mode: str, portfolio_id: str, portfolio_dir: str, date: str,
                   audit_id: int, kind: str,
                   on_done: Optional[Callable[[bool], None]] = None) -> Job:
        argv = engine_argv(self._settings, mode, portfolio_id, portfolio_dir, date, audit_id)
        return Job(portfolio_id, audit_id, kind,
                   lambda: self._run_engine(argv, portfolio_id, audit_id, mode), on_done)

    def _run_engine(self, argv: List[str], portfolio_id: str, audit_id: int, mode: str) -> None:
        s = self._settings
        try:
            os.makedirs(s.lock_dir, exist_ok=True)
        except OSError:
            pass  # flock reports it, and the row is failed below
        log.info("engine started", extra={"portfolio_id": portfolio_id, "audit_id": audit_id,
                                          "mode": mode, "argv": argv})
        res = self._runner.run(argv, s.engine_cwd, s.job_timeout_s)
        out_tail, err_tail = tail(res.stdout), tail(res.stderr)
        log.info("engine exited", extra={"portfolio_id": portfolio_id, "audit_id": audit_id,
                                         "mode": mode, "rc": res.rc, "timed_out": res.timed_out,
                                         "error": res.error, "stdout_tail": out_tail,
                                         "stderr_tail": err_tail})
        if res.error is not None:
            message = f"cannot start the engine: {res.error}"
        elif res.timed_out:
            message = f"engine timed out after {s.job_timeout_s:g}s and was killed"
        else:
            last = err_tail[-1] if err_tail else "(no stderr)"
            message = f"engine exited {res.rc} without recording an outcome: {last}"
        self._fail_if_running(audit_id, message)

    def _fail_if_running(self, audit_id: int, message: str) -> None:
        """The engine owns the outcome; only a row it left 'running' is failed here. If the
        database cannot be read or written, the row is abandoned for the recovery task."""
        try:
            row = self._store.get_row(audit_id)
            if row is not None and row.status == "running":
                if self._store.finish(audit_id, "failed", None, message):
                    log.warning("row failed by the agent",
                                extra={"audit_id": audit_id, "status": "failed",
                                       "error": message})
        except Exception as exc:
            self.mark_abandoned()
            log.error("cannot record the job outcome; the recovery task will re-drive the row",
                      extra={"audit_id": audit_id, "error": type(exc).__name__})
