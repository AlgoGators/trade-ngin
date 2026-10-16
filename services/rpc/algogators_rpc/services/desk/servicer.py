"""DeskService implementation.

GetRunStatus and GetDeskRisk (contract C9) read Postgres. The four commands check their trading.position_overrides row,
claim it and queue the work (commands.py), then answer ACCEPTED at once; the outcome lands on
the row, which AlgoLens polls (contract section 6). A row that is not pending is never run
again: the reply says its current status. A request that does not match its row (unknown id,
wrong kind, another portfolio or date) is REFUSED and the row is left alone.

Malformed input is answered with gRPC status INVALID_ARGUMENT on every RPC, so a caller bug is
never confused with an engine refusal. A database that cannot answer is UNAVAILABLE: retry with
the same audit_id.
"""

from __future__ import annotations

import logging
import time
from typing import Optional

import grpc
from google.protobuf.timestamp_pb2 import Timestamp

from algogators import desk_pb2, desk_pb2_grpc

from . import risk as rk
from . import status as st
from . import validation as v
from .commands import ACCEPTED, DONE, FAILED, REFUSED, Dispatcher, Outcome
from .config import DEFAULT_CONFIG_DIR
from .portfolios import PortfolioConfigError, portfolios_root, resolve_portfolio_dir
from .store import RunStatusStore, StoreError

log = logging.getLogger("desk.rpc")

_STATE = {
    st.NOT_STARTED: desk_pb2.RUN_STATE_NOT_STARTED,
    st.RUNNING: desk_pb2.RUN_STATE_RUNNING,
    st.SUCCEEDED: desk_pb2.RUN_STATE_SUCCEEDED,
    st.REFUSED: desk_pb2.RUN_STATE_REFUSED,
    st.FAILED: desk_pb2.RUN_STATE_FAILED,
}

_CHECK_STATUS = {
    rk.WITHIN: desk_pb2.RISK_CHECK_STATUS_WITHIN,
    rk.BOUND: desk_pb2.RISK_CHECK_STATUS_BOUND,
    rk.OVER: desk_pb2.RISK_CHECK_STATUS_OVER,
    rk.NOT_MEASURED: desk_pb2.RISK_CHECK_STATUS_NOT_MEASURED,
}


def _risk_check(check):
    out = desk_pb2.RiskCheck(term=check.term, label=check.label, unit=check.unit,
                             status=_CHECK_STATUS[check.status], detail=check.detail)
    if check.value is not None:
        out.value = check.value
    if check.max is not None:
        out.max = check.max
    return out


def _multiplier(x):
    out = desk_pb2.RiskMultiplier(name=x.name, label=x.label, detail=x.detail)
    if x.value is not None:
        out.value = x.value
    return out


_COMMAND_STATUS = {
    ACCEPTED: desk_pb2.COMMAND_STATUS_ACCEPTED,
    DONE: desk_pb2.COMMAND_STATUS_DONE,
    REFUSED: desk_pb2.COMMAND_STATUS_REFUSED,
    FAILED: desk_pb2.COMMAND_STATUS_FAILED,
}

_RISK_STATE = {
    rk.NO_BOOK: desk_pb2.DESK_RISK_STATE_NO_BOOK,
    rk.MODEL: desk_pb2.DESK_RISK_STATE_MODEL,
    rk.DESK: desk_pb2.DESK_RISK_STATE_DESK,
    rk.OVERRIDE: desk_pb2.DESK_RISK_STATE_OVERRIDE,
}

NO_DISPATCHER = "commands are not configured on this agent"


def _ts(value):
    if value is None:
        return None
    t = Timestamp()
    t.FromDatetime(value)
    return t


