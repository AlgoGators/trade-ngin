# Live Trading Module

## Overview

The live module (`src/live/`, headers in `include/trade_ngin/live/`) holds the parts a live runner uses to run one
date for one book: loading the stored book and the bars, pricing, P&L, margin, executions, the stored metrics, the
positions files, and for the equity book the corporate actions. The runners themselves are in `apps/strategies/`.

Where the detail lives:

| Topic | Document |
|---|---|
| What a run does on a date, the P&L frame, what each table and column holds | [docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md) |
| The container, the schedule, the wrapper and its exit codes, the watchdog, manual and catch-up rules, migrations | [docs/performance_upkeep.md](../../docs/performance_upkeep.md) |
| Which table or series is the source of truth for each fact | [docs/DATA_SOURCES_OF_TRUTH.md](../../docs/DATA_SOURCES_OF_TRUTH.md) |
| `average_price` on a stored position | [docs/AVERAGE_PRICE_LIFECYCLE.md](../../docs/AVERAGE_PRICE_LIFECYCLE.md) |
| The futures sizing loop and the risk modules | [docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md), [docs/RISK_MODULES.md](../../docs/RISK_MODULES.md) |
| Rolls and costs | [docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md), [docs/COST_MODEL.md](../../docs/COST_MODEL.md) |
| Equity corporate actions and the broker's basis | [docs/CORP_ACTIONS_DATA_BOUNDARY.md](../../docs/CORP_ACTIONS_DATA_BOUNDARY.md), [docs/BROKER_BASIS_RECONCILIATION.md](../../docs/BROKER_BASIS_RECONCILIATION.md) |
| Configuration files and keys | [docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md) |

---

## Files

Compiled sources in `src/live/`:

| File | Class | Role |
|---|---|---|
| `live_trading_coordinator.cpp` | `LiveTradingCoordinator` | owns and wires the live components for one book |
| `live_data_loader.cpp` | `LiveDataLoader` | the reads of a book's stored results, positions and history that the live components share (the runners also query the database directly for their guards) |
| `live_price_manager.cpp` | `LivePriceManager` | the closes a run prices with |
| `live_pnl_manager.cpp` | `LivePnLManager` | the day's P&L per position |
| `live_metrics_calculator.cpp` | `LiveMetricsCalculator` | the daily figures of a results row (pure calculations) |
| `live_historical_metrics.cpp` | `LiveHistoricalMetricsCalculator` | the since-inception figures |
| `execution_manager.cpp` | `ExecutionManager` | builds the day's executions from the change in the book |
| `execution_price_resolver.cpp` | `ExecutionPriceResolver` | the price a fill is booked at |
| `margin_manager.cpp` | `MarginManager` | posted and maintenance margin, notional and leverage |
| `csv_exporter.cpp` | `CSVExporter` | the positions files attached to the report |
| `broker_frame.cpp` | (functions) | relates the book's adjusted cost basis to the broker's |
| `corporate_actions_applier.cpp`, `corporate_actions_classification.cpp`, `corporate_actions_lifecycle.cpp`, `corporate_actions_audit_log.cpp` | `CorporateActionsApplier`, `CorporateActionsAuditLog` | equity corporate actions: classification, application, the applied record |

Most of the other headers in `include/trade_ngin/live/` are header-only rules a runner calls, each with its design
comment at the top (`pnl_manager_base.hpp` and `price_manager_base.hpp` are base classes). The ones an operator
meets by name in a log:

| Header | Rule |
|---|---|
| `risk_module_failure.hpp` | the risk step refused the book or the sizing read failed: the book is held, the day is stored and marked, the run exits 3 |
| `run_metadata_marks.hpp` | the `risk_refusal` and `strict_assertion` marks on the day's `trading.live_run_metadata` row |
| `live_sizing_read.hpp` | the read of the account's settled equity that sizes the futures book |
| `late_bar_warning.hpp` | the one-line warning for a bar that arrived after the run that should have consumed it |
| `stored_book_ownership.hpp` | refuses a run that would not load positions the portfolio has stored |
| `live_listing_guard.hpp` | refuses a run whose book or symbol list disagrees with the declared listing dates |
| `data_freshness.hpp` | the feed staleness checks |
| `email_body_file.hpp` | the report body written to a file on a run that does not send |

---

## Running Live Trading

### Binaries

| Binary | Source | Book | Configuration it loads |
|---|---|---|---|
| `live_portfolio_conservative` | `apps/strategies/live_portfolio_conservative.cpp` | CONSERVATIVE, the futures trend book (the scheduled one) | `./config`, portfolio `conservative` |
| `live_portfolio` | `apps/strategies/live_portfolio.cpp` | BASE, the two-sleeve futures test book | `./config`, portfolio `base` |
| `live_equity_mr` | `apps/strategies/live_equity_mean_reversion.cpp` | the equity mean reversion book | `./config`, portfolio `equity_mr` |

