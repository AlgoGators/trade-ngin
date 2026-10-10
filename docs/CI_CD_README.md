# CI/CD Pipeline Documentation

This document describes the comprehensive CI/CD pipeline setup for the Trade Ngin project.

## Overview

Continuous integration runs in GitHub Actions. The workflows are in `.github/workflows/`. There is
one pipeline workflow that lints, builds, tests and measures coverage, and five smaller workflows.
The `lint`, `build`, `image-generation` and `deploy-to-ec2` jobs of the pipeline run inside the CI
image `ghcr.io/algogators/trade-ngin-ci:latest`, which carries the build dependencies.

## Workflows

### 1. CI/CD Pipeline (`ci-cd-pipeline.yml`)

**Triggers:** a push to `main`, `develop`, `main-hd`, `prod` or `staging`, and a pull request onto
any branch.

**Jobs, in order:**

| Job (id) | Check name | What it does |
|---|---|---|
| `lint` | Code Linting | clang-format, clang-tidy, cppcheck and cpplint. The findings are written to a report and do not fail the job |
| `build` | Build and Test (Debug), Build and Test (Release) | builds Debug and Release (a matrix), runs the tests in both, and on Debug also runs `ctest` under Valgrind (the launcher only: the step does not pass `--trace-children=yes`, so the test binary itself is not checked), generates coverage with lcov and gcovr, checks the coverage threshold and runs the SonarCloud scan when its token is present and the run is a pull request or a push to `main`, `prod` or `staging` |
| `security-scan` | Security Scan | greps the sources for unsafe string functions and for words such as "password" |
| `report` | Generate Summary Report | writes a summary of the three jobs above; runs even when one of them failed |
| `image-generation` | Image Generation | builds and pushes the container image and scans it with Trivy; runs on pull requests and on pushes to `prod` and `staging` |
| `schema-ownership-guard` | Schema Ownership Guard | fails if a string literal in `src/data` or `src/storage` starts with the name of a schema this repository does not own (`futures_data`, `equities_data`, `options_data`, `synthetic`, `auth`, `research`); a schema named in the middle of a query, as the read queries do, is not matched |
| `deploy-to-ec2` | Deploy to EC2 | deploys on a push to `prod` |

**Coverage:** the threshold is `COVERAGE_THRESHOLD: 10` (line coverage, percent), set at the top of
the workflow and checked in the Debug build by the step "Check Coverage Threshold". There is no
separate coverage workflow: coverage is a step of the `build` job.

### 2. Other workflows

