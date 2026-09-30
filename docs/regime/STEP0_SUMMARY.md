# Regime detection, Step 0 — plain-English summary

Issue #103, Step 0 ("census and rebase readiness"). This file explains what was done and what
was found, without assuming you know the regime code. The detailed version, with commands and
exact numbers, is in `STEP0_FINDINGS.md` next to this file.

## The situation

There is a branch, `regime_detection_updated`, holding the regime detection work. It was last
touched on 2026-08-30 and has drifted a long way from `main` — `main` has moved 98 commits ahead
of where the branch split off, while the branch has 34 commits of its own.

Before anyone changes anything on it, #103 asks for a health check: does the branch still build,
do its tests still pass, and what breaks when it is brought back together with `main`?

That health check is Step 0. This is the result.

## What was found

**1. The branch is healthy.** It compiles with no errors, and all 430 of its tests pass. That is
exactly the number #103 predicted, so nothing has silently rotted since August.

**2. Merging is safe. Rebasing is not.**

This is the useful finding. There are two ways to bring the branch and `main` back together, and
they behave very differently.

*Merging* works. The combined result builds, and the full test suite — 1,980 tests, both sides
together — passes with zero failures.

*Rebasing* looks like it works but quietly destroys things. Rebasing replays the branch's 34
commits one at a time on top of `main`, and it stops on a conflict in `CMakeLists.txt`, the file
listing everything the project builds. How you resolve that conflict decides what happens:

- Resolve it by keeping the branch's version and you get a "clean" rebase with one conflict — but
  you have silently thrown away 47 lines of `main`'s build configuration. Gone: the benchmarks,
  the code-coverage setup, a library-path fix, and five source files. Nothing warns you. The
  project still builds, so it looks fine.
- Resolve it by keeping both sides and you get three conflicts instead of one, and the result
  does not build at all — it ends up listing the `apps` folder twice, which CMake rejects.

So neither shortcut is safe. **Recommendation: merge rather than rebase.** If a rebase is
required anyway, budget for three conflicts that a person has to resolve by hand, and re-check
that the project still configures afterwards.

**3. The list of touched files in the issue is incomplete.** #103 lists the files the branch
changes outside its own folder. That list is right as far as it goes, but it misses a few — most
importantly the branch adds a brand-new *shared* clustering library (`gmm.cpp`). That matters
because a separate piece of work, #107, is also planning to add a GMM to `main`. Those two will
collide unless someone reconciles them first.

**4. The shared maths libraries changed behaviour, but nothing on `main` uses them.**

The branch edits five shared statistics files — GARCH, EGARCH, Markov switching, HMM and the
Kalman filter. These are real numerical changes, not tidying. For example, the Markov-switching
variance floor used to always add a small constant and now only applies a floor when one is
needed, which shifts the result of *every* fit slightly.

The important question was whether that moves any number the engine currently produces. It does
not — because nothing on `main` actually calls these libraries yet. They are used only by the
regime code and by their own unit tests.

That is safe today, but it stops being safe the moment #107 adds its GMM, or PR #41 lands its own
edits to two of the same files.

One caveat worth knowing: three existing tests do exercise the changed classes and the branch
leaves them untouched (`test_egarch`, `test_convergence_monitoring`,
`test_statistics_integration`). They still pass, but only because their checks are loose. So the
set of tests that were updated alongside these changes is not complete.

**Recommendation:** land the shared-library fixes as their own separate PR together with the
statistics tests, so the numerical changes get reviewed on their own rather than buried inside a
large regime merge.

## What is still blocked

Step 0 has one more part, and the four analysis questions in section 8 of the issue mostly need
the same thing: **a scratch copy of the database**. The regime pipelines read real market and
macro data, so every measurement in #103 runs against a database copy. Section 12 of the issue
says that copy comes from HD on request, naming a snapshot date. That request has not been made
yet — it is the next action.

The one remaining question, section 8(iv), is about when the macro data ingest gets ported. That
is a conversation with the data owners, not a coding task.

## Where this sits in the whole issue

Step 0 is the first of nine steps, and the last two of those expand into three further issues
(#129, #130, #131) totalling 37 separate parts. #103 is a long programme of work; this is its
opening health check, not a large fraction of it.

Step 0 is also not "done" until HD accepts it. The issue is explicit that the section 8 analysis
comes back for review *before* any code changes numbers.

## Reproducing the build on macOS

The default system compiler is blocked behind an unsigned Xcode licence agreement and exits with
an error. That is avoidable without `sudo` — use the Command Line Tools compiler and Ninja:

```
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/Library/Developer/CommandLineTools/usr/bin/clang++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Ninja is required — the default Makefiles generator also trips over the blocked licence.
`DEVELOPER_DIR` is required, or the threads check fails and takes the GoogleTest lookup with it.
Dependencies come from Homebrew; no vcpkg tree is needed.
