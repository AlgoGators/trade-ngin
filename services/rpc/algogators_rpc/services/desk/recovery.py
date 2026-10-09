"""Re-drive of the command log (contract section 6: the RPC is a fast path, never the only one).

One background task of the desk service (service.py; the shared server runs it), every
QT_REDRIVE_INTERVAL_S (default 60 s), and once at startup (`on_start`):

1. **Startup recovery, until it succeeds.** Rows still 'running' that no job of this process
   owns are orphans of an earlier process (the engine it spawned died with the container): they
   go back to 'pending' ("re-driven after agent restart"). If the database is down at startup,
   every later pass retries this first; until it has succeeded, nothing else runs (R#1).
2. **Stale rows.** A 'running' row with no live job here that this process abandoned (a database
   error left it 'running'), or that is older than QT_JOB_TIMEOUT_S + QT_STALE_GRACE_S
   (default 1800 + 300 s), goes back to 'pending' ("re-driven: stale running row").
3. **The pending sweep.** Every 'pending' row is dispatched, oldest first, by kind, so a row
   whose RPC never arrived (gRPC down, AlgoLens restarted) still runs.

The claim (`... WHERE status = 'pending'`) makes double dispatch impossible, and the requeue
(`... WHERE status = 'running'`, the one running -> pending move the 025 trigger allows) never
touches a row a live job owns.
"""

from __future__ import annotations

import logging

log = logging.getLogger("desk.recovery")

RESTART_MESSAGE = "re-driven after agent restart"
STALE_MESSAGE = "re-driven: stale running row with no live job"


def sweep_pending(store, dispatcher) -> int:
    """Dispatch every pending row, oldest first. Returns how many were dispatched."""
    rows = store.pending_rows()
    count = 0
    for row in rows:
        try:
            out = dispatcher.dispatch(row)
        except Exception as exc:
            log.error("re-drive of a row failed", exc_info=True,
                      extra={"audit_id": row.id, "kind": row.kind,
                             "error": type(exc).__name__})
            continue
        count += 1
        log.info("row re-driven", extra={"audit_id": row.id, "kind": row.kind,
                                         "portfolio_id": row.portfolio_id,
                                         "status": out.status})
    return count


def recover_running(store, dispatcher) -> int:
    """Steps 1 and 2. Raises on a database error (the caller logs it and retries)."""
    startup = not dispatcher.recovered
    rows = store.running_rows()
    requeued = dispatcher.requeue_orphans(rows, RESTART_MESSAGE if startup else STALE_MESSAGE)
    if startup:
        dispatcher.recovered = True
        if requeued:
            log.warning("orphaned running rows reset to pending",
                        extra={"count": len(requeued), "audit_id": requeued})
        log.info("startup recovery done", extra={"count": len(requeued)})
    elif requeued:
        log.warning("stale running rows reset to pending",
                    extra={"count": len(requeued), "audit_id": requeued})
    return len(requeued)


def redrive(store, dispatcher) -> int:
    """One pass: recovery, then the sweep. Never raises. Returns how many rows were swept."""
    try:
        recover_running(store, dispatcher)
    except Exception as exc:
        log.error("recovery of running rows failed; the next pass retries",
                  extra={"error": type(exc).__name__, "status": "startup" if not
                         dispatcher.recovered else "stale"})
        if not dispatcher.recovered:
            return 0  # nothing runs before the previous process's rows are accounted for
    try:
        return sweep_pending(store, dispatcher)
    except Exception as exc:
        log.error("pending sweep failed; the next pass retries",
                  extra={"error": type(exc).__name__})
        return 0


# The startup pass is the same pass.
redrive_pending = redrive
