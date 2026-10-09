# Config Template

Copy this directory to `config/` and replace the placeholders with your actual values.

## Setup

```bash
cp -r config_template config
```

## Placeholders to Replace

### defaults.json
- `YOUR_DB_HOST` - PostgreSQL server host
- `YOUR_DB_USERNAME` - Database username
- `YOUR_DB_PASSWORD` - Database password
- `YOUR_DB_NAME` - Database name

### portfolios/base/email.json & portfolios/conservative/email.json
- `YOUR_SMTP_USERNAME` - SMTP auth username (e.g. Gmail address)
- `YOUR_SMTP_APP_PASSWORD` - SMTP app password (use app-specific password for Gmail)
- `YOUR_FROM_EMAIL` - Sender email address
- `YOUR_EMAIL_FOR_NOTIFICATIONS` - Recipient(s) for testing (in `to_emails` array)
- `to_emails_production` - Replace placeholder array with actual production recipient emails

## Structure

```
config/
├── defaults.json              # Database, execution, optimization (shared)
└── portfolios/
    ├── base/                  # BASE_PORTFOLIO (bt_portfolio, live_portfolio)
    │   ├── portfolio.json     # Strategies, capital, allocations
    │   ├── risk.json          # Risk limits
    │   └── email.json         # Email notifications
    └── conservative/          # CONSERVATIVE_PORTFOLIO (bt_portfolio_conservative, live_portfolio_conservative)
        ├── portfolio.json
        ├── risk.json
        └── email.json
```

## Risk schema 2 (T-6 commit 7)

`risk.json` now names the risk MODULES a book runs instead of carrying a flat set of
gating values, and there are no defaults left anywhere: every gating value is written
literally in the book's own `risk.json`, `use_optimization` lives at the top level of
`portfolio.json`, and `use_risk_management` and `risk_defaults` are gone. A book that runs
no risk layer says so with a `none` module carrying `_reason`, `_ruled_by` and `_ruled_on`.

**The loader REFUSES a schema-1 `risk.json`**, so a deployed config directory has to be
migrated once, before the first run of a build containing commit 7:

```
python3 scripts/migrate_risk_json.py config            # dry run -- READ THE DIFF
python3 scripts/migrate_risk_json.py config --in-place # write it
```

The `.schema1.bak` files the second command leaves behind are the rollback for going back
to an older build. The script refuses, rather than guessing, when a book's `risk.json` has
no `max_drawdown`/`max_leverage` (schema 1 silently fell back to 0.4 / 4.0) or when
`use_risk_management` resolves to false anywhere: that second case needs a `none` module
with a ruling, and the script will not invent one.

**Two hand steps after the script (T-6b-fix).**

1. *`lookback_unit`.* A `risk.json` migrated before T-6b commit 9 says `"lookback_unit":
   "bars"`. The Carver window keeps `lookback_period` distinct DATES (252 distinct dates, of
   which the sparse-date filter keeps about 187-200 on the futures books) and the loader now
   refuses `"bars"`. Re-running the script upgrades such a file to `"dates"` and nothing else,
   keeping a `.bars.bak` rollback copy.
2. *A book ruled `none` (today: EQUITY_MR).* The script writes a `carver` module for every
   book, because that is the only gate schema 1 had, and it will not invent a ruling. When
   the tracked `config_template/portfolios/<book>/risk.json` assigns the book `none`, the
   script prints a `NOTE:` naming the book, `_ruled_by` and `_ruled_on`. For EQUITY_MR the
   operator then replaces the deployed file's `modules` list by hand with the template's:

   ```
   "modules": [ { "id": "no_portfolio_risk", "type": "none",
                  "_reason": "<copied verbatim from the template>",
                  "_ruled_by": "HD", "_ruled_on": "2026-09-18" } ]
   ```

   leaving `risk_reporting`, `max_drawdown` and `max_leverage` as they are (the reporter still
   measures the book). Until that is done the deployed book keeps gating with the Carver
   module while the repository says it runs none. Each run of a `none` book logs one
   `RISK_NONE ... ruled_by=... ruled_on=...` line, which is how to confirm the step took.

`examples/risk_modules/` holds one worked example per module type. They carry placeholder
numbers and no runner loads them.

## `reserve_capital_pct` deleted (J3, T-7a commit 2a)

`reserve_capital_pct` (OPT-N4) is gone from every `portfolio.json` template. Nothing sized on
it: the last reader was removed in February 2026, and the figure was only echoed into one log
line and into the run-metadata JSON. A deployed `portfolio.json` that still carries the key
keeps loading; the key is ignored and each run logs one WARN naming it (`"reserve_capital_pct"
was deleted by J3 and is no longer read`). Delete the line to silence it.

Cut-over in stored rows: `trading.live_run_metadata.portfolio_config` and
`backtest.run_metadata.portfolio_config` / `.hyperparameters` written before this commit carry
`reserve_capital`; rows written after it do not. History is left as it was written.
