# COMPONENT TESTING & ANALYSIS

This document explains how to build, run, analyze, and extend the minimal component test environments for the trade-ngin system. Each component (portfolio, market data, signal, bt_trend, etc.) is fully containerized and ready for memory, threading, and performance analysis.

---

## 1. Build & Run Each Component Test

Each component has its own Docker Compose file. To build and run a test:

```sh
# Example for portfolio
cd /path/to/repo

docker-compose -f docker-compose.portfolio.yml build

docker-compose -f docker-compose.portfolio.yml up --abort-on-container-exit
```

Replace `portfolio` with `market_data`, `signal`, or `bt_trend` for other components.

---

## 2. Running Analysis Tools

Each component test directory contains scripts to run the test binary under:
- **Valgrind** (memory analysis)
- **ThreadSanitizer (TSAN)** (thread safety analysis)
- **perf** (CPU profiling, if available)

### Example (inside the container or mapped volume):

```sh
cd /app/build/bin

# Run Valgrind
./run_valgrind.sh
# Output: valgrind.log

# Run ThreadSanitizer
./run_tsan.sh
# Output: tsan.log

# Run perf (if available)
./run_perf.sh
# Output: perf_report.txt
```

---

## 3. Where to Find Logs

- All logs are written to the current directory (e.g., `/app/build/bin` inside the container).
- Look for `valgrind.log`, `tsan.log`, and `perf_report.txt` after running the scripts.
- The main test output is also logged (e.g., `portfolio_minimal_test.log`).

---

## 4. How to Extend a Stub/Test

- **Stubs**: Each component has a stub header (e.g., `portfolio_manager_stub.hpp`) that matches the real API. Add or update methods as the real code evolves.
- **Tests**: Each minimal test launches threads and exercises the real API. Add more realistic scenarios, memory allocations, or threading patterns as needed.
- **Analysis**: Use the provided scripts to check for memory leaks, race conditions, or performance bottlenecks as you extend the code.

---

## 5. Adding New Components

1. Copy an existing minimal test directory as a template.
2. Create a stub header for the new component, matching its real API.
3. Write a minimal test that launches threads and exercises the API.
4. Add `run_valgrind.sh`, `run_tsan.sh`, and `run_perf.sh` scripts.
5. Add a Dockerfile and Compose file for the new component.
6. Update this doc if needed.

---

## 6. Support & Troubleshooting

- If a tool is not installed in the container, update the Dockerfile to add it (e.g., `apt-get install linux-perf`).
- If you see errors in the logs, check the stub/test for missing or incorrect API usage.
- For advanced analysis, you can mount additional volumes or run the containers with extra capabilities as needed.

---

**This system is ready for new developers to run, analyze, and optimize each component in isolation.** 