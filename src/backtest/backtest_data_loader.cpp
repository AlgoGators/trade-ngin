#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/backtest/csv_bar_source.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace trade_ngin {
namespace backtest {

BacktestDataLoader::BacktestDataLoader(std::shared_ptr<PostgresDatabase> db)
    : db_(std::move(db)) {}

Result<std::vector<Bar>> BacktestDataLoader::load_market_data(const DataLoadConfig& config) {
    // Reject options upfront: data-ngin has zero options ingestion (no fetcher, no schema).
    // The options_data schema does not exist and will not for the foreseeable future.
    // This is the honest failure: loud and actionable, not a silent SQL error.
    if (config.asset_class == AssetClass::OPTIONS) {
        return make_error<std::vector<Bar>>(
            ErrorCode::MARKET_DATA_ERROR,
            "Options market data is not yet ingested by data-ngin. "
            "No fetcher, schema, or DAG exists for options. "
            "See data-ngin#39 for ingest roadmap.",
            "BacktestDataLoader");
    }

    const bool from_csv = (config.data_source == DataSource::CSV);
    if (from_csv) {
        if (config.csv_bar_dir.empty()) {
            return make_error<std::vector<Bar>>(
                ErrorCode::INVALID_ARGUMENT,
                "DataSource::CSV requires csv_bar_dir",
                "BacktestDataLoader");
        }
        if (config.asset_class != AssetClass::EQUITIES) {
            return make_error<std::vector<Bar>>(
                ErrorCode::INVALID_ARGUMENT,
                "DataSource::CSV carries the equity contract (raw OHLCV + div_cash + "
                "split_factor); only AssetClass::EQUITIES can read it",
                "BacktestDataLoader");
        }
        INFO("Backtest bar source: CSV (" + config.csv_bar_dir + ")");
    } else {
        // Ensure database connection
        auto conn_result = ensure_connection();
        if (conn_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conn_result.error()->code(),
                conn_result.error()->what(),
                "BacktestDataLoader");
        }
    }

    // Validate symbols list
    if (config.symbols.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::INVALID_ARGUMENT,
            "Empty symbols list provided for backtest",
            "BacktestDataLoader");
    }

    // Load market data in batches. Listing dates (the identity without
    // portfolio.json's listing_dates): a predecessor contract has no stored rows of its own, so it
    // is left out of the query and handed the rows stored under its listed contract's symbol below.
    std::vector<Bar> all_bars;
    size_t batch_size = config.batch_size > 0 ? config.batch_size : 5;
    const std::vector<std::string> stored_symbols =
        ListingDates::instance().stored_symbols(config.symbols);

    for (size_t i = 0; i < stored_symbols.size(); i += batch_size) {
        // Create a batch of symbols
        size_t end_idx = std::min(i + batch_size, stored_symbols.size());
        std::vector<std::string> symbol_batch(
            stored_symbols.begin() + i,
            stored_symbols.begin() + end_idx);

        // Load this batch from the configured source (never both)
        auto batch_result = from_csv ? load_symbol_batch_csv(symbol_batch, config)
                                     : load_symbol_batch(symbol_batch, config);
        if (batch_result.is_error()) {
            WARN("Error loading data for symbols batch " + std::to_string(i) + "-" +
                 std::to_string(end_idx) + ": " + batch_result.error()->what() +
                 ". Continuing with other batches.");
            continue;
        }

        auto& batch_bars = batch_result.value();
        all_bars.insert(all_bars.end(), batch_bars.begin(), batch_bars.end());
    }

    // Declared vendor relabellings (portfolio.json instrument_id_relabels; a no-op when absent) are
    // read first, so a predecessor's copy carries the same ids.
    for (const auto& line : ListingDates::instance().relabel_findings(all_bars)) WARN(line);
    ListingDates::instance().apply_relabels(all_bars);
    ListingDates::instance().add_predecessor_bars(config.symbols, all_bars);

    // Check for empty data
    if (all_bars.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::MARKET_DATA_ERROR,
            "No market data loaded for backtest",
            "BacktestDataLoader");
    }

    // Optional strictness (both off by default): a rotation book that silently loses
    // symbols or bars sizes them to zero with no error, which is worse than failing.
    if (config.require_all_symbols) {
        std::set<std::string> loaded;
        for (const auto& bar : all_bars) loaded.insert(bar.symbol);
        std::string missing;
        for (const auto& sym : config.symbols) {
            if (loaded.count(sym) == 0) missing += (missing.empty() ? "" : ", ") + sym;
        }
        if (!missing.empty()) {
            return make_error<std::vector<Bar>>(
                ErrorCode::MARKET_DATA_ERROR,
                "No market data for requested symbol(s): [" + missing +
                    "]. require_all_symbols is set; refusing to run on a partial universe.",
                "BacktestDataLoader");
        }
    }
    if (config.require_aligned_calendar) {
        auto alignment = validate_calendar_alignment(all_bars);
        if (alignment.is_error()) {
            return make_error<std::vector<Bar>>(
                alignment.error()->code(), alignment.error()->what(), "BacktestDataLoader");
        }
    }

    // Validate data quality
    auto validation_result = validate_data_quality(all_bars);
    if (validation_result.is_error()) {
        WARN(validation_result.error()->what());
        // Don't fail, just warn
    }

    INFO("Loaded a total of " + std::to_string(all_bars.size()) + " bars for " +
         std::to_string(config.symbols.size()) + " symbols");

    return Result<std::vector<Bar>>(all_bars);
}

