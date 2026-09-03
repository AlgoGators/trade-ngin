# F-8 — the adjusted-frame ↔ broker-basis rule

**Status:** written 2026-09-03 (Phase B session B-4, from the E4 audit's draft).
Branch `equities_integration`. Scope: **equities only** — futures carry no splits and no
dividends, and nothing in this document touches a futures write path.

This document exists because `trading.positions.average_price` for an equity is **not** the
number a broker statement prints. The two disagree by construction, by an amount that is
knowable exactly, and the whole point of writing the rule down is that a future reconciliation
knows which differences are *expected* and which are defects. Without it, the first person to
put a statement next to the book will either "fix" a correct number or accept a wrong one.

Read `AVERAGE_PRICE_LIFECYCLE.md` first: it says what `average_price` means at each step of a
live day. This document says how the meaning it settles on (COST BASIS, in the **adjusted**
frame) relates to the outside world.

---

## 1. The two frames

### Book (adjusted) frame — what this engine stores

`trading.positions.average_price` is restated on **every class-1 ex-date the book held
through**, by `CorporateActionsApplier::apply` (`src/live/corporate_actions_applier.cpp`):

| event | quantity | basis |
|---|---|---|
| split, factor `F` | `q ×= F` | `B /= F` |
| ADR ratio split, factor `F` | `q ×= F` | `B /= F` |
| dividend `d`, raw ex-date close `c` | unchanged | `B /= (1 + d/c)` |

`c` is the **RAW** close **on the ex-date**, not an adjusted one and not the prior close
(`corporate_actions_applier.hpp` field comment; E2-F3; the denominator fix `22d1feb8`). It has
to be the raw ex-date close because the applier's per-event rescale must equal
`compute_backward_adjustment_factors`' per-event step, and that works in raw closes; an
adjusted close carries every LATER event in the window, so under stacked events the two frames
diverge.

Marks are the back-adjusted close **anchored at the newest loaded bar** (F-7), so today's mark
is raw and the whole series behind it is expressed in today's units.

**Consequence:** a dividend is inside `total_pnl` *via the basis*, not as cash.
`trading.live_results.total_dividend_income` is **informational only** and must never be added
to a P&L total (`CORP_ACTIONS_DATA_BOUNDARY.md`, and two tests pin it from both directions).

### Broker frame — what a statement prints

- Quantity is restated by **splits only** — identical to the book.
- Cost basis is **never** touched by a dividend.
- Dividend cash is credited on the **pay** date, not the ex-date.
- A spinoff **delivers child shares** plus cash-in-lieu for the fractional remainder.

---

## 2. The identities, per open position, at any date T

### Identity 1 — quantity

```
q_book == q_broker            exactly
```

Exceptions, and only these:

| exception | shape | tolerance |
|---|---|---|
| **Spinoffs the terms feed can see** (E2-F31, landed `be604beb`) | parent quantity is now correct and the child is received; under the default `spinoff_child_policy = liquidate_at_first_close` the child is sold at its first close, so the book holds none of it while a broker statement shows the receipt and the sale — reconcile against the two `CORPACTION_<child>_<ex_date>` executions, not against the position | quantity exact on the parent; the child appears only in `trading.executions` |
| **Spinoffs the terms feed cannot see** — after 2025-08-29, or a child with no price series | the event is refused whole and logged as `SPINOFF NOT APPLIED`: parent quantity is correct, but its basis is PRE-spinoff against a POST-spinoff price series and the child is absent | none — this is a data gap, not a tolerance; the run says so every time |
| **Split fractions** | the book keeps a fractional share; the broker pays cash in lieu | `\|Δq\| < 1` share, AND a CIL execution must exist for `Δq × mark` |

A quantity difference outside those two is a defect. Do not tolerate it.

### Identity 2 — basis

```
B_broker == B_book × Π_i (1 + d_i / c_i)
```

over the dividends applied since the position's inception. Splits **cancel** — both frames
divide by `F` — so only dividends appear in the product. Exact in real arithmetic; **tolerance
1e-6 relative** for the float chain.

Each factor `(1 + d_i / c_i)` is exactly `PositionAdjustment.ratio_change`, which is what the
`basis_ratio` column on `trading.corp_action_applied` stores (§4). Before that column existed
the chain could only be recomputed by joining back to `ohlcv_1d` for the raw ex-date closes —
i.e. the ledger could not be inverted from the ledger.

### Identity 3 — P&L is NOT reconciled dollar-for-dollar

```
U_book            = q (M − B_book)
U_broker + D      = q (M − B_broker) + Σ q·d_i
```

These are **not** equal, and expecting them to be is the mistake this section exists to
prevent. For a single dividend the gap is exactly

```
U_book − (U_broker + D) = q · d · (B_broker − (c + d)) / (c + d)
```

— the dividend times the *relative distance* between the raw basis and the cum-dividend close.
It is **zero only when the position was bought at the cum-dividend close**. In general, over
`n` applied dividends:

```
gap = q · B_broker · (1 − 1 / Π_i r_i) − q · Σ_i d_i,      r_i = 1 + d_i / c_i
```

**The rule:**

1. Reconcile **quantity** (identity 1) exactly.
2. Reconcile **basis** (identity 2) exactly, to 1e-6 relative.
3. Reconcile **dividend cash per event**: `corp_action_applied.total_cash` against the broker's
   dividend ledger, tolerance `$0.01 × q`, allowing for ex→pay date timing (the broker credits
   on the pay date; the book books nothing on either date).
4. Report the **expected P&L gap** by the formula above as an **informational line**, tolerance
   formula ± $0.01. Never treat a matching gap as a break, and never treat a *mismatching* gap
   as rounding.

---

## 3. Worked example

100 shares of a $100 stock, bought at the cum-dividend close of 100.63. It pays `d = 0.63`
with a raw ex-date close of `c = 100.00`. Mark today `M = 105`.

```
r        = 1 + 0.63/100.00       = 1.0063
B_broker = 100.63                            (broker never moves it)
B_book   = 100.63 / 1.0063       = 100.00    (what trading.positions holds)

U_book        = 100 × (105 − 100.00)         = 500.00
U_broker + D  = 100 × (105 − 100.63) + 63.00 = 500.00
gap           = 100 × 0.63 × (100.63 − 100.63)/100.63 = 0.00
```

Zero, because the buy was at the cum close. Move the buy to 90.00 instead:

```
B_book = 90.00 / 1.0063 = 89.4366
U_book       = 100 × (105 − 89.4366)         = 1556.34
U_broker + D = 100 × (105 − 90.00) + 63.00   = 1563.00
gap          = 100 × 0.63 × (90.00 − 100.63)/100.63 = −6.66      ✓ 1556.34 − 1563.00
```

$6.66 on a $9,000 position is small; it is also **permanent and cumulative**, and it is the
number an operator will otherwise spend an afternoon on.

---

## 4. Where this is asserted

| site | what it checks | status |
|---|---|---|
| `src/live/broker_frame.{hpp,cpp}` | pure `raw_basis(adj_basis, applied_dividends)` and `expected_pnl_gap(...)` — the two functions above, with no DB and no I/O | LANDED `553ee9d1` |
| `tests/live/corp_actions/test_broker_frame.cpp` | `RawBasisRecoveredFromAdjustedBasisAndDividendChain`, `PnLGapEqualsDividendTimesBasisDistance` | LANDED `553ee9d1` |
| `tests/live/corp_actions/test_broker_frame_db.cpp` | `AppliedRowCarriesTheRatioNeededToInvertIt` — the `basis_ratio` column exists, is written, and inverts the stored basis | LANDED `553ee9d1` |
| `trading.corp_action_applied.basis_ratio` (migration 006) | the per-event factor, so identity 2 is computable from the ledger alone | LANDED `553ee9d1`, applied to `new_algo_data` 2026-09-03 |
| the live equity runner, after the class-1 apply | one INFO line per adjusted symbol: `F-8 basis frames \| SYM adjusted=X raw-equivalent=Y (n dividend event(s) …)`, and a WARN naming the symbol when a pre-006 row makes the chain uninvertible | LANDED `553ee9d1` |

**Until a broker adapter exists this is a human-run statement compare.** `src/broker` does not
exist; the "Broker reconciliation / partial fills" row deferred in the completion plan's §3 is
the *automation* of this rule, not the rule itself. Do not conflate the two: F-8 is closed by
the rule + the pure functions + the column; the deferred row stays deferred.

### The query

Per symbol, over one portfolio/strategy/name, this recovers the broker-frame basis from the
ledger with no join to `ohlcv_1d`:

```sql
SELECT p.symbol,
       p.average_price                                   AS basis_book,
       p.average_price * exp(sum(ln(a.basis_ratio)))     AS basis_broker,
       count(*)                                          AS dividends_applied
  FROM trading.positions p
  JOIN trading.corp_action_applied a
    ON a.portfolio_id = p.portfolio_id
   AND a.strategy_id  = p.strategy_id
   AND a.symbol       = p.symbol
   AND a.action_type  = 'DIVIDEND'
   AND a.basis_ratio IS NOT NULL
 WHERE p.date = $1 AND p.portfolio_id = $2 AND p.strategy_id = $3
 GROUP BY p.symbol, p.average_price;
```

`exp(sum(ln(...)))` rather than a product aggregate because PostgreSQL has no `product()`;
every `basis_ratio` is `> 0` by construction (`1 + d/c` with `c > 0`, or `F > 0`), so the log
is always defined. **Rows with `basis_ratio IS NULL` are rows written before migration 006** —
they are not zero-effect, they are *unknown*, and a chain containing one cannot be inverted
from the ledger. Say so in the reconciliation rather than treating NULL as 1.0.

### The verification run

Replay a window containing an applied dividend already in the validated set — **GOOGL 06-08**
or **META 06-15**. Then `avg_price × Π basis_ratio` from the dedup rows must equal the fill
price in `trading.executions` to 1e-6 relative, and the runner's log line must show both
numbers.

---

## 5. What this rule does NOT say

- It does not say the book is wrong. The adjusted frame is the correct frame for a
  total-return series: it is what keeps returns continuous across an ex-date, and it is what
  makes the backtest and the live book comparable.
- It does not license restating the broker's numbers into the book's frame in reporting. Report
  what each frame says and the expected gap between them.
- It says nothing about **realized** P&L on a closed position. A closed row carries no basis
  (`AVERAGE_PRICE_LIFECYCLE.md` rule 5, `average_price = 0`), so identity 2 has no subject
  there. The realized figure on the exit was struck against the *adjusted* basis, and it
  therefore carries the same gap, permanently. That is the strongest argument for storing
  `basis_ratio`: after the row closes, the ledger is the only remaining record of the chain.

---

## 6. Implementation status (update this table as the pieces land)

| piece | tier | status |
|---|---|---|
| the rule (this document) | Tier 1 | **DONE** 2026-09-03, commit `ad0532d5` |
| `src/live/broker_frame.{hpp,cpp}` | Tier 2 | **DONE** `553ee9d1` |
| the three tests | Tier 2 | **DONE** `553ee9d1` (2 pure + 1 DB-gated) |
| `basis_ratio` column, migration 006 | Tier 2 | **DONE** `553ee9d1`; migration applied to `new_algo_data` 2026-09-03 |
| runner log line | Tier 2 | **DONE** `553ee9d1` |
| GOOGL 06-08 / META 06-15 verification replay | Tier 2 | **NOT RUN** — see the note below |
| broker adapter / automated compare | deferred (§3 of the completion plan) | NOT STARTED, deliberately |

**The GOOGL/META verification replay was not run, and cannot yet prove what it was meant to.**
Every `trading.corp_action_applied` row that exists today predates migration 006, so
`basis_ratio` is NULL on all of them and `raw_basis` correctly answers UNKNOWN. The column only
starts carrying values on dividends applied by a run built after `553ee9d1`. The replay
therefore belongs to the Phase D end gate, which rebuilds the book from the chain start with
the new binary: at that point `avg_price × Π basis_ratio` can be compared against the fill price
in `trading.executions` to 1e-6 relative, exactly as the rule specifies. The mechanism itself is
proven by `AppliedRowCarriesTheRatioNeededToInvertIt`, which round-trips a real row through the
real table and inverts a basis with it.

**Migration number:** the E4 audit and `PHASE_B_E3_E4_FIXES.md` both say "migration 005" for
`basis_ratio`. 005 was taken on 2026-09-03 by `corp_action_applied.run_date` (E2-F23 / F23-C′,
commit `4e9cfabe`), so `basis_ratio` is **006**. The two are independent columns on the same
table and neither depends on the other.
