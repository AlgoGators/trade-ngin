# Continue here — state of the AlgoGators issues assigned to SivanRP

Cold-start context. Read this first, then `STEP0_FINDINGS.md` (the detail) and
`STEP0_SUMMARY.md` (the plain-English version) in this directory.

Last updated: 2026-10-10.

---

## The five issues assigned to me

| issue | what it is | state |
|---|---|---|
| **#103** Regime detection | The lane I own. Macro + market regime pipelines on `origin/regime_detection_updated`. | Step 0 partly done — see below |
| **#104** ML models | Companion document to #103. | Not started. Blocked by #105 (assigned to `gabbypolito`) |
| **#107** ML models, regime-linked | Part of #104. | Not started. Blocked by #105 |
| **#102** QT relative value | Backend contract + AlgoLens. | Not started |
| **#123** Unit testing rotation | Assigned to the whole team; a weekly wheel spin picks one person. | **Not my turn.** Do not work on this unless the rotation says so — the issue has zero comments, so no one has been picked yet |

---

## #103: what is done

Branch: `regime/step0-census` (two commits, branched off `main` at `08b15c00`).

**Step 0(a) — build.** `origin/regime_detection_updated` at `1efd363b` compiles
clean: `[163/163]`, 0 errors.

**Step 0(b) — tests.** `trade_ngin_tests` runs **430/430 pass**, 0 fail, 0 skip —
exactly the count the issue predicts.

**Step 0(c) — conflict preview.** RDU merges into `main` and into the stage-3 tip
(PR #99) with no conflicts. Only PR #41 conflicts, in `CMakeLists.txt` and
`tests/CMakeLists.txt`.

**§8(i) — census.** Answered. The headline finding: **merge, do not rebase.**
Rebasing conflicts in `CMakeLists.txt`, and neither mechanical resolution is safe —
taking RDU's side silently reverts 47 lines of `main`'s build config (benchmarks,
coverage, RPATH, five sources) while still compiling, and a union resolution emits
`add_subdirectory(apps)` twice and fails to configure. The merge builds 287/287 and
passes the combined 1,980-test suite with zero failures.

Also found: the issue's list of files RDU touches outside `src/regime_detection` is
incomplete — it omits a **new shared library** `src/statistics/clustering/gmm.cpp`
(209 lines), which collides with #107 Part A, also adding a GMM to `main`.

And: the shared statistics edits (GARCH/EGARCH demean, Markov-switching variance
floor, HMM guards and ridge, Kalman Joseph form) genuinely change those libraries'
output, but **nothing on `main` consumes them**, so no stored engine number moves
today. That stops being true the moment PR #41 or #107 Part A lands.

**§8(iii) — credit spreads.** Answered. `macro_data.credit_spreads` is populated
(3,977 rows, 2011-03-23 to 2026-04-07, both columns on 3,928). Only the macro
pipeline reads it, so Gap 6 is market-side detector work, not an ingest ask.
**FRED truncated both ICE BofA series to a rolling 3-year window in April 2026**, so
`scripts/fetch_macro_data.py` can no longer backfill pre-2011 or even re-fetch the
stored history. The database is now the only copy. A backfill needs ICE Data Indices
directly — a new vendor, which §4 makes an HD decision.

---

## #103: what is not done

| item | blocker |
|---|---|
| Step 0(d) timeline gate | needs a scratch **copy** of the database |
| §8(ii) baseline reproduction | same |
| §8(iv) macro ingest timing | a conversation with the data owners, not code |
| Steps 1–6 (#128) | gated on Step 0 being accepted by HD |
| Steps 7–8 (#129, #130, #131) | 37 parts; #128 blocks all three |

Step 0 is step 1 of 9. The work programme has not started.

---

## Read this before trying to continue from a phone or a cloud session

**The database is unreachable from anywhere but Sivan's machine on the VPC.**
It lives at `172.31.23.x:5432` — a private AWS VPC address, database
`new_algo_data`, PostgreSQL 16.14 + TimescaleDB. A cloud sandbox has no route to it.
So Step 0(d), §8(ii), and anything else needing data **cannot be done by a dispatched
session**. Do not attempt it and do not fabricate numbers.

**That database is production.** It carries `auth` (with password hashes),
`trading` (15 tables), `backtest`, `people`. §12 of the issue says plainly: *do not
run anything against production.* The regime runners write run-metadata rows into
shared tables. Reads through pgAdmin are fine; runners are not. Everything done so
far was read-only `SELECT`.

**The C++ build needs setting up from scratch in a fresh environment.** On macOS it
needs Homebrew `apache-arrow eigen googletest libpqxx nlohmann-json nlopt`, plus
Ninja. `/usr/bin/c++`, `/usr/bin/make` and `/usr/bin/gcov` are all Xcode-licence
gated and exit 69 until `sudo xcodebuild -license` is accepted — avoidable by using
the Command Line Tools toolchain instead:

```bash
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Ninja is required: the default Makefiles generator trips over the gated `make`.

### What a dispatched session *can* usefully do

- Read and reason about the regime code on `origin/regime_detection_updated`
- Git analysis: merges, rebases, conflict previews, file censuses
- Draft the §8(iv) questions for the data owners
- Draft documentation and issue comments
- Review the findings in this directory for errors

### What it cannot do

- Anything touching the database
- Anything needing measured numbers from a run
- Steps 1–6, which all require gates against a database copy

---

## Next actions, in order

1. **Post `STEP0_FINDINGS.md` as a comment on #103**, and in the same message ask HD
   for a scratch database copy, naming a snapshot date (§12 says to ask).
2. Wait for HD to accept Step 0. §8 is explicit that the analysis comes back for
   review *before* any code changes numbers.
3. Only then start Step 1.

Do not start Steps 1–6 before HD accepts Step 0.

---

## Related work in another repo

`algosystem` (`AlgoGators/algosystem`) has a finished, pushed branch
`feat/regime-detection` that wires up its own, separate regime-conditional
performance check. It is unrelated to this C++ pipeline — same words, different
codebase. Do not confuse the two.
