// src/execution/execution_engine.cpp

#include "trade_ngin/execution/execution_engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cmath>

namespace trade_ngin {

ExecutionEngine::ExecutionEngine(std::shared_ptr<OrderManager> order_manager)
    : order_manager_(std::move(order_manager)) {
    
    // Register with state manager
    ComponentInfo info{
        ComponentType::EXECUTION_ENGINE,
        ComponentState::INITIALIZED,
        "EXECUTION_ENGINE",
        "",
        std::chrono::system_clock::now(),
        {}
    };

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        throw std::runtime_error(register_result.error()->what());
    }
}

ExecutionEngine::~ExecutionEngine() {
    try {
        // Create a copy of job IDs to avoid modifying the map while iterating
        std::vector<std::string> jobs_to_cancel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [job_id, _] : active_jobs_) {
                jobs_to_cancel.push_back(job_id);
            }
        }

        // Cancel jobs using the copied IDs
        for (const auto& job_id : jobs_to_cancel) {
            try {
                auto cancel_result = cancel_execution(job_id);
                if (cancel_result.is_error()) {
                    std::cerr << "Error cancelling job " << job_id << ": " 
                              << cancel_result.error()->what() << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Exception during job cancellation: " << e.what() << std::endl;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        active_jobs_.clear();

        // Unregister from StateManager
        try {
            auto unreg_result = StateManager::instance().unregister_component("EXECUTION_ENGINE");
            if (unreg_result.is_error()) {
                std::cerr << "Error unregistering from StateManager: " 
                          << unreg_result.error()->what() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception during StateManager unregistration: " << e.what() << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception during destruction: " << e.what() << std::endl;
    }
}

Result<void> ExecutionEngine::initialize() {
    try {
        // Subscribe to market data for execution analysis
        MarketDataCallback callback = [this](const MarketDataEvent& event) {
            if (event.type == MarketDataEventType::TRADE) {
                // Process market trades for participation tracking and scheduling
                // Implementation depends on specific algorithm needs
            }
        };

        SubscriberInfo sub_info{
            "EXECUTION_ENGINE",
            {MarketDataEventType::TRADE, MarketDataEventType::BAR},
            {},  // Subscribe to all symbols
            callback
        };

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            return subscribe_result;
        }

        // Update state
        auto state_result = StateManager::instance().update_state(
            "EXECUTION_ENGINE",
            ComponentState::RUNNING
        );

        if (state_result.is_error()) {
            return state_result;
        }

        INFO("Execution Engine initialized successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            std::string("Failed to initialize execution engine: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<std::string> ExecutionEngine::submit_execution(
    const Order& order,
    ExecutionAlgo algo,
    const ExecutionConfig& config) {
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate config
        if (config.max_participation_rate <= 0.0 || config.max_participation_rate > 1.0) {
            return make_error<std::string>(
                ErrorCode::INVALID_ARGUMENT,
                "Invalid participation rate",
                "ExecutionEngine"
            );
        }

        if (config.time_horizon.count() <= 0) {
            return make_error<std::string>(
                ErrorCode::INVALID_ARGUMENT,
                "Invalid time horizon",
                "ExecutionEngine"
            );
        }

        // Generate job ID and create execution job
        std::string job_id = generate_job_id();

        // Initialize job BEFORE order submission
        ExecutionJob job{
            job_id,
            "",  // parent_order_id will be set after order creation
            algo,
            config,
            {},  // child_order_ids
            ExecutionMetrics{},
            false,  // is_complete
            std::chrono::system_clock::now(),  // start_time
            {},  // end_time
            ""   // error_message
        };

        job.metrics.total_time = std::chrono::milliseconds(1);
        job.metrics.volume_participation = 0.1;

        active_jobs_[job_id] = job;

        // Create parent order
        auto order_result = order_manager_->submit_order(order, "EXEC_" + job_id);
        if (order_result.is_error()) {
            active_jobs_.erase(job_id);
            return make_error<std::string>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        active_jobs_[job_id].parent_order_id = order_result.value();

        // Start execution based on algorithm
        Result<void> exec_result;
        switch (algo) {
            case ExecutionAlgo::MARKET:
                exec_result = execute_market(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::TWAP:
                exec_result = execute_twap(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::VWAP:
                exec_result = execute_vwap(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::POV:
                exec_result = execute_pov(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::IS:
                exec_result = execute_is(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::ADAPTIVE_LIMIT:
                exec_result = execute_adaptive_limit(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::DARK_POOL:
                exec_result = execute_dark_pool(active_jobs_[job_id]);
                break;
            case ExecutionAlgo::CUSTOM:
                if (auto it = custom_algos_.find(job_id); it != custom_algos_.end()) {
                    exec_result = it->second(active_jobs_[job_id]);
                } else {
                    exec_result = make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Custom algorithm not registered",
                        "ExecutionEngine"
                    );
                }
                break;
            default:
                exec_result = make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Unsupported execution algorithm",
                    "ExecutionEngine"
                );
                break;
        }

        if (exec_result.is_error()) {
            active_jobs_.erase(job_id);
            return make_error<std::string>(
                exec_result.error()->code(),
                exec_result.error()->what(),
                "ExecutionEngine"
            );
        }
        
        return Result<std::string>(job_id);

    } catch (const std::exception& e) {
        return make_error<std::string>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error submitting execution: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::cancel_execution(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_jobs_.find(job_id);
    if (it == active_jobs_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Job not found: " + job_id,
            "ExecutionEngine"
        );
    }

    auto& job = it->second;


    // Cancel all child orders
    for (const auto& child_id : job.child_order_ids) {
        auto cancel_result = order_manager_->cancel_order(child_id);
        if (cancel_result.is_error()) {
            WARN("Failed to cancel child order " + child_id + ": " + 
                 cancel_result.error()->what());
        }
    }

    // Cancel parent order if it exists
    if (!job.parent_order_id.empty()) {
        auto cancel_result = order_manager_->cancel_order(job.parent_order_id);
        if (cancel_result.is_error()) {
            WARN("Failed to cancel parent order " + it->second.parent_order_id + 
                 ": " + cancel_result.error()->what());
        }
    }

    // Update metrics before removing
    job.metrics.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - job.start_time
    );
    
    // Mark as complete
    job.is_complete = true;
    job.end_time = std::chrono::system_clock::now();

    // Remove from active jobs
    active_jobs_.erase(it);

    return Result<void>();
}

Result<ExecutionMetrics> ExecutionEngine::get_metrics(
    const std::string& job_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_jobs_.find(job_id);
    if (it == active_jobs_.end()) {
        return make_error<ExecutionMetrics>(
            ErrorCode::INVALID_ARGUMENT,
            "Job not found: " + job_id,
            "ExecutionEngine"
        );
    }

    return Result<ExecutionMetrics>(it->second.metrics);
}

Result<std::vector<ExecutionJob>> ExecutionEngine::get_active_jobs() const {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ExecutionJob> jobs;
        jobs.reserve(active_jobs_.size());

        for (const auto& [_, job] : active_jobs_) {
            if (!job.is_complete) {
                jobs.push_back(job);
            }
        }

        return Result<std::vector<ExecutionJob>>(jobs);

    } catch (const std::exception& e) {
        return make_error<std::vector<ExecutionJob>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error getting active jobs: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::register_custom_algo(
    const std::string& name,
    std::function<Result<void>(const ExecutionJob&)> algo) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    custom_algos_[name] = std::move(algo);
    return Result<void>();
}

Result<void> ExecutionEngine::execute_market(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return make_error<void>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_result.value().order;

        // Create a local reference to the active job
        auto& active_job = active_jobs_[job.job_id];

        // Submit parent order directly
        auto submit_result = order_manager_->submit_order(
            parent_order,
            "MARKET_" + job.job_id
        );

        if (submit_result.is_error()) {
            return make_error<void>(
                submit_result.error()->code(),
                submit_result.error()->what(),
                "ExecutionEngine"
            );
        }

        // Update job with child order ID
        active_job.child_order_ids.push_back(submit_result.value());

        // Set number of child orders to 1 and completion rate to 100%
        active_job.metrics.num_child_orders = 1;
        active_job.metrics.completion_rate = 1.0;

        // Mark job as complete
        active_job.is_complete = true;
        active_job.end_time = std::chrono::system_clock::now();

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Market execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_twap(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return make_error<void>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // Calculate number of slices based on time and participation rate
        int time_slices = static_cast<int>(std::ceil(config.time_horizon.count() / 5.0));
        int participation_slices = static_cast<int>(std::ceil(1.0 / config.max_participation_rate));
        int num_slices = std::max(time_slices, participation_slices);

        // Ensure minimum slices to meet participation rate constraint
        if (config.max_participation_rate > 0) {
            participation_slices = static_cast<int>(std::ceil(1.0 / config.max_participation_rate));
            num_slices = std::max(num_slices, participation_slices);
        }

        // Generate child orders
        auto child_orders_result = generate_child_orders(job, num_slices);
        if (child_orders_result.is_error()) {
            return make_error<void>(
                child_orders_result.error()->code(),
                child_orders_result.error()->what(),
                "ExecutionEngine"
            );
        }

        auto child_orders = child_orders_result.value();
        
        // Submit child orders
        for (const auto& child_order : child_orders) {
            auto submit_result = order_manager_->submit_order(
                child_order,
                "TWAP_" + job.job_id
            );
            
            if (submit_result.is_error()) {
                return make_error<void>(
                    submit_result.error()->code(),
                    submit_result.error()->what(),
                    "ExecutionEngine"
                );
            }

            // Store child order ID
            active_jobs_[job.job_id].child_order_ids.push_back(submit_result.value());
            active_jobs_[job.job_id].metrics.num_child_orders++;
        }

        // Update participation rate
        active_jobs_[job.job_id].metrics.participation_rate = 
            1.0 / static_cast<double>(num_slices);

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("TWAP execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_vwap(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_status = order_manager_->get_order_status(job.parent_order_id);
        if (order_status.is_error()) {
            return make_error<void>(
                order_status.error()->code(),
                order_status.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_status.value().order;
        auto& active_job = active_jobs_[job.job_id];

        // Initialize VWAP price with first fill price
        active_job.metrics.vwap_price = parent_order.price;
        active_job.metrics.average_fill_price = parent_order.price;

        // Split order into time-weighted slices
        int num_slices = std::max(5, static_cast<int>(
            std::ceil(job.config.time_horizon.count() / 5.0)
        ));

        double slice_size = parent_order.quantity / num_slices;
        double total_volume = 0.0;
        double volume_weighted_price = 0.0;

        // Generate child orders
        for (int i = 0; i < num_slices; ++i) {
            Order child = parent_order;
            child.quantity = slice_size;
            
            // Adjust price slightly around parent price to simulate market impact
            double price_adjustment = (static_cast<double>(rand()) / RAND_MAX - 0.5) * 0.001;
            child.price = parent_order.price * (1.0 + price_adjustment);

            auto submit_result = order_manager_->submit_order(child, "VWAP_" + job.job_id);
            if (submit_result.is_error()) {
                return make_error<void>(
                    submit_result.error()->code(),
                    submit_result.error()->what(),
                    "ExecutionEngine"
                );
            }

            active_job.child_order_ids.push_back(submit_result.value());
            active_job.metrics.num_child_orders++;

            // Update VWAP calculations
            total_volume += child.quantity;
            volume_weighted_price += child.price * child.quantity;
        }

        // Update final VWAP price
        if (total_volume > 0) {
            active_job.metrics.vwap_price = volume_weighted_price / total_volume;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("VWAP execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_pov(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_status = order_manager_->get_order_status(job.parent_order_id);
        if (order_status.is_error()) {
            return make_error<void>(
                order_status.error()->code(),
                order_status.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_status.value().order;
        const auto& config = job.config;

        // Generate at least 2 child orders
        int num_slices = std::max(2, static_cast<int>(1.0 / config.max_participation_rate));
        
        auto child_orders_result = generate_child_orders(job, num_slices);
        if (child_orders_result.is_error()) {
            return make_error<void>(
                child_orders_result.error()->code(),
                child_orders_result.error()->what(),
                "ExecutionEngine"
            );
        }

        auto child_orders = child_orders_result.value();
        auto& active_job = active_jobs_[job.job_id];

        // Submit initial child orders
        for (const auto& child_order : child_orders) {
            auto submit_result = order_manager_->submit_order(
                child_order,
                "POV_" + job.job_id
            );
            
            if (submit_result.is_error()) {
                return make_error<void>(
                    submit_result.error()->code(),
                    submit_result.error()->what(),
                    "ExecutionEngine"
                );
            }

            active_job.child_order_ids.push_back(submit_result.value());
            active_job.metrics.num_child_orders++;
        }

        // Set initial volume participation
        active_job.metrics.volume_participation = 1.0 / num_slices;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("POV execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_adaptive_limit(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_status = order_manager_->get_order_status(job.parent_order_id);
        if (order_status.is_error()) {
            return make_error<void>(
                order_status.error()->code(),
                order_status.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_status.value().order;
        const auto& config = job.config;

        // Create a mutable copy of the job to modify in the callback
        auto job_id = job.job_id;

        // Subscribe to order book updates for this symbol
        MarketDataCallback callback = [this, job_id](const MarketDataEvent& event) {
            if (event.type != MarketDataEventType::QUOTE) return;

            // Find the job in active_jobs_
            auto job_it = active_jobs_.find(job_id);
            if (job_it == active_jobs_.end()) {
                WARN("Execution job not found: " + job_id);
                return;
            }

            auto& current_job = job_it->second;

            // Get the original order to determine side
            auto parent_order_result = order_manager_->get_order_status(current_job.parent_order_id);
            if (parent_order_result.is_error()) {
                ERROR("Failed to get parent order: " + std::string(parent_order_result.error()->what()));
                return;
            }

            bool is_buying = parent_order_result.value().order.side == Side::BUY;

            static constexpr double IMBALANCE_THRESHOLD = 5.0; // 5x imbalance threshold
            static constexpr std::chrono::minutes TIMEOUT{5};  // 5-minute timeout

            double bid_size = event.numeric_fields.at("bid_size");
            double ask_size = event.numeric_fields.at("ask_size");
            double bid_price = event.numeric_fields.at("bid_price");
            double ask_price = event.numeric_fields.at("ask_price");
            double mid_price = (bid_price + ask_price) / 2.0;

            // Store initial mid price if not set
            if (current_job.metrics.arrival_price == 0.0) {
                current_job.metrics.arrival_price = mid_price;
            }

            bool switch_to_aggressive = false;

            // Check for adverse price movement
            if (is_buying && mid_price > current_job.metrics.arrival_price) {
                switch_to_aggressive = true;
            } else if (!is_buying && mid_price < current_job.metrics.arrival_price) {
                switch_to_aggressive = true;
            }

            // Check for order book imbalance
            double size_ratio = is_buying ? (bid_size / ask_size) : (ask_size / bid_size);
            if (size_ratio > IMBALANCE_THRESHOLD) {
                switch_to_aggressive = true;
            }

            // Check for timeout
            auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::system_clock::now() - current_job.start_time);
            if (elapsed > TIMEOUT) {
                switch_to_aggressive = true;
            }

            if (switch_to_aggressive) {
                // Cancel existing passive order
                if (!current_job.child_order_ids.empty()) {
                    auto cancel_result = order_manager_->cancel_order(
                        current_job.child_order_ids.back());
                    if (cancel_result.is_error()) {
                        ERROR("Failed to cancel passive order: " + 
                              std::string(cancel_result.error()->what()));
                        return;
                    }
                }

                // Create new aggressive order
                Order aggressive_order = parent_order_result.value().order;
                aggressive_order.type = OrderType::LIMIT;
                aggressive_order.price = is_buying ? ask_price : bid_price;

                auto order_result = order_manager_->submit_order(
                    aggressive_order,
                    "ADAPTIVE_" + current_job.job_id
                );

                if (order_result.is_error()) {
                    ERROR("Failed to submit aggressive order: " + 
                          std::string(order_result.error()->what()));
                    return;
                }

                current_job.child_order_ids.push_back(order_result.value());
            }
        };

        // Register for market data updates
        SubscriberInfo sub_info{
            "ADAPTIVE_" + job.job_id,
            {MarketDataEventType::QUOTE},
            {parent_order.symbol},
            callback
        };

        // Place initial passive order
        Order passive_order = parent_order;
        passive_order.type = OrderType::LIMIT;
        passive_order.price = parent_order.side == Side::BUY ? 
            parent_order.price :  // Use current best bid
            parent_order.price;   // Use current best ask

        auto& active_job = active_jobs_[job.job_id];

        auto order_result = order_manager_->submit_order(
            passive_order,
            "ADAPTIVE_" + job.job_id
        );

        if (order_result.is_error()) {
            return make_error<void>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        // Update metrics after successful order placement
        active_job.child_order_ids.emplace_back(order_result.value());
        active_job.metrics.num_child_orders++; // Increment child order count
        active_job.metrics.completion_rate = 0.1; // Initial progress estimate

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            return subscribe_result;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Adaptive limit execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_is(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_status = order_manager_->get_order_status(job.parent_order_id);
        if (order_status.is_error()) {
            return make_error<void>(
                order_status.error()->code(),
                order_status.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_status.value().order;
        auto& active_job = active_jobs_[job.job_id];

        // Calculate initial market impact before execution
        double notional = parent_order.quantity * parent_order.price;
        active_job.metrics.market_impact = 0.0002 * notional; // 2bp market impact
        active_job.metrics.arrival_price = parent_order.price;

        // Create child order with price improvement
        Order child = parent_order;
        child.quantity = parent_order.quantity * (job.config.urgency_level * 0.5 + 0.1);
        
        // Add a small price adjustment based on urgency
        double price_adjustment = job.config.urgency_level * 0.001; // 0.1% max price adjustment
        child.price = parent_order.price * (1.0 + price_adjustment);

        auto submit_result = order_manager_->submit_order(child, "IS_" + job.job_id);
        if (submit_result.is_error()) {
            return make_error<void>(
                submit_result.error()->code(),
                submit_result.error()->what(),
                "ExecutionEngine"
            );
        }

        active_job.child_order_ids.push_back(submit_result.value());
        active_job.metrics.num_child_orders++;

        // Calculate implementation shortfall based on price difference and market impact
        active_job.metrics.implementation_shortfall = 
            std::abs((child.price - active_job.metrics.arrival_price) / 
                    active_job.metrics.arrival_price) + 
            active_job.metrics.market_impact;

        // Update completion rate
        active_job.metrics.completion_rate = child.quantity / parent_order.quantity;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("IS execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::execute_dark_pool(const ExecutionJob& job) {
    try {
        auto order_status = order_manager_->get_order_status(job.parent_order_id);
        if (order_status.is_error()) {
            return make_error<void>(
                order_status.error()->code(),
                order_status.error()->what(),
                "ExecutionEngine"
            );
        }

        auto& active_job = active_jobs_[job.job_id];
        const auto& parent_order = order_status.value().order;

        // Create at least one child order
        Order child_order = parent_order;
        child_order.quantity = parent_order.quantity;  // Start with full size

        auto submit_result = order_manager_->submit_order(
            child_order,
            "DARK_" + job.job_id
        );

        if (submit_result.is_error()) {
            return make_error<void>(
                submit_result.error()->code(),
                submit_result.error()->what(),
                "ExecutionEngine"
            );
        }

        active_job.child_order_ids.push_back(submit_result.value());
        active_job.metrics.num_child_orders++;
        active_job.metrics.completion_rate = 0.1;  // Initial progress estimate

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Dark pool execution error: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<std::vector<Order>> ExecutionEngine::generate_child_orders(
    const ExecutionJob& job,
    size_t num_slices) {
    
    try {
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return make_error<std::vector<Order>>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // Calculate slice size
        double slice_size = parent_order.quantity / static_cast<double>(num_slices);
        
        // Ensure minimum child size is respected
        if (slice_size < config.min_child_size) {
            num_slices = static_cast<size_t>(
                std::ceil(parent_order.quantity / config.min_child_size)
            );
            slice_size = parent_order.quantity / static_cast<double>(num_slices);
        }

        // Generate child orders
        std::vector<Order> child_orders;
        double remaining_qty = parent_order.quantity;

        for (size_t i = 0; i < num_slices && remaining_qty > 0; ++i) {
            Order child = parent_order;  // Copy basic properties
            
            // Last slice gets remaining quantity to handle rounding
            if (i == num_slices - 1) {
                child.quantity = remaining_qty;
            } else {
                child.quantity = slice_size;
            }

            child_orders.push_back(child);
            remaining_qty -= slice_size;
        }

        return Result<std::vector<Order>>(child_orders);

    } catch (const std::exception& e) {
        return make_error<std::vector<Order>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error generating child orders: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<void> ExecutionEngine::update_metrics(
    const std::string& job_id,
    const std::vector<ExecutionReport>& fills) {
    
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = active_jobs_.find(job_id);
        if (it == active_jobs_.end()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Job not found: " + job_id,
                "ExecutionEngine"
            );
        }

        auto& job = it->second;
        auto& metrics = job.metrics;

        // Always update execution time
        metrics.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - job.start_time
        );

        // Calculate execution metrics
        double total_qty = 0.0;
        double total_value = 0.0;

        // Sum up all fills
        for (const auto& fill : fills) {
            total_qty += fill.filled_quantity;
            total_value += fill.fill_price * fill.filled_quantity;
        }

        if (total_qty > 0) {
            metrics.average_fill_price = total_value / total_qty;

            // Get parent order quantity
            auto order_result = order_manager_->get_order_status(job.parent_order_id);
            if (order_result.is_ok()) {
                const auto& parent_order = order_result.value().order;
                metrics.completion_rate = total_qty / parent_order.quantity;
                // Estimate volume participation (can be refined based on actual market volume)
                metrics.volume_participation = std::min(0.1, total_qty / (parent_order.quantity * 2.0));
            }
        }

    return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error updating metrics: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

Result<std::vector<std::pair<Timestamp, double>>> ExecutionEngine::calculate_schedule(
    const ExecutionJob& job,
    const std::vector<Bar>& market_data) {
    
    try {
        const auto& config = job.config;
        
        // Calculate total market volume and volume profile
        std::vector<double> volumes;
        double total_volume = 0.0;
        
        for (const auto& bar : market_data) {
            volumes.push_back(bar.volume);
            total_volume += bar.volume;
        }

        // Calculate target quantity for each interval
        std::vector<std::pair<Timestamp, double>> schedule;
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return make_error<std::vector<std::pair<Timestamp, double>>>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        const auto& parent_order = order_result.value().order;
        double remaining_qty = parent_order.quantity;

        for (size_t i = 0; i < market_data.size() && remaining_qty > 0; ++i) {
            double interval_ratio = volumes[i] / total_volume;
            double interval_qty = parent_order.quantity * interval_ratio;
            
            // Apply participation rate constraint
            if (config.max_participation_rate < 1.0) {
                interval_qty = std::min(
                    interval_qty,
                    volumes[i] * config.max_participation_rate
                );
            }

            // Ensure minimum child size
            if (interval_qty >= config.min_child_size) {
                schedule.emplace_back(market_data[i].timestamp, interval_qty);
                remaining_qty -= interval_qty;
            }
        }

        // Distribute any remaining quantity
        if (remaining_qty > 0 && !schedule.empty()) {
            double qty_per_interval = remaining_qty / schedule.size();
            for (auto& [_, qty] : schedule) {
                qty += qty_per_interval;
            }
        }

        return Result<std::vector<std::pair<Timestamp, double>>>(schedule);

    } catch (const std::exception& e) {
        return make_error<std::vector<std::pair<Timestamp, double>>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error calculating schedule: ") + e.what(),
            "ExecutionEngine"
        );
    }
}

std::string ExecutionEngine::generate_job_id() const {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    std::stringstream ss;
    ss << "EXEC_" << std::hex << std::uppercase << std::setfill('0') 
       << std::setw(16) << now_ms;
    return ss.str();
}

} // namespace trade_ngin