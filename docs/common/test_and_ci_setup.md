# Testing and CI Setup

The Asset Tracker Template features a comprehensive testing infrastructure with tests run both on real hardware (on target) and emulation.
Code analysis is performed through compliance checks and SonarCloud analysis.

## CI Pipeline Structure

The CI pipeline is composed by the following workflows:
- .github/workflows/build.yml: for building the devices firmware.
- .github/workflows/target-test.yml: for running tests on real hardware.
- .github/workflows/build-target-test.yml: workflow that glues together build.yml and target-test.yml.
- .github/workflows/sonarcloud.yml: for building and running tests on emulation, and run Sonarcloud analysis.
- .github/workflows/compliance.yml: for static compliance checks.

Additionally we are using AI assistant in .github/workflows/ai-review.yaml. It's an AI reviewer that runs on pull request.
Developer can choose to run it or not with a label.


The CI pipeline is triggered as follows:
- On pull request: build.yml, sonarcloud.yml, compliance.yml
The goal initially was to have a CI faster than ~5 minuts on PR. Current CI takes ~4minuts, with sonarcloud being bottleneck.
No target tests are run on PR to avoid instabilities.

- Direct pushes to the main branch when merge: build-target-test.yml, sonarcloud.yml
Only "fast" targets tests are run. Avoiding excessively time-consuming tests.

- Nightly schedule: build-target-test.yml
Full set of target tests. Includes "slow" tests such as full-modem-fw fota test and power consumption test.

### Hardware Testing
The project includes tests that run on real hardware devices, implemented in the `.github/workflows/target-test.yml` workflow. Key features include:

Tests on target are performed on self-hosted runners.
Read more here and how to set up your own instance for your project: [About Self Hosted](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners)


- Tests run on multiple target devices:
  - nRF9151 DK
  - Thingy:91 X
- Uses self-hosted runners labeled according to the connected device
- Parallel orizontal execution across different device types
- Parallel vertical execution on multiple same device jobs
- Runs in a containerized environment with hardware access
- pytest as test runner
- Supports Memfault integration for symbol file uploads
- Generates detailed test reports and logs
- Flexible test execution with support for specific test markers and paths

Try out tests locally: tests/on_target/README.md

### Emulation Testing
Emulation tests are implemented as part of the SonarCloud workflow (`.github/workflows/sonarcloud.yml`). These tests:

- Run on the native_sim platform using Twister
- Execute integration tests in an emulated environment
- Generate code coverage reports
- Use build wrapper for accurate code analysis

### SonarCloud Analysis
The SonarCloud integration (`.github/workflows/sonarcloud.yml`) provides:

- Static code analysis for C/C++ files
- Code coverage reporting
- Pull request analysis and main branch analysis
- Continuous monitoring of code quality metrics
- Separate analysis configurations for main branch and pull requests

### Compliance Testing
Compliance checks are implemented in `.github/workflows/compliance.yml` and include:

- Codeowners validation
- Devicetree compliance
- Git commit message linting
- Identity checks
- Code style and formatting (Nits)
- Python code linting
- Kernel coding style checks (checkpatch)


### Key Features

1. **Modularity**: Each aspect of testing is handled by a dedicated workflow
2. **Comprehensive Coverage**: Combines hardware testing, emulation, and static analysis
3. **Detailed Reporting**: Generates test reports, coverage data, and compliance checks
4. **Flexibility**: Supports different test configurations and target devices
5. **Quality Assurance**: Multiple layers of validation ensure code quality

## Test Results and Artifacts

Each workflow generates specific artifacts:
- Hardware test results and logs
- Coverage reports
- Compliance check outputs
- SonarCloud analysis reports

Artifacts are available in the GitHub Actions interface and can be used for debugging and quality assurance purposes.
