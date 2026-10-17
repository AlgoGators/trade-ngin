"""HD D-17: CheckSettings asks the engine's qt_settings_check whether a live run would accept a
desk settings change, before AlgoLens saves it. The binary itself is tested in C++
(tests/live/test_live_settings_check.cpp); here, how the desk service runs it and answers."""

import json
import subprocess

import grpc
import pytest

from conftest import PDIR, PID
from algogators import desk_pb2 as pb
from algogators_rpc.services.desk.servicer import DeskServicer
from algogators_rpc.services.desk.settings_check import (
    SettingsChecker,
    SettingsCheckUnavailable,
)


class Proc:
    def __init__(self, returncode, stdout="", stderr=""):
        self.returncode, self.stdout, self.stderr = returncode, stdout, stderr


def checker(settings, proc=None, raises=None):
    calls = []

    def run(argv, **kw):
        with open(argv[argv.index("--overrides-file") + 1], encoding="utf-8") as f:
            calls.append((argv, kw, json.load(f)))
        if raises:
            raise raises
        return proc

    return SettingsChecker(settings, run=run), calls


def test_the_check_runs_next_to_the_engine_with_the_overrides(settings):
    check, calls = checker(settings, Proc(0, '{"ok": true}\n'))
    assert check.check(PDIR, {"risk": {"max_drawdown": 0.25}}) == (True, "")
    argv, kw, overrides = calls[0]
    assert argv[0] == "/app/build/bin/Release/qt_settings_check"
    assert argv[1:4] == ["--portfolio-config", PDIR, "--overrides-file"]
    assert kw["cwd"] == "/app" and overrides == {"risk": {"max_drawdown": 0.25}}


def test_a_refusal_carries_the_reason_a_run_would_give(settings):
    check, _ = checker(settings, Proc(3, 'log line\n{"ok": false, "error": "R_max must be at most 10"}\n'))
    assert check.check(PDIR, {}) == (False, "R_max must be at most 10")


def test_config_files_that_do_not_load_make_the_check_unavailable_with_the_reason(settings):
    # Exit 4: the engine is misconfigured; the change cannot be judged, so it is not "refused".
    check, _ = checker(settings, Proc(4, '{"ok": false, "error": "the config files do not load: x"}
'))
    with pytest.raises(SettingsCheckUnavailable, match="the config files do not load"):
        check.check(PDIR, {})


@pytest.mark.parametrize("proc, raises", [
    (Proc(1, "", "Segmentation fault"), None),
    (Proc(0, "not json"), None),
    (None, OSError("no such file")),
    (None, subprocess.TimeoutExpired("x", 60)),
])
def test_a_check_that_cannot_run_is_unavailable_not_ok(settings, proc, raises):
    check, _ = checker(settings, proc, raises)
    with pytest.raises(SettingsCheckUnavailable):
        check.check(PDIR, {})


class Ctx:
    def __init__(self):
        self.aborted = None

    def abort(self, code, msg):
        self.aborted = (code, msg)
        raise RuntimeError(msg)


class FakeChecker:
    def __init__(self, answer=(True, ""), error=None):
        self.answer, self.error, self.calls = answer, error, []

    def check(self, portfolio_dir, overrides):
        self.calls.append((portfolio_dir, overrides))
        if self.error:
            raise SettingsCheckUnavailable(self.error)
        return self.answer


def call(servicer, overrides_json, pid=PID):
    ctx = Ctx()
    try:
        reply = servicer.CheckSettings(pb.SettingsCheckRequest(portfolio_id=pid, overrides_json=overrides_json), ctx)
    except RuntimeError:
        return None, ctx
    return reply, ctx


def test_rpc_answers_the_check(store, config_dir):
    fake = FakeChecker((False, "max_gross_leverage must be at most 10"))
    reply, _ = call(DeskServicer(store, config_dir=str(config_dir), settings_checker=fake), '{"risk": {}}')
    assert (reply.ok, reply.error) == (False, "max_gross_leverage must be at most 10")
    assert fake.calls == [(PDIR, {"risk": {}})]


@pytest.mark.parametrize("payload", ["[1]", "not json", ""])
def test_rpc_refuses_overrides_that_are_not_an_object(store, config_dir, payload):
    _, ctx = call(DeskServicer(store, config_dir=str(config_dir), settings_checker=FakeChecker()), payload)
    assert ctx.aborted[0] == grpc.StatusCode.INVALID_ARGUMENT


def test_rpc_is_unavailable_when_the_check_cannot_run(store, config_dir):
    servicer = DeskServicer(store, config_dir=str(config_dir), settings_checker=FakeChecker(error="exited 1"))
    _, ctx = call(servicer, "{}")
    assert ctx.aborted == (grpc.StatusCode.UNAVAILABLE, "exited 1")
    _, ctx = call(DeskServicer(store, config_dir=str(config_dir)), "{}")
    assert ctx.aborted[0] == grpc.StatusCode.UNAVAILABLE


def test_rpc_refuses_a_portfolio_with_no_config(store, config_dir):
    _, ctx = call(DeskServicer(store, config_dir=str(config_dir), settings_checker=FakeChecker()), "{}",
                  pid="NO_SUCH_PORTFOLIO")
    assert ctx.aborted[0] == grpc.StatusCode.FAILED_PRECONDITION
