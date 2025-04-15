#include "trade_ngin/portfolio/portfolio_manager.hpp"

namespace trade_ngin {

PortfolioManager::PortfolioManager(PortfolioConfig config, std::string id)
    : config_(config), id_(std::move(id)), instance_id_(id_)
{
    // Minimal stub: do nothing else
}

// All other methods can be left unimplemented for the minimal test

} // namespace trade_ngin 