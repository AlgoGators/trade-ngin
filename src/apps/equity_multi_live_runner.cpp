#include "trade_ngin/apps/equity_multi_live_runner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/live_config_selection.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/data_freshness.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"
#include "trade_ngin/live/equity_sleeve_netting.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

namespace trade_ngin::apps {
namespace {

constexpr double kAccountingTolerance = 1e-4;

bool finite(double value) {
    return std::isfinite(value);
}

template <typename T>
Result<T> refused(const std::string& message) {
    return make_error<T>(ErrorCode::INVALID_DATA, message,
                         "equity_multi_live_runner");
}

std::unordered_map<std::string, Position> combine_books(
    const std::map<std::string, std::unordered_map<std::string, Position>>& books) {
    std::unordered_map<std::string, Position> combined;
    for (const auto& [owner, rows] : books) {
        (void)owner;
        for (const auto& [symbol, row] : rows) {
            auto [it, inserted] = combined.emplace(symbol, row);
            if (!inserted) {
                const double old_quantity = it->second.quantity.as_double();
                const double add_quantity = row.quantity.as_double();
                const double next_quantity = old_quantity + add_quantity;
                if (std::abs(next_quantity) > 1e-12) {
                    it->second.average_price = Decimal(
                        (old_quantity * it->second.average_price.as_double() +
                         add_quantity * row.average_price.as_double()) /
                        next_quantity);
                } else {
                    it->second.average_price = Decimal(0.0);
                }
                it->second.quantity = Quantity(next_quantity);
                it->second.realized_pnl += row.realized_pnl;
                it->second.unrealized_pnl += row.unrealized_pnl;
            }
        }
    }
    return combined;
}

std::vector<Position> completed_rows(
    const std::unordered_map<std::string, Position>& book,
    const std::unordered_map<std::string, double>& marks, const Timestamp& now) {
    std::vector<Position> rows;
    rows.reserve(book.size());
    for (const auto& [symbol, source] : book) {
        Position row = source;
        row.symbol = symbol;
        row.last_update = now;
        const double quantity = row.quantity.as_double();
        if (std::abs(quantity) <= LiveDailyCycle::kRowTolerance) {
            row.quantity = Quantity(0.0);
            row.average_price = Decimal(0.0);
            row.unrealized_pnl = Decimal(0.0);
        } else {
            const auto mark = marks.find(symbol);
            if (mark == marks.end() || !(mark->second > 0.0)) {
                throw std::invalid_argument("multi_sleeve_position_mark_missing:" + symbol);
            }
            row.unrealized_pnl = Decimal(LivePnLManager::unrealized_from_cost_basis(
                quantity, row.average_price.as_double(), mark->second));
        }
        if (!LiveDailyCycle::is_dead_row(row)) rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const Position& left, const Position& right) {
                  return left.symbol < right.symbol;
              });
    return rows;
}

using RawCloseHistory =
    std::unordered_map<std::string, std::map<std::string, double>>;

double close_on(const RawCloseHistory& history, const std::string& symbol,
                const std::string& date) {
    const auto series = history.find(symbol);
    if (series == history.end()) return 0.0;
    const auto value = series->second.find(date);
    return value == series->second.end() ? 0.0 : value->second;
}

double first_close_on_or_after(const RawCloseHistory& history,
                               const std::string& symbol,
                               const std::string& date) {
    const auto series = history.find(symbol);
    if (series == history.end()) return 0.0;
    const auto value = series->second.lower_bound(date);
    return value == series->second.end() ? 0.0 : value->second;
}

double last_close_on_or_before(const RawCloseHistory& history,
                               const std::string& symbol,
                               const std::string& date) {
    const auto series = history.find(symbol);
    if (series == history.end()) return 0.0;
    auto value = series->second.upper_bound(date);
    if (value == series->second.begin()) return 0.0;
    --value;
    return value->second;
}

std::vector<TickerAlias> convert_aliases(
    const std::vector<PostgresDatabase::TickerAliasRow>& rows) {
    std::vector<TickerAlias> aliases;
    aliases.reserve(rows.size());
    for (const auto& row : rows) {
        aliases.push_back({row.historical_ticker, row.current_symbol,
                           row.effective_until, row.note});
    }
    return aliases;
}

struct PreviousCloseFrame {
    std::unordered_map<std::string, double> t1;
    std::unordered_map<std::string, double> t2;
};

PreviousCloseFrame previous_close_frame(
    const std::unordered_map<std::string, std::vector<Bar>>& bars_by_symbol,
    const std::string& previous_day) {
    PreviousCloseFrame frame;
    for (const auto& [symbol, bars] : bars_by_symbol) {
        std::map<std::string, double> closes;
        for (const auto& bar : bars) {
            const auto day = core::format_utc_date(bar.timestamp);
            if (day <= previous_day && bar.close.as_double() > 0.0)
                closes[day] = bar.close.as_double();
        }
        if (closes.empty()) continue;
        auto close = closes.rbegin();
        frame.t1[symbol] = close->second;
        ++close;
        if (close != closes.rend()) frame.t2[symbol] = close->second;
    }
    return frame;
}

Result<EquityOwnerCorporateActionOutput> apply_owner_actions_from_database(
    const AppConfig& config, const EquityLiveBookPlan& plan,
    const EquityLiveSleevePlan& sleeve,
    const std::shared_ptr<PostgresDatabase>& db,
    std::unordered_map<std::string, Position> positions,
    const std::vector<TickerAlias>& aliases,
    const std::unordered_map<std::string, std::string>& last_bar_date,
    const Timestamp& previous, const Timestamp& now) {
    const std::string source_day = core::format_utc_date(now);
    const std::string previous_day = core::format_utc_date(previous);
    std::vector<std::string> held;
    for (const auto& [symbol, position] : positions) {
        if (std::abs(position.quantity.as_double()) > LiveDailyCycle::kRowTolerance)
            held.push_back(symbol);
    }
    std::sort(held.begin(), held.end());
    if (held.empty()) {
        EquityOwnerCorporateActionOutput output;
        output.positions = std::move(positions);
        return Result<EquityOwnerCorporateActionOutput>(std::move(output));
    }

    CorporateActionsAuditLog audit(
        "state/" + plan.combined_strategy_id + "/" + sleeve.strategy_name,
        db, config.portfolio_id, plan.combined_strategy_id,
        sleeve.strategy_name);
    audit.set_run_date(source_day);
    auto loaded_audit = audit.load();
    if (loaded_audit.is_error())
        return refused<EquityOwnerCorporateActionOutput>(loaded_audit.error()->what());
    const std::string latest_applied = audit.latest_applied_ex_date();
    if (!latest_applied.empty() && latest_applied >= source_day)
        return refused<EquityOwnerCorporateActionOutput>(
            "owner_corporate_action_replay_state_ahead");

    auto inceptions = db->get_position_inception_dates(
        plan.combined_strategy_id, sleeve.strategy_name, config.portfolio_id,
        held, previous_day);
    if (inceptions.is_error())
        return refused<EquityOwnerCorporateActionOutput>(inceptions.error()->what());
    auto holding_starts = db->get_current_holding_start_dates(
        plan.combined_strategy_id, sleeve.strategy_name, config.portfolio_id,
        held, previous_day);
    if (holding_starts.is_error())
        return refused<EquityOwnerCorporateActionOutput>(holding_starts.error()->what());

    std::string window_start;
    for (const auto& symbol : held) {
        const auto inception = inceptions.value().find(symbol);
        const auto holding_start = holding_starts.value().find(symbol);
        if (inception == inceptions.value().end() || inception->second.empty() ||
            holding_start == holding_starts.value().end() ||
            holding_start->second.empty()) {
            return refused<EquityOwnerCorporateActionOutput>(
                "owner_corporate_action_history_incomplete:" + symbol);
        }
        if (window_start.empty() || inception->second < window_start)
            window_start = inception->second;
    }

    auto per_bar = db->get_per_bar_corporate_actions(
        held, window_start, source_day);
    auto raw_closes = db->get_historical_closes(held, window_start, source_day);
    auto last_buys = db->get_last_buy_dates(
        plan.combined_strategy_id, sleeve.strategy_name, config.portfolio_id,
        held, window_start, previous_day);
    auto delistings = db->get_delisting_dates(held, window_start);
    auto term_rows = db->get_corporate_actions(
        held, window_start, source_day,
        vendor_labels_for_class(CorpActionClass::TERMINATION));
    auto spin_rows = db->get_corporate_actions(
        held, window_start, source_day, {"spinoff"});
    auto feed_last = db->get_corp_action_feed_last_date(source_day);
    if (per_bar.is_error() || raw_closes.is_error() || last_buys.is_error() ||
        delistings.is_error() || term_rows.is_error() || spin_rows.is_error() ||
        feed_last.is_error()) {
        return refused<EquityOwnerCorporateActionOutput>(
            "owner_corporate_action_source_unavailable");
    }

    std::map<std::pair<std::string, std::string>, SpinoffBarColumns> bar_columns;
    for (const auto& row : per_bar.value()) {
        auto& columns = bar_columns[{row.ticker, row.date_str}];
        if (row.action == "split" || row.action == "adrratiosplit") {
            columns.has_split = true;
            columns.split_factor = row.value;
        } else if (row.action == "dividend") {
            columns.has_dividend = true;
            columns.dividend_cash = row.value;
            columns.close_at_ex_date =
                close_on(raw_closes.value(), row.ticker, row.date_str);
        }
    }

    std::map<std::pair<std::string, std::string>,
             std::vector<std::pair<std::string, double>>> spinoff_terms;
    std::set<std::string> child_symbols;
    for (const auto& row : spin_rows.value()) {
        if (row.contra_ticker.empty() || row.contra_ticker == "N/A" ||
            row.contra_ticker == row.ticker || !(row.value > 0.0) ||
            !finite(row.value)) {
            continue;
        }
        auto& children = spinoff_terms[{row.ticker, row.date_str}];
        const auto duplicate = std::find_if(
            children.begin(), children.end(), [&](const auto& child) {
                return child.first == row.contra_ticker;
            });
        if (duplicate == children.end())
            children.emplace_back(row.contra_ticker, row.value);
        child_symbols.insert(row.contra_ticker);
    }
    RawCloseHistory child_closes;
    if (!child_symbols.empty()) {
        const std::vector<std::string> children(
            child_symbols.begin(), child_symbols.end());
        auto loaded_children = db->get_historical_closes(
            children, window_start, source_day);
        if (loaded_children.is_error())
            return refused<EquityOwnerCorporateActionOutput>(
                loaded_children.error()->what());
        child_closes = std::move(loaded_children.value());
    }

    EquityOwnerCorporateActionInput input;
    input.positions = std::move(positions);
    input.aliases = aliases;
    input.holding_start_dates = holding_starts.value();
    input.as_of_date = source_day;
    input.termination_feed_last_date = feed_last.value();
    input.spinoff_child_policy =
        spinoff_child_policy_from_string(config.live.spinoff_child_policy);
    input.execution_time = now;

    std::set<std::pair<std::string, std::string>> routed_spinoffs;
    std::set<std::tuple<std::string, std::string, CorpActionType>> seen;
    for (const auto& row : per_bar.value()) {
        const auto key = std::make_pair(row.ticker, row.date_str);
        const auto terms = spinoff_terms.find(key);
        const auto columns = bar_columns.find(key);
        const bool spinoff =
            terms != spinoff_terms.end() && columns != bar_columns.end() &&
            columns->second.routes_a_spinoff();
        if (spinoff) {
            const bool reverse_split_row =
                (row.action == "split" || row.action == "adrratiosplit") &&
                columns->second.has_reverse_split();
            if (!reverse_split_row) {
                if (!routed_spinoffs.insert(key).second ||
                    audit.is_applied(row.ticker, row.date_str,
                                     CorpActionType::SPINOFF)) {
                    continue;
                }
                SpinoffEvent event;
                event.parent = row.ticker;
                event.ex_date = row.date_str;
                event.parent_restatement_factor =
                    columns->second.spinoff_factor();
                const bool split_applied =
                    audit.is_applied(row.ticker, row.date_str,
                                     CorpActionType::SPLIT) ||
                    audit.is_applied(row.ticker, row.date_str,
                                     CorpActionType::ADR_SPLIT);
                const auto retry = columns->second.retry_frame(split_applied);
                for (const auto& [child, ratio] : terms->second) {
                    event.children.push_back(
                        {child, ratio * retry.child_ratio_scale,
                         first_close_on_or_after(
                             child_closes, child, row.date_str)});
                }
                input.spinoffs.push_back(std::move(event));
                continue;
            }
        }

        CorpActionType type = CorporateActionsApplier::type_from_action_string(
            row.action);
        if (type == CorpActionType::UNKNOWN ||
            audit.is_applied(row.ticker, row.date_str, type) ||
            !seen.emplace(row.ticker, row.date_str, type).second) {
            continue;
        }
        const auto last = last_bar_date.find(row.ticker);
        if (last == last_bar_date.end() || row.date_str > last->second)
            continue;
        CorpActionEvent event;
        event.symbol = row.ticker;
        event.ex_date = row.date_str;
        event.type = type;
        event.value = row.value;
        const auto last_buy = last_buys.value().find(row.ticker);
        if (last_buy != last_buys.value().end() &&
            last_buy->second > row.date_str) {
            event.basis_provenance =
                CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE;
            event.basis_provenance_evidence = "BUY " + last_buy->second;
        } else {
            event.basis_provenance =
                CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;
        }
        if (type == CorpActionType::DIVIDEND) {
            event.close_at_ex_date =
                close_on(raw_closes.value(), row.ticker, row.date_str);
            if (!(event.close_at_ex_date > 0.0))
                return refused<EquityOwnerCorporateActionOutput>(
                    "owner_dividend_denominator_missing:" + row.ticker);
            Timestamp ex_date;
            if (core::parse_utc_date(row.date_str, ex_date)) {
                auto ex_positions = db->load_equity_system_positions_by_owner(
                    plan.combined_strategy_id, sleeve.strategy_name,
                    config.portfolio_id, ex_date - std::chrono::days(1));
                if (ex_positions.is_ok()) {
                    const auto held_at_ex = ex_positions.value().find(row.ticker);
                    if (held_at_ex != ex_positions.value().end())
                        event.qty_at_ex_date =
                            held_at_ex->second.quantity.as_double();
                }
            }
        }
        input.price_restatements.push_back(std::move(event));
    }

    std::map<std::pair<std::string, std::string>, TerminationEvent> terminations;
    for (const auto& [symbol, date] : delistings.value()) {
        const auto last = last_bar_date.find(symbol);
        if (last != last_bar_date.end() &&
            CorporateActionsLifecycle::delisting_is_stale(date, last->second)) {
            continue;
        }
        terminations[{symbol, date}] =
            {symbol, date, "delisted", "", 0.0, false};
    }
    for (const auto& row : term_rows.value()) {
        const auto last = last_bar_date.find(row.ticker);
        const std::string last_date =
            last == last_bar_date.end() ? "" : last->second;
        if (!CorporateActionsLifecycle::terms_row_terminates_its_ticker(
                row.action, row.date_str, last_date)) {
            continue;
        }
        terminations[{row.ticker, row.date_str}] =
            {row.ticker, row.date_str, row.action, row.contra_ticker,
             0.0, false};
    }
    for (const auto& [key, termination] : terminations) {
        if (audit.is_applied(termination.symbol, termination.event_date,
                             CorpActionType::TERMINATION)) {
            continue;
        }
        input.terminations.push_back(termination);
        const double final_close = last_close_on_or_before(
            raw_closes.value(), termination.symbol, termination.event_date);
        if (final_close > 0.0)
            input.final_closes[termination.symbol] = final_close;
    }

    auto applied = apply_equity_owner_corporate_actions(std::move(input));
    if (applied.is_error()) return applied;

    for (const auto& adjustment : applied.value().price_adjustments)
        audit.record(adjustment);
    for (const auto& adjustment : applied.value().lifecycle_adjustments) {
        if (adjustment.outcome == LifecycleOutcome::SPUN_OFF_CHILD_HELD ||
            adjustment.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD) {
            PositionAdjustment parent;
            parent.symbol = adjustment.symbol;
            parent.event_date = adjustment.event_date;
            parent.type = CorpActionType::SPINOFF;
            parent.quantity_before = adjustment.quantity_before;
            parent.quantity_after = adjustment.quantity_after;
            parent.avg_price_before = adjustment.avg_price_before;
            parent.avg_price_after = adjustment.avg_price_after;
            parent.event_value = adjustment.ratio_change;
            parent.ratio_change = adjustment.ratio_change;
            audit.record(parent);
            for (const auto& child : adjustment.children) {
                PositionAdjustment delivered;
                delivered.symbol = child.symbol;
                delivered.event_date = adjustment.event_date;
                delivered.type = CorpActionType::SPINOFF;
                delivered.quantity_after = child.quantity;
                delivered.avg_price_after = child.avg_price;
                delivered.event_value = child.first_close;
                delivered.ratio_change = 1.0;
                audit.record(delivered);
            }
        } else if (adjustment.outcome ==
                       LifecycleOutcome::EXITED_AT_FINAL_CLOSE ||
                   adjustment.outcome ==
                       LifecycleOutcome::CONVERTED_TO_CONTRA) {
            PositionAdjustment termination;
            termination.symbol = adjustment.symbol;
            termination.event_date = adjustment.event_date;
            termination.type = CorpActionType::TERMINATION;
            termination.quantity_before = adjustment.quantity_before;
            termination.quantity_after = adjustment.quantity_after;
            termination.event_value = adjustment.exit_price;
            termination.ratio_change = 1.0;
            audit.record(termination);
        }
    }
    if (!audit.save())
        return refused<EquityOwnerCorporateActionOutput>(
            "owner_corporate_action_audit_stage_failed");
    return applied;
}

}  // namespace

