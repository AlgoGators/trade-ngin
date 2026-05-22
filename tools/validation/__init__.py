"""BPGV strategy validation harness (Tier-3 Package F).

Modules:
    dsr             — Deflated Sharpe Ratio (Bailey-Lopez de Prado 2014).
    harvey_liu      — Multiple-testing haircut (HLZ 2016).
    politis_romano  — Stationary block bootstrap + Ledoit-Wolf 2008
                      Sharpe test.
    cpcv            — Combinatorial Purged K-Fold fold generator
                      (Lopez de Prado 2018).
    benchmarks      — SPY / 60-40 / EW SPY-TLT-GLD return series loader.
    runner          — Top-level orchestrator; emits a Markdown report.

See `runner.py --help` for usage.
"""
