// Actual PostgreSQL desk entrypoint, restricted to the owned disposable socket.
#include "trade_ngin/data/qt_desk_processor.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    const char* raw = std::getenv("ALGOLENS_TEST_DB");
    if (!raw || (argc != 4 && argc != 5)) return 2;
    const std::string dsn(raw);
    if (dsn.rfind("host=/tmp/algolens-repair-pg-",0) != 0 ||
        dsn.find(" dbname=algolens_test_") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos)
        return 2;
    try {
        pqxx::connection connection(dsn);
        if (std::string(argv[1]) == "--report") {
            auto evidence=trade_ngin::load_qt_desk_report_evidence(connection,argv[2],argv[3]);
            if(evidence.is_error()) { std::cout << "REPORT_PROOF=0\n";return 12; }
            std::cout << "REPORT_PROOF=1\nWORKFLOW_REQUIRED="
                      << evidence.value().at("workflow_required").get<bool>() << '\n';
            return 0;
        }
        const bool produce=argc==5&&std::string(argv[1])=="--accounting";
        if(argc==5&&!produce)return 2;
        auto receipt = produce
            ? trade_ngin::process_qt_desk_accounting_decision(connection,argv[2],argv[3],argv[4])
            : trade_ngin::process_qt_desk_decision(connection,argv[1],argv[2],argv[3]);
        if (receipt.is_error()) {
            std::cout << "DESK_REFUSED=1\n";
            std::cout << "DESK_REASON=" << receipt.error()->what() << '\n';
            return 10;
        }
        std::cout << "DESK_PROCESSED=1\nREPLAYED=" << receipt.value().replayed << '\n';
        return 0;
    } catch (const std::exception&) {
        std::cout << "DESK_REFUSED=1\n";
        return 11;
    }
}
