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

`examples/risk_modules/` holds one worked example per module type. They carry placeholder
numbers and no runner loads them.
