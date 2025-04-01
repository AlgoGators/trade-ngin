# Trade-Ngin Tools and Testing

This document provides an overview of the testing tools and utilities available in the Trade-Ngin project.

## Directory Structure

```
/
├── tools/                        # Utility and testing tools
│   ├── test_all_implementations.sh   # Comprehensive testing script
│   ├── generate_sample_data.py       # Generate sample backtest data
│   ├── visualize_results.sh          # Visualize backtest results
│   └── test_visualize_results.sh     # Test version of visualization script
│
├── IMPLEMENTATION_STATUS.md      # Current status of implemented features
├── README_DEPENDENCIES.md        # Dependency management information
├── README_TESTING.md             # Detailed testing instructions
└── README_TOOLS.md               # This file - overview of tools
```

## Available Tools

### Comprehensive Testing

The main testing tool is:

```bash
./tools/test_all_implementations.sh
```

This script runs a complete test suite including:
- Build system tests (with CMake)
- Logging system tests
- Configuration management tests
- Visualization tests
- Docker environment tests
- Documentation checks

Results are saved to a timestamped directory for review.

### Backtest Data Tools

Two tools are available for working with backtest data:

1. **Sample Data Generator**:
   ```bash
   ./tools/generate_sample_data.py --output-dir=<directory>
   ```
   Creates realistic sample backtest data for testing.

2. **Visualization Tool**:
   ```bash
   ./tools/visualize_results.sh <backtest_directory>
   ```
   Creates charts and visualizations from backtest data.

## Known Issues

The current testing system may encounter:

1. **Arrow Version Compatibility**
   - The project requires Arrow 6.0.0, but may find other versions (11.0.0, 19.0.1) on the system
   - Build may fail with error: "Could not find Arrow compatible with requested version 6.0.0"
   - Solutions:
     - Use Docker environment (correct Arrow version)
     - Update CMakeLists.txt to work with your installed Arrow version
     - Install Arrow 6.0.0

2. **CMake Cache Issues**
   - When switching between Docker and local builds, CMake cache may become corrupted
   - Solution: Always clean the build directory before building:
     ```bash
     rm -rf build && mkdir build && cd build && cmake ..
     ```

## Implementation Status

For a complete overview of the implementation status of all tasks, see `IMPLEMENTATION_STATUS.md`.

## Running Tests in Docker

If you prefer to use Docker to avoid compatibility issues:

```bash
# Build the Docker image
docker-compose build

# Run the test script in Docker
docker-compose run --rm trade-ngin ./tools/test_all_implementations.sh
```

## Further Reading

- **README_DEPENDENCIES.md**: Details on dependency management
- **README_TESTING.md**: Detailed testing instructions and troubleshooting
- **IMPLEMENTATION_STATUS.md**: Current status of all implemented features 