# Equity Paper Trading Deployment Guide

This guide explains how to activate equity paper trading when you're ready.

## Current Status
✅ All equity infrastructure is built and ready
✅ `live_equity_mean_reversion` executable is functional
✅ Database schema supports equities
✅ PnL calculations work correctly
⚠️ **Cron job is NOT active** (futures trading continues unaffected)

## What's Ready

### Files Created (Ready to Deploy)
1. ✅ `live_equity_mr.cron` - Cron job definition (runs 4:30 PM ET, Mon-Fri)
2. ✅ `scripts/run_live_equity_mr.sh` - Daily execution script
3. ✅ `run_live_equity_mr_multi.sh` - Multi-date backfill script
4. ✅ `build/bin/Release/live_equity_mean_reversion.exe` - Main executable

### Current Configuration
- **Strategy**: Mean Reversion
- **Symbols**: AAPL, MSFT, GOOGL, AMZN, TSLA
- **Schedule**: 4:30 PM ET, Monday-Friday (after market close)
- **Capital**: $100,000
- **Lookback**: 20 days
- **Entry Threshold**: 2.0 std devs
- **Exit Threshold**: 0.5 std devs

---

## Activation Steps (When Ready)

### Step 1: Test Manually First
Before activating the cron job, test the system manually:

```bash
# Test for a recent historical date
./build/bin/Release/live_equity_mean_reversion 2026-01-10 --send-email

# Check the results in database
psql -d trading -c "SELECT * FROM trading.live_results WHERE strategy_id='LIVE_EQUITY_MEAN_REVERSION' ORDER BY date DESC LIMIT 5;"
```

### Step 2: Backfill Historical Data (Optional)
If you want to populate historical paper trading results:

```bash
# Make script executable
chmod +x run_live_equity_mr_multi.sh

# Run for last 5 trading days
./run_live_equity_mr_multi.sh --last-n-days 5

# Or run for specific date range
./run_live_equity_mr_multi.sh --from 2026-01-06 --to 2026-01-10
```

### Step 3: Activate Cron Job in Docker

**Edit `Dockerfile` (around line 144)**

Find this section:
```dockerfile
COPY live_trend.cron /etc/cron.d/live_trend
RUN chmod 0644 /etc/cron.d/live_trend && \
    crontab /etc/cron.d/live_trend && \
    touch /var/log/cron.log
```

**Replace with:**
```dockerfile
# Copy cron files for both futures and equities
COPY live_trend.cron /etc/cron.d/live_trend
COPY live_equity_mr.cron /etc/cron.d/live_equity_mr

# Install both cron jobs and create log files
RUN chmod 0644 /etc/cron.d/live_trend && \
    chmod 0644 /etc/cron.d/live_equity_mr && \
    crontab /etc/cron.d/live_trend && \
    crontab -l | cat - /etc/cron.d/live_equity_mr | crontab - && \
    touch /var/log/cron.log && \
    touch /var/log/cron_equity.log
```

### Step 4: Rebuild and Deploy

```bash
# Rebuild Docker image
docker build -t trading-system .

# Stop old container
docker stop trading-container

# Start new container with both strategies
docker run -d \
  --name trading-container \
  -v $(pwd)/config.json:/app/config.json \
  -v $(pwd)/logs:/app/logs \
  --network trading-network \
  trading-system
```

### Step 5: Verify Activation

```bash
# Check that both cron jobs are installed
docker exec trading-container crontab -l

# You should see:
# 30 9 * * * sh /app/scripts/run_live_trend.sh >> /var/log/cron.log 2>&1
# 30 16 * * 1-5 sh /app/scripts/run_live_equity_mr.sh >> /var/log/cron_equity.log 2>&1

# Monitor equity cron log
docker exec trading-container tail -f /var/log/cron_equity.log
```

---

## Testing Without Cron (Recommended First)

Before activating the cron job, test manually:

### Local Testing (Windows)
```bash
# Test with a specific date
./build/bin/Release/live_equity_mean_reversion.exe 2026-01-10 --send-email
```

### Docker Testing (Without Cron)
```bash
# Build image (without activating cron)
docker build -t trading-system .

# Run equity strategy manually inside container
docker run --rm \
  -v $(pwd)/config.json:/app/config.json \
  trading-system \
  /app/build/bin/Release/live_equity_mean_reversion 2026-01-10 --send-email
```

---

## Schedule Comparison

| Strategy | Executable | Schedule | Days | Rationale |
|----------|-----------|----------|------|-----------|
| **Futures Trend** | `live_trend` | 9:30 AM ET | Every day | Before futures trading starts |
| **Equity Mean Rev** | `live_equity_mean_reversion` | 4:30 PM ET | Mon-Fri only | After market close, data available |

