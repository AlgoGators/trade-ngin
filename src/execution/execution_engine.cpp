// src/execution/execution_engine.cpp

#include "trade_ngin/execution/execution_engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace trade_ngin {

ExecutionEngine::ExecutionEngine(std::shared_ptr<OrderManager> order_manager)
    : order_manager_(std::move(order_manager)) {
    
    // Register with state manager
    ComponentInfo info{
        ComponentType::ORDER_MANAGER,
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
    // Cancel all active execution jobs
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [job_id, _] : active_jobs_) {
        cancel_execution(job_id);
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
        return Result<void>({});

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

        // Generate job ID and create execution job
        std::string job_id = generate_job_id();
        ExecutionJob job{
            job_id,
            "",  // parent_order_id will be set after order creation
            algo,
            config,
            {},  // child_order_ids
            ExecutionMetrics{},
            false,  // is_complete
            std::chrono::system_clock::now(),
            {},  // end_time
            ""   // error_message
        };

        // Create parent order
        auto order_result = order_manager_->submit_order(order, "EXEC_" + job_id);
        if (order_result.is_error()) {
            return make_error<std::string>(
                order_result.error()->code(),
                order_result.error()->what(),
                "ExecutionEngine"
            );
        }

        job.parent_order_id = order_result.value();
        active_jobs_[job_id] = job;

        // Start execution based on algorithm
        Result<void> exec_result = Result<void>({});
        switch (algo) {
            case ExecutionAlgo::TWAP:
                exec_result = execute_twap(job);
                break;
            case ExecutionAlgo::VWAP:
                exec_result = execute_vwap(job);
                break;
            case ExecutionAlgo::POV:
                exec_result = execute_pov(job);
                break;
            case ExecutionAlgo::IS:
                exec_result = execute_is(job);
                break;
            case ExecutionAlgo::ADAPTIVE_LIMIT:
                exec_result = execute_adaptive_limit(job);
                break;
            case ExecutionAlgo::CUSTOM:
                if (auto it = custom_algos_.find(job_id); it != custom_algos_.end()) {
                    exec_result = it->second(job);
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

        INFO("Started execution job " + job_id + " with algorithm " + 
             std::to_string(static_cast<int>(algo)));
        
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

    // Cancel all child orders
    for (const auto& child_id : it->second.child_order_ids) {
        auto cancel_result = order_manager_->cancel_order(child_id);
        if (cancel_result.is_error()) {
            WARN("Failed to cancel child order " + child_id + ": " + 
                 cancel_result.error()->what());
        }
    }

    // Cancel parent order if it exists
    if (!it->second.parent_order_id.empty()) {
        auto cancel_result = order_manager_->cancel_order(it->second.parent_order_id);
        if (cancel_result.is_error()) {
            WARN("Failed to cancel parent order " + it->second.parent_order_id + 
                 ": " + cancel_result.error()->what());
        }
    }

    // Mark job as complete
    it->second.is_complete = true;
    it->second.end_time = std::chrono::system_clock::now();
    it->second.error_message = "Cancelled by user";

    INFO("Cancelled execution job " + job_id);
    return Result<void>({});
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ExecutionJob> jobs;

    for (const auto& [_, job] : active_jobs_) {
        if (!job.is_complete) {
            jobs.push_back(job);
        }
    }

    return Result<std::vector<ExecutionJob>>(jobs);
}

Result<void> ExecutionEngine::register_custom_algo(
    const std::string& name,
    std::function<Result<void>(const ExecutionJob&)> algo) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    custom_algos_[name] = std::move(algo);
    return Result<void>({});
}

Result<void> ExecutionEngine::execute_twap(const ExecutionJob& job) {
    try {
        // Get order details
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return order_result.error();
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // Calculate number of slices based on time horizon
        int num_slices = static_cast<int>(
            std::ceil(config.time_horizon.count() / 5.0)  // 5-minute intervals
        );

        // Generate child orders
        auto child_orders_result = generate_child_orders(job, num_slices);
        if (child_orders_result.is_error()) {
            return child_orders_result.error();
        }

        auto child_orders = child_orders_result.value();
        
        // Schedule child order submissions
        auto now = std::chrono::system_clock::now();
        auto interval = config.time_horizon / num_slices;

        for (size_t i = 0; i < child_orders.size(); ++i) {
            auto submit_time = now + i * interval;
            // TODO: Implement proper order scheduling
            auto order_result = order_manager_->submit_order(
                child_orders[i],
                "TWAP_" + job.job_id
            );
            
            if (order_result.is_error()) {
                return order_result.error();
            }
        }

        return Result<void>({});

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
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return order_result.error();
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // TODO: Implement historical volume profile analysis
        // For now, use simple time-based slicing like TWAP
        int num_slices = static_cast<int>(
            std::ceil(config.time_horizon.count() / 5.0)
        );

        auto child_orders_result = generate_child_orders(job, num_slices);
        if (child_orders_result.is_error()) {
            return child_orders_result.error();
        }

        auto child_orders = child_orders_result.value();

        // TODO: Schedule based on volume profile
        // For now, use uniform distribution
        auto now = std::chrono::system_clock::now();
        auto interval = config.time_horizon / num_slices;

        for (size_t i = 0; i < child_orders.size(); ++i) {
            auto submit_time = now + i * interval;
            auto order_result = order_manager_->submit_order(
                child_orders[i],
                "VWAP_" + job.job_id
            );
            
            if (order_result.is_error()) {
                return order_result.error();
            }
        }

        return Result<void>({});

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
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return order_result.error();
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // Start with small child order
        Order child_order = parent_order;
        child_order.quantity = std::max(
            config.min_child_size,
            parent_order.quantity * 0.01  // Start with 1% of total size
        );

        auto order_result = order_manager_->submit_order(
            child_order,
            "POV_" + job.job_id
        );

        if (order_result.is_error()) {
            return order_result.error();
        }

        // Rest of the tracking and adjustments will be done in market data callback
        return Result<void>({});

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
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return order_result.error();
        }

        const auto& parent_order = order_result.value().order;
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

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            return subscribe_result;
        }

        // Place initial passive order
        Order passive_order = parent_order;
        passive_order.type = OrderType::LIMIT;
        passive_order.price = parent_order.side == Side::BUY ? 
            parent_order.price :  // Use current best bid
            parent_order.price;   // Use current best ask

        auto order_result = order_manager_->submit_order(
            passive_order,
            "ADAPTIVE_" + job.job_id
        );

        if (order_result.is_error()) {
            return order_result;
        }

        // Update the job's child order IDs in active_jobs_
        active_jobs_[job.job_id].child_order_ids.emplace_back(order_result.value());
        return Result<void>({});

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
        auto order_result = order_manager_->get_order_status(job.parent_order_id);
        if (order_result.is_error()) {
            return order_result.error();
        }

        const auto& parent_order = order_result.value().order;
        const auto& config = job.config;

        // Implementation shortfall typically starts with larger size
        // and then adjusts based on market impact
        double initial_size = parent_order.quantity * 
                            (config.urgency_level * 0.5 + 0.1);  // 10-60% of total

        Order child_order = parent_order;
        child_order.quantity = initial_size;

        auto order_result = order_manager_->submit_order(
            child_order,
            "IS_" + job.job_id
        );

        if (order_result.is_error()) {
            return order_result.error();
        }

        // Rest of the execution will be managed through market impact analysis
        return Result<void>({});

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("IS execution error: ") + e.what(),
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

        auto& metrics = it->second.metrics;

        // Calculate metrics
        double total_fill_qty = 0.0;
        double value_weighted_price = 0.0;

        for (const auto& fill : fills) {
            total_fill_qty += fill.filled_quantity;
            value_weighted_price += fill.fill_price * fill.filled_quantity;
        }

        if (total_fill_qty > 0) {
            metrics.average_fill_price = value_weighted_price / total_fill_qty;
        }

        // Update market impact and implementation shortfall
        if (metrics.arrival_price > 0) {
            metrics.implementation_shortfall = 
                (metrics.average_fill_price - metrics.arrival_price) / 
                metrics.arrival_price;
        }

        // Update completion rate
        auto order_result = order_manager_->get_order_status(it->second.parent_order_id);
        if (order_result.is_ok()) {
            const auto& parent_order = order_result.value().order;
            metrics.completion_rate = total_fill_qty / parent_order.quantity;
        }

        // Update execution time
        metrics.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - it->second.start_time
        );

        return Result<void>({});

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