| File | Name | Trigger | Purpose |
|---|---|---|---|
| `live-trading-watchdog.yml` | Live Trading Watchdog | daily schedule, manual, and pull requests that touch the watchdog | checks that the live run wrote its rows (on a pull request only the script's self-test runs); see [performance_upkeep.md](performance_upkeep.md) |
| `branch-protection.yml` | Branch Protection Setup | daily schedule and manual | applies the branch protection settings to `main`, `develop` and `main-hd`, in a job that runs only when the actor is the repository owner |
| `dependency-review.yml` | Dependency Review | pull requests onto `main` | fails a pull request that adds a dependency with a known vulnerability of severity high or above; no licence list is configured |
| `sbom.yml` | Generate SBOM | push to `main`, manual | produces the software bill of materials |
| `scorecard.yml` | OSSF Scorecard | weekly schedule, push to `main`, branch protection rule changes | supply-chain scorecard |

## Local Development Setup

### Prerequisites

Install the required tools for local development:

```bash
# Ubuntu/Debian
sudo apt-get install clang-format cpplint

# macOS
brew install clang-format
pip install cpplint
```

### Pre-commit Hook

Run the pre-commit hook before pushing code:

```bash
# Make the script executable
chmod +x scripts/pre-commit-hook.sh

# Run the pre-commit checks
./scripts/pre-commit-hook.sh
```

### Local Linting

Apart from `scripts/pre-commit-hook.sh` the repository carries no lint script (the
`./linting/auto_fix_lint.sh` that the hook's failure message names does not exist). Run the tools
the pipeline runs:

```bash
# Check formatting (what the pipeline checks)
find src include -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror

# Fix formatting in place
find src include -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i

# Style check
cpplint --recursive --filter=-legal/copyright,-build/include_order src include
```

### Local Testing

Build and test locally:

```bash
# Create build directory
mkdir -p build
cd build

# Configure with coverage
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-g -O0 -fprofile-arcs -ftest-coverage"

# Build
cmake --build . -j

# Run tests
ctest --output-on-failure --verbose

# Generate coverage report
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '/opt/*' '*/tests/*' '*/externals/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

## Coverage Requirements

- **Threshold enforced by CI:** 10 percent line coverage (`COVERAGE_THRESHOLD` in
  `.github/workflows/ci-cd-pipeline.yml`). A Debug build below it fails the `build` job.
- **Coverage Tools:** lcov, gcovr
- **Reports:** lcov `coverage.info`, gcovr XML (Cobertura format), gcovr text, and the SonarQube
  generic coverage XML
- **Integration:** the SonarCloud scan reads the coverage report when its token is configured

## Workflow Artifacts

### Available Artifacts

1. **Linting Reports**
   - Location: `linting-report-{run_number}` and `linting-summary-{run_number}`
   - Contains: Detailed linting results and formatting issues

2. **Coverage Reports**
   - Location: `coverage-reports-{run_number}`
   - Contains: HTML reports, XML reports, coverage data

3. **Test Results**
   - Location: `test-results-{run_number}-{build-type}`
   - Contains: CTest results and logs

4. **Summary Reports**
   - Location: `summary-report-{run_number}`
   - Contains: Overall pipeline status

### Accessing Artifacts

1. Go to your GitHub repository
2. Click on "Actions" tab
3. Select a workflow run
4. Scroll down to "Artifacts" section
5. Download the desired reports

## Pipeline Stages

### Stage 1: Linting
- **Purpose:** Ensure code quality and consistency
- **Tools:** clang-format, clang-tidy, cppcheck, cpplint
- **Failure:** findings are reported and do not fail the job; the build job waits for this job
- **Output:** Linting report artifact

### Stage 2: Building
- **Purpose:** Compile code in multiple configurations
- **Configurations:** Debug, Release
- **Coverage:** Enabled for Debug builds
- **Output:** Compiled binaries, test executables

### Stage 3: Testing
- **Purpose:** Run unit tests and measure coverage
- **Framework:** Google Test
- **Coverage:** lcov + gcovr
- **Threshold:** 10 percent line coverage
- **Output:** Test results, coverage reports

### Stage 4: Security
- **Purpose:** Basic security checks
- **Checks:** Common C++ security issues, hardcoded credentials
- **Output:** Security scan results

### Stage 5: Reporting
- **Purpose:** Generate comprehensive reports
- **Reports:** a summary of the lint, build and security results, uploaded as an artifact

## Configuration

### Environment Variables

- `COVERAGE_THRESHOLD`: 10 (minimum line coverage percentage)

### Branch Protection

`branch-protection.yml` configures these rules for the protected branches:

1. **Require status checks to pass before merging.** The contexts it names are `lint`,
   `build (Debug)`, `build (Release)` and `coverage`. None of them is the name of a check the
   pipeline reports: the checks are "Code Linting", "Build and Test (Debug)" and
   "Build and Test (Release)", and there is no coverage job (coverage is a step of the Debug
   `build` job).

2. **Require branches to be up to date before merging**

3. **Require one approving review, and dismiss stale PR approvals when new commits are pushed**

4. **Disallow force pushes and branch deletions**

### Customization

#### Adding New Linting Rules

Edit the linting steps in the workflow:

```yaml
- name: Run Custom Linter
  run: |
    # Add your custom linting command here
    custom-linter src include
```

#### Modifying Coverage Threshold

Change the environment variable:

```yaml
env:
  COVERAGE_THRESHOLD: 10  # line coverage, percent
```

#### Adding New Test Types

Update the CMake configuration and workflow:

```yaml
- name: Run Integration Tests
  run: |
    cd build
    ./integration_tests
```

## Troubleshooting

### Common Issues

1. **Lint findings** (they do not fail the job)
   - Run `clang-format -i` on the files named in the report
   - Check the linting report artifact for specific issues

2. **Coverage Below Threshold**
   - Add more unit tests
   - Check coverage report to identify uncovered code
   - Consider excluding test-only code from coverage

3. **Build Failures**
   - Check dependency installation
   - Verify CMake configuration
   - Review build logs in artifacts

4. **Test Failures**
   - Run tests locally to reproduce
   - Check test output in artifacts
   - Verify test data and mocks

### Debugging Workflows

1. **Enable Debug Logging**
   ```yaml
   - name: Debug Info
     run: |
       echo "Debug information"
       ls -la
   ```

2. **Check Artifacts**
   - Download and examine workflow artifacts
   - Look for error logs and reports

3. **Local Reproduction**
   - Run the same commands locally
   - Use the same environment: the CI image `ghcr.io/algogators/trade-ngin-ci:latest`

## Best Practices

### For Developers

1. **Always run pre-commit checks locally**
   ```bash
   ./scripts/pre-commit-hook.sh
   ```

2. **Write tests for new code**
   - Aim for 100% coverage of new code
   - Follow existing test patterns

3. **Keep linting clean**
   - Fix formatting issues before pushing
   - Use auto-fix when possible

4. **Monitor coverage**
   - Check coverage reports regularly
   - Add tests for uncovered code paths

### For Maintainers

1. **Monitor pipeline health**
   - Check workflow success rates
   - Review coverage trends

2. **Update dependencies**
   - Keep build tools updated
   - Monitor security advisories

3. **Optimize pipeline**
   - Cache dependencies when possible
   - Parallelize independent jobs

## Integration with IDEs

### VS Code

Add to `.vscode/settings.json`:

```json
{
  "C_Cpp.clang_format_style": "file",
  "C_Cpp.default.cppStandard": "c++20",
  "files.associations": {
    "*.hpp": "cpp",
    "*.cpp": "cpp"
  }
}
```

### CLion

1. Import CMake project
2. Configure clang-format integration
3. Set up Google Test integration

## Support

For issues with the CI/CD pipeline:

1. Check the workflow logs in GitHub Actions
2. Review the troubleshooting section
3. Create an issue with detailed error information
4. Include relevant artifacts and logs 