std::unordered_map<std::string, double>
EquityMultiLiveAccounting::live_result_metrics() const {
    return {
        {"total_cumulative_return", total_cumulative_return},
        {"total_annualized_return", total_annualized_return},
        {"total_pnl", total_pnl},
        {"total_unrealized_pnl", total_unrealized_pnl},
        {"total_realized_pnl", total_realized_pnl},
        {"current_portfolio_value", current_portfolio_value},
        {"net_leverage", net_leverage},
        {"portfolio_leverage", portfolio_leverage},
        {"gross_notional", gross_notional},
        {"net_notional", net_notional},
        {"daily_return", daily_return},
        {"daily_pnl", daily_pnl},
        {"total_transaction_costs", total_transaction_costs},
        {"daily_realized_pnl", daily_realized_pnl},
        {"daily_unrealized_pnl", daily_unrealized_pnl},
        {"daily_transaction_costs", daily_transaction_costs},
        {"margin_posted", margin_posted},
        {"cash_available", cash_available}};
}

Result<EquityMultiLiveAccounting> derive_equity_multi_live_accounting(
    const EquityMultiLivePriorAccounting& prior,
    const EquityMultiLiveDailyAccounting& daily) {
    const bool daily_valid =
        finite(daily.initial_capital) && daily.initial_capital > 0.0 &&
        finite(daily.daily_realized_pnl) && finite(daily.total_unrealized_pnl) &&
        finite(daily.daily_transaction_costs) &&
        daily.daily_transaction_costs >= 0.0 &&
        finite(daily.gross_notional) && daily.gross_notional >= 0.0 &&
        finite(daily.net_notional) && finite(daily.margin_posted) &&
        daily.margin_posted >= 0.0 && daily.trading_days > 0;
    if (!daily_valid) {
        return refused<EquityMultiLiveAccounting>(
            "multi_sleeve_daily_accounting_invalid");
    }

    double previous_equity = daily.initial_capital;
    double previous_realized = 0.0;
    double previous_unrealized = 0.0;
    double previous_costs = 0.0;
    if (prior.found) {
        const bool prior_valid =
            finite(prior.current_portfolio_value) && finite(prior.total_pnl) &&
            finite(prior.total_realized_pnl) &&
            finite(prior.total_unrealized_pnl) &&
            finite(prior.total_transaction_costs) &&
            prior.total_transaction_costs >= 0.0;
        const double derived_prior_total =
            prior.total_realized_pnl - prior.total_transaction_costs +
            prior.total_unrealized_pnl;
        if (!prior_valid ||
            std::abs(derived_prior_total - prior.total_pnl) >
                kAccountingTolerance ||
            std::abs(daily.initial_capital + prior.total_pnl -
                     prior.current_portfolio_value) > kAccountingTolerance) {
            return refused<EquityMultiLiveAccounting>(
                "multi_sleeve_prior_accounting_inconsistent");
        }
        previous_equity = prior.current_portfolio_value;
        previous_realized = prior.total_realized_pnl;
        previous_unrealized = prior.total_unrealized_pnl;
        previous_costs = prior.total_transaction_costs;
    }

    EquityMultiLiveAccounting result;
    result.daily_realized_pnl = daily.daily_realized_pnl;
    result.total_realized_pnl = previous_realized + daily.daily_realized_pnl;
    result.daily_unrealized_pnl =
        daily.total_unrealized_pnl - previous_unrealized;
    result.total_unrealized_pnl = daily.total_unrealized_pnl;
    result.daily_transaction_costs = daily.daily_transaction_costs;
    result.total_transaction_costs =
        previous_costs + daily.daily_transaction_costs;
    result.daily_pnl = result.daily_realized_pnl -
        result.daily_transaction_costs + result.daily_unrealized_pnl;
    result.total_pnl = result.total_realized_pnl -
        result.total_transaction_costs + result.total_unrealized_pnl;
    result.current_portfolio_value = daily.initial_capital + result.total_pnl;
    if (prior.found &&
        std::abs((result.current_portfolio_value - previous_equity) -
                 result.daily_pnl) > kAccountingTolerance) {
        return refused<EquityMultiLiveAccounting>(
            "multi_sleeve_equity_curve_discontinuous");
    }

    LiveMetricsCalculator calculator;
    result.daily_return =
        calculator.calculate_daily_return(result.daily_pnl, previous_equity);
    result.total_cumulative_return = calculator.calculate_total_return(
        result.current_portfolio_value, daily.initial_capital);
    result.total_annualized_return = calculator.calculate_annualized_return(
        result.total_pnl / daily.initial_capital, daily.trading_days);
    result.gross_notional = daily.gross_notional;
    result.net_notional = daily.net_notional;
    result.gross_leverage = result.current_portfolio_value != 0.0
        ? daily.gross_notional / result.current_portfolio_value
        : 0.0;
    result.net_leverage = result.current_portfolio_value != 0.0
        ? daily.net_notional / result.current_portfolio_value
        : 0.0;
    result.portfolio_leverage = result.gross_leverage;
    result.margin_posted = daily.margin_posted;
    result.cash_available =
        result.current_portfolio_value - daily.margin_posted;
    return Result<EquityMultiLiveAccounting>(std::move(result));
}

