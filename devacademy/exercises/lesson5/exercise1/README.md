# Lesson 5, Exercise 1: Send Custom Data to nRF Cloud

## Objective

Create a custom alert or sensor reading and send it to nRF Cloud using CoAP.

## Task Description

Implement a temperature alert that:
1. Monitors temperature readings
2. Sends an alert when temperature exceeds threshold
3. Uses custom app ID for the message
4. Displays in nRF Cloud

## Implementation

Edit `src/modules/cloud/cloud.c`:

```c
static int check_temperature_alert(double temperature)
{
    const double THRESHOLD = 25.0;
    
    if (temperature > THRESHOLD) {
        char message[64];
        snprintf(message, sizeof(message),
                "ALERT: Temperature %.2f°C exceeds threshold %.2f°C",
                temperature, THRESHOLD);
        
        int err = nrf_cloud_coap_message_send(
            "TEMP_ALERT",
            message,
            false,  /* Not binary */
            k_uptime_get(),
            true    /* Confirmable */
        );
        
        if (err) {
            LOG_ERR("Failed to send alert: %d", err);
            return err;
        }
        
        LOG_INF("Temperature alert sent");
    }
    
    return 0;
}
```

Call this function when processing environmental data.

## Testing

1. Heat up the device
2. Wait for temperature to exceed threshold
3. Check nRF Cloud for alert message
4. Verify message appears in device data

## Verification

- [ ] Alert sent when threshold exceeded
- [ ] Message appears in nRF Cloud
- [ ] Logs confirm transmission
- [ ] Multiple alerts work correctly

See `solution/` directory for complete implementation.

