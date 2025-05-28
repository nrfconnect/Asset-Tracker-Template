# Instructions on how to manage nRFCloud FOTA through REST calls

This document describes how to manage FOTA (Firmware Over The Air) updates using nRF Cloud's REST API. All examples use curl commands and assume you have set your API key in an environment variable:

```bash
export API_KEY="your-nrf-cloud-api-key"
```

You can find your API_KEY in "User Account" settings in your nRF Cloud profile.

For reference, see [nRF Cloud REST API](https://api.nrfcloud.com/)

## Upload Application Firmware

To upload an application firmware to nRF Cloud, you will need your compiled application binary file (typically found in the build/app/zephyr directory after building the project).

```bash
# Set path to your application binary
export BIN_FILE="build/app/zephyr/zephyr.signed.bin"  # Or path to your compiled binary

# Create manifest.json with firmware details
cat > manifest.json << EOF
{
    "name": "My Firmware",
    "description": "Firmware description",
    "fwversion": "1.0.0",
    "format-version": 1,
    "files": [
        {
            "file": "$(basename ${BIN_FILE})",
            "type": "application",
            "size": $(stat -f%z ${BIN_FILE})
        }
    ]
}
EOF

# Create zip containing firmware and manifest
zip -j firmware.zip ${BIN_FILE} manifest.json

# Upload to nRF Cloud
curl -X POST "https://api.nrfcloud.com/v1/firmwares" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/zip" \
  --data-binary @firmware.zip

# The response will include URIs like:
# https://firmware.nrfcloud.com/[bundle-id]/APP...
# Extract the bundle ID from the URI path for use in creating FOTA jobs
```

The bundle ID will be needed when creating FOTA jobs. For application firmware, it can be extracted from the URI path after `firmware.nrfcloud.com/`.

## List FOTA Jobs
```bash
curl -X GET "https://api.nrfcloud.com/v1/fota-jobs" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

## Create FOTA Job
```bash
curl -X POST "https://api.nrfcloud.com/v1/fota-jobs" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "deviceIds": ["device-id"],
    "bundleId": "bundle-id"
  }'
```

The response will include a `jobId` that can be used to track the job status.

## Apply FOTA Job
```bash
curl -X POST "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/apply" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

## Check FOTA Status
```bash
curl -X GET "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

The status will be one of: "QUEUED", "IN_PROGRESS", "FAILED", "SUCCEEDED", "TIMED_OUT", "CANCELLED", "REJECTED", "DOWNLOADING".

## Update Job Execution State
```bash
curl -X PATCH "https://api.nrfcloud.com/v1/fota-job-executions/${DEVICE_ID}/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "status": "CANCELLED"
  }'
```

Valid status values are: "QUEUED", "IN_PROGRESS", "FAILED", "SUCCEEDED", "TIMED_OUT", "CANCELLED", "REJECTED", "DOWNLOADING".

## Cancel FOTA Job
```bash
curl -X PUT "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/cancel" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

## Delete FOTA Job
```bash
curl -X DELETE "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```

## Delete Firmware Bundle
```bash
curl -X DELETE "https://api.nrfcloud.com/v1/firmwares/${BUNDLE_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```

## Example Workflow

The following is a complete example workflow for performing a FOTA update:

```bash
# 1. Set up environment variables
export API_KEY="your-nrf-cloud-api-key"
export BIN_FILE="build/zephyr/app_signed.bin"  # Your compiled application binary
export DEVICE_ID="your-device-id"

# 2. Create manifest and upload firmware
cat > manifest.json << EOF
{
    "name": "Application Update",
    "description": "New firmware version",
    "fwversion": "1.0.0",
    "format-version": 1,
    "files": [
        {
            "file": "$(basename ${BIN_FILE})",
            "type": "application",
            "size": $(stat -f%z ${BIN_FILE})
        }
    ]
}
EOF

zip -j firmware.zip ${BIN_FILE} manifest.json

curl -X POST "https://api.nrfcloud.com/v1/firmwares" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/zip" \
  --data-binary @firmware.zip

export BUNDLE_ID=<your_bundle_id>

# 3. Create and apply FOTA job
curl -X POST "https://api.nrfcloud.com/v1/fota-jobs" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d "{\"deviceIds\": [\"${DEVICE_ID}\"], \"bundleId\": \"${BUNDLE_ID}\"}" \
  | jq -r '.jobId'

export JOB_ID=<your_job_id>

curl -X POST "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}/apply" \
  -H "Authorization: Bearer ${API_KEY}"

# 4. Monitor job status
curl -X GET "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"

# 5. Clean up (optional)
# Delete job after completion
curl -X DELETE "https://api.nrfcloud.com/v1/fota-jobs/${JOB_ID}" \
  -H "Authorization: Bearer ${API_KEY}"

# Delete firmware bundle if no longer needed
curl -X DELETE "https://api.nrfcloud.com/v1/firmwares/${BUNDLE_ID}" \
  -H "Authorization: Bearer ${API_KEY}"
```