Result<EquityOwnerCorporateActionOutput> apply_equity_owner_corporate_actions(
    EquityOwnerCorporateActionInput input) {
    if (input.as_of_date.empty())
        return refused<EquityOwnerCorporateActionOutput>(
            "owner_corporate_action_as_of_date_required");

    EquityOwnerCorporateActionOutput output;
    output.positions = std::move(input.positions);

    const auto append_zero_cost_execution =
        [&](const std::string& order_id, const std::string& exec_id,
            const std::string& symbol, Side side, double quantity, double price) {
            if (!(quantity > 0.0) || !(price > 0.0) ||
                !finite(quantity) || !finite(price)) {
                return;
            }
            ExecutionReport execution;
            execution.order_id = order_id;
            execution.exec_id = exec_id;
            execution.symbol = symbol;
            execution.side = side;
            execution.filled_quantity = Quantity(quantity);
            execution.fill_price = Price(price);
            execution.fill_time = input.execution_time;
            execution.commissions_fees = Decimal(0.0);
            execution.implicit_price_impact = Decimal(0.0);
            execution.slippage_market_impact = Decimal(0.0);
            execution.total_transaction_costs = Decimal(0.0);
            execution.is_partial = false;
            output.evidence_executions.push_back(std::move(execution));
        };

    auto spinoff_log = CorporateActionsLifecycle::apply_spinoffs(
        output.positions, input.spinoffs, input.spinoff_child_policy);
    for (const auto& adjustment : spinoff_log) {
        if (adjustment.outcome != LifecycleOutcome::SPUN_OFF_CHILD_HELD &&
            adjustment.outcome != LifecycleOutcome::SPUN_OFF_CHILD_SOLD) {
            continue;
        }
        output.applied_class1_ex_date[adjustment.symbol] =
            adjustment.event_date;
        output.realized_by_symbol[adjustment.symbol] +=
            adjustment.realized_delta;
        for (const auto& child : adjustment.children) {
            const double received = child.quantity + child.fractional;
            append_zero_cost_execution(
                "CORPACTION_" + child.symbol + "_" + adjustment.event_date,
                "CA_" + child.symbol + "_" + adjustment.event_date + "_RECEIPT",
                child.symbol, Side::BUY, received, child.avg_price);
            const double disposed =
                adjustment.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD
                    ? received : child.fractional;
            append_zero_cost_execution(
                "CORPACTION_" + child.symbol + "_" + adjustment.event_date,
                "CA_" + child.symbol + "_" + adjustment.event_date +
                    (adjustment.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD
                         ? "_DISPOSAL" : "_CIL"),
                child.symbol, Side::SELL, disposed, child.first_close);
        }
    }
    output.lifecycle_adjustments.insert(
        output.lifecycle_adjustments.end(), spinoff_log.begin(), spinoff_log.end());

    output.price_adjustments = CorporateActionsApplier::apply(
        output.positions, input.price_restatements);
    for (const auto& adjustment : output.price_adjustments) {
        auto& date = output.applied_class1_ex_date[adjustment.symbol];
        if (date.empty() || adjustment.event_date > date)
            date = adjustment.event_date;
    }
    output.post_class1_positions = output.positions;

    auto rename_log = CorporateActionsLifecycle::apply_renames(
        output.positions, input.aliases, input.as_of_date,
        input.holding_start_dates);
    output.lifecycle_adjustments.insert(
        output.lifecycle_adjustments.end(), rename_log.begin(), rename_log.end());

    auto termination_log = CorporateActionsLifecycle::apply_terminations(
        output.positions, input.terminations, input.final_closes,
        input.termination_feed_last_date);
    for (const auto& adjustment : termination_log) {
        if (adjustment.outcome != LifecycleOutcome::EXITED_AT_FINAL_CLOSE)
            continue;
        output.realized_by_symbol[adjustment.symbol] +=
            adjustment.realized_delta;
        append_zero_cost_execution(
            "CORPACTION_" + adjustment.symbol + "_" + adjustment.event_date,
            "CA_" + adjustment.symbol + "_" + adjustment.event_date + "_EXIT",
            adjustment.symbol,
            adjustment.quantity_before > 0.0 ? Side::SELL : Side::BUY,
            std::abs(adjustment.quantity_before), adjustment.exit_price);
    }
    output.lifecycle_adjustments.insert(
        output.lifecycle_adjustments.end(), termination_log.begin(),
        termination_log.end());

    return Result<EquityOwnerCorporateActionOutput>(std::move(output));
}

