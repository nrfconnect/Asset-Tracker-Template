# Firmware updates (FOTA)

This guide covers how to perform firmware over-the-air (FOTA) updates with [Memfault OTA](https://docs.memfault.com/docs/platform/ota).

The template can update the following components of a device in one release:

| Component | Memfault software type | Image |
| --- | --- | --- |
| Application | `app` | `zephyr.signed.bin` (`-update-signed.bin` release artifact) |
| Modem firmware, delta | `mfw` (`CONFIG_MEMFAULT_FOTA_MODEM_SOFTWARE_TYPE`) [^1] | Delta image from one modem firmware version to another |
| Modem firmware, full | `mfw-full` (`CONFIG_APP_FOTA_MODEM_FULL_SOFTWARE_TYPE`) [^1] | Full modem firmware image (`.cbor`) |
| Bootloader (MCUboot), slot 0 image | `mcuboot-s0` | `signed_by_mcuboot_and_b0_mcuboot.bin` of the DFU package |
| Bootloader (MCUboot), slot 1 image | `mcuboot-s1` | `signed_by_mcuboot_and_b0_mcuboot_s1_variant.bin` of the DFU package |

[^1]: For bandwidth optimization it is recommended to use delta updates for the modem firmware whenever possible. As component updates are checked in lexicographical order it is recommended to name modem firmware components accordingly.

## Firmware versioning

The following sections cover the principles and practices used to define, manage, and maintain firmware versioning.

### Version components

The `app/VERSION` file defines the firmware version with the following components:

- **VERSION_MAJOR**: Major version.
- **VERSION_MINOR**: Minor version.
- **PATCHLEVEL**: Patch level.
- **VERSION_TWEAK**: Not used. (The build system requires the field, so keep it at `0`.)
- **EXTRAVERSION**: Extra string, for example `dev` or `rc1`.

The following example results in version `1.2.3-dev`:

```plaintext
VERSION_MAJOR = 1
VERSION_MINOR = 2
PATCHLEVEL = 3
VERSION_TWEAK = 0
EXTRAVERSION = dev
```

The device reports this version to Memfault as its software version. A Memfault release has a release version, which is the software version of the application. The other components of the release have a component version of their own, which is the version that the device reports for that component.

### Version ordering

Memfault only offers a version that sorts after the version that the device reports. This applies to the release version of the application and to the component version of every other component. Keep the following in mind:

- A pre-release version sorts before the plain release. For example, `1.2.3-rc1` sorts before `1.2.3`, so a device that runs `1.2.3` is not offered `1.2.3-rc1`.
- A component version is compared as a string. A modem component version that must be offered to a device that reports `mfw_nrf91x1_2.0.4` must sort after it, for example `mfw_nrf91x1_2.0.5`.
- The bootloader component version is `mcuboot_<version>`, where `<version>` is the `CONFIG_FW_INFO_FIRMWARE_VERSION` of the MCUboot image, for example `mcuboot_3`. The device reports the version of the bootloader that is running, as `mcuboot_2` for example.

### Preparing firmware

Complete the following steps when preparing application or bootloader firmware:

1. Update the `app/VERSION` file. Increment the appropriate version component.
1. Build the firmware.

    From the command line, run the following command:

    ```bash
    west build -p -b thingy91x/nrf9151/ns # Build for the appropriate board
    ```

    You can also use nRF Connect for VS Code. For information about building with the extension, see [Getting started](getting_started.md).

1. Locate the update files in the `app/build/` output directory:

    - `build/app/zephyr/zephyr.signed.bin`: The application firmware update.
    - `build/dfu_mcuboot.zip`: The bootloader update. The zip file contains the images for both bootloader slots and a `manifest.json` with the version of each.

For modem updates, use the delta images or the full modem image from the modem firmware package. Download the package from the Downloads page of your SiP: [nRF9151](https://www.nordicsemi.com/Products/nRF9151/Download). A delta image updates between two specific versions, and a full image can update from any version. For more information, see [Modem Firmware Update (FOTA)](https://docs.memfault.com/docs/mcu/nrf-modem-fota#modem-firmware-packages) in the Memfault documentation.

### Version verification

To verify that an update succeeded, check the version that the device reports in Memfault after the update:

- **Application updates**: The device software version matches the release version.
- **Modem updates**: The modem firmware version of the device matches the component version of the update.
- **Bootloader updates**: The bootloader version of the device matches the component version of the update.

## Performing FOTA updates

You manage FOTA updates with Memfault releases. After you deploy a release to the cohort of a device, the device checks for updates automatically at a configured interval and when the user triggers a check, for example with a button press. To trigger a check manually during development, connect to the device shell and run the following command:

```bash
att_fota poll
```

The device must be connected to the network and the cloud. If the device finds an update, the FOTA module starts the download automatically.

To trigger an immediate FOTA poll from the device, press and hold **Button 1**. On the Thingy:91 X, pressing on the top of the case pushes Button 1.

The device checks the application first, then the other components in turn. The first component that has an update is downloaded and installed.

### Memfault CLI

The following sections describe how to set up the Memfault CLI, and how to upload and deploy a release.

#### Setup

Install the [Memfault CLI](https://docs.memfault.com/docs/ci/install-memfault-cli) version 1.11.0 or later:

```bash
pip install memfault-cli
```

Set the following environment variables. Create the organization auth token under **Admin** > **Organization Auth Tokens** in Memfault. The role of the token must allow deploying releases (Manager). Find the slugs under **Project Settings** > **General**.

```bash
export MEMFAULT_ORG_TOKEN=<your-organization-auth-token>
export MEMFAULT_ORG=<your-organization-slug>
export MEMFAULT_PROJECT=<your-project-slug>
export HARDWARE_VERSION=<hardware-version-of-the-device>   # for example thingy91x or nrf9151dk
export COHORT=<cohort-of-the-device>
```

For the FOTA tests of this repository, see [tests/on_target/README.md](../../tests/on_target/README.md).

#### Complete update workflow

To deploy a firmware update, complete the following steps:

1. Upload the application image to a release. The release version is the version of the new application, for example `1.2.4`:

    ```bash
    export RELEASE_VERSION=1.2.4

    memfault upload-ota-payload \
      --hardware-version ${HARDWARE_VERSION} \
      --software-type app \
      --software-version ${RELEASE_VERSION} \
      build/app/zephyr/zephyr.signed.bin
    ```

1. Optionally, upload more components to the same release:

    ```bash
    # Modem firmware, delta from the version that the device reports
    memfault upload-ota-payload \
      --hardware-version ${HARDWARE_VERSION} \
      --software-type mfw \
      --software-version ${RELEASE_VERSION} \
      --component-version mfw_nrf91x1_2.0.5 \
      --component-base-version mfw_nrf91x1_2.0.4 \
      mfw_nrf91x1_update_from_2.0.4_to_2.0.5.bin

    # Modem firmware, full image
    memfault upload-ota-payload \
      --hardware-version ${HARDWARE_VERSION} \
      --software-type mfw-full \
      --software-version ${RELEASE_VERSION} \
      --component-version mfw_nrf91x1_2.0.5 \
      mfw_nrf91x1_2.0.5.cbor

    # Bootloader, upload both slot images with the same component version
    memfault upload-ota-payload \
      --hardware-version ${HARDWARE_VERSION} \
      --software-type mcuboot-s0 \
      --software-version ${RELEASE_VERSION} \
      --component-version mcuboot_3 \
      signed_by_mcuboot_and_b0_mcuboot.bin

    memfault upload-ota-payload \
      --hardware-version ${HARDWARE_VERSION} \
      --software-type mcuboot-s1 \
      --software-version ${RELEASE_VERSION} \
      --component-version mcuboot_3 \
      signed_by_mcuboot_and_b0_mcuboot_s1_variant.bin
    ```

    The full modem image uses the same component version as the delta images that lead to the same modem firmware.

1. Deploy the release to the cohort of the device:

    ```bash
    memfault deploy-release \
      --release-version ${RELEASE_VERSION} \
      --cohort ${COHORT}
    ```

1. Trigger a poll on the device with `att_fota poll`, or wait for the next periodic poll. The device logs `FOTA Job: <type>` when it finds an update.

1. Verify the update, as described in [Version verification](#version-verification).

To stop offering a release, deactivate it:

```bash
memfault deploy-release \
  --release-version ${RELEASE_VERSION} \
  --cohort ${COHORT} \
  --deactivate
```

Deactivated releases remain in Memfault together with their images. You can delete the releases and images in the Memfault web application.
