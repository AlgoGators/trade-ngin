"""s0_project.py <restored_snapshot_dir> <out_dir>   (T-AUDIT)
The restored S0 book is pre-migration history: every column a later migration added must read its history value on
every restored row (netting_adjustment NULL (013), instrument_id NULL (016), execution_type STRATEGY (015),
daily_roll_costs and total_roll_costs 0 (017), risk_detail NULL (020)). Writes a copy without those columns for the
comparison against S0. Exit 1 on any other value.
T-8a commit (12), migration 035: trading.live_results.settled_at on a restored book must hold what the migration's
backfill gives: the marker 1970-01-01 00:00:00+00 on every row that has a later row of its key (portfolio_id,
strategy_id), NULL on the last row of each key. Checked per key, then projected away like the others.
T-8a commit (12b), migration 036: the twenty statistic and overlay columns of trading.live_results hold NULL on every
restored row (R32: history is never rewritten; the columns have no backfill). Checked and projected away like the others.
The QT desk's migrations (021 to 029) add, on a book the desk never edited: portfolio_type 'system' on executions and
live_results (021), moved_by NULL on positions (021), book_source NULL on live_results (023), settings_used,
published_by, published_at (022), publish_source and sent_at (027) NULL on live_run_metadata, portfolio_group NULL and
desk_editable false on strategy_registry (023); checked and projected away like the others. Migration 040's three roll
split columns on executions hold NULL on every restored row; checked and projected away like the others. With the S0 directory as
a third argument, a column S0's own file already has is kept and compared against S0 (positions.portfolio_type, from
001, is in S0), and a table S0 has no file for (position_overrides, strategy_config) must hold no row of the book."""
import csv, os, shutil, sys
EXPECTED = {"netting_adjustment": "", "instrument_id": "", "execution_type": "STRATEGY", "daily_roll_costs": "0",
            "total_roll_costs": "0", "risk_detail": ""}
M036 = ("worst_day_date worst_day_symbol max_drawdown_sizing volatility_sizing worst_day_sizing worst_day_sizing_date "
        "worst_day_sizing_symbol monthly_skew monthly_tail_ratio calendar_year_returns losing_years total_trades "
        "total_strategy_fills total_roll_fills overlay_risk overlay_risk_jump overlay_risk_shock overlay_gross_leverage "
        "overlay_net_leverage var_95_1d").split()
EXPECTED.update({c: "" for c in M036})
QT = {"portfolio_type": "system", "moved_by": "", "book_source": "", "settings_used": "", "published_by": "",
      "published_at": "", "publish_source": "", "sent_at": "", "portfolio_group": "", "desk_editable": "f"}
EXPECTED.update(QT)
# Migration 040 (rolls step 4, the roll split): roll_bar_move, roll_gap_points, roll_gap_value NULL on every executions
# row written before it (no backfill; a value only on a per-contract roll's closing leg written with the switch on).
EXPECTED.update({c: "" for c in ("roll_bar_move", "roll_gap_points", "roll_gap_value")})
NUMERIC = {"daily_roll_costs", "total_roll_costs"}
def hist(c, v):
    if c in NUMERIC:
        try: return float(v) == 0.0
        except ValueError: return False
    return v == EXPECTED[c]
SETTLED_MARK = "1970-01-01 00:00:00+00"
def settled_off(head, rows):
    """rows of live_results whose settled_at is not the backfill's: (key, date, value, expected)"""
    k, d, p, s = head.index("settled_at"), head.index("date"), head.index("portfolio_id"), head.index("strategy_id")
    last = {}
    for row in rows: last[(row[p], row[s])] = max(last.get((row[p], row[s]), ""), row[d])
    off = []
    for row in rows:
        want = "" if row[d] == last[(row[p], row[s])] else SETTLED_MARK
        if row[k] != want: off.append(((row[p], row[s]), row[d], row[k], want))
    return off, len(last)
src, dst = sys.argv[1], sys.argv[2]
s0 = sys.argv[3] if len(sys.argv) > 3 else None
def s0_head(n):
    """the header of S0's own file of the table, or None when S0 has no such file (no S0 dir given: every column)"""
    if s0 is None: return []
    f = os.path.join(s0, n)
    if not os.path.exists(f): return None
    with open(f, newline="") as fh: return next(csv.reader(fh))
os.makedirs(dst, exist_ok=True); bad = 0
for n in sorted(os.listdir(src)):
    if not n.endswith(".csv"): continue
    with open(os.path.join(src, n), newline="") as fh:
        r = csv.reader(fh); head = next(r); rows = list(r)
    known = s0_head(n)
    if known is None:
        print("[%s] a table S0 has no file for: %d row(s) of the book, expected 0" % (n, len(rows))); bad += len(rows)
        continue
    drop = [c for c in head if c in EXPECTED and c not in known]
    if n == "trading_live_results.csv" and "settled_at" in head:
        off, nkeys = settled_off(head, rows); k = head.index("settled_at")
        print("[%s] settled_at: %d restored rows of %d key(s), %d with the backfill marker, %d NULL (the last row of each key), %d off the backfill rule" % (
            n, len(rows), nkeys, sum(1 for row in rows if row[k] == SETTLED_MARK), sum(1 for row in rows if row[k] == ""), len(off)))
        for o in off[:8]: print("      off: key=%s date=%s settled_at=%r expected %r" % o)
        bad += len(off)
        rows = [row[:k] + row[k + 1:] for row in rows]; head = head[:k] + head[k + 1:]
        if not [c for c in head if c in EXPECTED and c not in known]:
            with open(os.path.join(dst, n), "w", newline="") as out:
                w = csv.writer(out); w.writerow(head); w.writerows(rows)
            continue
        drop = [c for c in head if c in EXPECTED and c not in known]
    if not drop:
        shutil.copy(os.path.join(src, n), os.path.join(dst, n)); continue
    keep = [i for i, c in enumerate(head) if c not in drop]
    for c in drop:
        k = head.index(c); nn = sum(1 for row in rows if not hist(c, row[k]))
        print("[%s] %s: %d restored rows, %d off the history value %r" % (n, c, len(rows), nn, EXPECTED[c])); bad += nn
    with open(os.path.join(dst, n), "w", newline="") as out:
        w = csv.writer(out); w.writerow([head[i] for i in keep]); w.writerows([row[i] for i in keep] for row in rows)
sys.exit(1 if bad else 0)
