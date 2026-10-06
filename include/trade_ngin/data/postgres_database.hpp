// include/trade_ngin/data/postgres_database.hpp

#pragma once

#include <arrow/api.h>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/apps/consumption_projection.hpp"
#include "trade_ngin/apps/equity_model_prior.hpp"
#include "trade_ngin/apps/equity_run_projection.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/publication_transaction.hpp"
#include "trade_ngin/data/qt_seed_publication.hpp"
#include "trade_ngin/data/qt_empty_model_owner_publication.hpp"

namespace trade_ngin {

struct LiveAccountingContext {
    bool previous_found{false};
    std::string previous_source_day;
    double previous_current_portfolio_value{0.0};
    double previous_total_pnl{0.0};
    double previous_total_realized_pnl{0.0};
    double previous_total_unrealized_pnl{0.0};
    double previous_total_transaction_costs{0.0};
    double previous_daily_realized_pnl{0.0};
    double previous_daily_transaction_costs{0.0};
    double previous_gross_notional{0.0};
    double previous_net_notional{0.0};
    double previous_margin_posted{0.0};
    bool preceding_found{false};
    double preceding_current_portfolio_value{0.0};
    double preceding_total_pnl{0.0};
    double preceding_total_realized_pnl{0.0};
    double preceding_total_unrealized_pnl{0.0};
    double preceding_total_transaction_costs{0.0};
    int previous_trading_days{0};
    int trading_days{0};
};

// Explicitly dated completed MODEL position batch; never infers an empty day.
struct QtModelPositionBatch {
    std::string portfolio_id,strategy_id,strategy_name,source_day;
    std::vector<Position> positions;
};

struct EquityPreviousDayFinalization {
    std::string portfolio_id;
    std::string strategy_id;
    Timestamp source_date{};
    std::vector<QtModelPositionBatch> owner_batches;
    std::unordered_map<std::string, double> live_result_updates;
    double equity{0.0};
};

struct InvestorBookOnboarding {
    std::string config_key;
    std::string portfolio_id;
    double initial_capital{0.0};
    std::string opening_date;
    std::vector<std::string> strategy_ids;
    std::string created_by;
};
class DbTransaction {
public:
    ~DbTransaction();

    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&&) noexcept;
    DbTransaction& operator=(DbTransaction&&) noexcept;

    /**
     * @brief Commit every write made in this scope. Idempotent-safe: a second
     *        call is an error rather than a double commit.
     */
    Result<void> commit();

    /// True once commit() has succeeded. False means the destructor will roll back.
    bool committed() const { return committed_; }

    /// True when the scope holds a live transaction (false after a move).
    bool valid() const { return txn_ != nullptr; }

private:
    friend class PostgresDatabase;

    explicit DbTransaction(pqxx::connection& conn);

    pqxx::work& work() { return *txn_; }

    std::unique_ptr<pqxx::work> txn_;
    bool committed_{false};
};


enum class PublicationPriorRequirement { None, VerifiedEquity };

// House publications produce the governed QT continuation stream. Registered
// investor books are deliberately separate: their only operational model is
// the system stream and publication is automatic with the daily transaction.
enum class LivePublicationMode { QtHouse, SystemInvestor };

enum class PublicationEvidenceRequirement {
    LegacyNotCollected,
    RequiredFinalObservations,
};

class PublicationEvidenceToken final {
public:
    bool valid() const noexcept { return static_cast<bool>(marker_); }

private:
    friend class PostgresDatabase;
    std::shared_ptr<const void> marker_;
};

/**
 * @brief Database interface for PostgreSQL
 */
class PostgresDatabase : public DatabaseInterface {
public:
    struct CorpActionRow {
        std::string ticker;
        std::string date_str;  // YYYY-MM-DD
        std::string action;    // vendor label, e.g. "split" | "dividend" | "mergerto"
        double value;
        // Deal terms, populated only for TERMINATION-class rows that carry
        // them (contraticker/contraname are NULL for splits and dividends).
        std::string contra_ticker;
        std::string contra_name;
        std::string name;
    };

    /**
     * @brief Read corporate actions for a ticker list between two dates.
     *
     * Reads from equities_data.corporate_action (existing schema; no DDL).
     * That feed stopped receiving events on 2025-08-29, so this returns
     * nothing for recent windows. It remains the only source of TERMINATION
     * deal terms; PRICE_RESTATING events are now sourced from the live
     * per-bar columns via get_per_bar_corporate_actions() instead.
     *
     * @param actions      Vendor labels to filter on. Defaults to the
     *                     price-restating set for backward compatibility;
     *                     pass vendor_labels_for_class(TERMINATION) for the
     *                     deal-terms path.
     *
     * @param tickers      Symbols to query (typically the live portfolio's
     *                     equity universe).
     * @param start_date   Inclusive YYYY-MM-DD.
     * @param end_date     Inclusive YYYY-MM-DD.
     * @return Sorted by (date, ticker, action); empty result is not an error.
     */
    Result<std::vector<CorpActionRow>> get_corporate_actions(
        const std::vector<std::string>& tickers,
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& actions = {"split", "dividend", "adrratiosplit"});

    /**
     * @brief Last date present in equities_data.corporate_action.
     *
     * E4 item 3. The deal-terms feed's stop date used to be a compiled-in
     * constant (`kCorpActionTableFrozenAfter`) quoted in every termination WARN.
     * A constant cannot notice a revived subscription: after a backfill the
     * message keeps naming the old date until someone rebuilds, and the operator
     * gets no signal that the dormant rollover path just went live. Measuring it
     * makes the log line true by construction.
     *
     * Future-dated placeholder rows are excluded: the table carries tickerchange
     * rows dated 2027-07-18, and a bare max() would report a feed running two
     * years ahead of the run.
     *
     * @param as_of_date Inclusive upper bound, YYYY-MM-DD. Empty means no bound.
     * @return YYYY-MM-DD, or "" when the table holds no row at or before the
     *         bound. An unreachable database is an error, never "".
     */
    Result<std::string> get_corp_action_feed_last_date(const std::string& as_of_date = "");

