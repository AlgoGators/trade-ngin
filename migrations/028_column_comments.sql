-- 028_column_comments.sql
--
-- The comment migration of the statistics lane (LOOP_SPEC v6.2 section 7.1; T-8a commit (9)).
-- COMMENT ON only: 83 column comments and 4 table comments on trading.* and backtest.*. No
-- column, type, default, key, index or value is touched, so every enumerated column list (the
-- INSERT lists, the kColumns constants, the 013 fixture DDL, the gate comparators' projection) is
-- unchanged by it.
--
-- Each text says what the column holds at the binary this migration ships with:
--   * the margin and leverage family of trading.live_results (T-8D section 8), the ratio being the
--     finalised current_portfolio_value over margin_posted at the level of the book, NULL with no
--     margin;
--   * every column whose name does not carry its meaning (T-8D-2 R45): the P&L columns by asset
--     class, the cost columns, total_cumulative_return, average_price, the stored instants,
--     backtest.final_positions as a daily snapshot, the first-insert instants;
--   * the statistic columns: the grid, the sessions a year K, the base and anchor, the settled-row
--     rule and the units (the convention is on the table, the formula on each column), and
--     backtest.results beta / correlation as the lag-1 autocorrelation of the book's own returns;
--   * trading.signals.signal_value, the table comment of trading.equity_curve, and the reporter's
--     three risk columns (portfolio_var is the old price-weighted figure, not a value-at-risk).
-- A comment on a statistic whose definition moved says "Rows written before migration 028 keep the
-- earlier definition": no stored statistic of a past row is rewritten, but for the one row the
-- table comment of trading.live_results names (the Day T-1 row of the first run after the
-- migration, which that run refreshes).
--
-- trading.live_results.risk_scale: a COMMENT replaces the whole text, so this migration restates
-- migration 020's text in full and adds one sentence (equity rows still hold the reporter's
-- recommended scale). risk_detail keeps 020's comment: nothing here touches it.
--
-- SAFETY: comments only; refuses, changing nothing, when a commented table or column is missing;
-- transactional and idempotent.

BEGIN;
DO $$
DECLARE o text; p text[];
BEGIN
    FOREACH o IN ARRAY ARRAY[
        'trading.live_results',
        'trading.live_results.date',
        'trading.live_results.created_at',
        'trading.live_results.total_days',
        'trading.live_results.total_annualized_return',
        'trading.live_results.volatility',
        'trading.live_results.sharpe_ratio',
        'trading.live_results.downside_deviation',
        'trading.live_results.sortino_ratio',
        'trading.live_results.max_drawdown',
        'trading.live_results.win_rate',
        'trading.live_results.winning_days',
        'trading.live_results.losing_days',
        'trading.live_results.avg_win',
        'trading.live_results.avg_loss',
        'trading.live_results.best_day',
        'trading.live_results.worst_day',
        'trading.live_results.gross_profit',
        'trading.live_results.gross_loss',
        'trading.live_results.profit_factor',
        'trading.live_results.total_cumulative_return',
        'trading.live_results.current_portfolio_value',
        'trading.live_results.total_pnl',
        'trading.live_results.daily_pnl',
        'trading.live_results.total_realized_pnl',
        'trading.live_results.daily_realized_pnl',
        'trading.live_results.total_unrealized_pnl',
        'trading.live_results.daily_unrealized_pnl',
        'trading.live_results.daily_transaction_costs',
        'trading.live_results.total_transaction_costs',
        'trading.live_results.active_positions',
        'trading.live_results.margin_posted',
        'trading.live_results.cash_available',
        'trading.live_results.equity_to_margin_ratio',
        'trading.live_results.margin_cushion',
        'trading.live_results.portfolio_leverage',
        'trading.live_results.net_leverage',
        'trading.live_results.gross_leverage',
        'trading.live_results.total_dividend_income',
        'trading.live_results.portfolio_var',
        'trading.live_results.max_correlation',
        'trading.live_results.jump_risk',
        'trading.live_results.risk_scale',
        'trading.positions.average_price',
        'trading.positions.daily_unrealized_pnl',
        'trading.positions.last_update',
        'trading.positions.updated_at',
        'trading.executions.execution_time',
        'trading.executions.implicit_price_impact',
        'trading.executions.total_transaction_costs',
        'trading.signals.signal_value',
        'trading.signals.timestamp',
        'trading.signals.created_at',
        'trading.equity_curve',
        'trading.equity_curve.timestamp',
        'trading.live_run_metadata.created_at',
        'trading.live_run_metadata.strategy_configs',
        'trading.corp_action_applied.qty_held',
        'backtest.results',
        'backtest.results.start_date',
        'backtest.results.end_date',
        'backtest.results.total_return',
        'backtest.results.volatility',
        'backtest.results.sharpe_ratio',
        'backtest.results.downside_volatility',
        'backtest.results.sortino_ratio',
        'backtest.results.max_drawdown',
        'backtest.results.calmar_ratio',
        'backtest.results.var_95',
        'backtest.results.cvar_95',
        'backtest.results.beta',
        'backtest.results.correlation',
        'backtest.results.total_trades',
        'backtest.results.win_rate',
        'backtest.results.profit_factor',
        'backtest.results.avg_win',
        'backtest.results.avg_loss',
        'backtest.results.max_win',
        'backtest.results.max_loss',
        'backtest.results.avg_holding_period',
        'backtest.run_metadata.start_date',
        'backtest.run_metadata.end_date',
        'backtest.final_positions',
        'backtest.final_positions.average_price',
        'backtest.final_positions.last_update',
        'backtest.final_positions.updated_at',
        'backtest.executions.implicit_price_impact'
    ] LOOP
        p := string_to_array(o, '.');
        IF to_regclass(p[1] || '.' || p[2]) IS NULL THEN RAISE EXCEPTION '028: % does not exist', p[1] || '.' || p[2]; END IF;
        IF array_length(p, 1) = 3 AND NOT EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = p[1] AND table_name = p[2] AND column_name = p[3]) THEN
            RAISE EXCEPTION '028: column % does not exist', o;
        END IF;
    END LOOP;
