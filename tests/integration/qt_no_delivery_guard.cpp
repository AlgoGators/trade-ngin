// Test-only ELF interposition: refuse before any credential/delivery method runs.
// This library is never linked into the evaluator or live runner.
#include "trade_ngin/core/email_sender.hpp"
#include <cstdlib>

extern "C" int qt_no_delivery_guard_loaded() { return 1; }

namespace trade_ngin {
Result<void> EmailSender::initialize() { std::_Exit(86); }
Result<void> EmailSender::send_email(const std::string&, const std::string&, bool,
                                   const std::vector<std::string>&) { std::_Exit(86); }
Result<void> EmailSender::deliver_email(const std::string&, const std::string&, bool,
                                      const std::vector<std::string>&) { std::_Exit(86); }
}  // namespace trade_ngin
