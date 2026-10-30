#!/usr/bin/env python3
"""Writes RUN_REPORT.md and the analysis CSVs of a live results folder (the live counterpart of backtest_report.py).
The limits are read from the runs'
own OVERLAY lines, the universe from the bars, the largest contributor from the year (no year-specific text), and the
hand check reads each held contract's own closes (per-contract rolls book the roll bar from them).

Usage: live_report.py <live folder> <inputs folder>

  <live folder>    holds the exported live tables (live_results.csv, live_positions.csv, live_executions.csv, ...),
                   config_used/ and checks/
  <inputs folder>  fut.csv (futures .v.0 bars), cbar.csv (the held contracts' own bars: instrument_id, date, close),
                   meta.csv (contract metadata), mkt.csv (SPY QQQ AGG TLT GLD), sg.csv (SG Trend,
                   backtest.equity_curve run_id sg_trend; may be empty), bt2y.csv (the seed's 2y backtest equity curve;
                   may be empty), roll.csv (the ROLL legs' roll_bar_move per symbol and date), loglines.txt (the runs'
                   OVERLAY, BOOK and SIZING_CAPITAL lines). Written by 12_package.sh.
"""
import os, re, sys, json, datetime as dt
import numpy as np, pandas as pd

OUT, INP = sys.argv[1].rstrip("/"), sys.argv[2].rstrip("/")
A = os.path.join(OUT, "analysis")
os.makedirs(A, exist_ok=True)
CAP, TAU, K = 500000.0, 0.20, 311.0574
NOW = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M")

lr = pd.read_csv(f"{OUT}/live_results.csv")
lr["d"] = pd.to_datetime(lr.date.str[:10]); lr = lr.set_index("d").sort_index()
pos = pd.read_csv(f"{OUT}/live_positions.csv"); pos["d"] = pd.to_datetime(pos.date.str[:10])
ex = pd.read_csv(f"{OUT}/live_executions.csv"); ex["d"] = pd.to_datetime(ex.date.str[:10])
meta = pd.read_csv(f"{INP}/meta.csv"); meta["symbol"] = meta["Databento Symbol"] + ".v.0"
mult = meta.set_index("symbol")["Contract Size"].astype(float); sector = meta.set_index("symbol").Sector
fut = pd.read_csv(f"{INP}/fut.csv"); fut["d"] = pd.to_datetime(fut.time.str[:10]); fut = fut.sort_values(["symbol", "d"])
fut["dc"] = fut.groupby("symbol").close.diff()
cbar = pd.read_csv(f"{INP}/cbar.csv", dtype={"instrument_id": str}, parse_dates=["date"]).sort_values(["instrument_id", "date"])
cbar["prev_close"] = cbar.groupby("instrument_id").close.shift()
roll = pd.read_csv(f"{INP}/roll.csv", parse_dates=["d"])
mkt = pd.read_csv(f"{INP}/mkt.csv", parse_dates=["d"])
BENCH = [b_ for b_ in ["SPY", "QQQ", "AGG", "TLT", "GLD"] if (mkt.symbol == b_).any()]   # the benchmarks with data
sg = pd.read_csv(f"{INP}/sg.csv", parse_dates=["d"]).set_index("d").equity
bt = pd.read_csv(f"{INP}/bt2y.csv", parse_dates=["d"]).set_index("d").equity
NAME = os.environ.get("PGDATABASE", "")
NSYM = fut.symbol.nunique()
_pairs = json.load(open(f"{OUT}/config_used/portfolio.json")).get("listing_dates", {}).get("contracts", [])
NPRED = sum(1 for c in _pairs if (c["symbol"] + ".v.0") in set(fut.symbol) and (c["before"] + ".v.0") in set(fut.symbol))
NINST = NSYM - NPRED   # a listing pair is one instrument: the full-size contract before the listing, the micro after

v = lr.current_portfolio_value.astype(float)
START, END = v.index[0], v.index[-1]
settled = lr[lr.settled_at.notna()]
LAST = settled.index[-1]
last = settled.iloc[-1]
first_fill = ex.d.min()
yrs = (LAST - first_fill).days / 365.25
strat = ex[ex.execution_type != "ROLL"]


