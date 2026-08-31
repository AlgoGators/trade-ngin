-- ADR-005 §7: the parity gate that flips benchmark.mode from "live" to
-- "deferred". Compares --engine frozen's output (the 'benchmark_frozen_shadow'
-- stream, migration 007) against the existing benchmark_mode="live" pass's
-- output (the 'benchmark' stream) for the same portfolio and date range.
--
-- Pass criteria, per day (ADR-005 §7, verbatim):
--   - position sets identical (same symbols, quantity equal to the Decimal,
--     not within tolerance)
--   - equity (mark-to-market, D-4) equal to the cent
-- 20 consecutive passing trading days flips the default to "deferred". A
-- single failure is a bug in one of the two modes and blocks the flip --
-- the failing day's trading.run_inputs row is the reproduction case.
--
-- Usage (produce the shadow stream first, then compare):
--   benchmark_replay --portfolio base --engine frozen --through 2026-09-20 \
--       --target-stream benchmark_frozen_shadow
--   psql -f scripts/parity_gate.sql \
--       -v portfolio_id="'BASE_PORTFOLIO'" \
--       -v from_date="'2026-08-01'" \
--       -v through_date="'2026-09-20'"
--
-- This is intentionally plain SQL, not a C++ tool (ADR-005 §5.3: "this is a
-- data comparison, not new application code").

\if :{?portfolio_id}
\else
\echo 'Usage: psql -f parity_gate.sql -v portfolio_id="''BASE_PORTFOLIO''" -v from_date="''2026-08-01''" -v through_date="''2026-09-20''"'
\quit
\endif

WITH frozen_positions AS (
    SELECT date, strategy_name, symbol, quantity
    FROM trading.positions
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark_frozen_shadow'
      AND date BETWEEN :from_date AND :through_date
),
live_positions AS (
    SELECT date, strategy_name, symbol, quantity
    FROM trading.positions
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark'
      AND date BETWEEN :from_date AND :through_date
),
position_mismatches AS (
    SELECT COALESCE(f.date, l.date) AS date,
           COALESCE(f.strategy_name, l.strategy_name) AS strategy_name,
           COALESCE(f.symbol, l.symbol) AS symbol,
           f.quantity AS frozen_quantity,
           l.quantity AS live_quantity
    FROM frozen_positions f
    FULL OUTER JOIN live_positions l
      ON f.date = l.date AND f.strategy_name = l.strategy_name AND f.symbol = l.symbol
    -- Exact match required (ADR-005 7: "not within tolerance"). IS DISTINCT
    -- FROM treats two NULLs (a position present in only one side) as
    -- distinct too, which is what we want -- a symbol missing from either
    -- side is itself a mismatch.
    WHERE f.quantity IS DISTINCT FROM l.quantity
),
frozen_equity AS (
    SELECT timestamp::date AS date, equity
    FROM trading.equity_curve
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark_frozen_shadow'
      AND timestamp::date BETWEEN :from_date AND :through_date
),
live_equity AS (
    SELECT timestamp::date AS date, equity
    FROM trading.equity_curve
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark'
      AND timestamp::date BETWEEN :from_date AND :through_date
),
equity_mismatches AS (
    SELECT COALESCE(f.date, l.date) AS date, f.equity AS frozen_equity, l.equity AS live_equity
    FROM frozen_equity f
    FULL OUTER JOIN live_equity l ON f.date = l.date
    -- Cent-for-cent (ADR-005 7): compare rounded to 2 decimal places, not
    -- raw equality, so float/numeric representation noise below a cent
    -- isn't a false failure.
    WHERE ROUND(COALESCE(f.equity, 0)::numeric, 2) IS DISTINCT FROM
          ROUND(COALESCE(l.equity, 0)::numeric, 2)
       OR (f.equity IS NULL) IS DISTINCT FROM (l.equity IS NULL)
),
days_in_range AS (
    SELECT DISTINCT date FROM (
        SELECT date FROM frozen_positions
        UNION SELECT date FROM live_positions
        UNION SELECT date FROM frozen_equity
        UNION SELECT date FROM live_equity
    ) all_dates
),
day_verdicts AS (
    SELECT d.date,
           NOT EXISTS (SELECT 1 FROM position_mismatches pm WHERE pm.date = d.date)
               AS positions_match,
           NOT EXISTS (SELECT 1 FROM equity_mismatches em WHERE em.date = d.date)
               AS equity_matches,
           (SELECT count(*) FROM position_mismatches pm WHERE pm.date = d.date)
               AS position_mismatch_count
    FROM days_in_range d
)
SELECT date,
       (positions_match AND equity_matches) AS pass,
       positions_match,
       equity_matches,
       position_mismatch_count
FROM day_verdicts
ORDER BY date;

-- Current streak of consecutive PASS days, counting backward from the most
-- recent evaluated day. A single FAIL resets it to zero as of that day --
-- this is the number ADR-005 7 compares against 20 to decide the flip.
WITH frozen_positions AS (
    SELECT date, strategy_name, symbol, quantity
    FROM trading.positions
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark_frozen_shadow'
      AND date BETWEEN :from_date AND :through_date
),
live_positions AS (
    SELECT date, strategy_name, symbol, quantity
    FROM trading.positions
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark'
      AND date BETWEEN :from_date AND :through_date
),
position_mismatches AS (
    SELECT COALESCE(f.date, l.date) AS date
    FROM frozen_positions f
    FULL OUTER JOIN live_positions l
      ON f.date = l.date AND f.strategy_name = l.strategy_name AND f.symbol = l.symbol
    WHERE f.quantity IS DISTINCT FROM l.quantity
),
frozen_equity AS (
    SELECT timestamp::date AS date, equity
    FROM trading.equity_curve
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark_frozen_shadow'
      AND timestamp::date BETWEEN :from_date AND :through_date
),
live_equity AS (
    SELECT timestamp::date AS date, equity
    FROM trading.equity_curve
    WHERE portfolio_id = :portfolio_id
      AND portfolio_type = 'benchmark'
      AND timestamp::date BETWEEN :from_date AND :through_date
),
equity_mismatches AS (
    SELECT COALESCE(f.date, l.date) AS date
    FROM frozen_equity f
    FULL OUTER JOIN live_equity l ON f.date = l.date
    WHERE ROUND(COALESCE(f.equity, 0)::numeric, 2) IS DISTINCT FROM
          ROUND(COALESCE(l.equity, 0)::numeric, 2)
       OR (f.equity IS NULL) IS DISTINCT FROM (l.equity IS NULL)
),
days_in_range AS (
    SELECT DISTINCT date FROM (
        SELECT date FROM frozen_positions
        UNION SELECT date FROM live_positions
        UNION SELECT date FROM frozen_equity
        UNION SELECT date FROM live_equity
    ) all_dates
),
day_verdicts AS (
    SELECT d.date,
           NOT EXISTS (SELECT 1 FROM position_mismatches pm WHERE pm.date = d.date)
               AND NOT EXISTS (SELECT 1 FROM equity_mismatches em WHERE em.date = d.date)
               AS pass
    FROM days_in_range d
),
ranked AS (
    SELECT date, pass, ROW_NUMBER() OVER (ORDER BY date DESC) AS rn
    FROM day_verdicts
),
-- The streak ends at the first FAIL encountered going backward from the
-- most recent day; rn - 1 counts how many PASS rows precede it.
first_fail AS (
    SELECT MIN(rn) AS rn FROM ranked WHERE NOT pass
)
SELECT
    (SELECT count(*) FROM days_in_range) AS days_evaluated,
    COALESCE((SELECT rn - 1 FROM first_fail), (SELECT count(*) FROM ranked)) AS current_pass_streak,
    CASE WHEN COALESCE((SELECT rn - 1 FROM first_fail), (SELECT count(*) FROM ranked)) >= 20
         THEN 'PASS: >=20 consecutive days -- eligible to flip benchmark.mode to deferred'
         ELSE 'NOT YET: fewer than 20 consecutive passing days'
    END AS gate_verdict;