class DeskServicer(desk_pb2_grpc.DeskServiceServicer):
    def __init__(self, store: RunStatusStore, dispatcher: Optional[Dispatcher] = None,
                 config_dir: Optional[str] = None):
        self._store = store
        self._config_dir = config_dir or DEFAULT_CONFIG_DIR
        self._dispatcher = dispatcher

    # -- helpers ---------------------------------------------------------------------------

    @staticmethod
    def _invalid(context, rpc: str, exc: v.InvalidArgument, **fields):
        log.warning("invalid request", extra={"rpc": rpc, "status": "INVALID_ARGUMENT",
                                              "error": str(exc), **fields})
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(exc))

    def _command(self, rpc: str, context, call, **fields) -> Outcome:
        started = time.monotonic()
        if self._dispatcher is None:
            out = Outcome(REFUSED, NO_DISPATCHER)
        else:
            try:
                out = call(self._dispatcher)
            except StoreError as exc:
                log.error("command log unavailable",
                          extra={"rpc": rpc, "error": str(exc), **fields})
                context.abort(grpc.StatusCode.UNAVAILABLE, f"command log unavailable: {exc}")
        log.info("command", extra={
            "rpc": rpc, "status": out.status.upper(), "error": out.message, **fields,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1)})
        return out

    # -- commands ------------------------------------------------------------------------------

    def RunDesk(self, request, context):
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
            v.audit_id(request.audit_id)
            v.text("requested_by", request.requested_by)
        except v.InvalidArgument as exc:
            self._invalid(context, "RunDesk", exc, audit_id=request.audit_id)
        out = self._command("RunDesk", context,
                            lambda d: d.run_desk(request.audit_id, pid, day),
                            portfolio_id=pid, date=str(day), audit_id=request.audit_id)
        source = (desk_pb2.BOOK_SOURCE_DESK if out.status == ACCEPTED
                  else desk_pb2.BOOK_SOURCE_UNSPECIFIED)
        return desk_pb2.RunDeskReply(status=_COMMAND_STATUS[out.status], source=source,
                                     message=out.message)

    def RequestOverride(self, request, context):
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
            v.audit_id(request.audit_id)
            v.text("requested_by", request.requested_by)
            v.text("reason", request.reason)
        except v.InvalidArgument as exc:
            self._invalid(context, "RequestOverride", exc, audit_id=request.audit_id)
        out = self._command("RequestOverride", context,
                            lambda d: d.request_override(request.audit_id, pid, day),
                            portfolio_id=pid, date=str(day), audit_id=request.audit_id)
        return desk_pb2.CommandReply(status=_COMMAND_STATUS[out.status], message=out.message)

    def RecordDecision(self, request, context):
        # The token is a credential: it is checked for presence and never logged. audit_id is
        # the override_decision row AlgoLens inserted; that row's fields (approver_role,
        # requested_by, payload.approved), not the request's, are what is checked and run.
        try:
            v.audit_id(request.audit_id)
            v.text("approver", request.approver)
            v.text("token", request.token)
        except v.InvalidArgument as exc:
            self._invalid(context, "RecordDecision", exc, audit_id=request.audit_id)
        out = self._command("RecordDecision", context,
                            lambda d: d.record_decision(request.audit_id),
                            audit_id=request.audit_id)
        return desk_pb2.CommandReply(status=_COMMAND_STATUS[out.status], message=out.message)

    def Publish(self, request, context):
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
            v.text("published_by", request.published_by)
            if request.audit_id != 0:  # 0: the newest pending publish row for the day
                v.audit_id(request.audit_id)
        except v.InvalidArgument as exc:
            self._invalid(context, "Publish", exc)
        out = self._command("Publish", context,
                            lambda d: d.publish(request.audit_id, pid, day),
                            portfolio_id=pid, date=str(day), audit_id=request.audit_id)
        return desk_pb2.CommandReply(status=_COMMAND_STATUS[out.status], message=out.message)

    def Rescind(self, request, context):
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
            v.audit_id(request.audit_id)
        except v.InvalidArgument as exc:
            self._invalid(context, "Rescind", exc, audit_id=request.audit_id)
        out = self._command("Rescind", context,
                            lambda d: d.rescind(request.audit_id, pid, day),
                            portfolio_id=pid, date=str(day), audit_id=request.audit_id)
        return desk_pb2.CommandReply(status=_COMMAND_STATUS[out.status], message=out.message)

    # -- read ----------------------------------------------------------------------------------

    def GetRunStatus(self, request, context):
        started = time.monotonic()
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
        except v.InvalidArgument as exc:
            self._invalid(context, "GetRunStatus", exc)
        try:
            facts = self._store.run_facts(pid, day)
        except StoreError as exc:
            log.error("run status unavailable", exc_info=exc.__cause__ is not None,
                      extra={"rpc": "GetRunStatus", "portfolio_id": pid, "date": str(day),
                             "error": str(exc)})
            context.abort(grpc.StatusCode.UNAVAILABLE, f"run status unavailable: {exc}")
        derived = st.derive(facts, pid, day)
        reply = desk_pb2.RunStatus(state=_STATE[derived.state], message=derived.message,
                                   published=derived.published,
                                   published_by=derived.published_by,
                                   publish_source=derived.publish_source)
        for name in ("started_at", "finished_at", "published_at", "sent_at"):
            ts = _ts(getattr(derived, name))
            if ts is not None:
                getattr(reply, name).CopyFrom(ts)
        log.info("run status", extra={
            "rpc": "GetRunStatus", "portfolio_id": pid, "date": str(day),
            "state": derived.state,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1)})
        return reply

    def _limits(self, portfolio_id: str):
        try:
            name = resolve_portfolio_dir(self._config_dir, portfolio_id)
            return rk.load_limits(str(portfolios_root(self._config_dir) / name))
        except PortfolioConfigError:
            return []

    def _multipliers(self, portfolio_id: str):
        try:
            name = resolve_portfolio_dir(self._config_dir, portfolio_id)
            return rk.load_multipliers(self._config_dir,
                                       str(portfolios_root(self._config_dir) / name))
        except PortfolioConfigError:
            return []

    def GetDeskRisk(self, request, context):
        started = time.monotonic()
        try:
            pid = v.portfolio_id(request.portfolio_id)
            day = v.date(request.date)
        except v.InvalidArgument as exc:
            self._invalid(context, "GetDeskRisk", exc)
        try:
            row = self._store.desk_risk_row(pid, day)
        except StoreError as exc:
            log.error("desk risk unavailable", exc_info=exc.__cause__ is not None,
                      extra={"rpc": "GetDeskRisk", "portfolio_id": pid, "date": str(day),
                             "error": str(exc)})
            context.abort(grpc.StatusCode.UNAVAILABLE, f"desk risk unavailable: {exc}")
        report = rk.derive_desk_risk(row, self._limits(pid))
        blockers = rk.publish_blockers(report)
        reply = desk_pb2.DeskRisk(
            blocks_publish=bool(blockers),
            blockers=[desk_pb2.RiskBreach(term=b.term, detail=b.detail) for b in blockers],
            state=_RISK_STATE[report.state], audit_id=report.audit_id,
            within_limits=report.within_limits, approved_override=report.approved_override,
            breaches=[desk_pb2.RiskBreach(term=b.term, detail=b.detail) for b in report.breaches],
            m=report.m, binding_term=report.binding_term,
            over_limit_after_rounding=report.over_limit_after_rounding,
            over_limit_by_hold=report.over_limit_by_hold, refused=report.refused,
            symbols=[desk_pb2.SymbolRisk(symbol=s.symbol, asked=s.asked, given=s.given,
                                         moved_by=s.moved_by, category=s.category)
                     for s in report.symbols],
            limits=[desk_pb2.RiskLimit(name=n, value=x) for n, x in report.limits],
            checks=[_risk_check(c) for c in report.checks],
            multipliers=[_multiplier(x) for x in
                         rk.run_multipliers(report, row) + self._multipliers(pid)])
        for name in ("gross_leverage", "net_leverage", "margin_cushion", "equity_to_margin"):
            value = getattr(report, name)
            if value is not None:
                setattr(reply, name, value)
        log.info("desk risk", extra={
            "rpc": "GetDeskRisk", "portfolio_id": pid, "date": str(day), "state": report.state,
            "status": "WITHIN" if report.within_limits else "BREACH",
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1)})
        return reply

