// src/portfolio/portfolio_manager.cpp
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/optimization/one_pass_record.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/portfolio/allocation_split.hpp"
#include "trade_ngin/optimization/one_pass_log.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"
#include <unordered_set>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace trade_ngin {

PortfolioManager::PortfolioManager(PortfolioConfig config, std::string id,
                                   std::shared_ptr<InstrumentRegistry> registry)
    : config_(std::move(config)),
      id_(std::move(id)),
      registry_(std::move(registry)),
      instance_id_(id_),
      cost_manager_() {
    Logger::register_component("PortfolioManager");
    sizing_capital_ = config_.total_capital;  // T-7b-2 9c: until set_sizing_capital is called

    // The covariance history cap (portfolio.json "covariance_history_prices"). The loader
    // refuses a value below 2; a PortfolioConfig built in code is held to the same rule,
    // because a one-price series gives no return and the optimiser would silently drop
    // every symbol.
    if (config_.covariance_history_prices < 2) {
        throw std::invalid_argument(
            "PortfolioConfig.covariance_history_prices is " +
            std::to_string(config_.covariance_history_prices) +
            ": at least 2 prices are needed to compute a return");
    }

    // Initialize optimizer if enabled
    if (config_.use_optimization) {
        optimizer_ = std::make_unique<DynamicOptimizer>(config_.opt_config);
    }

    // Build the configured risk modules. An EMPTY list is not "no risk": it is a
    // PortfolioConfig somebody built in code and forgot a line of, and a book that runs
    // ungated because of a forgotten line is exactly what schema 2 exists to prevent. A
    // book that genuinely runs no risk layer says so with a single `none` assignment.
    if (config_.risk_modules.empty()) {
        throw std::invalid_argument(
            "PortfolioConfig.risk_modules is empty: assign a module, or {type: none} with "
            "_reason, _ruled_by, _ruled_on");
    }
    const bool no_risk_layer = config_.risk_modules.size() == 1 &&
                               config_.risk_modules.front().type == "none";
    // The Carver module builds its RiskManager (which registers "RiskManager") here, at
    // the point the manager was always built, so the initialized line below keeps its
    // [RiskManager] tag.
    // A module that cannot be BUILT is not a reason to run ungated. The catch here used to
    // log one ERROR and clear the list, eight lines below the comment explaining that an empty
    // list throws precisely so a forgotten line cannot leave a book unprotected: the same
    // failure, reached a different way, failed OPEN (T-6a ADVERSARIAL D-1). It rethrows now, as
    // the empty-list check does, so a book whose gate could not be built does not start.
    std::vector<RiskModulePtr> built_portfolio;
    if (!no_risk_layer) {
        try {
            for (const auto& module_config : config_.risk_modules) {
                auto module = make_risk_module(module_config, config_.risk_config.capital);
                if (module.is_error()) {
                    throw std::runtime_error(module.error()->what());
                }
                if (module.value()) built_portfolio.push_back(module.value());
            }
        } catch (const std::exception& e) {
            ERROR("Failed to initialize risk manager: " + std::string(e.what()));
            throw std::invalid_argument("Failed to initialize risk manager: " +
                                        std::string(e.what()));
        }
        if (built_portfolio.empty()) {
            WARN("Failed to create risk manager, risk management will be disabled");
        } else {
            // Kept HERE, at the point it has always been printed: before the sleeve modules are
            // built, so this commit reorders no line of any run's log.
            INFO("Risk manager initialized successfully with capital=" +
                 std::to_string(config_.risk_config.capital));
            Logger::register_component("PortfolioManager");
        }
    } else {
        INFO("Risk management is disabled in the configuration");
        // Who ruled that this book runs no risk layer, and when, once per PortfolioManager (one
        // per run on every runner). The generic line above names neither, while the stored
        // risk_scale keeps printing the reporter's value beside a book nothing cut (T-6b INTERIM
        // ADVERSARIAL D-1), so an operator reading the log could not tell a ruling from an
        // accident. The loader has already required all three fields of a `none` module.
        const RiskModuleConfig& none = config_.risk_modules.front();
        const auto* ruling = std::get_if<NoneModuleConfig>(&none.params);
        INFO("RISK_NONE pm=" + id_ + " module=" + none.id +
             " ruled_by=" + (ruling ? ruling->ruled_by : std::string("-")) +
             " ruled_on=" + (ruling ? ruling->ruled_on : std::string("-")) +
             ": this book runs no risk module; risk_scale in live_results is the reporter's "
             "reading, not a cut");
    }

    // Sleeve-scope modules. The loader checks every key against portfolio.json's `strategies`,
    // but that is not always the id the runner registers, so the key is checked again against
    // the registered strategies on the first process_market_data (validate_sleeve_keys_once).
    std::unordered_map<std::string, std::vector<RiskModulePtr>> built_sleeves;
    if (!config_.sleeve_risk_modules.empty()) {
        try {
            for (const auto& [strategy_id, module_configs] : config_.sleeve_risk_modules) {
                for (const auto& module_config : module_configs) {
                    auto module = make_risk_module(module_config, config_.risk_config.capital);
                    if (module.is_error()) {
                        throw std::runtime_error(module.error()->what());
                    }
                    if (module.value()) built_sleeves[strategy_id].push_back(module.value());
                }
            }
        } catch (const std::exception& e) {
            ERROR("Failed to initialize sleeve risk modules: " + std::string(e.what()));
            throw std::invalid_argument("Failed to initialize sleeve risk modules: " +
                                        std::string(e.what()));
        }
    }

    // The SAME rules set_risk_modules applies. The strategies are not registered yet, so the
    // sleeve-key rule is the one check deferred to the first process_market_data.
    {
        auto valid = validate_risk_modules(built_portfolio, built_sleeves, nullptr);
        if (valid.is_error()) {
            ERROR("Failed to initialize risk manager: " + std::string(valid.error()->what()));
            throw std::invalid_argument(valid.error()->what());
        }
    }
    risk_modules_ = std::move(built_portfolio);
    sleeve_risk_modules_ = std::move(built_sleeves);

    // Initialize with the provided ID
    ComponentInfo info{ComponentType::PORTFOLIO_MANAGER,
                       ComponentState::INITIALIZED,
                       id_,  // Use the provided ID
                       "",
                       std::chrono::system_clock::now(),
                       {{"total_capital", static_cast<double>(config_.total_capital)}}};

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        throw std::runtime_error(register_result.error()->what());
    }

    // Subscribe to market data and position updates
    MarketDataCallback callback = [this](const MarketDataEvent& event) {
        if (event.type == MarketDataEventType::POSITION_UPDATE) {
            // Handle position updates
            std::string strategy_id = event.string_fields.at("strategy_id");
            auto it = strategies_.find(strategy_id);
            if (it != strategies_.end()) {
                Position pos;
                pos.symbol = event.symbol;
                pos.quantity = event.numeric_fields.at("quantity");
                pos.average_price = event.numeric_fields.at("price");
                pos.last_update = event.timestamp;
                it->second.current_positions[event.symbol] = pos;
            }
        } else if (event.type == MarketDataEventType::BAR) {
            // Convert to Bar and process
            Bar bar;
            bar.timestamp = event.timestamp;
            bar.symbol = event.symbol;
            bar.open = event.numeric_fields.at("open");
            bar.high = event.numeric_fields.at("high");
            bar.low = event.numeric_fields.at("low");
            bar.close = event.numeric_fields.at("close");
            bar.volume = event.numeric_fields.at("volume");

            std::vector<Bar> bars{bar};
            auto result = this->process_market_data(bars);
            if (result.is_error()) {
                ERROR("Error processing market data: " + std::string(result.error()->what()));
            }
        }
    };

    SubscriberInfo sub_info{"PORTFOLIO_MANAGER",
                            {MarketDataEventType::BAR, MarketDataEventType::POSITION_UPDATE},
                            {},  // Subscribe to all symbols
                            callback};

    auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
    if (subscribe_result.is_error()) {
        throw std::runtime_error(subscribe_result.error()->what());
    }

    (void)StateManager::instance().update_state("PORTFOLIO_MANAGER", ComponentState::RUNNING);
}

Result<void> PortfolioManager::add_strategy(std::shared_ptr<StrategyInterface> strategy,
                                            double initial_allocation, bool use_optimization) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!strategy) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Strategy cannot be null",
                                "PortfolioManager");
    }

    const auto& metadata = strategy->get_metadata();

    if (strategies_.find(metadata.id) != strategies_.end()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Strategy with ID " + metadata.id + " already exists",
                                "PortfolioManager");
    }

    if (initial_allocation < config_.min_strategy_allocation ||
        initial_allocation > config_.max_strategy_allocation) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Initial allocation out of bounds",
                                "PortfolioManager");
    }

    // Validate total allocation doesn't exceed 1
    double total_allocation = initial_allocation;
    for (const auto& [_, info] : strategies_) {
        total_allocation += info.allocation;
    }

    if (total_allocation > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Total allocation would exceed 1.0",
                                "PortfolioManager");
    }

    // Create strategy info
    StrategyInfo info{
        strategy,
        initial_allocation,
        use_optimization && config_.use_optimization,
        {},  // current positions
        {}   // target positions
    };

    strategies_[metadata.id] = std::move(info);

    INFO("Added strategy " + metadata.id + " with allocation " +
         std::to_string(initial_allocation));

    return Result<void>();
}

namespace {

// The close of `symbol`'s latest-dated bar in `data`, 0.0 when it has none. Among bars of the same
// timestamp the first one wins, which is what the old first-match lookup returned whenever a call
// carried one date per symbol (every backtest cycle before T-7b-1 7a).
double latest_close_of(const std::vector<Bar>& data, const std::string& symbol) {
    const Bar* latest = nullptr;
    for (const auto& bar : data) {
        if (bar.symbol == symbol && (latest == nullptr || bar.timestamp > latest->timestamp)) {
            latest = &bar;
        }
    }
    return latest ? static_cast<double>(latest->close) : 0.0;
}

}  // namespace

