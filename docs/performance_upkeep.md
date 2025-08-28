# Performance & Upkeep

This combines CI/CD pipeline, unit-testing requirements, and the daily cron job to operate the live pipeline.

## Quick checklist
- Install deps using scripts in `requirements/`
- Push changes; CI builds and tests automatically
- Review CI artifacts
- Enable daily cron to run `live_trend`

## Local prerequisites
- Ubuntu: `sudo bash requirements/install_ubuntu.sh`
- macOS: `bash requirements/install_macos.sh`
- Configure: `cp config_template.json config.json` and update credentials

Build & test:
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -- -j$(nproc)
ctest --output-on-failure
```

## Unit testing
- Framework: GoogleTest
- All tests in `tests/` must pass on PRs and `main`
- Run subset: `ctest -R trend_following`
- DB tests require reachable PostgreSQL 

## CI/CD
#See CI/CD README and current implementation under .github/workflows
Expected Stages:
1) Configure: CMake configure, optional lint
2) Build: library `trade_ngin` and executables `bt_trend`, `live_trend`
3) Test: `ctest --output-on-failure`, publish results
4) Artifacts: upload build outputs under `build/bin/`
5) Docker: build/push image on tags


## Daily cron (live pipeline)
We schedule `apps/strategies/live_trend` to run daily, produce positions/metrics, write logs, and send summary email.
See cron template setup under scripts. 

Cron (Example):
```cron
# 06:00 UTC daily
0 6 * * * /usr/local/bin/run_trade_ngin_live.sh >> /var/log/trade_ngin/cron.log 2>&1
```


## Expectations when done
- CI green on PRs and `main`
- Tests pass; results archived
- Daily cron runs successfully; logs and email sent
- Failures are visible via logs/artifacts

## Troubleshooting
- Cron silent: verify paths and permissions; check DB connectivity
- Hung tests: `ctest -R <name> -VV` for verbose output