std::map<Timestamp, std::vector<Bar>> BacktestDataLoader::group_bars_by_timestamp(
    const std::vector<Bar>& bars) const {
    std::map<Timestamp, std::vector<Bar>> grouped;

    for (const auto& bar : bars) {
        grouped[bar.timestamp].push_back(bar);
    }

    return grouped;
}

Result<void> BacktestDataLoader::validate_data_quality(const std::vector<Bar>& bars) const {
    if (bars.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "Empty bars vector provided for validation",
            "BacktestDataLoader");
    }

    // Check price movement for each symbol
    std::unordered_map<std::string, double> min_prices, max_prices;

    for (const auto& bar : bars) {
        double close = static_cast<double>(bar.close);
        if (min_prices.find(bar.symbol) == min_prices.end()) {
            min_prices[bar.symbol] = close;
            max_prices[bar.symbol] = close;
        } else {
            min_prices[bar.symbol] = std::min(min_prices[bar.symbol], close);
            max_prices[bar.symbol] = std::max(max_prices[bar.symbol], close);
        }
    }

    bool has_price_movement = false;
    for (const auto& [symbol, min_price] : min_prices) {
        double max_price = max_prices[symbol];
        double price_range_pct = min_price > 0 ?
            (max_price - min_price) / min_price * 100.0 : 0.0;

        if (price_range_pct > 1.0) {  // At least 1% price movement
            has_price_movement = true;
            break;
        }
    }

    if (!has_price_movement) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "No significant price movement detected in market data. "
            "Strategy may not generate signals.",
            "BacktestDataLoader");
    }

    return Result<void>();
}

std::vector<std::string> BacktestDataLoader::get_unique_symbols(const std::vector<Bar>& bars) const {
    std::set<std::string> symbol_set;
    for (const auto& bar : bars) {
        symbol_set.insert(bar.symbol);
    }
    return std::vector<std::string>(symbol_set.begin(), symbol_set.end());
}

std::pair<Timestamp, Timestamp> BacktestDataLoader::get_date_range(
    const std::vector<Bar>& bars) const {
    if (bars.empty()) {
        return {Timestamp{}, Timestamp{}};
    }

    auto [min_it, max_it] = std::minmax_element(bars.begin(), bars.end(),
        [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

    return {min_it->timestamp, max_it->timestamp};
}

std::unordered_map<std::string, double> BacktestDataLoader::get_price_statistics(
    const std::vector<Bar>& bars,
    const std::string& symbol) const {
    std::unordered_map<std::string, double> stats;
    stats["min_price"] = 0.0;
    stats["max_price"] = 0.0;
    stats["price_range_pct"] = 0.0;

    bool first = true;
    for (const auto& bar : bars) {
        if (bar.symbol != symbol) continue;

        double close = static_cast<double>(bar.close);
        if (first) {
            stats["min_price"] = close;
            stats["max_price"] = close;
            first = false;
        } else {
            stats["min_price"] = std::min(stats["min_price"], close);
            stats["max_price"] = std::max(stats["max_price"], close);
        }
    }

    if (stats["min_price"] > 0) {
        stats["price_range_pct"] =
            (stats["max_price"] - stats["min_price"]) / stats["min_price"] * 100.0;
    }

    return stats;
}

Result<void> BacktestDataLoader::validate_calendar_alignment(const std::vector<Bar>& bars) const {
    auto utc_day = [](const Timestamp& ts) {
        auto tt = std::chrono::system_clock::to_time_t(ts);
        std::tm tm{};
        core::safe_gmtime(&tt, &tm);
        return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    };
    auto day_str = [](int key) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", key / 10000, (key / 100) % 100,
                      key % 100);
        return std::string(buf);
    };

    // Per-symbol set of UTC days; duplicates are an error in their own right.
    std::map<std::string, std::set<int>> days_by_symbol;
    std::set<int> union_days;
    std::string problems;
    int problem_count = 0;
    auto note = [&](const std::string& what) {
        if (problem_count < 20) problems += (problems.empty() ? "" : "; ") + what;
        ++problem_count;
    };

    for (const auto& bar : bars) {
        int key = utc_day(bar.timestamp);
        if (!days_by_symbol[bar.symbol].insert(key).second) {
            note(bar.symbol + " has two bars on " + day_str(key));
        }
        union_days.insert(key);
    }

    for (const auto& [symbol, days] : days_by_symbol) {
        if (days.empty()) continue;
        const int first = *days.begin();
        const int last = *days.rbegin();
        for (auto it = union_days.lower_bound(first); it != union_days.end() && *it <= last; ++it) {
            if (days.count(*it) == 0) note(symbol + " missing " + day_str(*it));
        }
    }

    if (problem_count > 0) {
        return make_error<void>(
            ErrorCode::MARKET_DATA_ERROR,
            "Trading calendar is not aligned across symbols (" + std::to_string(problem_count) +
                " problem(s)): " + problems +
                (problem_count > 20 ? "; ..." : "") +
                ". require_aligned_calendar is set; refusing to run with stale marks.",
            "BacktestDataLoader");
    }
    return Result<void>();
}

