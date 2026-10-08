#include "trade_ngin/live/execution_price_resolver.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace trade_ngin {

namespace {

/** UTC YYYY-MM-DD, matching the date convention the equity path settled on. */
std::string format_utc_date(const Timestamp& ts) {
    std::time_t t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_utc, "%Y-%m-%d");
    return os.str();
}

}  // namespace

long ExecutionPriceResolver::staleness_days(const Timestamp& as_of, const Timestamp& observed) {
    if (observed >= as_of) return 0;
    auto gap = std::chrono::duration_cast<std::chrono::hours>(as_of - observed).count();
    return static_cast<long>(gap / 24);
}

std::unordered_map<std::string, ExecutionPriceResolver::ResolvedClose>
ExecutionPriceResolver::latest_close_at_or_before(const std::vector<Bar>& bars,
                                                  const Timestamp& as_of) {
    std::unordered_map<std::string, ResolvedClose> out;
    out.reserve(bars.size() / 4 + 1);

    for (const auto& bar : bars) {
        // Never reach forward into the session being generated.
        if (bar.timestamp > as_of) continue;

        double close = bar.close.as_double();
        // A non-positive close is not a price. Skip rather than record it, so a bad
        // row cannot become the "most recent" observation and mask a good older one.
        if (close <= 0.0) continue;

        auto it = out.find(bar.symbol);
        if (it == out.end()) {
            out.emplace(bar.symbol, ResolvedClose{close, bar.timestamp});
        } else if (bar.timestamp > it->second.as_of) {
            it->second.price = close;
            it->second.as_of = bar.timestamp;
        }
    }

    return out;
}

ExecutionPriceResolver::FillReport ExecutionPriceResolver::fill_missing(
    std::unordered_map<std::string, double>& prices,
    const std::unordered_map<std::string, ResolvedClose>& fallback,
    const std::set<std::string>& needed,
    const Timestamp& as_of,
    int max_staleness_days) {

    FillReport report;

    for (const auto& symbol : needed) {
        auto existing = prices.find(symbol);
        if (existing != prices.end() && existing->second > 0.0) {
            // A real T-1 close is already present: leave it exactly as it was.
            continue;
        }

        auto fb = fallback.find(symbol);
        if (fb == fallback.end() || fb->second.price <= 0.0) {
            report.unpriced.push_back(symbol);
            continue;
        }

        long stale = staleness_days(as_of, fb->second.as_of);
        if (stale > max_staleness_days) {
            report.unpriced.push_back(symbol);
            continue;
        }

        prices[symbol] = fb->second.price;
        report.widened.push_back(symbol + " @ " + format_utc_date(fb->second.as_of) + " (" +
                                 std::to_string(stale) + " days stale)");
    }

    std::sort(report.widened.begin(), report.widened.end());
    std::sort(report.unpriced.begin(), report.unpriced.end());
    return report;
}

}  // namespace trade_ngin
