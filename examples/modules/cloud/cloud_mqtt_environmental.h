/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _CLOUD_MQTT_ENVIRONMENTAL_H_
#define _CLOUD_MQTT_ENVIRONMENTAL_H_

#include "environmental.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send environmental sensor data to cloud via MQTT
 *
 * @param env Pointer to environmental message
 *
 * @return 0 on success, negative error code otherwise
 */
int cloud_mqtt_environmental_send(const struct environmental_msg *env);

#ifdef __cplusplus
}
#endif

#endif /* _CLOUD_MQTT_ENVIRONMENTAL_H_ */
