# Provisioning

Device provisioning establishes credentials for secure communication with nRF Cloud CoAP.

<details>
<summary><strong>What happens during provisioning</strong></summary>

The Asset Tracker Template uses the <a href="https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_provisioning.html">nRF Device provisioning</a> library to handle device provisioning automatically. The library provisions the root CA certificate for the provisioning service to the modem during boot if it is not already present. During provisioning, the following steps occur:

<ol>
<li><strong>Secure Connection</strong>: The library establishes a secure DTLS connection to the nRF Cloud CoAP Provisioning Service. The device verifies the server's identity using the root CA certificate.</li>
<li><strong>Device Authentication</strong>: The device authenticates itself using a JSON Web Token (JWT) signed with the modem's factory-provisioned Device Identity private key. This key is securely stored in the modem hardware and cannot be extracted.</li>
<li><strong>Command Retrieval</strong>: After successful authentication, the device requests provisioning commands from the server. These commands typically include cloud access credentials and configuration settings.</li>
<li><strong>Modem Configuration</strong>: To write the received credentials and settings, the library performs the following:</li>

    - Suspends the DTLS session (to maintain the connection state).<br>
    - Temporarily sets the modem offline for credential writing.<br>
    - Writes the credentials to the modem's secure storage.<br>

<li><strong>Result Reporting</strong>: After executing the commands, the library resumes or re-establishes the DTLS connection (if needed), authenticates again with JWT, and reports the results back to the server. Successfully executed commands are removed from the server-side queue.</li>
<li><strong>Validation</strong>: The device uses the newly provisioned credentials to connect to nRF Cloud CoAP services.</li>
</ol>

<p>The modem must be offline during credential writing because the modem cannot be connected to the network while data is being written to its storage area (credential writing).
Therefore it is normal that LTE is disconnected or connected multiple times during provisioning.</p>

<p>The attestation token is different from the JWT - it is used during the initial device claiming process to prove device authenticity to nRF Cloud, not during the provisioning protocol itself.</p>

<p>For more details on the provisioning library, see the <a href="https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_provisioning.html">nRF Cloud device provisioning documentation</a>.</p>

</details>

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

    <details>
    <summary><strong>What can you do after provisioning</strong></summary>

    After your device is provisioned and connected, you can perform the following:

    - **Monitor device data**: View real-time data from your device including location, temperature, battery percentage, and other sensor readings in the nRF Cloud portal.
    - **Retrieve data programmatically**: Use the [Message Routing Service](https://docs.nordicsemi.com/bundle/nrf-cloud/page/Devices/MessagesAndAlerts/MessageRoutingService/ReceivingMessages.html) to automatically forward device messages to your own cloud infrastructure or application endpoints.
    - **Perform firmware updates**: Deploy over-the-air firmware updates to your device. See [Firmware Updates (FOTA)](fota.md) for detailed instructions on preparing and deploying firmware updates through nRF Cloud.

    </details>

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

   - **Shell**: `att_cloud provision`
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