END $$;
COMMENT ON TABLE trading.live_results IS $c028$One row per book (portfolio_id, strategy_id) and date. A row is written by the run of its own date and again when the next run settles it: the levels are rewritten with the day's P&L and the margin and leverage cells are recomputed on the finalised current_portfolio_value. THE STATISTIC COLUMNS (total_days, total_annualized_return, volatility, sharpe_ratio, sortino_ratio, downside_deviation, max_drawdown, win_rate, winning_days, losing_days, avg_win, avg_loss, best_day, worst_day, profit_factor, gross_profit, gross_loss) are taken on the statistics grid. GRID: a futures book's grid is the Sunday-to-Friday UTC dates on which at least 9 distinct symbols of its universe printed a daily bar; an equity book's grid is the NYSE sessions. Returns are taken on the grid's levels (current_portfolio_value): a grid date with no stored row carries the last stored level, and the P&L of a row dated off the grid lands in the next grid return. BASE AND ANCHOR: the series starts at the book's start (trading.strategy_trading_days_metadata.live_start_date, else the first stored row) on the book's initial capital; the start date is a return row only when its level differs from the initial capital. K: the sessions a year every annualised statistic uses, frozen per series in config (statistics.sessions_per_year) and logged each run on the STATISTICS_CONVENTION line: 311.0574 for futures, 252 for equities. A figure annualised on a count of weekdays is a different figure and is not stored. SETTLED-ROW RULE: the row a run writes for its own date carries the statistics through the last settled row (Day T-1); the next run refreshes them through the row's own date; a row no later run settles keeps the statistics it was written with, and a row dated off the grid carries the statistics of the last grid date before it. UNITS: returns, volatilities, the drawdown and the win rate in PERCENT (backtest.results stores fractions); gross_profit and gross_loss in dollars. Rows written before migration 028 keep the earlier definitions, and the statistics migration 030 adds are NULL on the rows written before it, with ONE exception to both: the Day T-1 row of the first run after the migration is refreshed by that run (the settled-row rule), so its statistic columns carry the new definitions and values.$c028$;
COMMENT ON COLUMN trading.live_results."date" IS $c028$The UTC date of the run that wrote the row. Once settled the row covers the close of the previous session to the close of this date's session.$c028$;
COMMENT ON COLUMN trading.live_results."created_at" IS $c028$The wall-clock instant the row was INSERTed (the column default now(), in the database session's time zone). The UPDATE by which the next run settles the row does not change it.$c028$;
COMMENT ON COLUMN trading.live_results."total_days" IS $c028$n: the count of returns of the statistics grid from the book's start through the settled row, equal to winning_days + losing_days + the flat days. Not a count of calendar days and not a count of stored rows. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."total_annualized_return" IS $c028$Percent. ((1 + R)^(K / n) - 1) x 100 on the statistics grid: R the return from the base (the initial capital) to the last grid level, n = total_days, K the sessions a year of the series (311.0574 futures, 252 equities). Computed for every n; 0 for an empty series, -100 when the level is not positive. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."volatility" IS $c028$Percent, annualised, REALISED: the sample standard deviation (n - 1) of the grid returns x sqrt(K). 0 with fewer than two returns. Looks back over the book's own returns: it is not the reporter's portfolio_var and not a forward estimate. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."sharpe_ratio" IS $c028$The mean grid return x K over volatility (the same as mean / sample standard deviation x sqrt(K)), risk-free rate 0. 0 when volatility is 0. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."downside_deviation" IS $c028$Percent, annualised: sqrt(the sum of min(r, 0)^2 over EVERY grid return / n) x sqrt(K), target 0. A return at or above 0 counts as a zero, not as a missing observation. 0 with fewer than two returns. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."sortino_ratio" IS $c028$The mean grid return x K over downside_deviation. 0 when downside_deviation is 0. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."max_drawdown" IS $c028$Percent, positive: the largest fall from a running peak over the grid levels from the book's start, the peak seeded at the base (the initial capital). The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."win_rate" IS $c028$Percent. winning_days / (winning_days + losing_days) x 100: a flat grid return is in neither count and in no denominator. 0 when there is neither a win nor a loss. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."winning_days" IS $c028$The grid returns above zero. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."losing_days" IS $c028$The grid returns below zero. The flat days are total_days - winning_days - losing_days. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."avg_win" IS $c028$Percent. The mean of the positive grid returns; 0 with none. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."avg_loss" IS $c028$Percent, positive: the absolute value of the mean of the negative grid returns; 0 with none. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."best_day" IS $c028$Percent. The largest grid return. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."worst_day" IS $c028$Percent. The smallest grid return. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."gross_profit" IS $c028$Dollars. The sum of the positive grid P&L; the P&L of a grid return is the difference of two grid levels, so it holds the P&L of every stored row folded into that return. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."gross_loss" IS $c028$Dollars, positive: the sum of the absolute values of the negative grid P&L (see gross_profit). The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."profit_factor" IS $c028$gross_profit / gross_loss; 999.99 when there is profit and no loss; 0 with neither. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."total_cumulative_return" IS $c028$Percent. (current_portfolio_value / the book's initial capital - 1) x 100, on the finalised value once the next run settles the day. A level, not a statistic of the grid: defined on every row. The capital the book started with is current_portfolio_value / (1 + total_cumulative_return / 100); AlgoLens derives its invested figure from this column (T-8D-2 R75).$c028$;
COMMENT ON COLUMN trading.live_results."current_portfolio_value" IS $c028$Dollars. The account value of the book at this row: the initial capital plus total_pnl, which is the previous row's value plus this row's daily_pnl. On a futures row the run of the row's date writes the value net of that day's costs only, and the next run finalises it with the day's settlement.$c028$;
COMMENT ON COLUMN trading.live_results."total_pnl" IS $c028$Dollars, cumulative, NET of all modelled transaction costs: total_realized_pnl - total_transaction_costs + total_unrealized_pnl. With daily_pnl and current_portfolio_value, one of the three P&L figures that add across asset classes.$c028$;
COMMENT ON COLUMN trading.live_results."daily_pnl" IS $c028$Dollars, NET of the day's transaction costs: this row's change of total_pnl and of current_portfolio_value. On a futures row it is minus the day's costs as the run of the row's date writes it, and the next run adds the day's settlement.$c028$;
COMMENT ON COLUMN trading.live_results."total_realized_pnl" IS $c028$Dollars, cumulative, GROSS of costs. Futures: the whole book's daily variation settlement to date (total_pnl + total_transaction_costs). Equities: the realised P&L of the closing fills to date. Do not add across asset classes.$c028$;
COMMENT ON COLUMN trading.live_results."daily_realized_pnl" IS $c028$Dollars, GROSS of costs. Futures: the day's settlement move of the whole book; 0 as the run of the row's date writes it, set when the next run settles the day. Equities: the realised P&L of the day's closing fills. Do not add across asset classes.$c028$;
COMMENT ON COLUMN trading.live_results."total_unrealized_pnl" IS $c028$Dollars. Futures: 0 by daily settlement. Equities: the open mark of the book at the row's close, a level. Do not add across asset classes.$c028$;
COMMENT ON COLUMN trading.live_results."daily_unrealized_pnl" IS $c028$Dollars. Futures: 0 by daily settlement. Equities: the change of total_unrealized_pnl from the previous row.$c028$;
COMMENT ON COLUMN trading.live_results."daily_transaction_costs" IS $c028$Dollars. The modelled cost of the day's fills, commissions and fees plus slippage and market impact, each fill at its cost AFTER netting between sleeves (trading.executions.total_transaction_costs minus netting_adjustment). ROLL legs are inside it; their part is daily_roll_costs.$c028$;
COMMENT ON COLUMN trading.live_results."total_transaction_costs" IS $c028$Dollars, cumulative: the previous row's total plus this row's daily_transaction_costs.$c028$;
COMMENT ON COLUMN trading.live_results."active_positions" IS $c028$The count of non-zero position rows of the book's sleeves, not of instruments: a symbol two sleeves hold counts twice.$c028$;
COMMENT ON COLUMN trading.live_results."margin_posted" IS $c028$Dollars. The total posted (initial) margin of the book on this row's date, summed over every position. Futures: the exchange initial margin per contract times contracts, a symbol two sleeves hold on opposite sides counted once on the net. Equities: the cash committed under the account mode at the position's cost basis (the T-1 close only when no basis is known): 100 percent of cost in a cash account; 50 percent long, 150 percent short under Reg T. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."cash_available" IS $c028$Dollars. current_portfolio_value minus margin_posted, on the finalised value once the next run settles the day: the portfolio value not committed as posted margin. Not a cash balance: futures initial margin stays in the account as collateral. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."equity_to_margin_ratio" IS $c028$current_portfolio_value divided by margin_posted, at the level of the book (HD 2026-09-10): one portfolio value over the sum of every position's posted margin, so a book holding futures and equities sums both margin kinds under one portfolio value. Written on the value the run of the row's date knows and recomputed on the finalised current_portfolio_value when the next run settles the day. Higher means more portfolio value per dollar of posted margin. NULL when the book posts no margin. Rows written before migration 028 keep the earlier definition. (On equity rows that was gross notional over posted margin.)$c028$;
COMMENT ON COLUMN trading.live_results."margin_cushion" IS $c028$Fraction. (current_portfolio_value - the maintenance requirement) / current_portfolio_value, recomputed on the finalised value when the next run settles the day. The maintenance requirement: futures, the exchange maintenance margin per contract times contracts; equities, 0 in a cash account (the cushion is then 1) and the initial margin under Reg T. NULL when the book posts no margin. Rows written before migration 028 keep the earlier definition. (A row with no requirement stored -1.)$c028$;
COMMENT ON COLUMN trading.live_results."portfolio_leverage" IS $c028$gross_notional (at the T-1 close) divided by current_portfolio_value, 0 when that value is not positive; recomputed on the finalised value when the next run settles the day. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN trading.live_results."net_leverage" IS $c028$The SIGNED net_notional (at the T-1 close) divided by current_portfolio_value, 0 when that value is not positive, on futures and equity rows; recomputed on the finalised value when the next run settles the day. Rows written before migration 028 keep the earlier definition. (On equity rows that was the risk report's figure, positions at cost basis over the configured capital.)$c028$;
COMMENT ON COLUMN trading.live_results."gross_leverage" IS $c028$Equity rows: gross_notional (at the T-1 close) divided by current_portfolio_value, equal to portfolio_leverage, recomputed on the finalised value when the next run settles the day. Futures rows: NULL, the futures runners do not write the column (read portfolio_leverage). Rows written before migration 028 keep the earlier definition. (On equity rows that was the risk report's figure, positions at cost basis over the configured capital.)$c028$;
COMMENT ON COLUMN trading.live_results."total_dividend_income" IS $c028$Dollars, cumulative: the dividend cash of the book through this date. Each dividend event on record (trading.corp_action_applied) counts its dividend per share x the shares of the trading.positions row dated the ex-date, and is dated on the ex-date row: that row carries it once the next run has settled it, and every later row carries it. It need not equal the sum of corp_action_applied.total_cash, which is computed on the holding of the day before the ex-date (qty_held); an event with no position row on its ex-date is counted on qty_held. Informational ONLY and never added to total_pnl: dividends are already inside the equity P&L through total-return-adjusted prices and the ex-date rescale of the cost basis (basis-weighted, not cash). Written by the equity runner; the futures runners do not write it. Rows written before migration 028 keep the earlier figure: the sum of total_cash, first carried by the row after the ex-date.$c028$;
COMMENT ON COLUMN trading.live_results."portfolio_var" IS $c028$The reporter's OLD price-weighted figure, kept for continuity with its value unchanged: sqrt(w' S w) with w = quantity x quoted price over the gross of the same, NO contract multiplier, and S the reporter's annualised covariance of the stored book over its long window. It is NOT a value-at-risk and NOT the book's risk. For the risk overlay's own readings of the book see the overlay_* columns added by migration 030.$c028$;
COMMENT ON COLUMN trading.live_results."max_correlation" IS $c028$The reporter's largest pairwise correlation (in absolute value) of the stored book over its long window; a measurement, the overlay applies no correlation cap.$c028$;
COMMENT ON COLUMN trading.live_results."jump_risk" IS $c028$The reporter's jump measure of the stored book over its long window (the 99th percentile, over the window's days, of the sum of the absolute weighted daily returns of the holdings, weights by notional); a measurement, not the overlay's R_jump.$c028$;
COMMENT ON COLUMN trading.live_results."risk_scale" IS $c028$The DELIVERED scale of the day's rebalance: the stored book's gross notional over the capped target's gross notional, both at the raw signal closes (held rows counted in both); it can exceed 1. The requested multiplier m_t is never stored here: it is risk_detail.risk_requested. Rows written before migration 020's binary hold the reporter's recommended scale. LOOP_SPEC v6.2 sections 7.2 and 10; migration 020. On equity rows the column still holds the reporter's recommended scale (the equity runner stores it); this sentence was added by migration 028.$c028$;
COMMENT ON COLUMN trading.positions."average_price" IS $c028$Futures rows: the price the strategy last marked the position at (the fill price on a traded date, otherwise the close of the strategy's last processed bar). It is not a cost basis and not always the T-1 close, it is NOT re-anchored when a bar is consumed late (there is no catch-up), and no P&L reads it. Equity rows: the weighted cost basis per share, restated by trading.corp_action_applied.basis_ratio on the ex-date; 0 on a closed row.$c028$;
COMMENT ON COLUMN trading.positions."daily_unrealized_pnl" IS $c028$Futures rows: always 0, by daily settlement. Equity rows: a LEVEL, not a daily change: quantity x (the row's mark - average_price); 0 on a closed row.$c028$;
COMMENT ON COLUMN trading.positions."last_update" IS $c028$The row's date as an instant: the run date's instant, not a write time: the dated runners store the run date at 05:00 UTC on futures rows and at 00:00 UTC on equity rows, and older rows carry other times of day. The time of day carries no meaning; read the date in a UTC session; it gives the date column.$c028$;
COMMENT ON COLUMN trading.positions."updated_at" IS $c028$Equal to last_update on every row the engine writes: the row's date as an instant, not a write time.$c028$;
COMMENT ON COLUMN trading.executions."execution_time" IS $c028$Not the time of a fill: the run date's instant, not a write time: the dated runners store the run date at 05:00 UTC on futures rows and at 00:00 UTC on equity rows, and older rows carry other times of day. The time of day carries no meaning; read the date in a UTC session; it gives the date column. A fill is priced at a session close, not at this instant.$c028$;
COMMENT ON COLUMN trading.executions."implicit_price_impact" IS $c028$PRICE units per contract or share (spread plus market impact), not dollars, stored to 8 decimals. It is not a term of total_transaction_costs: its dollar value is already inside slippage_market_impact.$c028$;
COMMENT ON COLUMN trading.executions."total_transaction_costs" IS $c028$Dollars. commissions_fees + slippage_market_impact: this sleeve row's OWN cost, before netting between sleeves. What the account is charged for the row is this minus netting_adjustment.$c028$;
COMMENT ON COLUMN trading.signals."signal_value" IS $c028$Equity rows: the mean-reversion z-score of the symbol, not a forecast. Futures rows: the sleeve's forecast; in the trend sleeve the ruled forecast F of LOOP_SPEC section 2.5, after the equity slow rule. It is the LATEST forecast as of the run date: a row is stored for every symbol on every run date, so a symbol with no T-1 bar stores its earlier forecast under the new date.$c028$;
COMMENT ON COLUMN trading.signals."timestamp" IS $c028$The run date the signal row belongs to, as an instant: the run date's instant, not a write time: the dated runners store the run date at 05:00 UTC on futures rows and at 00:00 UTC on equity rows, and older rows carry other times of day. The time of day carries no meaning; read the date in a UTC session.$c028$;
COMMENT ON COLUMN trading.signals."created_at" IS $c028$The instant of the row's first INSERT. A later write of the same key updates signal_value and leaves this column.$c028$;
COMMENT ON TABLE trading.equity_curve IS $c028$The daily account value of each live book: one point per book and run date, equity being the book's account value (trading.live_results.current_portfolio_value) of that date, written by the run of the date and rewritten when the next run settles the day. SETTLED BY DATE: a point dated D is settled when the trading.live_results row of the same book dated D has settled_at set (the column migration 029 adds); where that column does not exist yet, a point is settled once a later run has finalised its date.$c028$;
COMMENT ON COLUMN trading.equity_curve."timestamp" IS $c028$The point's date as an instant, part of the key: the run date's instant, not a write time: the dated runners store the run date at 05:00 UTC on futures rows and at 00:00 UTC on equity rows, and older rows carry other times of day. The time of day carries no meaning; read the date in a UTC session. The point's date is its UTC date.$c028$;
COMMENT ON COLUMN trading.live_run_metadata."created_at" IS $c028$The wall-clock instant of the row's FIRST insert for its (date, strategy_id, portfolio_id). A later write of the same day's row updates the three jsonb columns and leaves this column.$c028$;
COMMENT ON COLUMN trading.live_run_metadata."strategy_configs" IS $c028$The futures runners store each sleeve's configuration object as it was read from the configuration files (the raw config, not the values the strategy resolved from it); the equity runner stores the mean-reversion parameters the run resolved.$c028$;
COMMENT ON COLUMN trading.corp_action_applied."qty_held" IS $c028$On a DIVIDEND row: the share count the row's total_cash was computed on (total_cash = qty_held x dividend_per_share): the holding of the trading.positions row dated ex_date - 1, the day before the ex-date, when the run has it. Informational, as total_cash is: never added to P&L. trading.live_results.total_dividend_income counts the same event on the shares of the row dated the ex-date, so it need not equal the sum of total_cash.$c028$;
COMMENT ON TABLE backtest.results IS $c028$One row per run: the run's performance statistics. THE RETURN STATISTICS (total_return, volatility, sharpe_ratio, sortino_ratio, downside_volatility, max_drawdown, calmar_ratio, var_95, cvar_95, beta, correlation) are FRACTIONS (trading.live_results stores percent). Taken on the run's own equity-curve rows after the warm-up (one row per UTC date, the last), annualised with 252: at migration 028 the backtest statistics are not on the live statistics grid or its K (trading.live_results); that alignment belongs to a later migration. THE TRADE STATISTICS (total_trades, win_rate, profit_factor, avg_win, avg_loss, max_win, max_loss, avg_holding_period) are the book's account fills after netting between sleeves, in dollars. A trade is an ACCOUNT fill that reduces, closes or flips a non-zero net position in a contract. Account fills are the book's: the STRATEGY rows of one contract on one bar netted across the sleeves (a full cross is no fill, a partial offset is one fill of the net size); ROLL and BORROW rows are never a trade. A trade's P&L is in DOLLARS: closed quantity x (fill price - average entry price) x the side held x the point value, less the closing fill's cost after netting; the opening fills' costs are not inside a trade, with one exception: a fill through zero (a reversal) is ONE account fill with one cost, and its whole cost, the part that opens the new position included, is charged to the trade it closes. Rows written before migration 028 keep the earlier definitions.$c028$;
COMMENT ON COLUMN backtest.results."start_date" IS $c028$The start of the configured window of the run, warm-up included, stored as the host-local midnight of the configured date. The statistics start after the warm-up rows of the equity curve.$c028$;
COMMENT ON COLUMN backtest.results."end_date" IS $c028$The end of the configured window of the run, stored as the host-local midnight of the configured date.$c028$;
COMMENT ON COLUMN backtest.results."total_return" IS $c028$Fraction. (last level - first level) / first level of the equity curve after the warm-up; not annualised. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."volatility" IS $c028$Fraction, annualised: the population standard deviation (n) of the curve's returns x sqrt(252). Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."sharpe_ratio" IS $c028$The mean return x 252 over volatility, risk-free rate 0; 0 when volatility is 0. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."downside_volatility" IS $c028$Fraction, annualised: sqrt(the sum of min(r, 0)^2 over EVERY return / n) x sqrt(252), target 0; a return at or above 0 counts as a zero. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table). Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."sortino_ratio" IS $c028$The mean return x 252 over downside_volatility; 999 when there is no downside and the mean is not negative. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table). Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."max_drawdown" IS $c028$Fraction, positive: the largest fall from a running peak of the equity curve after the warm-up, the peak starting at the first level after the warm-up. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."calmar_ratio" IS $c028$The mean return x 252 over max_drawdown; 999 when there is no drawdown and the mean is not negative. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."var_95" IS $c028$Fraction of one day, positive for a loss: minus the return at position floor(0.05 x n) of the returns sorted ascending. Historical, not parametric. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."cvar_95" IS $c028$Fraction of one day, positive for a loss: minus the mean of the floor(0.05 x n) lowest returns (at least one); the var_95 observation itself is not among them. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."beta" IS $c028$NOT a market beta: the slope of the regression of a day's return on the previous day's return, of the book's own returns (lag-1 autocorrelation of daily returns). No benchmark enters it. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."correlation" IS $c028$NOT a correlation with a market: the lag-1 autocorrelation of the book's own daily returns. No benchmark enters it. Taken on the run's own equity-curve rows after the warm-up and annualised with 252, not on the live statistics grid (see the comment on the table).$c028$;
COMMENT ON COLUMN backtest.results."total_trades" IS $c028$The count of trades (round trips), not the count of backtest.executions rows. A trade is an ACCOUNT fill that reduces, closes or flips a non-zero net position in a contract. Account fills are the book's: the STRATEGY rows of one contract on one bar netted across the sleeves (a full cross is no fill, a partial offset is one fill of the net size); ROLL and BORROW rows are never a trade. A trade's P&L is in DOLLARS: closed quantity x (fill price - average entry price) x the side held x the point value, less the closing fill's cost after netting; the opening fills' costs are not inside a trade, with one exception: a fill through zero (a reversal) is ONE account fill with one cost, and its whole cost, the part that opens the new position included, is charged to the trade it closes. A position under 1e-9 contracts or shares after a fill is flat. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."win_rate" IS $c028$Fraction. The trades with a positive P&L over total_trades; a trade of exactly 0 is not a win. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."profit_factor" IS $c028$The summed P&L of the winning trades over the summed loss of the others; 999 when there is profit and no loss. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."avg_win" IS $c028$Dollars. The mean P&L of the winning trades; 0 with none. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."avg_loss" IS $c028$Dollars, positive: the mean loss of the trades that did not win; 0 with none. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."max_win" IS $c028$Dollars. The largest P&L of one trade. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."max_loss" IS $c028$Dollars, positive: the largest loss of one trade. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.results."avg_holding_period" IS $c028$Days. The mean, over the trades, of the time from the position's open (or its previous closing fill) to the closing fill; a holding of no length is left out. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c028$;
COMMENT ON COLUMN backtest.run_metadata."start_date" IS $c028$The start of the configured window of the run, warm-up included (a date).$c028$;
COMMENT ON COLUMN backtest.run_metadata."end_date" IS $c028$The end of the configured window of the run (a date).$c028$;
COMMENT ON TABLE backtest.final_positions IS $c028$A DAILY snapshot, despite the name: each strategy's book on every date after the warm-up, one row per run, strategy, symbol and date. The run's final book is the rows of its last date.$c028$;
COMMENT ON COLUMN backtest.final_positions."average_price" IS $c028$A mark, never a cost basis: the price the strategy last saw for the symbol.$c028$;
COMMENT ON COLUMN backtest.final_positions."last_update" IS $c028$The bar instant of the row's date, not a write time.$c028$;
COMMENT ON COLUMN backtest.final_positions."updated_at" IS $c028$Equal to last_update: the bar instant of the row's date, not a write time.$c028$;
COMMENT ON COLUMN backtest.executions."implicit_price_impact" IS $c028$PRICE units per contract or share (spread plus market impact), not dollars. It is not a term of total_transaction_costs.$c028$;
COMMIT;