def f2(x, p=2): return "" if x is None or (isinstance(x, float) and np.isnan(x)) else f"{x:,.{p}f}"
def pc(x, p=2): return "" if x is None or (isinstance(x, float) and np.isnan(x)) else f"{x:.{p}f}%"


def stats(eq):
    eq = eq.dropna(); r = eq.pct_change().dropna()
    k = len(r) / ((eq.index[-1] - eq.index[0]).days / 365.25)
    dd = eq / eq.cummax() - 1
    return dict(ret=(eq.iloc[-1] / eq.iloc[0] - 1) * 100, vol=r.std() * np.sqrt(k) * 100, sharpe=r.mean() / r.std() * np.sqrt(k),
                mdd=dd.min() * 100, worst=r.min() * 100, worst_d=r.idxmin().date())


def wk(s): return s[s.index.dayofweek < 5]


def since(x, d0): return x[x.index >= x[:d0].index[-1]]


# --- per symbol-day P&L, hand check: contracts x contract size x the held contract's own close move ---
pos["instrument_id"] = pos.instrument_id.astype(str).str.replace(r"\.0$", "", regex=True)
# the previous close is the held contract's close on the symbol's previous grid date (the .v.0 series' previous bar
# date), the bar the engine consumed last, not the contract's previous print of any kind
grid = fut[["symbol", "d"]].drop_duplicates().sort_values(["symbol", "d"])
grid["prev_d"] = grid.groupby("symbol").d.shift()
cb = cbar.rename(columns={"date": "d"})[["instrument_id", "d", "close"]]
p = pos.merge(cb, on=["instrument_id", "d"], how="left").merge(grid, on=["symbol", "d"], how="left")
p = p.merge(cb.rename(columns={"d": "prev_d", "close": "prev_close"}), on=["instrument_id", "prev_d"], how="left")
p["dc"] = p.close - p.prev_close
p["mult"] = p.symbol.map(mult)
p["stored_pnl"] = p.daily_realized_pnl + p.daily_unrealized_pnl
p["hand_pnl"] = p.quantity * p.mult * p.dc.fillna(0)
p["gap"] = p.stored_pnl - p.hand_pnl
p = p[(p.quantity != 0) | (p.stored_pnl != 0)].sort_values(["symbol", "d"]).copy()
rb = roll.groupby(["d", "symbol"]).roll_bar_move.sum()
p["roll_bar_move"] = [rb.get((d, s), np.nan) for d, s in zip(p.d, p.symbol)]
p["next_gap"] = p.groupby("symbol").gap.shift(-1)
b = p.gap.abs() >= 0.01
p["check"] = "exact"
rolled = p.roll_bar_move.notna()
p.loc[b & rolled & ((p.gap - p.roll_bar_move).abs() < 0.01), "check"] = "roll bar: the old contract's move, booked as roll_bar_move"
p.loc[b & rolled & (p.check == "exact"), "check"] = "roll bar, other"
pair = b & ~rolled & ((p.gap + p.next_gap).abs() < 0.01)
p.loc[pair, "check"] = "held one session, booked the next"
p.loc[pair.shift(1, fill_value=False) & b & (p.check == "exact"), "check"] = "held one session, booked the next"
p.loc[b & (p.check == "exact") & p.close.isna(), "check"] = "no bar of the held contract that day"
p.loc[b & (p.check == "exact"), "check"] = "other (listed in the CSV)"
p[["d", "symbol", "instrument_id", "quantity", "average_price", "close", "dc", "mult", "stored_pnl", "hand_pnl", "gap", "roll_bar_move", "check"]].rename(
    columns={"d": "date", "dc": "close_move", "mult": "contract_size"}).to_csv(f"{A}/symbol_days_hand_check.csv", index=False)
hc = p.groupby("check").gap.agg(["count", "sum"])

