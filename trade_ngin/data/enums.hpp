#pragma once
#include <string>
#include <stdexcept>
#include <ostream>

// Use enum classes for strong typing
enum class Catalog {
    DATABENTO,
    NORGATE
};

inline std::string to_string(Catalog c) {
    switch(c) {
    case Catalog::DATABENTO: return "data/catalog/databento/";
    case Catalog::NORGATE:   return "data/catalog/norgate/";
    }
    throw std::runtime_error("Unknown Catalog");
}

enum class URI {
    DATABENTO,
    NORGATE
};

inline std::string to_string(URI u) {
    switch(u) {
    case URI::DATABENTO: return "s3://algogatorsbucket/catalog/databento/";
    case URI::NORGATE:   return "s3://algogatorsbucket/catalog/norgate/";
    }
    throw std::runtime_error("Unknown URI");
}

enum class Asset {
    FUT,
    OPT,
    EQ
};

inline std::string to_string(Asset a) {
    switch(a) {
    case Asset::FUT: return "FUT";
    case Asset::OPT: return "OPT";
    case Asset::EQ:  return "EQ";
    }
    throw std::runtime_error("Unknown Asset");
}

enum class Dataset {
    CME
    // Add more datasets here
};

inline std::string to_string(Dataset d) {
    // Here we only have one
    switch(d) {
    case Dataset::CME: return "GLBX.MDP3";
    }
    throw std::runtime_error("Unknown Dataset");
}

inline Dataset dataset_from_string(const std::string& val) {
    // Lowercase compare or direct map
    if (val == "CME" || val == "GLBX.MDP3") {
        return Dataset::CME;
    }
    throw std::runtime_error("Invalid dataset string: " + val);
}

enum class Agg {
    DAILY,
    HOURLY,
    MINUTE,
    SECOND
};

inline std::string to_string(Agg a) {
    switch(a) {
    case Agg::DAILY:  return "ohlcv-1d";
    case Agg::HOURLY: return "ohlcv-1h";
    case Agg::MINUTE: return "ohlcv-1m";
    case Agg::SECOND: return "ohlcv-1s";
    }
    throw std::runtime_error("Unknown Agg");
}

enum class RollType {
    CALENDAR,
    OPEN_INTEREST,
    VOLUME
};

inline std::string to_string(RollType r) {
    switch(r) {
    case RollType::CALENDAR:      return "c";
    case RollType::OPEN_INTEREST: return "n";
    case RollType::VOLUME:        return "v";
    }
    throw std::runtime_error("Unknown RollType");
}

enum class ContractType {
    FRONT,
    BACK,
    THIRD,
    FOURTH,
    FIFTH
};

inline std::string to_string(ContractType c) {
    switch(c) {
    case ContractType::FRONT:  return "0";
    case ContractType::BACK:   return "1";
    case ContractType::THIRD:  return "2";
    case ContractType::FOURTH: return "3";
    case ContractType::FIFTH:  return "4";
    }
    throw std::runtime_error("Unknown ContractType");
}
