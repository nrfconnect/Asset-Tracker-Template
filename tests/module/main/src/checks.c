/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "app_common.h"
#include "power.h"
#include "network.h"
#include "checks.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(subscriber);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, subscriber, 0);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, subscriber, 0);

LOG_MODULE_REGISTER(trigger_module_checks, 4);

void check_network_event(enum network_msg_type expected_network_type)
{
	int err;
	const struct zbus_channel *chan;
	struct network_msg network_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&subscriber, &chan, &network_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No network event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &NETWORK_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&NETWORK_CHAN), zbus_chan_name(chan));
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_network_type, network_msg.type);
}

void check_power_event(enum power_msg_type expected_power_type)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&subscriber, &chan, &power_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No power event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &POWER_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&POWER_CHAN), zbus_chan_name(chan));
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_power_type, power_msg.type);
}

void check_no_events(uint32_t time_in_seconds)
{
	int err;
	const struct zbus_channel *chan;
	uint8_t *payload = NULL;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&subscriber, &chan, payload, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Unexpected message message on channel %s", zbus_chan_name(chan));
		TEST_FAIL();
	}
}
