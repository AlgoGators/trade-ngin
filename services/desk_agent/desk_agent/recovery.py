"""Re-drive of the command log (contract section 6: the RPC is a fast path, never the only one).

At startup, rows still 'running' are orphans of a previous agent process (the engine it spawned
died with the container): they go back to 'pending' with the message 're-driven after agent
restart'. Then every 'pending' row is dispatched, oldest first, by kind. The pending sweep
repeats every QT_REDRIVE_INTERVAL_S (default 60 s) in a daemon thread, so a row whose RPC never
arrived (gRPC down, AlgoLens restarted) still runs. The claim (`... WHERE status = 'pending'`)
makes double dispatch impossible.
"""

from __future__ import annotations

import logging
import threading
from typing import Optional

log = logging.getLogger("desk_agent.recovery")


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


def redrive_pending(store, dispatcher) -> int:
    """Startup: orphaned 'running' rows back to 'pending', then one sweep. Never raises."""
    try:
        orphans = store.reset_running()
        if orphans:
            log.warning("orphaned running rows reset to pending",
                        extra={"count": len(orphans), "audit_id": orphans})
        count = sweep_pending(store, dispatcher)
    except Exception as exc:
        log.error("startup re-drive failed; the periodic sweep will retry",
                  extra={"error": type(exc).__name__})
        return 0
    log.info("startup re-drive done", extra={"count": count})
    return count


def start_redrive_thread(store, dispatcher, interval_s: float,
                         stop: Optional[threading.Event] = None) -> threading.Thread:
    stop = stop or threading.Event()

    def loop():
        while not stop.wait(interval_s):
            try:
                sweep_pending(store, dispatcher)
            except Exception as exc:
                log.error("pending sweep failed", extra={"error": type(exc).__name__})

    thread = threading.Thread(target=loop, name="redrive", daemon=True)
    thread.start()
    return thread