# --- per symbol: holdings, forecast, unrounded target, P&L ---
rows = []
for line in open(f"{INP}/loglines.txt"):
    if "BOOK raw_target" in line:
        for s, a, _, _, _ in re.findall(r"([A-Z0-9]+\.v\.0):([-\d.e]+):([-\d.e]+):([-\d]+):([-\d]+)", line):
            rows.append((line[:10], s, float(a)))
bk = pd.DataFrame(rows, columns=["d", "symbol", "target"]); bk["d"] = pd.to_datetime(bk.d)
sig = pd.read_csv(f"{OUT}/live_signals.csv"); sig["d"] = pd.to_datetime(sig.timestamp.str[:10])
sig = sig.groupby(["d", "symbol"]).signal_value.last().reset_index()
cal = pd.date_range(START, END)
q = pos.pivot_table(index="d", columns="symbol", values="quantity", aggfunc="sum").reindex(cal).fillna(0)
bk = bk.merge(sig, on=["d", "symbol"], how="left")
bk["held"] = [q.at[d, s] if s in q.columns else 0 for d, s in zip(bk.d, bk.symbol)]
pn = p.groupby("symbol").stored_pnl.sum(); co = ex.groupby("symbol").total_transaction_costs.sum()
syms = []
for s, g in bk.groupby("symbol"):
    h = g[g.held != 0]
    syms.append(dict(symbol=s.replace(".v.0", ""), sector=sector.get(s), runs_long=int((g.held > 0).sum()), runs_short=int((g.held < 0).sum()),
                     max_contracts=int(g.held.abs().max()), avg_contracts_held=h.held.abs().mean() if len(h) else 0.0,
                     avg_abs_forecast_held=h.signal_value.abs().mean() if len(h) else np.nan,
                     avg_unrounded_target_held=h.target.abs().mean() if len(h) else np.nan, avg_unrounded_target_all=g.target.abs().mean(),
                     median_held_over_target=(h.held.abs() / h.target.abs().clip(lower=0.01)).median() if len(h) else np.nan,
                     gross_pnl=pn.get(s, 0.0), costs=co.get(s, 0.0), net_pnl=pn.get(s, 0.0) - co.get(s, 0.0)))
sy = pd.DataFrame(syms).sort_values("net_pnl", ascending=False)
sy.round(4).to_csv(f"{A}/symbols_year.csv", index=False)

# --- daily series beside the benchmarks; the book without its largest contributor ---
TOP = sy.iloc[0].symbol + ".v.0"
si = p[p.symbol == TOP].groupby("d").stored_pnl.sum().reindex(lr.index).fillna(0)
sic = ex[ex.symbol == TOP].groupby("d").total_transaction_costs.sum().reindex(lr.index).fillna(0)
vx = CAP + (lr.daily_pnl.astype(float) - si + sic).cumsum()
D = pd.DataFrame({"account": v, "account_without_" + TOP.replace(".v.0", ""): vx})
D["sg_trend"] = sg.reindex(D.index)
D["backtest_2y"] = bt.reindex(D.index)
for s in BENCH:
    D[s] = mkt[mkt.symbol == s].set_index("d").adjusted_close.reindex(D.index)
D.index.name = "date"; D.round(4).to_csv(f"{A}/daily_values_and_benchmarks.csv")

SGEND = sg.index[-1] if len(sg) else END
cmp_rows = [("This book", stats(wk(v)[:SGEND])), ("This book without " + TOP.replace(".v.0", ""), stats(wk(vx)[:SGEND]))]
if len(sg): cmp_rows.append(("SG Trend Index", stats(since(sg, START)[:SGEND])))
if len(bt): cmp_rows.append(("Our 2y backtest, same dates", stats(wk(bt[START:SGEND]))))
for s in BENCH:
    cmp_rows.append((s, stats(since(mkt[mkt.symbol == s].set_index("d").adjusted_close, START)[:SGEND])))

R = pd.DataFrame({"This book": wk(v).pct_change(), "SG Trend": sg.pct_change() if len(sg) else np.nan})
for s in BENCH:
    R[s] = mkt[mkt.symbol == s].set_index("d").adjusted_close.pct_change()
