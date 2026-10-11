#!/usr/bin/env python3
"""compare_csv_dirs.py <dir_A> <dir_B> [--allow tbl.col,...] [--allow-rows tbl,...] [--tol 1e-9]

Generic A/B over two directories of snapshot CSVs (snap.sh / snap_bt.sh output).

Column coverage is DERIVED from the CSV headers, never written here (the
GATE-compare-column-coverage rule T-1 applied to compare_chains.py): a column
added to the schema is compared from the moment it exists. Exactly two kinds of
column are not compared, and both are named in the output:

  PROVENANCE  insert wall clock and surrogate keys - two identical runs MUST
              differ here. The set is the same one compare_chains.py and
              fingerprint.sql use, plus the backtest run key (run_id embeds the
              run's own clock) and its dependants.
  PREDICTED   --allow, empty by default. A class B change has to predict the
              column it moves before the run; every allowed column is echoed.
  ROWS        --allow-rows (T-LOOP harness fix (e)), empty by default: the
              tables whose rows may be present on one side only (a moved book's
              fills and positions); the counts are printed and every other table
              must have the same keys on both sides.

  NULL-NESS   (T-8a commit (12)) a column that holds the wall-clock instant of the run that
              wrote it (trading.live_results.settled_at): two identical runs MUST differ in the
              value, and must agree on WHICH rows carry one. It is compared as "NULL or not NULL"
              on every key, the rule and its counts are printed for the table, and a mismatch is a
              value diff like any other.
  ONE-SIDED   a column present on one side only (the side after the migration that added it) is
              a HEADER DIFFERS failure unless it is named in --allow: it is then dropped from that
              side and reported with its row and non-empty counts. Never a silent pass.

Row identity is the whole key-tuple of non-provenance, non-float columns when no
key is registered, which makes the comparison order-independent.
Exit 0 only when every compared column agrees on every key and neither side has
a row the other lacks.
"""
import csv, sys, os, argparse, math, collections

PROVENANCE = {
    "live_results": {"id", "created_at"},
    "equity_curve": {"id"},
    "executions": {"created_at"},
    "positions": {"updated_at"},
    "signals": {"id", "created_at"},
    "corp_action_applied": {"id", "created_at", "applied_at"},
    "live_run_metadata": {"id", "created_at"},
    "strategy_trading_days_metadata": {"id", "created_at", "updated_at"},
    "strategy_lifecycle_log": {"id", "created_at"},
    # backtest.*: run_id is "<strategy>_<wall clock>" - a surrogate for the run,
    # not a result. Every child table carries it, so it is excluded there too and
    # the rows are keyed on what they describe instead.
    # backtest.*: run_id is "<strategy>_<window end>_<clock>" and every child table
    # carries it; date_inserted is the INSERT wall clock; executions.id / signals.id
    # are bigserial surrogates.
    "run_metadata": {"run_id", "portfolio_run_id", "date_inserted"},
    "results": {"run_id"},
    "final_positions": {"run_id"},
    "bt_equity_curve": {"run_id"},
    "bt_executions": {"id", "run_id"},
    "bt_signals": {"id", "run_id", "portfolio_run_id"},
}
# Columns compared by NULL-ness (see the docstring): stem -> columns.
NULLNESS = {
    "live_results": {"settled_at"},
}
KEYS = {
    "live_results": ("date",),
    "equity_curve": ("timestamp", "strategy_id", "portfolio_type"),
    "executions": ("date", "symbol", "side", "exec_id"),
    "positions": ("date", "symbol", "strategy_name"),
    "signals": ("timestamp", "symbol", "strategy_id"),
    "corp_action_applied": ("symbol", "action_type", "ex_date"),
    # The table's own UNIQUE constraint (date, strategy_id, portfolio_id).
    # compare_chains.py registers ("run_date", "strategy_id") and there is no
    # run_date column, so its key degrades to strategy_id alone and 226 rows
    # collapse into one dict entry - see the T-2 report, gate tooling.
    "live_run_metadata": ("date", "strategy_id", "portfolio_id"),
    "strategy_trading_days_metadata": ("strategy_id", "portfolio_id"),
    "strategy_lifecycle_log": ("strategy_id", "event_time"),
    # backtest.* -- keyed on what the row describes, never on run_id.
    "run_metadata": ("strategy_id", "portfolio_id", "start_date", "end_date"),
    "results": ("portfolio_id", "start_date", "end_date"),
    "final_positions": ("date", "symbol", "strategy_id", "portfolio_id"),
    "bt_equity_curve": ("timestamp", "portfolio_id"),
    "bt_executions": ("timestamp", "symbol", "side", "execution_id"),
    "bt_signals": ("timestamp", "symbol", "strategy_id"),
}

