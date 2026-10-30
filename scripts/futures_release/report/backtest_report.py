#!/usr/bin/env python3
"""Writes the three reports of the backtest results folder (09_backtests.sh report).

Usage: backtest_report.py <set folder> <knobs.md>   (the database from the PG* environment: lib.zsh sets it;
       the connection comes from the environment, never from arguments, so no address reaches a file)

  <results folder>/CONFIG_AND_KNOBS.md   every tuned number and the config files in full (from knobs.md and the
                                         config the set's first run used)
  <set folder>/SUMMARY.md                the set's windows side by side, their run ids and folders
  <run folder>/RUN_REPORT.md             one run: its id, window, engine, results, calendar years, files
"""
import csv, json, os, subprocess, sys, datetime as dt

SET, KNOBS = sys.argv[1:3]
NAME = os.environ["PGDATABASE"]
SET = SET.rstrip("/")
ROOT = os.path.dirname(SET)
SETNAME = os.path.basename(SET)
END = SETNAME.replace("conservative_end_", "").replace("conservative_pinned_", "")
PINNED = SETNAME.startswith("conservative_pinned_")
NOW = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M")
PRICES = os.path.join(SET, "prices.txt")
LASTBAR = open(PRICES).read().strip() if os.path.exists(PRICES) else "?"
WINDOWS = ("2y", "5y", "10y", "16y")


def sql(q):
    return subprocess.run([os.path.join(os.environ.get("PGBIN", ""), "psql"), "-X", "-At", "-v", "ON_ERROR_STOP=1", "-c", q],
                          capture_output=True, text=True, check=True).stdout.strip()


def jrow(q):
    out = sql("select row_to_json(t) from (%s) t" % q)
    return json.loads(out) if out else None


def num(v, f="{:,.2f}"):
    return "" if v is None else f.format(float(v))


def pct(v, places=2):
    return "" if v is None else ("{:." + str(places) + "f}%").format(float(v) * 100)


def years(a, b):
    try:
        return (dt.date.fromisoformat(b) - dt.date.fromisoformat(a)).days / 365.25
    except Exception:
        return None


# the set's runs: the last successful run of each window
runs = {}
with open(os.path.join(SET, "runs.tsv")) as f:
    for r in csv.DictReader(f, delimiter="\t"):
        if r["rc"] == "0" and r["run_id"] not in ("", "-"):
            runs[r["label"]] = r
ORDER = [l for l in WINDOWS if l in runs]
if not ORDER:
    sys.exit("no successful run in runs.tsv")
folders = {}
for d in sorted(os.listdir(SET)):
    p = os.path.join(SET, d, "run_id.txt")
    if os.path.isfile(p):
        folders[open(p).read().strip()] = d

res, ex = {}, {}
for l in ORDER:
    rid = runs[l]["run_id"].replace("'", "''")
    res[l] = jrow("select * from backtest.results where run_id='%s'" % rid)
    ex[l] = jrow(
        "select (select equity from backtest.equity_curve where run_id='{0}' order by timestamp desc limit 1) as last_equity, "
        "(select count(*) from backtest.executions where run_id='{0}' and execution_type='ROLL') as roll_legs, "
        "(select count(*) from backtest.executions where run_id='{0}' and coalesce(execution_type,'STRATEGY')<>'ROLL') as strategy_fills, "
        "(select count(distinct symbol) from backtest.executions where run_id='{0}') as symbols_traded, "
        "(select count(*) from backtest.final_positions f where run_id='{0}' and quantity<>0 and date=(select max(date) from backtest.final_positions g where g.run_id='{0}')) as open_at_end, "
        "(select portfolio_config::jsonb->'statistics_K' from backtest.run_metadata where run_id='{0}' limit 1) as k, "
        "(select portfolio_config::jsonb->'warmup_days' from backtest.run_metadata where run_id='{0}' limit 1) as warmup, "
        "(select sum(abs(quantity)) from backtest.executions where run_id='{0}' and coalesce(execution_type,'STRATEGY')<>'ROLL') as strategy_contracts, "
        "(select avg(held) from (select date, sum(abs(quantity)) as held from backtest.final_positions where run_id='{0}' "
        "and date >= (select min(timestamp)::date from backtest.executions where run_id='{0}') group by date) h) as avg_held".format(rid))


