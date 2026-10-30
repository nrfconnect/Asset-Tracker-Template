# Firmware updates (FOTA)

This guide covers how to perform firmware over-the-air (FOTA) updates using the [nRF Cloud REST API](https://api.nrfcloud.com).

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

Complete the following steps when preparing application or bootloader firmware. For modem updates, pre-provisioned modem bundles are already available in nRF Cloud. Use a modem bundle ID from the REST API when creating the FOTA job, as described in [Complete update workflow](#complete-update-workflow).

1. Update the `app/VERSION` file. Increment the appropriate version component.
1. Build the firmware.

    From the command line, run the following command:

    ```bash
    west build -p -b thingy91x/nrf9151/ns # Build for the appropriate board
    ```

    You can also use nRF Connect for VS Code. For information about building with the extension, see [Getting started](getting_started.md).

1. Locate the update bundles in the `app/build/` output directory:

    - `build/dfu_application.zip`: The application firmware update.
    - `build/dfu_mcuboot.zip`: The bootloader update.

### Version verification

To verify that an update succeeded, use one of the following methods:

- **Application updates**: Confirm the FOTA job status is `SUCCEEDED` through `GET /fota-jobs/{jobId}`, then check that the device `appVersion` field matches the new version through `GET /devices/{deviceId}`.
- **Modem updates**: Confirm the FOTA job status is `SUCCEEDED`, then check that the device `modemFirmware` field shows the new version.
- **Bootloader updates**: Confirm the FOTA job status is `SUCCEEDED`, then check that the device `bootloaderVersion` field shows the new version.

## Performing FOTA updates

You manage FOTA updates through the nRF Cloud REST API.

After you create and apply a FOTA job in nRF Cloud, the device checks for updates automatically at a configured interval and when the user triggers a check, for example with a button press. To trigger a check manually during development, connect to the device shell and run the following command:

```bash
att_fota poll
```

The device must be connected to the network and the cloud. If the device finds a pending update, the FOTA module starts the download automatically.

To trigger an immediate FOTA poll from the device, press and hold **Button 1**. On the Thingy:91 X, pressing on the top of the case pushes Button 1.

### REST API

The following sections describe how to set up the REST API access, run the complete update workflow, and use the individual API endpoints.

#### Setup

Set the following environment variables:

```bash
export API_KEY=<your-nrf-cloud-api-key>
export DEVICE_ID=<your-device-id>
```

On-target tests use the same key as `NRFCLOUD_API_KEY`. For more information, see [tests/on_target/README.md](../../tests/on_target/README.md).

The FOTA endpoints use the legacy nRF Cloud API key. To get your API key, complete the following steps:

1. Log in at [nRF Cloud](https://nrfcloud.com).
1. Go to `https://app.memfault.com/organizations/<org_slug>/nrfcloud-api-key`, where `<org_slug>` is your organization slug. You find the slug under **Project Settings** > **General**.
1. Copy the API key.
1. Use the key as `Authorization: Bearer $API_KEY` in the following curl examples.

For more information, see the [nRF Cloud REST API](https://api.nrfcloud.com) reference.

#### Complete update workflow

To deploy a firmware update, complete the following steps:

1. Create the manifest and upload the bundle:

    ```bash
    # Set path to your application binary
    export BIN_FILE="build/app/zephyr/zephyr.signed.bin"
    export FW_VERSION="1.2.3"

    # Create manifest.json with firmware details
    cat > manifest.json << EOF
    {
        "name": "My Firmware",
        "description": "Firmware description",
        "fwversion": "${FW_VERSION}",
        "format-version": 1,
        "files": [
            {
                "file": "$(basename ${BIN_FILE})",
                "type": "application",
                "size": $(stat -c%s ${BIN_FILE} 2>/dev/null || stat -f%z ${BIN_FILE})
            }
        ]
    }
    EOF

    # Create zip containing firmware and manifest
    zip -j firmware.zip ${BIN_FILE} manifest.json

    # Upload to nRF Cloud and extract bundle ID
    UPLOAD_RESPONSE=$(curl -X POST "https://api.nrfcloud.com/v1/firmwares" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Content-Type: application/zip" \
      --data-binary @firmware.zip)

    # Extract the bundle ID from the response (UUID from the URI path)
    export BUNDLE_ID=$(echo $UPLOAD_RESPONSE | jq -r '.uris[0]' | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
    ```

1. Create and apply the FOTA job:

    ```bash
    # Create job
    JOB_RESPONSE=$(curl -X POST "https://api.nrfcloud.com/v1/fota-jobs" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Content-Type: application/json" \
      -d "{\"deviceIds\": [\"${DEVICE_ID}\"], \"bundleId\": \"${BUNDLE_ID}\"}")

    export JOB_ID=$(echo $JOB_RESPONSE | jq -r '.jobId')

    # Apply job
    curl -X POST "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/apply" \
      -H "Authorization: Bearer ${API_KEY}"
    ```

1. Monitor the job status:

    ```bash
    curl -X GET "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Accept: application/json"
    ```

    The job status is `QUEUED`, `IN_PROGRESS`, `DOWNLOADING`, `SUCCEEDED`, `FAILED`, `TIMED_OUT`, `CANCELLED`, or `REJECTED`.

1. Verify the update by querying the device information:

    ```bash
    curl -X GET "https://api.nrfcloud.com/v1/devices/${DEVICE_ID}" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Accept: application/json"
    ```

    In the response, check the `appVersion` field for application updates, the `modemFirmware` field for modem updates, or the `bootloaderVersion` field for bootloader updates.

#### API reference

To list the FOTA jobs, run the following command:

```bash
curl -X GET "https://api.nrfcloud.com/v1/fota-jobs" \
  -H "Authorization: Bearer ${API_KEY}"
```

To cancel a FOTA job, run the following command:

```bash
curl -X PUT "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/cancel" \
  -H "Authorization: Bearer ${API_KEY}"
```

To delete a FOTA job, run the following command:

```bash
curl -X DELETE "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```

To delete a firmware bundle, run the following command:

```bash
curl -X DELETE "https://api.nrfcloud.com/v1/firmwares/${BUNDLE_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```
