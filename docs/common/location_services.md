# Location Services

The Asset Tracker uses [nRF Cloud's location services](https://docs.nordicsemi.com/bundle/nrf-cloud/page/LocationServices/LSOverview.html) to provide accurate device positioning through multiple methods. This document explains the available location services and their capabilities.

## Location Methods

The following location methods are supported:

* **GNSS (Global Navigation Satellite System)**

   - Highest accuracy positioning.
   - Best suited for outdoor use.
   - Higher power consumption compared to other methods.

* **Cellular**

   - Uses cellular tower information for positioning.
   - Works indoors.
   - Lower power consumption than GNSS.

* **Wi-Fi**

   - Uses nearby Wi-Fi access points for positioning.
   - Excellent for indoor positioning.
   - Lower power consumption than GNSS.

## Integration with nRF Cloud

The location data is automatically sent to nRF Cloud, allowing you to:

- Track device location in the nRF Cloud portal.
- Analyze location history.
- Monitor location accuracy and methods used.

## Power Considerations

Different location methods have different power consumption profiles:

- GNSS: Highest power consumption, best accuracy.
- Wi-Fi: Medium power consumption, good accuracy indoors.
- Cellular: Lowest additional power consumption (modem already active), moderate accuracy.

Each method offers a different balance between power consumption and accuracy, allowing you to choose the most appropriate method for your use case.
