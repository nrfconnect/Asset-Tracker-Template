# Provisioning

The firmware supports the nRF Cloud Provisioning service which allows it to be provisioned at any time. All that is needed is to claim the device on nRF Cloud using its
attestation token and wait for the device to connect to the provisioning service to receive its credentials that it will use to connect to the nRF Cloud CoAP instance.

The easiest way to onboard is to use nRF Connect for Desktop Quickstart and follow the steps you are guided through in the application.
Alternatively you can follow these steps:

1. Boot the device and wait for it to print its attestation token.
   This happens when it identifies that it is not connected to any nRF Cloud CoAP instance and it has not been claimed by any account.
2. Log in to your nRF Cloud account.
3. Click on Security Services -> Claimed Devices -> Claim Device
4. Paste Attestation token and set the provisioning rule to "nRF Cloud Onboarding"
5. Click on Claim Device and wait for the device to provision and connect to the nRF Cloud CoaP instance.

It is also possible to claim a device programatically using REST calls, documentation for this can be found in the following section:

If this completes successfully your device is registered to nRF Cloud.
For information on how to reprovision an already provisioned device, see the [Reprovisioning].

# Reprovisioning

For security reasons it might be desired to swap out the credentials used by the device.
This can be done by reprovisioning the device, issuing new provisioning commands, and making the device fetch and handle these commands.
After processing, the device will connect to nRF Cloud CoAP again, with new credentials.
This can be done mainly in two ways, manually via the nRF Cloud graphical UI or programatically using REST calls.

## Manual

1. Log in to your nRF Cloud account.
2. Click on Security Services -> Claimed Devices
3. Find the device that you want to reprovision and click on Add command.
4. Click on Cloud Access Key Generation and select the correct security tag used. If you havent changed anything, the default option should work.
5. Click on Create Command.

Now you need to make the device check for the new provisioning command, this can be done by issuing a command to the device via the device shadow
or via shell.

The shell command you need to call is att_cloud_provision now.
This will trigger the provisioning process on the device.

To trigger via cloud the shadow of the device needs to be updated to contain the cmd entry:

{
  "desired": {
    "command": [1, 1]
  }
}

The command entry needs a command type and ID "command": [<cmd>, <id>]. The command type for provisioning is "1". Make sure that the ID of the command is
updated if multiple commands in succession is wanted. The device will not execute two commands after each other if the ID is the same.

Updating the device configuration is also documented here, [Configuration through REST API]

## Programatically