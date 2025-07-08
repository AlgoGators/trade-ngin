# Type Conversion Guide for Trade-ngin

This guide explains how to properly handle type conversions between `Decimal`, `double`, `Price`, and `Quantity` types in the Trade-ngin codebase.

## Type Definitions

```cpp
using Price = Decimal;      // Financial price values
using Quantity = Decimal;   // Order/position quantities  
class Decimal;              // Fixed-point decimal with 8 decimal places
```

## Converting TO Decimal

### From double
```cpp
double d = 123.45;
Decimal dec = Decimal(d);                    // Constructor
Decimal dec2 = Decimal::from_double(d);      // Static method
```

### From integer
```cpp
int i = 100;
Decimal dec = Decimal(i);                    // Constructor
```

### From string (if needed)
```cpp
std::string s = "123.45";
Decimal dec = Decimal(std::stod(s));         // Parse then construct
```

## Converting FROM Decimal

### To double
```cpp
Decimal dec(123.45);
double d1 = static_cast<double>(dec);        // Explicit cast
double d2 = dec.as_double();                 // Helper method
double d3 = dec.to_double();                 // Helper method
```

### To string
```cpp
Decimal dec(123.45);
std::string s1 = dec.to_string();            // Method
std::string s2 = std::to_string(dec);        // STL function (works!)
```

### For database storage
```cpp
// When storing in database (pqxx expects double)
txn.exec_params(
    "INSERT INTO table (price) VALUES ($1)",
    static_cast<double>(decimal_price)
);
```

### For metrics/logging
```cpp
// StateManager metrics expect double
ComponentInfo info{
    ComponentType::PORTFOLIO_MANAGER,
    ComponentState::RUNNING,
    "ID",
    "",
    timestamp,
    {{"capital", static_cast<double>(decimal_capital)}}  // Convert for metrics
};
```

## Configuration Pattern

For configuration structs, financial values should be Decimal but serialize as double:

```cpp
struct Config {
    Decimal capital{Decimal(1000000.0)};     // Store as Decimal
    
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["capital"] = static_cast<double>(capital);  // Serialize as double
        return j;
    }
    
    void from_json(const nlohmann::json& j) override {
        if (j.contains("capital")) {
            capital = Decimal(j.at("capital").get<double>());  // Parse as double, store as Decimal
        }
    }
};
```

## String Concatenation Pattern

```cpp
// OLD (won't compile)
"Capital: " + std::to_string(decimal_capital)

// NEW (works!)
"Capital: " + decimal_capital.to_string()
// OR
"Capital: " + std::to_string(decimal_capital)  // Now supported!
```

## Mathematical Operations

```cpp
Decimal price(100.0);
Decimal quantity(10.0);

// All operations return Decimal
Decimal total = price * quantity;
Decimal average = total / Decimal(2.0);
bool expensive = price > Decimal(50.0);

// When interfacing with legacy double-based code
double legacy_result = some_legacy_function(
    static_cast<double>(price),
    static_cast<double>(quantity)
);
Decimal converted_back = Decimal(legacy_result);
```

## Common Patterns by Module

### Database Operations
```cpp
// Always convert Decimal to double for database
txn.exec_params(query, 
    symbol,
    static_cast<double>(position.quantity),
    static_cast<double>(position.average_price)
);
```

### Slippage Models
```cpp
// Slippage models work with double internally
double slipped_price = slippage_model->calculate_slippage(
    static_cast<double>(original_price),
    static_cast<double>(trade_quantity),
    side
);
Decimal final_price = Decimal(slipped_price);
```

### Risk Calculations
```cpp
// Risk models may work with double, convert at boundaries
double risk_val = risk_manager->calculate_var(
    positions_as_doubles  // Convert collection to doubles
);
```

### Logging and Metrics
```cpp
// For component registration
{{"total_capital", config.total_capital.as_double()}}

// For log messages
INFO("Portfolio value: " + portfolio_value.to_string());
```

## Migration Strategy

1. **Configuration structs**: Update to use Decimal for financial fields
2. **Core types**: Already updated (Price, Quantity = Decimal)
3. **Database operations**: Add explicit conversions to double
4. **Computational models**: Keep as double, convert at boundaries
5. **Logging/metrics**: Convert to double for external systems

## Compilation Error Fixes

### "Cannot convert Decimal to double"
```cpp
// Change this:
some_function(decimal_value);

// To this:
some_function(static_cast<double>(decimal_value));
```

### "No matching function for std::to_string"
```cpp
// Change this:
std::to_string(decimal_value);  // Should work now!

// Or use this:
decimal_value.to_string();
```

### "Cannot initialize double with Decimal"
```cpp
// Change this:
double d = decimal_value;

// To this:
double d = decimal_value.as_double();
```

This approach maintains financial precision where it matters while allowing smooth integration with existing double-based systems.