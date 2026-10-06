#pragma once
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/qt_desk_processor.hpp"
#include <memory>
#include <string>
#include <sys/types.h>

namespace trade_ngin::qt_desk_cli {
// Pre-DB path admission owns only an open existing parent directory descriptor.
// It creates nothing. The future writer must revalidate inode/path identity and
// exclusively publish a bounded regular file; no overwriting or symlink traversal.
class QtDeskCsvOutput {
    int parent_fd_;
    std::string parent_path_, name_;
    dev_t device_;
    ino_t inode_;
    QtDeskCsvOutput(int fd,std::string parent,std::string name,dev_t device,ino_t inode);
public:
    ~QtDeskCsvOutput();
    QtDeskCsvOutput(const QtDeskCsvOutput&)=delete;
    QtDeskCsvOutput& operator=(const QtDeskCsvOutput&)=delete;
    static Result<std::unique_ptr<QtDeskCsvOutput>> admit(const std::string& absolute_path);
    friend Result<std::string> export_committed_qt_desk_csv(pqxx::connection&,
        const QtDeskProcessedReceipt&,const std::string&,const std::string&,const QtDeskCsvOutput&);
};
// Must be called AFTER the actual accounting processor has committed this exact
// receipt. Re-admit actual durable SQL proof, complete original owner accounting,
// explicit price/instrument scope on this same connection. Returns the file SHA.
// No caller-supplied authority/ready flag; failure does not roll back the receipt.
Result<std::string> export_committed_qt_desk_csv(pqxx::connection&,
    const QtDeskProcessedReceipt&,const std::string& input_id,
    const std::string& source_day,const QtDeskCsvOutput&);
}
