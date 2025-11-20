# Provisioning

Device provisioning establishes credentials for secure communication with nRF Cloud CoAP.

## Manual Provisioning

1. Get the device attestation token:

    ```bash
    at at%attesttoken
    ```

    > [!NOTE]
    > Token is printed automatically on first boot of unprovisioned devices.

1. Log in to the [nRF Cloud](https://nrfcloud.com/#/) portal.
1. Select **Security Services** in the left sidebar.

    A panel opens to the right.

1. Select **Claimed Devices**.
1. Click **Claim Device**

    A pop-up opens.

1. Copy and paste the attestation token into the **Claim token** text box.
1. Set rule to nRF Cloud Onboarding and click **Claim Device**.

    <details>
    <summary><strong>If "nRF Cloud Onboarding" rule is not showing:</strong></summary>

    Create a new rule using the following configuration:

    <img src="../images/claim.png" alt="Claim Device" width="300" />
    </details>

1. Once connected, the device will be available under the **Devices** section in the **Device Management** navigation pane on the left.

### REST API Alternative

You can also use the REST API as an alternative for provisioning by running the following command:

```bash
curl 'https://api.provisioning.nrfcloud.com/v1/claimed-devices' \
-H 'Content-Type: application/json' \
-H 'Authorization: Bearer YOUR_API_TOKEN' \
-d '{"claimToken": "YOUR_DEVICE_ATTESTATION_TOKEN", "tags": ["nrf-cloud-onboarding"]}'
```

## Reprovisioning

To update device credentials:

> In an end product it is recommended to reprovision the device at a reasonable interval depending on the application use case for security reasons.

### Manual

1. Log in to the [nRF Cloud](https://nrfcloud.com/#/) portal.
1. Select **Security Services** in the left sidebar.

   A panel opens to the right.
1. Select **Claimed Devices**.
1. Find the device and click **Add Command**.
1. Select **Cloud Access Key Generation** and click **Create Command**.
1. Trigger on device:

   - **Shell**: `att_cloud_provision now`
   - **Cloud**: Update device shadow with `{"desired": {"command": [1, 1]}}`.

For detailed information on sending commands to devices through REST API, including command structure and available command types, see [Sending commands through REST API](configuration.md#sending-commands-through-rest-api) in the configuration documentation.

### REST API Alternative

You can also use the REST API as an alternative for reprovisioning by running the following command:

```bash
curl 'https://api.provisioning.nrfcloud.com/v1/claimed-devices/YOUR_DEVICE_ID/provisioning' \
-H 'Content-Type: application/json' \
-H 'Authorization: Bearer YOUR_API_TOKEN' \
-d '{"request": {"cloudAccessKeyGeneration": {"secTag": 16842753}}}'
```

For detailed API documentation, see [nRF Cloud REST API](https://api.nrfcloud.com/docs).
