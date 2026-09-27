// Owned PostgreSQL only. No engine, strategies, config loader, network or email.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
thread_local bool fail_projector_allocation = false;
thread_local bool scanning_allocation_stack = false;
thread_local bool projector_allocation_failed = false;
}

// Test binary only: a one-shot allocation failure inside the real projector.
// All unarmed allocations retain the ordinary malloc/free behavior.
void* operator new(std::size_t size) {
    if (fail_projector_allocation && !scanning_allocation_stack) {
        scanning_allocation_stack = true;
        void* frames[48];
        const int count = backtrace(frames, 48);
        bool in_projector = false;
        for (int index = 0; index < count; ++index) {
            Dl_info info{};
            if (dladdr(frames[index], &info) && info.dli_sname &&
                std::strstr(info.dli_sname, "project_live_config_fields")) {
                in_projector = true;
                break;
            }
        }
        scanning_allocation_stack = false;
        if (in_projector) {
            fail_projector_allocation = false;
            projector_allocation_failed = true;
            throw std::bad_alloc();
        }
    }
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace trade_ngin;

class ReentrantAttachDatabase final : public PostgresDatabase {
public:
    using PostgresDatabase::PostgresDatabase;

    void arm_in_progress_attach(const PublicationEvidenceToken& token,
                                const ConsumptionProjection& projection) {
        late_token_ = token;
        late_projection_.emplace(projection);
        armed_ = true;
    }
    bool attempted_late_attach() const noexcept { return attempted_; }
    bool refused_late_attach() const noexcept { return refused_; }

    Result<void> store_live_results_complete(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& metrics,
        const std::unordered_map<std::string, int>& int_metrics,
        const nlohmann::json& config, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "trading.live_results",
        const std::string& portfolio_type = "system") override {
        if (armed_) {
            armed_ = false;
            attempted_ = true;
            refused_ = attach_live_consumption(late_token_, *late_projection_).is_error();
        }
        return PostgresDatabase::store_live_results_complete(strategy_id,date,metrics,
            int_metrics,config,portfolio_id,table_name,portfolio_type);
    }

private:
    PublicationEvidenceToken late_token_;
    std::optional<ConsumptionProjection> late_projection_;
    bool armed_ = false;
    bool attempted_ = false;
    bool refused_ = false;
};

// Bounded, synthetic native observations. This is not a strategy invocation.
ConsumptionProjection synthetic_final_projection(bool controlled, bool benchmark_error,
                                                 std::size_t charge_count = 0,
                                                 const AppConfig* selected_config = nullptr) {
    RunConsumption run;
    run.controlled_selection = controlled;
    run.selection.emplace();
    run.selection->outcome = RunCallOutcome::ReturnedOk;
    SelectionRead selected;
    selected.name = "TREND";
    selected.enabled_live_read = true;
    selected.enabled_live_present = true;
    selected.enabled_live_value = true;
    selected.allocation_read = true;
    selected.allocation_value = 1.0;
    selected.effective_allocation = 1.0;
    if (selected_config) {
        SelectionConsumption observed;
        auto selected_result = controlled ?
            select_controlled_live_strategies(selected_config->strategies_config, &observed) :
            select_enabled_live_strategies(selected_config->strategies_config, &observed);
        if (selected_result.is_error()) throw std::runtime_error("synthetic_selection_failed");
        run.selection->payload = std::move(observed);
    } else if (controlled) {
        run.selection->payload.controlled_validation.push_back(selected);
        run.selection->payload.controlled_sum = 1.0;
    }
    if (!selected_config) {
        run.selection->payload.ordinary_selection.push_back(selected);
        run.selection->payload.ordinary_sum = 1.0;
        run.selection->payload.ordinary_normalized = false;
    }
    run.primary_factory.emplace();
    run.primary_factory->outcome = RunCallOutcome::ReturnedOk;
    FactoryRead factory;
    factory.name = "TREND";
    factory.profile = FactoryProfile::Standard;
    factory.effective_allocation = 1.0;
    factory.initial_capital_argument = 100000.0;
    factory.allocated_capital = 100000.0;
    factory.construction = SetupStage::Succeeded;
    factory.initialize = SetupStage::Succeeded;
    factory.start = SetupStage::Succeeded;
    run.primary_factory->payload.entries.push_back(factory);
    auto& registration = run.registrations.emplace_back();
    registration.identity = "TREND";
    registration.call.outcome = RunCallOutcome::ReturnedOk;
    registration.call.payload.outcome = PortfolioCallOutcome::ReturnedOk;
    registration.call.payload.initial_allocation = 1.0;
    registration.call.payload.min_allocation = 0.0;
    registration.call.payload.max_allocation = 1.0;
    registration.call.payload.total_allocation = 1.0;
    registration.call.payload.total_within_limit = true;
    registration.call.payload.stored_allocation = 1.0;
    run.historical_days_reached = true;
    run.historical_days = 252;
    run.market_fetch.outcome = RunCallOutcome::ReturnedOk;
    run.arrow_conversion.outcome = RunCallOutcome::ReturnedOk;
    run.market_input_completed = true;
    run.history_loop = {true,true};
    run.non_trading_decision_reached = true;
    run.skip_strategy_processing = false;
    run.preparation_stage = {true,true};
    run.primary_stage = {true,true};
    run.preparation.emplace();
    run.preparation->identity = "TREND";
    run.preparation->call.outcome = RunCallOutcome::ReturnedOk;
    run.preparation->call.payload.profile = StrategyConsumptionProfile::Standard;
    run.primary.emplace();
    run.primary->outcome = RunCallOutcome::ReturnedOk;
    run.primary->payload.outcome = PortfolioCallOutcome::ReturnedOk;
    auto& primary_strategy = run.primary->payload.strategies.emplace_back();
    primary_strategy.strategy_id = "TREND";
    primary_strategy.outcome = PortfolioCallOutcome::ReturnedOk;
    primary_strategy.strategy.profile = StrategyConsumptionProfile::Standard;
    for (std::size_t index = 0; index < charge_count; ++index) {
        // Finite synthetic settings with long valid numeric spellings make the
        // real PostgreSQL JSONB text expansion observable below the compact cap.
        constexpr double value_read = 1.2345678901234567e40;
        auto& charge = run.primary->payload.strategy_charges.emplace_back();
        charge.purpose = PortfolioChargePurpose::PerStrategy;
        charge.strategy_id = "TREND";
        charge.symbol = "ES";
        charge.charge_call = PortfolioCallOutcome::ReturnedOk;
        auto& value = charge.charge;
        value.spread.baseline_spread_ticks = value_read;
        value.spread.min_spread_ticks = value_read;
        value.spread.max_spread_ticks = value_read;
        value.spread.spread_cost_multiplier = value_read;
        value.spread.tick_size = value_read;
        value.volatility.lambda = value_read;
        value.volatility.min_multiplier = value_read;
        value.volatility.max_multiplier = value_read;
        value.impact.min_adv = value_read;
        value.impact.min_participation = value_read;
        value.impact.max_participation = value_read;
        value.impact.max_impact_bps = value_read;
        value.explicit_fee_per_contract = value_read;
        value.point_value = value_read;
    }
    run.execution_loop = {true,true};
    auto& batch = run.execution_batches.emplace_back();
    batch.identity = "TREND";
    batch.call.outcome = RunCallOutcome::ReturnedOk;
    batch.call.payload.state = DailyExecutionState::returned;
    run.pnl_path_decision_reached = true;
    run.pnl_path_eligible = true;
    run.pnl_loop = {true,true};
    auto& pnl = run.pnl_finalizations.emplace_back();
    pnl.strategy = "TREND";
    pnl.record_operands(true,1.0,100000.0,100000.0);
    pnl.call.outcome = RunCallOutcome::ReturnedOk;
    run.diagnostics_reached = true;
    run.diagnostics_completed = true;
    run.snapshot_risk.emplace();
    run.snapshot_risk->outcome = RunCallOutcome::ReturnedOk;
    run.snapshot_risk->payload.capital = Decimal::from_raw(
        std::numeric_limits<std::int64_t>::min());
    run.benchmark_decision_reached = true;
    run.benchmark_mode = RunBenchmarkMode::Live;
    run.benchmark_state = benchmark_error ? RunBenchmarkState::NonfatalError :
                                            RunBenchmarkState::Succeeded;
    return project_run_consumption(run);
}

void add_disabled_capacity_pairs(AppConfig& config, std::size_t count) {
    nlohmann::json pairs = nlohmann::json::array();
    for (std::size_t index = 0; index < count; ++index) {
        pairs.push_back({2147483647, 2147483647});
    }
    config.strategies_config["PAD"] = {{"enabled_live", false},
        {"type", "TrendFollowingStrategy"},
        {"config", {{"ema_windows", std::move(pairs)}}}};
}

int main(int argc, char** argv) {
    try {
        AppConfig synthetic;
        synthetic.portfolio_id = "BOOK";
        synthetic.strategies_config = {{"TREND",{{"enabled_live",true},
            {"default_allocation",1.0},{"type","TrendFollowingStrategy"}}}};
        const std::string mode = argc == 2 ? argv[1] : "";
        std::string case_mode = mode;
        if (case_mode.size() > 11 &&
            case_mode.compare(case_mode.size()-11,11,"_controlled") == 0 &&
            (case_mode.find("required_") != std::string::npos ||
             case_mode.find("legacy_attachment") == 0 ||
             case_mode.find("legacy_prior_attachment") == 0))
            case_mode.resize(case_mode.size()-11);
        const bool prior_refusal = case_mode.find("required_prior_") == 0 ||
            case_mode == "legacy_prior_attachment";
        if (case_mode.find("required_prior_") == 0)
            case_mode = "required_" + case_mode.substr(std::string("required_prior_").size());
        else if (case_mode == "legacy_prior_attachment")
            case_mode = "legacy_attachment";
        if (mode.find("publish_required_early_unavailable_complete") == 0 ||
            mode == "snapshot_early_unavailable")
            synthetic.strategies_config["DISABLED"] = {{"enabled_live",false},
                {"config",{{"ema_windows",{{1}}},{"private","synthetic-secret"}}}};
        const bool capacity_early = mode.find("publish_required_capacity_early") == 0 ||
            mode.find("publish_inspection_capacity_early_legacy") == 0 ||
            mode == "snapshot_capacity_early";
        if (capacity_early) {
            StrategySelection selected;
            selected.names = {"TREND"};
            selected.allocations = {{"TREND", 1.0}};
            selected.configs = {{"TREND", synthetic.strategies_config.at("TREND")}};
            std::size_t low = 0, high = 100000;
            while (low + 1 < high) {
                const auto middle = low + (high - low) / 2;
                auto trial = synthetic;
                add_disabled_capacity_pairs(trial, middle);
                const auto capture = build_live_config_inspection_capture(
                    trial, selected, std::nullopt);
                if (capture.at("status") == "available" &&
                    capture.dump().size() <= 2u * 1024u * 1024u - 8192u)
                    low = middle;
                else
                    high = middle;
            }
            add_disabled_capacity_pairs(synthetic, low);
            if (low == 0) return 51;
        }
        const bool missing_member = mode.find("missing_member") == 0;
        const std::string engine = missing_member ? "LIVE_MISSING_TREND" : "LIVE_TREND";
        if (missing_member) {
            synthetic.strategies_config["TREND"]["default_allocation"] = 0.5;
            synthetic.strategies_config["MISSING"] = synthetic.strategies_config["TREND"];
        }
        const auto snapshot = build_runtime_trading_snapshot(synthetic);
        if (snapshot.is_error()) return 19;
        if (argc == 2 && (mode == "snapshot" || mode == "snapshot_early_unavailable" ||
                          mode == "snapshot_capacity_early")) {
            std::cout << "RUNTIME_SNAPSHOT=" << snapshot.value().dump() << '\n';
            return 0;
        }
        const char* raw = std::getenv("ALGOLENS_TEST_DB");
        if (!raw || argc != 2) return 2;
        std::string dsn(raw);
        if (dsn.rfind("host=/tmp/algolens-repair-pg-", 0) != 0 ||
            dsn.find(" dbname=algolens_test_") == std::string::npos ||
            dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos)
            return 2;
        ReentrantAttachDatabase db(dsn);
        if (db.connect().is_error()) return 3;
        if (case_mode == "required_no_active_attach") {
            if (db.attach_live_consumption(PublicationEvidenceToken{},
                    ConsumptionProjection::unavailable(
                        ConsumptionUnavailableReason::InstrumentationMissing)).is_ok()) return 33;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (mode == "inspection_direct") {
            if (db.store_live_run_metadata(std::chrono::system_clock::from_time_t(1790035200),
                    "LIVE_TREND", "BOOK", {}, {{"config_inspection", {}}}, {}).is_ok()) return 25;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        const bool controlled = mode.find("controlled") != std::string::npos;
        const bool required = case_mode.find("required_") != std::string::npos;
        const bool request_token = required || case_mode == "legacy_attachment";
        PublicationEvidenceToken evidence_token;
        auto date = std::chrono::system_clock::from_time_t(1790035200);
        if (case_mode == "required_unknown_mode_prevalid" ||
            case_mode == "required_failure_prevalid" ||
            case_mode == "required_stop_prevalid") {
            auto seeded = db.begin_live_publication(engine,"BOOK",date,snapshot.value(),
                false,"local-test",PublicationEvidenceRequirement::RequiredFinalObservations,
                &evidence_token);
            if (seeded.is_error() || seeded.value() || !evidence_token.valid()) return 48;
            db.abandon_live_publication();
            if (case_mode == "required_stop_prevalid") {
                pqxx::connection setup_connection(dsn);
                pqxx::work setup(setup_connection);
                setup.exec("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'");
                setup.exec("INSERT INTO trading.runtime_intents "
                    "(registry_id,portfolio_id,engine_strategy_id,action,registry_revision,"
                    "config_snapshot,status,requested_by,request_reason,approved_by,"
                    "approval_reason,approved_at) "
                    "SELECT 'trend','BOOK','LIVE_TREND','stop',runtime_revision,$1::jsonb,"
                    "'approved','1','synthetic request','2','synthetic approval',now() "
                    "FROM trading.strategy_registry WHERE id='trend'",
                    pqxx::params{snapshot.value().dump()});
                setup.commit();
            }
        }
        const auto requirement = (case_mode == "required_unknown_mode" ||
                                  case_mode == "required_unknown_mode_prevalid") ?
            static_cast<PublicationEvidenceRequirement>(-1) :
            (required ? PublicationEvidenceRequirement::RequiredFinalObservations :
                        PublicationEvidenceRequirement::LegacyNotCollected);
        auto started = db.begin_live_publication(engine,
            (mode == "membership" || case_mode == "required_failure_prevalid") ? "SECOND" : "BOOK", date,
            snapshot.value(), controlled,
            mode == "publish_inspection_bad_producer_version" ? "bad version!" : "local-test",
            requirement, case_mode == "required_null_output" ? nullptr :
                         (request_token ? &evidence_token : nullptr));
        if (case_mode == "required_unknown_mode" || case_mode == "required_unknown_mode_prevalid" ||
            case_mode == "required_failure_prevalid" || case_mode == "required_null_output") {
            if (!started.is_error() || evidence_token.valid()) return 34;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (mode == "legacy_changed" || mode == "membership") {
            if (!started.is_error()) return 20;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (mode == "stop_controlled" || mode == "required_stop_controlled" ||
            case_mode == "required_stop_prevalid") {
            if (started.is_error() || !started.value() || evidence_token.valid()) return 21;
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (started.is_error() || started.value()) return 4;
        if (request_token && !evidence_token.valid()) return 31;
        if (case_mode == "required_already_active") {
            auto second_token = evidence_token;
            auto refused = db.begin_live_publication(engine,"BOOK",date,snapshot.value(),
                controlled,"local-test",requirement,&second_token);
            if (!refused.is_error() || second_token.valid() || !evidence_token.valid()) return 35;
        }
        nlohmann::json inspection_capture;
        if ((mode.find("inspection") != std::string::npos ||
             (required && case_mode != "required_missing_capture") ||
             case_mode == "legacy_attachment") &&
            mode != "publish_inspection_legacy_writer") {
            StrategySelection selected;
            selected.names = {"TREND"};
            selected.allocations = {{"TREND", 1.0}};
            selected.configs = {{"TREND", synthetic.strategies_config.at("TREND")}};
            if (mode == "publish_inspection_unavailable")
                synthetic.strategies_config["DISABLED"] = {{"enabled_live", false},
                    {"config", {{"ema_windows", {{1}}}, {"private", "synthetic-secret"}}}};
            inspection_capture = build_live_config_inspection_capture(
                synthetic, selected, std::nullopt);
            inspection_capture["capture_schema_version"] = 1;
            inspection_capture["captured_at"] = "2026-09-22T12:34:56.123456Z";
            if (capacity_early) {
                if (inspection_capture.at("status") != "available") return 52;
                std::cout << "INITIAL_CAPTURE_COMPACT_BYTES=" <<
                    inspection_capture.dump().size() << '\n';
                std::cout << "INITIAL_CAPTURE_FIELDS=" <<
                    inspection_capture.at("supplied").at("fields").size() << '\n';
                std::cout << "INITIAL_CAPTURE=" << inspection_capture.dump() << '\n';
            }
            if (mode.find("publish_inspection_fix2_") == 0 &&
                mode != "publish_inspection_fix2_resource_failure") {
                auto& selected_allocation = inspection_capture["selected_trend"]["strategies"][0]
                    ["selected_allocation"];
                if (mode == "publish_inspection_fix2_integer_integer" ||
                    mode == "publish_inspection_fix2_integer_float" ||
                    mode == "publish_inspection_fix2_signed_unsigned")
                    selected_allocation = 1;
                if (mode == "publish_inspection_fix2_zero_mixed") selected_allocation = 0;
                if (mode == "publish_inspection_fix2_negative_mixed" ||
                    mode == "publish_inspection_fix2_negative_unsigned_mismatch")
                    selected_allocation = -1;
                if (mode == "publish_inspection_fix2_large_equal_integer" ||
                    mode == "publish_inspection_fix2_large_unequal_mixed")
                    selected_allocation = 9007199254740993LL;
                if (mode == "publish_inspection_fix2_large_unequal_mixed_inverse")
                    selected_allocation = 9007199254740992.0;
                if (mode == "publish_inspection_fix2_nonfinite")
                    selected_allocation = std::numeric_limits<double>::infinity();
                if (mode == "publish_inspection_fix2_non_number")
                    selected_allocation = "synthetic-not-a-number";
            }
            auto field = [&](const std::string& path) -> nlohmann::json& {
                for (auto& item : inspection_capture["supplied"]["fields"])
                    if (item.at("path") == path) return item;
                throw std::runtime_error("missing synthetic projection field");
            };
            if (mode == "publish_inspection_fix1_omitted_type")
                field("/portfolio_id")["value_type"] =
                    {{"private", "synthetic-private-omitted-type"}};
            if (mode == "publish_inspection_fix1_descriptor_promotion") {
                auto& item = field("/portfolio_id");
                item["classification"] = "source_supported_config_input";
                item["reason"] = "source_reader";
                item["condition"] = "source_path";
                item["value_type"] = "number";
                item["unit"] = "account_currency";
                item["value_origin"] = "app_config_member";
                item["value_state"] = "included";
                item["value"] = 42.0;
            }
            if (mode == "publish_inspection_fix1_wrong_reason")
                field("/initial_capital")["reason"] = "backtest_only";
            if (mode == "publish_inspection_fix1_wrong_condition")
                field("/initial_capital")["condition"] = "no_active_profile_reader";
            if (mode == "publish_inspection_fix1_wrong_unit")
                field("/initial_capital")["unit"] = "fraction";
            if (mode == "publish_inspection_fix1_wrong_origin")
                field("/initial_capital")["value_origin"] = "configured_strategy_leaf";
            if (mode == "publish_inspection_fix1_wrong_type")
                field("/backtest/lookback_years")["value_type"] = "number";
            if (mode == "publish_inspection_fix1_wrong_state") {
                auto& item = field("/strategies/TREND/config/fx_rate");
                item["value_type"] = "number";
                item["value_origin"] = "configured_strategy_leaf";
                item["value_state"] = "included";
                item["value"] = 1.5;
            }
            if (mode == "publish_inspection_fix1_integer_underflow")
                field("/live/historical_days")["value"] = -2147483649LL;
            if (mode == "publish_inspection_fix1_pair_underflow")
                field("/strategy_defaults/fdm")["value"][0][0] = -2147483649LL;
            if (mode == "publish_inspection_fix1_float_capture_version")
                inspection_capture["capture_schema_version"] = 1.0;
            if (mode == "publish_inspection_fix1_float_projection_version")
                inspection_capture["supplied"]["projection_version"] = 1.0;
            if (mode == "publish_inspection_fix1_float_selected_version")
                inspection_capture["selected_trend"]["schema_version"] = 1.0;
            if (mode == "publish_inspection_fix1_bad_month")
                inspection_capture["captured_at"] = "2026-13-22T12:34:56.123456Z";
            if (mode == "publish_inspection_fix1_bad_day")
                inspection_capture["captured_at"] = "2026-04-31T12:34:56.123456Z";
            if (mode == "publish_inspection_fix1_bad_hour")
                inspection_capture["captured_at"] = "2026-09-22T24:34:56.123456Z";
            if (mode == "publish_inspection_fix1_bad_minute")
                inspection_capture["captured_at"] = "2026-09-22T12:60:56.123456Z";
            if (mode == "publish_inspection_fix1_bad_second")
                inspection_capture["captured_at"] = "2026-09-22T12:34:60.123456Z";
            if (mode == "publish_inspection_fix1_bad_leap")
                inspection_capture["captured_at"] = "1900-02-29T12:34:56.123456Z";
            if (mode == "publish_inspection_fix1_bad_year")
                inspection_capture["captured_at"] = "0000-02-29T12:34:56.123456Z";
            if (mode == "publish_inspection_fix1_valid_leap")
                inspection_capture["captured_at"] = "2000-02-29T12:34:56.123456Z";
            if (mode == "publish_inspection_bad_shape") inspection_capture["unexpected"] = "synthetic-secret";
            if (mode == "publish_inspection_bad_version") inspection_capture["capture_schema_version"] = 2;
            if (mode == "publish_inspection_bad_path")
                inspection_capture["supplied"]["fields"][0]["path"] = "/credentials";
            if (mode == "publish_inspection_missing_field")
                inspection_capture["supplied"]["fields"].erase(0);
            if (mode == "publish_inspection_missing_strategy_field") {
                auto& fields = inspection_capture["supplied"]["fields"];
                for (auto it = fields.begin(); it != fields.end(); ++it) {
                    if (it->at("path") == "/strategies/TREND/config/weight") {
                        fields.erase(it);
                        break;
                    }
                }
            }
            if (mode == "publish_inspection_bad_selected")
                inspection_capture["selected_trend"]["strategies"][0]["strategy_id"] = "OTHER";
            if (mode == "publish_inspection_capacity_uint64") {
                inspection_capture["selected_trend"]["strategies"][0]
                    ["constructor_normalized"]["max_history_size"] =
                    std::numeric_limits<std::uint64_t>::max();
            }
            if (mode.find("publish_required_bad_capture_complete") == 0 ||
                case_mode == "required_missing_attachment_bad_capture")
                inspection_capture["unexpected"] = "synthetic-private-rejected";
            if (case_mode == "required_missing_attachment_oversize")
                inspection_capture["unexpected"] = std::string(2u*1024u*1024u,'X');
        }
        if (mode.find("required_stale_") == 0 ||
            mode.find("required_cross_database") == 0) {
            auto queue_full = [&](PostgresDatabase& target) {
                std::vector<Position> fresh{Position("ES",Quantity(12),Price(100),
                    Decimal(0),Decimal(0),date)};
                return target.store_positions(fresh,engine,"TREND","BOOK","trading.positions").is_ok() &&
                    target.store_risk_limits(engine,"BOOK",{{"max_gross_leverage",4.0}}).is_ok() &&
                    target.store_live_results_complete(engine,date,{{"total_pnl",123}},
                        {},{},"BOOK").is_ok() &&
                    target.store_live_run_metadata(date,engine,"BOOK",{{"TREND",1.0}},
                        {{"total_capital",500000.0},{"config_inspection",inspection_capture}},{}).is_ok() &&
                    target.store_trading_equity_curve(engine,date,500000,"BOOK").is_ok() &&
                    target.seed_qt_positions_from_system(engine,"TREND","BOOK","2026-09-22").is_ok() &&
                    target.store_live_run_inputs(engine,"BOOK",date,
                        {{"trade_ngin_sha","local-test"},{"config_snapshot",snapshot.value()},
                         {"universe",{"ES"}},{"data_window",{}},{"engine_flags",{}}}).is_ok();
            };
            const auto projection = ConsumptionProjection::unavailable(
                ConsumptionUnavailableReason::InstrumentationMissing);
            const bool abandon_first = mode.find("after_abandon") != std::string::npos;
            if (abandon_first) db.abandon_live_publication();
            else if (!queue_full(db) ||
                     db.attach_live_consumption(evidence_token,projection).is_error() ||
                     db.publish_live_publication().is_error()) return 39;
            pqxx::connection observer_connection(dsn);
            auto prior = [&] {
                pqxx::read_transaction txn(observer_connection);
                const auto metadata = txn.exec("SELECT portfolio_config::text FROM trading.live_run_metadata");
                const auto results = txn.exec("SELECT to_jsonb(r)::text FROM trading.live_results r");
                return std::pair<std::string,std::string>{
                    metadata.empty() ? "" : metadata[0][0].as<std::string>(),
                    results.empty() ? "" : results[0][0].as<std::string>()};
            };
            const auto saved = prior();
            PublicationEvidenceToken next;
            PostgresDatabase cross(dsn);
            PostgresDatabase* target = &db;
            if (mode.find("cross_database") != std::string::npos) {
                if (cross.connect().is_error()) return 40;
                target = &cross;
            }
            auto begun_b = target->begin_live_publication(engine,"BOOK",date,snapshot.value(),
                controlled,"local-test",PublicationEvidenceRequirement::RequiredFinalObservations,&next);
            if (begun_b.is_error() || begun_b.value() || !next.valid() || !queue_full(*target)) return 41;
            if (target->attach_live_consumption(evidence_token,projection).is_ok()) return 42;
            if (target->publish_live_publication().is_ok() || prior() != saved) return 43;
            if (mode.find("cross_database") != std::string::npos &&
                !abandon_first) {
                // Both objects have discarded A/B; C is opened on the original object.
            }
            PublicationEvidenceToken third;
            auto begun_c = db.begin_live_publication(engine,"BOOK",date,snapshot.value(),
                controlled,"local-test",PublicationEvidenceRequirement::RequiredFinalObservations,&third);
            if (begun_c.is_error() || begun_c.value() || !third.valid() || !queue_full(db) ||
                db.attach_live_consumption(third,projection).is_error() ||
                db.publish_live_publication().is_error()) return 44;
            std::cout << "STABLE_AFTER_STALE=1\n";
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (mode.find("historical_") == 0) {
            pqxx::connection other(dsn);
            {
                pqxx::work editor(other);
                if (mode.find("historical_aba") == 0) {
                    editor.exec("UPDATE trading.strategy_registry SET lifecycle='retired';"
                                "UPDATE trading.strategy_registry SET lifecycle='live'");
                } else if (mode == "historical_superseded_controlled") {
                    editor.exec("UPDATE trading.runtime_intents SET status='superseded' WHERE status='approved'");
                }
                editor.commit();
            }
            auto raw_result = db.execute_scoped_live_update(
                "UPDATE trading.positions SET quantity=42 WHERE date='2026-09-21'",
                engine,"BOOK");
            std::vector<Position> previous{Position("ES",Quantity(99),Price(100),Decimal(0),Decimal(0),
                date-std::chrono::hours(24))};
            auto typed_result = db.store_positions(previous,engine,"TREND","BOOK","trading.positions");
            const bool allowed = mode == "historical_valid";
            if (raw_result.is_ok() != allowed || typed_result.is_ok() != allowed) return 23;
            pqxx::read_transaction observer(other);
            // Exact NUMERIC(28,8) returns scale even for these small integers.
            if (observer.exec("SELECT quantity FROM trading.positions WHERE date='2026-09-21'")[0][0].as<double>() !=
                (allowed ? 99.0 : 5.0)) return 24;
            db.abandon_live_publication();
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        std::vector<Position> rows{Position("ES",Quantity(12),Price(100),Decimal(0),Decimal(0),date)};
        if (db.store_positions(rows,engine,"TREND","BOOK","trading.positions").is_error()) return 5;
        rows[0].quantity = Quantity(99); // the publication must retain the captured 12
        if (mode != "incomplete" && db.store_risk_limits(engine,"BOOK",{{"max_gross_leverage",4.0}}).is_error()) return 6;
        const std::string column = (mode.find("rollback") == 0 ||
            case_mode == "required_callback_failure") ? "does_not_exist" : "total_pnl";
        if (db.store_live_results_complete(engine,date,
                {{column, mode == "publish_inspection_fix2_resource_failure" ? 999 : 123}},
                {}, {}, "BOOK").is_error()) return 7;
        nlohmann::json metadata_config = nlohmann::json::object();
        if (mode.find("inspection") != std::string::npos || required ||
            case_mode == "legacy_attachment") {
            metadata_config = {{"total_capital", 500000.0}, {"reserve_capital", 50000.0},
                               {"use_optimization", true}, {"use_risk_management", true}};
        }
        if (mode == "publish_inspection_fix2_resource_failure")
            metadata_config["total_capital"] = 999999.0;
        if (!inspection_capture.is_null()) metadata_config["config_inspection"] = inspection_capture;
        nlohmann::json metadata_allocations = {{"TREND", 1.0}};
        if (mode == "publish_inspection_fix2_integer_integer" ||
            mode == "publish_inspection_fix2_float_integer")
            metadata_allocations["TREND"] = 1;
        if (mode == "publish_inspection_fix2_zero_mixed")
            metadata_allocations["TREND"] = -0.0;
        if (mode == "publish_inspection_fix2_negative_mixed")
            metadata_allocations["TREND"] = -1.0;
        if (mode == "publish_inspection_fix2_signed_unsigned" ||
            mode == "publish_inspection_fix2_negative_unsigned_mismatch")
            metadata_allocations["TREND"] = std::uint64_t{1};
        if (mode == "publish_inspection_fix2_large_equal_integer" ||
            mode == "publish_inspection_fix2_large_unequal_mixed_inverse")
            metadata_allocations["TREND"] = std::uint64_t{9007199254740993ULL};
        if (mode == "publish_inspection_fix2_large_unequal_mixed")
            metadata_allocations["TREND"] = 9007199254740992.0;
        if (mode == "publish_inspection_fix1_allocation_changed")
            metadata_allocations = {{"TREND", 0.5}};
        if (mode == "publish_inspection_fix1_missing_selected")
            metadata_allocations = {{"TREND", 1.0}, {"OTHER", 0.5}};
        if (mode == "publish_inspection_fix1_extra_selected")
            metadata_allocations = nlohmann::json::object();
        if (mode == "inspection_mismatch") {
            if (db.store_live_run_metadata(date,engine,"SECOND",metadata_allocations,metadata_config,{}).is_ok())
                return 26;
        } else {
            auto stored = db.store_live_run_metadata(date,engine,"BOOK",metadata_allocations,
                                                      metadata_config,{});
            if (stored.is_error() != (case_mode == "required_missing_capture")) return 14;
        }
        std::optional<ConsumptionProjection> capacity_candidate;
        if ((required && case_mode.find("required_missing_attachment") != 0 &&
             case_mode != "required_missing_capture") || case_mode == "legacy_attachment") {
            auto projection = mode.find("publish_required_capacity") == 0 ?
                synthetic_final_projection(controlled,false,1000,
                    capacity_early ? &synthetic : nullptr) :
                ((mode.find("publish_required_complete") == 0 ||
                  mode.find("publish_required_copy_isolation") == 0 ||
                  mode.find("publish_required_moved_to") == 0 ||
                  mode.find("publish_required_after_success") == 0 ||
                  mode.find("publish_required_bad_capture_complete") == 0 ||
                  mode.find("publish_required_early_unavailable_complete") == 0) ?
                synthetic_final_projection(controlled,false) :
                (mode.find("publish_required_partial") == 0 ?
                 synthetic_final_projection(controlled,true) :
                 ConsumptionProjection::unavailable(
                     ConsumptionUnavailableReason::InstrumentationMissing)));
            if (case_mode == "publish_required_capacity_compact") {
                std::size_t low = 1000, high = 1170;
                while (low + 1 < high) {
                    const auto middle = low + (high - low) / 2;
                    auto attempted = synthetic_final_projection(controlled,false,middle);
                    if (attempted.document().at("status") == "complete") {
                        projection = std::move(attempted);
                        low = middle;
                    } else {
                        high = middle;
                    }
                }
                std::cout << "TYPED_CHARGES=" << low << '\n';
            }
            if (mode.find("publish_required_capacity") == 0)
                std::cout << "TYPED_CONSUMPTION_BYTES=" <<
                    projection.document().dump().size() << '\n';
            if (capacity_early) {
                std::cout << "TYPED_CONSUMPTION_STATUS=" <<
                    projection.document().at("status").get<std::string>() << '\n';
                std::cout << "TYPED_CONSUMPTION_NODES=" <<
                    projection.document().at("nodes").size() << '\n';
            }
            if (mode.find("publish_required_capacity") == 0)
                capacity_candidate.emplace(projection);
            if (case_mode == "required_wrong_token" || case_mode == "required_moved_from" ||
                case_mode == "legacy_attachment") {
                if (case_mode == "required_moved_from") {
                    auto moved_to = std::move(projection);
                    if (db.attach_live_consumption(evidence_token, projection).is_ok()) return 36;
                    if (db.attach_live_consumption(evidence_token, moved_to).is_ok()) return 37;
                } else {
                    const auto& refused_token = case_mode == "required_wrong_token" ?
                        PublicationEvidenceToken{} : evidence_token;
                    if (db.attach_live_consumption(refused_token, projection).is_ok()) return 36;
                    if (db.attach_live_consumption(evidence_token, projection).is_ok()) return 37;
                }
            } else {
                if (mode.find("publish_required_moved_to") == 0) {
                    auto moved_to = std::move(projection);
                    if (db.attach_live_consumption(evidence_token, moved_to).is_error()) return 32;
                    moved_to = ConsumptionProjection::unavailable(
                        ConsumptionUnavailableReason::InstrumentationMissing);
                } else {
                    if (db.attach_live_consumption(evidence_token, projection).is_error()) return 32;
                    if (mode.find("publish_required_copy_isolation") == 0)
                        projection = ConsumptionProjection::unavailable(
                            ConsumptionUnavailableReason::InstrumentationMissing);
                }
                if (case_mode == "required_in_progress")
                    db.arm_in_progress_attach(evidence_token, projection);
                if (case_mode == "required_duplicate" &&
                    db.attach_live_consumption(evidence_token, projection).is_ok()) return 38;
            }
        }
        if (mode == "inspection_repeat" &&
            db.store_live_run_metadata(date,engine,"BOOK",{{"TREND",1.0}},metadata_config,{}).is_ok())
            return 27;
        if (db.store_trading_equity_curve(engine,date,500000,"BOOK").is_error()) return 15;
        if (db.seed_qt_positions_from_system(engine,"TREND","BOOK","2026-09-22").is_error()) return 16;
        if (db.store_live_run_inputs(engine,"BOOK",date,
            {{"trade_ngin_sha","local-test"},{"config_snapshot",snapshot.value()},
             {"universe",{"ES"}},{"data_window",{}},{"engine_flags",{}}}).is_error()) return 17;
        pqxx::connection other(dsn);
        {
            pqxx::work observer(other);
            if (mode != "publish_concurrent" && mode != "missing_member_stale" &&
                mode.find("publish_inspection") != 0 &&
                !capacity_early &&
                !prior_refusal &&
                case_mode != "required_in_progress" &&
                case_mode != "required_callback_failure" &&
                case_mode != "required_callback_resource_failure" &&
                mode != "rollback_inspection" && mode != "retire_inspection" &&
                observer.exec("SELECT count(*) FROM trading.positions")[0][0].as<int>() != 0) return 8;
            if (mode.find("retire") == 0) observer.exec("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'");
            observer.commit();
        }
        if (mode == "publish_concurrent") {
            std::cout << "RUNTIME_READY" << std::endl;
            std::string command;
            if (!std::getline(std::cin,command) || command != "publish") return 22;
        }
        const bool resource_mode = mode == "publish_inspection_fix2_resource_failure" ||
            case_mode == "required_callback_resource_failure";
        if (resource_mode) {
            projector_allocation_failed = false;
            fail_projector_allocation = true;
        }
        auto result = db.publish_live_publication();
        fail_projector_allocation = false;
        if (case_mode == "required_in_progress") {
            std::cout << "LATE_ATTACH_ATTEMPTED=" << db.attempted_late_attach() << '\n';
            std::cout << "LATE_ATTACH_REFUSED=" << db.refused_late_attach() << '\n';
            std::cout << "PUBLICATION_COMMITTED=" << result.is_ok() << '\n';
            if (!db.attempted_late_attach() || !db.refused_late_attach() ||
                result.is_ok()) return 46;
            std::cout << "IN_PROGRESS_REFUSAL_ROLLED_BACK=1\n";
            std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
            return 0;
        }
        if (resource_mode) {
            std::cout << "PROJECTOR_ALLOCATION_FAILED=" <<
                (projector_allocation_failed ? 1 : 0) << '\n';
            std::cout << "RESOURCE_PUBLICATION_STATUS=" <<
                (result.is_error() ? "aborted" : "committed") << '\n';
            return projector_allocation_failed ? 0 : 30;
        }
        if (case_mode == "required_callback_failure") {
            std::cout << "CALLBACK_PUBLICATION_STATUS=" <<
                (result.is_error() ? "aborted" : "committed") << '\n';
            return result.is_error() ? 0 : 49;
        }
        if (prior_refusal) {
            std::cout << "PRIOR_REFUSAL_STATUS=" <<
                (result.is_error() ? "aborted" : "committed") << '\n';
            return result.is_error() ? 0 : 50;
        }
        const bool expect_success = mode.find("publish") == 0 ||
            case_mode == "required_already_active";
        if (result.is_ok() != expect_success) return 9;
        if (mode.find("publish_required_after_success") == 0 &&
            db.attach_live_consumption(evidence_token,
                ConsumptionProjection::unavailable(
                    ConsumptionUnavailableReason::InstrumentationMissing)).is_ok()) return 47;
        pqxx::read_transaction observer(other);
        auto published = observer.exec("SELECT quantity FROM trading.positions WHERE portfolio_type='system' AND strategy_name='TREND'");
        if (expect_success) {
            if (published.size() != 1 || published[0][0].as<double>() != 12) return 10;
            const int result_count = observer.exec("SELECT count(*) FROM trading.live_results")[0][0].as<int>();
            if ((mode.find("publish_inspection") == 0 || capacity_early) ?
                result_count < 1 : result_count != 1) return 11;
        } else if (mode != "rollback_inspection" && mode != "retire_inspection" &&
            (!published.empty() ||
             observer.exec("SELECT count(*) FROM trading.risk_limits")[0][0].as<int>() != 0)) return 12;
        if (controlled && observer.exec("SELECT status FROM trading.runtime_attempts")[0][0].as<std::string>() !=
            (expect_success ? "applied" : "failed")) return 18;
        if (expect_success &&
            (mode.find("inspection") != std::string::npos || required) &&
            mode != "publish_inspection_legacy_writer") {
            const auto stored = observer.exec("SELECT portfolio_config::text FROM trading.live_run_metadata");
            if (stored.size() != 1) return 28;
            const auto metadata = nlohmann::json::parse(stored[0][0].as<std::string>());
            const auto& child = metadata.at("config_inspection");
            if (child.size() != 12 ||
                child.at("publication_schema_version") != (required ? 2 : 1) ||
                child.at("profile") != "live_portfolio_runner_futures" ||
                child.at("identity").at("capture_id") != child.at("identity").at("publication_id")) return 29;
            if (capacity_candidate) {
                auto candidate = child;
                if (capacity_early) {
                    candidate["status"] = inspection_capture.at("status");
                    candidate["reason"] = inspection_capture.at("reason");
                    candidate["supplied"] = inspection_capture.at("supplied");
                    candidate["selected_trend"] = inspection_capture.at("selected_trend");
                }
                candidate["consumption"] = capacity_candidate->document();
                const auto candidate_compact = candidate.dump();
                const auto candidate_jsonb_bytes = observer.exec(
                    "SELECT octet_length(($1::jsonb)::text)",
                    pqxx::params{candidate_compact})[0][0].as<long long>();
                candidate["consumption"] = ConsumptionProjection::unavailable(
                    ConsumptionUnavailableReason::CapacityExceeded).document();
                const auto fallback_compact = candidate.dump();
                const auto fallback_jsonb_bytes = observer.exec(
                    "SELECT octet_length(($1::jsonb)::text)",
                    pqxx::params{fallback_compact})[0][0].as<long long>();
                const auto committed_jsonb_bytes = observer.exec(
                    "SELECT octet_length((portfolio_config -> 'config_inspection')::text) "
                    "FROM trading.live_run_metadata")[0][0].as<long long>();
                std::cout << "CANDIDATE_COMPACT_BYTES=" << candidate_compact.size() << '\n';
                std::cout << "CANDIDATE_JSONB_BYTES=" << candidate_jsonb_bytes << '\n';
                std::cout << "FALLBACK_COMPACT_BYTES=" << fallback_compact.size() << '\n';
                std::cout << "FALLBACK_JSONB_BYTES=" << fallback_jsonb_bytes << '\n';
                std::cout << "COMMITTED_COMPACT_BYTES=" << child.dump().size() << '\n';
                std::cout << "COMMITTED_JSONB_BYTES=" << committed_jsonb_bytes << '\n';
                if (!capacity_early)
                    std::cout << "CONSUMPTION_CANDIDATE=" <<
                        capacity_candidate->document().dump() << '\n';
            }
            std::cout << "CONFIG_PUBLICATION=" << child.dump() << '\n';
        }
        std::cout << "RUNTIME_PUBLICATION_OK=" << mode << '\n';
        return 0;
    } catch (...) { std::cerr << "runtime probe failed\n"; return 13; }
}