def yrs_traded(l):
    return years(runs[l]["first_traded_day"], runs[l]["last_day"])


def cagr(l):
    tr, y = res[l]["total_return"], yrs_traded(l)
    return None if tr is None or not y else (1 + float(tr)) ** (1 / y) - 1


def per_year(l, v):
    y = yrs_traded(l)
    return None if v is None or not y else float(v) / y


def cost_pct_year(l):
    v = per_year(l, res[l]["transaction_costs"])
    return None if v is None else v / 500000.0


def cost_sr(l):
    c, vol = cost_pct_year(l), res[l]["volatility"]
    return None if c is None or not vol else c / float(vol)


def sharpe_before_costs(l):
    c, sr = cost_sr(l), res[l]["sharpe_ratio"]
    return None if c is None or sr is None else float(sr) + c


def turnover(l):
    traded, held = per_year(l, ex[l]["strategy_contracts"]), ex[l]["avg_held"]
    return None if traded is None or not held else traded / (2.0 * float(held))


MEASURES = [
    ("Final account value", lambda l: num(ex[l]["last_equity"])),
    ("Total return", lambda l: pct(res[l]["total_return"])),
    ("Annualised return (compound, per calendar year traded)", lambda l: pct(cagr(l))),
    ("Volatility, annualised (account)", lambda l: pct(res[l]["volatility"])),
    ("Volatility, annualised (sizing capital)", lambda l: pct(res[l].get("volatility_sizing"))),
    ("Sharpe ratio", lambda l: num(res[l]["sharpe_ratio"], "{:.2f}")),
    ("Sortino ratio", lambda l: num(res[l]["sortino_ratio"], "{:.2f}")),
    ("Max drawdown (account)", lambda l: pct(res[l]["max_drawdown"])),
    ("Max drawdown (sizing capital)", lambda l: pct(res[l].get("max_drawdown_sizing"))),
    ("Calmar ratio", lambda l: num(res[l]["calmar_ratio"], "{:.2f}")),
    ("Worst day", lambda l: "%s on %s (%s)" % (pct(res[l].get("worst_day")), res[l].get("worst_day_date") or "", res[l].get("worst_day_symbol") or "")),
    ("VaR 95 / CVaR 95 (one day)", lambda l: "%s / %s" % (pct(res[l]["var_95"]), pct(res[l]["cvar_95"]))),
    ("Monthly skew / tail ratio", lambda l: "%s / %s" % (num(res[l].get("monthly_skew"), "{:.2f}"), num(res[l].get("monthly_tail_ratio"), "{:.2f}"))),
    ("Losing calendar years", lambda l: "" if res[l].get("losing_years") is None else str(res[l]["losing_years"])),
    ("Transaction costs (all fills)", lambda l: num(res[l]["transaction_costs"])),
    ("of which roll costs", lambda l: num(res[l]["roll_costs"])),
    ("Costs per year traded", lambda l: num(per_year(l, res[l]["transaction_costs"]))),
    ("Costs per year, % of starting capital", lambda l: pct(cost_pct_year(l))),
    ("Costs in Sharpe units a year / Sharpe before costs", lambda l: "%s / %s" % (num(cost_sr(l), "{:.3f}"), num(sharpe_before_costs(l), "{:.2f}"))),
    ("Turnover, round trips a year (strategy contracts traded / 2 x average contracts held)", lambda l: num(turnover(l), "{:.1f}")),
    ("Strategy fills / roll legs (a contract's strategy fills on one day count once)", lambda l: "%s / %s" % (res[l]["total_strategy_fills"], ex[l]["roll_legs"])),
    ("Closing trades (round trips)", lambda l: str(res[l]["total_trades"])),
    ("Win rate / profit factor (round trips)", lambda l: "%s / %s" % (pct(res[l]["win_rate"], 1), num(res[l]["profit_factor"], "{:.2f}"))),
    ("Average win / average loss (dollars)", lambda l: "%s / %s" % (num(res[l]["avg_win"]), num(res[l]["avg_loss"]))),
    ("Average holding period (days)", lambda l: num(res[l]["avg_holding_period"], "{:.1f}")),
    ("Instruments traded / held at the end", lambda l: "%s / %s" % (ex[l]["symbols_traded"], ex[l]["open_at_end"])),
    ("Statistics K / warm-up rows", lambda l: "%s / %s" % (ex[l]["k"], ex[l]["warmup"])),
]
CAGR_NOTE = ("The annualised return is (1 + total return) ^ (1 / years traded) - 1, years counted in calendar days from "
             "the first traded day to the last day. The engine's Calmar ratio uses its own annualised return on the "
             "statistics grid, so the two can differ slightly. Every figure is of the account after every cost unless "
             "it says sizing capital; the definitions are in `CONFIG_AND_KNOBS.md`, section 10. Costs in Sharpe units are the "
             "year's costs as a share of capital over the annual volatility, so the Sharpe before costs is the Sharpe "
             "plus that figure, approximately. The returns leave the market "
             "move of the day a contract rolls off the books while charging both roll legs in full; see "
             "`CONFIG_AND_KNOBS.md`, section 8.")


