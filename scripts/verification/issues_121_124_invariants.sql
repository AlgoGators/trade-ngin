\set ON_ERROR_STOP on
\pset tuples_only on
\pset format unaligned

-- Required variables: book_a, book_b, strategy_id, source_day, final_day.
-- Values are SQL literals, for example -v book_a="'INVESTOR_P2_A'".
CREATE TEMP TABLE issue_121_124_checks AS
WITH scope AS (
    SELECT ARRAY[:book_a, :book_b]::text[] AS books
), checks(name, passed, detail) AS (
    SELECT 'twenty_ten_day_publications', count(*) = 20, count(*)::text
      FROM trading.investor_book_publications, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND source_day BETWEEN :source_day AND :final_day
    UNION ALL
    SELECT 'distinct_publication_ids', count(DISTINCT publication_id) = 20,
           count(DISTINCT publication_id)::text
      FROM trading.investor_book_publications, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND source_day BETWEEN :source_day AND :final_day
    UNION ALL
    SELECT 'system_only_publications', count(*) = 0, count(*)::text
      FROM trading.investor_book_publications, scope
     WHERE portfolio_id = ANY(scope.books) AND model_stream <> 'system'
    UNION ALL
    SELECT 'no_qt_positions', count(*) = 0, count(*)::text
      FROM trading.positions, scope
     WHERE portfolio_id = ANY(scope.books) AND portfolio_type = 'qt'
    UNION ALL
    SELECT 'independent_book_ids', count(DISTINCT book_id) = 2,
           count(DISTINCT book_id)::text
      FROM trading.investor_books, scope
     WHERE portfolio_id = ANY(scope.books)
    UNION ALL
    SELECT 'independent_opening_anchors', count(*) = 2, count(*)::text
      FROM trading.strategy_trading_days_metadata, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND live_start_date = DATE '2026-10-30'
    UNION ALL
    SELECT 'ten_dates_per_book', count(*) = 2,
           coalesce(string_agg(portfolio_id || ':' || day_count, ',' ORDER BY portfolio_id), '')
      FROM (
        SELECT portfolio_id, count(DISTINCT source_day)::text day_count
          FROM trading.investor_book_publications, scope
         WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
           AND source_day BETWEEN :source_day AND :final_day
         GROUP BY portfolio_id
        HAVING count(DISTINCT source_day) = 10
      ) ten_day_books
    UNION ALL
    SELECT 'owner_scoped_split_audit', count(*) = 4, count(*)::text
      FROM trading.corp_action_applied, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND symbol = 'SYN' AND action_type = 'SPLIT'
       AND ex_date = DATE '2026-11-03'
    UNION ALL
    SELECT 'opposing_owner_fills', count(*) = 2, count(*)::text
      FROM trading.executions
     WHERE portfolio_id = :book_a AND strategy_id = :strategy_id
       AND date = :source_day
       AND ((strategy_name = 'ALPHA' AND side = 'BUY')
         OR (strategy_name = 'BETA' AND side = 'SELL'))
    UNION ALL
    SELECT 'one_account_execution',
           jsonb_array_length(engine_flags->'account_executions') = 1,
           jsonb_array_length(engine_flags->'account_executions')::text
      FROM trading.run_inputs
     WHERE portfolio_id = :book_a AND strategy_id = :strategy_id
       AND date = :source_day
    UNION ALL
    SELECT 'day_two_opens', count(*) = 4, count(*)::text
      FROM trading.executions, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND date = (:source_day::date + 1) AND side = 'BUY'
    UNION ALL
    SELECT 'day_three_closes', count(*) = 4, count(*)::text
      FROM trading.executions, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND date = (:source_day::date + 2) AND side = 'SELL'
    UNION ALL
    SELECT 'day_three_flat', coalesce(sum(abs(quantity)), 0) < 0.000001,
           coalesce(sum(abs(quantity)), 0)::text
      FROM trading.positions, scope
     WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
       AND date = (:source_day::date + 2) AND portfolio_type = 'system'
    UNION ALL
    SELECT 'capital_scales_within_one_share',
           abs(b.quantity - 2 * a.quantity) <= 1,
           a.quantity::text || ':' || b.quantity::text
      FROM trading.positions a
      JOIN trading.positions b
        ON b.strategy_id = a.strategy_id
       AND b.strategy_name = a.strategy_name
       AND b.date = a.date
       AND b.symbol = a.symbol
       AND b.portfolio_type = a.portfolio_type
     WHERE a.portfolio_id = :book_a AND b.portfolio_id = :book_b
       AND a.strategy_id = :strategy_id AND a.strategy_name = 'ALPHA'
       AND a.date = :source_day AND a.symbol = 'SYN'
       AND a.portfolio_type = 'system' AND a.quantity <> 0
    UNION ALL
    SELECT 'w12_total_pnl_identity', max(residual) < 0.000001,
           max(residual)::text
      FROM (
        SELECT abs(total_pnl - (total_realized_pnl - total_transaction_costs
                                + total_unrealized_pnl)) AS residual
          FROM trading.live_results, scope
         WHERE portfolio_id = ANY(scope.books) AND strategy_id = :strategy_id
           AND date BETWEEN :source_day AND :final_day
           AND portfolio_type = 'system'
      ) residuals
    UNION ALL
    SELECT 'w12_owner_realized_reconciliation', max(residual) < 0.000001,
           max(residual)::text
      FROM (
        SELECT abs(r.daily_realized_pnl - coalesce(sum(p.daily_realized_pnl), 0)) residual
          FROM trading.live_results r
          CROSS JOIN scope
          LEFT JOIN trading.positions p
            ON p.portfolio_id = r.portfolio_id
           AND p.strategy_id = r.strategy_id
           AND p.date = r.date
           AND p.portfolio_type = 'system'
         WHERE r.portfolio_id = ANY(scope.books) AND r.strategy_id = :strategy_id
           AND r.date BETWEEN :source_day AND :final_day
           AND r.portfolio_type = 'system'
         GROUP BY r.portfolio_id, r.strategy_id, r.date, r.daily_realized_pnl
      ) residuals
)
SELECT * FROM checks;

SELECT name || '|ok|' || detail
  FROM issue_121_124_checks
 ORDER BY name;

-- Make psql exit nonzero when any invariant fails.
SELECT 1 / nullif((NOT EXISTS (
    SELECT 1 FROM issue_121_124_checks WHERE NOT passed))::int, 0);
