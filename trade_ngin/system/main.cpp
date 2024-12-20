#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <memory>
#include <chrono>
#include "instrument.hpp"
#include "contract.hpp"

// A simple DataClient that provides mock data.
// For a real implementation, integrate actual parquet reading or Databento API calls.
class LocalDataClient : public DataClient {
public:
    std::optional<DatasetRange> get_dataset_range(Dataset ds) override {
        // Return a fixed range. A real impl would query metadata
        DatasetRange r;
        r.start = std::chrono::system_clock::now() - std::chrono::hours(24*365);
        r.end   = std::chrono::system_clock::now();
        return r;
    }

    std::optional<DataFrame> get_contract_data(
        Dataset ds,
        const std::string& symbol_str,
        Agg schema,
        RollType roll,
        ContractType ct,
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) override {
        // Mock data frame
        // In real code, read parquet or CSV, fill DataFrame properly.
        // Since Contract expects data_.empty() == false to proceed, let's return a non-empty DataFrame.
        // We'll just override empty() to return false by making a subclass:
        class NonEmptyDF : public DataFrame {
            bool empty() const {return false;}
        };
        return NonEmptyDF();
    }

    std::optional<DataFrame> get_definitions(Dataset ds, const DataFrame&) override {
        // Similar to above, return a non-empty DataFrame.
        class NonEmptyDF : public DataFrame {
            bool empty() const {return false;}
        };
        return NonEmptyDF();
    }
};

static std::vector<std::unique_ptr<Instrument>> initialize_instruments_from_csv(const std::string& csv_path) {
    std::ifstream file(csv_path);
    if (!file.is_open()) {
        throw std::runtime_error("Unable to open " + csv_path);
    }

    // Expected CSV format: dataSymbol,dataSet,instrumentType,multiplier
    // Example:
    // ES,CME,FUTURE,50
    // NQ,CME,FUTURE,20
    // ... etc.

    std::vector<std::unique_ptr<Instrument>> instruments;
    std::string line;
    // Skip header if present
    if (std::getline(file, line)) {
        // optional: check if line is a header
        // assume first line is a header and ignore it
    }
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string dataSymbol, dataSetStr, instrTypeStr, multiplierStr;

        if (!std::getline(ss, dataSymbol, ',')) continue;
        if (!std::getline(ss, dataSetStr, ',')) continue;
        if (!std::getline(ss, instrTypeStr, ',')) continue;
        if (!std::getline(ss, multiplierStr, ',')) continue;

        double multiplier = std::stod(multiplierStr);

        // Convert dataSetStr to Dataset enum
        Dataset ds;
        {
            std::string lower = dataSetStr;
            for (auto& c: lower) c = ::toupper(c);
            if (lower == "CME") ds = Dataset::CME;
            else throw std::runtime_error("Unknown dataset: " + dataSetStr);
        }

        InstrumentType it = instrument_type_from_str(instrTypeStr);
        if (it == InstrumentType::FUTURE) {
            instruments.push_back(std::make_unique<Future>(dataSymbol, ds, multiplier));
        } else {
            throw std::runtime_error("Unsupported instrument type in CSV: " + instrTypeStr);
        }
    }

    return instruments;
}

int main() {
    try {
        LocalDataClient client;
        std::string csv_path = "data/contract.csv";
        auto instruments = initialize_instruments_from_csv(csv_path);

        for (auto& inst : instruments) {
            if (inst->get_asset() == Asset::FUT) {
                Future* fut = dynamic_cast<Future*>(inst.get());
                if (!fut) throw std::runtime_error("Instrument is FUT but not castable to Future");

                // Add front contract daily calendar data
                fut->add_data(client, Agg::DAILY, RollType::CALENDAR, ContractType::FRONT, Catalog::DATABENTO);

                // Print some price info
                const auto& prices = fut->price();
                std::cout << "Instrument: " << fut->symbol() << "\n";
                std::cout << "First few prices: ";
                for (size_t i = 0; i < std::min<size_t>(5, prices.size()); ++i) {
                    std::cout << prices[i] << " ";
                }
                std::cout << "\n";
            }
        }

        std::cout << "All instruments loaded and data fetched successfully.\n";
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