R = R[(R.index > START) & (R.index <= END)].dropna(axis=1, how="all").dropna()
corr = R.corr()["This book"].drop("This book")
beta = np.cov(R["This book"], R["SPY"])[0, 1] / R["SPY"].var() if "SPY" in R and len(R) > 2 else float("nan")
M = pd.DataFrame({"This book": v.resample("ME").last(), "SG Trend": sg.resample("ME").last() if len(sg) else np.nan,
                  "SPY": mkt[mkt.symbol == "SPY"].set_index("d").adjusted_close.resample("ME").last() if "SPY" in BENCH else np.nan})
base = pd.DataFrame({"This book": [CAP], "SG Trend": [sg[:START].iloc[-1] if len(sg[:START]) else np.nan],
                     "SPY": [mkt[(mkt.symbol == "SPY") & (mkt.d <= START)].adjusted_close.iloc[-1] if "SPY" in BENCH else np.nan]}, index=[pd.Timestamp("2025-09-30")])
M = pd.concat([base, M[M.index >= START]]).pct_change(fill_method=None).dropna(how="all") * 100
M.index = M.index.strftime("%Y-%m"); M.round(3).to_csv(f"{A}/monthly_returns.csv")

# --- risk readings from the runs' logs ---
ov = []
for line in open(f"{INP}/loglines.txt"):
    if "[RiskManager] OVERLAY " in line:
        kv = dict(re.findall(r"(\w+)=([^\s\]]+)", line.split("OVERLAY ", 1)[1])); kv["date"] = line[:10]; ov.append(kv)
ov = pd.DataFrame(ov)[["date", "binding", "m", "R", "R_jump", "R_shock", "L_g", "L_n", "capital"]]
ov.to_csv(f"{A}/risk_overlay_by_run.csv", index=False)
for c in ["m", "R", "R_jump", "R_shock", "L_g", "L_n", "capital"]: ov[c] = ov[c].astype(float)
lim = dict(re.findall(r"(R_max|R_jump_max|R_shock_max|L_max|L_net_max)=([\d.]+)", [l for l in open(f"{INP}/loglines.txt") if "[RiskManager] OVERLAY " in l][0]))
limits = [("Expected volatility (R)", "overlay_risk", float(lim["R_max"])), ("Jump risk (R_jump)", "overlay_risk_jump", float(lim["R_jump_max"])),
          ("Shock risk (R_shock)", "overlay_risk_shock", float(lim["R_shock_max"])), ("Gross leverage", "overlay_gross_leverage", float(lim["L_max"])),
          ("Net leverage", "overlay_net_leverage", float(lim["L_net_max"]))]

# --- figures for the results table ---
cagr = (v.iloc[-1] / CAP) ** (1 / yrs) - 1
mdd = float(last.max_drawdown) / 100
costs = float(last.total_transaction_costs); rcost = float(last.total_roll_costs)
cpy = costs / yrs; cpct = cpy / CAP; vol = float(last.volatility) / 100
csr = cpct / vol
held = pos[pos.d >= first_fill].groupby("d").quantity.apply(lambda s: s.abs().sum())
held = held.reindex(pd.date_range(first_fill, END)).fillna(0)
turn = (strat.quantity.abs().sum() / yrs) / (2 * held.mean())
r_wd = wk(v).pct_change().dropna()
var95 = -np.percentile(r_wd, 5) * 100; cvar95 = -r_wd[r_wd <= np.percentile(r_wd, 5)].mean() * 100
cy = json.loads(last.calendar_year_returns) if isinstance(last.calendar_year_returns, str) else {}
open_end = int((pos[pos.d == END].quantity != 0).sum())

L = []
w = L.append
w("# CONSERVATIVE live book: %s to %s" % (START.date(), END.date()))
w("")
w("One year of the live runner, run once for every calendar date in order on the database `%s`, each run dated " % NAME +
  "and reading only the prices stored up to the day before it. Every setting it ran on is kept in `config_used/`. "
  "Generated %s UTC from the stored rows." % NOW)
