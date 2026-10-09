"""DeskService implementation.

GetRunStatus reads Postgres. The four commands check their trading.position_overrides row,
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

from qt.v1 import desk_pb2, desk_pb2_grpc

from . import status as st
from . import validation as v
from .commands import ACCEPTED, DONE, FAILED, REFUSED, Dispatcher, Outcome
from .store import RunStatusStore, StoreError

log = logging.getLogger("desk_agent.rpc")

_STATE = {
    st.NOT_STARTED: desk_pb2.RUN_STATE_NOT_STARTED,
    st.RUNNING: desk_pb2.RUN_STATE_RUNNING,
    st.SUCCEEDED: desk_pb2.RUN_STATE_SUCCEEDED,
    st.REFUSED: desk_pb2.RUN_STATE_REFUSED,
    st.FAILED: desk_pb2.RUN_STATE_FAILED,
}

_COMMAND_STATUS = {
    ACCEPTED: desk_pb2.COMMAND_STATUS_ACCEPTED,
    DONE: desk_pb2.COMMAND_STATUS_DONE,
    REFUSED: desk_pb2.COMMAND_STATUS_REFUSED,
    FAILED: desk_pb2.COMMAND_STATUS_FAILED,
}

NO_DISPATCHER = "commands are not configured on this agent"


def _ts(value):
    if value is None:
        return None
    t = Timestamp()
    t.FromDatetime(value)
    return t


class DeskServicer(desk_pb2_grpc.DeskServiceServicer):
    def __init__(self, store: RunStatusStore, dispatcher: Optional[Dispatcher] = None):
        self._store = store
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
                                   published_by=derived.published_by)
        for name in ("started_at", "finished_at", "published_at"):
            ts = _ts(getattr(derived, name))
            if ts is not None:
                getattr(reply, name).CopyFrom(ts)
        log.info("run status", extra={
            "rpc": "GetRunStatus", "portfolio_id": pid, "date": str(day),
            "state": derived.state,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 1)})
        return reply