**Both run independently** - No conflicts or dependencies!

---

## Monitoring After Activation

### Check Equity Results Daily
```bash
# Today's results
psql -d trading -c "
SELECT
  date,
  daily_pnl,
  total_pnl,
  portfolio_leverage,
  active_positions
FROM trading.live_results
WHERE strategy_id='LIVE_EQUITY_MEAN_REVERSION'
  AND DATE(date) = CURRENT_DATE;
"
```

### Check Logs
```bash
# Equity cron log
docker exec trading-container tail -50 /var/log/cron_equity.log

# Equity strategy log
docker exec trading-container tail -50 /app/logs/live_equity_mr_*.log
```

### Check Email Reports
- You should receive daily email at ~4:35 PM ET (Mon-Fri)
- Subject: "Equity Mean Reversion - Daily Report"

---

## Rollback (Deactivate Equities)

If you need to disable equity trading:

### Option 1: Disable Cron Job (Keep Infrastructure)
```bash
# SSH into container
docker exec -it trading-container bash

# Remove equity cron job
crontab -l | grep -v "run_live_equity_mr" | crontab -

# Verify
crontab -l  # Should only show live_trend
```

### Option 2: Full Rollback (Rebuild Without Equities)
```bash
# Restore original Dockerfile (remove equity cron lines)
# Rebuild
docker build -t trading-system .
docker stop trading-container
docker run -d --name trading-container trading-system
```

---

## Data Requirements Checklist

Before activating, ensure:
- [ ] `market_data.us_stocks` has daily data for all symbols
- [ ] Data is available by 4:00 PM ET daily
- [ ] Database has at least 30 days of history (for 20-day lookback + buffer)
- [ ] No missing dates in data (strategy handles gaps, but verify quality)
- [ ] Email SMTP credentials in `config.json` are correct
- [ ] Tested manually for 3+ different dates successfully

---

## Customization (Before Activation)

### Change Symbols
**Edit:** `apps/strategies/live_equity_mean_reversion.cpp` (line ~200)
```cpp
// Current:
auto symbols_result = db->get_symbols(AssetClass::EQUITIES);
if (symbols_result.is_ok()) {
    symbols = symbols_result.value();
}

// Change to specific symbols:
symbols = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA", "META"};
```
Then rebuild: `cmake --build build --target live_equity_mean_reversion --config Release`

### Change Schedule
**Edit:** `live_equity_mr.cron`
```cron
# Current: 4:30 PM ET
30 16 * * 1-5 sh /app/scripts/run_live_equity_mr.sh >> /var/log/cron_equity.log 2>&1

# Change to 5:00 PM ET
00 17 * * 1-5 sh /app/scripts/run_live_equity_mr.sh >> /var/log/cron_equity.log 2>&1
```

### Change Strategy Parameters
**Edit:** `apps/strategies/live_equity_mean_reversion.cpp` (line ~350)
```cpp
MeanReversionConfig mean_rev_config;
mean_rev_config.lookback_period = 20;      // Change lookback
mean_rev_config.entry_threshold = 2.0;     // Change entry z-score
mean_rev_config.exit_threshold = 0.5;      // Change exit z-score
mean_rev_config.risk_target = 0.15;        // Change risk target
mean_rev_config.position_size = 0.1;       // Change position size
```
Then rebuild.

---

## Support & Troubleshooting

### Common Issues

**Issue: No results in database**
- Check logs: `/var/log/cron_equity.log`
- Verify data exists: `SELECT COUNT(*) FROM market_data.us_stocks WHERE date = CURRENT_DATE;`
- Check executable permissions: `ls -la /app/build/bin/Release/live_equity_mean_reversion`

**Issue: Email not sent**
- Verify SMTP credentials in `config.json`
- Check email logs in strategy log file
- Test email manually: Run with `--send-email` flag

**Issue: Cron not running**
- Check cron is running: `docker exec trading-container ps aux | grep cron`
- Verify crontab: `docker exec trading-container crontab -l`
- Check timezone: `docker exec trading-container date` (should be ET)

---

## Summary

**Current State:**
- ✅ Everything built and tested
- ✅ Futures trading unaffected
- ⚠️ Equity cron job NOT active

**To Activate:**
1. Test manually (5 minutes)
2. Edit Dockerfile - add 3 lines (1 minute)
3. Rebuild Docker image (5 minutes)
4. Deploy and monitor (ongoing)

**Total activation time: ~15 minutes**

**Deactivation:** Just remove cron line and rebuild (or disable cron job dynamically)

---

**You're all set!** When you're ready to go live with equity paper trading, follow Steps 1-5 above.