Result<void> PortfolioManager::process_market_data(const std::vector<Bar>& data,
                                                   bool skip_execution_generation,
                                                   std::optional<Timestamp> current_timestamp,
                                                   const std::unordered_set<std::string>*
                                                       session_symbols) {
    std::vector<std::string> processed_strategies;

    try {
        std::unordered_map<std::string, Position> prev_portfolio_positions;
        // Per-strategy snapshot of the optimizer's prior-cycle output, consumed by the
        // chop-source attribution pass at end of cycle.
        std::unordered_map<std::string, std::unordered_map<std::string, Position>> prev_positions;
        // The caller's part of this rebalance's hold set (set_hold_set), this call's only.
        std::unordered_set<std::string> caller_holds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            caller_holds.swap(pending_hold_set_);
            pending_hold_set_.clear();
            one_pass_day_ = OnePassDay{};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Validate market data
            if (data.empty()) {
                ERROR("Empty market data provided");
                return make_error<void>(ErrorCode::MARKET_DATA_ERROR, "Empty market data provided",
                                        "PortfolioManager");
            }

            // Store current positions for each strategy to detect changes
            for (const auto& [id, info] : strategies_) {
                prev_positions[id] = info.current_positions;
            }

            // Snapshot of aggregated portfolio positions, used only for the
            // backward-compat portfolio-level execution diff below
            // (recent_executions_, no production consumer). Returns the
            // under-scaled view (Σ qᵢ × allocᵢ) — fine here because the
            // matching post_opt computed downstream uses the same scale, so
            // the diff is internally consistent.
            prev_portfolio_positions = get_positions_internal();

            // Process data through each strategy
            for (auto& [id, info] : strategies_) {
                Logger::register_component(info.strategy->get_metadata().name);
                if (!info.strategy) {
                    ERROR("Null strategy pointer found for ID: " + id);
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                            "Null strategy pointer found for ID: " + id,
                                            "PortfolioManager");
                }

                try {
                    // info.current_positions must persist as the optimizer's prior-cycle output
                    // (anchor for its cost penalty); do not overwrite with strategy positions here.
                    std::ostringstream oss;
                    for (auto& [sym, pos] : info.current_positions) {
                        oss << sym << ": " << pos.quantity << ", ";
                    }
                    DEBUG("Current positions for strategy " + id + ": " + oss.str());

                    // A strategy that is not RUNNING is a lifecycle state, not an
                    // ingest failure: it has been deliberately stopped or paused
                    // and simply does not take part in this cycle. Skip it
                    // BEFORE the refusal below, so that a stopped sleeve does not
                    // abort the whole portfolio -- and skip reading its targets
                    // too, which is the half that matters, because its target map
                    // is whatever it held when it stopped.
                    if (info.strategy->get_state() != StrategyState::RUNNING) {
                        WARN("Strategy " + id + " is not RUNNING; it takes no part in this "
                             "cycle and its target positions are not read");
                        // Not reading the targets is not enough: the map this manager keeps for
                        // the sleeve still holds the previous rebalance's targets, and everything
                        // after this loop in the same call reads it (the optimizer, the risk
                        // steps, the rounding, the book copy and execution generation). Empty it,
                        // so the stopped sleeve contributes no target (ledger row
                        // DA96324A-stopped-sleeve-map). Unreachable in every runner today: each
                        // starts its strategies before the first call and stops them after the
                        // last.
                        info.target_positions.clear();
                        continue;
                    }

                    // Process market data through strategy
                    auto result = info.strategy->on_data(data);
                    if (result.is_error()) {
                        // on_data-swallowed-futures. This used to log the error
                        // and carry straight on to get_target_positions() below.
                        //
                        // Reached only for a RUNNING strategy that failed to
                        // ingest the bars -- a genuine data failure, not the
                        // lifecycle case handled just above.
                        //
                        // A strategy that failed to ingest the bars returns
                        // targets computed from whatever it last saw -- the
                        // previous cycle's, or, in a process that starts with
                        // empty instrument data, ZERO for every symbol. Zero
                        // targets against a held book is not "no change": it is
                        // a full-book liquidation, generated by a strategy that
                        // never saw a price, and it would have been shipped with
                        // an ERROR line in the log and an exit code of 0.
                        //
                        // BA-17 / E2-F43 put an assertion around this on the
                        // equity runner (live_equity_mean_reversion.cpp, the
                        // FEED ASSERTION block) after the same reasoning. The
                        // futures runners had no equivalent, and this is the
                        // shared site both go through, so the refusal belongs
                        // here where it covers every caller.
                        //
                        // On a clean run on_data does not fail, so no run that
                        // completes today is affected.
                        ERROR("Error processing data for strategy " + id + ": " +
                              result.error()->what());
                        std::cerr << "Error processing data for strategy " << id << ": "
                                  << result.error()->what() << std::endl;
                        return make_error<void>(
                            result.error()->code(),
                            "Strategy " + id + " failed to process market data, so its target "
                            "positions would be stale or empty. Refusing to derive targets "
                            "from a strategy that did not see this cycle's prices: " +
                            std::string(result.error()->what()),
                            "PortfolioManager");
                    }

                    // Get target positions using polymorphic dispatch
                    // Each strategy type implements get_target_positions() appropriately:
                    // - BaseStrategy: returns get_positions() (standard positions map)
                    // - TrendFollowing/Fast/Slow: returns positions from instrument_data_
                    // This automatically handles all strategy types without type-checking
                    info.target_positions = info.strategy->get_target_positions();
                    DEBUG("Retrieved " + std::to_string(info.target_positions.size()) +
                          " target positions from strategy " + id);

                    processed_strategies.push_back(id);

                } catch (const std::exception& e) {
                    ERROR("Exception processing strategy " + id + ": " + std::string(e.what()));
                    continue;
                }
            }

            // PM-price-history. The history is updated AFTER every strategy has seen this
            // call's bars, from this call's bars, so the optimiser's newest bar is the bar
            // the strategy signals from. The futures runners' prewarm, which used to fill
            // the history before this call, is gone with it. Ruled class C (STAGE3_PLAN
            // §25a.2, §28b.3) and measured (T-4e). The PM keeps its own history from the
            // bars (T-6c commit B); no strategy's get_price_history() is read.
            update_historical_returns(data);
        }

        // Rebalance boundary (silent): the risk decisions recorded, the pinned strategies and
        // the applied factors are this call's only, and every risk module starts its rebalance
        // exactly once, before any lap (portfolio modules first, then each sleeve's).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            risk_decisions_.clear();
            pinned_scopes_.clear();
            rebalance_applied_.clear();
            delivered_lap1_book_.clear();
            delivered_has_lap1_ = false;
            delivered_cut_ = DeliveredCut{};
            delivered_npc_.clear();
        }
        // T-7b-2 CGW (the E-7 mechanism in the risk gate): the symbols the book can hold this
        // rebalance, which the Carver gate's window intersects its dates over. A symbol some strategy
        // lists and signals (StrategyInterface::is_signalling: a trend sleeve does not while the
        // symbol's price history is shorter than its longest EMA window), or one any strategy targets
        // or holds non-zero, so every book a lap can form maps into the window: the optimizer admits
        // only signalled symbols (E-7) and keeps every other symbol at its strategy's own target. A
        // symbol outside the set cannot be held today, so its missing dates no longer cut the dates
        // of every symbol that can.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            gate_participants_.clear();
            for (const auto& [sid, info] : strategies_) {
                for (const auto& [symbol, pos] : info.target_positions) {
                    if (std::abs(static_cast<double>(pos.quantity)) > 1e-12 ||
                        (info.strategy && info.strategy->is_signalling(symbol))) {
                        gate_participants_.insert(symbol);
                    }
                }
                auto prev = prev_positions.find(sid);
                if (prev == prev_positions.end()) continue;
                for (const auto& [symbol, pos] : prev->second) {
                    if (std::abs(static_cast<double>(pos.quantity)) > 1e-12) {
                        gate_participants_.insert(symbol);
                    }
                }
            }
        }
        {
            const RiskContext rebalance_ctx = make_risk_context(
                RiskPhase::REBALANCE_START, 0, RiskScope::PORTFOLIO, id_, sizing_capital_,
                data, current_timestamp, skip_execution_generation);
            for (auto& module : risk_modules_) {
                module->begin_rebalance(rebalance_ctx);
            }
            for (auto& [sid, modules] : sleeve_risk_modules_) {
                RiskContext sleeve_ctx = rebalance_ctx;
                sleeve_ctx.scope = RiskScope::SLEEVE;
                sleeve_ctx.scope_id = sid;
                for (auto& module : modules) {
                    module->begin_rebalance(sleeve_ctx);
                }
            }
        }

        // The loader checks every sleeve key against portfolio.json's `strategies`; the runner
        // does not always register the strategy under that key (the live equity runner registers
        // LIVE_EQUITY_MEAN_REVERSION for the config key MEAN_REVERSION), and a key that names no
        // registered strategy would simply never fire -- gating the backtest and not the live
        // run. Refuse instead, once, now that the strategies are known.
        {
            auto keys = validate_sleeve_keys_once();
            if (keys.is_error()) return keys;
        }

        // Invalidate covariance cache - will be recomputed once on first iteration and reused
        covariance_cache_valid_ = false;

        // Sleeve-scope risk: once, before the optimiser and the loop, each sleeve's own modules
        // on its own targets (silent and a no-op when no sleeve has modules).
        if (!sleeve_risk_modules_.empty()) {
            auto sleeve_result =
                apply_sleeve_risk(data, prev_positions, current_timestamp,
                                  skip_execution_generation);
            if (sleeve_result.is_error()) return sleeve_result;
        }

        // A book that names an overlay sleeve is rebalanced by the one pass (LOOP_SPEC sections 4
        // to 6): the overlay once on the capped target, one search from the held book, one
        // rounding, the trim, the fills. Nothing below this block changes its book.
        const bool one_pass = one_pass_book();
        if (!one_pass) {
            // A trend sleeve is rebalanced by the one pass and by nothing else. A book that holds
            // one and names no overlay sleeve (or names one that is not registered, or no tau)
            // would go through the generic step below on a placeholder weight and no cost:
            // refused, never optimised on defaults.
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [sid, info] : strategies_) {
                if (std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy)) {
                    return make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Portfolio " + id_ + " holds the trend sleeve " + sid +
                            " and names no overlay sleeve the one pass can run on (overlay_sleeve "
                            "'" + config_.overlay_sleeve + "', overlay_tau " +
                            std::to_string(config_.overlay_tau) +
                            "); a trend sleeve is not rebalanced by the generic optimiser step",
                        "PortfolioManager");
                }
            }
        }
        RiskLapOutcome risk_outcome;  // pin_all set by a portfolio-scope REFUSE / REPLACE
        if (one_pass) {
            auto passed = rebalance_one_pass(data, skip_execution_generation, current_timestamp,
                                             session_symbols, caller_holds, prev_positions);
            if (passed.is_error()) return passed;
        } else {
            // Every other book: the optimiser's step, then the portfolio risk step, each once.
            // The log lines of this path are the ones its single pass always wrote (the equity
            // book's logs do not move), which is why they still say "iteration 1".
            INFO("Iteration 1 of dynamic optimization + risk loop");
            if (config_.use_optimization && optimizer_) {
                try {
                    Logger::register_component("DynamicOptimizer");
                    auto opt_result = optimize_positions();
                    if (opt_result.is_error()) {
                        WARN("Portfolio optimization failed in iteration 1: " +
                             std::string(opt_result.error()->what()) +
                             ", continuing without optimization");
                    }
                } catch (const std::exception& e) {
                    WARN("Exception during portfolio optimization in iteration 1: " +
                         std::string(e.what()) + ", continuing without optimization");
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // The book the portfolio risk step reads (the delivered cut's denominator).
                for (const auto& [id, info] : strategies_) {
                    for (const auto& [sym, pos] : info.target_positions) {
                        delivered_lap1_book_[sym] += static_cast<double>(pos.quantity);
                    }
                }
                delivered_has_lap1_ = true;
            }

            if (!risk_modules_.empty()) {
                // A portfolio-scope risk step that cannot answer REFUSES the scope (HD 2026-09-21,
                // option b): every strategy is held at its previous book and no order is sent. A
                // module whose evaluate fails is refused inside apply_risk_management
                // (refuse_on_failed_gatekeeper); what fails the step itself (a module's on_bars
                // throwing, an exception after evaluate) is refused here, recorded as a REFUSE
                // row carrying the error, so the runner's metadata mark and exit code see it.
                const RiskContext lap_ctx =
                    make_risk_context(RiskPhase::LAP, 1, RiskScope::PORTFOLIO, id_, sizing_capital_,
                                      data, current_timestamp, skip_execution_generation);
                std::string step_failure;
                try {
                    Logger::register_component("RiskManager");
                    auto risk_result = apply_risk_management(data, lap_ctx, risk_outcome);
                    if (risk_result.is_error()) {
                        step_failure = risk_result.error()->what();
                    } else {
                        INFO("Portfolio risk management applied successfully in iteration 1");
                    }
                } catch (const std::exception& e) {
                    step_failure = e.what();
                }
                if (!step_failure.empty() && !risk_outcome.pin_all &&
                    !risk_outcome.refuse_unseeded) {
                    ERROR("Portfolio risk management failed in iteration 1: " + step_failure +
                          "; the portfolio risk step could not answer, so the scope is refused: "
                          "every strategy is held at its previous book and no orders are sent");
                    RiskDecision none;
                    none.module_id = kRiskStepModuleId;
                    record_risk_decision(lap_ctx, kRiskStepModuleId, std::move(none),
                                         RiskAction::REFUSE, Decimal(1.0), false, step_failure);
                    if (!scope_is_seeded(id_)) {
                        risk_outcome.refuse_unseeded = true;
                        risk_outcome.unseeded_scope = id_;
                    } else {
                        risk_outcome.pin_all = true;
                        risk_outcome.action = RiskAction::REFUSE;
                    }
                    risk_outcome.module_id = kRiskStepModuleId;
                }
            } else {
                INFO("Risk management not enabled, skipping risk checks in iteration 1");
            }

            // A portfolio-scope REFUSE (or REPLACE) pins every strategy. The strategies' own
            // targets and signals are untouched; process_market_data returns OK.
            if (risk_outcome.refuse_unseeded) {
                return make_error<void>(
                    ErrorCode::RISK_LIMIT_EXCEEDED,
                    "Risk module " + risk_outcome.module_id + " refused scope " +
                        risk_outcome.unseeded_scope +
                        ", whose previous book was never seeded; refusing the run rather than "
                        "shipping a flat book",
                    "PortfolioManager");
            }
            if (risk_outcome.pin_all) {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool distributable = strategies_.size() == 1;
                if (risk_outcome.action == RiskAction::REPLACE && !distributable) {
                    ERROR("Risk module " + risk_outcome.module_id +
                          " replaced the book of a portfolio of " +
                          std::to_string(strategies_.size()) +
                          " strategies, which cannot be distributed; every strategy is pinned to "
                          "its previous positions instead");
                }
                for (auto& [id, info] : strategies_) {
                    if (risk_outcome.action == RiskAction::REPLACE && distributable) {
                        info.target_positions = risk_outcome.replace_book;
                    } else {
                        info.target_positions = prev_positions[id];
                    }
                    pinned_scopes_.insert(id);
                }
                if (risk_outcome.action == RiskAction::REPLACE && distributable) {
                    INFO("Risk replacement: the strategy's targets replaced by risk module " +
                         risk_outcome.module_id + " after iteration 1; leaving the loop");
                } else {
                    INFO("Risk refusal: every strategy pinned to its previous positions after "
                         "iteration 1; leaving the loop");
                }
            }

            // A book of whole contracts stores whole contracts: after the one risk step every
            // quantity of an unpinned strategy is rounded to the nearest whole contract, once.
            if (risk_outcome.pin_all) {
                INFO("Final positions pinned by a risk " +
                     std::string(risk_outcome.action == RiskAction::REPLACE ? "replacement"
                                                                            : "refusal") +
                     " after 1 iterations; rounding skipped.");
            } else if (config_.allow_fractional_positions) {
                INFO("Fractional positions permitted; accepting iteration 1 output as final (risk "
                     "scale applied once). Converged!");
                INFO("Final positions fully integer after 1 iterations.");
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& [id, info] : strategies_) {
                    if (pinned_scopes_.count(id)) continue;  // pinned by a risk module
                    for (auto& [symbol, pos] : info.target_positions) {
                        pos.quantity =
                            static_cast<Decimal>(std::round(static_cast<double>(pos.quantity)));
                    }
                }
                INFO("Final positions fully integer after 1 iterations.");
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // CRITICAL FIX: Update current_positions with optimized/rounded target_positions
            // This ensures get_strategy_positions() returns integer positions, not fractional ones
            for (auto& [id, info] : strategies_) {
                info.current_positions = info.target_positions;
            }
        }

        // post_opt feeds the backward-compat portfolio-level executions loop
        // below (recent_executions_). It returns under-scaled aggregated
        // quantities (Σ qᵢ × allocᵢ); only used for diff-based execution
        // synthesis that no production consumer reads. Broker executions are
        // emitted per-strategy from info.target_positions.
        auto post_opt = get_portfolio_positions();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // DO NOT clear strategy_executions_ here - they need to accumulate across all periods
            // for saving at the end of the backtest. Each period's executions are appended.
            // Only clear at the very beginning of the backtest (handled elsewhere if needed)

            // Skip execution generation during warmup to prevent warmup executions from being
            // created
            if (!skip_execution_generation) {
                // Check if this is first post-warmup day for portfolio-level executions
                // (check BEFORE generating strategy executions)
                bool is_first_post_warmup_day_portfolio = true;
                for (const auto& [strategy_id, _] : strategies_) {
                    if (strategy_executions_[strategy_id].size() > 0) {
                        is_first_post_warmup_day_portfolio = false;
                        break;
                    }
                }
                bool should_generate_portfolio_establishment_execs =
                    is_first_post_warmup_day_portfolio;

                // K3 (T-7b-2 8b): where each sleeve's reports of THIS bar start, so the netting
                // below sees this bar's rows only and never re-nets an earlier bar's (backtest only,
                // see below).
                std::unordered_map<std::string, size_t> netting_bar_start;
                if (is_backtest_) {
                    for (const auto& [sid, sinfo] : strategies_) {
                        (void)sinfo;
                        netting_bar_start[sid] = strategy_executions_[sid].size();
                    }
                }

                // Generate execution reports per strategy (before aggregation)
                // This allows accurate per-strategy execution tracking. The one pass has already
                // generated its own fills (rebalance_one_pass).
                for (auto& [strategy_id, info] : strategies_) {
                    if (one_pass) break;
                    auto& strategy_execs = strategy_executions_[strategy_id];
                    // Start counter from current size to ensure unique IDs across all periods.
                    // T-ROLLX: the ROLL legs (RL-<sid>-<n>, inserted ahead of a bar's fills) are
                    // not counted, so the EX-<sid>-<n> sequence of the STRATEGY fills is unchanged.
                    int exec_counter = static_cast<int>(std::count_if(
                        strategy_execs.begin(), strategy_execs.end(), [](const ExecutionReport& e) {
                            return e.execution_type != ExecutionType::ROLL;
                        }));

                    INFO(
                        "Generating executions for strategy " + strategy_id +
                        ", target_positions size: " + std::to_string(info.target_positions.size()) +
                        ", existing executions: " + std::to_string(exec_counter));

                    // Per-strategy filled-position ledger: the net position this
                    // manager has actually traded into, accumulated from the
                    // executions it generates. Sizing the next order as
                    // (target - filled) is self-correcting -- a position that ever
                    // desyncs from its target (e.g. a target the warmup left
                    // unexecuted) gets traded back rather than stranded. Deliberately
                    // NOT sourced from strategy get_positions(): that is not a
                    // reliable actual-holdings record across strategy types
                    // (trend-following never maintains its positions_ map).
                    auto& strategy_filled = filled_positions_[strategy_id];
                    INFO("Filled-position ledger for strategy " + strategy_id +
                         " size: " + std::to_string(strategy_filled.size()));

                    // One fill of `symbol` from the ledger's `current_qty` to `new_qty` at
                    // `latest_price` (the signal group's close). The target loop and, with a
                    // session set, the close-out of a symbol absent from the target share it.
                    auto generate_fill = [&](const std::string& symbol, double current_qty,
                                             double new_qty, double latest_price) {
                        const double trade_size = new_qty - current_qty;
                        const Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                        // Create execution report for this strategy
                        ExecutionReport exec;
                        exec.order_id = "PM-" + strategy_id + "-" + std::to_string(exec_counter);
                        exec.exec_id = "EX-" + strategy_id + "-" + std::to_string(exec_counter);
                        exec.symbol = symbol;
                        exec.side = side;
                        exec.filled_quantity = std::abs(trade_size);
                        exec.fill_price = latest_price;
                        // CRITICAL FIX: Execution fill_time should use the CURRENT day's
                        // timestamp, not the previous day's bars timestamp. The 'data' parameter
                        // contains previous day's bars (for signal generation), but executions
                        // happen on the current day. Use current_timestamp if provided, otherwise
                        // fall back to data timestamp.
                        exec.fill_time = current_timestamp.has_value()
                                             ? current_timestamp.value()
                                             : (data.empty() ? std::chrono::system_clock::now()
                                                             : data[0].timestamp);
                        // Calculate transaction costs using TransactionCostManager.
                        // E2-F29: pass the SIGNED trade so the sell-side-only SEC/TAF gate
                        // (`quantity < 0`) is reachable; every other term takes |qty|.
                        auto cost_result =
                            cost_manager_.calculate_costs(symbol, trade_size, latest_price);
                        exec.commissions_fees = Decimal(cost_result.commissions_fees);
                        exec.implicit_price_impact = Decimal(cost_result.implicit_price_impact);
                        exec.slippage_market_impact = Decimal(cost_result.slippage_market_impact);
                        exec.total_transaction_costs = Decimal(cost_result.total_transaction_costs);
                        exec.is_partial = false;

                        // Add to strategy-specific executions
                        strategy_execs.push_back(exec);
                        exec_counter++;
                        // Ledger now reflects the position we just traded into.
                        strategy_filled[symbol] = new_qty;
                        INFO("Generated execution for strategy " + strategy_id + ": " + symbol +
                             " " + (side == Side::BUY ? "BUY" : "SELL") +
                             " qty=" + std::to_string(exec.filled_quantity));
                    };

                    // Generate executions based on individual strategy position changes
                    for (const auto& [symbol, new_pos] : info.target_positions) {
                        double current_qty = 0.0;
                        auto filled_it = strategy_filled.find(symbol);
                        if (filled_it != strategy_filled.end()) {
                            current_qty = filled_it->second;
                        }

                        double new_qty = static_cast<double>(new_pos.quantity);
                        double trade_size = new_qty - current_qty;

                        // Trade whenever the target differs from what has actually
                        // been filled. Establishing a position from flat is simply
                        // current_qty == 0 and needs no special case.
                        if (std::abs(trade_size) > 1e-6) {
                            // The symbol's LATEST-dated bar in this call (the signal group's
                            // close). T-7b-1 7a: on a release cycle the backtest feeds a withheld
                            // JUNK bar ahead of the symbol's new bar, so the first bar is not the
                            // latest; the first of equal dates is kept, as before.
                            const double latest_price = latest_close_of(data, symbol);

                            if (latest_price == 0.0) {
                                continue;  // Skip if price not available
                            }

                            generate_fill(symbol, current_qty, new_qty, latest_price);
                        }
                    }

                    INFO("Total executions generated for strategy " + strategy_id + ": " +
                         std::to_string(strategy_execs.size()));
                }

                // K3, the netting adjustment (T-7b-2 8b; HD 2026-09-25 item 23): this bar's
                // sleeve reports of one symbol are one account order, the signed sum Q; each
                // report keeps its own cost and gets its pro-rata share of sum C(q_i) - C(Q),
                // priced by this manager's cost model at the bar's price (C(0) = 0). A symbol one
                // sleeve trades gets 0. The equity curve still charges the reports' own costs.
                // Only in a backtest (set_backtest_mode, set by BacktestCoordinator::run_portfolio),
                // where these reports are the fills that get stored (T-7b-2 C8b4). A live runner's
                // pass is a fresh process whose filled ledger is empty, so its reports are each
                // sleeve's whole held book: no order, never stored; the runners net the rows they
                // store themselves, after PHASE 4.
                if (is_backtest_ && !one_pass) {
                    std::vector<transaction_cost::SleeveExecution> bar_rows;
                    for (auto& [sid, execs] : strategy_executions_) {
                        auto from = netting_bar_start.find(sid);
                        const size_t k0 = from == netting_bar_start.end() ? 0 : from->second;
                        for (size_t k = k0; k < execs.size(); ++k)
                            bar_rows.push_back({sid, &execs[k]});
                    }
                    const auto netting = transaction_cost::apply_netting_adjustments(
                        bar_rows, [this](const std::string& sym, double q, double px) {
                            return cost_manager_.calculate_costs(sym, q, px)
                                .total_transaction_costs;
                        });
                    for (const auto& line : netting.info_lines) INFO(line);
                    for (const auto& line : netting.warn_lines) WARN(line);
                }

                // Also generate portfolio-level executions (aggregated) for backward compatibility
                recent_executions_.clear();

                for (const auto& [symbol, new_pos] : post_opt) {
                    double current_qty = 0.0;

                    // Get previous portfolio position quantity for this symbol
                    auto prev_pos_it = prev_portfolio_positions.find(symbol);
                    if (prev_pos_it != prev_portfolio_positions.end()) {
                        current_qty = static_cast<double>(prev_pos_it->second.quantity);
                    }

                    double new_qty = static_cast<double>(new_pos.quantity);

                    // Generate execution if:
                    // 1. Position changed (normal case), OR
                    // 2. This is first post-warmup day and position is non-zero (establishment
                    // execution)
                    bool position_changed = (std::abs(new_qty - current_qty) > 1e-6);
                    bool is_establishment_exec =
                        should_generate_portfolio_establishment_execs && (std::abs(new_qty) > 1e-6);

                    if (position_changed || is_establishment_exec) {
                        // Calculate trade size
                        // For establishment executions, use the full new_qty (we're establishing
                        // the position) For normal changes, use the difference
                        double trade_size =
                            is_establishment_exec ? new_qty : (new_qty - current_qty);
                        Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                        // The symbol's LATEST-dated bar in this call (see above).
                        const double latest_price = latest_close_of(data, symbol);

                        if (latest_price == 0.0) {
                            continue;  // Skip if price not available
                        }

                        // Create execution report
                        ExecutionReport exec;
                        exec.order_id =
                            "PM-" + id_ + "-" + std::to_string(recent_executions_.size());
                        exec.exec_id =
                            "EX-" + id_ + "-" + std::to_string(recent_executions_.size());
                        exec.symbol = symbol;
                        exec.side = side;
                        exec.filled_quantity = std::abs(trade_size);
                        exec.fill_price = latest_price;
                        // CRITICAL FIX: Execution fill_time should use the CURRENT day's timestamp,
                        // not the previous day's bars timestamp. The 'data' parameter contains
                        // previous day's bars (for signal generation), but executions happen on
                        // the current day. Use current_timestamp if provided, otherwise fall back
                        // to data timestamp.
                        exec.fill_time = current_timestamp.has_value()
                                             ? current_timestamp.value()
                                             : (data.empty() ? std::chrono::system_clock::now()
                                                             : data[0].timestamp);
                        // Calculate transaction costs using TransactionCostManager.
                        // E2-F29: pass the SIGNED trade so the sell-side-only SEC/TAF gate
                        // (`quantity < 0`) is reachable; every other term takes |qty|.
                        auto cost_result =
                            cost_manager_.calculate_costs(symbol, trade_size, latest_price);
                        exec.commissions_fees = Decimal(cost_result.commissions_fees);
                        exec.implicit_price_impact = Decimal(cost_result.implicit_price_impact);
                        exec.slippage_market_impact = Decimal(cost_result.slippage_market_impact);
                        exec.total_transaction_costs = Decimal(cost_result.total_transaction_costs);
                        exec.is_partial = false;

                        // Add to recent executions (portfolio-level)
                        recent_executions_.push_back(exec);
                    }
                }
            }  // End of if (!skip_execution_generation) block
        }

        // T-7b-2 C9a (T-VOL C4): the delivered cut, measured once the book the runner stores is
        // final (current_positions, after the backtest's session hold above). Both books are
        // valued here, at one notional per contract per symbol. Log only: the runners print it.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::map<std::string, double> final_book;
            for (const auto& [id, info] : strategies_) {
                for (const auto& [sym, pos] : info.current_positions) {
                    final_book[sym] += static_cast<double>(pos.quantity);
                }
            }
            std::set<std::string> symbols;
            for (const auto& [sym, q] : delivered_lap1_book_) symbols.insert(sym);
            for (const auto& [sym, q] : final_book) symbols.insert(sym);
            delivered_npc_ = delivered_notional_per_contract(symbols);
            delivered_cut_ = measure_delivered_cut(
                delivered_has_lap1_ ? &delivered_lap1_book_ : nullptr, final_book, delivered_npc_);
        }
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing market data: ") + e.what(),
                                "PortfolioManager");
    }
}

std::vector<double> PortfolioManager::calculate_weights_per_contract(
    const std::vector<std::string>& symbols, double capital) const {
    std::vector<double> weights_per_contract(symbols.size(), 0.0);

    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];

        // Get contract multiplier/size for this instrument
        double contract_size = 1.0;  // Default
        double price = 1.0;          // Default
        double fx_rate = 1.0;        // Default exchange rate

        // Look up instrument details if available
        if (registry_ && registry_->has_instrument(symbol)) {
            auto instrument = registry_->get_instrument(symbol);
            contract_size = instrument->get_multiplier();

            // Look up the symbol's average_price for weight-per-contract
            // sizing. Reads price only — get_portfolio_positions() scales
            // quantity by allocation but leaves average_price untouched, so
            // safe here. Don't replicate this pattern for any field that
            // depends on quantity.
            const auto& portfolio_positions = get_portfolio_positions();
            auto pos_it = portfolio_positions.find(symbol);
            if (pos_it != portfolio_positions.end()) {
                price = static_cast<double>(pos_it->second.average_price);
            }
        }

        // Calculate weight per contract = (notional per contract) / capital
        double notional_per_contract = contract_size * price * fx_rate;
        weights_per_contract[i] = notional_per_contract / capital;

        // Ensure weight is positive and reasonable
        if (weights_per_contract[i] <= 0.0 || std::isnan(weights_per_contract[i])) {
            WARN("Invalid weight per contract for " + symbol + ", using default of 0.01");
            weights_per_contract[i] = 0.01;  // Reasonable default
        }
    }

    return weights_per_contract;
}

std::vector<double> PortfolioManager::calculate_trading_costs(
    const std::vector<std::string>& symbols, [[maybe_unused]] double capital) const {
    // The generic optimiser step's cost vector. Only a book that names no overlay sleeve reaches
    // it (a futures book is rebalanced by the one pass, which prices each contract through the
    // cost model itself), and such a book has no trend sleeve to read a contract size and price
    // from: every entry is zero, named once per symbol as it always was. The branch that read a
    // trend sleeve's instrument data was reached by no book after the one pass and is gone.
    std::vector<double> costs(symbols.size(), 0.0);
    for (const auto& symbol : symbols) {
        WARN("Symbol " + symbol + " not found in trading data, using zero cost");
    }
    return costs;
}

