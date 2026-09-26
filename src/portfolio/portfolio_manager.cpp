// src/portfolio/portfolio_manager.cpp
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/portfolio/allocation_split.hpp"
#include "trade_ngin/portfolio/cut_delivery.hpp"
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
        // Chop-source attribution snapshots: integer position values at each pipeline phase,
        // used at end of cycle to tag each integer transition with its trigger.
        std::unordered_map<std::string, std::unordered_map<std::string, double>> attr_strategy_target;
        std::unordered_map<std::string, std::unordered_map<std::string, double>> attr_post_qp;
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

                    // Chop-source attribution: snapshot strategy's integer target before optimizer runs
                    for (const auto& [sym, pos] : info.target_positions) {
                        attr_strategy_target[id][sym] = static_cast<double>(pos.quantity);
                    }

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

        //  Iterative dynamic opt + risk management loop
        // Up to 5 iterations for convergence to fully integer positions. The final rounding step
        // can cause minor tracking error/risk profile deviation

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

        int max_iterations = 5;
        int iteration = 0;
        bool done = false;
        RiskLapOutcome risk_outcome;  // pin_all set by a portfolio-scope REFUSE / REPLACE

        while (!done && iteration++ < max_iterations) {
            INFO("Iteration " + std::to_string(iteration) + " of dynamic optimization + risk loop");

            // Dynamic Optimization step
            if (config_.use_optimization && optimizer_) {
                try {
                    Logger::register_component("DynamicOptimizer");
                    auto opt_result = optimize_positions();
                    {
                        // 9e: the lap's book before the gate, the base of a cut this lap delivers.
                        std::lock_guard<std::mutex> lock(mutex_);
                        lap_book_before_gate_.clear();
                        for (const auto& [sid, sinfo] : strategies_) {
                            if (!sinfo.use_optimization || pinned_scopes_.count(sid)) continue;
                            for (const auto& [sym, pos] : sinfo.target_positions) {
                                lap_book_before_gate_[sym] += static_cast<double>(pos.quantity);
                            }
                        }
                    }
                    if (opt_result.is_error()) {
                        WARN("Portfolio optimization failed in iteration " +
                             std::to_string(iteration) + ": " +
                             std::string(opt_result.error()->what()) +
                             ", continuing without optimization");
                    }
                } catch (const std::exception& e) {
                    WARN("Exception during portfolio optimization in iteration " +
                         std::to_string(iteration) + ": " + std::string(e.what()) +
                         ", continuing without optimization");
                }
            }
            // Chop-source attribution: snapshot first optimizer call output (before risk manager)
            if (iteration == 1) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, info] : strategies_) {
                    for (const auto& [sym, pos] : info.target_positions) {
                        attr_post_qp[id][sym] = static_cast<double>(pos.quantity);
                    }
                }
                // T-7b-2 C9a: the same book summed into the account's contracts per symbol, the
                // book lap 1's portfolio risk step reads (the delivered cut's denominator).
                for (const auto& [id, info] : strategies_) {
                    for (const auto& [sym, pos] : info.target_positions) {
                        delivered_lap1_book_[sym] += static_cast<double>(pos.quantity);
                    }
                }
                delivered_has_lap1_ = true;
            }

            // Risk Management step
            lap_cut_factor_ = 1.0;
            bool has_risk_manager = !risk_modules_.empty();
            if (has_risk_manager) {
                // A portfolio-scope risk step that cannot answer REFUSES the scope (HD 2026-09-21,
                // option b): every strategy is held at its previous book and no order is sent.
                // The old WARN that went on WITHOUT risk management is gone. A module whose
                // evaluate fails is refused inside apply_risk_management
                // (refuse_on_failed_gatekeeper); what fails the step itself (a module's on_bars
                // throwing, an exception after evaluate) is refused here, recorded as a REFUSE
                // row carrying the error, so the runner's metadata mark and exit code see it.
                const RiskContext lap_ctx =
                    make_risk_context(RiskPhase::LAP, iteration, RiskScope::PORTFOLIO, id_,
                                      sizing_capital_, data, current_timestamp,
                                      skip_execution_generation);
                std::string step_failure;
                try {
                    Logger::register_component("RiskManager");
                    auto risk_result = apply_risk_management(data, lap_ctx, risk_outcome);
                    if (risk_result.is_error()) {
                        step_failure = risk_result.error()->what();
                    } else {
                        INFO("Portfolio risk management applied successfully in iteration " +
                             std::to_string(iteration));
                    }
                } catch (const std::exception& e) {
                    step_failure = e.what();
                }
                if (!step_failure.empty() && !risk_outcome.pin_all &&
                    !risk_outcome.refuse_unseeded) {
                    ERROR("Portfolio risk management failed in iteration " +
                          std::to_string(iteration) + ": " + step_failure +
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
                INFO("Risk management not enabled, skipping risk checks in iteration " +
                     std::to_string(iteration));
            }

            // A portfolio-scope REFUSE (or REPLACE) pins every strategy and ends the loop. The
            // strategies' own targets and signals are untouched; process_market_data returns OK.
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
                         risk_outcome.module_id + " after iteration " + std::to_string(iteration) +
                         "; leaving the loop");
                } else {
                    INFO("Risk refusal: every strategy pinned to its previous positions after "
                         "iteration " + std::to_string(iteration) + "; leaving the loop");
                }
                done = true;
                break;
            }

            // 9e: a lap the gate cut delivers the cut in whole contracts (cut_delivery.hpp): the
            // cut book is the next lap's input, so the next lap re-optimises it from the held
            // anchor and re-reads the gate, and neither its deadband nor forced rounding can undo
            // the cut.
            bool cut_delivered = false;
            if (lap_cut_factor_ < 1.0 && !config_.allow_fractional_positions &&
                config_.use_optimization && optimizer_) {
                deliver_lap_cut(iteration);
                cut_delivered = true;
            }

            // Check for partial contracts in final positions.
            // When the portfolio permits fractional positions there is nothing to
            // converge to, so a fraction is the answer rather than a reason to
            // iterate. Re-entering the loop would re-apply the risk scale to an
            // already-scaled book, compounding it once per lap (E2-F1); the gate
            // is scale-invariant (E2-F2) so shrinking never satisfies it and the
            // position decays to zero. Futures leave the flag false and are
            // unaffected: whole contracts already converge on the first pass.
            bool partials_found = false;
            if (!config_.allow_fractional_positions) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, info] : strategies_) {
                    if (pinned_scopes_.count(id)) continue;  // pinned by a risk module
                    for (const auto& [symbol, pos] : info.target_positions) {
                        double fractional = std::abs(static_cast<double>(pos.quantity) -
                                                     std::round(static_cast<double>(pos.quantity)));
                        if (fractional > 1e-6) {
                            partials_found = true;
                            INFO("Fractional contract detected in iteration " +
                                 std::to_string(iteration) + ": " + symbol +
                                 ", quantity=" + std::to_string(pos.quantity));
                            break;
                        }
                    }
                    if (partials_found)
                        break;
                }
            }

            if (!partials_found && !cut_delivered) {
                if (config_.allow_fractional_positions) {
                    INFO("Fractional positions permitted; accepting iteration " +
                         std::to_string(iteration) +
                         " output as final (risk scale applied once). Converged!");
                } else {
                    INFO("No partial contracts after iteration " + std::to_string(iteration) +
                         ". Converged!");
                    // The 1e-6 test above is a convergence test, not a guard: a quantity it
                    // passed as whole may still hold a fraction (a SCALE of 0.9999999 on a 1-lot
                    // gives 0.9999999), from either term. Store the whole contract it was judged
                    // to be, once, here, for every unpinned scope; an exact integer is untouched.
                    int snapped = 0;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        for (auto& [id, info] : strategies_) {
                            if (pinned_scopes_.count(id)) continue;  // pinned by a risk module
                            for (auto& [symbol, pos] : info.target_positions) {
                                const double q = static_cast<double>(pos.quantity);
                                const double whole = std::round(q);
                                if (q != whole) {
                                    pos.quantity = static_cast<Decimal>(whole);
                                    ++snapped;
                                }
                            }
                        }
                    }
                    if (snapped > 0) {
                        INFO("RISK_CONVERGED_SNAP symbols=" + std::to_string(snapped) +
                             " iteration=" + std::to_string(iteration) +
                             ": quantities within 1e-6 of a whole contract stored as that whole "
                             "contract on the converged exit");
                    }
                }
                done = true;
            }
        }

        // This safeguard forcibly rounds all final positions to integers in case of conflicting
        // rounding logic
        if (!done) {
            WARN("Max iterations reached (" + std::to_string(max_iterations) +
                 "). Forcing final rounding to remove any partial contracts.");

            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, info] : strategies_) {
                if (pinned_scopes_.count(id)) continue;  // pinned by a risk module
                for (auto& [symbol, pos] : info.target_positions) {
                    double original_quantity = static_cast<double>(pos.quantity);
                    pos.quantity =
                        static_cast<Decimal>(std::round(static_cast<double>(pos.quantity)));
                    if (std::abs(original_quantity - static_cast<double>(pos.quantity)) > 1e-6) {
                        INFO("Final forced rounding for " + symbol + ": " +
                             std::to_string(original_quantity) + " -> " +
                             std::to_string(pos.quantity));
                    }
                }
            }
            INFO("Final rounding completed. No partial contracts remain.");
        } else if (risk_outcome.pin_all) {
            INFO("Final positions pinned by a risk " +
                 std::string(risk_outcome.action == RiskAction::REPLACE ? "replacement"
                                                                        : "refusal") +
                 " after " + std::to_string(iteration) + " iterations; rounding skipped.");
        } else {
            INFO("Final positions fully integer after " + std::to_string(iteration) +
                 " iterations.");
        }

        // Post-rounding risk point: finalize() on the final book, before the final check, the
        // chop-source attribution and the current-positions copy below all read it (silent
        // unless a module warns, refuses or is rejected).
        if (!risk_modules_.empty() || !sleeve_risk_modules_.empty()) {
            auto post_result =
                apply_post_rounding_risk(data, std::min(iteration, max_iterations), prev_positions,
                                         current_timestamp, skip_execution_generation);
            if (post_result.is_error()) return post_result;
        }

        // Final verification of all positions for partial contracts.
        // Diagnostic only -- it reports, it does not alter the position. Skipped when the
        // portfolio permits fractional positions, where a fraction is the intended result
        // and not an anomaly: reporting it would emit an ERROR per symbol per bar (1,702
        // in one equity run) and bury the errors that do matter.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!config_.allow_fractional_positions) {
                for (const auto& [id, info] : strategies_) {
                    if (pinned_scopes_.count(id)) continue;  // pinned by a risk module
                    for (const auto& [symbol, pos] : info.target_positions) {
                        double fractional = std::abs(static_cast<double>(pos.quantity) -
                                                     std::round(static_cast<double>(pos.quantity)));
                        if (fractional > 1e-6) {
                            ERROR("FINAL CHECK: Fractional contract detected for " + symbol +
                                  " after all iterations. Quantity=" + std::to_string(pos.quantity));
                        }
                    }
                }
            }

            // Chop-source attribution: classify each integer position transition for trades
            // about to be generated (final integer != prev integer). Tags with which pipeline
            // layer caused the change (strategy / QP / risk-scale / unclassified).
            for (auto& [id, info] : strategies_) {
                auto prev_it = prev_positions.find(id);
                if (prev_it == prev_positions.end()) continue;
                for (const auto& [sym, target_pos] : info.target_positions) {
                    double final_q = std::round(static_cast<double>(target_pos.quantity));
                    double prev_q = 0.0;
                    auto pp = prev_it->second.find(sym);
                    if (pp != prev_it->second.end()) {
                        prev_q = std::round(static_cast<double>(pp->second.quantity));
                    }
                    if (std::abs(final_q - prev_q) < 0.5) continue;  // No trade

                    double strat_q = std::round(attr_strategy_target[id][sym]);
                    double qp_q = std::round(attr_post_qp[id][sym]);
                    std::string source;
                    if (std::abs(strat_q - prev_q) >= 0.5) {
                        source = "STRATEGY_FLIP";
                    } else if (std::abs(qp_q - strat_q) >= 0.5) {
                        source = "QP_FLIP";
                    } else if (std::abs(final_q - qp_q) >= 0.5) {
                        source = "RISK_SCALE_FLIP";
                    } else {
                        source = "UNCLASSIFIED";
                    }
                    DEBUG("CHOP_SOURCE: symbol=" + sym + " source=" + source +
                         " prev=" + std::to_string(prev_q) +
                         " strat=" + std::to_string(strat_q) +
                         " qp=" + std::to_string(qp_q) +
                         " final=" + std::to_string(final_q));
                }
            }

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
                // This allows accurate per-strategy execution tracking
                for (auto& [strategy_id, info] : strategies_) {
                    auto& strategy_execs = strategy_executions_[strategy_id];
                    // Start counter from current size to ensure unique IDs across all periods
                    int exec_counter = static_cast<int>(strategy_execs.size());

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
                            // The backtest predicate (T-7a C4; T-4c J1 re-keyed on the session
                            // classifier): a symbol whose signal-group bar is not a SESSION (no
                            // bar, or a JUNK bar) gets no fill and NO BOOK CHANGE. Its book is
                            // held at what has actually been filled, so it cannot earn P&L on
                            // contracts it never bought. The hold does not queue the change: a
                            // later cycle trades only if its own re-anchored target, computed on
                            // that cycle's bars, still differs from the ledger, so a hold can
                            // outlast the gap or never trade at all (T-4c E13 / ADVERSARIAL F5).
                            if (session_symbols && !session_symbols->count(symbol)) {
                                auto book_it = info.current_positions.find(symbol);
                                if (book_it != info.current_positions.end()) {
                                    book_it->second.quantity = Decimal(current_qty);
                                }
                                INFO("BOOK_GATE backtest " + symbol + " (" + strategy_id +
                                     "): no SESSION bar in the signal group -- book held at "
                                     "filled qty=" + std::to_string(current_qty) +
                                     " instead of target " + std::to_string(new_qty));
                                continue;
                            }

                            // The symbol's LATEST-dated bar in this call (the signal group's
                            // close). T-7b-1 7a: on a release cycle the backtest feeds a withheld
                            // JUNK bar ahead of the symbol's new bar, so the first bar is not the
                            // latest; the first of equal dates is kept, as before.
                            const double latest_price = latest_close_of(data, symbol);

                            if (latest_price == 0.0) {
                                // T-7b-2 9 (J1): with a session set, no fill means no book
                                // change, as live's STRICT rollback puts an unpriced change back
                                // to its stored row (execute_strategy_day_strict). Without a set
                                // (live callers, the equity backtest) the old skip is kept.
                                if (session_symbols) {
                                    auto book_it = info.current_positions.find(symbol);
                                    if (book_it != info.current_positions.end()) {
                                        book_it->second.quantity = Decimal(current_qty);
                                    }
                                    WARN("BOOK_GATE backtest " + symbol + " (" + strategy_id +
                                         "): no usable close in the signal group -- book held "
                                         "at filled qty=" + std::to_string(current_qty) +
                                         " instead of target " + std::to_string(new_qty));
                                }
                                continue;  // Skip if price not available
                            }

                            generate_fill(symbol, current_qty, new_qty, latest_price);
                        }
                    }

                    // T-7b-2 9 (J1; T-4c section 8 condition 3(a), ADVERSARIAL J1-E): a symbol the
                    // ledger holds that the target map no longer carries is never visited by the
                    // loop above, and `current_positions = target_positions` has already dropped
                    // it from the book with no fill. With a session set it is treated as live
                    // treats it: not a SESSION -> re-inserted at the filled quantity from the
                    // previous book (hold_non_session_symbols' second loop, no close-out); a
                    // SESSION -> closed out to flat at the signal group's close (the execution
                    // step's close-out loop). Sorted, after the target loop, so the target loop's
                    // fills keep their order and ids. Without a set the parent's drop is kept.
                    if (session_symbols) {
                        std::vector<std::string> absent;
                        for (const auto& [symbol, filled_qty] : strategy_filled) {
                            if (std::abs(filled_qty) <= 1e-6) continue;
                            if (info.target_positions.count(symbol)) continue;
                            absent.push_back(symbol);
                        }
                        std::sort(absent.begin(), absent.end());
                        for (const auto& symbol : absent) {
                            const double current_qty = strategy_filled.at(symbol);
                            const double latest_price = session_symbols->count(symbol)
                                                            ? latest_close_of(data, symbol)
                                                            : 0.0;
                            if (latest_price != 0.0) {
                                INFO("BOOK_GATE backtest " + symbol + " (" + strategy_id +
                                     "): absent from the target on a SESSION bar -- closed out "
                                     "from filled qty=" + std::to_string(current_qty) +
                                     " at the signal group's close");
                                generate_fill(symbol, current_qty, 0.0, latest_price);
                                continue;
                            }
                            Position held;
                            auto prev_strategy = prev_positions.find(strategy_id);
                            if (prev_strategy != prev_positions.end()) {
                                auto prev_row = prev_strategy->second.find(symbol);
                                if (prev_row != prev_strategy->second.end()) held = prev_row->second;
                            }
                            held.symbol = symbol;
                            held.quantity = Decimal(current_qty);
                            info.current_positions[symbol] = held;
                            INFO("BOOK_GATE backtest " + symbol + " (" + strategy_id + "): " +
                                 (session_symbols->count(symbol)
                                      ? std::string("no usable close in the signal group")
                                      : std::string("no SESSION bar in the signal group")) +
                                 " -- absent from the target, book held at filled qty=" +
                                 std::to_string(current_qty) + "; no close-out");
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
                if (is_backtest_) {
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
    std::vector<double> costs(symbols.size(), 0.0);

    // Collect all trading data once
    std::unordered_map<std::string, const InstrumentData*> all_trading_data;
    for (const auto& [strategy_id, info] : strategies_) {
        auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
        if (trend_strategy) {
            const auto& strategy_data = trend_strategy->get_all_instrument_data();
            for (const auto& [symbol, data] : strategy_data) {
                all_trading_data[symbol] = &data;
            }
        }
    }

    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];

        // Get contract size and price for this symbol
        auto it = all_trading_data.find(symbol);
        if (it != all_trading_data.end()) {
            const auto& data = *(it->second);
            double contract_size = data.contract_size;
            double price = data.price_history.empty() ? 1.0 : data.price_history.back();
            double fx_rate = 1.0;  // Default exchange rate

            // F4 (T-7b-1 C8d, ledger M-04): the entry is the cost of ONE contract over that
            // contract's notional. The optimizer charges |dw| x costs[i] with dw in weight
            // (notional / capital); n contracts are dw = n x notional / capital and cost
            // n x cost_per_contract dollars, n x cost_per_contract / capital of capital, which is
            // |dw| x cost_per_contract / notional. The entry was cost_per_contract / capital,
            // which understated the penalty by capital / notional (13.8x for MES at 7,252.5 on
            // $500,000, 4.6x for ZF at 107.85). notional uses the same contract_size and price
            // as the weights per contract in optimize_positions.
            double notional_per_contract = contract_size * price * fx_rate;
            auto cost_result = cost_manager_.calculate_costs(symbol, 1.0, price);
            double cost_per_contract = cost_result.total_transaction_costs;
            costs[i] = (notional_per_contract > 0.0) ? (cost_per_contract / notional_per_contract)
                                                     : 0.0;
        } else {
            WARN("Symbol " + symbol + " not found in trading data, using zero cost");
            costs[i] = 0.0;
        }
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
        touched.insert(bar.symbol);
    }
    for (const auto& symbol : touched) {
        auto& series = closes_by_date_.at(symbol);
        while (series.size() > max_prices) {
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

            double ret = (curr_price - prev_price) / prev_price;

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
            auto& series = out[symbol];
            series.reserve(dates.size() - 1);
            for (size_t t = 1; t < dates.size(); ++t) {
                const double prev_price = closes.at(dates[t - 1]);
                const double curr_price = closes.at(dates[t]);
                series.push_back((curr_price - prev_price) / prev_price);
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
        const double share = total > 1e-8 ? s.contribution / total : 0.0;
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

CutDelivery deliver_cut_in_whole_contracts(const CutDeliveryInput& in) {
    CutDelivery out;
    const double f = in.factor;
    auto held = [&in](const std::string& sym) {
        auto it = in.held.find(sym);
        return it == in.held.end() ? 0.0 : std::round(it->second);
    };
    auto notional = [&in](const std::string& sym) {
        auto it = in.notional_per_contract.find(sym);
        return it == in.notional_per_contract.end() ? -1.0 : it->second;
    };
    std::map<std::string, double>& R = out.book;
    for (const auto& [sym, q] : in.lap_book) R[sym] = std::round(q);
    for (const auto& [sym, q] : R) {
        const double n = notional(sym);
        if (n <= 0.0) {
            if (q != 0.0) ++out.unknown_notional;
            continue;
        }
        out.lap_notional += std::abs(q) * n;
        out.held_notional += std::abs(held(sym)) * n;
    }
    out.target_notional = f * out.lap_notional;

    // The covariance's index by symbol (empty without a covariance: best fit only).
    std::unordered_map<std::string, size_t> cix;
    if (!in.covariance_symbols.empty() && in.covariance.size() == in.covariance_symbols.size()) {
        for (size_t i = 0; i < in.covariance_symbols.size(); ++i) cix[in.covariance_symbols[i]] = i;
    }
    const double cap = in.capital;
    // Squared tracking error of the book R against the gate's target f x (the lap's book), in the
    // optimizer's weight space (contracts x notional / capital).
    auto te_sq = [&]() {
        const size_t m = in.covariance_symbols.size();
        std::vector<double> e(m, 0.0);
        for (const auto& [sym, idx] : cix) {
            auto it = R.find(sym);
            const double n = notional(sym);
            if (it == R.end() || n <= 0.0) continue;
            auto bt = in.lap_book.find(sym);
            const double b = bt == in.lap_book.end() ? 0.0 : bt->second;
            e[idx] = (f * b - it->second) * n / cap;
        }
        double t = 0.0;
        for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < m; ++j) t += e[i] * in.covariance[i][j] * e[j];
        return t;
    };
    auto gross = [&]() {
        double g = 0.0;
        for (const auto& [sym, q] : R) {
            const double n = notional(sym);
            if (n > 0.0) g += std::abs(q) * n;
        }
        return g;
    };
    // A contract beyond the held book on the same side (or the whole position when the held book is
    // flat or on the other side) is one the day's request added.
    auto is_new = [&](const std::string& sym, double q) {
        const double h = held(sym);
        if (h == 0.0 || (h > 0.0) != (q > 0.0)) return true;
        return std::abs(q) > std::abs(h);
    };

    double g = gross();
    while (g > out.target_notional + 1e-6) {
        const double excess = g - out.target_notional;
        std::string pick;
        bool pick_new = false;
        for (int pass = 0; pass < 2 && pick.empty(); ++pass) {
            // pass 0: the contracts the day's request added; pass 1: held contracts.
            double best_cover = -1.0, best_large = -1.0;
            std::string cover, large;
            for (const auto& [sym, q] : R) {
                const double n = notional(sym);
                if (q == 0.0 || n <= 0.0) continue;
                if ((pass == 0) != is_new(sym, q)) continue;
                if (n >= excess && (best_cover < 0.0 || n < best_cover)) {
                    best_cover = n;
                    cover = sym;
                }
                if (n > best_large) {
                    best_large = n;
                    large = sym;
                }
            }
            pick = !cover.empty() ? cover : large;
            if (!pick.empty()) {
                // Inside the class, the removal that leaves the book nearest the target in tracking
                // error; a symbol outside the covariance is not ranked.
                double best = -1.0;
                std::string te_pick;
                for (auto& [sym, q] : R) {
                    const double n = notional(sym);
                    if (q == 0.0 || n <= 0.0 || !cix.count(sym)) continue;
                    if ((pass == 0) != is_new(sym, q)) continue;
                    const double step = q > 0.0 ? 1.0 : -1.0;
                    q -= step;
                    const double t = te_sq();
                    q += step;
                    if (best < 0.0 || t < best - 1e-18) {
                        best = t;
                        te_pick = sym;
                    }
                }
                if (!te_pick.empty()) pick = te_pick;
                pick_new = is_new(pick, R[pick]);
            }
        }
        if (pick.empty()) break;
        R[pick] -= (R[pick] > 0.0 ? 1.0 : -1.0);
        (pick_new ? out.removed_new : out.removed_held) += 1;
        out.removed.emplace_back(pick, pick_new);
        g = gross();
    }
    for (const auto& [sym, q] : R) {
        const double n = notional(sym);
        if (n > 0.0) out.cut_notional += std::abs(q) * n;
    }
    return out;
}

// 9e: deliver this lap's cut in whole contracts (deliver_cut_in_whole_contracts), write the cut
// book into the optimizing sleeves' targets as the next lap's input (split by largest remainder of
// each sleeve's cut target), and log it as RISK_CUT_BOOK.
void PortfolioManager::deliver_lap_cut(int lap) {
    std::lock_guard<std::mutex> lock(mutex_);
    CutDeliveryInput in;
    in.lap_book = lap_book_before_gate_;
    for (const auto& [sid, sinfo] : strategies_) {
        if (!sinfo.use_optimization || pinned_scopes_.count(sid)) continue;
        for (const auto& [sym, pos] : sinfo.current_positions) {
            in.held[sym] += static_cast<double>(pos.quantity);
        }
    }
    in.notional_per_contract = cut_notional_per_contract_;
    in.factor = lap_cut_factor_;
    in.capital = static_cast<double>(sizing_capital_);
    if (covariance_cache_valid_) {
        in.covariance_symbols = cached_symbols_;
        in.covariance = cached_covariance_;
    }
    const CutDelivery d = deliver_cut_in_whole_contracts(in);

    std::string removed;
    for (const auto& [sym, added] : d.removed) removed += " " + sym + (added ? "(new)" : "(held)");
    auto held = [&in](const std::string& sym) {
        auto it = in.held.find(sym);
        return it == in.held.end() ? 0.0 : std::round(it->second);
    };
    std::string changed;
    for (const auto& [sym, q] : d.book) {
        const double b = std::round(in.lap_book.at(sym));
        if (q != b || q != held(sym)) {
            std::ostringstream c;
            c << " " << sym << " held=" << held(sym) << " lap=" << b << " cut=" << q;
            changed += c.str();
        }
    }
    for (const auto& [sym, q] : d.book) {
        std::vector<SleeveContribution> contributions;
        for (auto& [sid, sinfo] : strategies_) {
            if (!sinfo.use_optimization || pinned_scopes_.count(sid)) continue;
            if (!sinfo.target_positions.count(sym)) continue;
            const double target = static_cast<double>(sinfo.target_positions.at(sym).quantity);
            contributions.push_back({sid, target, target});
        }
        if (contributions.empty()) continue;
        const SleeveDistribution split = distribute_optimizer_contracts(q, contributions);
        for (size_t k = 0; k < contributions.size(); ++k) {
            strategies_.at(contributions[k].strategy_id).target_positions[sym].quantity =
                static_cast<Decimal>(static_cast<double>(split.stored[k]));
        }
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "RISK_CUT_BOOK lap=%d factor=%.10f notional lap_book=%.2f held=%.2f "
                  "target=%.2f cut_book=%.2f delivered=%.6f removed_new=%d removed_held=%d "
                  "unknown_notional=%d",
                  lap, lap_cut_factor_,
                  d.lap_notional, d.held_notional, d.target_notional, d.cut_notional,
                  d.lap_notional > 0.0 ? d.cut_notional / d.lap_notional : 1.0, d.removed_new,
                  d.removed_held, d.unknown_notional);
    INFO(std::string(buf) + " removed:" + removed + " |" + changed);
}

Result<void> PortfolioManager::optimize_positions() {
    try {
        // Get unique symbols across all strategies and collect data under lock
        std::vector<std::string> symbols;
        std::unordered_map<std::string, const InstrumentData*> all_trading_data;
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

            // Collect all instrument data
            for (const auto& [id, info] : strategies_) {
                auto trend_strategy =
                    std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                if (trend_strategy) {
                    const auto& strategy_data = trend_strategy->get_all_instrument_data();
                    for (const auto& [symbol, data] : strategy_data) {
                        all_trading_data[symbol] = &data;
                    }
                }
            }

            // Calculate weights per contract (only for valid symbols)
            weights_per_contract.reserve(symbols.size());

            for (auto const& symbol : symbols) {
                // Get contract size and price for this symbol
                auto it = all_trading_data.find(symbol);
                if (it != all_trading_data.end()) {
                    const auto& data = *(it->second);
                    double contract_size = data.contract_size;
                    double price = data.price_history.empty() ? 1.0 : data.price_history.back();
                    double fx_rate = 1.0;  // Default exchange rate

                    // Calculate notional per contract
                    double notional_per_contract = contract_size * price * fx_rate;
                    weights_per_contract.push_back(notional_per_contract /
                                                   static_cast<double>(sizing_capital_));
                } else {
                    WARN("Symbol " + symbol + " not found in trading data, using default weight");
                    weights_per_contract.push_back(0.01);  // Reasonable default
                }
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

            // 9e: the notional per contract a delivered cut is measured in, as priced here.
            for (size_t i = 0; i < symbols.size(); ++i) {
                cut_notional_per_contract_[symbols[i]] =
                    weights_per_contract[i] * static_cast<double>(sizing_capital_);
            }
        }  // End of mutex lock scope

        // Use cached covariance if valid, otherwise compute and cache
        std::vector<std::vector<double>> covariance;
        if (covariance_cache_valid_ && cached_symbols_ == symbols) {
            // Reuse cached covariance (iterations 2-5 within same day)
            covariance = cached_covariance_;
            DEBUG("Using cached covariance matrix for convergence iteration");
        } else {
            // Compute covariance matrix (first iteration or symbols changed)
            covariance = calculate_covariance_matrix(date_aligned_returns(closes_by_symbol));
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

                // Get original position from before optimization (stored in your trading data)
                for (const auto& [_, info] : strategies_) {
                    auto trend_strategy =
                        std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                    if (trend_strategy) {
                        const auto& data = trend_strategy->get_all_instrument_data();
                        auto it = data.find(symbol);
                        if (it != data.end()) {
                            original_position = it->second.final_position;
                            break;
                        }
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

std::map<std::string, double> PortfolioManager::delivered_notional_per_contract(
    const std::set<std::string>& symbols) const {
    std::map<std::string, double> out;
    std::unordered_map<std::string, const InstrumentData*> trend_data;
    for (const auto& [id, info] : strategies_) {
        auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
        if (!trend_strategy) continue;
        for (const auto& [symbol, data] : trend_strategy->get_all_instrument_data()) {
            trend_data[symbol] = &data;
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
    const RiskContext& ctx, bool finalize_phase, std::vector<std::string>& errors) {
    std::vector<RiskDecision> decisions;
    decisions.reserve(modules.size());
    errors.assign(modules.size(), std::string());
    for (size_t k = 0; k < modules.size(); ++k) {
        RiskDecision decision;
        decision.module_id = modules[k]->id();
        std::string failure;
        try {
            auto result = finalize_phase ? modules[k]->finalize(book, ctx)
                                         : modules[k]->evaluate(book, ctx);
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
                evaluate_scope_modules(risk_modules_, portfolio_positions, lap_ctx, false, errors);

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
                lap_cut_factor_ = scale;
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
                evaluate_scope_modules(modules, book, ctx, false, errors);

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

Result<void> PortfolioManager::apply_post_rounding_risk(
    const std::vector<Bar>& data, int lap,
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& prev_positions,
    std::optional<Timestamp> as_of, bool is_warmup) {
    if (risk_modules_.empty() && sleeve_risk_modules_.empty()) return Result<void>();
    // A scope refused here whose previous book was never seeded, as at the lap sites.
    std::string unseeded_refusal;

    // One scope's finalize: NONE and WARN pass, REFUSE pins, SCALE and REPLACE are rejected.
    // Returns true when the scope must be pinned.
    auto finalize_scope = [&](std::vector<RiskModulePtr>& modules,
                              const std::unordered_map<std::string, Position>& book,
                              const RiskContext& ctx) -> bool {
        // Fail CLOSED, as the two evaluate sites: a module whose finalize fails no longer
        // discards a REFUSE another module returned on the rounded book.
        std::vector<std::string> errors;
        std::vector<RiskDecision> decisions =
            evaluate_scope_modules(modules, book, ctx, true, errors);
        // SCALE and REPLACE are not applied at this point: reject them before combining.
        std::vector<RiskDecision> honoured = decisions;
        for (auto& d : honoured) {
            if (d.action == RiskAction::SCALE || d.action == RiskAction::REPLACE) {
                ERROR("Risk module " + d.module_id + " returned " + risk_action_name(d.action) +
                      " at the post-rounding point; only NONE, WARN and REFUSE are applied there");
                d.action = RiskAction::NONE;
            }
        }
        RiskVerdict verdict = combine_risk_decisions(honoured, ctx);
        std::string failed_gatekeeper;
        const bool refused_by_failure =
            refuse_on_failed_gatekeeper(modules, errors, ctx, verdict, failed_gatekeeper);
        const bool refuse = verdict.action == RiskAction::REFUSE;
        if (refuse && !scope_is_seeded(ctx.scope_id)) {
            ERROR("Risk module " +
                  (refused_by_failure ? failed_gatekeeper : decisions[verdict.winner].module_id) +
                  " refused " + risk_scope_name(ctx.scope) + " " + ctx.scope_id + " " +
                  risk_location(ctx) +
                  ", but this scope's previous book was never seeded: pinning would ship a FLAT "
                  "book, not yesterday's. Seed it with update_strategy_position before "
                  "process_market_data.");
            deliver_and_record(modules, decisions, verdict, ctx, false, errors);
            unseeded_refusal = ctx.scope_id;
            return false;
        }
        if (refuse && !refused_by_failure) {
            const RiskDecision& d = decisions[verdict.winner];
            WARN("Risk module " + d.module_id + " refused " + risk_scope_name(ctx.scope) + " " +
                 ctx.scope_id + " " + risk_location(ctx) + ": " + d.reason +
                 "; positions pinned to the previous book");
        }
        deliver_and_record(modules, decisions, verdict, ctx, refuse, errors);
        return refuse;
    };

    auto record_empty = [&](const std::vector<RiskModulePtr>& modules, const RiskContext& ctx) {
        for (const auto& module : modules) {
            RiskDecision none;
            none.module_id = module->id();
            record_risk_decision(ctx, module->id(), std::move(none), RiskAction::NONE,
                                 Decimal(1.0), true, "");
        }
    };

    auto pin = [&](const std::string& sid) {
        auto prev = prev_positions.find(sid);
        strategies_.at(sid).target_positions =
            prev != prev_positions.end() ? prev->second
                                         : std::unordered_map<std::string, Position>{};
        pinned_scopes_.insert(sid);
    };

    try {
        if (!risk_modules_.empty()) {
            std::unordered_map<std::string, Position> book;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [_, info] : strategies_) {
                    for (const auto& [symbol, pos] : info.target_positions) {
                        auto it = book.find(symbol);
                        if (it == book.end()) {
                            book[symbol] = pos;
                        } else {
                            it->second.quantity += pos.quantity;
                        }
                    }
                }
            }
            const RiskContext ctx =
                make_risk_context(RiskPhase::POST_ROUNDING, lap, RiskScope::PORTFOLIO, id_,
                                  sizing_capital_, data, as_of, is_warmup);
            if (book.empty()) {
                record_empty(risk_modules_, ctx);
            } else if (finalize_scope(risk_modules_, book, ctx)) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& [sid, _] : strategies_) pin(sid);
            }
        }

        std::vector<std::pair<std::string, double>> sleeves;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [sid, info] : strategies_) {
                if (sleeve_risk_modules_.count(sid) && !pinned_scopes_.count(sid)) {
                    sleeves.emplace_back(sid, info.allocation);
                }
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
                RiskPhase::POST_ROUNDING, lap, RiskScope::SLEEVE, sid,
                Decimal(static_cast<double>(sizing_capital_) * allocation), data, as_of,
                is_warmup);
            if (book.empty()) {
                record_empty(modules, ctx);
            } else if (finalize_scope(modules, book, ctx)) {
                std::lock_guard<std::mutex> lock(mutex_);
                pin(sid);
            }
        }
    } catch (const std::exception& e) {
        ERROR("Exception during post-rounding risk management: " + std::string(e.what()));
    }
    if (!unseeded_refusal.empty()) {
        return make_error<void>(ErrorCode::RISK_LIMIT_EXCEEDED,
                                "A risk module refused scope " + unseeded_refusal +
                                    " at the post-rounding point, but its previous book was "
                                    "never seeded; refusing the run rather than shipping a flat "
                                    "book",
                                "PortfolioManager");
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