    /**
     * @brief PRICE_RESTATING events sourced from the live per-bar columns.
     *
     * equities_data.ohlcv_1d carries div_cash and split_factor on the bar the
     * event goes ex. Unlike equities_data.corporate_action these are current,
     * so this is the production source for class-1 events. Splits (including
     * ADR-ratio changes and spin-offs, which the vendor also encodes in
     * split_factor) surface as action "split"; cash dividends as "dividend".
     *
     * @param tickers    Symbols to query; empty returns an empty result.
     * @param start_date Inclusive YYYY-MM-DD.
     * @param end_date   Inclusive YYYY-MM-DD.
     * @return Sorted by (date, ticker, action); empty result is not an error.
     */
    virtual Result<std::vector<CorpActionRow>> get_per_bar_corporate_actions(
        const std::vector<std::string>& tickers,
        const std::string& start_date,
        const std::string& end_date);

    /** @brief One equities_data.ticker_aliases row (SERIES_CONTINUITY source). */
    /**
     * @brief Delisting dates for the given symbols (TERMINATION timing).
     *
     * From equities_data.ohlcv_1d.delisting_date, which is maintained
     * independently of the frozen corporate_action feed.
     *
     * @param tickers   Symbols to look up.
     * @param from_date Inclusive YYYY-MM-DD floor; a delisting older than this
     *        is not returned. Empty means no floor (the pre-BA-8 behaviour).
     *
     * BA-8 / C-1 D12: delisting_date is keyed on the TICKER, and this reads
     * `max(delisting_date)` over the symbol's whole history, so a reused ticker
     * inherits the dead company's row -- HPC carries 2008-11-24, MER 2008-12-31.
     * Acting on one exits a live position at a stale price. The runner has a
     * bars-contradict guard, but that guard needs a bar: `delisting_is_stale`
     * returns false when `last_bar_date` is EMPTY, so a held symbol with no bar
     * in the load window is still terminated on a decade-old row. A floor closes
     * that at the source -- a 2008 delisting has no business reaching a 2026 run.
     *
     * @return symbol -> YYYY-MM-DD for symbols carrying a delisting date at or
     *         after `from_date`.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_delisting_dates(
        const std::vector<std::string>& tickers, const std::string& from_date = "");

    /**
     * @brief Earliest date each symbol was held (non-zero) by this strategy.
     *
     * The corp-action window must reach back to when a position was
     * ESTABLISHED, not to when its row was last written. `last_update` cannot
     * serve that purpose: load_positions_by_date() selects
     * `WHERE DATE(last_update) = DATE($n)`, so every row it returns carries the
     * requested date by construction, and the table has zero rows where
     * last_update differs from date. Deriving a lookback from it always
     * collapses to "yesterday". This queries the position history instead.
     *
     * Erring wide is safe: re-fetched events are rejected by
     * trading.corp_action_applied, so the only cost of an over-wide window is
     * query time.
     *
     * @return symbol -> YYYY-MM-DD of the earliest non-zero holding. Symbols
     *         with no history are absent.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_position_inception_dates(
        const std::string& strategy_id,
        const std::string& strategy_name,
        const std::string& portfolio_id,
        const std::vector<std::string>& symbols,
        const std::string& on_or_before = "",
        const std::string& table_name = "trading.positions");

    /**
     * @brief Date the CURRENT holding of each symbol began -- the class-2 era input.
     *
     * `get_position_inception_dates` is `min(date)` over all history and fails WIDE on
     * purpose, which is right for the class-1 price window and wrong for the rename era
     * test. Tickers get reused. A strategy that held META (Facebook) in 2021, closed it,
     * and re-bought META (Meta Platforms) in 2026 has a lifetime inception of 2021, which
     * satisfies `inception <= effective_until` for our own META -> METV backfill
     * (effective_until 2022-01-31) and re-keys a live 100-share position onto a symbol
     * with no bars at all. Class 2 must ask a narrower question: when did the holding we
     * hold RIGHT NOW start? (BA-2 / C-3 D1.)
     *
     * A holding starts after the most recent flat row. A closed position keeps exactly one
     * row -- quantity 0 on the day it closed (E2-F19, `LiveDailyCycle::is_dead_row`) -- so
     * that row is the break. Absent such a row the position was never closed and this
     * equals the lifetime inception.
     *
     * Direction of error: too LATE is safe (the rename is skipped and retried next run);
     * too EARLY re-keys a live holding, which is silent and permanent. This errs late.
     *
     * BA-19: `on_or_before` bounds BOTH halves of the question to rows the run can
     * legitimately see -- normally the run's own `previous_date`. `trading.positions` is not
     * append-only in practice: an interrupted windowed reset, or a replay abandoned partway,
     * leaves rows dated AFTER the date being replayed. Unbounded, such a row moves the answer
     * twice over -- a future non-zero row can become the `min(date)` of "the current
     * holding", and a future FLAT row raises the break date past every real row and makes the
     * symbol vanish from the map entirely, silently skipping its rename. Neither failure is
     * visible in the run's output. Empty (the default) means unbounded, which is the previous
     * behaviour exactly.
     *
     * @param on_or_before inclusive YYYY-MM-DD ceiling on the rows considered; empty = none.
     * @return symbol -> YYYY-MM-DD the current holding began. Symbols with no non-zero
     *         row after the last flat row are ABSENT, and class 2 skips them.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_current_holding_start_dates(
        const std::string& strategy_id,
        const std::string& strategy_name,
        const std::string& portfolio_id,
        const std::vector<std::string>& symbols,
        const std::string& on_or_before = "",
        const std::string& table_name = "trading.positions");

    /**
     * @brief Latest date this strategy BOUGHT each symbol, on or after `on_or_after`.
     *
     * E2-F17 basis provenance. A class-1 corporate action may only restate a cost basis that
     * was formed BEFORE the ex-date. A fill on run D is priced at close(D-1), so a BUY dated
     * strictly AFTER the ex-date was struck at a price the market had already adjusted and is
     * positive evidence that the basis is contaminated.
     *
     * A BUY is the only fill that re-forms a long book's cost basis
     * (base_strategy.cpp on_execution), and this book is long-only -- shorts are refused by the
     * equity runner -- so BUY side alone is sufficient.
     *
     * Symbols with no such BUY are ABSENT from the map. Absence means "no evidence", NEVER
     * "clean": the caller must resolve it to APPLY, not to skip.
     *
     * `on_or_before` is MANDATORY and must be the date of the book being examined (T-1), not
     * today and never unbounded. On a replay the executions table also holds rows for dates
     * AFTER the run being processed, so an unbounded query reads the future: it reported
     * `BUY 2026-07-30` as evidence against a 2026-06-08 ex-date while replaying June, and
     * skipped a dividend that had to apply.
     *
     * @return symbol -> YYYY-MM-DD of the most recent qualifying BUY.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_last_buy_dates(
        const std::string& strategy_id,
        const std::string& strategy_name,
        const std::string& portfolio_id,
        const std::vector<std::string>& symbols,
        const std::string& on_or_after,
        const std::string& on_or_before,
        const std::string& table_name = "trading.executions");

    /**
     * @brief Raw closes for specific symbols over a date range.
     *
     * Targeted top-up for the corp-action path when a position predates the
     * bulk price load. The dividend denominator needs a close AT each ex-date,
     * not a contiguous series, so this is a plain indexed range read (~45 ms
     * for one symbol over ten years) rather than a re-run of the ~25 s
     * adjusted-series window function.
     *
     * Returns RAW closes, matching close_by_symbol_date's frame -- the
     * denominator is raw-dollar over the ex-date close (05-22 doc §B6).
     *
     * @return symbol -> (YYYY-MM-DD -> close).
     */
    virtual Result<std::unordered_map<std::string, std::map<std::string, double>>>
    get_historical_closes(const std::vector<std::string>& symbols,
                          const std::string& start_date,
                          const std::string& end_date);


