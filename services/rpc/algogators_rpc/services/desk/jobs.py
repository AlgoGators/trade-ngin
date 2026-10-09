"""Running commands: one FIFO queue and worker thread per portfolio.

Jobs for the same portfolio run one at a time, in the order they were queued; different
portfolios run concurrently. An engine job spawns

    flock <lock_dir>/<portfolio_id>.lock <engine_binary> --<mode> --portfolio-config <dir>
          --date YYYY-MM-DD --audit-id N

in <engine_cwd>. The daily cron model run takes the same lock, so a desk command and the model
run never overlap. The binary records the row's outcome itself (status, result, message,
finished_at); exit 0 = done, 2 = refused, anything else = failed. If the row is still 'running'
after the process exits (a crash, a kill, a timeout), the agent marks it failed.
"""

from __future__ import annotations

import logging
import os
import queue
import signal
import subprocess
import threading
from collections import deque
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Sequence

from .command_store import CommandStore
from .config import CommandSettings

log = logging.getLogger("desk.jobs")

TAIL_LINES = 40
MODES = ("desk", "override", "publish")


@dataclass(frozen=True)
class RunResult:
    rc: Optional[int]
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    # Set when the process could not be started at all.
    error: Optional[str] = None


class Runner:
    """Spawns a process. Tests inject a fake with the same `run`."""

    def run(self, argv: Sequence[str], cwd: str, timeout: float) -> RunResult:
        raise NotImplementedError


class SubprocessRunner(Runner):
    def run(self, argv: Sequence[str], cwd: str, timeout: float) -> RunResult:
        posix = os.name == "posix"
        try:
            # A new session, so a timeout kills flock and the engine under it together.
            proc = subprocess.Popen(list(argv), cwd=cwd, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                    text=True, errors="replace", start_new_session=posix)
        except OSError as exc:
            return RunResult(rc=None, error=f"{type(exc).__name__}: {exc.strerror or exc}")
        try:
            out, err = proc.communicate(timeout=timeout)
            return RunResult(rc=proc.returncode, stdout=out or "", stderr=err or "")
        except subprocess.TimeoutExpired:
            if posix:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    proc.kill()
            else:
                proc.kill()
            out, err = proc.communicate()
            return RunResult(rc=proc.returncode, stdout=out or "", stderr=err or "",
                             timed_out=True)


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
    """A unit of work for one portfolio's queue."""

    portfolio_id: str
    audit_id: int
    kind: str
    work: Callable[[], None]

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
            try:
                job.run()
            except Exception as exc:
                log.error("job crashed", exc_info=True,
                          extra={"portfolio_id": portfolio_id, "audit_id": job.audit_id,
                                 "kind": job.kind, "error": type(exc).__name__})
                self._fail_if_running(job.audit_id, f"agent error running the job: "
                                                    f"{type(exc).__name__}")
            finally:
                q.task_done()

    # -- engine jobs ---------------------------------------------------------------------------

    def engine_job(self, mode: str, portfolio_id: str, portfolio_dir: str, date: str,
                   audit_id: int, kind: str) -> Job:
        argv = engine_argv(self._settings, mode, portfolio_id, portfolio_dir, date, audit_id)
        return Job(portfolio_id, audit_id, kind,
                   lambda: self._run_engine(argv, portfolio_id, audit_id, mode))

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
        """The engine owns the outcome; only a row it left 'running' is failed here."""
        try:
            row = self._store.get_row(audit_id)
            if row is not None and row.status == "running":
                if self._store.finish(audit_id, "failed", None, message):
                    log.warning("row failed by the agent",
                                extra={"audit_id": audit_id, "status": "failed",
                                       "error": message})
        except Exception as exc:
            log.error("cannot record the job outcome",
                      extra={"audit_id": audit_id, "error": type(exc).__name__})