void PortfolioManager::update_historical_returns(const std::vector<Bar>& data) {
    if (data.empty())
        return;

    // Record this call's closes (T-6c commit B; T-BASE_ADVERSARIAL finding 4, option 4). The
    // PM keeps its own price history from the bars it is fed -- the same bars every strategy
    // receives in the loop above -- instead of borrowing the strategies' histories. It used to
    // copy each strategy's get_price_history() and, when two strategies offered different
    // series for one symbol, keep the first-registered one; the series then depended on which
    // sleeve was registered first and on what each sleeve chose to keep (a mean-reversion
    // sleeve trims to 40 prices, a fast trend sleeve never clears). Now no sleeve is read.
    //
    // Rules:
    //  * one close per symbol per DATE (the bar's UTC calendar day): a repeated date
    //    overwrites the earlier close, so a duplicate bar cannot lengthen a series. This is
    //    the second line of defence: the futures loader already returns one bar per symbol
    //    per date (T-6c commit B0), so an overwrite should never happen. When it does -- the
    //    same date twice in ONE call, or a later call bringing a DIFFERENT close for a stored
    //    date -- it is logged as PM_HISTORY_REPEATED_DATE. A later call re-feeding a stored
    //    date with the same close (the BASE and equity runners' bar replay followed by their
    //    final feed) is the normal case and silent;
    //  * at most config_.covariance_history_prices dates per symbol, the oldest date dropped
    //    first (756 by default, the trend sleeve's own cap);
    //  * a symbol that leaves the feed keeps its series, which simply stops growing; its
    //    returns are still computed below.
    // The returns below are computed from the kept closes in date order exactly as before (the
    // 2,520-return cap); they decide which symbols have enough history for the optimizer. The
    // covariance itself is built from the closes aligned by DATE (date_aligned_returns, T-7a
    // INSERT S3): a symbol that has left the feed ends the dates every symbol shares.
    const size_t max_prices = config_.covariance_history_prices;
    std::set<std::string> touched;
    std::set<std::pair<std::string, int64_t>> seen_this_call;
    for (const auto& bar : data) {
        if (bar.symbol.empty())
            continue;
        const auto day_point = std::chrono::floor<std::chrono::days>(bar.timestamp);
        const int64_t day = day_point.time_since_epoch().count();
        const double close = static_cast<double>(bar.close);
        auto& series = closes_by_date_[bar.symbol];
        auto stored = series.find(day);
        const bool repeated_in_call = !seen_this_call.emplace(bar.symbol, day).second;
        if (stored != series.end() && (repeated_in_call || stored->second != close)) {
            const std::chrono::year_month_day ymd{day_point};
            char date_buf[16];
            std::snprintf(date_buf, sizeof(date_buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                          static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
            char close_buf[64];
            std::snprintf(close_buf, sizeof(close_buf), "old_close=%.10g new_close=%.10g",
                          stored->second, close);
            WARN("PM_HISTORY_REPEATED_DATE symbol=" + bar.symbol + " date=" + date_buf + " " +
                 close_buf + " in_call=" + (repeated_in_call ? "1" : "0") +
                 ": a stored date's close was overwritten");
        }
        series[day] = close;
        ids_by_date_[bar.symbol][day] = bar.instrument_id;  // T-ROLLX: the kept bar's contract id
        touched.insert(bar.symbol);
    }
    for (const auto& symbol : touched) {
        auto& series = closes_by_date_.at(symbol);
        auto& ids = ids_by_date_[symbol];
        while (series.size() > max_prices) {
            ids.erase(series.begin()->first);
            series.erase(series.begin());
        }
    }

    // Now calculate returns for each symbol that has price history
    for (const auto& [symbol, series] : closes_by_date_) {
        std::vector<double> prices;
        prices.reserve(series.size());
        for (const auto& [day, close] : series) {
            prices.push_back(close);
        }
        // T-ROLLX (LOOP_SPEC v6.1 section 2.3): a bar whose contract id differs from the previous
        // stored bar's is a change bar; its return is 0 (the splice step is not a return).
        std::vector<std::string> ids;
        ids.reserve(series.size());
        if (auto id_series = ids_by_date_.find(symbol); id_series != ids_by_date_.end()) {
            for (const auto& [day, close] : series) {
                auto id = id_series->second.find(day);
                ids.push_back(id == id_series->second.end() ? std::string() : id->second);
            }
        } else {
            ids.assign(series.size(), std::string());
        }
        const roll_series::ChangeFlags flags = roll_series::classify_instrument_changes(ids);

        // Clear previous returns for this symbol BEFORE the two-price guard, so a symbol
        // whose history dropped below two prices loses its stale returns instead of keeping
        // those of an older, longer series. find(), not operator[]: a symbol that never had
        // returns gains no empty entry here, which would change the symbol count logged
        // below (a symbol that does reach the calculation still gets its entry from it).
        auto previous = historical_returns_.find(symbol);
        if (previous != historical_returns_.end()) {
            previous->second.clear();
        }

        // Need at least 2 prices to calculate a return
        if (prices.size() < 2) {
            DEBUG("Symbol " + symbol + " has only " + std::to_string(prices.size()) +
                  " prices, skipping return calculation");
            continue;
        }

        // Calculate returns - use all available history
        for (size_t i = 1; i < prices.size(); ++i) {
            double prev_price = prices[i - 1];
            double curr_price = prices[i];

            if (prev_price <= 0.0)
                continue;

            double ret = flags.change[i] ? 0.0 : (curr_price - prev_price) / prev_price;

            if (std::isfinite(ret)) {
                historical_returns_[symbol].push_back(ret);
            }
        }

        DEBUG("Calculated " + std::to_string(historical_returns_[symbol].size()) +
              " returns for symbol " + symbol + " from " + std::to_string(prices.size()) +
              " prices");

        // Limit history length if needed
        if (historical_returns_[symbol].size() > max_history_length_) {
            // Keep only the most recent returns
            size_t excess = historical_returns_[symbol].size() - max_history_length_;
            historical_returns_[symbol].erase(historical_returns_[symbol].begin(),
                                              historical_returns_[symbol].begin() + excess);
        }
    }

    // Log aggregate stats
    size_t total_returns = 0;
    for (const auto& [symbol, returns] : historical_returns_) {
        total_returns += returns.size();
    }

    INFO("Total historical data: " + std::to_string(total_returns) + " returns across " +
         std::to_string(historical_returns_.size()) + " symbols");
}

namespace {

// "YYYY-MM-DD" of a closes_by_date_ key (days since the epoch of the bar's in-process instant).
std::string covariance_day_label(int64_t day) {
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{day}}};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return buf;
}

// The returns calculate_covariance_matrix needs before it falls back to the 0.01 diagonal; T-7b-1
// 7d applies the same number to the date intersection (date_aligned_returns' floor).
constexpr size_t kCovarianceMinReturns = 20;

// max |rho| over the pairs (i < j) of `symbols` in `cov` whose two legs are both in `held` (every
// pair when `held` is null); the pair as "A/B" in `pair`. A pair with a non-positive variance or a
// non-finite correlation is skipped and |rho| is clamped to 1, as the Carver gate's
// calculate_correlation_multiplier does. -1 when there is no pair.
double covariance_max_abs_rho(const std::vector<std::string>& symbols,
                              const std::vector<std::vector<double>>& cov,
                              const std::set<std::string>* held, std::string& pair) {
    double best = -1.0;
    pair = "-";
    if (cov.size() != symbols.size()) return best;
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (held && !held->count(symbols[i])) continue;
        for (size_t j = i + 1; j < symbols.size(); ++j) {
            if (held && !held->count(symbols[j])) continue;
            const double vi = cov[i][i];
            const double vj = cov[j][j];
            if (!(vi > 0.0) || !(vj > 0.0)) continue;
            const double r = cov[i][j] / std::sqrt(vi * vj);
            if (!std::isfinite(r)) continue;
            const double a = std::min(1.0, std::abs(r));
            if (a > best) {
                best = a;
                pair = symbols[i] + "/" + symbols[j];
            }
        }
    }
    return best;
}

std::string covariance_rho_text(double v) { return v < 0.0 ? std::string("-") : std::to_string(v); }

}  // namespace

std::unordered_map<std::string, std::vector<double>> PortfolioManager::date_aligned_returns(
    const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol) const {
    return date_aligned_returns(closes_by_symbol, {});
}

std::unordered_map<std::string, std::map<int64_t, double>> PortfolioManager::adjusted_closes_by_symbol(
    const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol) const {
    // T-ROLLX (LOOP_SPEC v6.1 section 2.3): the adjusted level of each stored close, from the closes
    // and the contract ids recorded together in update_historical_returns, in date order, anchored
    // on the symbol's latest stored close. A symbol with no recorded ids reads its raw closes.
    std::unordered_map<std::string, std::map<int64_t, double>> out;
    for (const auto& [symbol, closes] : closes_by_symbol) {
        auto id_series = ids_by_date_.find(symbol);
        if (id_series == ids_by_date_.end() || closes.empty()) continue;
        std::vector<int64_t> days;
        std::vector<double> raw;
        std::vector<std::string> ids;
        days.reserve(closes.size());
        raw.reserve(closes.size());
        ids.reserve(closes.size());
        for (const auto& [day, close] : closes) {
            days.push_back(day);
            raw.push_back(close);
            auto id = id_series->second.find(day);
            ids.push_back(id == id_series->second.end() ? std::string() : id->second);
        }
        const roll_series::ChangeFlags flags = roll_series::classify_instrument_changes(ids);
        const std::vector<double> adjusted = roll_series::adjusted_levels(raw, flags.change);
        auto& levels = out[symbol];
        for (size_t i = 0; i < days.size(); ++i) levels[days[i]] = adjusted[i];
    }
    return out;
}

std::unordered_map<std::string, std::vector<double>> PortfolioManager::date_aligned_returns(
    const std::unordered_map<std::string, std::map<int64_t, double>>& closes_by_symbol,
    const std::unordered_map<std::string, std::map<int64_t, double>>& adjusted_by_symbol) const {
    // T-7a INSERT S3 (ledger PM-covariance-count-aligned). The covariance used to pair each
    // symbol's k-th-last return with every other symbol's k-th-last return: by COUNT. A symbol
    // whose date set differs (a feed gap, a Sunday-stamped bar, MBT's weekend bars from
    // 2026-06-13) was then paired with other days. Measured on 2026-05-01: 475 of the 480 MBT/MES
    // rows paired different dates. The series are now aligned by DATE:
    //
    //  * a symbol's usable dates are those whose close is finite and above zero;
    //  * D is the INTERSECTION of the usable dates of every symbol that has at least two of them
    //    (a symbol missing a date contributes no return that day, and the day is dropped for all);
    //  * every such symbol's series is r_t = (c(D[t]) - c(D[t-1])) / c(D[t-1]) for t = 1..|D|-1:
    //    the return runs from the PREVIOUS DATE OF THE INTERSECTION, not from the symbol's own
    //    previous date. So a return spans the same interval for every symbol, and a date only
    //    one symbol has (a weekend bar) leaves that symbol's return across it exactly what it is
    //    without the extra bar. It is the Carver gate's F5 rule (carver_risk_module.cpp: a date
    //    survives only if every symbol printed on it, returns between consecutive survivors);
    //  * a symbol with fewer than two usable closes gets an EMPTY series and does not shrink D;
    //    calculate_covariance_matrix's empty-series and C-20 guards handle it as before.
    //
    // Every returned non-empty series has the same length, |D| - 1, so the count alignment in
    // calculate_covariance_matrix is the identity on them and its min_periods is |D| - 1. The
    // 756-price cap is applied when the closes are recorded (update_historical_returns), before
    // this. When every symbol has the same dates this returns exactly the series the count
    // alignment used, bit for bit.
    //
    // T-7b-1 7d (the S3 follow-up; T-7a_S1_S3_CODE_REVIEW S3-1, S3-2) narrows the participants in
    // two steps before the intersection is taken, and says so with a WARN per symbol left out:
    //
    //  * STALE: U is the union of the participants' usable dates. A participant whose last usable
    //    date has more than k dates of U after it (k = config_.covariance_stale_dates, portfolio.json
    //    "covariance_stale_dates", absent means 5) is left out: one symbol whose feed stopped used to
    //    end D for every symbol. At exactly k it stays. A calendar gap nobody printed is not a date of
    //    U; a date only one participant printed is. Decided once, on the union of all participants.
    //  * FLOOR: while D gives fewer than kCovarianceMinReturns (20) returns, the participant with
    //    the fewest usable dates is left out (ties: the later first usable date, then the smaller
    //    symbol name); a participant with as many dates as the most-dated one never is, and if
    //    leaving out the shorter ones cannot reach 20 returns nobody is left out. Before this, a D
    //    under 20 returns sent the WHOLE matrix to calculate_covariance_matrix's 0.01 diagonal.
    //
    // A participant left out gets an EMPTY series, so calculate_covariance_matrix gives it the
    // guarded column (the C-20 path: 0.01 variance, zero covariances) and every other entry is what
    // it is without that symbol. With nobody left out this is S3's rule exactly, and the
    // COVARIANCE_DATE_ALIGNED line is byte-identical to S3's.
    std::unordered_map<std::string, std::vector<double>> out;
    std::vector<std::string> participants;
    std::unordered_map<std::string, std::vector<int64_t>> usable_dates;  // ascending
    for (const auto& [symbol, closes] : closes_by_symbol) {
        std::vector<int64_t> usable;
        for (const auto& [day, close] : closes) {
            if (std::isfinite(close) && close > 0.0) usable.push_back(day);
        }
        out[symbol];  // every symbol is in the result, empty unless it takes part
        if (usable.size() >= 2) {
            participants.push_back(symbol);
            usable_dates[symbol] = std::move(usable);
        }
    }
    std::sort(participants.begin(), participants.end());

    // STALE: the participants whose last usable date trails the newest date of U by more than k.
    if (!participants.empty()) {
        std::set<int64_t> all_dates;
        for (const auto& symbol : participants) {
            const auto& d = usable_dates.at(symbol);
            all_dates.insert(d.begin(), d.end());
        }
        const std::vector<int64_t> u(all_dates.begin(), all_dates.end());
        const size_t k = config_.covariance_stale_dates;
        std::vector<std::string> kept;
        for (const auto& symbol : participants) {
            const int64_t last = usable_dates.at(symbol).back();
            const size_t behind =
                static_cast<size_t>(u.end() - std::upper_bound(u.begin(), u.end(), last));
            if (behind > k) {
                WARN("COVARIANCE_STALE_PARTICIPANT symbol=" + symbol +
                     " last=" + covariance_day_label(last) +
                     " newest=" + covariance_day_label(u.back()) +
                     " dates_behind=" + std::to_string(behind) + " k=" + std::to_string(k) +
                     ": left out of the optimizer's date intersection; its column is the guarded "
                     "0.01 variance with zero covariances");
            } else {
                kept.push_back(symbol);
            }
        }
        participants.swap(kept);
    }

    // The intersection of the participants' usable dates, with the 2,520-return cap
    // (max_history_length_) as before: the newest cap + 1 dates. The cap cannot bind at the
    // 756-price cap the closes are recorded under.
    auto intersect = [&](const std::vector<std::string>& ps) {
        std::vector<int64_t> d;
        if (ps.empty()) return d;
        std::map<int64_t, size_t> seen;
        for (const auto& symbol : ps) {
            for (int64_t day : usable_dates.at(symbol)) ++seen[day];
        }
        for (const auto& [day, count] : seen) {
            if (count == ps.size()) d.push_back(day);
        }
        if (d.size() > max_history_length_ + 1) {
            d.erase(d.begin(), d.end() - static_cast<std::ptrdiff_t>(max_history_length_ + 1));
        }
        return d;
    };
    std::vector<int64_t> dates = intersect(participants);

    // FLOOR: the 20 returns calculate_covariance_matrix needs, applied to the intersection. Only a
    // participant with FEWER usable dates than the most-dated participant can be left out (the
    // floor is there to stop a short participant shrinking everyone's window; leaving out a
    // most-dated one cannot lengthen the others'), and nobody is left out unless that reaches
    // the floor: when it cannot, every participant stays and the matrix takes the 0.01 diagonal
    // exactly as under S3, rather than a symbol's data being thrown away for nothing.
    if (participants.size() > 1 && dates.size() < kCovarianceMinReturns + 1) {
        size_t most_dates = 0;
        for (const auto& symbol : participants) {
            most_dates = std::max(most_dates, usable_dates.at(symbol).size());
        }
        std::vector<std::string> left_in = participants;
        std::vector<int64_t> left_in_dates = dates;
        std::vector<std::pair<std::string, size_t>> left_out;  // symbol, intersection returns before
        while (left_in_dates.size() < kCovarianceMinReturns + 1) {
            auto shortest = left_in.end();
            for (auto it = left_in.begin(); it != left_in.end(); ++it) {
                const auto& d = usable_dates.at(*it);
                if (d.size() >= most_dates) continue;
                if (shortest == left_in.end()) {
                    shortest = it;
                    continue;
                }
                const auto& ds = usable_dates.at(*shortest);
                if (d.size() != ds.size() ? d.size() < ds.size()
                    : d.front() != ds.front() ? d.front() > ds.front()
                    : *it < *shortest) {
                    shortest = it;
                }
            }
            if (shortest == left_in.end()) break;  // only most-dated participants are left
            left_out.emplace_back(*shortest,
                                  left_in_dates.empty() ? 0 : left_in_dates.size() - 1);
            left_in.erase(shortest);
            left_in_dates = intersect(left_in);
        }
        if (left_in_dates.size() >= kCovarianceMinReturns + 1) {
            for (const auto& [symbol, before] : left_out) {
                const auto& ds = usable_dates.at(symbol);
                WARN("COVARIANCE_FLOOR_DROP symbol=" + symbol +
                     " own_dates=" + std::to_string(ds.size()) +
                     " first=" + covariance_day_label(ds.front()) +
                     " intersection_returns=" + std::to_string(before) +
                     " floor=" + std::to_string(kCovarianceMinReturns) +
                     ": the intersection is under the floor; the participant with the fewest dates "
                     "is left out of it, not the whole matrix; its column is the guarded 0.01 "
                     "variance with zero covariances");
            }
            participants.swap(left_in);
            dates.swap(left_in_dates);
        }
    }

    size_t shortest_own_returns = SIZE_MAX;
    std::set<int64_t> union_dates;  // of the participants left in, for the log line
    for (const auto& symbol : participants) {
        const auto& d = usable_dates.at(symbol);
        shortest_own_returns = std::min(shortest_own_returns, d.size() - 1);
        union_dates.insert(d.begin(), d.end());
    }

    size_t returns = 0;
    if (dates.size() >= 2) {
        for (const auto& symbol : participants) {
            const auto& closes = closes_by_symbol.at(symbol);
            // T-ROLLX: the numerator reads the ADJUSTED level (the raw close plus the later
            // contract-switch steps) so a switch between two intersection dates is not a return;
            // the denominator stays the RAW previous close (LOOP_SPEC v6.1 section 2.4).
            auto adjusted = adjusted_by_symbol.find(symbol);
            const std::map<int64_t, double>* levels =
                adjusted != adjusted_by_symbol.end() ? &adjusted->second : &closes;
            auto& series = out[symbol];
            series.reserve(dates.size() - 1);
            for (size_t t = 1; t < dates.size(); ++t) {
                const double prev_price = closes.at(dates[t - 1]);
                auto prev_level = levels->find(dates[t - 1]);
                auto curr_level = levels->find(dates[t]);
                const double prev_adjusted =
                    prev_level != levels->end() ? prev_level->second : prev_price;
                const double curr_adjusted =
                    curr_level != levels->end() ? curr_level->second : closes.at(dates[t]);
                series.push_back((curr_adjusted - prev_adjusted) / prev_price);
            }
        }
        returns = dates.size() - 1;
    }

    size_t dropped_inside = 0;
    size_t dropped_after = 0;
    if (!dates.empty()) {
        for (int64_t day : union_dates) {
            if (day > dates.back()) {
                ++dropped_after;
            } else if (day >= dates.front() &&
                       !std::binary_search(dates.begin(), dates.end(), day)) {
                ++dropped_inside;
            }
        }
    }
    INFO("COVARIANCE_DATE_ALIGNED symbols=" + std::to_string(participants.size()) + "/" +
         std::to_string(closes_by_symbol.size()) + " dates=" + std::to_string(dates.size()) +
         " returns=" + std::to_string(returns) +
         " first=" + (dates.empty() ? std::string("-") : covariance_day_label(dates.front())) +
         " last=" + (dates.empty() ? std::string("-") : covariance_day_label(dates.back())) +
         " dropped_inside=" + std::to_string(dropped_inside) +
         " dropped_after=" + std::to_string(dropped_after) + " shortest_own_returns=" +
         (shortest_own_returns == SIZE_MAX ? std::string("-")
                                           : std::to_string(shortest_own_returns)) +
         ": the covariance pairs returns by date over the dates every symbol printed");
    return out;
}

std::vector<std::vector<double>> PortfolioManager::calculate_covariance_matrix(
    const std::unordered_map<std::string, std::vector<double>>& returns_by_symbol) {
    // Get all symbols in a consistent order
    std::vector<std::string> ordered_symbols;
    for (const auto& [symbol, _] : returns_by_symbol) {
        ordered_symbols.push_back(symbol);
    }
    std::sort(ordered_symbols.begin(), ordered_symbols.end());

    size_t num_assets = ordered_symbols.size();

    if (num_assets == 0) {
        ERROR("No assets available for covariance calculation");
    }

    // Find minimum length of return series for all symbols
    size_t min_periods = SIZE_MAX;
    for (const auto& symbol : ordered_symbols) {
        if (returns_by_symbol.at(symbol).empty()) {
            // If any symbol has no returns, we can't calculate covariance
            WARN("Symbol " + symbol + " has no return data");
            continue;
        }
        min_periods = std::min(min_periods, returns_by_symbol.at(symbol).size());
    }

    if (min_periods == SIZE_MAX)
        min_periods = 0;

    if (min_periods < 20) {  // Need sufficient data
        WARN("Insufficient return data for covariance calculation: " + std::to_string(min_periods) +
             " periods");
        // Return diagonal matrix with default variance
        std::vector<std::vector<double>> default_cov(num_assets,
                                                     std::vector<double>(num_assets, 0.0));
        for (size_t i = 0; i < num_assets; ++i) {
            default_cov[i][i] = 0.01;  // Default variance on diagonal
        }
        return default_cov;
    }

    // Create a matrix of aligned returns
    std::vector<std::vector<double>> aligned_returns(min_periods,
                                                     std::vector<double>(num_assets, 0.0));

    // T-4/C2f: the columns the C-20 guard below skipped, so their diagonal can be
    // restored after the covariance is built (SEQUENCE §2.7). Empty whenever the
    // guard does not fire, which is every run measured so far (T4_COVGUARD = 0).
    std::vector<size_t> covguard_rows;

    for (size_t i = 0; i < num_assets; ++i) {
        const auto& symbol = ordered_symbols[i];
        const auto& returns = returns_by_symbol.at(symbol);

        // C-20: a symbol whose return series is shorter than min_periods was
        // skipped by the scan above (the empty case `continue`s there)
        // but is still in ordered_symbols, so the unsigned subtraction below
        // wraps and the copy reads far outside `returns`. Leave its column at
        // the zeros aligned_returns was built with, which keeps the matrix
        // square: the optimizer validates the covariance dimension against the
        // symbol list, so dropping the symbol instead would break the caller.
        if (returns.size() < min_periods) {
            WARN("T4_COVGUARD symbol=" + symbol);
            covguard_rows.push_back(i);
            continue;
        }

        // Take the most recent min_periods returns. On the optimizer's input (date_aligned_returns)
        // every non-empty series already has min_periods returns paired by date, so this is the
        // identity there.
        size_t start_idx = returns.size() - min_periods;
        for (size_t j = 0; j < min_periods; ++j) {
            aligned_returns[j][i] = returns[start_idx + j];
        }
    }

    // Calculate means for each asset using aligned returns
    std::vector<double> means(num_assets, 0.0);
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t t = 0; t < min_periods; ++t) {
            means[i] += aligned_returns[t][i];
        }
        means[i] /= min_periods;
    }

    // Calculate covariance matrix
    std::vector<std::vector<double>> covariance(num_assets, std::vector<double>(num_assets, 0.0));

    // Avoid division by zero when min_periods == 1
    double divisor = (min_periods > 1) ? (min_periods - 1) : 1.0;

    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            double cov_sum = 0.0;
            for (size_t t = 0; t < min_periods; ++t) {
                cov_sum += (aligned_returns[t][i] - means[i]) * (aligned_returns[t][j] - means[j]);
            }

            covariance[i][j] = cov_sum / divisor;

            // Annualize the covariance (assuming daily data with 252 trading days)
            covariance[i][j] *= 252.0;
        }
    }

    // T-4/C2f -- SEQUENCE §2.7's other correction. A symbol the C-20 guard skipped
    // keeps the all-zero column aligned_returns was built with, so its VARIANCE is
    // zero too: the greedy's s^2 * Sigma_ii / 2 penalty vanishes and the name looks
    // like free risk (dynamic_optimizer.cpp:157-169), and correlation_multiplier's
    // `var_i <= 0.0` test would silently skip it. Give the guarded column the same
    // default variance the insufficient-data branch above uses. Scoped to the rows
    // the guard actually skipped -- NOT a blanket sweep over every diagonal -- so
    // that with T4_COVGUARD never firing this loop is provably a no-op and cannot
    // perturb a legitimate symbol's variance; that is what makes the futures-arm
    // identity check against C2 a clean test of the window fix alone.
    for (size_t i : covguard_rows) {
        if (covariance[i][i] <= 0.0) {
            covariance[i][i] = 0.01;  // same default as the min_periods < 20 branch
        }
    }

    return covariance;
}

