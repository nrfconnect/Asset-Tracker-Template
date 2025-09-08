# Provisioning

Device provisioning establishes credentials for secure communication with nRF Cloud CoAP.

## Manual Provisioning

1. Get the device attestation token:

   ```bash
   at at%attesttoken
   ```

   *Note: Token is printed automatically on first boot of unprovisioned devices.*

2. In nRF Cloud: **Security Services** → **Claimed Devices** → **Claim Device**
3. Paste token, set rule to "nRF Cloud Onboarding", click **Claim Device**

### REST API Alternative

```bash
curl 'https://api.provisioning.nrfcloud.com/v1/claimed-devices' \
-H 'Content-Type: application/json' \
-H 'Authorization: Bearer YOUR_API_TOKEN' \
-d '{"claimToken": "YOUR_DEVICE_ATTESTATION_TOKEN", "tags": ["nrf-cloud-onboarding"]}'
```

## Reprovisioning

To update device credentials:

> In an end-product it's recommended to reprovision the device at a reasonable interval depending on the application use case, for security reasons.

### Manual

1. In nRF Cloud: **Security Services** → **Claimed Devices** → find device → **Add Command**
2. Select **Cloud Access Key Generation** → **Create Command**
3. Trigger on device:
   - **Shell**: `att_cloud_provision now`
   - **Cloud**: Update device shadow with `{"desired": {"command": [1, 1]}}`.

*For detailed information on sending commands to devices via REST API, including command structure and available command types, see [Sending commands through REST API](configuration.md#sending-commands-through-rest-api) in the configuration documentation.*

### REST API Alternative

```bash
curl 'https://api.provisioning.nrfcloud.com/v1/claimed-devices/YOUR_DEVICE_ID/provisioning' \
-H 'Content-Type: application/json' \
-H 'Authorization: Bearer YOUR_API_TOKEN' \
-d '{"request": {"cloudAccessKeyGeneration": {"secTag": 16842753}}}'
```

*For detailed API documentation, see [nRF Cloud REST API](https://api.nrfcloud.com/docs).*
