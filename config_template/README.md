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