    struct AppliedCorpActionRow {
        std::string symbol;
        std::string action_type;
        std::string ex_date;  ///< YYYY-MM-DD
        double qty_held{0.0};
        double dividend_per_share{0.0};
        double total_cash{0.0};
        /**
         * Run date of the pass that wrote the row (E2-F23, migration 005).
         * YYYY-MM-DD, or "" for a legacy row written before the column existed.
         *
         * NOT part of the natural key. It exists so a run can tell whether the
         * dedup rows it is about to trust came from an EARLIER pass or a LATER
         * one -- the ex-date cannot, because the measured failure (BKNG, ex-date
         * 2026-04-06, re-run of 04-07 over an un-reset dedup table) has an
         * ex-date safely in the past, and `applied_at` cannot, because an earlier
         * chain always carries an earlier wall clock.
         */
        std::string run_date;
        /**
         * The factor this event divided the cost basis by (F-8, migration 006):
         * F for a SPLIT/ADR_SPLIT, 1 + d/c for a DIVIDEND, i.e.
         * PositionAdjustment.ratio_change. Multiplying
         * trading.positions.average_price by the product of these over a holding
         * recovers the BROKER-frame basis, which a dividend never moves.
         *
         * `basis_ratio_known` false means the stored value was NULL -- a row
         * written before 006, or a TERMINATION, which restates nothing. NULL is
         * UNKNOWN and NOT 1.0: treating it as 1.0 reports an adjusted basis as
         * broker-equivalent. Reconciliation only; not part of the natural key.
         */
        double basis_ratio{1.0};
        bool basis_ratio_known{false};
    };

    /**
     * @brief Load every corp action already applied for one portfolio+strategy+name.
     *
     * Durable replacement for the applied_corp_actions.json state file, which
     * lived under a container path with no volume and so was lost on redeploy.
     * Loading everything is deliberate: the record is the strategy's lifetime
     * dedup set and is small (order thousands of rows even at full universe
     * scale), and cumulative dividend income is summed from it.
     *
     * strategy_name is part of the key, not decoration: one strategy_id can
     * carry several names (the live runners build a combined id, so
     * LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST holds both TREND_FOLLOWING and
     * TREND_FOLLOWING_FAST rows). Reading without it hands one strategy's
     * applied events to another, which then skips its own adjustment and
     * carries a permanently wrong cost basis, and sums dividend income across
     * every name under the id.
     */
    virtual Result<std::vector<AppliedCorpActionRow>> load_applied_corp_actions(
        const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name);

    /**
     * @brief Record corp actions as applied. Idempotent per natural key.
     *
     * ON CONFLICT DO NOTHING against
     * (portfolio_id, strategy_id, strategy_name, symbol, action_type, ex_date):
     * re-recording an event is a no-op rather than an error, so a partially
     * completed run is safe to repeat.
     */
    virtual Result<void> store_applied_corp_actions(
        const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name,
        const std::vector<AppliedCorpActionRow>& rows);

    /**
     * @brief Record applied corp actions inside a caller-owned unit of work.
     *
     * Composed with store_positions(DbTransaction&, ...) so an adjusted position
     * and the dedup row that protects it cannot be separated by a failure.
     */
    virtual Result<void> store_applied_corp_actions(
        DbTransaction& txn, const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name,
        const std::vector<AppliedCorpActionRow>& rows);

