#include "trade_ngin/core/live_config_override.hpp"
#include "trade_ngin/core/logger.hpp"
#include <array>
#include <iostream>
#include <string>
namespace {
int refuse(const std::string& code) {
    std::cout << nlohmann::json{{"schema","live-config-validation/v1"},
        {"error",{{"code",code},{"retryable",false}}}}.dump() << '\n';
    return 2;
}
}
int main(int argc,char** argv) {
    trade_ngin::LoggerConfig logging; logging.destination=trade_ngin::LogDestination::NONE;
    trade_ngin::Logger::instance().initialize(logging);
    try {
        if(argc!=1) {
            // Paths exist only on the operator export command, never in stdin validation.
            if(argc!=6 || std::string(argv[1])!="--export-base" ||
                std::string(argv[2])!="--config-root" || std::string(argv[4])!="--portfolio")
                return refuse("live_config_invalid_arguments");
            auto config=trade_ngin::ConfigLoader::load_trading(argv[3],argv[5]);
            if(config.is_error()) return refuse(config.error()->what());
            auto snapshot=trade_ngin::build_runtime_trading_snapshot(config.value());
            if(snapshot.is_error()) return refuse(snapshot.error()->what());
            auto valid=trade_ngin::parse_runtime_trading_snapshot(snapshot.value());
            if(valid.is_error()) return refuse(valid.error()->what());
            std::cout << snapshot.value().dump() << '\n'; return 0;
        }
        std::string bytes; std::array<char,8192> buffer{};
        while(std::cin) {
            std::cin.read(buffer.data(),buffer.size());
            const auto count=static_cast<size_t>(std::cin.gcount());
            if(count>trade_ngin::live_config_max_request_bytes-bytes.size()) return refuse("live_config_request_size");
            bytes.append(buffer.data(),count);
        }
        if(std::cin.bad()) return refuse("live_config_input_failed");
        auto request=trade_ngin::parse_live_config_request(bytes);
        if(request.is_error()) return refuse(request.error()->what());
        const bool baseline=request.value().is_object() && request.value().contains("schema") &&
            request.value().at("schema")=="live-config-baseline-validation/v1";
        auto result=baseline ? trade_ngin::validate_live_config_baseline_request(request.value())
                             : trade_ngin::validate_live_config_request(request.value());
        if(result.is_error()) return refuse(result.error()->what());
        std::cout << result.value().dump() << '\n'; return 0;
    } catch(const std::exception&) { return refuse("live_config_invalid_request"); }
}
