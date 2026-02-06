# Configuration Guide

This guide explains how to configure the trade-ngin trading system using the modular configuration system.

---

## Table of Contents

1. [Overview](#overview)
2. [Directory Structure](#directory-structure)
3. [Configuration Files](#configuration-files)
4. [Adding a New Portfolio](#adding-a-new-portfolio)
5. [Configuration Reference](#configuration-reference)
6. [Examples](#examples)
7. [Migration from Legacy Config](#migration-from-legacy-config)

---

## Overview

The configuration system uses a hierarchical structure with:

- **Shared defaults** (`config/defaults.json`) - Settings common to all portfolios
- **Per-portfolio configs** - Portfolio-specific overrides organized by domain:
  - `portfolio.json` - Capital, strategies, allocations
  - `risk.json` - Risk limits and constraints
  - `email.json` - Notification settings

This approach allows you to:
- Change shared settings in one place
- Customize individual portfolios without code changes
- Add new portfolios by creating config files

---

## Directory Structure

```
config/
├── defaults.json                    # Shared defaults (database, execution, optimization)
│
└── portfolios/
    ├── base/                        # BASE_PORTFOLIO
    │   ├── portfolio.json           # Capital, strategies, allocations
    │   ├── risk.json                # Risk limits
    │   └── email.json               # Email recipients
    │
    └── conservative/                # CONSERVATIVE_PORTFOLIO
        ├── portfolio.json
        ├── risk.json
        └── email.json
```

---

## Configuration Files

### defaults.json

Contains settings shared across all portfolios. Edit this file to change:

| Section | Description |
|---------|-------------|
| `database` | Database connection settings |
| `execution` | Commission rates, slippage, position limits |
| `optimization` | Portfolio optimization parameters |
| `backtest` | Backtest-specific settings |
| `live` | Live trading settings |
| `strategy_defaults` | Default strategy parameters |
| `risk_defaults` | Default risk parameters |

**Example:**
```json
{
  "database": {
    "host": "13.58.153.216",
    "port": "5432",
    "username": "postgres",
    "password": "your_password",
    "name": "new_algo_data",
    "num_connections": 5
  },
  "execution": {
    "commission_rate": 0.0005,
    "slippage_bps": 1.0,
    "position_limit_backtest": 1000.0,
    "position_limit_live": 500.0
  },
  "optimization": {
    "tau": 1.0,
    "cost_penalty_scalar": 50.0,
    "max_iterations": 100
  }
}
```

### portfolio.json

Defines portfolio identity, capital, and strategy configuration.

| Field | Type | Description |
|-------|------|-------------|
| `portfolio_id` | string | Unique identifier for the portfolio |
| `initial_capital` | number | Starting capital in dollars |
| `reserve_capital_pct` | number | Percentage held as reserve (0.0-1.0) |
| `strategies` | object | Strategy definitions (see below) |

**Strategy Configuration:**
```json
{
  "strategies": {
    "STRATEGY_NAME": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 0.7,
      "type": "TrendFollowingStrategy",
      "config": {
        "weight": 0.03,
        "risk_target": 0.2,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32]],
        "vol_lookback_short": 32,
        "vol_lookback_long": 252
      }
    }
  }
}
```

### risk.json

Defines risk limits for the portfolio.

| Field | Type | Description |
|-------|------|-------------|
| `var_limit` | number | Value at Risk limit (e.g., 0.15 = 15%) |
| `jump_risk_limit` | number | Jump risk threshold |
| `max_gross_leverage` | number | Maximum gross leverage |
| `max_net_leverage` | number | Maximum net leverage |
| `max_drawdown` | number | Maximum allowed drawdown |
| `max_leverage` | number | Maximum strategy leverage |

**Example:**
```json
{
  "var_limit": 0.15,
  "jump_risk_limit": 0.10,
  "max_gross_leverage": 4.0,
  "max_net_leverage": 2.0,
  "max_drawdown": 0.4,
  "max_leverage": 4.0
}
```

### email.json

Configures email notifications.

| Field | Type | Description |
|-------|------|-------------|
| `smtp_host` | string | SMTP server hostname |
| `smtp_port` | number | SMTP port (typically 587 for TLS) |
| `username` | string | SMTP authentication username |
| `password` | string | SMTP authentication password |
| `from_email` | string | Sender email address |
| `use_tls` | boolean | Enable TLS encryption |
| `to_emails` | array | Recipients for test/dev mode |
| `to_emails_production` | array | Recipients for production |

---

## Adding a New Portfolio

1. **Create the portfolio directory:**
   ```bash
   mkdir -p config/portfolios/my_portfolio
   ```

2. **Create portfolio.json:**
   ```json
   {
     "portfolio_id": "MY_PORTFOLIO",
     "initial_capital": 100000.0,
     "reserve_capital_pct": 0.10,
     "strategies": {
       "TREND_FOLLOWING": {
         "enabled_backtest": true,
         "enabled_live": true,
         "default_allocation": 1.0,
         "type": "TrendFollowingStrategy",
         "config": {
           "weight": 0.03,
           "risk_target": 0.2,
           "idm": 2.5,
           "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64], [32, 128], [64, 256]],
           "vol_lookback_short": 32,
           "vol_lookback_long": 252
         }
       }
     }
   }
   ```

3. **Create risk.json:**
   ```json
   {
     "var_limit": 0.12,
     "jump_risk_limit": 0.08,
     "max_gross_leverage": 3.0,
     "max_net_leverage": 1.5,
     "max_drawdown": 0.35,
     "max_leverage": 3.0
   }
   ```

4. **Create email.json:**
   ```json
   {
     "smtp_host": "smtp.gmail.com",
     "smtp_port": 587,
     "username": "your_email@gmail.com",
     "password": "your_app_password",
     "from_email": "your_email@gmail.com",
     "use_tls": true,
     "to_emails": ["recipient@example.com"]
   }
   ```

5. **Use in application:**
   ```cpp
   auto config_result = ConfigLoader::load("./config", "my_portfolio");
   ```

---

## Configuration Reference

### Execution Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `commission_rate` | 0.0005 | Commission as fraction (5 bps) |
| `slippage_bps` | 1.0 | Slippage in basis points |
| `position_limit_backtest` | 1000.0 | Max position size in backtest |
| `position_limit_live` | 500.0 | Max position size in live trading |

### Optimization Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `tau` | 1.0 | Risk aversion parameter |
| `cost_penalty_scalar` | 50.0 | Multiplier for transaction cost penalty |
| `asymmetric_risk_buffer` | 0.1 | Buffer for asymmetric risk |
| `max_iterations` | 100 | Maximum optimization iterations |
| `convergence_threshold` | 1e-6 | Convergence threshold |
| `use_buffering` | true | Enable position buffering |
| `buffer_size_factor` | 0.05 | Buffer size as fraction of position |

### Risk Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `confidence_level` | 0.99 | Confidence level for VaR (99%) |
| `lookback_period` | 252 | Historical lookback in trading days |
| `max_correlation` | 0.7 | Maximum allowed correlation |
| `var_limit` | 0.15 | VaR limit as fraction of capital |
| `jump_risk_limit` | 0.10 | Jump risk limit |
| `max_gross_leverage` | 4.0 | Maximum gross leverage |
| `max_net_leverage` | 2.0 | Maximum net leverage |

### Strategy Types

| Type | Description |
|------|-------------|
| `TrendFollowingStrategy` | Standard trend following with multiple EMA windows |
| `TrendFollowingFastStrategy` | Faster trend following with shorter windows |
| `TrendFollowingSlowStrategy` | Slower trend following with longer windows |

---

## Examples

### Conservative Portfolio

Lower risk limits, single strategy, higher reserve:

**portfolio.json:**
```json
{
  "portfolio_id": "CONSERVATIVE_PORTFOLIO",
  "initial_capital": 300000.0,
  "reserve_capital_pct": 0.15,
  "strategies": {
    "TREND_FOLLOWING": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 1.0,
      "type": "TrendFollowingStrategy",
      "config": {
        "risk_target": 0.15
      }
    }
  }
}
```

**risk.json:**
```json
{
  "var_limit": 0.10,
  "jump_risk_limit": 0.05,
  "max_gross_leverage": 2.0,
  "max_net_leverage": 1.0,
  "max_drawdown": 0.3,
  "max_leverage": 2.0
}
```

### Aggressive Portfolio

Higher risk limits, multiple strategies:

**portfolio.json:**
```json
{
  "portfolio_id": "AGGRESSIVE_PORTFOLIO",
  "initial_capital": 500000.0,
  "reserve_capital_pct": 0.05,
  "strategies": {
    "TREND_FOLLOWING": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 0.6,
      "type": "TrendFollowingStrategy",
      "config": {
        "risk_target": 0.25
      }
    },
    "TREND_FOLLOWING_FAST": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 0.4,
      "type": "TrendFollowingFastStrategy",
      "config": {
        "risk_target": 0.30
      }
    }
  }
}
```

**risk.json:**
```json
{
  "var_limit": 0.20,
  "jump_risk_limit": 0.15,
  "max_gross_leverage": 5.0,
  "max_net_leverage": 3.0,
  "max_drawdown": 0.5,
  "max_leverage": 5.0
}
```

---

## Migration from Legacy Config

If you're using the old single-file configuration (`config.json`), follow these steps:

1. **Keep legacy file temporarily** - The system supports both formats during migration

2. **Create new config structure:**
   ```bash
   mkdir -p config/portfolios/base
   ```

3. **Split your config:**
   - Database settings → `config/defaults.json`
   - Email settings → `config/portfolios/base/email.json`
   - Strategy definitions → `config/portfolios/base/portfolio.json`
   - Add risk settings → `config/portfolios/base/risk.json`

4. **Update application code:**
   ```cpp
   // Old way (legacy)
   auto credentials = std::make_shared<CredentialStore>("./config.json");

   // New way
   auto config_result = ConfigLoader::load("./config", "base");
   if (config_result.is_error()) {
       // Handle error
   }
   auto config = config_result.value();
   ```

5. **Test thoroughly** before removing legacy config files

---

## Troubleshooting

### Config file not found
- Verify the config directory path is correct
- Check file permissions
- Ensure all required files exist (portfolio.json, risk.json, email.json)

### JSON parse error
- Validate JSON syntax using a JSON linter
- Check for trailing commas (not allowed in JSON)
- Ensure all strings are double-quoted

### Values not applied
- Check that the field name matches exactly (case-sensitive)
- Verify the value type (number vs string)
- Portfolio-specific values override defaults

---

## Best Practices

1. **Version control** - Keep config files in version control
2. **Sensitive data** - Consider using environment variables for passwords
3. **Documentation** - Add `_description` fields to document purpose
4. **Testing** - Test config changes in backtest before live trading
5. **Backup** - Keep backups before major config changes