namespace {

// One sleeve's part of a symbol in the post-optimizer distribution: its contracts, unrounded.
struct SleeveQuota {
    std::string strategy_id;
    double quota;
};

// Largest-remainder (Hamilton) split: the book's integer is round(sum of the quotas), rounded
// ONCE (half away from zero, as std::round), and every sleeve gets the floor of its quota plus
// one contract for each of the largest remainders until the integers sum to the book's integer
// exactly. Tie rule (deterministic): the larger remainder first, remainders compared at a 1e-9
// resolution so floating noise cannot order two equal remainders; on a tie, the smaller
// strategy_id first. A book whose quotas sum below zero is split as the mirror image of the long
// book (negate, split, negate), so a short is treated exactly as a long.
std::vector<int64_t> split_largest_remainder(const std::vector<SleeveQuota>& sleeves) {
    const size_t n = sleeves.size();
    std::vector<int64_t> out(n, 0);
    if (n == 0)
        return out;

    double sum = 0.0;
    for (const auto& s : sleeves)
        sum += s.quota;
    const double sign = sum < 0.0 ? -1.0 : 1.0;

    const int64_t book = std::llround(sign * sum);
    int64_t floors = 0;
    std::vector<int64_t> remainder_1e9(n, 0);
    for (size_t k = 0; k < n; ++k) {
        const double q = sign * sleeves[k].quota;
        const double f = std::floor(q);
        out[k] = static_cast<int64_t>(f);
        floors += out[k];
        remainder_1e9[k] = std::llround((q - f) * 1e9);
    }

    std::vector<size_t> order(n);
    for (size_t k = 0; k < n; ++k)
        order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (remainder_1e9[a] != remainder_1e9[b])
            return remainder_1e9[a] > remainder_1e9[b];
        return sleeves[a].strategy_id < sleeves[b].strategy_id;
    });

    // 0 <= book - floors <= n: round(sum) is within 0.5 of the sum and each floor within 1 of
    // its quota, so no sleeve receives more than one extra contract.
    const int64_t extra = book - floors;
    for (int64_t k = 0; k < extra; ++k)
        out[order[static_cast<size_t>(k) % n]] += 1;

    if (sign < 0.0)
        for (auto& q : out)
            q = -q;
    return out;
}

}  // namespace

SleeveDistribution distribute_optimizer_contracts(double optimizer_contracts,
                                                  const std::vector<SleeveContribution>& sleeves) {
    SleeveDistribution d;
    double total = 0.0;
    bool any_long = false, any_short = false;
    for (const auto& s : sleeves) {
        total += s.contribution;
        any_long = any_long || s.contribution > 0.0;
        any_short = any_short || s.contribution < 0.0;
    }

    const int rounded_contracts = static_cast<int>(std::round(optimizer_contracts));
    std::vector<SleeveQuota> quotas;
    quotas.reserve(sleeves.size());

    // T-7b-2 8b: opposed sleeves. Split only the optimizer's deviation from the net target,
    // weighted by each sleeve's |target|, so nothing is amplified and a cancelling total keeps
    // both sleeves' books (see allocation_split.hpp).
    if (any_long && any_short) {
        std::vector<double> t;
        t.reserve(sleeves.size());
        double net = 0.0, gross = 0.0;
        for (const auto& s : sleeves) {
            const double c = std::isnan(s.contracts) ? s.contribution : s.contracts;
            t.push_back(c);
            net += c;
            gross += std::fabs(c);
        }
        const double deviation = optimizer_contracts - net;
        for (size_t k = 0; k < sleeves.size(); ++k) {
            const double quota = t[k] + deviation * std::fabs(t[k]) / gross;
            quotas.push_back({sleeves[k].strategy_id, quota});
            d.quota.push_back(quota);
            d.per_sleeve_rounding.push_back(static_cast<int64_t>(std::llround(quota)));
        }
        d.stored = split_largest_remainder(quotas);
        for (auto q : d.stored)
            d.book += q;
        return d;
    }

    for (const auto& s : sleeves) {
        // D9 (LOOP_SPEC v6.1 section 5.4, T-ROLLX): a sleeve's share is computed on any non-zero
        // total; the test that zeroed a single-sign negative total (and so every short) is removed.
        const double share = std::abs(total) > 1e-8 ? s.contribution / total : 0.0;
        // A sleeve with no share has a quota of 0.
        const double quota = share == 0.0 ? 0.0 : optimizer_contracts * share;
        quotas.push_back({s.strategy_id, quota});
        d.quota.push_back(quota);
        d.per_sleeve_rounding.push_back(
            static_cast<int64_t>(std::round(share == 0.0 ? 0.0 : rounded_contracts * share)));
    }

    d.stored = split_largest_remainder(quotas);
    for (auto q : d.stored)
        d.book += q;
    return d;
}

Result<void> PortfolioManager::optimize_positions() {
    try {
        // Get unique symbols across all strategies and collect data under lock
        std::vector<std::string> symbols;
        std::vector<double> current_weights;
        std::vector<double> target_weights;
        std::vector<double> weights_per_contract;
        std::vector<double> costs;
        // The date-keyed closes of the symbols in the matrix (T-7a INSERT S3): the covariance
        // is built from them aligned by DATE, not from the per-symbol return vectors by count.
        std::unordered_map<std::string, std::map<int64_t, double>> closes_by_symbol;

        // Store original contributions per strategy per symbol for proportional distribution
        // Map: symbol -> strategy_id -> contribution (quantity * weight_per_contract)
        std::unordered_map<std::string, std::unordered_map<std::string, double>> original_contribs;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // First collect all potential symbols
            std::vector<std::string> all_symbols;
            for (const auto& [strat_id, info] : strategies_) {
                if (!info.use_optimization || pinned_scopes_.count(strat_id))
                    continue;

                for (const auto& [symbol, _] : info.target_positions) {
                    all_symbols.push_back(symbol);
                }
            }

            // Remove duplicates
            std::sort(all_symbols.begin(), all_symbols.end());
            all_symbols.erase(std::unique(all_symbols.begin(), all_symbols.end()),
                              all_symbols.end());

            if (all_symbols.empty()) {
                INFO("No symbols found for optimization, skipping");
                return Result<void>();
            }

            int min_history_length = 20;  // Minimum history length for covariance calculation

            // T-OPT E-7 (ledger OPT-new-symbol-collapses-min-periods): a symbol enters the
            // optimizer only when at least one optimizing strategy that lists it SIGNALS it
            // (StrategyInterface::is_signalling; a trend sleeve does not while the symbol's price
            // history is shorter than its longest EMA window, so the target it lists is not a
            // forecast of its own). Before this a contract still warming up entered on the PM's own
            // 20 returns and date_aligned_returns' intersection over every participant became ITS
            // dates for every symbol (on the frozen backtest, the roots without Sunday bars set the
            // window of the symbols already signalled on every rebalance before they were signalled
            // themselves); 7d's floor catches that only below 20 returns. A symbol left
            // out keeps the strategy's own target (zero while it warms up) exactly as a symbol
            // with too little history does below; the PM still records its closes, so it enters
            // with its full history the day it is signalled. The Carver gate's window applies the
            // same test to its own intersection (T-7b-2 CGW, gate_participants_ in
            // process_market_data).
            std::vector<std::string> not_signalling;
            size_t not_signalling_nonzero = 0;
            auto signalled = [&](const std::string& symbol) {
                for (const auto& [strat_id, info] : strategies_) {
                    if (!info.use_optimization || pinned_scopes_.count(strat_id)) continue;
                    if (!info.target_positions.count(symbol)) continue;
                    if (info.strategy && info.strategy->is_signalling(symbol)) return true;
                }
                return false;
            };

            // Filter symbols to only those with sufficient historical data FIRST
            for (const auto& symbol : all_symbols) {
                if (!signalled(symbol)) {
                    not_signalling.push_back(symbol);
                    for (const auto& [strat_id, info] : strategies_) {
                        if (!info.use_optimization || pinned_scopes_.count(strat_id)) continue;
                        auto t = info.target_positions.find(symbol);
                        if (t != info.target_positions.end() &&
                            std::abs(static_cast<double>(t->second.quantity)) > 1e-12) {
                            ++not_signalling_nonzero;
                            break;
                        }
                    }
                    continue;
                }
                auto it = historical_returns_.find(symbol);
                if (it != historical_returns_.end() &&
                    it->second.size() >= static_cast<size_t>(min_history_length)) {
                    symbols.push_back(symbol);
                    // Copy the data under lock. historical_returns_[symbol] is computed from
                    // closes_by_date_[symbol], so the entry exists whenever the returns do; a
                    // symbol without one enters with no closes and is handled as an empty
                    // series (the C-20 guard in calculate_covariance_matrix).
                    auto closes = closes_by_date_.find(symbol);
                    closes_by_symbol[symbol] = closes != closes_by_date_.end()
                                                   ? closes->second
                                                   : std::map<int64_t, double>{};
                } else {
                    INFO("Symbol " + symbol +
                         " has insufficient historical data for optimization, skipping symbol");
                }
            }

            if (!not_signalling.empty()) {
                std::string list;
                for (const auto& symbol : not_signalling) {
                    list += (list.empty() ? "" : ",") + symbol;
                }
                INFO("OPTIMIZER_NOT_SIGNALLING count=" + std::to_string(not_signalling.size()) +
                     " symbols=" + list +
                     " nonzero_targets=" + std::to_string(not_signalling_nonzero) +
                     ": no optimizing strategy signals them yet (warm-up); left out of the "
                     "optimizer's covariance and its date intersection, each keeps its strategy's "
                     "own target");
            }

            if (symbols.empty()) {
                INFO("No symbols have sufficient historical data for optimization, skipping");
                return Result<void>();
            }

            // Calculate weights per contract (only for valid symbols). Only a book that names no
            // overlay sleeve reaches this step, and it has no trend sleeve to read a contract size
            // and price from (the branch that did was reached by no book after the one pass and
            // is gone): every symbol takes the default weight, named as it always was.
            weights_per_contract.reserve(symbols.size());

            for (auto const& symbol : symbols) {
                WARN("Symbol " + symbol + " not found in trading data, using default weight");
                weights_per_contract.push_back(0.01);  // Reasonable default
            }

            // Build current and target in weight space (only for valid symbols)
            current_weights.resize(symbols.size(), 0.0);
            target_weights.resize(symbols.size(), 0.0);

            // PRE_OPTIMIZER_TRACE: log the optimizer's per-strategy current_positions size,
            // to verify Fix #7 (PortfolioManager seeding) actually took effect. If empty here,
            // optimizer's coord descent runs from a zero baseline (the source of daily churn).
            for (const auto& [strat_id, info] : strategies_) {
                if (!info.use_optimization || pinned_scopes_.count(strat_id))
                    continue;
                DEBUG("PRE_OPTIMIZER_TRACE: strat=" + strat_id +
                     " current_positions_size=" +
                     std::to_string(info.current_positions.size()) +
                     " target_positions_size=" +
                     std::to_string(info.target_positions.size()));
            }

            for (size_t i = 0; i < symbols.size(); ++i) {
                const std::string& symbol = symbols[i];

                // Aggregate across strategies, in account weight (ledger N2): a sleeve sizes its
                // contracts on its own capital slice (capital x allocation), so each of its
                // contracts is one contract of the account's book and weighs w; the account
                // holds the sum of the sleeves' contracts, which is the book apply_risk_management
                // gates. The allocation is already inside the quantity and is not applied again.
                for (const auto& [strat_id, info] : strategies_) {
                    if (!info.use_optimization || pinned_scopes_.count(strat_id))
                        continue;

                    if (info.current_positions.count(symbol)) {
                        current_weights[i] +=
                            static_cast<double>(info.current_positions.at(symbol).quantity) *
                            weights_per_contract[i];
                    }
                    if (info.target_positions.count(symbol)) {
                        double contrib =
                            static_cast<double>(info.target_positions.at(symbol).quantity) *
                            weights_per_contract[i];
                        target_weights[i] += contrib;

                        // Store original contribution for proportional distribution after
                        // optimization
                        original_contribs[symbol][strat_id] = contrib;
                    }
                }
            }

            // Calculate trading costs (inside lock since it accesses strategies_)
            costs = calculate_trading_costs(symbols, static_cast<double>(sizing_capital_));
        }  // End of mutex lock scope

        // Use cached covariance if valid, otherwise compute and cache
        std::vector<std::vector<double>> covariance;
        if (covariance_cache_valid_ && cached_symbols_ == symbols) {
            // Reuse cached covariance (iterations 2-5 within same day)
            covariance = cached_covariance_;
            DEBUG("Using cached covariance matrix for convergence iteration");
        } else {
            // Compute covariance matrix (first iteration or symbols changed)
            covariance = calculate_covariance_matrix(date_aligned_returns(
                closes_by_symbol, adjusted_closes_by_symbol(closes_by_symbol)));
            // Cache for subsequent iterations
            cached_symbols_ = symbols;
            cached_covariance_ = covariance;
            covariance_cache_valid_ = true;
            DEBUG("Computed and cached covariance matrix for " + std::to_string(symbols.size()) +
                  " symbols");
        }

        // Call the optimizer
        if (!optimizer_) {
            ERROR("Optimizer not initialized");
            return make_error<void>(ErrorCode::NOT_INITIALIZED, "Optimizer not initialized",
                                    "PortfolioManager");
        }

        auto result = optimizer_->optimize(current_weights, target_weights, costs,
                                           weights_per_contract, covariance);

        if (result.is_error()) {
            return make_error<void>(result.error()->code(),
                                    "Optimization failed: " + std::string(result.error()->what()),
                                    "PortfolioManager");
        }

        // Log optimization result metrics
        INFO("Optimization metrics: tracking error=" +
             std::to_string(result.value().tracking_error) +
             ", cost=" + std::to_string(result.value().cost_penalty) +
             ", iterations=" + std::to_string(result.value().iterations));

        const auto& optimized_positions = result.value().positions;

        // Apply optimized positions back to strategies under lock
        {
            std::lock_guard<std::mutex> lock(mutex_);

            for (size_t i = 0; i < symbols.size(); ++i) {
                const auto& symbol = symbols[i];

                // The optimizer's answer in account contracts, UNROUNDED (ledger N2: the
                // aggregate is the sum of the sleeves' contracts, the account's book).
                double raw_contracts = optimized_positions[i] / weights_per_contract[i];

                // Each sleeve's quota is its share of the answer, raw x share, unrounded; the
                // book is rounded ONCE and split by largest remainder, so the stored sleeve
                // integers sum to round(raw) exactly (distribute_optimizer_contracts). Before
                // N2 the aggregate weighted each contract by its sleeve's allocation and the
                // quota divided by it again, so one optimizer step (one w) became 1 / (sum(q x
                // allocation) / sum(q)) contracts of book: 3.33 on a symbol only a 0.3 sleeve
                // holds. One sleeve at allocation 1.0 stores round(raw) as before.
                std::vector<SleeveContribution> contributions;
                for (auto& [strat_id, info] : strategies_) {
                    if (!info.use_optimization || pinned_scopes_.count(strat_id))
                        continue;
                    if (!info.target_positions.count(symbol))
                        continue;
                    contributions.push_back(
                        {strat_id, original_contribs[symbol][strat_id],
                         static_cast<double>(info.target_positions.at(symbol).quantity)});
                }

                const SleeveDistribution d =
                    distribute_optimizer_contracts(raw_contracts, contributions);
                bool differs = false;
                for (size_t k = 0; k < contributions.size(); ++k) {
                    strategies_.at(contributions[k].strategy_id).target_positions[symbol].quantity =
                        static_cast<Decimal>(static_cast<double>(d.stored[k]));
                    differs = differs || d.stored[k] != d.per_sleeve_rounding[k];
                }
                if (differs) {
                    std::ostringstream line;
                    line << "ALLOCATION_SPLIT sym=" << symbol << " optimizer=" << raw_contracts
                         << " book=" << d.book << " split:";
                    for (size_t k = 0; k < contributions.size(); ++k)
                        line << " " << contributions[k].strategy_id << "=" << d.stored[k]
                             << " (quota " << d.quota[k] << ")";
                    line << "; rounding each sleeve would store:";
                    for (size_t k = 0; k < contributions.size(); ++k)
                        line << " " << contributions[k].strategy_id << "="
                             << d.per_sleeve_rounding[k];
                    INFO(line.str());
                }
            }

            // Log final positions after optimization
            DEBUG("Final optimized positions:");
            for (size_t i = 0; i < symbols.size(); ++i) {
                const std::string& symbol = symbols[i];
                double original_position = 0.0;
                double optimized_position = 0.0;

                // Sum up positions across all strategies: the account's contracts (ledger N2;
                // a sleeve's contracts already carry its allocation)
                for (const auto& [_, info] : strategies_) {
                    if (info.target_positions.count(symbol) > 0) {
                        optimized_position +=
                            static_cast<double>(info.target_positions.at(symbol).quantity);
                    }
                }

                INFO("Symbol " + symbol + ": raw=" + std::to_string(original_position) +
                     ", optimized=" + std::to_string(optimized_position) +
                     ", change=" + std::to_string(optimized_position - original_position));
            }
        }  // End of mutex lock scope

        INFO("Position optimization completed successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error during optimization: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error during optimization: ") + e.what(),
                                "PortfolioManager");
    }
}

bool PortfolioManager::one_pass_book() const {
    return !config_.overlay_sleeve.empty() && config_.overlay_tau > 0.0 &&
           strategies_.count(config_.overlay_sleeve) > 0;
}

void PortfolioManager::record_no_pass(const std::string& signal_date, Timestamp cycle,
                                      bool is_warmup) const {
    one_pass::append_no_pass_record(id_, signal_date, core::format_utc_date(cycle), is_warmup);
}

OnePassDay PortfolioManager::last_one_pass() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return one_pass_day_;
}

