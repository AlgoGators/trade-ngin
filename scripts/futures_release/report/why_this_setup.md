## Why this setup

The full reasoning, with the evidence for each point, goes in the repository with the knowledge documents, a separate pull
request. In short:

- **Sample covariance and the fraction step.** The set chosen from Carver's joint search of the optimiser covariance, the
  buffer step and the limits over 21 windows. The carver covariance and the edge step lifted only the 2y and lowered
  the 5y and 16y, which is a fit to one period.
- **Limits 1.75 / 4.75 / 3.00 / 7.25 / 5.50.** Each is its reading's 99th percentile after the warm-up on the uncapped
  16y, to the nearest 0.25; net is 5.59 rounded down. 5.75 was measured too and scored the same, 0.275 against 0.274.
- **The 756-bar covariance window.** Every shorter lookback (160, 252, 504) lowered the results and failed on all three
  bases.
- **Carry off.** Carry on lowered the after-cost Sharpe in every pair, under both overlay covariances.
- **Carver's overlay covariance for the risk overlay** (his sigma with weekly EWMA correlations).
- **Micro gold (MGC) and micro silver (SIL) from their listing dates** (2010-10-04 and 2013-06-17). At 500k one
  full-size gold or silver contract is far above its risk target; in the 2025-26 year a single full silver contract
  made 181,138 of the book's 211,714 while the strategy wanted about a tenth of it. The micros let the size follow the
  target. Before its listing date each micro trades its full-size contract.
- **The equity slow rule.** An equity index short stands only when both slow speeds agree.
- **The speed-limit removals.** Each contract drops the trend speeds whose cost is over 0.15 Sharpe a year (Carver's
  limit).
- **Tried and rejected:** the carver covariance with the edge step (2y only); net 5.75 (a coin flip); the equity cut with
  the high-volatility switch (it passed on the base limits and failed on its own); the high-volatility switch alone; the
  shorter covariance windows; micro crude MCL (it did not raise the paired Sharpe); carry on.
