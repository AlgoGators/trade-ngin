# The frozen run config

Every seeding run writes its run config from these files (`lib.zsh`, `write_config`).

| File | What it is |
|---|---|
| `portfolio.json`, `risk.json` | the chosen setup, unchanged |
| `defaults.json` | the chosen setup, less the database block (written at run time from the target) and less `backtest.lookback_years` and `backtest.frozen_end_date` (set per run) |
| `email.json` | a dead address on a port nothing listens on: no seeding run passes `--send-email`, and a send could not leave the machine |

`SHA256SUMS` is checked before every run; a changed file stops the run. The release's own `config_template` should equal
these files apart from the database and window keys; steps 09 and 11 print the difference on `prepare`.