Result<void> PortfolioManager::rebalance_one_pass(
    const std::vector<Bar>& data, bool is_warmup, std::optional<Timestamp> as_of,
    const std::unordered_set<std::string>* session_symbols,
    const std::unordered_set<std::string>& caller_holds,
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>&
        prev_positions) {
    // The decision row of this rebalance, recorded after the lock is released.
    RiskDecision decision;
    RiskAction applied = RiskAction::NONE;
    Decimal applied_factor{Decimal(1.0)};
    RiskPhase phase = RiskPhase::LAP;
    std::string failure;
    bool unseeded = false;
    std::string overlay_module_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto first = strategies_.find(config_.overlay_sleeve);

        // The overlay's limits: the book's one carver module carries them (ratios to tau and the
        // two leverage limits). A book that names an overlay sleeve runs the overlay and nothing
        // else at portfolio scope.
        const CarverRiskModule* carver = nullptr;
        for (const auto& module : risk_modules_) {
            const auto* candidate = dynamic_cast<const CarverRiskModule*>(module.get());
            if (candidate == nullptr || !candidate->overlay_limits().set() || carver != nullptr ||
                risk_modules_.size() != 1) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "A book that names an overlay sleeve runs one carver risk module carrying the "
                    "overlay's limits (R_max, R_jump_max, R_shock_max) and no other portfolio "
                    "module",
                    "PortfolioManager");
            }
            carver = candidate;
        }
        if (carver == nullptr) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "A book that names an overlay sleeve has no carver risk module "
                                    "carrying the overlay's limits",
                                    "PortfolioManager");
        }
        overlay_module_id = carver->id();

        // The sleeves in id order, and each sleeve's held book: the filled ledger in a backtest,
        // the book the call started with (the seeded T-1 book of a live run) otherwise.
        std::vector<std::string> sids;
        for (const auto& [sid, info] : strategies_) {
            (void)info;
            sids.push_back(sid);
        }
        std::sort(sids.begin(), sids.end());
        auto held_of = [&](const std::string& sid, const std::string& symbol) {
            if (is_backtest_) {
                const auto ledger = filled_positions_.find(sid);
                if (ledger == filled_positions_.end()) return 0.0;
                const auto q = ledger->second.find(symbol);
                return q == ledger->second.end() ? 0.0 : q->second;
            }
            const auto book = prev_positions.find(sid);
            if (book == prev_positions.end()) return 0.0;
            const auto q = book->second.find(symbol);
            return q == book->second.end() ? 0.0 : static_cast<double>(q->second.quantity);
        };

        // The symbols: everything a sleeve lists or holds (section 5.1: no silent default). One the
        // first sleeve has no series, close or multiplier for cannot be weighed: with no position
        // it is named and left alone; with a held position it enters the pass as a FIXED row at
        // its last usable close. One it weighs but the cost model cannot price is held (below).
        std::set<std::string> listed;
        for (const auto& sid : sids) {
            for (const auto& [symbol, pos] : strategies_.at(sid).target_positions) {
                (void)pos;
                listed.insert(symbol);
            }
            if (is_backtest_) {
                const auto ledger = filled_positions_.find(sid);
                if (ledger != filled_positions_.end()) {
                    for (const auto& [symbol, q] : ledger->second) {
                        if (q != 0.0) listed.insert(symbol);
                    }
                }
            } else {
                const auto book = prev_positions.find(sid);
                if (book != prev_positions.end()) {
                    for (const auto& [symbol, pos] : book->second) {
                        if (static_cast<double>(pos.quantity) != 0.0) listed.insert(symbol);
                    }
                }
            }
        }
        // A stopped overlay sleeve leaves the book with no series to weigh anything on: the scope is
        // refused (the held book stored, REFUSE recorded), never stored as a clean day.
        std::string scope_refusal;
        const bool overlay_running = first->second.strategy &&
                                     first->second.strategy->get_state() == StrategyState::RUNNING;
        if (!overlay_running) {
            scope_refusal = "the overlay sleeve " + config_.overlay_sleeve +
                            " is not RUNNING, so the book cannot be weighed";
        }
        std::vector<std::string> symbols;
        std::vector<StrategyInterface::OverlaySeries> own;
        std::vector<double> costs;
        std::vector<std::string> unpriced;       // not passed: every sleeve keeps its held quantity
        std::vector<char> unweighed_held;        // per passed symbol: a held row fixed at its last close
        std::vector<std::string> unweighed_lines;
        for (const auto& symbol : listed) {
            if (!overlay_running) {
                unpriced.push_back(symbol);
                continue;
            }
            StrategyInterface::OverlaySeries series;
            const bool has_series = first->second.strategy->overlay_series(symbol, &series);
            const bool weighed = has_series && series.close > 0.0 && series.multiplier > 0.0 &&
                                 std::isfinite(series.close) && std::isfinite(series.multiplier);
            // The cost of one contract bought at the signal close, as the cost model prices it. A
            // symbol the cost model has no usable volume for (never fed, or fed nothing but zero
            // volume) has no cost: the model would price it on a generic ADV, which is not a price.
            const bool cost_fed = cost_manager_.has_usable_volume(symbol);
            const double cost =
                weighed && cost_fed ? cost_manager_.calculate_costs(symbol, 1.0, series.close)
                                          .total_transaction_costs
                                    : 0.0;
            if (weighed && cost_fed && std::isfinite(cost) && cost > 0.0) {
                symbols.push_back(symbol);
                own.push_back(std::move(series));
                costs.push_back(cost);
                unweighed_held.push_back(0);
                continue;
            }
            if (weighed) {
                // The sleeve weighs it but the cost model has no cost of its own for it (it was
                // never fed the symbol, or prices it at nothing). It stays in the pass, a
                // participant whose dates count in the overlay's window, as a HELD row at its held
                // quantity (zero included): it cannot be opened or traded on a default cost.
                unweighed_lines.push_back(
                    "BOOK_UNPRICED " + symbol + ": the cost model has no cost for it (" +
                    (cost_fed ? "a cost that is not a positive number"
                              : (cost_manager_.has_volume_history(symbol)
                                     ? "its fed volume is zero"
                                     : "never fed its volume")) +
                    "); held at the held quantity as a fixed row, no fill");
                symbols.push_back(symbol);
                own.push_back(std::move(series));
                costs.push_back(0.0);
                unweighed_held.push_back(1);
                continue;
            }
            bool held_row = false;
            for (const auto& sid : sids) held_row = held_row || held_of(sid, symbol) != 0.0;
            if (!held_row) {
                unpriced.push_back(symbol);
                continue;
            }
            // Section 6.1: a held symbol that cannot be weighed today is valued at its last usable
            // close and its stored multiplier and enters the pass as a FIXED row at its held
            // quantity, so it is in the leverage readings, the cap check and both sides of the
            // delivered scale; it gets no fill. With no usable close at all the scope is refused.
            StrategyInterface::OverlaySeries fixed_row;
            {
                // The last usable close: this manager's latest stored close of the symbol, else
                // the close the last pass valued it on. The stored multiplier: the last pass's,
                // else the registry's.
                fixed_row.close = 0.0;
                fixed_row.multiplier = 0.0;
                const auto valued = one_pass_valued_.find(symbol);
                if (valued != one_pass_valued_.end()) {
                    fixed_row.close = valued->second.first;
                    fixed_row.multiplier = valued->second.second;
                } else if (registry_ && registry_->has_instrument(symbol)) {
                    auto instrument = registry_->get_instrument(symbol);
                    if (instrument) fixed_row.multiplier = instrument->get_multiplier();
                }
                const auto closes = closes_by_date_.find(symbol);
                if (closes != closes_by_date_.end() && !closes->second.empty() &&
                    closes->second.rbegin()->second > 0.0) {
                    fixed_row.close = closes->second.rbegin()->second;
                }
            }
            if (!(fixed_row.close > 0.0) || !(fixed_row.multiplier > 0.0) ||
                !std::isfinite(fixed_row.close) || !std::isfinite(fixed_row.multiplier)) {
                if (scope_refusal.empty()) {
                    scope_refusal = "the held symbol " + symbol +
                                    " cannot be weighed today and has no usable close or multiplier";
                }
                unpriced.push_back(symbol);
                continue;
            }
            unweighed_lines.push_back(
                "BOOK_UNPRICED " + symbol + ": no series, close or multiplier for it today; "
                "held as a fixed row valued at its last usable close " +
                std::to_string(fixed_row.close) + " x multiplier " +
                std::to_string(fixed_row.multiplier) + ", no fill");
            symbols.push_back(symbol);
            own.push_back(std::move(fixed_row));
            costs.push_back(0.0);
            unweighed_held.push_back(1);
        }
        const size_t n = symbols.size();

        one_pass::DayInputs in;
        in.capital = static_cast<double>(sizing_capital_);
        in.tau = config_.overlay_tau;
        in.cap = config_.per_name_cap;
        in.cost_multiplier = config_.opt_config.cost_penalty_scalar;
        in.sign_band = config_.sign_close_band;
        in.b_sigma_floor = config_.b_sigma_floor;
        in.trim_max = config_.trim_max;
        in.max_iterations = config_.opt_config.max_iterations;
        {
            const auto& ratios = carver->overlay_limits();
            in.limits = {ratios.risk * in.tau, ratios.jump * in.tau, ratios.shock * in.tau,
                         ratios.gross, ratios.net};
        }
        in.multiplier.assign(n, 1.0);
        in.close.assign(n, 0.0);
        in.held.assign(n, 0.0);
        in.target.assign(n, 0.0);
        in.first_forecast.assign(n, 0.0);
        in.cost = costs;
        in.signalling.assign(n, 0);
        in.first_signalling.assign(n, 0);
        in.hold.assign(n, 0);
        in.has_bar.assign(n, 0);
        in.ever_signalled.assign(n, 0);
        in.jump_sigma_daily.assign(n, 0.0);
        std::unordered_set<std::string> fed;
        for (const auto& bar : data) fed.insert(bar.symbol);
        // Each sleeve's unrounded contribution N*_s and held quantity, per symbol.
        std::vector<std::vector<double>> contribution(sids.size(), std::vector<double>(n, 0.0));
        std::vector<std::vector<double>> sleeve_held(sids.size(), std::vector<double>(n, 0.0));
        one_pass::Mask slow_zeroed(n, 0);
        std::vector<overlay::ParticipantSeries> views(n);
        bool sleeve_pinned = false;
        for (size_t i = 0; i < n; ++i) {
            const std::string& symbol = symbols[i];
            in.multiplier[i] = own[i].multiplier;
            in.close[i] = own[i].close;
            if (!unweighed_held[i]) one_pass_valued_[symbol] = {own[i].close, own[i].multiplier};
            in.first_forecast[i] = own[i].forecast;
            in.first_signalling[i] = own[i].signalling;
            in.jump_sigma_daily[i] = own[i].jump_sigma_daily;
            slow_zeroed[i] = own[i].slow_rule_zeroed;
            in.has_bar[i] = fed.count(symbol) > 0;
            // Section 6.1: the hold applies on sized days only (in warm-up the book follows the
            // search): the caller's set, and in the backtest every symbol outside the session set.
            in.hold[i] = unweighed_held[i] ||
                         (!is_warmup &&
                          (caller_holds.count(symbol) > 0 ||
                           (session_symbols != nullptr && session_symbols->count(symbol) == 0)));
            for (size_t s = 0; s < sids.size(); ++s) {
                const auto& info = strategies_.at(sids[s]);
                sleeve_held[s][i] = held_of(sids[s], symbol);
                in.held[i] += sleeve_held[s][i];
                if (pinned_scopes_.count(sids[s])) sleeve_pinned = true;
                // A sleeve that is not RUNNING signals nothing: its held rows close on the next
                // SESSION bar (D33) through the split below.
                if (!info.strategy || info.strategy->get_state() != StrategyState::RUNNING) continue;
                StrategyInterface::OverlaySeries sleeve;
                if (!info.strategy->overlay_series(symbol, &sleeve) || !sleeve.signalling) continue;
                const auto scaled = rebalance_applied_.find(sids[s]);
                contribution[s][i] = sleeve.optimal_position *
                                     (scaled == rebalance_applied_.end() ? 1.0 : scaled->second);
                in.signalling[i] = 1;
                in.target[i] += contribution[s][i];
            }
            // A fresh process keeps no memory of earlier signals: a held position stands for one.
            in.ever_signalled[i] = ever_signalled_.count(symbol) > 0 ||
                                   (!is_backtest_ && in.held[i] != 0.0);
            views[i].present = true;
            views[i].day = &own[i].day;
            views[i].returns = &own[i].returns;
        }
        const overlay::Inputs window = overlay::build_inputs(in.tau, symbols, views);
        in.returns = window.returns;
        in.ordinals = window.ordinals;
        // the optimiser's closes and levels on the union of the symbols' own dates
        for (size_t i = 0; i < n; ++i) {
            in.opt_ordinals.insert(in.opt_ordinals.end(), own[i].opt_day.begin(), own[i].opt_day.end());
        }
        std::sort(in.opt_ordinals.begin(), in.opt_ordinals.end());
        in.opt_ordinals.erase(std::unique(in.opt_ordinals.begin(), in.opt_ordinals.end()),
                              in.opt_ordinals.end());
        const double nan = std::numeric_limits<double>::quiet_NaN();
        in.opt_closes.assign(in.opt_ordinals.size(), std::vector<double>(n, nan));
        in.opt_levels.assign(in.opt_ordinals.size(), std::vector<double>(n, nan));
        for (size_t i = 0; i < n; ++i) {
            for (size_t k = 0; k < own[i].opt_day.size(); ++k) {
                const auto row = static_cast<size_t>(
                    std::lower_bound(in.opt_ordinals.begin(), in.opt_ordinals.end(), own[i].opt_day[k]) -
                    in.opt_ordinals.begin());
                in.opt_closes[row][i] = own[i].opt_close[k];
                in.opt_levels[row][i] = own[i].opt_level[k];
            }
        }

        // Listing dates (nothing is ever due without portfolio.json's listing_dates, and rule
        // close_reenter leaves the switch to the close-out and the pass below). Once per pair, on
        // the first sized rebalance whose signal feed holds the listed contract's bar dated on or
        // after its listing date with both contracts free to trade (a non-session bar or a pending
        // roll holds both: they read one series) and the listed contract signalling: the held
        // predecessor is closed and the listed contract's held quantity is set by the rule, BEFORE
        // the pass, which then starts from that held book (the block sits after the pass's inputs are
        // built and before the pass: nothing between reads the held book). Each move is a fill at the signal close,
        // priced by this manager's cost model on its own contract's terms (ids LC- and LO-). A pair
        // whose predecessor never signalled on a sized rebalance of this run (the run starts
        // trading after the listing date) has no switch: the listed contract is an ordinary symbol.
        if (is_backtest_ && !is_warmup && scope_refusal.empty() && ListingDates::instance().enabled()) {
            for (size_t i = 0; i < n; ++i) {
                // signalling on a bar fed today: a stale answer on a day its bar is withheld is not one
                if (in.signalling[i] && in.has_bar[i] &&
                    ListingDates::instance().is_predecessor(symbols[i])) {
                    listing_predecessor_traded_.insert(symbols[i]);
                }
            }
        }
        // The switch's fills and ledger entries are PENDING until the pass has run: a pass that is
        // refused sends no order, so a refused day undoes the switch and it is made on a later pass.
        struct PendingListingSwitch {
            std::string sid, from, to;
            double new_to;
            std::vector<ExecutionReport> legs;
            std::vector<std::string> lines;
        };
        std::vector<PendingListingSwitch> pending_switches;
        std::vector<std::string> switched_this_pass;
        std::vector<double> held_before_switch;
        std::vector<std::vector<double>> sleeve_held_before_switch;
        if (is_backtest_ && !is_warmup && scope_refusal.empty() && !sleeve_pinned &&
            ListingDates::instance().enabled() &&
            ListingDates::instance().switch_rule() != ListingSwitchRule::kCloseReenter) {
            held_before_switch = in.held;
            sleeve_held_before_switch = sleeve_held;
            // The pairs whose switch is made on this pass, with the band read on the book before any
            // of them is switched.
            struct DueSwitch {
                ListingConversion c;
                size_t i_from, i_to;
                bool in_band;
            };
            std::vector<DueSwitch> due;
            auto wait = [&](size_t i_from, size_t i_to) {
                // waits for the next rebalance on which both can trade; a held predecessor is HELD
                // meanwhile, never closed by the close-out, and the listed contract is not opened
                // beside it by the pass
                if (in.held[i_from] != 0.0) {
                    in.hold[i_from] = 1;
                    in.hold[i_to] = 1;
                }
            };
            for (const auto& c : ListingDates::instance().conversions_due(data)) {
                if (listing_switched_.count(c.to)) continue;
                const auto at_from = std::find(symbols.begin(), symbols.end(), c.from);
                const auto at_to = std::find(symbols.begin(), symbols.end(), c.to);
                if (at_from == symbols.end()) continue;
                const size_t i_from = static_cast<size_t>(at_from - symbols.begin());
                if (at_to == symbols.end()) {
                    if (in.held[i_from] != 0.0) in.hold[i_from] = 1;  // held, never closed out, while it waits
                    continue;
                }
                const size_t i_to = static_cast<size_t>(at_to - symbols.begin());
                if (!listing_predecessor_traded_.count(c.from) && in.held[i_from] == 0.0) {
                    listing_switched_.insert(c.to);
                    INFO("LISTING_SWITCH " + c.from + " -> " + c.to +
                         ": none, the predecessor never traded in this run");
                    continue;
                }
                if (in.hold[i_from] || in.hold[i_to] || !in.has_bar[i_from] || !in.has_bar[i_to] ||
                    !in.signalling[i_to] || !std::isfinite(in.target[i_to])) {
                    wait(i_from, i_to);
                    continue;
                }
                // the deferral band (section 5.2) on the predecessor's holding: the listed
                // contract's first-sleeve forecast is weaker than the band and against the holding
                const bool in_band = in.first_signalling[i_to] &&
                                     in.held[i_from] * in.first_forecast[i_to] < 0.0 &&
                                     std::abs(in.first_forecast[i_to]) < in.sign_band;
                due.push_back({c, i_from, i_to, in_band});
            }
            // The target the listed contract is entered at is the pass's own SCALED target, the
            // capped target times the overlay's scalar m (section 4), so that on a day the overlay
            // cuts the book the switch lands on the cut target and nothing is left to trim. m is a
            // reading of the target book with every due predecessor exited (a free row enters at
            // its target whatever is held), so it is taken from the pass itself, run once on that
            // book: the same m, cap and target the pass below computes.
            std::vector<double> scaled_target(n, 0.0);
            const ListingSwitchRule rule = ListingDates::instance().switch_rule();
            const bool target_rule =
                rule == ListingSwitchRule::kOpenAtTarget || rule == ListingSwitchRule::kCarryToTarget;
            if (target_rule && std::any_of(due.begin(), due.end(),
                                           [](const DueSwitch& d) { return !d.in_band; })) {
                one_pass::DayInputs probe = in;
                for (const auto& d : due) {
                    if (d.in_band) probe.held[d.i_to] += d.c.ratio * probe.held[d.i_from];
                    probe.held[d.i_from] = 0.0;
                }
                one_pass::DayResult dry;
                std::string dry_refusal;
                try {
                    dry = one_pass::rebalance(probe);
                    dry_refusal = dry.refusal;
                } catch (const std::exception& e) {
                    dry_refusal = e.what();
                }
                std::vector<DueSwitch> kept;
                for (const auto& d : due) {
                    // a pass that cannot be run, or a listed contract the pass would not treat as
                    // a free row, makes no switch today
                    if (!dry_refusal.empty() || (!d.in_band && !dry.free[d.i_to])) {
                        wait(d.i_from, d.i_to);
                        continue;
                    }
                    scaled_target[d.i_to] = dry.scaled_target[d.i_to];
                    kept.push_back(d);
                }
                due = std::move(kept);
            }
            for (const auto& d : due) {
                const ListingConversion& c = d.c;
                const size_t i_from = d.i_from;
                const size_t i_to = d.i_to;
                const bool in_band = d.in_band;
                const double u_to = in.multiplier[i_to] * in.close[i_to] / in.capital;
                // The BOOK's move (section 5.4: one pass on the portfolio book): the book's net
                // predecessor holding is exited and the listed contract entered at the book's
                // scaled target, whole contracts for the book.
                const double book_from = in.held[i_from];
                const double book_to = in.held[i_to];
                const ListingSwitch book = plan_listing_switch(
                    rule, c.ratio, book_from, book_to, scaled_target[i_to],
                    u_to > 0.0 ? in.cap / u_to : 0.0, in_band);
                listing_switched_.insert(c.to);
                switched_this_pass.push_back(c.to);
                const bool carried = rule == ListingSwitchRule::kConvert ||
                                     (target_rule && in_band && book_from != 0.0);
                if (rule == ListingSwitchRule::kCarryToTarget && book_from == 0.0 && !carried) {
                    continue;  // a pair the book holds none of is left to the pass
                }
                // The split to the sleeves, by the book's own rule: a carried holding is each
                // sleeve's own leg, ratio for one; otherwise the book's whole number is split in
                // proportion to the sleeves' unrounded contributions by largest remainder
                // (distribute_optimizer_contracts, the function the pass's own split calls), and
                // a row no sleeve contributes to that the book takes to flat leaves every sleeve
                // flat. Every sleeve's predecessor leg goes to flat.
                std::vector<double> sleeve_to(sids.size(), 0.0);
                if (carried) {
                    for (size_t s = 0; s < sids.size(); ++s) {
                        sleeve_to[s] = sleeve_held[s][i_to] + c.ratio * sleeve_held[s][i_from];
                    }
                } else {
                    double total_contribution = 0.0;
                    for (size_t s = 0; s < sids.size(); ++s) total_contribution += contribution[s][i_to];
                    const bool any_contribution = std::abs(total_contribution) > 1e-8;
                    if (any_contribution || book.new_to != 0.0) {
                        std::vector<SleeveContribution> parts;
                        for (size_t s = 0; s < sids.size(); ++s) {
                            parts.push_back({sids[s], any_contribution ? contribution[s][i_to]
                                                                       : sleeve_held[s][i_to]});
                        }
                        const SleeveDistribution split = distribute_optimizer_contracts(book.new_to, parts);
                        for (size_t s = 0; s < sids.size(); ++s) {
                            sleeve_to[s] = static_cast<double>(split.stored[s]);
                        }
                    }
                }
                // The rows as stored: one per sleeve and contract, each priced as if that sleeve
                // traded alone (ids LC- the predecessor's leg to flat, LO- the listed contract's
                // move), then netted as every bar's rows are (transaction_cost/netting.hpp): the
                // account sends ONE order per symbol, the signed sum of the sleeves' rows, and
                // each row's netting_adjustment is its share of what the account did not pay.
                const size_t first_pending = pending_switches.size();
                for (size_t s = 0; s < sids.size(); ++s) {
                    ListingSwitch plan;
                    plan.close_from = -sleeve_held[s][i_from];
                    plan.trade_to = sleeve_to[s] - sleeve_held[s][i_to];
                    plan.new_to = sleeve_to[s];
                    if (plan.close_from == 0.0 && plan.trade_to == 0.0) continue;
                    const std::string& sid = sids[s];
                    size_t seq = listing_leg_seq_[sid];
                    for (const auto& p : pending_switches) seq += p.sid == sid ? 1 : 0;
                    ListingConversion priced = c;
                    priced.from_close = in.close[i_from];
                    priced.to_close = in.close[i_to];
                    PendingListingSwitch pending{sid, c.from, c.to, plan.new_to, {}, {}};
                    pending.legs = make_listing_switch_fills(
                        priced, plan, as_of ? *as_of : data[0].timestamp,
                        "LC-" + sid + "-" + std::to_string(seq), "LO-" + sid + "-" + std::to_string(seq),
                        [this](const std::string& sym, double q, double px) {
                            const auto cost = cost_manager_.calculate_costs(sym, q, px);
                            return ListingLegCost{cost.commissions_fees, cost.implicit_price_impact,
                                                  cost.slippage_market_impact,
                                                  cost.total_transaction_costs};
                        });
                    for (const auto& leg : pending.legs) {
                        pending.lines.push_back(
                            "LISTING_LEG " + sid + " " + leg.symbol + " " +
                            (leg.side == Side::BUY ? "BUY" : "SELL") + " qty=" +
                            std::to_string(static_cast<double>(leg.filled_quantity)) + " px=" +
                            std::to_string(static_cast<double>(leg.fill_price)) + " cost=" +
                            std::to_string(static_cast<double>(leg.total_transaction_costs)) +
                            " id=" + leg.exec_id + " rule=" + to_string(rule) +
                            (in_band ? " (deferral band: carried)" : "") + " target=" +
                            std::to_string(in.target[i_to]) + " scaled=" +
                            std::to_string(scaled_target[i_to]) + " book " + c.from + " " +
                            std::to_string(book_from) + " -> 0, " + c.to + " " +
                            std::to_string(book_to) + " -> " + std::to_string(book.new_to) +
                            " (sleeve " + c.from + " " + std::to_string(sleeve_held[s][i_from]) +
                            " -> 0; " + c.to + " " + std::to_string(sleeve_held[s][i_to]) + " -> " +
                            std::to_string(plan.new_to) + ")");
                    }
                    pending_switches.push_back(std::move(pending));
                }
                {
                    std::vector<transaction_cost::SleeveExecution> rows;
                    for (size_t k = first_pending; k < pending_switches.size(); ++k) {
                        for (auto& leg : pending_switches[k].legs) rows.push_back({pending_switches[k].sid, &leg});
                    }
                    const auto netting = transaction_cost::apply_netting_adjustments(
                        rows, [this](const std::string& sym, double q, double px) {
                            return cost_manager_.calculate_costs(sym, q, px).total_transaction_costs;
                        });
                    if (first_pending < pending_switches.size()) {
                        for (const auto& line : netting.info_lines) {
                            pending_switches[first_pending].lines.push_back(line);
                        }
                    }
                    for (const auto& line : netting.warn_lines) WARN(line);
                }
                in.held[i_from] = 0.0;
                in.held[i_to] = 0.0;
                for (size_t s = 0; s < sids.size(); ++s) {
                    sleeve_held[s][i_from] = 0.0;
                    sleeve_held[s][i_to] = sleeve_to[s];
                    in.held[i_to] += sleeve_to[s];
                }
            }
        }

        // The pass. A sleeve its own risk module refused cannot be held apart from one search on
        // the summed book, so its refusal refuses the book; anything the arithmetic throws does too.
        auto held_book = [&](const std::string& why) {
            one_pass::DayResult held;
            held.u.assign(n, 0.0);
            for (auto* mask : {&held.free, &held.fixed, &held.band, &held.closeout,
                               &held.participant, &held.sign_closed, &held.clipped, &held.by_hold}) {
                mask->assign(n, 0);
            }
            for (auto* vector : {&held.capped_target, &held.scaled_target, &held.trimmed,
                                 &held.sign_fill, &held.rest_fill}) {
                vector->assign(n, 0.0);
            }
            // Every per-symbol vector the rebalance's record writes is sized: a refused day has a
            // record row too (nothing capped, the search's and the pre-trim book the held book).
            held.cap_bound.assign(n, 0);
            held.search_book = in.held;
            held.pre_trim = in.held;
            held.book = in.held;
            held.refusal = why;
            return held;
        };
        one_pass::DayResult result;
        if (!scope_refusal.empty()) {
            result = held_book(scope_refusal);
        } else if (sleeve_pinned) {
            result = held_book("a sleeve of the book was refused by its own risk module");
        } else {
            try {
                result = one_pass::rebalance(in);
            } catch (const std::exception& e) {
                result = held_book(std::string("the one pass failed: ") + e.what());
            }
        }
        if (!result.refusal.empty() && !switched_this_pass.empty()) {
            // a refused pass sends no order: the switch is undone and waits for a later pass
            const bool on_reread = result.refusal_on_reread;
            const std::string why = result.refusal;
            in.held = held_before_switch;
            sleeve_held = sleeve_held_before_switch;
            for (const auto& to : switched_this_pass) listing_switched_.erase(to);
            pending_switches.clear();
            result = held_book(why);
            result.refusal_on_reread = on_reread;
            WARN("LISTING_SWITCH undone: the pass was refused (" + why + "); no switch fill is written");
        }
        for (auto& pending : pending_switches) {
            ++listing_leg_seq_[pending.sid];
            for (auto& leg : pending.legs) strategy_executions_[pending.sid].push_back(std::move(leg));
            for (const auto& line : pending.lines) INFO(line);
            auto& ledger = filled_positions_[pending.sid];
            ledger[pending.from] = 0.0;
            ledger[pending.to] = pending.new_to;
        }
        const bool refused = !result.refusal.empty();
        for (size_t i = 0; i < n; ++i) {
            if (in.signalling[i]) ever_signalled_.insert(symbols[i]);
        }
        const std::string day = as_of ? core::format_utc_date(*as_of) : std::string("none");
        // The record's signal date: the date of the newest bar this call was fed.
        std::string signal_date = "none";
        if (!data.empty()) {
            Timestamp newest = data.front().timestamp;
            for (const auto& bar : data) newest = std::max(newest, bar.timestamp);
            signal_date = core::format_utc_date(newest);
        }
        one_pass::append_one_pass_record(id_, signal_date, day, is_warmup, symbols, in, result);

        // The log lines (section 7.7).
        Logger::register_component("RiskManager");
        INFO(one_pass::overlay_line(symbols, in, result));
        if (!refused && result.window.blind()) {
            // Section 4 (D27): a BLIND window is warned. The three covariance multipliers are 1 and
            // only the leverage term can cut.
            WARN("OVERLAY_BLIND complete_dates=" + std::to_string(result.window.complete_dates) +
                 " of " + std::to_string(result.window.window_dates) +
                 " window dates: the covariance readings are blind (multipliers 1), the leverage "
                 "term still applies");
        }
        if (!refused) {
            Logger::register_component("DynamicOptimizer");
            INFO(one_pass::optimiser_line(symbols, result));
            if (result.pass_capped) {
                WARN("OPTIMISER_PASS_CAP the search stopped at its pass cap after " +
                     std::to_string(result.passes) + " passes");
            }
            INFO(one_pass::book_line(symbols, in, result, slow_zeroed));
            const one_pass::KeptLines kept = one_pass::kept_lines(symbols, in, result);
            for (const auto& line : kept.info) INFO(line);
            for (const auto& line : kept.warn) WARN(line);
            // The marks of section 6.4 are the risk layer's lines, and the risk layer's tag is the
            // one the rebalance leaves registered, as it always was: every line a caller logs after
            // this call keeps the tag it had.
            Logger::register_component("RiskManager");
            if (!is_warmup && !result.over_limit.empty()) {
                WARN(one_pass::risk_trim_line(symbols, result));
            }
            if (!is_warmup && !result.by_hold_terms.empty()) {
                WARN(one_pass::over_limit_by_hold_line(symbols, result));
            }
        }
        for (const auto& symbol : unpriced) {
            // Left out of the pass: with no position there is nothing to hold; with one (a refused
            // scope) every sleeve keeps its held quantity.
            bool held_row = false;
            for (const auto& sid : sids) held_row = held_row || held_of(sid, symbol) != 0.0;
            WARN("BOOK_UNPRICED " + symbol +
                 (held_row ? ": it cannot be weighed today; left out of the pass, every sleeve "
                             "keeps its held quantity, no fill"
                           : ": the overlay sleeve has no series, close or multiplier for it and "
                             "no sleeve holds it; left out of the pass, not opened"));
        }
        for (const auto& line : unweighed_lines) WARN(line);

        // The record the runners store, and the decision row.
        OnePassDay record;
        record.ran = true;
        record.sized = !is_warmup;
        record.refused = refused;
        record.sizing_capital = in.capital;
        decision.module_id = overlay_module_id;
        if (refused) {
            phase = result.refusal_on_reread ? RiskPhase::POST_ROUNDING : RiskPhase::LAP;
            if (sleeve_pinned) phase = RiskPhase::LAP;
            decision.action = RiskAction::REFUSE;
            decision.reason = result.refusal;
            applied = RiskAction::REFUSE;
            failure = result.refusal;
            ERROR("Risk overlay refused portfolio " + id_ + ": " + result.refusal +
                  "; every sleeve is held at its previous book and no order is sent");
            unseeded = !scope_is_seeded(id_);
        } else {
            record.risk_requested = result.multiplier.m;
            record.binding_term = result.multiplier.binding;
            record.overlay_blind = result.window.blind();
            for (const auto& [term, excess] : result.over_limit) {
                (void)excess;
                record.over_limit_after_rounding_terms +=
                    (record.over_limit_after_rounding_terms.empty() ? "" : ";") + term;
            }
            record.over_limit_after_rounding_excess = result.over_limit_excess_units;
            for (const auto& term : result.by_hold_terms) {
                record.over_limit_by_hold_terms +=
                    (record.over_limit_by_hold_terms.empty() ? "" : ";") + term;
            }
            for (size_t i = 0; i < n; ++i) {
                if (!result.by_hold[i]) continue;
                record.over_limit_by_hold_symbols +=
                    (record.over_limit_by_hold_symbols.empty() ? "" : " ") + symbols[i];
            }
            record.risk_scale = result.risk_scale;
            record.capped_target_gross = result.target_gross * in.capital;
            record.stored_gross = result.stored_gross * in.capital;
            one_pass_weights_.clear();
            for (const std::size_t i : result.participants) {
                one_pass_weights_.emplace_back(symbols[i], result.u[i]);
            }
            one_pass_target_gross_ = result.target_gross;
            decision.blind = record.overlay_blind;
            if (result.multiplier.m < 1.0) {
                decision.action = RiskAction::SCALE;
                decision.scale = result.multiplier.m;
                applied = RiskAction::SCALE;
                applied_factor = Decimal(result.multiplier.m);
            }
            // The book the delivered scale is measured against, and its notionals per contract.
            for (size_t i = 0; i < n; ++i) {
                delivered_npc_[symbols[i]] = in.multiplier[i] * in.close[i];
                if (!result.participant[i]) continue;
                delivered_lap1_book_[symbols[i]] =
                    result.free[i] ? result.capped_target[i]
                                   : (result.closeout[i] ? 0.0 : in.held[i]);
            }
            delivered_has_lap1_ = true;
        }

        // The split back to the sleeves (section 5.4) and each sleeve's new book. A row the pass
        // did not move (a held row, a refused day, an unpriced symbol) keeps every sleeve's held
        // quantity; a moved row is split in proportion to the sleeves' unrounded contributions by
        // largest remainder.
        std::vector<std::vector<double>> sleeve_new = sleeve_held;
        if (!refused) {
            for (size_t i = 0; i < n; ++i) {
                if (!result.free[i] && !result.closeout[i]) continue;
                // A row with no contribution is a symbol no sleeve targets any more (a zero
                // forecast, a close-out) or one whose contributions sum to nothing a share can be
                // formed on (|sum N*| <= 1e-8). Its book is split in proportion to what each sleeve
                // holds; taken to flat, every sleeve stores 0.
                double total_contribution = 0.0;
                for (size_t s = 0; s < sids.size(); ++s) total_contribution += contribution[s][i];
                const bool any_contribution = std::abs(total_contribution) > 1e-8;
                if (!any_contribution && result.book[i] == 0.0) {
                    for (size_t s = 0; s < sids.size(); ++s) sleeve_new[s][i] = 0.0;
                    continue;
                }
                std::vector<SleeveContribution> parts;
                for (size_t s = 0; s < sids.size(); ++s) {
                    parts.push_back(
                        {sids[s], any_contribution ? contribution[s][i] : sleeve_held[s][i]});
                }
                const SleeveDistribution split = distribute_optimizer_contracts(result.book[i], parts);
                for (size_t s = 0; s < sids.size(); ++s) {
                    sleeve_new[s][i] = static_cast<double>(split.stored[s]);
                }
            }
        }
        for (size_t s = 0; s < sids.size(); ++s) {
            auto& info = strategies_.at(sids[s]);
            const auto prev_book = prev_positions.find(sids[s]);
            std::unordered_map<std::string, Position> book;
            // The row is the sleeve's own row of today (its mark and its fields, as the book always
            // carried them) with the pass's quantity; a symbol the sleeve no longer lists keeps the
            // row it was held with.
            auto row = [&](const std::string& symbol, double quantity) {
                Position pos;
                const auto target = info.target_positions.find(symbol);
                if (target != info.target_positions.end()) {
                    pos = target->second;
                } else if (prev_book != prev_positions.end()) {
                    const auto p = prev_book->second.find(symbol);
                    if (p != prev_book->second.end()) pos = p->second;
                }
                pos.symbol = symbol;
                pos.quantity = Decimal(quantity);
                book[symbol] = pos;
            };
            for (size_t i = 0; i < n; ++i) {
                const bool listed_by_sleeve = info.target_positions.count(symbols[i]) > 0;
                if (!listed_by_sleeve && sleeve_new[s][i] == 0.0 && sleeve_held[s][i] == 0.0) continue;
                row(symbols[i], sleeve_new[s][i]);
            }
            for (const auto& symbol : unpriced) {
                const double held = held_of(sids[s], symbol);
                if (held == 0.0 && info.target_positions.count(symbol) == 0) continue;
                row(symbol, held);
            }
            info.target_positions = book;
            info.current_positions = std::move(book);
            if (refused) pinned_scopes_.insert(sids[s]);
        }

        // The fills (section 6.3), on a rebalance that trades: per sleeve and symbol, the
        // forecast-sign close to flat and then the move to the new book, each at the signal close
        // and priced by this manager's cost model. The sign closes of one symbol are netted among
        // the sleeves, and the other fills among themselves; ROLL legs are never in either set.
        if (!refused && !is_warmup) {
            std::vector<std::pair<std::string, size_t>> sign_rows, rest_rows;
            for (size_t s = 0; s < sids.size(); ++s) {
                const std::string& sid = sids[s];
                auto& execs = strategy_executions_[sid];
                auto& ledger = filled_positions_[sid];
                int counter = static_cast<int>(
                    std::count_if(execs.begin(), execs.end(), [](const ExecutionReport& e) {
                        return e.execution_type != ExecutionType::ROLL;
                    }));
                auto fill = [&](const std::string& symbol, double quantity, double price) {
                    ExecutionReport exec;
                    exec.order_id = "PM-" + sid + "-" + std::to_string(counter);
                    exec.exec_id = "EX-" + sid + "-" + std::to_string(counter);
                    exec.symbol = symbol;
                    exec.side = quantity > 0.0 ? Side::BUY : Side::SELL;
                    exec.filled_quantity = std::abs(quantity);
                    exec.fill_price = price;
                    exec.fill_time = as_of ? *as_of
                                           : (data.empty() ? std::chrono::system_clock::now()
                                                           : data[0].timestamp);
                    const auto cost = cost_manager_.calculate_costs(symbol, quantity, price);
                    exec.commissions_fees = Decimal(cost.commissions_fees);
                    exec.implicit_price_impact = Decimal(cost.implicit_price_impact);
                    exec.slippage_market_impact = Decimal(cost.slippage_market_impact);
                    exec.total_transaction_costs = Decimal(cost.total_transaction_costs);
                    exec.is_partial = false;
                    execs.push_back(exec);
                    ++counter;
                };
                for (size_t i = 0; i < n; ++i) {
                    double from = sleeve_held[s][i];
                    if (result.sign_closed[i] && from != 0.0) {
                        fill(symbols[i], -from, in.close[i]);
                        sign_rows.emplace_back(sid, execs.size() - 1);
                        record.sign_closes[sid][symbols[i]] = -from;
                        from = 0.0;
                    }
                    if (sleeve_new[s][i] != from) {
                        fill(symbols[i], sleeve_new[s][i] - from, in.close[i]);
                        rest_rows.emplace_back(sid, execs.size() - 1);
                    }
                    if (sleeve_new[s][i] != sleeve_held[s][i] || ledger.count(symbols[i]) > 0) {
                        ledger[symbols[i]] = sleeve_new[s][i];
                    }
                }
            }
            if (is_backtest_) {
                for (auto* group : {&sign_rows, &rest_rows}) {
                    std::vector<transaction_cost::SleeveExecution> bar_rows;
                    for (const auto& [sid, index] : *group) {
                        bar_rows.push_back({sid, &strategy_executions_[sid][index]});
                    }
                    const auto netting = transaction_cost::apply_netting_adjustments(
                        bar_rows, [this](const std::string& sym, double q, double px) {
                            return cost_manager_.calculate_costs(sym, q, px).total_transaction_costs;
                        });
                    for (const auto& line : netting.info_lines) INFO(line);
                    for (const auto& line : netting.warn_lines) WARN(line);
                }
            }
        }
        one_pass_day_ = std::move(record);
    }

    RiskContext ctx = make_risk_context(phase, 1, RiskScope::PORTFOLIO, id_, sizing_capital_, data,
                                        as_of, is_warmup);
    record_risk_decision(ctx, overlay_module_id, std::move(decision), applied, applied_factor, false,
                         failure);
    if (unseeded) {
        return make_error<void>(ErrorCode::RISK_LIMIT_EXCEEDED,
                                "The risk overlay refused portfolio " + id_ +
                                    ", whose previous book was never seeded; refusing the run "
                                    "rather than shipping a flat book",
                                "PortfolioManager");
    }
    return Result<void>();
}

