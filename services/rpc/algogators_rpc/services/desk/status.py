"""Pure mapping from RunFacts to the RunStatus the desk pages show."""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass
from typing import Optional

from .store import RunFacts

NOT_STARTED, RUNNING, SUCCEEDED, REFUSED, FAILED = (
    "NOT_STARTED", "RUNNING", "SUCCEEDED", "REFUSED", "FAILED")


@dataclass(frozen=True)
class DerivedStatus:
    state: str
    message: str
    started_at: Optional[dt.datetime] = None
    finished_at: Optional[dt.datetime] = None
    published: bool = False
    published_by: str = ""
    published_at: Optional[dt.datetime] = None
    # Contract C7 (027): desk | fallback | model-only, and when the e-mail went out.
    publish_source: str = ""
    sent_at: Optional[dt.datetime] = None


def _config(row) -> dict:
    cfg = row.portfolio_config
    if isinstance(cfg, str):  # a driver that hands jsonb back as text
        try:
            cfg = json.loads(cfg)
        except ValueError:
            return {}
    return cfg if isinstance(cfg, dict) else {}


def _describe_refusal(mark) -> str:
    if not isinstance(mark, dict):
        return "no reason recorded"
    parts = [str(mark.get("reason") or "no reason recorded")]
    where = ", ".join(f"{k} {mark[k]}" for k in ("module", "scope_id", "phase") if mark.get(k))
    return parts[0] + (f" ({where})" if where else "")


def derive(facts: RunFacts, portfolio_id: str, date: dt.date) -> DerivedStatus:
    rows = list(facts.metadata)
    if not rows:
        return DerivedStatus(NOT_STARTED,
                             f"no trading.live_run_metadata row for {portfolio_id} on {date}")

    started = min((r.created_at for r in rows if r.created_at is not None), default=None)
    published_at = None
    published_by = publish_source = ""
    if facts.has_publish_columns:
        pubs = [r for r in rows if r.published_at is not None]
        if pubs:
            last = max(pubs, key=lambda r: r.published_at)
            published_at, published_by = last.published_at, last.published_by or ""
            publish_source = last.publish_source or ""
    sent_at = max((r.sent_at for r in rows if r.sent_at is not None), default=None)
    common = dict(started_at=started, published=published_at is not None,
                  published_by=published_by, published_at=published_at,
                  publish_source=publish_source, sent_at=sent_at,
                  finished_at=facts.results_written_at)

    configs = [_config(r) for r in rows]
    strict = next((c["strict_assertion"] for c in configs if "strict_assertion" in c), None)
    if strict is not None:
        reason = strict.get("reason") if isinstance(strict, dict) else None
        return DerivedStatus(FAILED, "strict assertion: " + (reason or "the run stored no book"),
                             **common)
    refusal = next((c["risk_refusal"] for c in configs if "risk_refusal" in c), None)
    if refusal is not None:
        return DerivedStatus(REFUSED, "risk refusal: " + _describe_refusal(refusal), **common)
    if facts.has_results:
        return DerivedStatus(SUCCEEDED, "model run complete", **common)
    return DerivedStatus(RUNNING, "run started; its trading.live_results row has not been "
                                  "written yet (still running, or it died)", **common)
