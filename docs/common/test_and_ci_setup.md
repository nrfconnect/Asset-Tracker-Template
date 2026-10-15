# Testing and CI setup

The Asset Tracker Template features a comprehensive testing infrastructure, with tests that run both on real hardware, which is called on target, and in emulation.
The template analyzes the code through compliance checks and SonarCloud analysis.

## CI pipeline structure

The CI pipeline is composed of the following workflows:

- [.github/workflows/build.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/build.yml): For building the device's firmware.
- [.github/workflows/target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/target-test.yml): For running tests on real hardware.
- [.github/workflows/build-and-target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/build-and-target-test.yml): Workflow that glues together `build.yml` and `target-test.yml`.
- [.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml): For building and running tests in emulation, and for running the SonarCloud analysis.
- [.github/workflows/compliance.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/compliance.yml): For static compliance checks.
- [.github/workflows/doc-sync.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/doc-sync.yml): For publishing documentation to Nordic Semiconductor's external documentation platform.
- [.github/workflows/create-doc-bundle.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/create-doc-bundle.yml): Creates the documentation bundle that the template uploads to Nordic Semiconductor's [official documentation site](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/index.html).

The following events trigger the CI pipeline:

- **Pull request**: Runs `build.yml`, `sonarcloud.yml`, and `compliance.yml`. The pipeline runs no target tests on a pull request, to avoid instabilities.
- **Push to main**: Runs `build-and-target-test.yml`, `sonarcloud.yml`, and `doc-sync.yml`. The pipeline runs only the fast target tests, which avoids excessively time-consuming tests.
- **Nightly**: Runs `build-and-target-test.yml` with the full set of target tests. This includes slow tests, such as the full modem FOTA test and the application and MCUboot bootloader FOTA tests.

### Hardware tests

The [.github/workflows/target-test.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/target-test.yml) workflow runs the hardware tests. For the implementation details, see the [tests/on_target](https://github.com/nrfconnect/Asset-Tracker-Template/tree/main/tests/on_target) folder.

The tests on target run on self-hosted runners. To set up your own instance for your project, see [About self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners) in the GitHub documentation.

The hardware tests have the following characteristics:

- The tests run on multiple target devices:
    - nRF9151 DK
    - Thingy:91 X
- The workflow uses self-hosted runners labeled according to the connected device.
- Parallel horizontal execution across different device types.
- Parallel vertical execution on multiple jobs for the same device.
- The tests run in a containerized environment with hardware access.
- The test runner is `pytest`.
- The tests support Memfault integration for symbol file uploads.
- The workflow generates detailed test reports and logs.
- Flexible test execution with support for specific test markers and paths.

To try out the tests locally, see [tests/on_target/README.md](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/tests/on_target/README.md).

### Emulated target tests

The emulation tests are implemented as part of the SonarCloud workflow, [.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml). These tests have the following characteristics:

- The tests run on the [`native_sim`](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/boards/native/native_sim/doc/index.html) platform using [Twister](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/develop/test/twister.html).
- The tests execute integration tests in an emulated environment.
- The workflow generates code coverage reports.
- The workflow uses a build wrapper for accurate code analysis.

### SonarCloud analysis

The SonarCloud integration, [.github/workflows/sonarcloud.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/sonarcloud.yml), provides the following analysis:

- Static code analysis for C and C++ files.
- Code coverage reporting.
- Pull request analysis and main branch analysis.
- Continuous monitoring of code quality metrics.
- Separate analysis configurations for the main branch and for pull requests.

### Compliance testing

The compliance checks are implemented in [.github/workflows/compliance.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/compliance.yml) and include the following checks:

- Codeowners validation.
- Devicetree compliance.
- Git commit message linting.
- Identity checks.
- Code style and formatting, called Nits.
- Python code linting.
- Kernel coding style checks with `checkpatch`.

### Documentation sync

The documentation sync workflow, [.github/workflows/doc-sync.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/doc-sync.yml), is triggered on a push to main when the `docs/` folder changes. It performs two jobs:

1. **Create documentation bundle**: Uses [create-doc-bundle.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/create-doc-bundle.yml) to package the documentation for external publishing.
2. **Publish documentation**: Uploads the documentation bundle to the prod and dev documentation hosting platforms.

The build workflow, [.github/workflows/build.yml](https://github.com/nrfconnect/Asset-Tracker-Template/blob/main/.github/workflows/build.yml), keeps the `gh-pages` branch in sync with main. The workflow merges main into `gh-pages` before it updates the badge JSON files and the interactive HTML reports for memory usage and power measurements on the [GitHub Pages site](https://nrfconnect.github.io/Asset-Tracker-Template).

### Key features

The testing infrastructure has the following key features:

1. **Modularity**: A dedicated workflow handles each aspect of testing.
2. **Comprehensive coverage**: The infrastructure combines hardware testing, emulation, and static analysis.
3. **Detailed reporting**: The workflows generate test reports, coverage data, and compliance checks.
4. **Flexibility**: The infrastructure supports different test configurations and target devices.
5. **Quality assurance**: Multiple layers of validation ensure code quality.

## Test results and artifacts

Each workflow generates specific artifacts:

- Hardware test results and logs.
- Coverage reports.
- Compliance check outputs.
- SonarCloud analysis reports.

The artifacts are available in the GitHub Actions interface, and you can use them for debugging and quality assurance.
