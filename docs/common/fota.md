# Firmware updates (FOTA)

This guide covers how to perform Firmware Over The Air (FOTA) updates using
[nRF Cloud](docs.nrfcloud.com). Updates are delivered to the device through
nRF Cloud's CoAP transport. See the [FOTA module](../modules/fota_module.md) for how the device itself handles an update once it is
available.

## Firmware versioning

The following sections cover the principles and practices used to define, manage, and maintain firmware versioning.

### Version components

Firmware versions are defined in the `app/VERSION` file:

- **VERSION_MAJOR**: Major version
- **VERSION_MINOR**: Minor version
- **PATCHLEVEL**: Patch level
- **VERSION_TWEAK**: Additional component (typically 0)
- **EXTRAVERSION**: Extra string (for example, "dev", "rc1")

Example resulting in version `1.2.3-dev`:

```plaintext
VERSION_MAJOR = 1
VERSION_MINOR = 2
PATCHLEVEL = 3
VERSION_TWEAK = 0
EXTRAVERSION = dev
```

### Preparing firmware

- **Application**: Update the `app/VERSION` file (increment the appropriate version component),
  then build normally:

    ```bash
    west build -p -b thingy91x/nrf9151/ns # Build for the appropriate board
    ```

    Or use the nRF Connect for VS Code extension. See the [Getting Started](getting_started.md)
    guide for details on building with the extension. The OTA payload is the signed application
    binary at `build/app/zephyr/zephyr.signed.bin`.

- **Modem**: Nordic publishes delta modem firmware update packages -- download the
  `mfw_nrf91x1_<version>.zip` release for your target version; it contains pre-built delta
  binaries named `mfw_nrf91x1_update_from_<from_version>_to_<to_version>.bin` for the supported
  upgrade paths. Delta modem FOTA requires `CONFIG_MEMFAULT_FOTA_MODEM_UPDATE=y` and a real
  `CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY` compiled into the firmware (find it under
  **Settings → General** in your modem Memfault project). See
  [Create a Modem Firmware Project](https://docs.nrfcloud.com/docs/mcu/nrf-modem-fota#step-1-create-a-modem-firmware-project).

- **Bootloader**: Not currently supported through this OTA flow.

### Version verification

The device continues to report its firmware versions to nRF Cloud's device shadow regardless of
how the update was delivered. To verify a successful update, query the device shadow:

```bash
curl -X GET "https://api.nrfcloud.com/v1/devices/${DEVICE_ID}" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

- **Application updates**: Check that the `appVersion` field matches the new version.
- **Modem updates**: Check that the `modemFirmware` field matches the new version.

## Performing FOTA updates

After deploying a release to a cohort (see below), the device checks for updates automatically on
a configured interval and when triggered by user input (for example, a button press). To trigger a
check manually during development, connect to the device shell and run:

```bash
att_fota poll
```

The device must be connected to the network and cloud. If a pending update is found, the FOTA
module starts the download automatically.

To trigger an immediate FOTA poll from the device, press and hold **Button 1**. On **Thingy:91 X**,
pressing on the top of the case pushes Button 1.

### Setup

Updates are managed with the [`memfault` CLI](https://mflt.io/memfault-cli) (`pip install
memfault-cli`). You'll need an organization token, organization slug, and project slug:

```bash
export ORG_TOKEN=<your-memfault-org-token>   # https://app.memfault.com/organizations/-/settings/auth-tokens
export ORG_SLUG=<your-org-slug>              # https://app.memfault.com/organizations/-/projects/-/settings
export PROJECT_SLUG=<your-app-project-slug>
```

Modem firmware is managed under a separate Memfault project (the same one whose project key you
compiled into `CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY`):

```bash
export MODEM_ORG_TOKEN=<your-modem-org-token>
export MODEM_ORG_SLUG=<your-modem-org-slug>
export MODEM_PROJECT_SLUG=<your-modem-project-slug>
```

The on-target test suite uses the same setup; see
[tests/on_target/README.md](../../tests/on_target/README.md) and
[tests/on_target/utils/nrfcloud.py](../../tests/on_target/utils/nrfcloud.py) for the CLI wrapper
these examples are based on.

### Application update (full release)

```bash
export HW_VERSION=<hardware_version>   # e.g. thingy91x, nrf9151dk
export NEW_VERSION=1.2.3

memfault --org-token $ORG_TOKEN --org $ORG_SLUG --project $PROJECT_SLUG \
  upload-ota-payload \
  --hardware-version $HW_VERSION \
  --software-type app \
  --software-version $NEW_VERSION \
  build/app/zephyr/zephyr.signed.bin

memfault --org-token $ORG_TOKEN --org $ORG_SLUG --project $PROJECT_SLUG \
  deploy-release --release-version $NEW_VERSION --cohort default
```

### Modem update (delta release)

Delta modem releases are declared with `--delta-from`/`--delta-to` rather than
`--software-version`, matching the `from`/`to` versions in the delta binary's filename:

```bash
export FROM_VERSION=mfw_nrf91x1_2.0.3
export TO_VERSION=mfw_nrf91x1_2.0.4

memfault --org-token $MODEM_ORG_TOKEN --org $MODEM_ORG_SLUG --project $MODEM_PROJECT_SLUG \
  upload-ota-payload \
  --hardware-version $HW_VERSION \
  --software-type mfw \
  --delta-from $FROM_VERSION \
  --delta-to $TO_VERSION \
  mfw_nrf91x1_update_from_2.0.3_to_2.0.4.bin

memfault --org-token $MODEM_ORG_TOKEN --org $MODEM_ORG_SLUG --project $MODEM_PROJECT_SLUG \
  deploy-release --delta-from $FROM_VERSION --delta-to $TO_VERSION --cohort default
```

> [!NOTE]
> Memfault applies [SemVer precedence](https://semver.org/#spec-item-11) when deciding whether a
> delta represents an upgrade: a version with a pre-release suffix (e.g. `2.0.4-FOTA-TEST`) has
> *lower* precedence than the plain version (`2.0.4`), so a device already at `2.0.4` is never
> offered a delta to `2.0.4-FOTA-TEST` unless the target cohort's **"bypass version checks"**
> setting is enabled in the Memfault dashboard. This only matters for pre-release-style test
> versions -- a normal version bump (e.g. `2.0.3` → `2.0.4`) is an unambiguous upgrade and needs no
> special handling.

### Deactivating a release

```bash
memfault --org-token $ORG_TOKEN --org $ORG_SLUG --project $PROJECT_SLUG \
  deploy-release --release-version $NEW_VERSION --cohort default --deactivate
```

For a delta release, pass `--delta-from`/`--delta-to` instead of `--release-version`, as above.

### Reference

See `memfault --help`, `memfault upload-ota-payload --help`, and `memfault deploy-release --help`
for the full set of options (hardware versions, rollout percentage, must-pass-through releases,
and more).