def calendar(l):
    cy = res[l].get("calendar_year_returns")
    if isinstance(cy, str):
        cy = json.loads(cy)
    return cy or {}


def cy_cell(c):
    if c is None:
        return ""
    v = c.get("return") if isinstance(c, dict) else c
    return pct(v) + (" (part)" if isinstance(c, dict) and c.get("partial") else "")


def window_row(l):
    r = runs[l]
    y = yrs_traded(l)
    return r["lookback_years"], r["first_bar"], r["first_traded_day"], r["last_day"], ("" if y is None else "%.2f" % y)


FILES = [
    ("`RUN_REPORT.md`", "this report"),
    ("`run_id.txt`", "the run id, the key of this run's rows in every backtest table"),
    ("`window.txt`", "the window, the engine commit, the binary's checksum, the database, the run time"),
    ("`config_used/`", "`portfolio.json`, `risk.json` and `defaults.json` exactly as the run used them (the database login removed)"),
    ("`backtest_results.csv`", "the run's row of summary statistics"),
    ("`backtest_equity_curve.csv`", "one row per bar date: account value, the overlay's five readings, the day's risk detail (sizing capital, risk requested, the binding limit)"),
    ("`backtest_executions.csv`", "every fill: date, contract, side, quantity, price, fee, spread and impact parts, cost, STRATEGY or ROLL"),
    ("`backtest_final_positions.csv`", "the book each day: contracts, average price, realized and unrealized P&L"),
    ("`backtest_run_metadata.csv`", "the configuration as the engine recorded it (sleeves, window rule, warm-up, K, listing dates, rule removals)"),
    ("`backtest_signals.csv`", "header only: no backtest writes signals"),
    ("`result_block.txt`", "the summary the runner printed"),
    ("`run_log_filtered.txt.gz`, `run_log_tail.txt`", "the runner's WARN, ERROR and key lines, and its last 300 lines"),
    ("`export.txt`", "the row count of each exported table"),
]

