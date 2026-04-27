# email_sender.cpp — Refactor Plan for Unit Test Compatibility

**Status:** deferred from Phase 3 — planning doc only
**Current coverage:** 0.1% line, 0.0% branch (2226 lines, 0 effective covered)
**Target after refactor:** 85% line, 55% branch (~1,900 lines covered)
**Effort estimate:** 2–3 hours of tests + minimal source change (1 line or zero)

## Why it can't be unit-tested today

`EmailSender::send_email()` (around `src/core/email_sender.cpp:137`) opens a libcurl SMTP handle, connects to the configured SMTP host, and sends the email. The curl IO can't run in a unit-test environment without either a fake SMTP server or curl injection.

**However**, the bulk of the file (~1900 of 2226 lines) is **21 private `format_*` methods** declared in `include/trade_ngin/core/email_sender.hpp` lines 162–304. These methods take data structs (positions, executions, risk metrics, strategy metrics) and return formatted HTML/text strings. They have no IO dependencies. They're untestable today only because they're private and there's no public dry-run entry point.

## Proposed refactor

Pick one of three access strategies for the `format_*` helpers:

### Option 1 — `#define private public` in tests (zero source change, matches existing pattern)

Already used in `tests/strategy/test_trend_following.cpp` and `tests/instruments/test_instrument_registry.cpp`. Pros: no source change. Cons: brittle if the class layout changes.

```cpp
// in tests/core/test_email_sender.cpp
#define private public
#include "trade_ngin/core/email_sender.hpp"
#undef private
```

### Option 2 — Single-line visibility change (recommended)

Move the 21 `format_*` declarations from `private:` to `protected:` in `include/trade_ngin/core/email_sender.hpp`. Add a tiny test subclass `class EmailSenderTestPeer : public EmailSender { using EmailSender::format_*; };` to expose them.

**Source change:** 1 line (`private:` → `protected:` block label move). No behavior change.

### Option 3 — Friend declaration

Add `friend class trade_ngin::testing::EmailSenderTestPeer;` to `EmailSender` class. Same end result, slightly more invasive (1 line + needs a real peer class declaration).

**Recommendation:** Option 2. Smallest surface, no macro tricks, signals intent ("subclasses for testing may extend").

### What about `send_email()`?

Two ways to test most of `send_email()`:

1. **Dry-run entry point** (small additive source change): Add `Result<std::string> render_email(...)` that runs the same body-assembly logic as `send_email()` but returns the assembled MIME message instead of POSTing it via curl. Then `send_email()` becomes `auto rendered = render_email(...); if (rendered.is_error()) return ...; return curl_send(rendered.value());`. Tests call `render_email`. ~20-line source change.

2. **Curl injection** (cleaner but larger): Extract `ICurlSmtpClient` interface, inject. Tests use a mock that asserts on the assembled email. ~80-line refactor.

Recommendation: option 1 (dry-run) for ROI. Curl IO loop ~50 lines stays untested.

## Source change line counts

| File | Lines changed |
|---|---|
| `include/trade_ngin/core/email_sender.hpp` | 1 (private→protected) + ~3 (declare `render_email`) |
| `src/core/email_sender.cpp` | ~30 (extract body assembly into `render_email`, call from `send_email`) |
| **Total** | ~35 lines source change |

## What gets unit-tested

| Surface | Inputs / setup | Coverage gain |
|---|---|---|
| `format_positions_table` | synthetic `unordered_map<string, Position>` | ~80 lines |
| `format_risk_metrics` | synthetic RiskResult | ~30 lines |
| `format_strategy_metrics` | strategy → metric map | ~40 lines |
| `format_executions_table` | vector<ExecutionReport> | ~70 lines |
| `format_symbols_table_for_positions` | positions + MockPostgresDatabase + date | ~60 lines |
| `format_yesterday_finalized_positions_table` (2 overloads) | positions + db | ~120 lines |
| `format_rollover_warning` | rollover data | ~50 lines |
| `format_strategy_positions_tables` + helpers | multi-strategy positions | ~150 lines |
| `format_strategy_display_name` | strategy_id strings | ~30 lines |
| `format_strategy_executions_tables` + helpers | per-strategy executions | ~120 lines |
| Other `format_*` (~10 more) | various | ~400 lines |
| `generate_trading_report_body` orchestrator | all of the above | ~250 lines |
| `generate_trading_report_from_db` orchestrator | with mock db | ~150 lines |
| `render_email` (proposed) | any rendered email | ~80 lines |
| `send_email`'s pre-curl branches (config validation) | invalid configs | ~40 lines |
| Curl IO inside `send_email` | NOT covered | (untested ~50 lines) |

## Test file plan

- `tests/core/test_email_sender.cpp` — ~700 LOC covering all format_* + orchestrators

## Things to verify in tests

For format_* methods generally:
- Empty input collections → produce valid empty-table HTML (or expected fallback)
- Single-row input → exactly one row in output
- Multi-row → correct ordering and counts
- HTML special chars (e.g. `&`, `<`, `>`) in symbol names → properly escaped
- Numeric formatting (% vs raw, decimal places, thousands separator)
- Sign formatting for PnL (gain green / loss red CSS classes if used)
- Missing optional fields → fallback rendering, not crash

For `generate_trading_report_body`:
- All sections present when all data provided
- Sections gracefully omitted when corresponding data is empty
- Date stamp matches the supplied `Timestamp`

## Out of scope

- Actual SMTP delivery (the curl loop)
- Email rendering across mail clients (Gmail/Outlook visual diffs)
- Credential management / `CredentialStore` integration (covered in its own test file)