Result<void> run_multi_sleeve_equity_live_day(
    const AppConfig& config, const EquityLiveBookPlan& plan,
    const std::shared_ptr<PostgresDatabase>& db, InstrumentRegistry& registry,
    const HolidayChecker& holidays, const Timestamp& now,
    const Timestamp& start_date, const Timestamp& end_date,
    bool historical_replay, const nlohmann::json& configuration_selection) {
    if (!db || plan.legacy_single || plan.sleeves.empty() ||
        plan.combined_strategy_id.empty() || config.portfolio_id.empty()) {
        return refused<void>("multi_sleeve_scope_invalid");
    }
    if (configuration_selection.is_null()) {
        auto selected = select_live_configuration(*db, config, TRADE_NGIN_GIT_SHA);
        if (selected.is_error()) return refused<void>(selected.error()->what());
        const auto entries = collect_enabled_equity_strategies(selected.value().config.strategies_config,"enabled_live");
        if (entries.is_error()) return refused<void>("multi_sleeve_source_config_invalid");
        const auto selected_plan = build_equity_live_book_plan(entries.value());
        if (selected_plan.is_error()) return refused<void>("multi_sleeve_source_plan_mismatch");
        return run_multi_sleeve_equity_live_day(selected.value().config, selected_plan.value(), db,
            registry, holidays, now, start_date, end_date, historical_replay, selected.value().receipt);
    }
    const auto source_entries=collect_enabled_equity_strategies(config.strategies_config,"enabled_live");
    if(source_entries.is_error())return refused<void>("multi_sleeve_source_config_invalid");
    const auto source_plan=build_equity_live_book_plan(source_entries.value());
    if(source_plan.is_error() || source_plan.value().combined_strategy_id!=plan.combined_strategy_id ||
        source_plan.value().sleeves.size()!=plan.sleeves.size())
        return refused<void>("multi_sleeve_source_plan_mismatch");
    for(size_t i=0;i<plan.sleeves.size();++i) {
        const auto& expected=source_plan.value().sleeves[i];const auto& actual=plan.sleeves[i];
        if(expected.source_id!=actual.source_id || expected.strategy_name!=actual.strategy_name ||
            expected.definition!=actual.definition || expected.allocation!=actual.allocation ||
            expected.symbols!=actual.symbols || expected.uses_database_symbol_fallback!=actual.uses_database_symbol_fallback)
            return refused<void>("multi_sleeve_source_plan_mismatch");
    }
    if(source_plan.value().symbols!=plan.symbols || source_plan.value().allow_fractional_shares!=plan.allow_fractional_shares)
        return refused<void>("multi_sleeve_source_plan_mismatch");
    const auto previous = holidays.find_previous_trading_day(now);
    if (!previous) return refused<void>("multi_sleeve_previous_session_missing");
    const std::string source_day = core::format_utc_date(now);
    const std::string previous_day = core::format_utc_date(*previous);
    if (!holidays.covers_date(source_day))
        return refused<void>("multi_sleeve_holiday_calendar_out_of_range");

    std::tm now_tm{};
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    gmtime_r(&now_time, &now_tm);
    const bool non_trading = LiveDailyCycle::is_non_trading_day(now_tm, holidays);

    nlohmann::json allocations = nlohmann::json::object();
    nlohmann::json strategy_configs = nlohmann::json::object();
    for (const auto& sleeve : plan.sleeves) {
        auto definition = sleeve.definition;
        definition["default_allocation"] = sleeve.allocation;
        allocations[sleeve.strategy_name] = sleeve.allocation;
        strategy_configs[sleeve.strategy_name] = definition.at("config");
    }
    auto snapshot = build_runtime_trading_snapshot(config);
    if (snapshot.is_error()) return refused<void>("multi_sleeve_snapshot_invalid");
    PublicationEvidenceToken evidence_token;
    auto begun = db->begin_live_publication(
        plan.combined_strategy_id, config.portfolio_id, now, snapshot.value(),
        true, TRADE_NGIN_GIT_SHA, PublicationEvidenceRequirement::RequiredFinalObservations,
        &evidence_token, PublicationPriorRequirement::None, configuration_selection);
    if (begun.is_error()) return refused<void>(begun.error()->what());
    if (begun.value()) return Result<void>();
    auto publication_guard = std::shared_ptr<void>(nullptr, [db](void*) {
        db->abandon_live_publication("multi_sleeve_run_abandoned");
    });

    std::map<std::string, std::unordered_map<std::string, Position>> prior_by_owner;
    std::set<std::string> symbol_set(plan.symbols.begin(), plan.symbols.end());
    bool database_fallback = false;
    for (const auto& sleeve : plan.sleeves) {
        database_fallback = database_fallback || sleeve.uses_database_symbol_fallback;
        auto loaded = db->load_equity_system_positions_by_owner(
            plan.combined_strategy_id, sleeve.strategy_name,
            config.portfolio_id, *previous);
        if (loaded.is_error()) return refused<void>(loaded.error()->what());
        prior_by_owner.emplace(sleeve.strategy_name, loaded.value());
        for (const auto& [symbol, row] : loaded.value()) {
            if (std::abs(row.quantity.as_double()) > LiveDailyCycle::kRowTolerance)
                symbol_set.insert(symbol);
        }
    }
    const auto pre_action_by_owner = prior_by_owner;
    if (database_fallback) {
        auto database_symbols = db->get_symbols(AssetClass::EQUITIES);
        if (database_symbols.is_error()) return refused<void>(database_symbols.error()->what());
        symbol_set.insert(database_symbols.value().begin(), database_symbols.value().end());
    }

    auto loaded_aliases = db->get_ticker_aliases();
    if (loaded_aliases.is_error())
        return refused<void>("multi_sleeve_ticker_aliases_unavailable");
    const auto aliases = convert_aliases(loaded_aliases.value());
    const auto rename_map = CorporateActionsLifecycle::build_rename_map(aliases);
    for (const auto& sleeve : plan.sleeves) {
        std::vector<std::string> held;
        for (const auto& [symbol, row] : prior_by_owner.at(sleeve.strategy_name)) {
            if (std::abs(row.quantity.as_double()) > LiveDailyCycle::kRowTolerance)
                held.push_back(symbol);
        }
        if (held.empty()) continue;
        auto holding_starts = db->get_current_holding_start_dates(
            plan.combined_strategy_id, sleeve.strategy_name,
            config.portfolio_id, held, previous_day);
        if (holding_starts.is_error())
            return refused<void>("multi_sleeve_holding_start_unavailable:" +
                                 sleeve.strategy_name);
        for (const auto& symbol : held) {
            const auto start = holding_starts.value().find(symbol);
            if (start == holding_starts.value().end() || start->second.empty())
                return refused<void>("multi_sleeve_holding_start_missing:" +
                                     sleeve.strategy_name + ":" + symbol);
            for (const auto& successor : CorporateActionsLifecycle::rename_chain(
                     rename_map, symbol, start->second, source_day)) {
                symbol_set.insert(successor);
            }
        }
    }
    std::vector<std::string> symbols(symbol_set.begin(), symbol_set.end());
    if (symbols.empty()) return refused<void>("multi_sleeve_universe_empty");

    auto market = db->get_market_data(symbols, start_date, end_date,
                                      AssetClass::EQUITIES,
                                      DataFrequency::DAILY, "ohlcv");
    if (market.is_error()) return refused<void>(market.error()->what());
    auto converted = DataConversionUtils::arrow_table_to_bars(market.value());
    if (converted.is_error()) return refused<void>(converted.error()->what());
    const auto all_bars = converted.value();
    if (all_bars.empty()) return refused<void>("multi_sleeve_market_data_empty");

    std::unordered_map<std::string, std::string> last_bar_date;
    std::unordered_map<std::string, double> marks;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    for (const auto& bar : all_bars) {
        bars_by_symbol[bar.symbol].push_back(bar);
        const auto day = core::format_utc_date(bar.timestamp);
        if (!last_bar_date.contains(bar.symbol) || day > last_bar_date.at(bar.symbol)) {
            last_bar_date[bar.symbol] = day;
            marks[bar.symbol] = bar.close.as_double();
        }
    }
    const auto freshness = assess_feed_freshness(last_bar_date,
        core::format_utc_date(end_date), symbols);
    if (!historical_replay &&
        (!freshness.any_data || freshness.absent > 0 ||
         freshness.days_behind > config.live.data_staleness_tolerance_days))
        return refused<void>("multi_sleeve_market_data_stale");

    std::map<std::string, std::vector<ExecutionReport>>
        corporate_executions_by_owner;
    std::map<std::string, std::unordered_map<std::string, double>>
        corporate_realized_by_owner;
    std::map<std::string, std::unordered_map<std::string, Position>>
        post_class1_by_owner;
    std::map<std::string, std::unordered_map<std::string, std::string>>
        applied_class1_by_owner;
    for (const auto& sleeve : plan.sleeves) {
        auto applied = apply_owner_actions_from_database(
            config, plan, sleeve, db,
            std::move(prior_by_owner.at(sleeve.strategy_name)), aliases,
            last_bar_date, *previous, now);
        if (applied.is_error()) return refused<void>(applied.error()->what());
        post_class1_by_owner[sleeve.strategy_name] =
            std::move(applied.value().post_class1_positions);
        applied_class1_by_owner[sleeve.strategy_name] =
            std::move(applied.value().applied_class1_ex_date);
        prior_by_owner[sleeve.strategy_name] =
            std::move(applied.value().positions);
        corporate_executions_by_owner[sleeve.strategy_name] =
            std::move(applied.value().evidence_executions);
        corporate_realized_by_owner[sleeve.strategy_name] =
            std::move(applied.value().realized_by_symbol);
        for (const auto& [symbol, position] :
             prior_by_owner.at(sleeve.strategy_name)) {
            if (std::abs(position.quantity.as_double()) >
                    LiveDailyCycle::kRowTolerance &&
                (!marks.contains(symbol) || !(marks.at(symbol) > 0.0))) {
                return refused<void>("multi_sleeve_corporate_action_mark_missing:" +
                                     sleeve.strategy_name + ":" + symbol);
            }
        }
    }

    auto loaded_accounting = db->get_live_accounting_context(
        plan.combined_strategy_id, config.portfolio_id, now, "system");
    if (loaded_accounting.is_error())
        return refused<void>(loaded_accounting.error()->what());
    const auto& loaded = loaded_accounting.value();
    EquityMultiLivePriorAccounting effective_prior_accounting;
    if (loaded.previous_found) {
        if (loaded.previous_source_day != previous_day)
            return refused<void>("multi_sleeve_previous_accounting_day_mismatch");
        const auto closes = previous_close_frame(bars_by_symbol, previous_day);
        std::vector<QtModelPositionBatch> finalized_batches;
        double finalized_unrealized = 0.0;
        double finalized_realized_rows = 0.0;
        for (const auto& sleeve : plan.sleeves) {
            const auto& owner = sleeve.strategy_name;
            std::unordered_map<std::string, Position> open;
            std::unordered_map<std::string, Position> closed;
            LiveDailyCycle::split_open_and_closed(
                pre_action_by_owner.at(owner), open, closed);
            auto finalization_book = LiveDailyCycle::select_finalization_book(
                open, post_class1_by_owner.at(owner),
                applied_class1_by_owner.at(owner), previous_day);
            std::vector<Position> finalized;
            if (!finalization_book.empty()) {
                LivePnLManager pnl(config.initial_capital, registry);
                pnl.set_asset_type(AssetType::EQUITY);
                auto result = pnl.finalize_previous_day(
                    finalization_book, closes.t1, closes.t2,
                    loaded.preceding_found
                        ? loaded.preceding_current_portfolio_value
                        : config.initial_capital,
                    loaded.previous_daily_transaction_costs,
                    LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
                if (result.is_error())
                    return refused<void>(result.error()->what());
                finalized = std::move(result.value().finalized_positions);
                LiveDailyCycle::restore_loaded_realized(finalized, open);
            }
            for (const auto& [symbol, row] : closed) finalized.push_back(row);
            std::erase_if(finalized, [](const Position& row) {
                return LiveDailyCycle::is_dead_row(row);
            });
            std::sort(finalized.begin(), finalized.end(),
                      [](const Position& left, const Position& right) {
                          return left.symbol < right.symbol;
                      });
            for (auto& row : finalized) {
                row.last_update = *previous;
                finalized_unrealized += row.unrealized_pnl.as_double();
                finalized_realized_rows += row.realized_pnl.as_double();
            }
            finalized_batches.push_back(
                {config.portfolio_id, plan.combined_strategy_id, owner,
                 previous_day, std::move(finalized)});
        }
        if (std::abs(finalized_realized_rows -
                     loaded.previous_daily_realized_pnl) >
            kAccountingTolerance) {
            return refused<void>(
                "multi_sleeve_previous_realized_rows_do_not_reconcile");
        }
        const EquityMultiLivePriorAccounting preceding{
            loaded.preceding_found,
            loaded.preceding_current_portfolio_value,
            loaded.preceding_total_pnl,
            loaded.preceding_total_realized_pnl,
            loaded.preceding_total_unrealized_pnl,
            loaded.preceding_total_transaction_costs};
        const EquityMultiLiveDailyAccounting previous_daily{
            config.initial_capital,
            loaded.previous_daily_realized_pnl,
            finalized_unrealized,
            loaded.previous_daily_transaction_costs,
            loaded.previous_gross_notional,
            loaded.previous_net_notional,
            loaded.previous_margin_posted,
            loaded.previous_trading_days};
        auto finalized_accounting = derive_equity_multi_live_accounting(
            preceding, previous_daily);
        if (finalized_accounting.is_error())
            return refused<void>(finalized_accounting.error()->what());
        const auto& finalized = finalized_accounting.value();
        EquityPreviousDayFinalization staged{
            config.portfolio_id,
            plan.combined_strategy_id,
            *previous,
            std::move(finalized_batches),
            finalized.live_result_metrics(),
            finalized.current_portfolio_value};
        auto staged_result =
            db->stage_equity_previous_day_finalization(staged);
        if (staged_result.is_error())
            return refused<void>(staged_result.error()->what());
        effective_prior_accounting = {
            true,
            finalized.current_portfolio_value,
            finalized.total_pnl,
            finalized.total_realized_pnl,
            finalized.total_unrealized_pnl,
            finalized.total_transaction_costs};
    }

    PortfolioConfig portfolio_config;
    portfolio_config.total_capital = Decimal(config.initial_capital);
    portfolio_config.reserve_capital =
        Decimal(config.initial_capital * config.reserve_capital_pct);
    portfolio_config.max_strategy_allocation =
        config.strategy_defaults.max_strategy_allocation;
    portfolio_config.min_strategy_allocation =
        config.strategy_defaults.min_strategy_allocation;
    portfolio_config.use_optimization = false;
    portfolio_config.covariance_history_prices = config.covariance_history_prices;
    portfolio_config.risk_modules = config.risk_schema.portfolio;
    portfolio_config.sleeve_risk_modules = config.risk_schema.sleeves;
    portfolio_config.use_risk_management = !config.risk_schema.is_none();
    portfolio_config.risk_config = config.risk_config;
    portfolio_config.risk_config.capital = Decimal(config.initial_capital);
    portfolio_config.allow_fractional_positions = plan.allow_fractional_shares;
    auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
    auto registry_ptr = std::shared_ptr<InstrumentRegistry>(
        &registry, [](InstrumentRegistry*) {});

    std::map<std::string, std::shared_ptr<MeanReversionStrategy>> strategies;
    for (const auto& sleeve : plan.sleeves) {
        StrategyConfig strategy_config;
        strategy_config.capital_allocation = config.initial_capital * sleeve.allocation;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.max_drawdown = config.max_drawdown;
        strategy_config.max_leverage = config.max_leverage;
        const auto& sleeve_symbols = sleeve.symbols.empty() ? symbols : sleeve.symbols;
        for (const auto& symbol : sleeve_symbols) {
            strategy_config.position_limits[symbol] =
                config.execution.position_limit_live;
            strategy_config.trading_params[symbol] = 1.0;
            strategy_config.costs[symbol] = config.execution.commission_rate;
        }
        auto strategy = std::make_shared<MeanReversionStrategy>(
            sleeve.strategy_name, strategy_config,
            build_mean_reversion_config(sleeve.definition.at("config")),
            db, registry_ptr);
        auto initialized = strategy->initialize();
        if (initialized.is_error()) return refused<void>(initialized.error()->what());
        auto started = strategy->start();
        if (started.is_error()) return refused<void>(started.error()->what());
        auto seeded = LiveDailyCycle::prepare_strategy_for_signals(
            *strategy, prior_by_owner.at(sleeve.strategy_name));
        if (seeded.is_error()) return refused<void>(seeded.error()->what());
        auto added = portfolio->add_strategy(
            strategy, sleeve.allocation, false,
            portfolio_config.use_risk_management);
        if (added.is_error()) return refused<void>(added.error()->what());
        strategies.emplace(sleeve.strategy_name, std::move(strategy));
    }

    PortfolioConsumptionTrace primary_consumption;
    std::map<std::string, std::unordered_map<std::string, Position>> target_by_owner;
    if (non_trading) {
        for (const auto& [owner, prior] : prior_by_owner)
            target_by_owner[owner] = LiveDailyCycle::carry_forward(prior);
    } else {
        auto processed = portfolio->process_market_data(all_bars,false,std::nullopt,&primary_consumption);
        if (processed.is_error()) return refused<void>(processed.error()->what());
        for (const auto& sleeve : plan.sleeves) {
            std::unordered_map<std::string, Timestamp> last_ingested;
            const auto& sleeve_symbols = sleeve.symbols.empty() ? symbols : sleeve.symbols;
            for (const auto& symbol : sleeve_symbols) {
                const auto* data = strategies.at(sleeve.strategy_name)
                                       ->get_instrument_data(symbol);
                if (data) last_ingested[symbol] = data->last_update;
            }
            const auto feed = LiveDailyCycle::verify_strategy_ingested_bars(
                sleeve_symbols, all_bars, last_ingested);
            if (!feed.not_ingested.empty() || feed.verified == 0)
                return refused<void>("multi_sleeve_strategy_feed_incomplete:" +
                                     sleeve.strategy_name);
        }
        const auto targets = portfolio->get_strategy_positions();
        for (const auto& sleeve : plan.sleeves) {
            const auto found = targets.find(sleeve.strategy_name);
            if (found == targets.end())
                return refused<void>("multi_sleeve_target_owner_missing");
            target_by_owner.emplace(sleeve.strategy_name, found->second);
        }
    }

    ExecutionManager execution_manager;
    execution_manager.get_transaction_cost_manager()
        .register_equity_costs_from_bars(symbols, bars_by_symbol);
    LiveDailyCycle::feed_cost_model(
        execution_manager.get_transaction_cost_manager(), symbols,
        bars_by_symbol);

    std::map<std::string, std::vector<ExecutionReport>> executions_by_owner =
        std::move(corporate_executions_by_owner);
    std::vector<ExecutionReport> account_executions;
    if (!non_trading) {
        std::vector<live::EquitySleevePositionBook> netting_books;
        for (const auto& sleeve : plan.sleeves) {
            live::EquitySleevePositionBook book;
            book.strategy_name = sleeve.strategy_name;
            for (const auto& [symbol, row] : prior_by_owner.at(sleeve.strategy_name))
                book.previous_quantities[symbol] = row.quantity.as_double();
            for (const auto& [symbol, row] : target_by_owner.at(sleeve.strategy_name))
                book.target_quantities[symbol] = row.quantity.as_double();
            for (const auto& [symbol, price] : marks)
                book.reference_prices[symbol] = price;
            netting_books.push_back(std::move(book));
        }
        auto netting = live::build_equity_sleeve_netting_plan(netting_books);
        if (netting.is_error()) return refused<void>(netting.error()->what());
        auto generated = live::generate_equity_sleeve_executions(
            execution_manager, netting.value(), now);
        if (generated.is_error()) return refused<void>(generated.error()->what());
        account_executions = generated.value().account_executions;
        for (auto& owned : generated.value().sleeve_executions) {
            auto strategy = strategies.find(owned.strategy_name);
            if (strategy == strategies.end())
                return refused<void>("multi_sleeve_execution_owner_missing");
            auto delivered = strategy->second->on_execution(owned.execution);
            if (delivered.is_error()) return refused<void>(delivered.error()->what());
            executions_by_owner[owned.strategy_name].push_back(
                std::move(owned.execution));
        }
    }

    std::map<std::string, std::unordered_map<std::string, Position>> final_by_owner;
    std::map<std::string, std::vector<Position>> rows_by_owner;
    double daily_realized = 0.0;
    double total_unrealized = 0.0;
    double daily_cost = 0.0;
    for (const auto& sleeve : plan.sleeves) {
        const auto& owner = sleeve.strategy_name;
        final_by_owner[owner] = non_trading
            ? target_by_owner.at(owner)
            : strategies.at(owner)->get_positions();
        for (const auto& [symbol, realized] : corporate_realized_by_owner[owner]) {
            auto row = final_by_owner[owner].find(symbol);
            if (row == final_by_owner[owner].end())
                return refused<void>("multi_sleeve_corporate_realized_row_missing:" +
                                     owner + ":" + symbol);
            row->second.realized_pnl = Decimal(
                row->second.realized_pnl.as_double() + realized);
        }
        rows_by_owner[owner] = completed_rows(final_by_owner.at(owner), marks, now);
        for (const auto& row : rows_by_owner.at(owner)) {
            if (row.quantity.as_double() < -LiveDailyCycle::kRowTolerance)
                return refused<void>("multi_sleeve_short_requires_borrow_accrual:" +
                                     owner + ":" + row.symbol);
            daily_realized += row.realized_pnl.as_double();
            total_unrealized += row.unrealized_pnl.as_double();
        }
        for (const auto& execution : executions_by_owner[owner])
            daily_cost += execution.net_transaction_costs().as_double();
    }

    for (const auto& sleeve : plan.sleeves) {
        const auto& owner = sleeve.strategy_name;
        const QtModelPositionBatch batch{
            config.portfolio_id, plan.combined_strategy_id, owner,
            source_day, rows_by_owner.at(owner)};
        auto stored = db->store_model_position_batch(batch);
        if (stored.is_error()) return refused<void>(stored.error()->what());
        if (!executions_by_owner[owner].empty()) {
            stored = db->store_executions(
                executions_by_owner[owner], plan.combined_strategy_id, owner,
                config.portfolio_id, "trading.executions", "system");
            if (stored.is_error()) return refused<void>(stored.error()->what());
        }
        if (!non_trading) {
            std::unordered_map<std::string, double> signals;
            const auto& sleeve_symbols = sleeve.symbols.empty() ? symbols : sleeve.symbols;
            for (const auto& symbol : sleeve_symbols)
                signals[symbol] = strategies.at(owner)->get_z_score(symbol);
            stored = db->store_signals(signals, plan.combined_strategy_id,
                                       owner, config.portfolio_id, now,
                                       "trading.signals");
            if (stored.is_error()) return refused<void>(stored.error()->what());
        }
    }

    const auto account_book = combine_books(final_by_owner);
    double gross_notional = 0.0;
    double net_notional = 0.0;
    int active_positions = 0;
    for (const auto& [symbol, row] : account_book) {
        const auto mark = marks.find(symbol);
        if (std::abs(row.quantity.as_double()) <= LiveDailyCycle::kRowTolerance) continue;
        if (mark == marks.end()) return refused<void>("multi_sleeve_account_mark_missing");
        const double notional = row.quantity.as_double() * mark->second;
        gross_notional += std::abs(notional);
        net_notional += notional;
        ++active_positions;
    }
    const EquityMultiLiveDailyAccounting daily_accounting{
        config.initial_capital,
        daily_realized,
        total_unrealized,
        daily_cost,
        gross_notional,
        net_notional,
        gross_notional,
        loaded.trading_days};
    auto derived_accounting = derive_equity_multi_live_accounting(
        effective_prior_accounting, daily_accounting);
    if (derived_accounting.is_error())
        return refused<void>(derived_accounting.error()->what());
    const auto& accounting = derived_accounting.value();

    const nlohmann::json result_config = {
        {"schema", "multi-sleeve-system/v1"},
        {"owners", allocations},
        {"account_netting", true},
        {"corporate_action_policy", "owner-scoped-class-1-2-3"}};
    const auto metrics = accounting.live_result_metrics();
    auto stored = db->delete_live_results(
        plan.combined_strategy_id, now, config.portfolio_id,
        "trading.live_results", "system");
    if (stored.is_error()) return refused<void>(stored.error()->what());
    stored = db->delete_live_equity_curve(
        plan.combined_strategy_id, now, config.portfolio_id,
        "trading.equity_curve", "system");
    if (stored.is_error()) return refused<void>(stored.error()->what());
    stored = db->store_live_results_complete(
        plan.combined_strategy_id, now, metrics,
        {{"active_positions", active_positions}}, result_config,
        config.portfolio_id);
    if (stored.is_error()) return refused<void>(stored.error()->what());
    stored = db->store_equity_trading_equity_curve(
        plan.combined_strategy_id, now, accounting.current_portfolio_value,
        config.portfolio_id);
    if (stored.is_error()) return refused<void>(stored.error()->what());

    nlohmann::json portfolio_metadata = {
        {"total_capital", config.initial_capital},
        {"reserve_capital", config.initial_capital * config.reserve_capital_pct},
        {"use_optimization", false},
        {"use_risk_management", portfolio_config.use_risk_management},
        {"allow_fractional_positions", plan.allow_fractional_shares},
        {"account_netting", true}};
    portfolio_metadata["config_inspection"]={{"capture_schema_version",3},
        {"profile","live_equity_multi_sleeve"}};
    stored = db->store_live_run_metadata(
        now, plan.combined_strategy_id, config.portfolio_id,
        allocations, portfolio_metadata, strategy_configs);
    if (stored.is_error()) return refused<void>(stored.error()->what());
    stored = db->store_risk_limits(
        plan.combined_strategy_id, config.portfolio_id,
        {{"max_gross_leverage", config.risk_config.max_gross_leverage},
         {"max_net_leverage", config.risk_config.max_net_leverage}});
    if (stored.is_error()) return refused<void>(stored.error()->what());
    auto inputs = build_run_inputs_row(
        TRADE_NGIN_GIT_SHA, snapshot.value(), symbols, all_bars,
        config.benchmark_mode, start_date, end_date);
    inputs["engine_flags"]["equity_multi_sleeve"] = true;
    inputs["engine_flags"]["account_netting"] = true;
    inputs["engine_flags"]["account_executions"] = nlohmann::json::array();
    for (const auto& execution : account_executions) {
        inputs["engine_flags"]["account_executions"].push_back({
            {"symbol", execution.symbol},
            {"side", execution.side == Side::BUY ? "BUY" : "SELL"},
            {"quantity_exact", execution.filled_quantity.to_string()},
            {"reference_price_exact", execution.fill_price.to_string()},
            {"total_transaction_costs_exact",
             execution.total_transaction_costs.to_string()}});
    }
    stored = db->store_live_run_inputs(
        plan.combined_strategy_id, config.portfolio_id, now, inputs);
    if (stored.is_error()) return refused<void>(stored.error()->what());

    if (db->live_publication_mode() != LivePublicationMode::SystemInvestor) {
        std::vector<std::string> owners;
        for (const auto& sleeve : plan.sleeves) owners.push_back(sleeve.strategy_name);
        stored = seed_qt_proposal_positions(
            *db, plan.combined_strategy_id, owners, config.portfolio_id, now);
        if (stored.is_error()) return refused<void>(stored.error()->what());
        stored = seed_qt_report_positions(
            *db, plan.combined_strategy_id, owners, config.portfolio_id, now);
        if (stored.is_error()) return refused<void>(stored.error()->what());
    }
    stored = db->attach_equity_multi_consumption(evidence_token,primary_consumption,non_trading);
    if (stored.is_error()) return refused<void>(stored.error()->what());
    stored = db->publish_live_publication();
    if (stored.is_error()) return refused<void>(stored.error()->what());
    publication_guard.reset();

    for (const auto& [owner, strategy] : strategies) {
        auto stopped = strategy->stop();
        if (stopped.is_error())
            WARN("Failed to stop multi-sleeve owner " + owner + ": " +
                 stopped.error()->what());
    }
    INFO("Published multi-sleeve system book " + plan.combined_strategy_id +
         " for " + config.portfolio_id + " on " + source_day);
    return Result<void>();
}

}  // namespace trade_ngin::apps