# RUN_REPORT.md in each run folder
for l in ORDER:
    r = runs[l]
    folder = folders.get(r["run_id"])
    if not folder:
        continue
    lb, fb, ft, ld, y = window_row(l)
    L = []
    w = L.append
    w("# CONSERVATIVE backtest %s: %s to %s" % (l, ft, ld))
    w("")
    w("One run of the set `%s`. The set's windows side by side are in `../SUMMARY.md`; every setting the run used is "
      "explained in `../../CONFIG_AND_KNOBS.md` and kept exactly as it ran in `config_used/` here. Generated %s UTC "
      "from the stored rows." % (SETNAME, NOW))
    w("")
    w("| Item | Value |")
    w("|---|---|")
    endtxt = "to the pinned end %s" % END if PINNED else "ending when the run started (%s UTC)" % END
    for a, b in [("Run id", "`%s`" % r["run_id"]), ("Window", "%s: %s years of prices loaded, %s" % (l, lb, endtxt)),
                 ("Prices stored to", LASTBAR), ("First day of the equity curve", fb),
                 ("Warm-up", "256 bar dates (no trading, not in any statistic)"), ("First traded day", ft),
                 ("Last day", ld), ("Years traded", y), ("Engine", r["engine"]), ("Database", r["database"]),
                 ("Run took", "%s s, finished %s" % (r["seconds"], r["finished"]))]:
        w("| %s | %s |" % (a, b))
    w("")
    w("## Results")
    w("")
    w("| Measure | Value |")
    w("|---|---:|")
    for name, f in MEASURES:
        w("| %s | %s |" % (name, f(l)))
    w("")
    w(CAGR_NOTE)
    w("")
    cy = calendar(l)
    if cy:
        w("## Calendar-year returns")
        w("")
        w("| Year | Return |")
        w("|---|---:|")
        for yr in sorted(cy):
            w("| %s | %s |" % (yr, cy_cell(cy[yr])))
        w("")
    w("## Files in this folder")
    w("")
    w("| File | What it holds |")
    w("|---|---|")
    for a, b in FILES:
        w("| %s | %s |" % (a, b))
    w("")
    w("From the database: `select * from backtest.results where run_id = '%s'`; the same run id keys `equity_curve`, "
      "`executions`, `final_positions` and `run_metadata`." % r["run_id"])
    open(os.path.join(SET, folder, "RUN_REPORT.md"), "w").write("\n".join(L) + "\n")

# SUMMARY.md in the set folder
L = []
w = L.append
w("# CONSERVATIVE backtests: %s" % SETNAME.replace("conservative_", "").replace("_", " "))
w("")
WHY = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "why_this_setup.md")).read().rstrip()
w(WHY)
w("")
if PINNED:
    w("One window pinned to the end date %s, to compare with a run made earlier on that end date. Prices stored to "
      "%s. Generated %s UTC from the stored rows of `%s`." % (END, LASTBAR, NOW, NAME))
else:
    w("%d window%s of the CONSERVATIVE book, run on %s UTC on the same engine, the same config and the same prices "
      "(stored to %s), so they end on the same last bar. Generated %s UTC from the stored rows of `%s`."
      % (len(ORDER), "" if len(ORDER) == 1 else "s", END, LASTBAR, NOW, NAME))
w("")
w("Every setting is explained in `../CONFIG_AND_KNOBS.md`. Each run's own report, config and tables are in its folder.")
w("")
w("## The runs")
w("")
w("| Window | Prices loaded | First traded day | Last day | Years traded | Run id | Folder |")
w("|---|---|---|---|---:|---|---|")
for l in ORDER:
    lb, fb, ft, ld, y = window_row(l)
    fo = folders.get(runs[l]["run_id"], "?")
    w("| %s | %s years (from %s) | %s | %s | %s | `%s` | [`%s/`](%s/RUN_REPORT.md) |" % (l, lb, fb, ft, ld, y, runs[l]["run_id"], fo, fo))
w("")
w("Engine: `%s`. Database: `%s`." % (runs[ORDER[0]]["engine"], runs[ORDER[0]]["database"]))
w("")
w("## Results side by side")
w("")
w("| Measure | " + " | ".join(ORDER) + " |")
w("|---|" + "---:|" * len(ORDER))
for name, f in MEASURES:
    w("| %s | %s |" % (name, " | ".join(f(l) for l in ORDER)))
w("")
w(CAGR_NOTE)
w("")
allyears = sorted({yr for l in ORDER for yr in calendar(l)})
if allyears:
    w("## Calendar-year returns")
    w("")
    w("| Year | " + " | ".join(ORDER) + " |")
    w("|---|" + "---:|" * len(ORDER))
    for yr in allyears:
        w("| %s | %s |" % (yr, " | ".join(cy_cell(calendar(l).get(yr)) for l in ORDER)))
    w("")
