# Live Pipeline Specification

This document describes what the live pipeline must achieve, how to run it, and how components interact. It references `apps/backtest/bt_trend.cpp` and `apps/strategies/live_trend.cpp` for implementation details.

## Objectives
- Generate daily positions using the same core logic as backtesting
- Email current positions to subscribers and internal members
- Log live positions similarly to backtesting logs
- Persist and reuse prior-day positions; mark them to today's prices and expose metrics

## Executables
- Backtest reference: `apps/backtest/bt_trend.cpp`
- Live run: `apps/strategies/live_trend.cpp`

## Required inputs
- `config.json` with DB credentials and strategy settings
- Reachable PostgreSQL with required market data

## Run commands
```bash
# Backtest (reference)
./build/bin/Release/bt_trend --config ./config.json

# Live pipeline
./build/bin/Release/live_trend --config ./config.json
```

## Emailing positions
- Build a daily summary: symbol, quantity, price, market value, notional exposure, signal strength, necessary risk metrics, pnl compared to prior day positions
- Recipients:
  - External subscribers (distribution list)
  - Internal allocators
- Implementation notes:
  - Add an SMTP sender utility (e.g., environment variables for SMTP host/user)
  - Generate CSV/HTML summary
  - Attach log snippet for the run window

## Logging (live)
- Mirror backtest logging format and rotation
- Minimal fields: timestamp, symbol, target/actual position, price, P&L deltas
- Store under `logs/live_trend_YYYYMMDD.log`

## Prior-day positions and marking to market
- On start, load prior-day positions from DB
- Fetch current prices; compute marked-to-market P&L and exposures
- Include metrics:
  - Realized/Unrealized P&L
  - Gross/Net exposure
  - Leverage and risk limits checks
- Persist updated positions after the run

## Failure handling
- Non-zero exit triggers alert email with error summary to the systems team and leadership
- Database or data unavailability: retry with backoff, then fail fast
- Ensure idempotence per day (safe re-run)

## Cron integration
- See `docs/performance_upkeep.md` for a daily scheduling example (06:00 UTC)
- Ensure the runtime user has permissions to read `config.json` and write `logs/`

## Monitoring
- Email summary includes: start/end time, status, number of instruments processed, total exposure, P&L snapshot
- In the future, connect these metrics and portfolio performance to Algolens