w("")
w("| Item | Value |"); w("|---|---|")
for k_, v_ in [("Book", "CONSERVATIVE_PORTFOLIO, one trend following sleeve on %d futures (%d stored series; a listing pair trades its full-size contract before the listing and the micro after), long and short" % (NINST, NSYM)),
               ("Starting capital", "500,000.00"), ("Runs", "%d dated runs, one per calendar date, every one exit code 0" % len(lr)),
               ("First run / first fill", "%s / %s" % (START.date(), first_fill.date())),
               ("Last settled day", "%s (the run of %s settles it)" % (LAST.date(), END.date())),
               ("Prices stored to", str(fut.d.max().date())),
               ("Years traded", "%.2f" % yrs),
               ("Engine", open(f"{OUT}/checks/engine.txt").read().splitlines()[0].replace("engine: ", "")),
               ("Database", NAME),
               ("Fill price", "each day's trades fill at the previous day's close, with commission, spread and impact costs charged")]:
    w("| %s | %s |" % (k_, k_ and v_))
w("")
w("## Results")
w("")
w("| Measure | Value |"); w("|---|---:|")
res = [("Final account value", f2(v.iloc[-1])),
       ("Total return", pc(float(last.total_cumulative_return))),
       ("Annualised return (compound, per calendar year traded)", pc(cagr * 100)),
       ("Volatility, annualised (account)", pc(float(last.volatility))),
       ("Volatility, annualised (sizing capital)", pc(float(last.volatility_sizing))),
       ("Sharpe ratio", f2(float(last.sharpe_ratio))),
       ("Sortino ratio", f2(float(last.sortino_ratio))),
       ("Max drawdown (account)", pc(float(last.max_drawdown))),
       ("Max drawdown (sizing capital)", pc(float(last.max_drawdown_sizing))),
       ("Calmar ratio (annualised return / max drawdown)", f2(cagr / mdd)),
       ("Worst day", "%s on %s (%s)" % (pc(float(last.worst_day)), last.worst_day_date, str(last.worst_day_symbol).replace(".v.0", ""))),
       ("Best day", pc(float(last.best_day))),
       ("VaR 95 / CVaR 95 (one day, weekday closes)", "%s / %s" % (pc(var95), pc(cvar95))),
       ("Losing calendar years", str(int(last.losing_years))),
       ("Transaction costs (all fills)", f2(costs)),
       ("of which roll costs", f2(rcost)),
       ("Costs per year traded", f2(cpy)),
       ("Costs per year, % of starting capital", pc(cpct * 100)),
       ("Costs in Sharpe units a year / Sharpe before costs", "%s / %s" % (f2(csr, 3), f2(float(last.sharpe_ratio) + csr))),
       ("Turnover, round trips a year (strategy contracts traded / 2 x average contracts held)", f2(turn, 1)),
       ("Strategy fills / roll legs (a contract's strategy fills on one day count once)", "%d / %d" % (int(last.total_strategy_fills), int(last.total_roll_fills))),
       ("Closing trades (round trips)", str(int(last.total_trades))),
       ("Winning days / losing days / days with a P&L", "%d / %d / %d" % (int(last.winning_days), int(last.losing_days), int(last.total_days))),
       ("Win rate / profit factor (days)", "%s / %s" % (pc(float(last.win_rate), 1), f2(float(last.profit_factor)))),
       ("Average winning day / losing day", "%s / %s" % (pc(float(last.avg_win)), pc(float(last.avg_loss)))),
       ("Instruments traded / held at the end", "%d / %d" % (ex.symbol.nunique(), open_end)),
       ("Statistics K", "311.0574 (the same grid and K as the backtests)")]
for a, b_ in res: w("| %s | %s |" % (a, b_))
w("")
w("Every figure is of the account after every cost unless it says sizing capital. The sizing capital is 500,000 less "
  "the drawdown of the book's cumulative P&L from its peak, never above 500,000: it sized the book from %s on the last day. "
  "The live statistics count wins and losses by day; the backtest reports count them by round trip. A roll books the old "
  "contract's move on the roll bar from the contracts' own closes and charges both roll legs in full, the same as the backtests."
  % f2(float(re.findall(r"capital=([\d.]+)", [l for l in open(f"{INP}/loglines.txt") if "SIZING_CAPITAL date=%s" % END.date() in l][0])[0])))
