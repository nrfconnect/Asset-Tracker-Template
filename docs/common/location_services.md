# Location services

The Asset Tracker Template uses [nRF Cloud location services](https://docs.nordicsemi.com/bundle/nrf-cloud/page/LocationServices/LSOverview.html) to provide accurate device positioning through multiple methods. This page explains the available location services and their capabilities.

## Location methods

The template supports the following location methods:

- **Global Navigation Satellite System (GNSS)**:

    - Highest accuracy positioning.
    - Best suited for outdoor use.
    - Higher power consumption compared to the other methods.

- **Cellular**:

    - Uses cellular tower information for positioning.
    - Works indoors.
    - Lower power consumption than GNSS.

- **Wi-Fi**:

    - Uses nearby Wi-Fi access points for positioning.
    - Excellent for indoor positioning.
    - Lower power consumption than GNSS.

## Integration with nRF Cloud

> [!NOTE]
> nRF Cloud is transitioning to a Memfault-integrated experience. The portal features described in this section are in the legacy nRF Cloud portal. After logging in at [nRF Cloud](https://nrfcloud.com), open the legacy app using the link in the bottom left corner of the new UI.

The template sends the location data to nRF Cloud automatically, which lets you perform the following tasks:

- Track the device location in the nRF Cloud portal.
- Analyze the location history.
- Monitor the location accuracy and the methods used.

## Power considerations

The location methods have the following power consumption profiles:

- **GNSS**: Highest power consumption, best accuracy.
- **Wi-Fi**: Medium power consumption, good accuracy indoors.
- **Cellular**: Lowest additional power consumption, because the modem is already active, and moderate accuracy.

Each method offers a different balance between power consumption and accuracy, which lets you choose the most appropriate method for your use case.
