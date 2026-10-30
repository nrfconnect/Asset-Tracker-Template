# Connecting

Connecting a device to [nRF Cloud](https://nrfcloud.com) involves three steps:

- **Onboarding**: Registering the device with your nRF Cloud project using the device's unique attestation token. You perform this action in the nRF Cloud portal or over the API. Onboarding both claims the device and adds it to your project in a single step, and the service delivers the credentials automatically.
- **Provisioning**: Securely installing cloud access credentials onto the device. After onboarding, the nRF Provisioning Service delivers these credentials to the device over a Datagram Transport Layer Security (DTLS) protected Constrained Application Protocol (CoAP) channel, and the firmware writes them into the modem's secure storage.
- **Cloud connection**: Establishing a secure CoAP connection from the device to nRF Cloud using the provisioned credentials.

The Asset Tracker Template firmware performs the provisioning and the cloud connection automatically once the device has been onboarded. This page describes how to perform the onboarding, which is the only step that needs a user action, and how to trigger and reset the flow during development.

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

<p>The modem must be offline while the library writes the credentials, because it cannot be connected to the network while data is written to its secure storage.
Therefore, it is normal for LTE to disconnect and reconnect multiple times during provisioning.</p>

<p>The attestation token is different from the JWT. The device uses the attestation token during the initial onboarding to prove its authenticity to nRF Cloud, not during the provisioning protocol itself.</p>

<p>For more information about the provisioning library, see the <a href="https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_provisioning.html">nRF Cloud device provisioning documentation</a>.</p>

</details>

## Connecting a device

Connecting a device needs actions on both the device and the nRF Cloud portal. First, you get the attestation token from the device, then you onboard the device in nRF Cloud, and finally you trigger the device to fetch its credentials.

### Step 1: Obtain the device attestation token

The attestation token uniquely identifies your device and proves its authenticity to nRF Cloud. You can get it in two ways:

- **Automatically on first boot**: When an unprovisioned device boots the Asset Tracker Template firmware for the first time, it prints the attestation token to the serial log. Power the device on, connect a serial terminal at 115200 baud as soon as the USB serial port enumerates, and look for the line labeled `Attestation token:` in the log output. If you missed it, reset the device while the serial terminal is still attached.
- **Manually with a shell command**: If you missed the token on first boot or need to retrieve it again, run the following AT command in the device shell through the serial terminal:

    ```bash
    at at%attesttoken
    ```

    The device prints the token as a string starting with `%ATTESTTOKEN:`. Copy the entire token value, without the `%ATTESTTOKEN:` prefix.

### Step 2: Onboard the device in nRF Cloud

Onboarding claims the device and registers it with your nRF Cloud project in a single step. The service delivers the credentials automatically once you add the device.

1. Log in to the [nRF Cloud](https://nrfcloud.com) portal.
1. Go to **Fleet** > **Devices** and select **Add devices**.
1. When prompted for the device type, select **nRF91 Series**.
1. Paste the attestation token from Step 1 into the **Attestation token** field.
1. Preview and confirm the token to add the device to your fleet.

For the full walkthrough, see [Device onboarding](https://docs.nrfcloud.com/docs/nrfcloud/device-onboarding) in the nRF Cloud documentation. If you previously claimed devices through the legacy Security Services flow, [Secure device onboarding](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding) explains what changed.

### Step 3: Trigger provisioning on the device

After onboarding, the device must contact the provisioning service to fetch its credentials. To trigger the contact immediately, use one of the following methods:

- **Thingy:91 X**: Press and hold **Button 1**, which is the button on the top of the device, for about three seconds.
- **nRF9151 DK**: Press and hold **Button 1** for about three seconds.

The device polls the provisioning service, receives its credentials, and connects to nRF Cloud over CoAP. Provisioning can take up to a minute. Once it completes, the device appears under **Fleet** > **Devices** in the nRF Cloud portal. Each device page shows its provisioning status and the time it last checked in, so you can confirm that the credential exchange completed.

> [!NOTE]
> It is normal for the LTE connection to disconnect and reconnect during provisioning. The modem must go offline temporarily while the library writes the credentials to its secure storage. For more information, see the **What happens during provisioning** section on this page.

<details>
<summary><strong>What can you do after connecting</strong></summary>

<p>After your device is connected to nRF Cloud, you can perform the following:</p>

<p>
<ul>
<li><strong>Monitor device data:</strong> View real-time data from your device, including location, temperature, battery percentage, and other sensor readings in the <a href="https://nrfcloud.com">nRF Cloud</a> portal.</li>
<li><strong>Retrieve data programmatically:</strong>
<ul>
<li>Use the <a href="https://docs.nrfcloud.com/docs/nrfcloud/message-routing">Message Routing Service</a> to automatically forward device messages to your own cloud infrastructure or application endpoints.</li>
<li>Query historical device messages using the <a href="https://docs.nrfcloud.com/api/list-device-messages">List nRF9x Device Messages</a> endpoint of the nRF Cloud API. The request needs an <a href="https://docs.nrfcloud.com/docs/platform/organization-auth-tokens">organization auth token</a> with the <code>device:read</code> permission, and your organization and project slugs, which you find under <strong>Project Settings</strong> &gt; <strong>General</strong>.</li>
</ul>
</li>
</ul>
</p>
<details>
<summary><strong>Retrieve historical messages</strong></summary>

```bash
curl "https://api.nrfcloud.com/v1/organizations/${NRFCLOUD_ORG}/projects/${NRFCLOUD_PROJECT}/messages?deviceId=${DEVICE_ID}&pageLimit=10" \
  -H "Authorization: Bearer ${NRFCLOUD_OAT}" \
  -H "Accept: application/json"
```

The response lists the newest messages first. Filter by `appId`, or limit the time range with `start` and `end`. When more messages are available, pass the returned `pageNextToken` in the next request to fetch the next page.

</details>

<p>
<ul>
<li><strong>Perform firmware updates:</strong> Deploy over-the-air firmware updates to your device. See <a href="https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/fota.html">Firmware updates (FOTA)</a> for detailed instructions on preparing and deploying firmware updates through nRF Cloud.
</ul>
</li>
</p>
</details>

### API alternative

To onboard devices programmatically, use the nRF Cloud bulk onboarding endpoint. It accepts a CSV file with as few as one data row, which holds a `deviceId` and an `attestationToken` column, and it is authenticated with an organization auth token. For the CSV format, the endpoint, and the required credentials, see [Onboarding devices in bulk](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding#onboarding-devices-in-bulk).

## Reprovisioning

Reprovisioning replaces the credentials that are currently stored on the device. For security reasons, reprovision the devices of an end product at a reasonable interval, which depends on the application use case.

To reprovision a device manually, complete the following steps:

1. In the nRF Cloud portal, open the device page and select **Reset** under **Provisioning Status**. The device generates new credentials on its next check-in.
1. Trigger the check-in on the device, so that it re-provisions immediately instead of waiting for the next scheduled poll. Use one of the following methods:

    - **Shell**: Run `att_cloud provision`.
    - **Cloud**: Update the device shadow with `{"desired": {"command": [1, 1]}}`.

For information about sending commands to devices through the REST API, including the command structure and the available command types, see [Sending commands through REST API](configuration.md#sending-commands-through-rest-api) in the configuration documentation.

## Removing a device

If a device is already claimed on another project, you must release it there before you can onboard it to a different project. To remove a device, open the nRF Cloud portal, go to **Fleet** > **Devices**, open the device page, and choose one of the following options:

- **Delete Device** removes the device from the fleet and releases the claim, so another project can onboard it.
- **Deactivate** removes the device from the fleet but keeps the claim, which blocks other projects. You can reactivate the device later.

Both actions permanently remove the device's fleet data, which is the historical messages and the configuration. You must onboard a deleted device again to reconnect it.

For more information, see [Deleting, unclaiming, and deactivating devices](https://docs.nrfcloud.com/docs/nrfcloud/secure-device-onboarding#deleting-unclaiming-and-deactivating-devices) in the nRF Cloud documentation.