    struct TickerAliasRow {
        std::string historical_ticker;
        std::string current_symbol;
        std::string effective_until;  // YYYY-MM-DD; empty when NULL
        std::string note;
    };

    /**
     * @brief Read the curated historical-ticker -> current-symbol map.
     *
     * A curated subset, not the full rename history: symbols absent from it
     * are simply left unmapped by the caller.
     */
    virtual Result<std::vector<TickerAliasRow>> get_ticker_aliases();


    Result<std::unique_ptr<DbTransaction>> begin_unit_of_work();
    // Explicit system/equity UOW writer: caller owns the only commit.
    Result<void> store_positions(DbTransaction& txn, const std::vector<Position>& positions,
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const std::string& table_name);


    struct MarketDataSnapshot {
        pqxx::result rows;
        std::shared_ptr<arrow::Table> table;
    };
    // Reuses the legacy read and conversion in an explicitly inherited transaction.
    // Does not acquire a connection, commit, or publish MarketDataBus events.
    static Result<MarketDataSnapshot> read_market_data_snapshot(
        pqxx::work& transaction, const std::vector<std::string>& symbols,
        const Timestamp& start_date, const Timestamp& end_date, AssetClass asset_class,
        DataFrequency frequency, const std::string& data_type);

    // Captures immutable current-day writes without opening a transaction over
    // compute. A true return means a retired scope's approved stop was recorded.
    Result<bool> begin_live_publication(const std::string& strategy_id,
        const std::string& portfolio_id, const Timestamp& date,
        const nlohmann::json& snapshot, bool controlled, const std::string& version,
        PublicationEvidenceRequirement requirement = PublicationEvidenceRequirement::LegacyNotCollected,
        PublicationEvidenceToken* token_out = nullptr,
        PublicationPriorRequirement prior_requirement = PublicationPriorRequirement::None);
    Result<std::unordered_map<std::string,Position>> load_equity_model_system_positions(
        const std::string& portfolio_id,const Timestamp& date);
    Result<std::unordered_map<std::string,Position>> load_equity_system_positions_by_owner(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const Timestamp& date);
    Result<void> store_model_position_batch(const QtModelPositionBatch&);
    Result<void> clear_equity_model_current_positions(const std::string& portfolio_id, const Timestamp& date);
    Result<VerifiedEquityModelPrior> capture_equity_model_prior(
        const EquityModelPriorSelection&, const EquityModelPriorOwner&);
    Result<void> attach_equity_run_consumption(const PublicationEvidenceToken&, const EquityRunProjection&);
    Result<void> attach_live_consumption(const PublicationEvidenceToken& token,
        const ConsumptionProjection& projection);
    Result<void> publish_live_publication();
    std::optional<LivePublicationMode> live_publication_mode() const noexcept;
    Result<void> record_qt_model_seed_publication(const QtModelSeedPublication& publication);
    void abandon_live_publication(const std::string& failure_code = "computation_failed");
    Result<void> store_live_run_inputs(const std::string& strategy_id,
        const std::string& portfolio_id, const Timestamp& date, const nlohmann::json& row);
    // Scoped SQL is retained only for the existing previous-day finalizer.
    Result<void> execute_scoped_live_update(const std::string& query,
        const std::string& strategy_id, const std::string& portfolio_id);
    /**
     * @brief Constructor
     * @param connection_string Connection string for PostgreSQL
     */
    explicit PostgresDatabase(std::string connection_string);

    /**
     * @brief Destructor
     */
    ~PostgresDatabase() override;

    // Delete copy and move operations
    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;
    PostgresDatabase(PostgresDatabase&&) = delete;
    PostgresDatabase& operator=(PostgresDatabase&&) = delete;

    /**
     * @brief Connect to the database
     * @return Result indicating success or failure
     */
    Result<void> connect() override;

    /**
     * @brief Disconnect from the database
     */
    void disconnect() override;

    /**
     * @brief Check if the database connection is active
     * @return True if connected, false otherwise
     */
    bool is_connected() const override;

