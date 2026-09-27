#pragma once
#include <memory>
#include <pqxx/pqxx>
#include <utility>

namespace trade_ngin {
// A typed database method either owns its small transaction or participates in
// the final publication transaction. Only the publisher commits the latter.
class PublicationTransaction {
    std::unique_ptr<pqxx::work> owned_;
    pqxx::work* transaction_;
public:
    PublicationTransaction(pqxx::connection& connection, pqxx::work* shared)
        : owned_(shared ? nullptr : std::make_unique<pqxx::work>(connection)),
          transaction_(shared ? shared : owned_.get()) {}
    template<class... Args> auto exec(Args&&... args) {
        return transaction_->exec(std::forward<Args>(args)...);
    }
    template<class Value> auto quote(const Value& value) { return transaction_->quote(value); }
    void commit() { if (owned_) owned_->commit(); }
    void abort() { transaction_->abort(); }
    operator pqxx::work&() { return *transaction_; }
};
}  // namespace trade_ngin
