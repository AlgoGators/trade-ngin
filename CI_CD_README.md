# CI/CD Pipeline Documentation

This document describes the comprehensive CI/CD pipeline setup for the Trade Ngin project.

## Overview

The CI/CD pipeline consists of several GitHub Actions workflows that ensure code quality, testing, and coverage requirements are met before code can be merged.

## Workflows

### 1. Main CI/CD Pipeline (`ci-cd-pipeline.yml`)

**Triggers:** Push to `main`/`develop` branches, Pull Requests to `main`/`develop`

**Jobs:**
- **Lint:** Code formatting and style checks
- **Build:** Compilation in Debug and Release modes
- **Security Scan:** Basic security checks
- **Report:** Summary report generation

**Features:**
- Runs on both Debug and Release builds
- Enforces 75% code coverage threshold
- Generates detailed reports
- Uploads artifacts for review

### 2. Code Coverage Workflow (`code-coverage.yml`)

**Triggers:** Push to `main`/`develop` branches, Pull Requests to `main`/`develop`

**Features:**
- Dedicated coverage analysis
- Uploads to Codecov for historical tracking
- Comments coverage results on PRs
- Generates HTML and XML reports
- Creates coverage badges

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

Use the existing linting scripts:

```bash
# Run linting and generate report
./linting/lint_runner.sh

# Auto-fix common linting issues
./linting/auto_fix_lint.sh
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
make -j$(nproc)

# Run tests
ctest --output-on-failure --verbose

# Generate coverage report
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '/opt/*' '*/tests/*' '*/externals/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

## Coverage Requirements

- **Minimum Coverage:** 75%
- **Coverage Tools:** lcov, gcovr
- **Reports:** HTML, XML (Cobertura format)
- **Integration:** Codecov for historical tracking

## Workflow Artifacts

### Available Artifacts

1. **Linting Reports**
   - Location: `linting-report-{run_number}`
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
- **Tools:** clang-format, cpplint
- **Failure:** Blocks subsequent stages
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
- **Threshold:** 75% minimum
- **Output:** Test results, coverage reports

### Stage 4: Security
- **Purpose:** Basic security checks
- **Checks:** Common C++ security issues, hardcoded credentials
- **Output:** Security scan results

### Stage 5: Reporting
- **Purpose:** Generate comprehensive reports
- **Reports:** Summary, detailed coverage, linting results
- **Integration:** GitHub comments, Codecov upload

## Configuration

### Environment Variables

- `COVERAGE_THRESHOLD`: 75 (minimum coverage percentage)

### Branch Protection

Recommended branch protection rules for `main` and `develop`:

1. **Require status checks to pass before merging**
   - `lint` job
   - `build` job (both Debug and Release)
   - `coverage` job

2. **Require branches to be up to date before merging**

3. **Dismiss stale PR approvals when new commits are pushed**

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
  COVERAGE_THRESHOLD: 80  # Increase to 80%
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

1. **Linting Fails**
   - Run `./linting/auto_fix_lint.sh` locally
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
   - Use the same environment (Ubuntu latest)

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
