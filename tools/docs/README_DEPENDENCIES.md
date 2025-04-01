# Trade-Ngin Dependency Management

This document explains how dependencies are managed in the Trade-Ngin project.

## Overview

Trade-Ngin uses a component-based architecture with carefully managed dependencies to improve maintainability and testability. The project distinguishes between:

1. **Internal Dependencies**: Components within the Trade-Ngin codebase
2. **External Dependencies**: Third-party libraries used by Trade-Ngin

## Internal Dependencies

The codebase is organized into logical components, each with well-defined dependencies:

- **Core**: Basic types, logging, error handling, and utilities
- **Data**: Database access and market data handling
- **Instruments**: Financial instrument definitions and registry
- **Order**: Order management and processing
- **Risk**: Risk management and position sizing
- **Optimization**: Portfolio optimization algorithms
- **Execution**: Order execution and transaction cost analysis
- **Portfolio**: Portfolio management and tracking
- **Strategy**: Trading strategy implementations
- **Backtest**: Backtesting engine and analysis

### Component Dependency Graph

The dependency graph is visualized using GraphViz. To generate the dependency graph:

```bash
./build.sh --graph
```

This will generate a PNG file in the `build/dependency_graphs` directory.

### Component Guidelines

1. **Minimize Dependencies**: Each component should have as few dependencies as possible.
2. **Dependency Direction**: Lower-level components should not depend on higher-level components.
3. **Interface Abstraction**: Use interfaces to decouple components.
4. **Forward Declarations**: Use forward declarations to reduce header dependencies.

## External Dependencies

Trade-Ngin relies on several external libraries:

1. **nlohmann/json**: JSON serialization/deserialization
2. **Apache Arrow**: Efficient columnar data processing
3. **libpqxx**: PostgreSQL C++ client library
4. **GoogleTest**: Testing framework

### Managing External Dependencies

We provide several mechanisms for managing external dependencies:

1. **Dependency Wrappers**: External libraries are wrapped with our own interfaces to isolate them (see `JsonWrapper` and `DatabaseInterface`).
2. **Version Pinning**: Specific versions of dependencies can be enforced via CMake.
3. **Vendoring**: Dependencies can be vendored (included directly in the codebase) using the `USE_VENDORED_LIBS` CMake option.
4. **Docker Environment**: A containerized development environment is provided with all dependencies pre-installed.

### Using the Docker Environment

The easiest way to manage dependencies is to use the provided Docker environment:

```bash
# Start the Docker environment
./docker-run.sh up

# Open a shell in the Docker container
./docker-run.sh shell

# Build the project
./build.sh
```

### Manual Dependency Installation

If you prefer not to use Docker, you can install dependencies manually:

#### Ubuntu/Debian

```bash
# Install system dependencies
apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    ninja-build \
    pkg-config \
    libpq-dev \
    graphviz

# Install Arrow
apt-get install -y libarrow-dev libarrow-dataset-dev

# Install nlohmann_json
apt-get install -y nlohmann-json3-dev

# Install libpqxx
apt-get install -y libpqxx-dev

# Install GoogleTest
apt-get install -y libgtest-dev
```

#### macOS

```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install cmake ninja pkg-config postgresql arrow nlohmann-json libpqxx googletest graphviz
```

## Dependency Isolation

To maintain good separation of concerns and to make it easier to replace or update external libraries, we use several isolation techniques:

### Wrapper Classes

External dependencies are wrapped with our own interfaces. For example:

- `JsonWrapper`: Wraps nlohmann/json
- `DatabaseInterface`: Provides an abstraction over the database library

### Dependency Injection

Dependencies are injected rather than created directly, making components testable:

```cpp
// Good: Dependencies are injected
TrendFollowingStrategy(
    std::string id,
    StrategyConfig config,
    TrendFollowingConfig trend_config,
    std::shared_ptr<DatabaseInterface> db,
    InstrumentRegistry* registry = nullptr);

// Bad: Dependency created directly inside
TrendFollowingStrategy(
    std::string id,
    StrategyConfig config,
    TrendFollowingConfig trend_config) {
    // DON'T do this: creates hard dependency
    db_ = std::make_shared<PostgresDatabase>();
}
```

## Building with Different Dependency Configurations

The build system supports different dependency configurations:

```bash
# Build with pinned dependency versions
./build.sh --pin-versions

# Build with vendored libraries
./build.sh --use-vendor
```

## CMake Integration

External projects that use Trade-Ngin can integrate it via CMake:

```cmake
find_package(trade_ngin REQUIRED)
target_link_libraries(your_app PRIVATE trade_ngin::trade_ngin)

# Or link to specific components only
target_link_libraries(your_app PRIVATE 
    trade_ngin::trade_ngin_core
    trade_ngin::trade_ngin_strategy
)
```

## Best Practices

1. **Always use wrapper interfaces** for external libraries.
2. **Use dependency injection** to make components testable.
3. **Keep the dependency graph acyclic** to avoid circular dependencies.
4. **Be explicit about dependencies** in class documentation.
5. **Use forward declarations** when possible to reduce compile-time dependencies.
6. **Run with --graph** regularly to visualize dependencies and catch unwanted dependencies early. 