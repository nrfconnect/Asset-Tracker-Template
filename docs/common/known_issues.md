# Known Issues

This page documents known issues and limitations across different components of the Asset Tracker Template ecosystem.

## nRF Cloud

### Historical data view shows receivedAt timestamp instead of sample timestamp (LRCS-415)

**Issue:** The nRF Cloud historical data view displays the `receivedAt` timestamp (when the cloud received the message) instead of the sample timestamp from the message payload. This causes batched samples with different timestamps to appear clustered at the transmission time.

**Workaround:**
- Use the live view (timestamps are correct there)
- Use the [REST API](https://api.nrfcloud.com) to retrieve device messages (`GET /v1/messages?device_id={DEVICE_ID}`) and parse timestamps from the message payload
- Use the [Message Routing Service](https://docs.nordicsemi.com/bundle/nrf-cloud/page/Devices/MessagesAndAlerts/MessageRoutingService/ReceivingMessages.html) to automatically forward device messages to your own infrastructure in real-time.
- For production deployments, it's recommended to rely on programmatic data export (REST API or Message Routing Service) rather than the web portal for accurate timestamp handling and automated data processing.
