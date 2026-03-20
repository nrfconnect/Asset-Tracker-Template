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


ZBUS_CHAN_DECLARE(timer_chan);

ZBUS_MSG_SUBSCRIBER_DEFINE(fota_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(location_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(network_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(power_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(storage_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);
ZBUS_MSG_SUBSCRIBER_DEFINE(timer_subscriber);
ZBUS_CHAN_ADD_OBS(fota_chan, fota_subscriber, 0);
ZBUS_CHAN_ADD_OBS(location_chan, location_subscriber, 0);
ZBUS_CHAN_ADD_OBS(network_chan, network_subscriber, 0);
ZBUS_CHAN_ADD_OBS(power_chan, power_subscriber, 0);
ZBUS_CHAN_ADD_OBS(storage_chan, storage_subscriber, 0);
ZBUS_CHAN_ADD_OBS(cloud_chan, cloud_subscriber, 0);
ZBUS_CHAN_ADD_OBS(timer_chan, timer_subscriber, 0);

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

	if (chan != &location_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&location_chan), zbus_chan_name(chan));
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

	if (chan != &network_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&network_chan), zbus_chan_name(chan));
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

	if (chan != &power_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&power_chan), zbus_chan_name(chan));
		return -3;
	}

	return power_msg.type;
}

int priv_expect_fota_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct fota_msg fota_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(100000));
	if (err == -ENOMSG) {
		LOG_ERR("No FOTA event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &fota_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&fota_chan), zbus_chan_name(chan));
		return -3;
	}

	return fota_msg.type;
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

	if (chan != &storage_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&storage_chan), zbus_chan_name(chan));
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

	if (chan != &cloud_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&cloud_chan), zbus_chan_name(chan));
		return -3;
	}

	return cloud_msg.type;
}

int priv_expect_timer_event(void)
{
	int err;
	const struct zbus_channel *chan;
	struct timer_msg timer_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_MSEC(100));

	err = zbus_sub_wait_msg(&timer_subscriber, &chan, &timer_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		LOG_ERR("No timer event received");
		return -1;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		return -2;
	}

	if (chan != &timer_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&timer_chan), zbus_chan_name(chan));
		return -3;
	}

	return timer_msg.type;
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
	struct fota_msg fota_msg;

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected FOTA event: %d", fota_msg.type);
	TEST_FAIL();
}

static void expect_no_storage_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct storage_msg storage_msg;

	err = zbus_sub_wait_msg(&storage_subscriber, &chan, &storage_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected storage event: %d", storage_msg.type);
	TEST_FAIL();
}

static void expect_no_cloud_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct cloud_msg cloud_msg;

	err = zbus_sub_wait_msg(&cloud_subscriber, &chan, &cloud_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected cloud event: %d", cloud_msg.type);
	TEST_FAIL();
}

static void expect_no_timer_events(void)
{
	int err;
	const struct zbus_channel *chan;
	struct timer_msg timer_msg;

	err = zbus_sub_wait_msg(&timer_subscriber, &chan, &timer_msg, K_MSEC(10000));
	if (err == -ENOMSG) {
		return;
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		TEST_FAIL();

		return;
	}

	LOG_ERR("Received unexpected timer event: %d", timer_msg.type);
	TEST_FAIL();
}

void expect_no_events(uint32_t timeout_sec)
{
	k_sleep(K_SECONDS(timeout_sec));

	expect_no_location_events();
	expect_no_network_events();
	expect_no_power_events();
	expect_no_fota_events();
	expect_no_cloud_events();
	expect_no_storage_events();
	expect_no_timer_events();
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
		struct fota_msg fota_msg;

		err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_NO_WAIT);
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

void purge_timer_events(void)
{
	while (true) {
		int err;
		const struct zbus_channel *chan;
		struct timer_msg timer_msg;

		err = zbus_sub_wait_msg(&timer_subscriber, &chan, &timer_msg, K_NO_WAIT);
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
	purge_timer_events();
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

	if (chan != &location_chan) {
		LOG_ERR("Received message from wrong channel, expected %s, got %s",
			zbus_chan_name(&location_chan), zbus_chan_name(chan));
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
