import dataclasses
import datetime as dt
import json
import threading

import grpc
import pytest

from algogators_rpc.client import versioned_channel
from algogators_rpc.registry import Registry
from algogators_rpc.server import build_server

from algogators_rpc.services.desk.command_store import BookLine, CommandRow
from algogators_rpc.services.desk.commands import Dispatcher
from algogators_rpc.services.desk.config import CommandSettings
from algogators_rpc.services.desk.jobs import JobManager, RunResult
from algogators_rpc.services.desk.service import build_service
from algogators_rpc.services.desk.store import RunFacts, StoreError

UTC = dt.timezone.utc
PID = "QT_CONSERVATIVE_PORTFOLIO"
PDIR = "qt_conservative"
DAY = dt.date(2026, 10, 7)
NY = __import__("zoneinfo").ZoneInfo("America/New_York")
# Contract C7: an approval counts by its row's created_at against 10:00 New York on its date, and
# the dispatcher's clock decides --send-now (09:30). The fixtures approve at 08:00 and dispatch at
# 08:30 on DAY, before the send; tests of the cutoff move both.
PUBLISH_CREATED = dt.datetime(2026, 10, 7, 8, 0, tzinfo=NY)
DISPATCH_AT = dt.datetime(2026, 10, 7, 8, 30, tzinfo=NY)


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
                created_at=PUBLISH_CREATED if kind == "publish" else now())
    base.update(kw)
    return CommandRow(**base)


class FakeCommandStore:
    """trading.position_overrides in memory, with the claim semantics of the real UPDATEs and
    the 025 transitions (pending -> running -> done/refused/failed; running -> pending only by
    requeue; terminal rows final). `error` makes every call fail like a database that is down;
    `fail` maps a method name to how many of its next calls fail."""

    TERMINAL = ("done", "refused", "failed")

    def __init__(self, rows=()):
        self.rows = {r.id: r for r in rows}
        self.book_data = {"system": {}, "qt_proposal": {}, "qt": {}}
        self.published = {}          # (portfolio_id, date) -> published_at
        self.lock = threading.Lock()
        self.claims = []
        self.requeues = []
        self.error = None
        self.fail = {}

    def add(self, row):
        self.rows[row.id] = row
        return row

    def _check(self, name=""):
        if self.error:
            raise StoreError(self.error)
        if self.fail.get(name):
            self.fail[name] -= 1
            raise StoreError(f"database error (OperationalError) in {name}")

    def _set(self, audit_id, **changes):
        old = self.rows[audit_id]
        new_status = changes.get("status", old.status)
        assert old.status not in self.TERMINAL, f"row {audit_id} is final ({old.status})"
        assert (old.status, new_status) in (("pending", "running"), ("running", "done"),
                                            ("running", "refused"), ("running", "failed"),
                                            ("running", "pending"), ("running", "running"),
                                            ("pending", "pending")), (old.status, new_status)
        self.rows[audit_id] = dataclasses.replace(old, **changes)
        return self.rows[audit_id]

    def get_row(self, audit_id):
        self._check("get_row")
        with self.lock:
            return self.rows.get(audit_id)

    def claim(self, audit_id):
        self._check("claim")
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "pending":
                return None
            self.claims.append(audit_id)
            return self._set(audit_id, status="running", started_at=now())

    def finish(self, audit_id, status, result, message):
        self._check("finish")
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "running":
                return False
            self._set(audit_id, status=status, result=result, message=message,
                      finished_at=now())
            return True

    def record_mail(self, audit_id, token_hash, hours, result):
        self._check("record_mail")
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "running":
                return None
            return self._set(audit_id, token_hash=token_hash, result=result,
                             token_expires_at=now() + dt.timedelta(hours=hours)).token_expires_at

    def running_rows(self):
        self._check("running_rows")
        with self.lock:
            t = now()
            return [(r, None if r.started_at is None else (t - r.started_at).total_seconds())
                    for _, r in sorted(self.rows.items()) if r.status == "running"]

    def requeue(self, audit_id, message):
        self._check("requeue")
        with self.lock:
            row = self.rows.get(audit_id)
            if row is None or row.status != "running":
                return False
            self.requeues.append(audit_id)
            self._set(audit_id, status="pending", started_at=None, finished_at=None,
                      message=message)
            return True

    def pending_rows(self):
        self._check("pending_rows")
        with self.lock:
            return [r for _, r in sorted(self.rows.items()) if r.status == "pending"]

    def decisions_for(self, parent_id):
        self._check("decisions_for")
        with self.lock:
            return [r for _, r in sorted(self.rows.items())
                    if r.kind == "override_decision" and r.parent_id == parent_id]

    def published_at(self, portfolio_id, date):
        self._check("published_at")
        return self.published.get((portfolio_id, date))

    def newest_pending_publish(self, portfolio_id, date):
        self._check("newest_pending_publish")
        with self.lock:
            rows = [r for r in self.rows.values() if r.kind == "publish" and r.status == "pending"
                    and r.portfolio_id == portfolio_id and r.date == date]
            return max(rows, key=lambda r: r.id) if rows else None

    def books(self, portfolio_id, date):
        self._check("books")
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

    def run(self, argv, cwd, timeout, copy_to=None):
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
        self.now = DISPATCH_AT
        self.jobs = JobManager(cstore, settings, runner)
        self.dispatcher = Dispatcher(cstore, self.jobs, settings,
                                     sender=lambda cfg, to, subject, body:
                                     self.sent.append((cfg, list(to), subject, body)),
                                     clock=lambda: self.now)

    def wait(self):
        self.jobs.wait_idle()


@pytest.fixture
def agent(cstore, runner, settings):
    return Agent(cstore, runner, settings)


@pytest.fixture
def registry(store, agent):
    registry = Registry()
    registry.register(build_service(store, agent.dispatcher))
    return registry


@pytest.fixture
def server_port(registry):
    server, _, port = build_server(registry, "127.0.0.1:0", max_workers=2)
    server.start()
    yield port
    server.stop(grace=None)


@pytest.fixture
def channel(server_port):
    """A client channel that sends the version header, as AlgoLens' does."""
    with versioned_channel(f"127.0.0.1:{server_port}") as ch:
        yield ch


@pytest.fixture
def raw_channel(server_port):
    """A client channel without the version header."""
    with grpc.insecure_channel(f"127.0.0.1:{server_port}") as ch:
        yield ch


__all__ = ["BookLine"]
