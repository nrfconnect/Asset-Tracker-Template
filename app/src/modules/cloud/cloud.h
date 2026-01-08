/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_H_
#define _CLOUD_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	CLOUD_CHAN
);

struct cloud_payload {
	/** Buffer containing the payload data to be sent to the cloud */
	uint8_t buffer[CONFIG_APP_CLOUD_PAYLOAD_BUFFER_MAX_SIZE];

	/** Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

struct cloud_shadow_response {
	/** Buffer containing the shadow response data received from the cloud */
	uint8_t buffer[CONFIG_APP_CLOUD_SHADOW_RESPONSE_BUFFER_MAX_SIZE];

	/** Length of the data stored inside the buffer */
	size_t buffer_data_len;
};

enum cloud_msg_type {
	/* Output message types */

	/* The device is disconnected from the cloud */
	CLOUD_DISCONNECTED = 0x1,

	/* The device is connected to the cloud and ready to send/receive data */
	CLOUD_CONNECTED,

	/* Response message containing the desired section of the device shadow. The shadow data
	 * is CBOR-encoded and can be found in the .response.buffer field with length specified
	 * in .response.buffer_data_len. This is a response to CLOUD_SHADOW_GET_DESIRED.
	 */
	CLOUD_SHADOW_RESPONSE_DESIRED,

	/* Response message containing the delta section of the device shadow, which represents
	 * the difference between the reported and desired states. The shadow data is CBOR-encoded
	 * and can be found in the .response.buffer field with length specified in
	 * .response.buffer_data_len. This is a response to CLOUD_SHADOW_GET_DELTA.
	 */
	CLOUD_SHADOW_RESPONSE_DELTA,

	/* Response message indicating that the desired section of the device shadow is empty.
	 * This occurs when querying the shadow and no desired state has been set by the cloud.
	 * No payload is associated with this message. This is a response to
	 * CLOUD_SHADOW_GET_DESIRED.
	 */
	CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED,

	/* Response message indicating that the delta section of the device shadow is empty.
	 * This occurs when there are no differences between the reported and desired states.
	 * No payload is associated with this message. This is a response to
	 * CLOUD_SHADOW_GET_DELTA.
	 */
	CLOUD_SHADOW_RESPONSE_EMPTY_DELTA,

	/* Input message types */

	/* Request to send a JSON payload to the cloud. The payload data is located in the
	 * .payload.buffer field with length specified in .payload.buffer_data_len.
	 */
	CLOUD_PAYLOAD_JSON,

	/* Request to report the current device configuration to the device shadow. The payload
	 * contains CBOR-encoded configuration parameters (update_interval, sample_interval,
	 * buffer_mode) that will be sent to the shadow's reported section. The data is located
	 * in the .payload.buffer field with length specified in .payload.buffer_data_len.
	 *
	 * The event can also carry command parameters to be reported to the cloud to acknowledge
	 * command execution. The command type and parameters are encoded in the payload buffer.
	 */
	CLOUD_SHADOW_UPDATE_REPORTED,

	/* Request the desired section of the device shadow from the cloud. This retrieves the
	 * target configuration that the cloud wants the device to use. The response is sent as
	 * either CLOUD_SHADOW_RESPONSE_DESIRED or CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED.
	 */
	CLOUD_SHADOW_GET_DESIRED,

	/* Request the delta section of the device shadow from the cloud. This retrieves the
	 * difference between the device's reported state and the cloud's desired state. The
	 * response is sent as either CLOUD_SHADOW_RESPONSE_DELTA or
	 * CLOUD_SHADOW_RESPONSE_EMPTY_DELTA.
	 */
	CLOUD_SHADOW_GET_DELTA,

	/* Request to initiate device provisioning. This puts the cloud module into provisioning
	 * mode where it connects to the nRF Cloud provisioning endpoint to check for provisioning
	 * commands. Use this to onboard new devices using their attestation token, or to
	 * reprovision devices with new credentials when the old ones expire or need rotation.
	 */
	CLOUD_PROVISIONING_REQUEST,
};

struct cloud_msg {
	enum cloud_msg_type type;
	union  {
		/** Contains payload data to be sent to the cloud.
		 *  payload is valid for CLOUD_PAYLOAD_JSON and CLOUD_SHADOW_UPDATE_REPORTED events.
		 */
		struct cloud_payload payload;

		/** Contains shadow response data received from the cloud.
		 *  response is valid for CLOUD_SHADOW_RESPONSE_DESIRED and
		 *  CLOUD_SHADOW_RESPONSE_DELTA events.
		 */
		struct cloud_shadow_response response;
	};
};

#define MSG_TO_CLOUD_MSG_PTR(_msg)	((const struct cloud_msg *)_msg)

#define UNIX_TIME_MS_2026_01_01 1767222000000LL

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_H_ */