A Release build puts them in `build/bin/Release/`. Each resolves `./config` and `logs/` from the current directory,
so start it from the directory that holds `config/`.

### Command Line

A live run always carries a date.

```bash
# Production (what the scheduled wrapper runs): the date and --send-email
./build/bin/Release/live_portfolio_conservative 2026-04-28 --send-email

# A test or a replay: the date, no --send-email (nothing is mailed)
./build/bin/Release/live_portfolio_conservative 2026-04-28

# The same, with the report body written to a file instead
TRADE_NGIN_EMAIL_BODY_DIR=/path/to/existing/dir ./build/bin/Release/live_portfolio_conservative 2026-04-28
```

### Arguments

All three binaries take the same two arguments, in either order. An argument that is neither a date nor
`--send-email` prints the usage line and exits 1. The equity runner accepts only an exact `YYYY-MM-DD`
(`core::parse_utc_date`, `include/trade_ngin/core/time_utils.hpp:134-153`). The futures runners test only that a
date could be read from the argument (`apps/strategies/live_portfolio_conservative.cpp:113-125`), and in every
runner a second date on the line replaces the first with no message: type the date exactly as `YYYY-MM-DD`, once.

| Argument | Format | Description |
|---|---|---|
| Date | `YYYY-MM-DD` | The date to run. Always given. A run started without a date takes the wall clock and turns the email on. Do not start one: nothing in the code refuses it. The futures runners read the date as midnight in the host's local time and the equity runner as midnight UTC; the three run modes are in [docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md), section 1. |
| `--send-email` | Flag | Build the report and mail it. Production passes it; a test or a replay never does. |

### Environment

| Variable | Value | Description |
|----------|-------|-------------|
| TRADE_NGIN_EMAIL_BODY_DIR | An existing directory | Futures runners only (`live_portfolio`, `live_portfolio_conservative`). On a run that does NOT send (a date given, no `--send-email`) the report body is built exactly as for a send and written there as `email_body_<portfolio_id>_<date>.html`; nothing is mailed. `--send-email` wins: on a run that sends (`--send-email`, or a run without a date) the variable is ignored, the report is mailed as usual, one WARN line says so and no file is written. An empty value is the same as unset. The directory is not created; a second run of the same book and date replaces the file. |

### Rules

- One runner at a time: never start two live or backtest runners together.
- A replay runs every date forward in order. A lone re-run of an older date is not supported, and the runner does
  not refuse it.
- The futures book runs every calendar day; a run refuses to start when the previous day's run is missing.

A futures live run exits 0 when it reaches the end of the run with the book not refused, 3 when it reaches the
end with the book held because the risk step refused it or the sizing read failed, and 1 when it stops early.
Exit 0 covers a completed day, a carried day, and a run in which a store failed, was logged and was passed over;
the evidence of a completed day is the date's `trading.live_results` row, not the exit code. The wrapper also
returns 0, without starting the binary, when it finds its lock held (nothing stored). A refusal by the risk step never ends at 0: the
`live_run_metadata` row is marked `risk_refusal` and the run ends at 3, whether a module could not answer or a
sleeve module decided to refuse, or at 1 when the refused book has no stored previous positions to be held at.
Exit 1 stores nothing when the run refuses before its `live_run_metadata` row is written, and can leave that
row, the signals or a half-written day when it stops later. The equity runner and the backtest runners return
only 0 or 1.

The reasons, the full exit-code table (0, 1, 3, and the wrapper's 127), the log locations and the scheduled job are
in [docs/performance_upkeep.md](../../docs/performance_upkeep.md).

---

## What a run stores

A live run writes the `trading` schema: `trading.positions`, `trading.executions`, `trading.signals`,
`trading.live_results`, `trading.equity_curve` and `trading.live_run_metadata`, and for the equity book
`trading.corp_action_applied`. It reads its start date from `trading.strategy_trading_days_metadata`. There is no
`live` schema. What one
row of each table is, its key, when it is final and the meaning of each P&L column are in
[docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md): a row dated D is final only after the run of D+1, and
the P&L columns are not a single quantity times price change formula.

---

## Email Report

A run that sends builds an HTML report (`EmailSender::generate_trading_report_body`, `src/core/email_sender.cpp`,
with charts from `src/core/chart_generator.cpp`) and mails it with the positions files from `CSVExporter` attached.
On a day the book was held because the risk step refused it or the sizing read failed, the subject and the top of the
body are flagged (`risk_module_failure.hpp`). Recipients and SMTP settings are in each portfolio's `email.json`
([docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md)).

---

## Testing

The live tests are in `tests/live/` and are part of `trade_ngin_tests`:

```bash
ctest --test-dir build --output-on-failure
```

---

## References

- [Strategy Development Guide](../strategy/README.md)
- [Portfolio Module](../portfolio/README.md)
- [Backtest Module](../backtest/README.md)
- [Transaction Cost Module](../transaction_cost/README.md)
- [Data Module](../data/README.md)