RiskContext PortfolioManager::make_risk_context(RiskPhase phase, int lap, RiskScope scope,
                                                const std::string& scope_id, Decimal capital,
                                                const std::vector<Bar>& data,
                                                std::optional<Timestamp> as_of,
                                                bool is_warmup) const {
    RiskContext ctx;
    ctx.phase = phase;
    ctx.lap = lap;
    ctx.as_of = as_of;
    ctx.is_backtest = is_backtest_;
    ctx.is_warmup = is_warmup;
    ctx.capital = capital;
    ctx.portfolio_id = id_;
    ctx.scope = scope;
    ctx.scope_id = scope_id;
    ctx.bars = &data;
    ctx.applied = rebalance_applied_;
    ctx.gate_participants = &gate_participants_;
    return ctx;
}

Result<void> PortfolioManager::validate_risk_modules(
    const std::vector<RiskModulePtr>& portfolio_modules,
    const std::unordered_map<std::string, std::vector<RiskModulePtr>>& sleeve_modules,
    const std::unordered_set<std::string>* known_strategy_ids) {
    auto invalid = [](const std::string& why) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, why, "PortfolioManager");
    };
    // One scope: no null, unique ids, at most one REPLACE-capable module. Returns the number of
    // COMPOSITION-term modules, or -1 with `why` set.
    auto check_scope = [](const std::vector<RiskModulePtr>& modules, const std::string& scope,
                          std::string& why) -> int {
        std::set<std::string> ids;
        int replace_capable = 0;
        int composition = 0;
        for (const auto& module : modules) {
            if (!module) {
                why = "Risk module cannot be null (" + scope + ")";
                return -1;
            }
            if (!ids.insert(module->id()).second) {
                why = "Duplicate risk module id: " + module->id() + " (" + scope + ")";
                return -1;
            }
            if (module->capabilities().count(RiskAction::REPLACE)) ++replace_capable;
            if (module->terms().count(RiskTerm::COMPOSITION)) ++composition;
        }
        if (replace_capable > 1) {
            why = "More than one REPLACE-capable risk module (" + scope + ")";
            return -1;
        }
        return composition;
    };

    std::string why;
    const int portfolio_composition = check_scope(portfolio_modules, "portfolio", why);
    if (portfolio_composition < 0) return invalid(why);
    if (portfolio_composition > 1) {
        return invalid("More than one COMPOSITION-term risk module at the portfolio scope; the "
                       "scale-invariant terms would be counted twice");
    }

    // Sleeves in a stable order: the map is unordered and the FIRST offending key must be the
    // one named, whatever the standard library iterates.
    std::vector<std::string> sids;
    sids.reserve(sleeve_modules.size());
    for (const auto& [sid, modules] : sleeve_modules) {
        (void)modules;
        sids.push_back(sid);
    }
    std::sort(sids.begin(), sids.end());
    for (const auto& sid : sids) {
        if (known_strategy_ids != nullptr && !known_strategy_ids->count(sid)) {
            return invalid("Sleeve risk modules for an unregistered strategy: " + sid);
        }
        const int composition = check_scope(sleeve_modules.at(sid), "sleeve " + sid, why);
        if (composition < 0) return invalid(why);
        // C-2, here as well as in the loader (T-6b INTERIM ADVERSARIAL C-2): the Carver module
        // divides by the PORTFOLIO's capital and ignores RiskContext::capital, so at sleeve scope
        // its leverage limits would be read against the whole book's money. The loader refuses it
        // in a risk.json; this refuses it on the constructor and set_risk_modules paths too.
        for (const auto& module : sleeve_modules.at(sid)) {
            if (module->type() == "carver") {
                return invalid("Risk module " + module->id() + " on sleeve " + sid +
                               " is type \"carver\", which is only valid at portfolio scope: the "
                               "Carver gate divides by the portfolio's capital");
            }
        }
        if (composition + portfolio_composition > 1) {
            return invalid("More than one COMPOSITION-term risk module along the chain of sleeve " +
                           sid + " and the portfolio; the scale-invariant terms would be counted "
                           "twice");
        }
    }
    return Result<void>();
}

Result<void> PortfolioManager::validate_sleeve_keys_once() {
    if (sleeve_keys_validated_) return Result<void>();
    std::unordered_set<std::string> registered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, info] : strategies_) {
            (void)info;
            registered.insert(sid);
        }
    }
    auto valid = validate_risk_modules(risk_modules_, sleeve_risk_modules_, &registered);
    if (valid.is_error()) return valid;
    sleeve_keys_validated_ = true;
    return Result<void>();
}

Result<void> PortfolioManager::set_risk_modules(
    std::vector<RiskModulePtr> portfolio_modules,
    std::unordered_map<std::string, std::vector<RiskModulePtr>> sleeve_modules) {
    std::unordered_set<std::string> registered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, info] : strategies_) {
            (void)info;
            registered.insert(sid);
        }
    }
    auto valid = validate_risk_modules(portfolio_modules, sleeve_modules, &registered);
    if (valid.is_error()) return valid;
    std::lock_guard<std::mutex> lock(mutex_);
    risk_modules_ = std::move(portfolio_modules);
    sleeve_risk_modules_ = std::move(sleeve_modules);
    // The keys were just checked against the registered strategies.
    sleeve_keys_validated_ = true;
    return Result<void>();
}

DeliveredCut PortfolioManager::last_delivered_cut() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return delivered_cut_;
}

DeliveredCut PortfolioManager::delivered_cut_for_book(
    const std::map<std::string, double>& stored_book) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!delivered_cut_.has_final) return delivered_cut_;
    std::map<std::string, double> npc = delivered_npc_;
    std::set<std::string> missing;
    for (const auto& [symbol, q] : stored_book) {
        if (q != 0.0 && npc.find(symbol) == npc.end()) missing.insert(symbol);
    }
    if (!missing.empty()) {
        for (const auto& [symbol, v] : delivered_notional_per_contract(missing)) npc[symbol] = v;
    }
    return measure_delivered_cut(delivered_has_lap1_ ? &delivered_lap1_book_ : nullptr, stored_book,
                                 npc);
}

double PortfolioManager::delivered_scale_for_book(
    const std::map<std::string, double>& stored_book) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!(one_pass_target_gross_ > 0.0)) return 1.0;
    // The pass's own sum, in its order, so an unchanged book reads the pass's figure to the bit.
    double stored_gross = 0.0;
    for (const auto& [symbol, u] : one_pass_weights_) {
        const auto row = stored_book.find(symbol);
        const double quantity = row == stored_book.end() ? 0.0 : row->second;
        stored_gross += std::abs(quantity * u);
    }
    return stored_gross / one_pass_target_gross_;
}

std::map<std::string, double> PortfolioManager::delivered_notional_per_contract(
    const std::set<std::string>& symbols) const {
    std::map<std::string, double> out;
    // The trend sleeves' own contract size and last price. A book that names an overlay sleeve
    // reads THAT sleeve alone (the sleeve the pass weighs the book on); any other book reads its
    // sleeves in id order, the first that holds the symbol. Never whichever sleeve an unordered
    // walk visits last: two sleeves can carry different rows for one symbol.
    std::unordered_map<std::string, const InstrumentData*> trend_data;
    std::vector<std::string> sleeve_ids;
    if (!config_.overlay_sleeve.empty() && strategies_.count(config_.overlay_sleeve) > 0) {
        sleeve_ids.push_back(config_.overlay_sleeve);
    } else {
        for (const auto& [id, info] : strategies_) {
            (void)info;
            sleeve_ids.push_back(id);
        }
        std::sort(sleeve_ids.begin(), sleeve_ids.end());
    }
    for (const auto& id : sleeve_ids) {
        auto trend_strategy =
            std::dynamic_pointer_cast<TrendFollowingStrategy>(strategies_.at(id).strategy);
        if (!trend_strategy) continue;
        for (const auto& [symbol, data] : trend_strategy->get_all_instrument_data()) {
            trend_data.emplace(symbol, &data);
        }
    }
    for (const auto& symbol : symbols) {
        auto t = trend_data.find(symbol);
        if (t != trend_data.end() && !t->second->price_history.empty()) {
            out[symbol] = t->second->contract_size * t->second->price_history.back();
            continue;
        }
        auto c = closes_by_date_.find(symbol);
        if (c == closes_by_date_.end() || c->second.empty()) continue;
        double multiplier = 1.0;
        if (registry_ && registry_->has_instrument(symbol)) {
            auto instrument = registry_->get_instrument(symbol);
            if (instrument) multiplier = instrument->get_multiplier();
        }
        out[symbol] = c->second.rbegin()->second * multiplier;
    }
    return out;
}