w("## Costs against Carver's guidelines")
w("")
w("Carver judges cost by what it takes out of the Sharpe ratio, not by the number of trades: the whole system's costs "
  "should stay well under a third of its Sharpe before costs, and one trading rule on one contract should cost no more "
  "than about 0.15 Sharpe units a year (the limit behind the rules removed by cost). His turnovers for the six speeds, "
  "fastest to slowest, are 98.5, 50.2, 25.4, 13.2, 7.6 and 5.2 round trips a year; a book's positions turn over less "
  "than its forecasts because of the no-trade buffer, whole-contract rounding and the cost weight in the search.")
w("")
w("| | " + " | ".join(ORDER) + " |")
w("|---|" + "---:|" * len(ORDER))
w("| Costs in Sharpe units a year | %s |" % " | ".join(num(cost_sr(l), "{:.3f}") for l in ORDER))
w("| Sharpe before costs | %s |" % " | ".join(num(sharpe_before_costs(l), "{:.2f}") for l in ORDER))
w("| Share of the Sharpe before costs taken by costs | %s |" % " | ".join(
    pct(cost_sr(l) / sharpe_before_costs(l), 1) if cost_sr(l) is not None and sharpe_before_costs(l) else "" for l in ORDER))
w("| Guideline: under a third | %s |" % " | ".join(
    ("within" if cost_sr(l) / sharpe_before_costs(l) < 1 / 3 else "OVER") if cost_sr(l) is not None and sharpe_before_costs(l) and sharpe_before_costs(l) > 0 else "n/a (Sharpe before costs not positive)" for l in ORDER))
w("| Turnover, round trips a year | %s |" % " | ".join(num(turnover(l), "{:.1f}") for l in ORDER))
w("")
w("`runs.tsv` lists every run made for this set (a failed attempt is a row with a non-zero exit code and a "
  "`*_FAILED_*` folder).")
open(os.path.join(SET, "SUMMARY.md"), "w").write("\n".join(L) + "\n")

# CONFIG_AND_KNOBS.md at the root of the results folder
cfg_dir = os.path.join(SET, folders.get(runs[ORDER[0]]["run_id"], ""), "config_used")


def strip(o):
    if isinstance(o, dict):
        return {k: strip(v) for k, v in o.items() if not k.startswith("_")}
    return o


knobs = open(KNOBS).read()
p = json.load(open(os.path.join(cfg_dir, "portfolio.json")))
rk = json.load(open(os.path.join(cfg_dir, "risk.json")))
d = json.load(open(os.path.join(cfg_dir, "defaults.json")))
for k, v in {"{{PORTFOLIO_JSON}}": json.dumps(strip(p), indent=2),
             "{{RISK_JSON}}": json.dumps(strip(rk), indent=2),
             "{{DEFAULTS_JSON}}": json.dumps(strip({a: b for a, b in d.items() if a != "database"}), indent=2)}.items():
    knobs = knobs.replace(k, v)
L = ["# CONSERVATIVE backtests: config and every tuned number", "",
     "The settings the backtests in this folder run on. Engine `%s`; config as used by the set `%s` (written %s UTC). "
     "Each set folder has a `SUMMARY.md` of its windows; each run folder has its own `RUN_REPORT.md`, its tables, and "
     "the exact config it ran on in `config_used/`, which stays the record for that run if the config changes later."
     % (runs[ORDER[0]]["engine"], SETNAME, NOW), "", WHY, "", knobs.rstrip(), ""]
open(os.path.join(ROOT, "CONFIG_AND_KNOBS.md"), "w").write("\n".join(L))
print("written: %s, %s, and RUN_REPORT.md in %d run folder(s)" % (
    os.path.join(ROOT, "CONFIG_AND_KNOBS.md"), os.path.join(SET, "SUMMARY.md"), len(ORDER)))
