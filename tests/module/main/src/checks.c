/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "app_common.h"
#include "location.h"
#include "power.h"
#include "network.h"
#include "checks.h"
#include "fota.h"
#include "storage.h"
#include "cloud.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(fota_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(location_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(network_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(storage_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);
ZBUS_CHAN_ADD_OBS(FOTA_CHAN, fota_subscriber, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, location_subscriber, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, network_subscriber, 0);
ZBUS_CHAN_ADD_OBS(POWER_CHAN, power_subscriber, 0);
ZBUS_CHAN_ADD_OBS(STORAGE_CHAN, storage_subscriber, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, cloud_subscriber, 0);

LOG_MODULE_REGISTER(main_module_checks, 1);

int priv_expect_location_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct location_msg location_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&location_subscriber, &chan, &location_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No location event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &LOCATION_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&LOCATION_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return location_msg.type;
}

int priv_expect_network_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct network_msg network_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&network_subscriber, &chan, &network_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No network event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &NETWORK_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&NETWORK_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return network_msg.type;
}

int priv_expect_power_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No power event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &POWER_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&POWER_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return power_msg.type;
}

int priv_expect_fota_event(void)
{
	int err;
	const struct zbus_channel *chan;
	enum fota_msg_type fota_msg_type;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg_type, K_MSEC(100000));
	if (err == -ENOMSG) {
		LOG_ERR("No FOTA event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &FOTA_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&FOTA_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return fota_msg_type;
}

int priv_expect_storage_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct storage_msg storage_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&storage_subscriber, &chan, &storage_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No storage event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &STORAGE_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&STORAGE_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return storage_msg.type;
}

int priv_expect_cloud_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct cloud_msg cloud_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&cloud_subscriber, &chan, &cloud_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No cloud event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &CLOUD_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&CLOUD_CHAN), zbus_chan_name(chan));
		return -3;
	}

	return cloud_msg.type;
}

static void expect_no_location_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct location_msg location_msg;

	err = zbus_sub_wait_msg(&location_subscriber, &chan, &location_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected location event: %d", location_msg.type);
	TEST_FAIL();
}

static void expect_no_network_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct network_msg network_msg;

	err = zbus_sub_wait_msg(&network_subscriber, &chan, &network_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected network event: %d", network_msg.type);
	TEST_FAIL();
}

static void expect_no_power_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct power_msg power_msg;

	err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected power event: %d", power_msg.type);
	TEST_FAIL();
}

static void expect_no_fota_events(void)
{
	int err;
	const struct zbus_channel *chan;
	enum fota_msg_type fota_msg_type;

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg_type, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected FOTA event: %d", fota_msg_type);
	TEST_FAIL();
}

void expect_no_events(uint32_t timeout_sec)
{
	k_sleep(K_SECONDS(timeout_sec));

	expect_no_location_events();
	expect_no_network_events();
	expect_no_power_events();
	expect_no_fota_events();
}

void purge_location_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct location_msg location_msg;

		err = zbus_sub_wait_msg(&location_subscriber, &chan, &location_msg, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_network_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct network_msg network_msg;

		err = zbus_sub_wait_msg(&network_subscriber, &chan, &network_msg, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_power_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct power_msg power_msg;

		err = zbus_sub_wait_msg(&power_subscriber, &chan, &power_msg, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_fota_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		enum fota_msg_type fota_msg_type;

		err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg_type, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_storage_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct storage_msg storage_msg;

		err = zbus_sub_wait_msg(&storage_subscriber, &chan, &storage_msg, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_cloud_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct cloud_msg cloud_msg;

		err = zbus_sub_wait_msg(&cloud_subscriber, &chan, &cloud_msg, K_NO_WAIT);
		if (err == -ENOMSG) {
			break;
		} else if (err) {
			LOG_ERR("zbus_sub_wait, error: %d", err);
			TEST_FAIL();

			return;
		}
	}
}

void purge_all_events(void)
{
	purge_location_events();
	purge_network_events();
	purge_power_events();
	purge_fota_events();
	purge_storage_events();
	purge_cloud_events();
}

int wait_for_location_event(enum location_msg_type expected_type, uint32_t timeout_sec)
{
	int err;
	const struct zbus_channel *chan;
	struct location_msg location_msg;
	uint32_t start_time = k_uptime_seconds();
	uint32_t elapsed_time;

	err = zbus_sub_wait_msg(&location_subscriber, &chan, &location_msg,
				K_SECONDS(timeout_sec));
	if (err == -ENOMSG) {
		return -ENOMSG;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);

		return err;
	}

	if (chan != &LOCATION_CHAN) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&LOCATION_CHAN), zbus_chan_name(chan));
		return -EINVAL;
	}

	if (location_msg.type != expected_type) {
		LOG_ERR("Received unexpected location event: %d", location_msg.type);
		return -EINVAL;
	}

	elapsed_time = k_uptime_seconds() - start_time;

	LOG_DBG("Received expected location event: %d, wait: %d", location_msg.type, elapsed_time);

	return elapsed_time;
}