std::vector<RiskDecisionRecord> PortfolioManager::last_risk_decisions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return risk_decisions_;
}

nlohmann::json PortfolioManager::risk_decisions_json() const {
    std::vector<nlohmann::json> modules;
    std::vector<RiskDecisionRecord> records;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& module : risk_modules_) {
            nlohmann::json m = module->describe();
            m["scope"] = risk_scope_name(RiskScope::PORTFOLIO);
            m["scope_id"] = id_;
            modules.push_back(std::move(m));
        }
        // Sleeves in a stable order (strategies_ is unordered).
        std::vector<std::string> sids;
        for (const auto& [sid, _] : sleeve_risk_modules_) sids.push_back(sid);
        std::sort(sids.begin(), sids.end());
        for (const auto& sid : sids) {
            for (const auto& module : sleeve_risk_modules_.at(sid)) {
                nlohmann::json m = module->describe();
                m["scope"] = risk_scope_name(RiskScope::SLEEVE);
                m["scope_id"] = sid;
                modules.push_back(std::move(m));
            }
        }
        records = risk_decisions_;
    }
    return build_risk_decisions_json(modules, records);
}

namespace {

// HD's ruling of 2026-09-18: "the applied risk scale is logged per lap beside the reporter's
// value now" (STAGE3_PLAN line 839; T-6a ADVERSARIAL E-1 found no line did it). One line per
// scope per lap.
//
//   requested   what the winning module ASKED for (1 when nothing asked)
//   applied     the QUANTISED factor the book was actually multiplied by, double(verdict.factor)
//               -- 1 when nothing was multiplied, which is the value the WARN above never showed
//   cumulative  the product of those factors for this scope so far this rebalance
//
// The first four fields keep T4_APPLIED's spelling and order on purpose, so t4_parse.py,
// c6a_gatecheck.py and overlev*.py need a token rename rather than a new regex. %.17g, never
// to_string: six decimals already nearly published a false finding once
// (feedback_measure_definitions_change_meaning), and this is the number a cut is judged by.
std::string risk_applied_line(int lap, double requested, double applied, double cumulative,
                              RiskAction action, const std::string& module_id, RiskScope scope,
                              const std::string& scope_id, double invariant, double leverage) {
    char buf[640];
    // invariant = min(VaR, jump, correlation), the COMPOSITION reading the level rule treats as
    // an absolute request; leverage = the MAGNITUDE reading charged as a per-lap rate. They are
    // the two halves of `requested`, and the pair is what makes a level-cut arm checkable: with
    // only the combined number there is no way to tell a declined invariant request from a
    // leverage request that was honoured (T-4f ARM 1; t4_validate.py reads this line).
    std::snprintf(buf, sizeof(buf),
                  "RISK_APPLIED lap=%d requested=%.17g applied=%.17g cumulative=%.17g action=%s"
                  " module=%s scope=%s scope_id=%s invariant=%.17g leverage=%.17g",
                  lap, requested, applied, cumulative, risk_action_name(action),
                  module_id.empty() ? "-" : module_id.c_str(), risk_scope_name(scope),
                  scope_id.c_str(), invariant, leverage);
    return std::string(buf);
}

// Where a risk decision was taken, for the log lines that name it.
std::string risk_location(const RiskContext& ctx) {
    switch (ctx.phase) {
        case RiskPhase::LAP:
            return "in iteration " + std::to_string(ctx.lap);
        case RiskPhase::SLEEVE:
            return "before the loop";
        case RiskPhase::POST_ROUNDING:
            return "after rounding";
        case RiskPhase::REBALANCE_START:
            break;
    }
    return "at the rebalance start";
}

}  // namespace

void PortfolioManager::record_risk_decision(const RiskContext& ctx, const std::string& module_id,
                                            RiskDecision requested, RiskAction applied_action,
                                            Decimal applied_factor, bool empty_book,
                                            std::string error) {
    RiskDecisionRecord rec;
    rec.phase = ctx.phase;
    rec.lap = ctx.lap;
    rec.scope = ctx.scope;
    rec.scope_id = ctx.scope_id;
    rec.module_id = module_id;
    rec.requested = std::move(requested);
    rec.applied_action = applied_action;
    rec.applied_factor = applied_factor;
    rec.empty_book = empty_book;
    rec.error = std::move(error);
    std::lock_guard<std::mutex> lock(mutex_);
    risk_decisions_.push_back(std::move(rec));
}

PortfolioManager::RiskVerdict PortfolioManager::combine_risk_decisions(
    const std::vector<RiskDecision>& decisions, const RiskContext& ctx) const {
    RiskVerdict v;
    v.rows.assign(decisions.size(), {RiskAction::NONE, Decimal(1.0)});
    const size_t none = static_cast<size_t>(-1);
    size_t refuse = none, replace = none, scale = none;

    // Module order: an invalid SCALE is an ERROR and a WARN is logged; neither touches the book.
    for (size_t k = 0; k < decisions.size(); ++k) {
        const RiskDecision& d = decisions[k];
        switch (d.action) {
            case RiskAction::SCALE:
                // Applied iff 0 <= scale < 1 (NaN fails both): the test the gate's own
                // risk_exceeded is. Within a scope the applied SCALE is the MIN, never a product.
                if (d.scale >= 0.0 && d.scale < 1.0) {
                    if (scale == none || d.scale < decisions[scale].scale) scale = k;
                } else {
                    ERROR("Risk module " + d.module_id + " returned SCALE " +
                          std::to_string(d.scale) + "; not applied");
                }
                break;
            case RiskAction::WARN:
                WARN("Risk module " + d.module_id + " warning on " + risk_scope_name(ctx.scope) +
                     " " + ctx.scope_id + " " + risk_location(ctx) + ": " + d.reason);
                v.rows[k].first = RiskAction::WARN;
                v.action = RiskAction::WARN;
                break;
            case RiskAction::REFUSE:
                if (refuse == none) refuse = k;
                break;
            case RiskAction::REPLACE:
                if (replace == none) replace = k;
                break;
            case RiskAction::NONE:
                break;
        }
    }

    // Precedence REFUSE > REPLACE > SCALE > WARN > NONE; a losing request is recorded, not applied.
    if (refuse != none) {
        v.action = RiskAction::REFUSE;
        v.winner = refuse;
    } else if (replace != none) {
        v.action = RiskAction::REPLACE;
        v.winner = replace;
    } else if (scale != none) {
        v.action = RiskAction::SCALE;
        v.winner = scale;
        v.scale = decisions[scale].scale;
        v.factor = Decimal(v.scale);
    }
    if (v.winner != none) {
        v.rows[v.winner] = {v.action, v.action == RiskAction::SCALE ? v.factor : Decimal(1.0)};
    }
    return v;
}

std::vector<RiskDecision> PortfolioManager::evaluate_scope_modules(
    std::vector<RiskModulePtr>& modules, const std::unordered_map<std::string, Position>& book,
    const RiskContext& ctx, std::vector<std::string>& errors) {
    std::vector<RiskDecision> decisions;
    decisions.reserve(modules.size());
    errors.assign(modules.size(), std::string());
    for (size_t k = 0; k < modules.size(); ++k) {
        RiskDecision decision;
        decision.module_id = modules[k]->id();
        std::string failure;
        try {
            auto result = modules[k]->evaluate(book, ctx);
            if (result.is_error()) {
                failure = result.error()->what();
            } else {
                decision = result.value();
                decision.module_id = modules[k]->id();
            }
        } catch (const std::exception& e) {
            // A THROWING module is the same failure reached a different way. It used to escape
            // to the catch around the whole apply, which returned OK and threw away every
            // decision the healthy modules had already returned.
            failure = e.what();
        }
        if (!failure.empty()) {
            ERROR("Risk management calculation failed: " + failure);
            decision = RiskDecision();
            decision.module_id = modules[k]->id();
            errors[k] = std::move(failure);
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

bool PortfolioManager::refuse_on_failed_gatekeeper(const std::vector<RiskModulePtr>& modules,
                                                   const std::vector<std::string>& errors,
                                                   const RiskContext& ctx, RiskVerdict& verdict,
                                                   std::string& module_id) const {
    if (verdict.action == RiskAction::REFUSE) return false;
    const bool portfolio_scope = ctx.scope == RiskScope::PORTFOLIO;
    for (size_t k = 0; k < modules.size() && k < errors.size(); ++k) {
        if (errors[k].empty()) continue;
        if (portfolio_scope) {
            // HD 2026-09-21, option (b): a PORTFOLIO-scope module of ANY capability that cannot
            // answer refuses the scope. The shipped futures books run one Carver module
            // ({SCALE, WARN}); its failure used to contribute NONE and ship the book uncut.
            ERROR("Risk module " + modules[k]->id() + " failed on " + risk_scope_name(ctx.scope) +
                  " " + ctx.scope_id + " " + risk_location(ctx) + ": " + errors[k] +
                  "; a portfolio-scope module that cannot answer refuses the scope: every "
                  "strategy is held at its previous book and no orders are sent");
        } else {
            // T-7b-2 C10b (HD 2026-09-24 ruling 18): a SLEEVE-scope module of ANY capability that
            // cannot answer refuses its sleeve, as the portfolio rule does for the book: the
            // sleeve is held at its previous (seeded) book and sends no orders, the other sleeves
            // go on. Until C10b only a REFUSE-capable module refused here, and a failed
            // SCALE-only sleeve module left its sleeve uncut.
            ERROR("Risk module " + modules[k]->id() + " failed on " + risk_scope_name(ctx.scope) +
                  " " + ctx.scope_id + " " + risk_location(ctx) + ": " + errors[k] +
                  "; a sleeve-scope module that cannot answer refuses its sleeve: the sleeve is "
                  "held at its previous book and sends no orders, the other sleeves go on");
        }
        verdict.action = RiskAction::REFUSE;
        verdict.winner = static_cast<size_t>(-1);
        verdict.scale = 1.0;
        verdict.factor = Decimal(1.0);
        for (auto& row : verdict.rows) row = {RiskAction::NONE, Decimal(1.0)};
        // The failed module's row is the refusal: recorded as applied REFUSE with its error, so
        // risk_decisions_json()'s outcome and the runners' metadata mark see it.
        if (k < verdict.rows.size()) verdict.rows[k] = {RiskAction::REFUSE, Decimal(1.0)};
        module_id = modules[k]->id();
        return true;
    }
    return false;
}

void PortfolioManager::deliver_and_record(const std::vector<RiskModulePtr>& modules,
                                          std::vector<RiskDecision>& decisions,
                                          const RiskVerdict& verdict, const RiskContext& ctx,
                                          bool pinned, const std::vector<std::string>& errors,
                                          size_t scopes_skipped) {
    auto failed = [&errors](size_t k) { return k < errors.size() && !errors[k].empty(); };
    for (size_t k = 0; k < decisions.size() && k < modules.size(); ++k) {
        // A module that failed was never evaluated: there is no decision of its own for the PM
        // to report back, and a module counting its callbacks must not be told it was applied.
        if (failed(k)) continue;
        RiskApplied applied;
        applied.action = verdict.action;
        applied.requested_scale =
            decisions[k].action == RiskAction::SCALE ? decisions[k].scale : 1.0;
        applied.factor = verdict.action == RiskAction::SCALE ? verdict.factor : Decimal(1.0);
        applied.won = verdict.rows[k].first != RiskAction::NONE;
        applied.pinned = pinned;
        applied.scopes_skipped = scopes_skipped;
        applied.partial = scopes_skipped > 0 && verdict.action == RiskAction::SCALE;
        modules[k]->on_applied(applied, ctx);
    }
    for (size_t k = 0; k < decisions.size(); ++k) {
        std::string id = decisions[k].module_id;
        const bool bad = failed(k);
        // A failed module's row is NONE unless its failure refused the scope
        // (refuse_on_failed_gatekeeper marks that row REFUSE).
        const RiskAction bad_action =
            k < verdict.rows.size() && verdict.rows[k].first == RiskAction::REFUSE
                ? RiskAction::REFUSE
                : RiskAction::NONE;
        record_risk_decision(ctx, id, std::move(decisions[k]),
                             bad ? bad_action : verdict.rows[k].first,
                             bad ? Decimal(1.0) : verdict.rows[k].second, false,
                             bad ? errors[k] : std::string());
    }
}

Result<void> PortfolioManager::apply_risk_management(const std::vector<Bar>& data,
                                                     const RiskContext& lap_ctx,
                                                     RiskLapOutcome& outcome) {
    Logger::register_component("RiskManager");

    if (risk_modules_.empty()) {
        WARN("Risk manager not initialized, skipping risk management");
        return Result<void>();
    } else {
        INFO("Using risk manager");
    }

    try {
        // Every module sees this lap's bars before the book is built and before the
        // empty-book return (the Carver module appends them to its window, trims it and
        // builds its MarketData here).
        for (auto& module : risk_modules_) {
            module->on_bars(data, lap_ctx);
        }

        // Collect aggregated portfolio positions for risk evaluation.
        // Use simple per-strategy sum (Σ qᵢ) instead of get_portfolio_positions(),
        // which scales by allocation a second time. Strategies size for their
        // capital slice already, so the broker holds Σ qᵢ — that's what risk
        // checks (VaR, gross leverage, correlation, jump) must operate on.
        std::unordered_map<std::string, Position> portfolio_positions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [_, info] : strategies_) {
                for (const auto& [symbol, pos] : info.target_positions) {
                    auto it = portfolio_positions.find(symbol);
                    if (it == portfolio_positions.end()) {
                        portfolio_positions[symbol] = pos;
                    } else {
                        it->second.quantity += pos.quantity;
                    }
                }
            }
        }

        // Check if we have positions to process
        if (portfolio_positions.empty()) {
            INFO("No positions to apply risk management to");
            for (const auto& module : risk_modules_) {
                RiskDecision none;
                none.module_id = module->id();
                record_risk_decision(lap_ctx, module->id(), std::move(none), RiskAction::NONE,
                                     Decimal(1.0), true, "");
            }
            return Result<void>();
        }

        // Apply risk management with proper error handling
        try {
            // Fail CLOSED: every module is evaluated, a failing one contributes NONE and its
            // message, and the scope combines what the healthy modules returned. A REFUSE that
            // one module already returned is applied even if a later module then fails.
            std::vector<std::string> errors;
            std::vector<RiskDecision> decisions =
                evaluate_scope_modules(risk_modules_, portfolio_positions, lap_ctx, errors);

            // T-7b-1 7d: the optimizer's and the risk gate's max |rho| side by side, once per
            // rebalance. Lap 1 is the one lap every rebalance with a book has; both numbers are
            // then read on the same book (the optimizer's answer, before any cut), the optimizer's
            // matrix is the one it built this rebalance (cached for the later laps), and the gate's
            // reading is scale-free, so a later lap's cut would not change it unless a position
            // rounded to zero. optimizer= is over the held pairs (both legs non-zero) that the
            // optimizer's matrix covers, the pairs the gate's correlation term reads;
            // optimizer_all= over every pair of the optimizer's matrix. Not printed when the
            // optimizer built no matrix this rebalance (the optimizer is off, as on the equity
            // books, or it had no symbols).
            if (lap_ctx.lap == 1 && covariance_cache_valid_) {
                std::set<std::string> held;
                for (const auto& symbol : cached_symbols_) {
                    auto it = portfolio_positions.find(symbol);
                    if (it != portfolio_positions.end() &&
                        std::abs(static_cast<double>(it->second.quantity)) > 1e-12) {
                        held.insert(symbol);
                    }
                }
                std::string pair_held;
                std::string pair_all;
                const double rho_held =
                    covariance_max_abs_rho(cached_symbols_, cached_covariance_, &held, pair_held);
                const double rho_all =
                    covariance_max_abs_rho(cached_symbols_, cached_covariance_, nullptr, pair_all);
                std::string gate = "-";
                std::string gate_module = "-";
                std::string gate_blind = "-";
                for (const auto& d : decisions) {
                    if (!d.metrics.has_value()) continue;
                    gate = std::to_string(static_cast<double>(d.metrics->correlation_risk));
                    gate_module = d.module_id.empty() ? "-" : d.module_id;
                    gate_blind = d.blind ? "1" : "0";
                    break;
                }
                INFO("COVARIANCE_MAX_RHO held=" + std::to_string(held.size()) +
                     " optimizer=" + covariance_rho_text(rho_held) + " optimizer_pair=" + pair_held +
                     " optimizer_all=" + covariance_rho_text(rho_all) +
                     " optimizer_all_pair=" + pair_all + " gate=" + gate +
                     " gate_module=" + gate_module + " gate_blind=" + gate_blind +
                     ": max |rho| on lap 1's book, the optimizer's date-aligned covariance beside "
                     "the risk gate's own window; once per rebalance");
            }

            // The decisions are applied by action (precedence REFUSE > REPLACE > SCALE > WARN).
            RiskVerdict verdict = combine_risk_decisions(decisions, lap_ctx);
            std::string failed_gatekeeper;
            const bool refused_by_failure = refuse_on_failed_gatekeeper(
                risk_modules_, errors, lap_ctx, verdict, failed_gatekeeper);
            bool pinned = false;
            // Strategies the multiply below skipped because a sleeve module already pinned them.
            size_t scopes_skipped = 0;
            if (verdict.action == RiskAction::REFUSE || verdict.action == RiskAction::REPLACE) {
                static const RiskDecision kNoDecision{};
                const RiskDecision& d =
                    verdict.winner < decisions.size() ? decisions[verdict.winner] : kNoDecision;
                const std::string winner_id =
                    refused_by_failure ? failed_gatekeeper : d.module_id;
                if (verdict.action == RiskAction::REFUSE &&
                    !scope_is_seeded(lap_ctx.scope_id)) {
                    ERROR("Risk module " + winner_id + " refused " +
                          risk_scope_name(lap_ctx.scope) + " " + lap_ctx.scope_id + " " +
                          risk_location(lap_ctx) +
                          ", but this scope's previous book was never seeded: pinning would ship "
                          "a FLAT book, not yesterday's. Seed it with update_strategy_position "
                          "before process_market_data.");
                    deliver_and_record(risk_modules_, decisions, verdict, lap_ctx, false, errors);
                    outcome.refuse_unseeded = true;
                    outcome.unseeded_scope = lap_ctx.scope_id;
                    outcome.module_id = winner_id;
                    return Result<void>();
                }
                if (verdict.action == RiskAction::REFUSE) {
                    if (!refused_by_failure) {
                        WARN("Risk module " + d.module_id + " refused " +
                             risk_scope_name(lap_ctx.scope) + " " + lap_ctx.scope_id + " " +
                             risk_location(lap_ctx) + ": " + d.reason +
                             "; positions pinned to the previous book");
                    }
                } else {
                    WARN("Risk module " + d.module_id + " replaced the book of " +
                         risk_scope_name(lap_ctx.scope) + " " + lap_ctx.scope_id + " " +
                         risk_location(lap_ctx) + ": " + d.reason);
                    outcome.replace_book = d.book;
                }
                // The loop pins every strategy and leaves (process_market_data).
                outcome.pin_all = true;
                outcome.action = verdict.action;
                outcome.module_id = winner_id;
                pinned = true;
            } else if (verdict.action == RiskAction::SCALE) {
                const double scale = verdict.scale;
                WARN("Risk limits exceeded, scaling positions by " + std::to_string(scale));

                // DESIGN DECISION: Risk scaling applies to target_positions only (Approach A)
                // Rationale: Risk management reduces the strategy's desired exposure, not actual
                // holdings. When current_positions ≈ target_positions (normal case), this behaves
                // correctly. Edge cases (current ≠ target) result in slightly more aggressive
                // de-risking, which is acceptable for risk management purposes. Alternative
                // Approach B (scale both current and target) would provide immediate proportional
                // de-risking but changes "what we think we hold" which could confuse PnL tracking.
                // We keep Approach A for consistency and simplicity.
                //
                // Example: current=+12, target=+10, scale=0.5
                //   new_target = 10 × 0.5 = +5
                //   trade = 5 - 12 = sell 7 contracts
                //   end position = +5 (50% of desired, not 50% of actual)
                //
                // The operand stays the double: each quantity is multiplied by Decimal(scale),
                // never recomputed as Decimal(double(q) * scale). A strategy pinned by a risk
                // module keeps its pinned book.

                // Scale positions in all strategies under lock. A pinned strategy is
                // skipped, so the book the modules measured is cut by less than `scale`; the
                // count travels to on_applied so a module keeping a level can say so (A-5).
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& [id, info] : strategies_) {
                        if (pinned_scopes_.count(id)) {
                            ++scopes_skipped;
                            continue;
                        }
                        for (auto& [symbol, pos] : info.target_positions) {
                            pos.quantity *= scale;
                        }
                    }
                }
                rebalance_applied_.emplace(lap_ctx.scope_id, 1.0).first->second *=
                    static_cast<double>(verdict.factor);
            } else {
                // A module whose gate read the book over its limits but asked for nothing more
                // (the level cut declines a lap already cut to this level or deeper) must not be
                // reported as "not exceeded" directly under "risk_exceeded=1" (T-6b INTERIM
                // ADVERSARIAL B-4; T-4 ADVERSARIAL finding 12 was this class of false line).
                bool gate_exceeded = false;
                for (const auto& d : decisions) {
                    if (d.metrics.has_value() && d.metrics->risk_exceeded) gate_exceeded = true;
                }
                if (gate_exceeded) {
                    INFO("Risk cut already applied at this level or deeper; no further scaling "
                         "this lap");
                } else {
                    INFO("Risk limits not exceeded, no scaling needed");
                }
            }

            // Read BEFORE deliver_and_record: it std::move()s every decision into its record,
            // so decisions[winner].module_id is an empty moved-from string afterwards.
            const double logged_requested =
                verdict.winner < decisions.size() &&
                        decisions[verdict.winner].action == RiskAction::SCALE
                    ? decisions[verdict.winner].scale
                    : 1.0;
            const std::string logged_winner =
                verdict.winner < decisions.size() ? decisions[verdict.winner].module_id : "";
            double logged_invariant = 1.0;
            double logged_leverage = 1.0;
            for (const auto& d : decisions) {
                if (!d.metrics.has_value()) continue;
                logged_invariant = std::min({static_cast<double>(d.metrics->portfolio_multiplier),
                                             static_cast<double>(d.metrics->jump_multiplier),
                                             static_cast<double>(d.metrics->correlation_multiplier)});
                logged_leverage = static_cast<double>(d.metrics->leverage_multiplier);
                break;
            }

            deliver_and_record(risk_modules_, decisions, verdict, lap_ctx, pinned, errors,
                               scopes_skipped);

            {
                const double applied = verdict.action == RiskAction::SCALE
                                           ? static_cast<double>(verdict.factor)
                                           : 1.0;
                auto it = rebalance_applied_.find(lap_ctx.scope_id);
                const double cumulative = it == rebalance_applied_.end() ? 1.0 : it->second;
                INFO(risk_applied_line(lap_ctx.lap, logged_requested, applied, cumulative,
                                       verdict.action, logged_winner, lap_ctx.scope,
                                       lap_ctx.scope_id, logged_invariant, logged_leverage));
            }
        } catch (const std::exception& e) {
            // Fail closed: the loop refuses the portfolio scope on this error (HD 2026-09-21).
            // This used to return OK, which shipped whatever the book was at the throw.
            ERROR("Exception during risk management: " + std::string(e.what()));
            return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                    std::string("Exception during risk management: ") + e.what(),
                                    "PortfolioManager");
        }

        INFO("Risk management applied successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error during risk management: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error during risk management: ") + e.what(),
                                "PortfolioManager");
    }
}

