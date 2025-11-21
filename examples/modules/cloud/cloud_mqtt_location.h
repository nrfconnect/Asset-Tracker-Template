/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_MQTT_LOCATION_H_
#define _CLOUD_MQTT_LOCATION_H_

#include "location.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle location messages and forward GNSS data to cloud via MQTT
 *
 * @param msg Pointer to location message
 */
void cloud_mqtt_location_handle_message(const struct location_msg *msg);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_MQTT_LOCATION_H_ */
