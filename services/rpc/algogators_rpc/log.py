"""Structured (one JSON object per line) logging to stdout, so `docker logs` is the log."""

from __future__ import annotations

import json
import logging
import sys
from datetime import datetime, timezone

# Fields passed through `extra=` that are copied into the JSON line. Anything else is ignored,
# which is also what keeps a stray secret in a record attribute out of the log.
_FIELDS = ("rpc", "method", "code", "api_version", "services", "task", "portfolio_id", "date",
           "audit_id", "status", "state", "elapsed_ms", "listen", "db", "error", "count", "kind",
           "mode", "rc", "timed_out", "argv", "stdout_tail", "stderr_tail")


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        out = {
            "ts": datetime.fromtimestamp(record.created, tz=timezone.utc)
                          .isoformat(timespec="milliseconds"),
            "level": record.levelname,
            "logger": record.name,
            "msg": record.getMessage(),
        }
        for key in _FIELDS:
            if hasattr(record, key):
                out[key] = getattr(record, key)
        if record.exc_info:
            out["exc"] = self.formatException(record.exc_info)
        return json.dumps(out, default=str)


def setup(level: str = "INFO") -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(JsonFormatter())
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(level)
