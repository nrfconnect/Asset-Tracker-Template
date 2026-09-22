# Connecting

Connecting a device to [nRF Cloud](https://nrfcloud.com) involves three steps:

- **Onboarding** - Registering the device with your nRF Cloud project using the device's unique **attestation token**. This is an action performed in the nRF Cloud portal (or over the API). Onboarding both claims the device and adds it to your project in a single step; credentials are delivered automatically.
- **Provisioning** - Securely installing cloud access credentials onto the device. After onboarding, the nRF Provisioning Service delivers these credentials to the device over a DTLS-protected CoAP channel, and the firmware writes them into the modem's secure storage.
- **Cloud connection** - Establishing a secure CoAP connection from the device to nRF Cloud using the provisioned credentials.

The Asset Tracker Template firmware performs provisioning and cloud connection automatically once the device has been onboarded. This page describes how to perform the required user actions (onboarding) and how to trigger and reset the flow during development.

<details>
<summary><strong>What happens during provisioning</strong></summary>

The Asset Tracker Template uses the <a href="https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_provisioning.html">nRF Device provisioning</a> library to handle device provisioning automatically. The library provisions the root CA certificate for the provisioning service to the modem during boot if it is not already present. During provisioning, the following steps occur:

<ol>
<li><strong>Secure Connection</strong>: The library establishes a secure DTLS connection to the nRF Cloud CoAP Provisioning Service. The device verifies the server's identity using the root CA certificate.</li>
<li><strong>Device Authentication</strong>: The device authenticates itself using a JSON Web Token (JWT) signed with the modem's factory-provisioned Device Identity private key. This key is securely stored in the modem hardware and cannot be extracted.</li>
<li><strong>Command Retrieval</strong>: After successful authentication, the device requests provisioning commands from the server. These commands typically include cloud access credentials and configuration settings.</li>
<li><strong>Modem Configuration</strong>: To write the received credentials and settings, the library performs the following:<br>

   - Suspends the DTLS session (to maintain the connection state).<br>
   - Temporarily sets the modem offline.<br>
   - Writes the credentials to the modem's secure storage.<br>
</li>
<li><strong>Result Reporting</strong>: After executing the commands, the library resumes or re-establishes the DTLS connection (if needed), authenticates again with JWT, and reports the results back to the server. Successfully executed commands are removed from the server-side queue.</li>
<li><strong>Validation</strong>: The device uses the newly provisioned credentials to connect to nRF Cloud CoAP services.</li>
</ol>

<p>The modem must be offline during credential writing because it cannot be connected to the network while data is being written to its secure storage.
Therefore, it is normal for LTE to disconnect and reconnect multiple times during provisioning.</p>

<p>The attestation token is different from the JWT, it is used during the initial device onboarding process to prove device authenticity to nRF Cloud, not during the provisioning protocol itself.</p>

<p>For more details on the provisioning library, see the <a href="https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_provisioning.html">nRF Cloud device provisioning documentation</a>.</p>

</details>

## Connecting a device

Connecting a device requires actions on both the device and the nRF Cloud portal. First, you obtain the attestation token from the device, then you onboard the device in nRF Cloud, and finally you trigger the device to fetch its credentials.

### Step 1: Obtain the device attestation token

The **attestation token** uniquely identifies your device and proves its authenticity to nRF Cloud. You can obtain it in two ways:

- **Automatic (on first boot)**: When an unprovisioned device boots the Asset Tracker Template firmware for the first time, it prints the attestation token to the serial log. Power the device on, connect a serial terminal (115200 baud) as soon as the USB serial port enumerates, and look for the line labelled `Attestation token:` in the log output. If you missed it, reset the device while the serial terminal is still attached.

- **Manual (using a shell command)**: If you missed the token on first boot or need to retrieve it again, run the following AT command in the device shell through the serial terminal:

    ```bash
    at at%attesttoken
    ```

    The token is printed as a string starting with `%ATTESTTOKEN:`. Copy the entire token value (excluding the `%ATTESTTOKEN:` prefix).

### Step 2: Onboard the device in nRF Cloud

Onboarding claims the device and registers it with your nRF Cloud project in a single step. Credentials are delivered automatically once the device is added.

1. Log in to the [nRF Cloud](https://nrfcloud.com) portal.
1. Go to **Fleet** → **Devices** and select **Add devices**.
1. When prompted for the device type, select **nRF91 Series**.
1. Paste the attestation token from Step 1 into the **Attestation token** field.
1. Preview and confirm the token to add the device to your fleet.

For the full walkthrough, see [Device Onboarding](https://docs.nrfcloud.com/docs/nrfcloud/device-onboarding) in the nRF Cloud documentation. If you previously claimed devices through the legacy Security Services flow, [Secure Device Onboarding](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding) explains what changed.

### Step 3: Trigger provisioning on the device

After onboarding, the device must contact the provisioning service to fetch its credentials. To trigger this immediately:

- **Thingy:91 X**: Press and hold the button on the top of the device (**Button 1**) for about three seconds.
- **nRF9151 DK**: Press and hold **Button 1** for about three seconds.

The device will poll the provisioning service, receive its credentials, and connect to nRF Cloud over CoAP. Provisioning can take up to a minute. Once complete, the device appears under **Fleet** → **Devices** in the nRF Cloud portal. Each device page shows its provisioning status and the time it last checked in, so you can confirm the credential exchange completed.

> [!NOTE]
> It is normal for the LTE connection to disconnect and reconnect during provisioning. The modem must go offline temporarily while credentials are written to its secure storage. See the "What happens during provisioning" section at the top of this page for details.

<details>
<summary><strong>What can you do after connecting</strong></summary>

<p>After your device is connected to nRF Cloud, you can perform the following:</p>

<p>
<ul>
<li><strong>Monitor device data:</strong> View real-time data from your device, including location, temperature, battery percentage, and other sensor readings in the <a href="https://nrfcloud.com">nRF Cloud</a> portal.</li>
<li><strong>Retrieve data programmatically:</strong>
<ul>
<li>Use the <a href="https://docs.nordicsemi.com/bundle/nrf-cloud/page/Devices/MessagesAndAlerts/MessageRoutingService/ReceivingMessages.html">Message Routing Service</a> to automatically forward device messages to your own cloud infrastructure or application endpoints.</li>
<li>Query historical device messages using the REST API. For complete endpoint details, see the <a href="https://api.nrfcloud.com/">REST API documentation</a> and <a href="https://api.nrfcloud.com/v1/openapi.json">OpenAPI specification</a>.</li>
</ul>
</li>
</ul>
</p>
<details>
<summary><strong>Retrieve historical messages</strong></summary>

```bash
curl -X GET "https://api.nrfcloud.com/v1/messages?device_id=${DEVICE_ID}&pageLimit=10" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Accept: application/json"
```

</details>

<p>
<ul>
<li><strong>Perform firmware updates:</strong> Deploy over-the-air firmware updates to your device. See <a href="https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/fota.html">Firmware Updates (FOTA)</a> for detailed instructions on preparing and deploying firmware updates through nRF Cloud.
</ul>
</li>
</p>
</details>

### API alternative

To onboard devices programmatically, use the nRF Cloud bulk onboarding endpoint. It accepts a CSV with as few as one data row (a `deviceId` and `attestationToken` column) and is authenticated with an organization auth token. See [Onboarding devices in bulk](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding#onboarding-devices-in-bulk) for the CSV format, endpoint, and required credentials.

## Reprovisioning

Reprovisioning replaces the credentials currently stored on the device. In an end product, it is recommended to reprovision devices at a reasonable interval (depending on the application use case) for security reasons.

### Manual

1. In the nRF Cloud portal, open the device page and select **Reset** under **Provisioning Status**. The device generates new credentials on its next check-in.

1. Trigger the check-in on the device so it re-provisions immediately instead of waiting for the next scheduled poll:

    - **Shell**: `att_cloud provision`
    - **Cloud**: Update device shadow with `{"desired": {"command": [1, 1]}}`.

For detailed information on sending commands to devices through REST API, including command structure and available command types, see [Sending commands through REST API](configuration.md#sending-commands-through-rest-api) in the configuration documentation.

## Removing a device

If a device is already claimed on another project, it must be released there before it can be onboarded to a different project. To remove a device, open the nRF Cloud portal, go to **Fleet** → **Devices**, open the device page, and choose one of the following:

- **Delete Device** removes the device from the fleet and releases the claim, so another project can onboard it.
- **Deactivate** removes the device from the fleet but keeps the claim, blocking other projects; it can be reactivated later.

Both actions permanently remove the device's fleet data (historical messages and configuration). A deleted device must be onboarded again to reconnect.

For details, see [Deleting, unclaiming, and deactivating devices](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding#deleting-unclaiming-and-deactivating-devices) in the nRF Cloud documentation.
