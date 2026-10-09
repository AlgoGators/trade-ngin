"""Startup re-drive of pending commands (plan 3b).

Every command is keyed on its trading.position_overrides row, written by AlgoLens before it
calls. On startup the agent will re-drive every row still pending, so a call dropped by a
restart loses nothing. Nothing is driven yet: the commands themselves are not implemented
(plan E6-E8), and the pending-status column arrives with them. This stub only says so.
"""

from __future__ import annotations

import logging

log = logging.getLogger("desk_agent.recovery")


def redrive_pending(store) -> int:
    """Re-drive pending position_overrides rows. Returns how many were re-driven (0 for now)."""
    log.info("startup re-drive skipped: no command is implemented yet (plan E6-E8)",
             extra={"count": 0})
    return 0
