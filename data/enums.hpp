#pragma once

enum class Dataset {
    CME,
    ICE,
    CRYPTO
};

enum class Asset {
    FUT,
    OPT,
    STOCK,
    CRYPTO
};

enum class Agg {
    TICK,
    MINUTE,
    HOUR,
    DAILY
};

enum class RollType {
    CALENDAR,
    VOLUME,
    OPEN_INTEREST
};

enum class ContractType {
    FRONT,
    BACK,
    CALENDAR_SPREAD
}; 