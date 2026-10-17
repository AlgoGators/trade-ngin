-- D-1 (2026-10-16): retire the QT model twin. Run once on new_algo_data by Dominick, after HD has
-- read it. The engine-rpc scheduler stops running the twin with the release that carries this
-- script (QT_CATCHUP_PORTFOLIOS defaults to qt_conservative); also remove qt_conservative_model
-- from QT_CATCHUP_PORTFOLIOS in the host's .env if it is set there.
--
-- What it does: AlgoLens stops listing the twin (it lists active registry rows). Nothing is
-- deleted: the twin's positions, executions, results and metadata stay, as HD asked.
BEGIN;
UPDATE trading.strategy_registry
   SET is_active = false, updated_at = now()
 WHERE portfolio_id = 'QT_CONSERVATIVE_MODEL_PORTFOLIO' AND is_active;
-- Expect exactly one row: the twin, now inactive.
SELECT portfolio_id, is_active FROM trading.strategy_registry
 WHERE portfolio_id = 'QT_CONSERVATIVE_MODEL_PORTFOLIO';
COMMIT;