def load(path):
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        return r.fieldnames or [], list(r)

def numeric(a, b, tol):
    try:
        fa, fb = float(a), float(b)
    except (TypeError, ValueError):
        return None
    if math.isnan(fa) and math.isnan(fb):
        return True
    return abs(fa - fb) <= tol

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("A"); ap.add_argument("B")
    ap.add_argument("--allow", default="")
    ap.add_argument("--allow-rows", default="")
    ap.add_argument("--tol", type=float, default=1e-9)
    ap.add_argument("--prefix", default="")
    a = ap.parse_args()

    allow = set(x.strip() for x in a.allow.split(",") if x.strip())
    allow_rows = set(x.strip() for x in a.allow_rows.split(",") if x.strip())
    print("PREDICTED-DIFF ALLOWLIST: " + (", ".join(sorted(allow)) if allow else "empty (nothing is permitted to differ)"))
    if allow_rows:
        print("PREDICTED ROW-SET CHANGES (tables): " + ", ".join(sorted(allow_rows)))

    names = sorted(set(os.listdir(a.A)) | set(os.listdir(a.B)))
    names = [n for n in names if n.endswith(".csv")]
    bad = 0
    for n in names:
        pa, pb = os.path.join(a.A, n), os.path.join(a.B, n)
        stem = n[:-4]
        if stem.startswith("trading_"):
            stem = stem[len("trading_"):]
        elif stem.startswith("backtest_"):
            stem = stem[len("backtest_"):]
            # equity_curve / executions / signals exist in BOTH schemas with
            # different columns; keep the backtest ones under their own name.
            if stem in ("equity_curve", "executions", "signals"):
                stem = "bt_" + stem
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"[{stem}] MISSING on one side: A={os.path.exists(pa)} B={os.path.exists(pb)}")
            bad += 1
            continue
        ha, ra = load(pa)
        hb, rb = load(pb)
        # T-LOOP (declared projection, LOOP_SPEC v6.1 H4/H5/H14): a column named in the environment
        # COMPARE_PROJECT_OUT ("stem.col,...") that exists on exactly ONE side (the side after the
        # migration that added it) is dropped from that side before the header check, and the
        # count of its non-empty cells is printed so a declaration "NULL on every row" can be
        # checked. A column present on BOTH sides is compared like any other.
        for pc in [x.strip() for x in os.environ.get("COMPARE_PROJECT_OUT", "").split(",") if x.strip()]:
            pstem, _, pcol = pc.rpartition(".")
            if pstem != stem or (pcol in ha) == (pcol in hb):
                continue
            side, hdr, rows = ("A", ha, ra) if pcol in ha else ("B", hb, rb)
            k = hdr.index(pcol)
            nonempty = sum(1 for row in rows if row.get(pcol, "") not in ("", None))
            print(f"[{stem}] PROJECTED OUT {pcol} on side {side} only (declared): {len(rows)} rows, {nonempty} non-empty")
            hdr.remove(pcol)
            for row in rows:
                row.pop(pcol, None)
        # T-8a commit (12): a column on ONE side only that --allow names is dropped from that side and
        # reported (ONE-SIDED). One that --allow does not name leaves the headers different: a failure.
        for oc in [c for c in list(ha) + list(hb) if (c in ha) != (c in hb)]:
            if f"{stem}.{oc}" not in allow:
                continue
            side, hdr, rows = ("A", ha, ra) if oc in ha else ("B", hb, rb)
            nonempty = sum(1 for row in rows if row.get(oc, "") not in ("", None))
            print(f"[{stem}] ONE-SIDED COLUMN {oc} on side {side} only (allowed by --allow {stem}.{oc}): {len(rows)} rows, {nonempty} non-empty, {len(rows) - nonempty} empty")
            hdr.remove(oc)
            for row in rows:
                row.pop(oc, None)
        if ha != hb:
            print(f"[{stem}] HEADER DIFFERS A={ha} B={hb}")
            bad += 1
            continue
        prov = PROVENANCE.get(stem, {"id", "created_at", "updated_at"})
        cols = [c for c in ha if c not in prov]
        # NULL-NESS: the value becomes "<set>" or "" on both sides before anything is compared.
        for nc in sorted(c for c in NULLNESS.get(stem, ()) if c in ha):
            sa = sum(1 for row in ra if row[nc] not in ("", None)); sb = sum(1 for row in rb if row[nc] not in ("", None))
            print(f"[{stem}] NULL-NESS RULE {nc}: a run's wall-clock instant, compared as NULL / not NULL on every key; set on A={sa} of {len(ra)} rows, B={sb} of {len(rb)} rows")
            for row in ra + rb:
                row[nc] = "<set>" if row[nc] not in ("", None) else ""
        registered = KEYS.get(stem)
        if registered is not None:
            missing = [k for k in registered if k not in ha]
            if missing:
                print(f"[{stem}] REFUSING: registered key column(s) {missing} are not in the "
                      f"header {ha}. A key that is not present silently collapses rows into "
                      f"one dict entry and hides every row but the last.")
                bad += 1
                continue
            key = list(registered)
        else:
            key = cols
        def index(rows):
            d = collections.OrderedDict()
            for row in rows:
                k = tuple(row[c] for c in key)
                d.setdefault(k, []).append(row)
            return d
        ia, ib = index(ra), index(rb)
        onlyA = [k for k in ia if k not in ib]
        onlyB = [k for k in ib if k not in ia]
        diffs = []
        mult = []
        for k in ia:
            if k not in ib:
                continue
            la, lb = ia[k], ib[k]
            if len(la) != len(lb):
                # T-LOOP (C1 post incident): a key held by several rows (two sleeves' fills of one
                # symbol-day) that gains or loses a copy is a row-set change: declared when the table's
                # row set is (--allow-rows), a value diff otherwise.
                if stem in allow_rows:
                    mult.append((k, len(la), len(lb)))
                else:
                    diffs.append((k, "<row multiplicity>", len(la), len(lb)))
                continue
            for x, y in zip(la, lb):
                for c in cols:
                    if x[c] == y[c]:
                        continue
                    eq = numeric(x[c], y[c], a.tol)
                    if eq is True:
                        continue
                    if f"{stem}.{c}" in allow:
                        continue
                    diffs.append((k, c, x[c], y[c]))
        n_allowed = 0
        for k in ia:
            if k not in ib: continue
            for x, y in zip(ia[k], ib[k]):
                for c in cols:
                    if x[c] != y[c] and f"{stem}.{c}" in allow and numeric(x[c], y[c], a.tol) is not True:
                        n_allowed += 1
        rows_ok = stem in allow_rows
        status = "IDENTICAL" if not (diffs or onlyA or onlyB or mult) else (
            "DIFFERS (rows declared)" if not diffs and rows_ok else "DIFFERS")
        extra = f" allowed-diffs={n_allowed}" if n_allowed else ""
        extra += f" multiplicity-changes={len(mult)} (rows declared)" if mult else ""
        print(f"[{stem}] A={len(ra)} B={len(rb)} compared_cols={len(cols)} {status} onlyA={len(onlyA)} onlyB={len(onlyB)} value-diffs={len(diffs)}{extra}")
        for d in diffs[:12]:
            print(f"      key={d[0]} col={d[1]} A={d[2]} B={d[3]}")
        if len(diffs) > 12:
            print(f"      ... and {len(diffs)-12} more")
        for k in onlyA[:5]:
            print(f"      only in A: {k}")
        for k in onlyB[:5]:
            print(f"      only in B: {k}")
        for k, na, nb in mult[:5]:
            print(f"      multiplicity (rows declared): {k} A={na} B={nb}")
        if diffs or ((onlyA or onlyB) and not rows_ok):
            bad += 1
    print("=== RESULT: " + ("IDENTICAL on every compared column" if bad == 0 else f"DIFFERS in {bad} table(s)"))
    sys.exit(0 if bad == 0 else 1)

main()
