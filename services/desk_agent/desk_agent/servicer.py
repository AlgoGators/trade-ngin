"""DeskService implementation.

Implemented: GetRunStatus (reads Postgres). RunDesk, RequestOverride, RecordDecision and
Publish validate their input and then REFUSE with "not implemented yet: <ruling>". They never
report success for work that was not done (ruling 24). They will shell out to the C++ binary in
--desk / --publish modes (plan 3b) in later PRs (plan E6, E7, E8).

Malformed input is answered with gRPC status INVALID_ARGUMENT on every RPC, so a caller bug is
never confused with an engine refusal.
"""

from __future__ import annotations

import logging
import time

import grpc
from google.protobuf.timestamp_pb2 import Timestamp

from qt.v1 import desk_pb2, desk_pb2_grpc

from . import status as st
from . import validation as v
from .store import RunStatusStore, StoreError

log = logging.getLogger("desk_agent.rpc")

_STATE = {
    st.NOT_STARTED: desk_pb2.RUN_STATE_NOT_STARTED,
    st.RUNNING: desk_pb2.RUN_STATE_RUNNING,
    st.SUCCEEDED: desk_pb2.RUN_STATE_SUCCEEDED,
    st.REFUSED: desk_pb2.RUN_STATE_REFUSED,
    st.FAILED: desk_pb2.RUN_STATE_FAILED,
}

# What each unimplemented command waits on.
NOT_IMPLEMENTED = {
    "RunDesk": "not implemented yet: desk loop (plan 3b; plan E5/E6, rulings 14, 15)",
    "RequestOverride": "not implemented yet: override e-mail (D4, D5; plan A5/E7)",
    "RecordDecision": "not implemented yet: override decision (D5, D17; plan A5/E7)",
    "Publish": "not implemented yet: publish (D7, ruling 29; plan A6/E8)",
}


def _ts(value):
    if value is None:
        return None
    t = Timestamp()
    t.FromDatetime(value)
    return t


class DeskServicer(desk_pb2_grpc.DeskServiceServicer):
    def __init__(self, store: RunStatusStore):
        self._store = store

    # -- helpers ---------------------------------------------------------------------------

    @staticmethod
    def _invalid(context, rpc: str, exc: v.InvalidArgument, **fields):
        log.warning("invalid request", extra={"rpc": rpc, "status": "INVALID_ARGUMENT",
                                              "error": str(exc), **fields})
        context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(exc))

    def _refuse_unimplemented(self, rpc: str, reply_type, **fields):
        message = NOT_IMPLEMENTED[rpc]
        log.info("command refused", extra={"rpc": rpc, "status": "REFUSED", **fields})
        return reply_type(status=desk_pb2.COMMAND_STATUS_REFUSED, message=message)

    # -- commands (refused until their PRs land) ---------------------------------------------

    def RunDesk(self, request, context):
        try:
            v.portfolio_id(request.portfolio_id)
            v.date(request.date)
            v.audit_id(request.audit_id)
            v.text("requested_by", request.requested_by)
        except v.InvalidArgument as exc:
            self._invalid(context, "RunDesk", exc, audit_id=request.audit_id)
        return self._refuse_unimplemented(
            "RunDesk", desk_pb2.RunDeskReply, portfolio_id=request.portfolio_id,
            date=request.date, audit_id=request.audit_id)

    def RequestOverride(self, request, context):
        try:
            v.portfolio_id(request.portfolio_id)
            v.date(request.date)
            v.audit_id(request.audit_id)
            v.text("requested_by", request.requested_by)
            v.text("reason", request.reason)
        except v.InvalidArgument as exc:
            self._invalid(context, "RequestOverride", exc, audit_id=request.audit_id)
        return self._refuse_unimplemented(
            "RequestOverride", desk_pb2.CommandReply, portfolio_id=request.portfolio_id,
            date=request.date, audit_id=request.audit_id)

    def RecordDecision(self, request, context):
        # The token is a credential: it is checked for presence and never logged.
        try:
            v.audit_id(request.audit_id)
            v.text("approver", request.approver)
            v.text("token", request.token)
        except v.InvalidArgument as exc:
            self._invalid(context, "RecordDecision", exc, audit_id=request.audit_id)
        return self._refuse_unimplemented("RecordDecision", desk_pb2.CommandReply,
                                          audit_id=request.audit_id)

    def Publish(self, request, context):
        try:
            v.portfolio_id(request.portfolio_id)
            v.date(request.date)
            v.text("published_by", request.published_by)
        except v.InvalidArgument as exc:
            self._invalid(context, "Publish", exc)
        return self._refuse_unimplemented("Publish", desk_pb2.CommandReply,
                                          portfolio_id=request.portfolio_id, date=request.date)

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