    /**
     * @brief Get market data for a list of symbols
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the market data
     */
    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& data_type = "ohlcv") override;

    /**
     * @brief Get latest market prices for symbols
     * @param symbols Vector of symbols to get prices for
     * @param asset_class Asset class of the symbols
     * @param freq Data frequency
     * @param data_type Type of data (ohlcv, etc.)
     * @return Result containing map of symbol to latest price
     */
    Result<std::unordered_map<std::string, double>> get_latest_prices(
        const std::vector<std::string>& symbols, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& data_type = "ohlcv") override;

    /**
     * @brief Load positions by date and strategy
     * @param strategy_id Combined strategy identifier (e.g.,
     * "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST")
     * @param strategy_name Individual strategy name (e.g., "TREND_FOLLOWING"). If empty, loads all
     *                      positions matching the strategy_id.
     * @param portfolio_id Portfolio identifier (e.g., BASE_PORTFOLIO, CONSERVATIVE_PORTFOLIO)
     * @param date Date to load positions for
     * @param table_name Name of the positions table
     * @return Result containing map of symbol to position
     */
    Result<std::unordered_map<std::string, Position>> load_positions_by_date(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name = "trading.positions",
        const std::string& portfolio_type = "system") override;

    // Strict investor-report read. Unlike load_positions_by_date, this never
    // falls back to an unscoped legacy query when portfolio_type is absent.
    using ReportPositionRows =
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>;
    // All requested individual strategies share one SELECT/MVCC snapshot.
    virtual Result<ReportPositionRows> load_report_positions_by_date(
        const std::string& strategy_id, const std::vector<std::string>& strategy_names,
        const std::string& portfolio_id, const Timestamp& report_date,
        const std::string& portfolio_type);

    // Current immutable desk receipt and source proof, under ordered SQL locks.
    // Missing capability/evidence never permits a legacy fallback.
    virtual Result<nlohmann::json> load_qt_processed_report_evidence(
        const std::string& portfolio_id, const std::string& source_day);
    virtual Result<ReportPositionRows> load_qt_proposal_positions_by_date(
        const std::string& strategy_id, const std::vector<std::string>& strategy_names,
        const std::string& portfolio_id, const Timestamp& date);
    virtual Result<int> seed_qt_proposal_positions_from_system(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const std::string& date);

    /**
     * @brief Store execution reports in the database
     * @param executions List of execution reports
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> validate_portfolio_id(const std::string& portfolio_id) const;
    Result<void> validate_strategy_id(const std::string& strategy_id) const;
    Result<void> validate_operational_stream(const std::string& portfolio_id,
                                             const std::string& portfolio_type);
    Result<void> validate_execution_report(const ExecutionReport& exec) const;

    // Creates an immutable system-stream investor book and all annualization
    // anchors in one database transaction. An identical replay returns the
    // existing book_id; any conflicting replay fails without partial rows.
    Result<std::string> onboard_investor_book(const InvestorBookOnboarding& request);

    Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                  const std::string& strategy_id, const std::string& strategy_name,
                                  const std::string& portfolio_id,
                                  const std::string& table_name = "trading.executions", const std::string& portfolio_type = "system") override;

    /**
     * @brief Store positions in the database
     * @param positions List of positions
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to store data
     * @param portfolio_type "system" (engine output) or "qt" (human-adjusted, executed)
     * @return Result indicating success or failure
     */
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string& strategy_id, const std::string& strategy_name,
                                 const std::string& portfolio_id, const std::string& table_name,
                                 const std::string& portfolio_type = "system") override;

    /**
     * @brief Store signals in the database
     * @param signals Map of signals by symbol
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param timestamp Timestamp for the signals
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const std::string& strategy_name,
                               const std::string& portfolio_id, const Timestamp& timestamp,
                               const std::string& table_name) override;

    /**
     * @brief Get a list of symbols from the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the list of symbols
     */
    Result<std::vector<std::string>> get_symbols(AssetClass asset_class,
                                                 DataFrequency freq = DataFrequency::DAILY,
                                                 const std::string& data_type = "ohlcv") override;

    /**
     * @brief Execute a query and return the result as an Arrow table
     * @param query SQL query to execute
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override;

    /**
     * @brief Execute a direct SQL query without Arrow table conversion
     * @param query SQL query to execute
     * @return Result indicating success or failure
     */
    Result<void> execute_direct_query(const std::string& query);

    // ============================================================================
    // BACKTEST DATA STORAGE METHODS
    // ============================================================================

    /**
     * @brief Store backtest execution data
     * @param executions Vector of execution reports
     * @param run_id Backtest run identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_executions(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& portfolio_id,
        const std::string& table_name = "backtest.executions") override;

    // Multi-strategy version: store executions with strategy_id
    virtual Result<void> store_backtest_executions_with_strategy(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id,
        const std::string& table_name = "backtest.executions");

    /**
     * @brief Store backtest signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param run_id Backtest run identifier
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_signals(
        const std::unordered_map<std::string, double>& signals, const std::string& strategy_id,
        const std::string& run_id, const Timestamp& timestamp,
        const std::string& portfolio_id,
        const std::string& table_name = "backtest.signals") override;

    /**
     * @brief Store backtest run metadata
     * @param run_id Backtest run identifier
     * @param name Run name
     * @param description Run description
     * @param start_date Start date
     * @param end_date End date
     * @param hyperparameters JSON configuration
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_metadata(
        const std::string& run_id, const std::string& name, const std::string& description,
        const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters, const std::string& portfolio_id,
        const std::string& table_name = "backtest.run_metadata") override;

    // Multi-strategy version: store metadata with portfolio_run_id, strategy_allocation,
    // portfolio_config
    virtual Result<void> store_backtest_metadata_with_portfolio(
        const std::string& run_id, const std::string& portfolio_run_id,
        const std::string& strategy_id, double strategy_allocation,
        const nlohmann::json& portfolio_config, const std::string& name,
        const std::string& description, const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters, const std::string& portfolio_id,
        const std::string& table_name = "backtest.run_metadata");

    // ============================================================================
    // LIVE TRADING DATA STORAGE METHODS
    // ============================================================================

    /**
     * @brief Store live trading daily results
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param total_return Total return for the day
     * @param sharpe_ratio Sharpe ratio
     * @param sortino_ratio Sortino ratio
     * @param max_drawdown Maximum drawdown
     * @param calmar_ratio Calmar ratio
     * @param volatility Volatility
     * @param total_trades Total number of trades
     * @param win_rate Win rate
     * @param profit_factor Profit factor
     * @param avg_win Average win
     * @param avg_loss Average loss
     * @param max_win Maximum win
     * @param max_loss Maximum loss
     * @param avg_holding_period Average holding period
     * @param var_95 Value at Risk (95%)
     * @param cvar_95 Conditional Value at Risk (95%)
     * @param beta Beta
     * @param correlation Correlation
     * @param downside_volatility Downside volatility
     * @param config Additional configuration JSON
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double sharpe_ratio, double sortino_ratio, double max_drawdown, double calmar_ratio,
        double volatility, int total_trades, double win_rate, double profit_factor, double avg_win,
        double avg_loss, double max_win, double max_loss, double avg_holding_period, double var_95,
        double cvar_95, double beta, double correlation, double downside_volatility,
        const nlohmann::json& config, const std::string& table_name = "trading.results") override;

    /**
     * @brief Store live trading results with new schema
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param total_return Total return for the day
     * @param volatility Portfolio volatility
     * @param total_pnl Total P&L
     * @param unrealized_pnl Unrealized P&L
     * @param realized_pnl Realized P&L
     * @param current_portfolio_value Current portfolio value
     * @param portfolio_var Portfolio VaR
     * @param net_leverage Net leverage
     * @param gross_leverage Gross leverage
     * @param max_correlation Max correlation risk
     * @param jump_risk Jump risk (99th percentile)
     * @param risk_scale Risk scale factor
     * @param total_notional Total notional exposure
     * @param active_positions Number of active positions
     * @param config Strategy configuration JSON
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_live_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double volatility, double total_pnl, double unrealized_pnl, double realized_pnl,
        double current_portfolio_value, double daily_realized_pnl, double daily_unrealized_pnl,
        double portfolio_var, double net_leverage, double gross_leverage,
        double margin_leverage, double margin_cushion, double max_correlation, double jump_risk,
        double risk_scale, double gross_notional, double net_notional, int active_positions,
        double total_transaction_costs, double margin_posted, double cash_available,
        const nlohmann::json& config, const std::string& table_name,
        const std::string& portfolio_id, const std::string& portfolio_type = "system") override;

    Result<std::tuple<double, double, double>> get_previous_live_aggregates(
        const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name = "trading.live_results", const std::string& portfolio_type = "system") override;

    // Complete cumulative frame used by the multi-sleeve equity daily
    // accounting identity. The prior row and portfolio-scoped trading-day
    // count are captured in one read transaction.
    Result<LiveAccountingContext> get_live_accounting_context(
        const std::string& strategy_id, const std::string& portfolio_id,
        const Timestamp& date, const std::string& portfolio_type = "system");

    // Queue the complete T-1 cash-equity finalization behind the current
    // publication. Owner rows, the combined result row, and the equity curve
    // commit in the same transaction as day T or all roll back together.
    Result<void> stage_equity_previous_day_finalization(
        const EquityPreviousDayFinalization& finalization);

    /**
     * @brief Store live trading equity curve point
     * @param strategy_id Strategy identifier
     * @param timestamp Timestamp of the equity point
     * @param equity Equity value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve(
        const std::string& strategy_id, const Timestamp& timestamp, double equity,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve",
        const std::string& portfolio_type = "system") override;

    // Explicit EQ system owner path; UTC instant, existing book fence/publication
    // part and full owner/stream conflict key. Legacy override stays unchanged.
    virtual Result<void> store_equity_trading_equity_curve(const std::string& strategy_id,
        const Timestamp& timestamp,double equity,const std::string& portfolio_id);

    /**
     * @brief Seed the 'qt' position stream from the 'system' stream for one day.
     *
     * Carries the latest QT state (including zeros) into the requested date.
     * New identities are seeded from that day's system positions. Existing
     * same-day QT rows are never overwritten; missing identities are filled.
     * QT closes/replaces a position explicitly, never by deleting its row.
     *
     * No-op (with a warning) if the dual-portfolio migration has not been applied.
     *
     * @return Result containing the number of rows seeded (0 if already seeded)
     */
    virtual Result<int> seed_qt_positions_from_system(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const std::string& date,
        const std::string& table_name = "trading.positions");

    /**
     * @brief Store multiple live trading equity curve points
     * @param strategy_id Strategy identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve_batch(
        const std::string& strategy_id,
        const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve", const std::string& portfolio_type = "system") override;

    // ============================================================================
    // NEW METHODS TO REPLACE RAW SQL (Phase 0 Refactoring)
    // ============================================================================

    /**
     * @brief Delete stale executions for a given date and order IDs
     * @param order_ids List of order IDs to match
     * @param date Date to filter executions
     * @param table_name Name of the executions table
     * @return Result indicating success or failure
     */
    virtual Result<void> delete_stale_executions(const std::vector<std::string>& order_ids,
                                                  const Timestamp& date,
                                                  const std::string& strategy_name,
                                                  const std::string& table_name = "trading.executions", const std::string& portfolio_type = "system");

    virtual Result<void> delete_stale_executions_scoped(
        const std::vector<std::string>& order_ids, const Timestamp& date,
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const std::string& table_name = "trading.executions",
        const std::string& portfolio_type = "system");

    /**
     * @brief Store backtest summary results (replaces raw SQL INSERT)
     * @param run_id Backtest run identifier
     * @param start_date Start date of backtest
     * @param end_date End date of backtest
     * @param metrics Map of metric name to value
     * @param table_name Name of the results table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_summary(
        const std::string& run_id, const Timestamp& start_date, const Timestamp& end_date,
        const std::unordered_map<std::string, double>& metrics,
        const std::string& portfolio_id,
        const std::string& table_name = "backtest.results");

    /**
     * @brief Store backtest equity curve batch (replaces raw SQL INSERT)
     * @param run_id Backtest run identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_equity_curve_batch(
        const std::string& run_id, const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id,
        const std::string& table_name = "backtest.equity_curve");

    /**
     * @brief Store backtest final positions (replaces raw SQL INSERT)
     * @param positions Vector of positions
     * @param run_id Backtest run identifier
     * @param table_name Name of the positions table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_positions(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& portfolio_id,
        const std::string& table_name = "backtest.final_positions");

    virtual Result<void> replace_backtest_positions_for_date(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id,
        const Timestamp& date,
        const std::string& table_name = "backtest.final_positions");

    // Multi-strategy version: store positions with strategy_id
    virtual Result<void> store_backtest_positions_with_strategy(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id,
        const std::string& table_name = "backtest.final_positions");

    /**
     * @brief Update live results for previous day finalization (replaces raw SQL UPDATE)
     * @param strategy_id Strategy identifier
     * @param date Date to update
     * @param updates Map of column name to new value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    Result<void> update_equity_historical_metrics(const std::string& strategy_id,
        const std::string& portfolio_id,const Timestamp& date,
        const std::unordered_map<std::string,double>& updates,
        const std::vector<std::string>& null_columns);

    virtual Result<void> update_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& updates,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results", const std::string& portfolio_type = "system");

    /**
     * @brief Update live equity curve (replaces raw SQL UPDATE)
     * @param strategy_id Strategy identifier
     * @param date Date to update
     * @param equity New equity value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> update_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date, double equity,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve", const std::string& portfolio_type = "system");

    /**
     * @brief Delete existing live results for a date (replaces raw SQL DELETE)
     * @param strategy_id Strategy identifier
     * @param date Date to delete
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    virtual Result<void> delete_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results", const std::string& portfolio_type = "system");

    /**
     * @brief Delete existing equity curve entry for a date (replaces raw SQL DELETE)
     * @param strategy_id Strategy identifier
     * @param date Date to delete
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> delete_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve", const std::string& portfolio_type = "system");

    /**
     * @brief Store complete live results row with all metrics (replaces raw SQL INSERT)
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param metrics Complete set of metrics as key-value pairs
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_live_results_complete(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& metrics,
        const std::unordered_map<std::string, int>& int_metrics, const nlohmann::json& config,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results", const std::string& portfolio_type = "system");

    /**
     * @brief Store live trading run metadata
     * @param date Trading date
     * @param strategy_id Combined strategy identifier
     * @param portfolio_id Portfolio identifier
     * @param strategy_allocations Map of strategy name to allocation (JSON)
     * @param portfolio_config Portfolio configuration (JSON)
     * @param strategy_configs Per-strategy configurations (JSON)
     * @param table_name Name of the table
     * @return Result indicating success or failure
     */
    Result<void> store_live_run_metadata(
        const Timestamp& date, const std::string& strategy_id, const std::string& portfolio_id,
        const nlohmann::json& strategy_allocations, const nlohmann::json& portfolio_config,
        const nlohmann::json& strategy_configs,
        const std::string& table_name = "trading.live_run_metadata");

    /**
     * @brief Store risk limits that the engine enforces for AlgoLens validation
     *
     * Called once per trading session after risk limits are finalized. Creates one row in
     * trading.risk_limits with the envelope that will be enforced for this run.
     * AlgoLens queries this table (ORDER BY published_at DESC LIMIT 1) to validate manual
     * position edits before committing them.
     *
     * Failure to publish does NOT stop the trading run (logs a warning and continues).
     * AlgoLens treats a missing envelope as "not evaluated" rather than "pass", so a failed
     * publish degrades safely: the UI shows yellow instead of green.
     *
     * @param strategy_id Combined strategy identifier (e.g., "LIVE_TREND_FOLLOWING_FAST")
     * @param portfolio_id Portfolio identifier (e.g., "BASE_PORTFOLIO", "CONSERVATIVE_PORTFOLIO")
     * @param limits JSONB object. The engine publishes ONLY limits it actually enforces:
     *        - max_symbol_position_contracts: map<string, double>, per-symbol caps in
     *          CONTRACT UNITS (not notional dollars)
     *        - max_gross_leverage: double, enforced by RiskManager::calculate_leverage_multiplier
     *        - max_net_leverage:   double, same enforcement point
     *
     *        Deliberately NOT published: max_gross_notional and max_position_count. The
     *        engine constrains leverage RATIOS, not dollar caps, and has no notion of a
     *        maximum open-position count. Emitting either would put a number nobody chose
     *        in front of a trader as a green light -- worse than an absent limit, because
     *        an absent one is visibly absent. If the fund later wants those limits, they
     *        must be enforced here first, then published; never the reverse.
     * @param table_name Name of the table to insert into (default: "trading.risk_limits")
     * @return Result indicating success or failure
     */
    Result<void> store_risk_limits(const std::string& strategy_id, const std::string& portfolio_id,
                                    const nlohmann::json& limits,
                                    const std::string& table_name = "trading.risk_limits");

    /**
     * @brief Get contract metadata for trading instruments
     * @return Result containing Arrow table with contract metadata
     */
    virtual Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const;

    /**
     * @brief Convert asset class to string for database queries
     * @param asset_class Asset class to convert
     * @return String representation for database queries
     */
    std::string asset_class_to_string(AssetClass asset_class) const;

    /**
     * @brief Get the connection string
     * @return Connection string
     */
    std::string get_connection_string() const {
        return connection_string_;
    }

    /**
     * @brief Get the component ID
     * @return Component ID
     */
    const std::string& get_component_id() const {
        return component_id_;
    }

    /**
     * @brief Validate table name for SQL injection prevention
     * @param table_name Table name to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_table_name(const std::string& table_name) const;

    /** Validate a schema, table, or column identifier before concatenation. */
    Result<void> validate_identifier(const std::string& identifier) const;

    /**
     * @brief Check whether a column exists on a (schema-qualified) table.
     *
     * Used to detect at runtime whether the dual-portfolio migration
     * (migrations/001_add_portfolio_type.sql) has been applied, so the engine
     * behaves correctly against both an upgraded and a not-yet-upgraded
     * database. That decouples rolling out this code from running the
     * migration -- neither has to go first.
     *
     * @param txn Active transaction to query within
     * @param qualified_table Table name, optionally schema-qualified
     * @param column Column to look for
     * @return true if the column exists
     */
    bool column_exists(pqxx::work& txn, const std::string& qualified_table,
                       const std::string& column) const;