Result<std::vector<Bar>> BacktestDataLoader::load_symbol_batch(
    const std::vector<std::string>& symbols,
    const DataLoadConfig& config) {
    try {
        auto result = db_->get_market_data(
            symbols, config.start_date, config.end_date,
            config.asset_class, config.data_freq, config.data_type);

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "BacktestDataLoader");
        }

        auto arrow_table = result.value();
        if (arrow_table->num_rows() == 0) {
            return make_error<std::vector<Bar>>(
                ErrorCode::DATA_NOT_FOUND,
                "Market data query returned an empty table",
                "BacktestDataLoader");
        }

        // Convert Arrow table to Bars
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(arrow_table);
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conversion_result.error()->code(),
                conversion_result.error()->what(),
                "BacktestDataLoader");
        }

        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Exception loading market data: ") + e.what(),
            "BacktestDataLoader");
    }
}

Result<std::vector<Bar>> BacktestDataLoader::load_symbol_batch_csv(
    const std::vector<std::string>& symbols,
    const DataLoadConfig& config) {
    std::vector<Bar> bars;
    for (const auto& symbol : symbols) {
        auto loaded = load_csv_bars(symbol, config.csv_bar_dir, config.start_date,
                                    config.end_date);
        if (loaded.is_error()) {
            // Match the database path: its one `symbol = ANY(...)` query leaves a symbol with
            // no rows in the window simply absent while its batch partners load, so a missing
            // file or an empty window (DATA_NOT_FOUND) skips this symbol only. Failing the
            // batch dropped healthy partners with it -- a late-inception DBMF took BIL's 2011
            // bars down and left BIL on the untiered cost default. require_all_symbols still
            // refuses the run on the absent symbol. A malformed file is a data-integrity
            // error and still fails the batch.
            if (loaded.error()->code() == ErrorCode::DATA_NOT_FOUND) {
                WARN("No CSV bars for " + symbol + " in the requested window: " +
                     std::string(loaded.error()->what()));
                continue;
            }
            return make_error<std::vector<Bar>>(loaded.error()->code(), loaded.error()->what(),
                                                "BacktestDataLoader");
        }
        auto& symbol_bars = loaded.value();
        bars.insert(bars.end(), symbol_bars.begin(), symbol_bars.end());
    }
    if (bars.empty()) {
        // The whole batch empty is what the database path reports as an error.
        return make_error<std::vector<Bar>>(ErrorCode::DATA_NOT_FOUND,
                                            "No CSV bars for any symbol in the batch",
                                            "BacktestDataLoader");
    }
    // Same ordering contract as the SQL path: ORDER BY time, symbol.
    std::stable_sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.symbol < b.symbol;
    });
    return Result<std::vector<Bar>>(std::move(bars));
}

Result<void> BacktestDataLoader::ensure_connection() {
    if (!db_) {
        return make_error<void>(
            ErrorCode::CONNECTION_ERROR,
            "Database interface is null",
            "BacktestDataLoader");
    }

    if (!db_->is_connected()) {
        auto connect_result = db_->connect();
        if (connect_result.is_error()) {
            return make_error<void>(
                connect_result.error()->code(),
                "Failed to connect to database: " + std::string(connect_result.error()->what()),
                "BacktestDataLoader");
        }
    }

    return Result<void>();
}

} // namespace backtest
} // namespace trade_ngin
