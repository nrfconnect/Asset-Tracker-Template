# Testing and CI setup

The Asset Tracker Template features a comprehensive testing infrastructure with tests run both on real hardware (on target) and emulation.
Code analysis is performed through compliance checks and SonarCloud analysis.

## CI Pipeline Structure

The CI pipeline is composed of the following workflows:

- [.github/workflows/build.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/build.yml): For building the device's firmware.
- [.github/workflows/target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/target-test.yml): For running tests on real hardware.
- [.github/workflows/build-and-target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/build-and-target-test.yml): Workflow that glues together build.yml and target-test.yml.
- [.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml): For building and running tests on emulation, and running Sonarcloud analysis.
- [.github/workflows/compliance.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/compliance.yml): For static compliance checks.

Additionally, AI assistant is used in [.github/workflows/ai-review.yaml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/ai-review.yaml). It is an AI reviewer that runs on pull requests.
You can choose to run it or not with a label.

The CI pipeline is triggered as follows:

- Pull Request: `build.yml`, `sonarcloud.yml`, `compliance.yml`. No target tests are run on PR to avoid instabilities.
- Push to main: `build-and-target-test.yml`, `sonarcloud.yml`. Only "fast" target tests are run. Avoiding excessively time-consuming tests.
- Nightly: `build-and-target-test.yml`. Full set of target tests. Includes "slow" tests such as the full modem FOTA test and the power consumption test.

### Hardware Tests

Run by [.github/workflows/target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/target-test.yml) workflow, implementation details in the [tests/on_target](https://github.com/nrfconnect/Asset-Tracker-Template/tree/main/tests/on_target) folder.

Tests on target are performed using self-hosted runners. How to set up your own instance for your project: [About Self Hosted](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners)

- Tests run on multiple target devices:
    - nRF9151 DK
    - Thingy:91 X
- Uses self-hosted runners labeled according to the connected device.
- Parallel horizontal execution across different device types.
- Parallel vertical execution on multiple same device jobs.
- Runs in a containerized environment with hardware access.
- `pytest` as test runner.
- Supports Memfault integration for symbol file uploads.
- Generates detailed test reports and logs.
- Flexible test execution with support for specific test markers and paths.

Try out tests locally: [tests/on_target/README.md](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/tests/on_target/README.md)

### Emulated Target Tests

Emulation tests are implemented as part of the SonarCloud workflow ([.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml)). These tests:

- Run on the [`native_sim`](https://docs.nordicsemi.com/bundle/ncs-3.0.1/page/zephyr/boards/native/native_sim/doc/index.html) platform using [Twister](https://docs.nordicsemi.com/bundle/ncs-3.0.1/page/zephyr/develop/test/twister.html)
- Execute integration tests in an emulated environment.
- Generate code coverage reports.
- Use build wrapper for accurate code analysis.

### SonarCloud Analysis

The SonarCloud integration ([.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml)) provides:

- Static code analysis for C/C++ files.
- Code coverage reporting
- Pull request analysis and main branch analysis
- Continuous monitoring of code quality metrics
- Separate analysis configurations for main branch and pull requests

### Compliance Testing

Compliance checks are implemented in [.github/workflows/compliance.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/compliance.yml) and include:

- Codeowners validation
- Devicetree compliance
- Git commit message linting
- Identity checks
- Code style and formatting (Nits)
- Python code linting
- Kernel coding style checks (checkpatch)

### Key Features

The following are the key features of testing:

1. **Modularity**: Each aspect of testing is handled by a dedicated workflow.
2. **Comprehensive Coverage**: Combines hardware testing, emulation, and static analysis.
3. **Detailed Reporting**: Generates test reports, coverage data, and compliance checks.
4. **Flexibility**: Supports different test configurations and target devices.
5. **Quality Assurance**: Multiple layers of validation ensure code quality.

## Test Results and Artifacts

Each workflow generates specific artifacts:

- Hardware test results and logs
- Coverage reports
- Compliance check outputs
- SonarCloud analysis reports

Artifacts are available in the GitHub Actions interface and can be used for debugging and quality assurance purposes.