private:
    enum PublicationPart : unsigned {
        PositionsPart=1, LimitsPart=2, ResultsPart=4, InputsPart=8,
        QtSeedPart=16, MetadataPart=32, EquityPart=64,
        SystemCompletePublication=111, CompletePublication=127
    };
    struct CapturedEquityModelPrior {
        EquityModelPriorSelection selection;
        EquityModelPriorOwner owner;
        VerifiedEquityModelPrior observed;
    };
    struct PendingPublication {
        std::string registry_id, strategy_id, portfolio_id, date, attempt_id,
                    publication_id, producer_version;
        long long registry_revision = 0, intent_id = 0;
        nlohmann::json snapshot;
        std::vector<std::function<Result<void>()>> writes;
        std::vector<std::string> fresh_system_members;
        std::vector<QtSeedRow> fresh_system_components;
        std::vector<QtModelEmptyBatchScope> fresh_empty_batches;
        std::vector<std::pair<ComponentPositionKey,std::string>> inserted_proposal_revisions;
        std::set<std::string> proposal_sealed_members;
        PublicationEvidenceRequirement evidence_requirement =
            PublicationEvidenceRequirement::LegacyNotCollected;
        std::shared_ptr<const void> evidence_marker;
        std::optional<ConsumptionProjection> final_consumption;
        std::optional<EquityRunProjection> equity_final_consumption;
        PublicationPriorRequirement prior_requirement = PublicationPriorRequirement::None;
        LivePublicationMode mode = LivePublicationMode::QtHouse;
        std::optional<CapturedEquityModelPrior> equity_prior;
        bool invalid_payload = false;
        bool inspection_capture_queued = false;
        unsigned parts = 0;
    };
    std::unique_ptr<PendingPublication> pending_publication_;
    pqxx::work* publication_transaction_ = nullptr;
    bool defer_live_write(const Timestamp& date, std::function<Result<void>()> write, unsigned part=0);
    bool defer_live_write(std::function<Result<void>()> write, unsigned part=0);
    void fence_live_write(pqxx::work& txn, const std::string& strategy_id,
        const std::string& portfolio_id, const std::string& stream = "system",
        bool proposal_seed_operation = false);
    void require_proposal_capability(pqxx::work& txn);
    void require_qt_exact_storage_capability(pqxx::work& txn);
    void poison_proposal_refusal();
    Result<void> store_applied_corp_actions_in(pqxx::work& txn,
        const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name, const std::vector<AppliedCorpActionRow>& rows);

    std::string connection_string_;
    std::unique_ptr<pqxx::connection> connection_;
    std::mutex mutex_;
    std::string component_id_;

    /**
     * @brief Validate the database connection
     * @return Result indicating success or failure
     */
    Result<void> validate_connection() const;

    /**
     * @brief Format a timestamp as a string
     * @param ts Timestamp to format
     * @return Formatted string
     */
    static std::string format_timestamp(const Timestamp& ts);

    /**
     * @brief Convert a Side enum to a string
     * @param side Side to convert
     * @return String representation of the side
     */
    std::string side_to_string(Side side) const;

    /**
     * @brief Execute market data query with proper parameterization
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @param txn Database transaction
     * @return Result containing the query result
     */
    static Result<pqxx::result> execute_market_data_query(const std::vector<std::string>& symbols,
                                                   const Timestamp& start_date,
                                                   const Timestamp& end_date,
                                                   AssetClass asset_class, DataFrequency freq,
                                                   const std::string& data_type,
                                                   pqxx::work& txn);

    /**
     * @brief Validate table name components to prevent injection
     * @param asset_class Asset class
     * @param data_type Data type
     * @param freq Data frequency
     * @return Result indicating success or failure
     */
    static Result<void> validate_table_name_components(AssetClass asset_class,
                                                const std::string& data_type,
                                                DataFrequency freq);

    /**
     * @brief Validate symbol for SQL injection prevention
     * @param symbol Symbol to validate
     * @return Result indicating success or failure
     */
    static Result<void> validate_symbol(const std::string& symbol);

    /**
     * @brief Validate symbols for SQL injection prevention
     * @param symbols Symbols to validate
     * @return Result indicating success or failure
     */
    static Result<void> validate_symbols(const std::vector<std::string>& symbols);

    /**
     * @brief Validate position data
     * @param pos Position to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_position(const Position& pos) const;

    /**
     * @brief Validate signal data
     * @param symbol Symbol for signal
     * @param signal Signal value
     * @return Result indicating success or failure
     */
    Result<void> validate_signal_data(const std::string& symbol, double signal) const;

    /**
     * @brief Convert a pqxx result to an Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    static Result<std::shared_ptr<arrow::Table>> convert_to_arrow_table(const pqxx::result& result);

    /**
     * @brief Convert contract metadata result to Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> convert_metadata_to_arrow(
        const pqxx::result& result) const;

    /**
     * @brief Convert any pqxx result to a generic Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table with columns dynamically determined
     */
    Result<std::shared_ptr<arrow::Table>> convert_generic_to_arrow(
        const pqxx::result& result) const;

    /**
     * @brief Get the latest data time for a given asset class and frequency
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the latest data time
     */
    Result<Timestamp> get_latest_data_time(AssetClass asset_class, DataFrequency freq,
                                           const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the time range for data in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the time range
     */
    Result<std::pair<Timestamp, Timestamp>> get_data_time_range(
        AssetClass asset_class, DataFrequency freq, const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the number of data points in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param symbol Symbol to retrieve
     * @param data_type Type of data to retrieve
     * @return Result containing the number of data points
     */
    Result<size_t> get_data_count(AssetClass asset_class, DataFrequency freq,
                                  const std::string& symbol,
                                  const std::string& data_type = "ohlcv") const;
};

}  // namespace trade_ngin