w("")
w("## Calendar-year returns"); w("")
w("| Year | Return |"); w("|---|---:|")
for y_, c_ in sorted(cy.items()): w("| %s | %s%s |" % (y_, pc(c_["return"]), " (part)" if c_.get("partial") else ""))
w("")
w("## Monthly returns"); w("")
w("| Month | This book | SG Trend | SPY |"); w("|---|---:|---:|---:|")
for i, r_ in M.iterrows(): w("| %s | %s | %s | %s |" % (i, pc(r_["This book"]), pc(r_["SG Trend"]), pc(r_["SPY"])))
w("")
w("## Against trend followers and markets"); w("")
w("Same dates for every row, %s to %s (the last SG Trend date stored). Weekday closes; a weekend's P&L counts on Monday." % (START.date(), SGEND.date()))
w(""); w("| | Return | Volatility | Sharpe | Max drawdown | Worst day |"); w("|---|---:|---:|---:|---:|---|")
for n_, s_ in cmp_rows:
    w("| %s | %s | %s | %s | %s | %s (%s) |" % (n_, pc(s_["ret"], 1), pc(s_["vol"], 1), f2(s_["sharpe"]), pc(s_["mdd"], 1), pc(s_["worst"], 1), s_["worst_d"]))
w("")
w("Daily correlation of this book with: %s. Beta to SPY %.2f." % (", ".join("%s %.2f" % (k_, x_) for k_, x_ in corr.items()), beta))
w("")
w("## Where the return came from"); w("")
top = sy.iloc[0]; tot = float(sy.net_pnl.sum())
w("The largest contributor was %s (%s): %s of the year's %s, holding at most %d contracts while the strategy's own size "
  "for it averaged %s contracts. Without it the year is %s."
  % (top.symbol, top.sector, f2(float(top.net_pnl), 0), f2(tot, 0), int(top.max_contracts), f2(float(top.avg_unrounded_target_held)),
     pc((vx.iloc[-1] / CAP - 1) * 100, 1)))
w("")
w("| Symbol | Sector | Runs long / short | Max contracts | Average signal held | Average strategy size held (contracts) | Net P&L |")
w("|---|---|---|---:|---:|---:|---:|")
for _, r_ in sy[sy.runs_long + sy.runs_short > 0].iterrows():
    w("| %s | %s | %d / %d | %d | %s | %s | %s |" % (r_.symbol, r_.sector, r_.runs_long, r_.runs_short, r_.max_contracts,
                                                   f2(r_.avg_abs_forecast_held, 1), f2(r_.avg_unrounded_target_held), f2(r_.net_pnl, 0)))
w("")
w("Never held: %s. The signal is the forecast on a scale capped at 20. Full detail in `analysis/symbols_year.csv`." %
  ", ".join(sy[sy.runs_long + sy.runs_short == 0].symbol))
w("")
sec = sy.groupby("sector").net_pnl.sum().sort_values(ascending=False)
w("By sector: " + ", ".join("%s %s" % (k_, f2(x_, 0)) for k_, x_ in sec.items()) + ".")
w("")
w("## Biggest days"); w("")
w("| Day | P&L | Return | Largest contributors |"); w("|---|---:|---:|---|")
dp = lr.daily_pnl.astype(float)
for d_ in list(dp.nlargest(5).index) + list(dp.nsmallest(5).index):
    s_ = p[p.d == d_].set_index("symbol").stored_pnl.sort_values(key=abs, ascending=False).head(3)
    w("| %s | %s | %s | %s |" % (d_.date(), f2(dp[d_], 0), pc(dp[d_] / v.shift(1)[d_] * 100), ", ".join("%s %s" % (k_.replace(".v.0", ""), f2(x_, 0)) for k_, x_ in s_.items())))
