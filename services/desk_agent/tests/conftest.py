import dataclasses
import datetime as dt
import json
import threading

import grpc
import pytest

from desk_agent.command_store import BookLine, CommandRow
from desk_agent.commands import Dispatcher
from desk_agent.config import CommandSettings
from desk_agent.jobs import JobManager, RunResult
from desk_agent.server import build_server
from desk_agent.store import RunFacts, StoreError

UTC = dt.timezone.utc
PID = "QT_CONSERVATIVE_PORTFOLIO"
PDIR = "qt_conservative"
DAY = dt.date(2026, 10, 7)


class FakeStore:
    """The servicer's only DB dependency, scripted per test."""

    def __init__(self):
        self.facts = {}
        self.error = None
        self.calls = []

    def run_facts(self, portfolio_id, date):
        self.calls.append((portfolio_id, date))
        if self.error:
            raise StoreError(self.error)
        return self.facts.get((portfolio_id, date), RunFacts())


def now():
    return dt.datetime.now(UTC)


def make_row(id, kind, **kw):
    base = dict(id=id, portfolio_id=PID, date=DAY, kind=kind, status="pending",
                requested_by="desk@algogators.com", reason="test reason", payload={},
                created_at=now())
    base.update(kw)
    return CommandRow(**base)


class FakeCommandStore:
    """trading.position_overrides in memory, with the claim semantics of the real UPDATEs."""

    def __init__(self, rows=()):
        self.rows = {r.id: r for r in rows}
        self.book_data = {"system": {}, "qt_proposal": {}, "qt": {}}
        self.lock = threading.Lock()
        self.claims = []
        self.error = None

    def add(self, row):
        self.rows[row.id] = row
        return row

    def _check(self):
        if self.error:
            raise StoreError(self.error)

    def _set(self, audit_id, **changes):
        self.rows[audit_id] = dataclasses.replace(self.rows[audit_id], **changes)
        return self.rows[audit_id]

    def get_row(self, audit_id):
        self._check()
        with self.lock:
            return self.rows.get(audit_id)

    def claim(self, audit_id):
        self._check()
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "pending":
                return None
            self.claims.append(audit_id)
            return self._set(audit_id, status="running", started_at=now())

    def finish(self, audit_id, status, result, message):
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "running":
                return False
            self._set(audit_id, status=status, result=result, message=message,
                      finished_at=now())
            return True

    def set_token(self, audit_id, token_hash, hours):
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "running":
                return None
            return self._set(audit_id, token_hash=token_hash,
                             token_expires_at=now() + dt.timedelta(hours=hours)).token_expires_at

    def reset_running(self):
        with self.lock:
            ids = sorted(i for i, r in self.rows.items() if r.status == "running")
            for i in ids:
                self._set(i, status="pending", started_at=None,
                          message="re-driven after agent restart")
            return ids

    def pending_rows(self):
        self._check()
        with self.lock:
            return [r for _, r in sorted(self.rows.items()) if r.status == "pending"]

    def newest_pending_publish(self, portfolio_id, date):
        with self.lock:
            rows = [r for r in self.rows.values() if r.kind == "publish" and r.status == "pending"
                    and r.portfolio_id == portfolio_id and r.date == date]
            return max(rows, key=lambda r: r.id) if rows else None

    def books(self, portfolio_id, date):
        return {k: dict(v) for k, v in self.book_data.items()}


class FakeRunner:
    """Stands in for the subprocess. `behave(argv)` returns a RunResult and may play the engine
    by recording the row's outcome on the store."""

    def __init__(self, store, behave=None):
        self.store = store
        self.calls = []
        self.behave = behave or self.engine_records("done")
        self.lock = threading.Lock()

    def engine_records(self, status, rc=0):
        def behave(argv):
            audit_id = int(argv[argv.index("--audit-id") + 1])
            self.store.finish(audit_id, status, {"by": "engine"}, f"engine said {status}")
            return RunResult(rc=rc, stdout="ok\n", stderr="")
        return behave

    def run(self, argv, cwd, timeout):
        with self.lock:
            self.calls.append((list(argv), cwd, timeout))
        return self.behave(list(argv))


def write_portfolio(config_dir, dirname=PDIR, portfolio_id=PID, email=None):
    d = config_dir / "portfolios" / dirname
    d.mkdir(parents=True, exist_ok=True)
    (d / "portfolio.json").write_text(json.dumps({"portfolio_id": portfolio_id}))
    if email is not None:
        (d / "email.json").write_text(json.dumps(email))
    return d


@pytest.fixture
def store():
    return FakeStore()


@pytest.fixture
def config_dir(tmp_path):
    root = tmp_path / "config"
    write_portfolio(root)
    return root


@pytest.fixture
def settings(config_dir, tmp_path):
    return CommandSettings(engine_binary="/app/build/bin/Release/live_portfolio_conservative",
                           engine_cwd="/app", config_dir=str(config_dir),
                           lock_dir=str(tmp_path / "locks"), job_timeout_s=30,
                           approvers="vp=vp@algogators.com,president=pres@algogators.com",
                           email_disabled=True)


@pytest.fixture
def cstore():
    return FakeCommandStore()


@pytest.fixture
def runner(cstore):
    return FakeRunner(cstore)


class Agent:
    """A dispatcher wired to fakes, with the mail sender recorded."""

    def __init__(self, cstore, runner, settings):
        self.store, self.runner, self.settings = cstore, runner, settings
        self.sent = []
        self.jobs = JobManager(cstore, settings, runner)
        self.dispatcher = Dispatcher(cstore, self.jobs, settings,
                                     sender=lambda cfg, to, subject, body:
                                     self.sent.append((cfg, list(to), subject, body)))

    def wait(self):
        self.jobs.wait_idle()


@pytest.fixture
def agent(cstore, runner, settings):
    return Agent(cstore, runner, settings)


@pytest.fixture
def channel(store, agent):
    server, _, port = build_server(store, "127.0.0.1:0", max_workers=2,
                                   dispatcher=agent.dispatcher)
    server.start()
    with grpc.insecure_channel(f"127.0.0.1:{port}") as ch:
        yield ch
    server.stop(grace=None)


__all__ = ["BookLine"]
