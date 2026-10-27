# Firmware updates (FOTA)

This guide covers how to perform Firmware Over The Air (FOTA) updates using
[nRF Cloud](https://docs.nrfcloud.com). Updates are delivered to the device through
nRF Cloud's CoAP transport. See the [FOTA module](../modules/fota_module.md) for how the device itself handles an update once it is
available.

## Firmware versioning

The following sections cover the principles and practices used to define, manage, and maintain firmware versioning.

### Version components

The `app/VERSION` file defines the firmware version with the following components:

- **VERSION_MAJOR**: Major version.
- **VERSION_MINOR**: Minor version.
- **PATCHLEVEL**: Patch level.
- **VERSION_TWEAK**: Additional component, which is usually `0`.
- **EXTRAVERSION**: Extra string, for example `dev` or `rc1`.

The following example results in version `1.2.3-dev`:

```plaintext
VERSION_MAJOR = 1
VERSION_MINOR = 2
PATCHLEVEL = 3
VERSION_TWEAK = 0
EXTRAVERSION = dev
```

### Preparing firmware

Complete the following steps when preparing application or modem firmware.

- **Application**: Update the `app/VERSION` file. Increment the appropriate version component. Then build the firmware.

    From the command line, run the following command:

    ```bash
    west build -p -b thingy91x/nrf9151/ns # Build for the appropriate board
    ```

    You can also use nRF Connect for VS Code. For information about building with the extension, see [Getting started](getting_started.md). The OTA payload is the signed application binary at `build/app/zephyr/zephyr.signed.bin`.

- **Modem**: Nordic publishes delta modem firmware update packages. Download the `mfw_nrf91x1_<version>.zip` release for your target version. It contains pre-built delta binaries named `mfw_nrf91x1_update_from_<from_version>_to_<to_version>.bin` for the supported upgrade paths.

    Delta modem FOTA requires `CONFIG_MEMFAULT_FOTA_MODEM_UPDATE=y` and the Memfault project key of your modem project. Find the key under **Settings** > **General** in your modem Memfault project. For information about creating the project, see [Create a modem firmware project](https://docs.nrfcloud.com/docs/mcu/nrf-modem-fota#step-1-create-a-modem-firmware-project).

    Provide the key to the device in one of the following ways:

    - Compile it into the firmware with `CONFIG_MEMFAULT_FOTA_MODEM_PROJECT_KEY`.
    - Set it at runtime in the device shadow. Add `control.memfaultModemKey` to the desired state of the device, for example `{"desired": {"control": {"memfaultModemKey": "<key>"}}}` through `PATCH /v1/devices/{deviceId}/state`. The key can have at most 32 characters. The device applies it when it polls the shadow, which requires `CONFIG_APP_CLOUD_SHADOW_CONTROL` and is enabled by default. The key is not stored, so the device applies it again after every reboot.

- **Bootloader**: Not currently supported through this OTA flow.

### Version verification

The device continues to report its firmware versions to the nRF Cloud device shadow, regardless of how the update was delivered. To verify that an update succeeded, query the device shadow:

```bash
curl -X GET "https://api.nrfcloud.com/v1/devices/${DEVICE_ID}" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

- **Application updates**: Check that the `appVersion` field matches the new version.
- **Modem updates**: Check that the `modemFirmware` field matches the new version.

## Performing FOTA updates

After you deploy a release to a cohort, the device checks for updates automatically at a configured interval and when the user triggers a check, for example with a button press. To trigger a check manually during development, connect to the device shell and run the following command:

```bash
att_fota poll
```

The device must be connected to the network and the cloud. If the device finds a pending update, the FOTA module starts the download automatically.

To trigger an immediate FOTA poll from the device, press and hold **Button 1**. On the Thingy:91 X, pressing on the top of the case pushes Button 1.

### Setup

You manage updates with the [`memfault` CLI](https://mflt.io/memfault-cli), which you can install with `pip install memfault-cli`. You need an organization token, an organization slug, and a project slug.

Set the following environment variables:

```bash
export ORG_TOKEN=<your-memfault-org-token>   # https://app.memfault.com/organizations/-/settings/auth-tokens
export ORG_SLUG=<your-org-slug>              # https://app.memfault.com/organizations/-/projects/-/settings
export PROJECT_SLUG=<your-app-project-slug>
```

Modem firmware is managed under a separate Memfault project, which is the same one whose project key the device uses for modem updates:

```bash
export MODEM_ORG_TOKEN=<your-modem-org-token>
export MODEM_ORG_SLUG=<your-modem-org-slug>
export MODEM_PROJECT_SLUG=<your-modem-project-slug>
```

The on-target test suite uses the same setup. For more information, see [tests/on_target/README.md](../../tests/on_target/README.md). For the CLI wrapper that these examples are based on, see [tests/on_target/utils/nrfcloud.py](../../tests/on_target/utils/nrfcloud.py).

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

Delta modem releases are declared with `--delta-from` and `--delta-to` instead of `--software-version`. The values match the `from` and `to` versions in the delta binary filename:

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
> Memfault applies [SemVer precedence](https://semver.org/#spec-item-11) when deciding whether a delta represents an upgrade. A version with a pre-release suffix, for example `2.0.4-FOTA-TEST`, has lower precedence than the plain version (`2.0.4`). A device already at `2.0.4` is therefore never offered a delta to `2.0.4-FOTA-TEST`, unless the target cohort has the **Bypass version checks** setting enabled in the Memfault dashboard. This only matters for pre-release-style test versions. A normal version bump, for example `2.0.3` to `2.0.4`, is an unambiguous upgrade and needs no special handling.

### Deactivating a release

To deactivate a full release, run the following command:

```bash
memfault --org-token $ORG_TOKEN --org $ORG_SLUG --project $PROJECT_SLUG \
  deploy-release --release-version $NEW_VERSION --cohort default --deactivate
```

To deactivate a delta release, such as the modem update from [Modem update (delta release)](#modem-update-delta-release), identify it with `--delta-from` and `--delta-to`:

```bash
memfault --org-token $MODEM_ORG_TOKEN --org $MODEM_ORG_SLUG --project $MODEM_PROJECT_SLUG \
  deploy-release --delta-from $FROM_VERSION --delta-to $TO_VERSION --cohort default --deactivate
```

### Reference

See `memfault --help`, `memfault upload-ota-payload --help`, and `memfault deploy-release --help`
for the full set of options (hardware versions, rollout percentage, must-pass-through releases,
and more).
