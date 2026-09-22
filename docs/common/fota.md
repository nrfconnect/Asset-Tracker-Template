# Firmware updates (FOTA)

This guide covers how to perform Firmware Over The Air (FOTA) updates using the [nRF Cloud REST API](https://api.nrfcloud.com/).

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

Complete the following steps when preparing **application** or **bootloader** firmware. For **modem** updates, pre-provisioned modem bundles are already available in nRF Cloud. Use a modem bundle ID from the REST API when creating the FOTA job (see [Complete update workflow](#complete-update-workflow) below).

1. Update the `app/VERSION` file. Increment the appropriate version component.
1. Build the firmware.

    Using the command line:

    ```bash
    west build -p -b thingy91x/nrf9151/ns # Build for the appropriate board
    ```

    Or use the nRF Connect for VS Code. See the [Getting Started](getting_started.md) guide for details on building with the extension.

1. Locate update bundles in the output directory (`app/build/`):

    - `build/dfu_application.zip` - Application firmware update
    - `build/dfu_mcuboot.zip` - Bootloader update

### Version verification

To verify a successful update:

- **Application updates**: Confirm the FOTA job status is `SUCCEEDED` via `GET /fota-jobs/{jobId}`, then check that the device `appVersion` field matches the new version via `GET /devices/{deviceId}`.
- **Modem updates**: Confirm the FOTA job status is `SUCCEEDED`, then check that the device `modemFirmware` field shows the new version.
- **Bootloader updates**: Confirm the FOTA job status is `SUCCEEDED`, then check that the device `bootloaderVersion` field shows the new version.

## Performing FOTA updates

FOTA updates are managed through the nRF Cloud REST API.

After creating and applying a FOTA job in nRF Cloud, the device checks for updates automatically on a configured interval and when triggered by user input (for example, a button press). To trigger a check manually during development, connect to the device shell and run:

```bash
att_fota poll
```

The device must be connected to the network and cloud. If a pending update is found, the FOTA module starts the download automatically.

To trigger an immediate FOTA poll from the device, press and hold **Button 1**. On **Thingy:91 X**, pressing on the top of the case pushes Button 1.

### REST API

#### Setup

```bash
export API_KEY=<your-nrf-cloud-api-key>
export DEVICE_ID=<your-device-id>
```

On-target tests use the same key as `NRFCLOUD_API_KEY`; see [tests/on_target/README.md](../../tests/on_target/README.md).

To obtain your API key:

1. Log in at [nrfcloud.com](https://nrfcloud.com) and open the **legacy app** using the link in the **bottom left corner** of the new UI.
1. Select the correct **team** in the upper right corner.
1. Open the **burger menu** (upper right) → **User Account**.
1. Copy the API key from **Team Details**.
1. Use it as `Authorization: Bearer $API_KEY` in the curl examples below.

See [Managing tokens and keys](https://docs.memfault.com/docs/legacy-nrfcloud/tokens-and-keys) and the [nRF Cloud REST API](https://api.nrfcloud.com/) reference for details.

#### Complete update workflow

1. Create manifest and upload bundle:

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

1. Create and apply FOTA job:

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

1. Monitor job status:

    ```bash
    curl -X GET "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Accept: application/json"
    ```

    Job status values: `QUEUED`, `IN_PROGRESS`, `DOWNLOADING`, `SUCCEEDED`, `FAILED`, `TIMED_OUT`, `CANCELLED`, `REJECTED`

1. Verify the update by querying device information:

    ```bash
    curl -X GET "https://api.nrfcloud.com/v1/devices/${DEVICE_ID}" \
      -H "Authorization: Bearer ${API_KEY}" \
      -H "Accept: application/json"
    ```

    Check the `appVersion` (application updates), `modemFirmware` (modem updates), or `bootloaderVersion` (bootloader updates) field in the response.

#### API reference

**List FOTA jobs**:

```bash
curl -X GET "https://api.nrfcloud.com/v1/fota-jobs" \
  -H "Authorization: Bearer ${API_KEY}"
```

**Cancel FOTA job**:

```bash
curl -X PUT "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/cancel" \
  -H "Authorization: Bearer ${API_KEY}"
```

**Delete FOTA job**:

```bash
curl -X DELETE "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```

**Delete firmware bundle**:

```bash
curl -X DELETE "https://api.nrfcloud.com/v1/firmwares/${BUNDLE_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```