w("")
w("The 10 best days made %s of the year's P&L." % pc(dp.nlargest(10).sum() / dp.sum() * 100, 0))
w("")
w("## Risk limits"); w("")
w("| Limit | Setting | Highest on the held book | Days over |"); w("|---|---:|---:|---:|")
for n_, c_, l_ in limits:
    s_ = lr[c_].dropna().astype(float).abs()
    w("| %s | %s | %s on %s | %d |" % (n_, f2(l_), f2(s_.max(), 3), s_.idxmax().date(), int((s_ > l_ + 1e-9).sum())))
cut = ov[ov.binding != "none"]
w("")
w("The risk check scaled the book down on %d of %d runs that traded (%s), by at most %s. Readings are on the sizing "
  "capital; volatility limits are annual." % (len(cut), len(ov), ", ".join(cut.date), pc((1 - cut.m.min()) * 100, 1)))
w("")
w("## Checks"); w("")
w("| Check | Result |"); w("|---|---|")
w("| One row per calendar date; value moves by the day's P&L; P&L = realized + unrealized - costs; costs add up; each roll is two legs netting to zero; positions change only by the day's trades; every day settled but the last | all pass over the whole year (`checks/check.txt`) |")
w("| Each symbol's daily P&L recomputed by hand (contracts x contract size x the held contract's own close move) | %s |" % "; ".join("%s: %d" % (k_, int(r_["count"])) for k_, r_ in hc.iterrows()))
w("| Look-ahead | none: a run reads prices through the day before; the same holdings one day later earn about the same |")
w("")
w("## Files in this folder"); w("")
w("| File | What it holds |"); w("|---|---|")
for a, b_ in [("`RUN_REPORT.md`", "this report"),
              ("`config_used/`", "`portfolio.json`, `risk.json` and `defaults.json` as the runs used them (database login and address removed)"),
              ("`live_results.csv`", "one row per date: account value, daily and total P&L, costs, leverage, margin, risk readings, the running statistics"),
              ("`live_positions.csv`", "the book each day: contracts per symbol, average price, the day's P&L"),
              ("`live_executions.csv`", "every fill: date, contract, side, quantity, price, fee, spread and impact parts, cost, STRATEGY or ROLL"),
              ("`live_signals.csv`", "each run's forecast per symbol"),
              ("`live_equity_curve.csv`", "account value per date"),
              ("`live_run_metadata.csv`", "each run's configuration as the engine recorded it"),
              ("`analysis/symbols_year.csv`", "per symbol: runs long and short, contracts, average signal, the strategy's unrounded size, held over that size, P&L and costs"),
              ("`analysis/symbol_days_hand_check.csv`", "every symbol-day: stored P&L, hand P&L, the difference and its class"),
              ("`analysis/daily_values_and_benchmarks.csv`", "account value, value without the largest contributor, SG Trend, the 2y backtest, SPY, QQQ, AGG, TLT, GLD per date"),
              ("`analysis/monthly_returns.csv`", "monthly returns of this book, SG Trend and SPY"),
              ("`analysis/risk_overlay_by_run.csv`", "the risk check's readings and the limit that bound, per run"),
              ("`checks/`", "the identity checks (`check.txt`), each run's exit code and time (`progress.txt`), the engine commit, the other books' fingerprints, each run's log"),
              ("`bodies/`", "each run's report body as it would have been mailed (built, never sent)")]:
    w("| %s | %s |" % (a, b_))
w("")
w("From the database: `select * from trading.live_results where portfolio_id = 'CONSERVATIVE_PORTFOLIO' and date between "
  "'%s' and '%s'`; the same portfolio id keys `positions`, `executions`, `signals` and `equity_curve`." % (START.date(), END.date()))
open(f"{OUT}/RUN_REPORT.md", "w").write("\n".join(L) + "\n")
with open(f"{OUT}/export.txt", "w") as f:
    for n_ in sorted(os.listdir(OUT)):
        if n_.endswith(".csv"): f.write("%s: %d rows\n" % (n_, len(pd.read_csv(os.path.join(OUT, n_)))))
print("written", OUT)