Result<void> PortfolioManager::apply_sleeve_risk(
    const std::vector<Bar>& data,
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& prev_positions,
    std::optional<Timestamp> as_of, bool is_warmup) {
    if (sleeve_risk_modules_.empty()) return Result<void>();

    // strategies_ order, as every other per-strategy pass.
    std::vector<std::pair<std::string, double>> sleeves;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, info] : strategies_) {
            if (sleeve_risk_modules_.count(sid)) sleeves.emplace_back(sid, info.allocation);
        }
    }

    for (const auto& [sid, allocation] : sleeves) {
        auto& modules = sleeve_risk_modules_.at(sid);
        if (modules.empty()) continue;
        std::unordered_map<std::string, Position> book;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            book = strategies_.at(sid).target_positions;
        }
        const RiskContext ctx = make_risk_context(
            RiskPhase::SLEEVE, 0, RiskScope::SLEEVE, sid,
            Decimal(static_cast<double>(sizing_capital_) * allocation), data, as_of,
            is_warmup);

        try {
            for (auto& module : modules) {
                module->on_bars(data, ctx);
            }
            if (book.empty()) {
                for (const auto& module : modules) {
                    RiskDecision none;
                    none.module_id = module->id();
                    record_risk_decision(ctx, module->id(), std::move(none), RiskAction::NONE,
                                         Decimal(1.0), true, "");
                }
                continue;
            }

            // Fail CLOSED, as the portfolio scope: the sleeve is no longer left untouched
            // because one of its modules failed, which discarded any REFUSE the others returned.
            std::vector<std::string> errors;
            std::vector<RiskDecision> decisions =
                evaluate_scope_modules(modules, book, ctx, errors);

            RiskVerdict verdict = combine_risk_decisions(decisions, ctx);
            std::string failed_gatekeeper;
            const bool refused_by_failure =
                refuse_on_failed_gatekeeper(modules, errors, ctx, verdict, failed_gatekeeper);
            bool pinned = false;
            if (verdict.action == RiskAction::REFUSE || verdict.action == RiskAction::REPLACE) {
                static const RiskDecision kNoDecision{};
                const RiskDecision& d =
                    verdict.winner < decisions.size() ? decisions[verdict.winner] : kNoDecision;
                if (verdict.action == RiskAction::REFUSE && !scope_is_seeded(sid)) {
                    ERROR("Risk module " +
                          (refused_by_failure ? failed_gatekeeper : d.module_id) +
                          " refused sleeve " + sid + " " + risk_location(ctx) +
                          ", but this sleeve's previous book was never seeded: pinning would "
                          "ship a FLAT book, not yesterday's. Seed it with "
                          "update_strategy_position before process_market_data.");
                    deliver_and_record(modules, decisions, verdict, ctx, false, errors);
                    return make_error<void>(
                        ErrorCode::RISK_LIMIT_EXCEEDED,
                        "Risk module " + (refused_by_failure ? failed_gatekeeper : d.module_id) +
                            " refused sleeve " + sid +
                            ", whose previous book was never seeded; refusing the run rather "
                            "than shipping a flat book",
                        "PortfolioManager");
                }
                std::lock_guard<std::mutex> lock(mutex_);
                auto& info = strategies_.at(sid);
                if (verdict.action == RiskAction::REFUSE) {
                    if (!refused_by_failure) {
                        WARN("Risk module " + d.module_id + " refused sleeve " + sid + " " +
                             risk_location(ctx) + ": " + d.reason +
                             "; positions pinned to the previous book");
                    }
                    auto prev = prev_positions.find(sid);
                    info.target_positions =
                        prev != prev_positions.end() ? prev->second
                                                     : std::unordered_map<std::string, Position>{};
                } else {
                    WARN("Risk module " + d.module_id + " replaced the book of sleeve " + sid +
                         " " + risk_location(ctx) + ": " + d.reason);
                    info.target_positions = d.book;
                }
                pinned_scopes_.insert(sid);
                pinned = true;
            } else if (verdict.action == RiskAction::SCALE) {
                const double scale = verdict.scale;
                WARN("Risk limits exceeded on sleeve " + sid + ", scaling its positions by " +
                     std::to_string(scale));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& [symbol, pos] : strategies_.at(sid).target_positions) {
                        pos.quantity *= scale;
                    }
                }
                rebalance_applied_.emplace(sid, 1.0).first->second *=
                    static_cast<double>(verdict.factor);
            }
            // Read BEFORE deliver_and_record, as at the portfolio scope: it moves the decisions.
            const double logged_requested =
                verdict.winner < decisions.size() &&
                        decisions[verdict.winner].action == RiskAction::SCALE
                    ? decisions[verdict.winner].scale
                    : 1.0;
            const std::string logged_winner =
                verdict.winner < decisions.size() ? decisions[verdict.winner].module_id : "";

            deliver_and_record(modules, decisions, verdict, ctx, pinned, errors);

            {
                const double applied = verdict.action == RiskAction::SCALE
                                           ? static_cast<double>(verdict.factor)
                                           : 1.0;
                auto it = rebalance_applied_.find(sid);
                const double cumulative = it == rebalance_applied_.end() ? 1.0 : it->second;
                INFO(risk_applied_line(ctx.lap, logged_requested, applied, cumulative,
                                       verdict.action, logged_winner, ctx.scope, sid, 1.0, 1.0));
            }
        } catch (const std::exception& e) {
            // T-7b-2 C10b: the sleeve's risk step could not answer (a module's on_bars threw, or
            // an exception after evaluate), so the sleeve is refused as a failed module refuses
            // it, recorded as a REFUSE row of kRiskStepModuleId carrying the error (the runners'
            // flag reads it). It used to log this line and leave the sleeve uncut.
            const std::string failure = e.what();
            ERROR("Exception during sleeve risk management for " + sid + ": " + failure +
                  "; the sleeve risk step could not answer, so the sleeve is refused: it is held "
                  "at its previous book and sends no orders, the other sleeves go on");
            RiskDecision none;
            none.module_id = kRiskStepModuleId;
            record_risk_decision(ctx, kRiskStepModuleId, std::move(none), RiskAction::REFUSE,
                                 Decimal(1.0), false, failure);
            if (!scope_is_seeded(sid)) {
                ERROR("The sleeve risk step refused sleeve " + sid + " " + risk_location(ctx) +
                      ", but this sleeve's previous book was never seeded: pinning would ship a "
                      "FLAT book, not yesterday's. Seed it with update_strategy_position before "
                      "process_market_data.");
                return make_error<void>(ErrorCode::RISK_LIMIT_EXCEEDED,
                                        "The sleeve risk step refused sleeve " + sid +
                                            ", whose previous book was never seeded; refusing "
                                            "the run rather than shipping a flat book",
                                        "PortfolioManager");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            auto prev = prev_positions.find(sid);
            strategies_.at(sid).target_positions =
                prev != prev_positions.end() ? prev->second
                                             : std::unordered_map<std::string, Position>{};
            pinned_scopes_.insert(sid);
        }
    }
    return Result<void>();
}

Result<void> PortfolioManager::seed_strategy_history(const std::vector<Bar>& bars) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, info] : strategies_) {
        if (!info.strategy) continue;
        auto r = info.strategy->seed_history(bars);
        if (r.is_error()) {
            return make_error<void>(r.error()->code(),
                                    "Strategy " + id + " refused the seeded history: " +
                                        std::string(r.error()->what()),
                                    "PortfolioManager");
        }
    }
    return Result<void>();
}

Result<void> PortfolioManager::set_sizing_capital(double capital) {
    if (!std::isfinite(capital) || capital <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Sizing capital " + std::to_string(capital) +
                                    " is not a finite positive number; the book keeps sizing on " +
                                    std::to_string(static_cast<double>(sizing_capital_)),
                                "PortfolioManager");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // One quantised figure for every reader (the optimizer and the modules read the Decimal).
    const Decimal as_decimal(capital);
    const double sized = static_cast<double>(as_decimal);
    // Every strategy first, then every module: the first refusal stops the call. A strategy or
    // module that refuses has kept its old capital, so the caller must treat an error as fatal
    // for the rebalance (both runners refuse the run; the backtest fails the day), never size on.
    for (auto& [id, info] : strategies_) {
        if (!info.strategy) continue;
        auto r = info.strategy->set_capital_allocation(sized * info.allocation);
        if (r.is_error()) {
            return make_error<void>(r.error()->code(),
                                    "Strategy " + id + " refused the sizing capital: " +
                                        std::string(r.error()->what()),
                                    "PortfolioManager");
        }
    }
    for (auto& module : risk_modules_) {
        auto r = module->set_capital(as_decimal);
        if (r.is_error()) {
            return make_error<void>(r.error()->code(),
                                    "Risk module " + module->id() + " refused the sizing capital: " +
                                        std::string(r.error()->what()),
                                    "PortfolioManager");
        }
    }
    for (auto& [sid, modules] : sleeve_risk_modules_) {
        for (auto& module : modules) {
            auto r = module->set_capital(as_decimal);
            if (r.is_error()) {
                return make_error<void>(r.error()->code(),
                                        "Sleeve " + sid + " risk module " + module->id() +
                                            " refused the sizing capital: " +
                                            std::string(r.error()->what()),
                                        "PortfolioManager");
            }
        }
    }
    sizing_capital_ = as_decimal;
    return Result<void>();
}

double PortfolioManager::sizing_capital() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<double>(sizing_capital_);
}

Result<void> PortfolioManager::update_allocations(
    const std::unordered_map<std::string, double>& allocations) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto validation = validate_allocations(allocations);
    if (validation.is_error()) {
        return validation;
    }

    // Update allocations and scale positions accordingly
    for (auto& [id, info] : strategies_) {
        if (allocations.count(id)) {
            double scale = allocations.at(id) / info.allocation;
            info.allocation = allocations.at(id);

            // Scale positions
            for (auto& [symbol, pos] : info.target_positions) {
                pos.quantity *= scale;
            }
        }
    }

    return Result<void>();
}

std::unordered_map<std::string, Position> PortfolioManager::get_portfolio_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, Position> portfolio_positions;

    for (const auto& [_, info] : strategies_) {
        for (const auto& [symbol, pos] : info.target_positions) {
            if (portfolio_positions.count(symbol) == 0) {
                portfolio_positions[symbol] = pos;
                portfolio_positions[symbol].quantity *= info.allocation;
            } else {
                portfolio_positions[symbol].quantity += pos.quantity * info.allocation;
            }
        }
    }

    return portfolio_positions;
}

std::unordered_map<std::string, double> PortfolioManager::get_required_changes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, double> changes;

    for (const auto& [_, info] : strategies_) {
        for (const auto& [symbol, target] : info.target_positions) {
            double current = info.current_positions.count(symbol)
                                 ? static_cast<double>(info.current_positions.at(symbol).quantity)
                                 : 0.0;

            changes[symbol] = (static_cast<double>(target.quantity) - current) * info.allocation;
        }
    }

    return changes;
}

Result<void> PortfolioManager::validate_allocations(
    const std::unordered_map<std::string, double>& allocations) const {
    if (allocations.empty() || strategies_.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "No strategies to allocate to",
                                "PortfolioManager");
    }

    double total = 0.0;

    for (const auto& [id, allocation] : allocations) {
        if (strategies_.find(id) == strategies_.end()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Strategy " + id + " not found",
                                    "PortfolioManager");
        }

        if (allocation < config_.min_strategy_allocation ||
            allocation > config_.max_strategy_allocation) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Allocation for " + id + " out of bounds", "PortfolioManager");
        }

        total += allocation;
    }

    if (std::abs(total - 1.0) > 1e-6) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Allocations must sum to 1.0",
                                "PortfolioManager");
    }

    return Result<void>();
}

std::vector<ExecutionReport> PortfolioManager::get_recent_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_executions_;
}

std::unordered_map<std::string, std::vector<ExecutionReport>>
PortfolioManager::get_strategy_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategy_executions_;
}

void PortfolioManager::append_synthetic_execution(const std::string& strategy_id,
                                                  const ExecutionReport& exec) {
    std::lock_guard<std::mutex> lock(mutex_);
    strategy_executions_[strategy_id].push_back(exec);
}

size_t PortfolioManager::insert_executions_at(const std::string& strategy_id, size_t index,
                                              const std::vector<ExecutionReport>& execs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& v = strategy_executions_[strategy_id];
    const size_t at = std::min(index, v.size());
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), execs.begin(), execs.end());
    return at;
}

size_t PortfolioManager::roll_leg_count(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = strategy_executions_.find(strategy_id);
    if (it == strategy_executions_.end()) return 0;
    return static_cast<size_t>(std::count_if(it->second.begin(), it->second.end(), [](const ExecutionReport& e) {
        return e.execution_type == ExecutionType::ROLL;
    }));
}

void PortfolioManager::clear_execution_history() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only clear portfolio-level executions (recent_executions_)
    // DO NOT clear strategy_executions_ - they need to accumulate across all periods
    // for saving at the end of the backtest
    recent_executions_.clear();
    // strategy_executions_ is NOT cleared here - it accumulates for final save
}

void PortfolioManager::clear_all_executions() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Clear both portfolio-level and strategy-level executions
    // Used during warmup to ensure no executions from warmup period persist
    recent_executions_.clear();
    strategy_executions_.clear();
    // Keep the filled-position ledger in lockstep with strategy_executions_.
    // E2-P3-a: the filled-position ledger MUST be cleared with the executions it mirrors.
    //
    // filled_positions_ records what this manager has actually traded into, and order sizing
    // is `target - filled`. Clearing the execution history without clearing the ledger would
    // leave sizing measuring against fills that no longer exist, so the next cycle would
    // under-trade by exactly the discarded quantity.
    //
    // This is the ONLY reset. BacktestCoordinator::reset() does not touch PortfolioManager at
    // all, so a coordinator reset does not clear this -- safe today because run_portfolio has
    // a single period loop and each process builds a fresh PortfolioManager, but it means a
    // second backtest period against a reused manager would size against stale fills. If a
    // period loop is ever added, reset the manager here too rather than assuming this covers
    // it.
    filled_positions_.clear();
}

int PortfolioManager::register_equity_cost_configs(
    const std::vector<std::string>& symbols,
    const std::unordered_map<std::string, std::vector<Bar>>& bars_by_symbol,
    int adv_lookback_days) {
    // E2-C9 -- see the header for why both cost managers must be registered.
    return cost_manager_.register_equity_costs_from_bars(symbols, bars_by_symbol,
                                                         adv_lookback_days);
}

void PortfolioManager::update_cost_manager_market_data(const std::string& symbol, double volume,
                                                       double close_price,
                                                       double prev_close_price) {
    cost_manager_.update_market_data(symbol, volume, close_price, prev_close_price);
}

std::vector<std::shared_ptr<StrategyInterface>> PortfolioManager::get_strategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<StrategyInterface>> result;
    result.reserve(strategies_.size());

    for (const auto& [_, info] : strategies_) {
        result.push_back(info.strategy);
    }

    return result;
}

std::unordered_map<std::string, std::unordered_map<std::string, Position>>
PortfolioManager::get_strategy_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> result;

    for (const auto& [strategy_id, info] : strategies_) {
        result[strategy_id] = info.current_positions;  // These are the optimized positions
    }

    return result;
}

std::unordered_map<std::string, std::unordered_map<std::string, Position>>
PortfolioManager::get_filled_strategy_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> result;
    for (const auto& [strategy_id, filled] : filled_positions_) {
        auto& book = result[strategy_id];
        for (const auto& [symbol, quantity] : filled) {
            Position pos;
            pos.symbol = symbol;
            pos.quantity = Quantity(quantity);
            book[symbol] = pos;
        }
    }
    return result;
}

Result<void> PortfolioManager::update_strategy_position(const std::string& strategy_id,
                                                        const std::string& symbol,
                                                        const Position& updated_pos) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Strategy not found: " + strategy_id,
                                "PortfolioManager");
    }

    // Update both current_positions and target_positions
    // current_positions is what gets returned by get_strategy_positions() and saved to DB
    it->second.current_positions[symbol] = updated_pos;
    it->second.target_positions[symbol] = updated_pos;
    // This call IS the seeding of the previous book: the backtest coordinator makes it every
    // day and the two futures live runners make it once from load_positions_by_date. A REFUSE
    // pins the scope to current_positions, so recording who was seeded is what lets the PM
    // tell "ship yesterday's book" apart from "ship nothing" (T-6a ADVERSARIAL A-2).
    seeded_scopes_.insert(strategy_id);
    // The portfolio scope's book is the sum of its sleeves', so seeding any sleeve seeds it.
    seeded_scopes_.insert(id_);

    return Result<void>();
}

double PortfolioManager::get_portfolio_value(
    const std::unordered_map<std::string, double>& current_prices) const {
    static int call_count = 0;
    call_count++;

    // Start with total capital
    double portfolio_value = static_cast<double>(config_.total_capital);

    if (call_count <= 3) {
        INFO("PV_CALL #" + std::to_string(call_count) + ": starting with total capital: $" +
             std::to_string(portfolio_value));
    }

    DEBUG("Portfolio value calculation starting with total capital: $" +
          std::to_string(portfolio_value));

    // Acquire the mutex and get a copy of the portfolio positions
    std::unordered_map<std::string, Position> positions_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        positions_copy = get_positions_internal();
    }

    // Log position PnL values for first call only
    if (call_count == 1) {
        double total_rpnl = 0.0;
        for (const auto& [symbol, pos] : positions_copy) {
            double rpnl = static_cast<double>(pos.realized_pnl);
            total_rpnl += rpnl;
            INFO("FIRST_PV: " + symbol + " realized_pnl=$" + std::to_string(rpnl) +
                 " qty=" + std::to_string(static_cast<double>(pos.quantity)));
        }
        INFO("FIRST_PV TOTAL realized_pnl=$" + std::to_string(total_rpnl));
    }

    DEBUG("Found " + std::to_string(positions_copy.size()) +
          " positions for portfolio value calculation");
    DEBUG("Available current prices for " + std::to_string(current_prices.size()) + " symbols");

    // Process the positions outside of the mutex lock
    int positions_with_prices = 0;
    int positions_without_prices = 0;

    for (const auto& [symbol, pos] : positions_copy) {
        auto it = current_prices.find(symbol);
        if (it != current_prices.end()) {
            // Calculate unrealized P&L using current market price
            double current_price = it->second;
            double avg_price = static_cast<double>(pos.average_price);
            double quantity = static_cast<double>(pos.quantity);

            // Only recompute unrealized if the position has a non-zero value stored.
            //
            // E2-P3-b: the original comment asserted "For REALIZED_ONLY accounting (futures),
            // unrealized_pnl is always 0" as though it were a system-wide invariant. It is
            // not. It held for the STRATEGY's own positions_ -- which is what
            // get_positions_internal() returns here, and which TrendFollowingStrategy never
            // populates -- but the backtest coordinator wrote a NON-zero unrealized onto
            // futures rows until E2-F2 fixed it. The invariant this guard leans on was false
            // for two commits and nobody noticed, because this function has no production
            // caller (tests only).
            //
            // The guard itself is fine: it skips the recompute unless something is stored. But
            // if this function ever gains a caller, check where its positions came from first
            // -- the expression below omits point_value and is therefore valid for equities
            // only.
            if (std::abs(static_cast<double>(pos.unrealized_pnl)) > 1e-6) {
                // For equities: unrealized_pnl = quantity * (current_price - avg_price)
                double unrealized_pnl = quantity * (current_price - avg_price);
                portfolio_value += unrealized_pnl;
            }
            positions_with_prices++;
        } else {
            // If no current price available, use the stored unrealized P&L
            WARN("No current price available for position " + symbol + ", using stored P&L: $" +
                 std::to_string(static_cast<double>(pos.unrealized_pnl)));
            portfolio_value += static_cast<double>(pos.unrealized_pnl);
            positions_without_prices++;
        }

        // Add realized P&L for this position
        double rpnl = static_cast<double>(pos.realized_pnl);
        portfolio_value += rpnl;
    }

    if (positions_without_prices > 0) {
        DEBUG("Portfolio valuation: " + std::to_string(positions_with_prices) +
              " positions with current prices, " + std::to_string(positions_without_prices) +
              " positions using stored P&L");
    }

    DEBUG("Final portfolio value: $" + std::to_string(portfolio_value));
    return portfolio_value;
}

std::unordered_map<std::string, Position> PortfolioManager::get_positions_internal() const {
    // This method is called with the mutex already locked
    std::unordered_map<std::string, Position> portfolio_positions;

    for (const auto& [_, info] : strategies_) {
        // CRITICAL FIX: Read positions directly from strategy (with P&L) instead of cached
        // target_positions
        const auto& strategy_positions = info.strategy->get_positions();
        for (const auto& [symbol, pos] : strategy_positions) {
            if (portfolio_positions.count(symbol) == 0) {
                portfolio_positions[symbol] = pos;
                portfolio_positions[symbol].quantity *= info.allocation;
                portfolio_positions[symbol].realized_pnl *= Decimal(info.allocation);
                portfolio_positions[symbol].unrealized_pnl *= Decimal(info.allocation);
            } else {
                portfolio_positions[symbol].quantity += pos.quantity * info.allocation;
                portfolio_positions[symbol].realized_pnl +=
                    pos.realized_pnl * Decimal(info.allocation);
                portfolio_positions[symbol].unrealized_pnl +=
                    pos.unrealized_pnl * Decimal(info.allocation);
            }
        }
    }

    return portfolio_positions;
}

}  // namespace trade_ngin
