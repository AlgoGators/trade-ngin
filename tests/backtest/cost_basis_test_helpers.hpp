// tests/backtest/cost_basis_test_helpers.hpp
//
// T-7b-2 8c: a whole backtest (BacktestCoordinator::run_portfolio) on bars the test controls, so
// the cost of a stored fill can be checked against the inputs the backtest should have priced it
// on. The database serves the test's rows for whatever window and symbols it is asked for (the
// run's own load, and any other window a component reads) and the per-bar split events; a
// scheduled strategy trades on the dates the test names.

#pragma once

#include <arrow/api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace testing {
namespace cost_basis {

/// Trading day i (weekdays only) at 00:00Z counted from Monday 2025-01-06; negative i are the
/// weekdays before it.
inline Timestamp trading_day(int i) {
    constexpr int64_t kMonday = 1736121600LL;  // 2025-01-06 00:00:00Z
    int64_t day = 0;  // calendar days from kMonday
    if (i >= 0) {
        day = 7LL * (i / 5) + (i % 5);
    } else {
        const int back = -i;  // 1 = the previous Friday
        day = -7LL * ((back + 4) / 5) + (4 - ((back - 1) % 5));
    }
    return Timestamp(std::chrono::seconds(kMonday + 86400LL * day));
}

inline std::string ymd(int i) { return core::format_utc_date(trading_day(i)); }

struct Row {
    std::string symbol;
    int day;  ///< trading-day index
    double close;
    double volume;
    bool locked = false;  ///< high == low: the session classifier's JUNK
    int weekend_before = 0;  ///< 0: the bar is trading day `day`; 1 / 2: the Sunday / Saturday before it
};

/// A row's instant: trading day `day`, or the Sunday (1) or Saturday (2) before it.
inline Timestamp row_time(const Row& r) {
    return trading_day(r.day) - std::chrono::hours(24 * r.weekend_before);
}

inline std::shared_ptr<arrow::Table> to_table(const std::vector<Row>& rows) {
    auto* pool = arrow::default_memory_pool();
    auto schema = arrow::schema(
        {arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
         arrow::field("symbol", arrow::utf8()), arrow::field("open", arrow::float64()),
         arrow::field("high", arrow::float64()), arrow::field("low", arrow::float64()),
         arrow::field("close", arrow::float64()), arrow::field("volume", arrow::float64())});
    arrow::TimestampBuilder t(arrow::timestamp(arrow::TimeUnit::SECOND), pool);
    arrow::StringBuilder s(pool);
    arrow::DoubleBuilder o(pool), h(pool), l(pool), c(pool), v(pool);
    for (const auto& r : rows) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              row_time(r).time_since_epoch())
                              .count();
        ARROW_CHECK_OK(t.Append(secs));
        ARROW_CHECK_OK(s.Append(r.symbol));
        ARROW_CHECK_OK(o.Append(r.close));
        ARROW_CHECK_OK(h.Append(r.locked ? r.close : r.close * 1.01));
        ARROW_CHECK_OK(l.Append(r.locked ? r.close : r.close * 0.99));
        ARROW_CHECK_OK(c.Append(r.close));
        ARROW_CHECK_OK(v.Append(r.volume));
    }
    std::shared_ptr<arrow::Array> ta, sa, oa, ha, la, ca, va;
    ARROW_CHECK_OK(t.Finish(&ta));
    ARROW_CHECK_OK(s.Finish(&sa));
    ARROW_CHECK_OK(o.Finish(&oa));
    ARROW_CHECK_OK(h.Finish(&ha));
    ARROW_CHECK_OK(l.Finish(&la));
    ARROW_CHECK_OK(c.Finish(&ca));
    ARROW_CHECK_OK(v.Finish(&va));
    return arrow::Table::Make(schema, {ta, sa, oa, ha, la, ca, va});
}

/// Serves `rows` (time order) for any symbols and window it is asked for, and `splits` as the
/// per-bar corporate actions; records every call.
class ServingDb : public MockPostgresDatabase {
public:
    using MockPostgresDatabase::MockPostgresDatabase;

    std::vector<Row> rows;
    struct Split {
        std::string symbol;
        int day;
        double factor;
    };
    std::vector<Split> splits;

    struct MarketCall {
        Timestamp from;
        Timestamp to;
        AssetClass asset_class;
    };
    std::vector<MarketCall> market_calls;
    std::vector<std::pair<std::string, std::string>> corp_action_calls;

    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class, DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override {
        (void)freq;
        (void)data_type;
        market_calls.push_back({start_date, end_date, asset_class});
        std::vector<Row> out;
        for (const auto& r : rows) {
            const auto ts = row_time(r);
            if (ts < start_date || ts > end_date) continue;
            if (std::find(symbols.begin(), symbols.end(), r.symbol) == symbols.end()) continue;
            out.push_back(r);
        }
        std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
            return std::make_tuple(row_time(a), a.symbol) < std::make_tuple(row_time(b), b.symbol);
        });
        return Result<std::shared_ptr<arrow::Table>>(to_table(out));
    }

    Result<std::vector<CorpActionRow>> get_per_bar_corporate_actions(
        const std::vector<std::string>& tickers, const std::string& start_date,
        const std::string& end_date) override {
        corp_action_calls.emplace_back(start_date, end_date);
        std::vector<CorpActionRow> out;
        for (const auto& s : splits) {
            const std::string d = ymd(s.day);
            if (d < start_date || d > end_date) continue;
            if (std::find(tickers.begin(), tickers.end(), s.symbol) == tickers.end()) continue;
            CorpActionRow row;
            row.ticker = s.symbol;
            row.date_str = d;
            row.action = "split";
            row.value = s.factor;
            out.push_back(row);
        }
        return Result<std::vector<CorpActionRow>>(out);
    }
};

/// Holds, per symbol, the quantity scheduled for the latest signal bar it has been fed
/// (targets[symbol][day] applies from that trading day's signal bar on).
class ScheduledStrategy : public BaseStrategy {
public:
    ScheduledStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Scheduled Strategy";
    }

    std::map<std::string, std::map<int, double>> targets;
    /// the same by instant (a weekend bar's), applied after `targets` (T-7b-2 C8c3)
    std::map<std::string, std::map<Timestamp, double>> targets_at;

    Result<void> on_data(const std::vector<Bar>& data) override {
        for (const auto& b : data) {
            if (b.timestamp > last_signal_) last_signal_ = b.timestamp;
        }
        return Result<void>();
    }

    std::unordered_map<std::string, Position> get_target_positions() const override {
        std::unordered_map<std::string, Position> out;
        for (const auto& [symbol, by_day] : targets) {
            double qty = 0.0;
            for (const auto& [day, q] : by_day) {
                if (trading_day(day) <= last_signal_) qty = q;
            }
            if (auto at = targets_at.find(symbol); at != targets_at.end()) {
                for (const auto& [ts, q] : at->second) {
                    if (ts <= last_signal_) qty = q;
                }
            }
            Position p;
            p.symbol = symbol;
            p.quantity = Decimal(qty);
            p.average_price = Decimal(100.0);
            out[symbol] = p;
        }
        return out;
    }

private:
    Timestamp last_signal_{};
};

inline std::string temp_csv_dir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() / ("trade_ngin_cost_basis_" + tag);
    std::filesystem::create_directories(dir);
    return dir.string();
}

}  // namespace cost_basis
}  // namespace testing
}  // namespace trade_ngin
