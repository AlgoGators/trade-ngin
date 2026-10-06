#include "trade_ngin/apps/qt_book_tail.hpp"
#include "trade_ngin/apps/qt_desk_cycle.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
namespace trade_ngin {
Result<nlohmann::json> run_book_tail(const QtFuturesBookTailInputs& input) {
    return produce_qt_futures_accounting(input.decision,input.selection,input.accounting_input);
}
Result<nlohmann::json> run_book_tail(const QtEquityBookTailInputs& input) {
    return recompute_qt_equity_accounting_output(input.decision,input.selection,
        input.accounting_input,input.producer_authority);
}
}
