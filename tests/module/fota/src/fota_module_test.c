/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <net/nrf_cloud_fota_poll.h>

#include "app_common.h"
#include "fota.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, nrf_cloud_fota_poll_init, struct nrf_cloud_fota_poll_ctx *);
FAKE_VALUE_FUNC(int, nrf_cloud_fota_poll_process_pending, struct nrf_cloud_fota_poll_ctx *);
FAKE_VALUE_FUNC(int, nrf_cloud_fota_poll_process, struct nrf_cloud_fota_poll_ctx *);
FAKE_VALUE_FUNC(int, nrf_cloud_fota_poll_update_apply, struct nrf_cloud_fota_poll_ctx *);
FAKE_VALUE_FUNC(int, fota_download_cancel);
FAKE_VOID_FUNC1(callback_t, int);

ZBUS_MSG_SUBSCRIBER_DEFINE(fota_subscriber);
ZBUS_CHAN_ADD_OBS(fota_chan, fota_subscriber, 0);

/* Mirror of the FOTA module's private channel and message type. The definitions are file-static
 * inside fota.c so the testre-declares them here in order to publish messages on priv_fota_chan
 * directly. The struct layout must stay byte-identical with the
 * production type.
 */
ZBUS_CHAN_DECLARE(priv_fota_chan);

enum test_priv_fota_msg_type {
	TEST_FOTA_PRIV_MODEM_INITIALIZED,
	TEST_FOTA_PRIV_DOWNLOADING,
	TEST_FOTA_PRIV_REBOOT_NEEDED,
	TEST_FOTA_PRIV_IMAGE_APPLY_NEEDED,
	TEST_FOTA_PRIV_ABORTED,
};

struct test_priv_fota_msg {
	enum test_priv_fota_msg_type type;
};

LOG_MODULE_REGISTER(fota_module_test, 4);

static struct nrf_cloud_fota_poll_ctx test_fota_ctx;

/* NRF_MODEM_LIB_ON_INIT creates a structure with the callback.
 * We can access it to invoke the modem init callback in tests.
 */
struct nrf_modem_lib_init_cb {
	void (*callback)(int ret, void *ctx);
	void *context;
};
extern struct nrf_modem_lib_init_cb nrf_modem_hook_fota_modem_init_hook;

/* Forward declarations */
static void event_expect(enum fota_msg_type expected_fota_type);
static void no_events_expect(uint32_t time_in_seconds);
static void event_send(enum fota_msg_type msg);
static void invoke_nrf_cloud_fota_callback_stub_status(enum nrf_cloud_fota_status status);
static void publish_priv_fota_unhandled(void);

int init_custom_fake(struct nrf_cloud_fota_poll_ctx *ctx)
{
	LOG_DBG("Setup stub for internal nRF Cloud FOTA poll callback handler");

	test_fota_ctx = *ctx;

	return 0;
}

void setUp(void)
{
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(nrf_cloud_fota_poll_init);
	RESET_FAKE(nrf_cloud_fota_poll_process_pending);
	RESET_FAKE(nrf_cloud_fota_poll_process);
	RESET_FAKE(nrf_cloud_fota_poll_update_apply);
	RESET_FAKE(fota_download_cancel);

	FFF_RESET_HISTORY();

	nrf_cloud_fota_poll_init_fake.custom_fake = init_custom_fake;

	const struct zbus_channel *chan;
	struct fota_msg received_msg;

	while (zbus_sub_wait_msg(&fota_subscriber, &chan, &received_msg, K_NO_WAIT) == 0) {
		/* Purge all messages from the channel */
	}

	/* Initialize the fota module by calling the modem init callback
	 * via the hook structure
	 */
	nrf_modem_hook_fota_modem_init_hook.callback(0,
		nrf_modem_hook_fota_modem_init_hook.context);

	/* Wait for initialization */
	k_sleep(K_MSEC(100));
}

void tearDown(void)
{
	/* Verify the test produced no unexpected events. */
	no_events_expect(3600);

	/* Reset DuTs internal state between each test case */
	event_send(FOTA_DOWNLOAD_CANCEL);
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_CANCELED);

	/* Allow the module thread to settle, then drain any leftover events. */
	k_sleep(K_MSEC(200));

	const struct zbus_channel *chan;
	struct fota_msg drained;

	while (zbus_sub_wait_msg(&fota_subscriber, &chan, &drained, K_NO_WAIT) == 0) {
		/* Purge all messages from the channel */
	}
}

static void event_expect(enum fota_msg_type expected_fota_type)
{
	int err;
	const struct zbus_channel *chan;
	struct fota_msg fota_msg;

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(1000));
	if (err == -ENOMSG) {
		LOG_ERR("No fota event received");
		TEST_FAIL();
	} else if (err) {
		LOG_ERR("zbus_sub_wait, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	if (chan != &fota_chan) {
		LOG_ERR("Received message from wrong channel");
		TEST_FAIL();
	}

	TEST_ASSERT_EQUAL(expected_fota_type, fota_msg.type);
}

static void no_events_expect(uint32_t time_in_seconds)
{
	int err;
	const struct zbus_channel *chan;
	struct fota_msg fota_msg;

	/* Allow the test thread to sleep so that the DUT's thread is allowed to run. */
	k_sleep(K_SECONDS(time_in_seconds));

	err = zbus_sub_wait_msg(&fota_subscriber, &chan, &fota_msg, K_MSEC(1000));
	if (err == 0) {
		LOG_ERR("Received fota event with type %d", fota_msg.type);
		TEST_FAIL();
	}
}

static void event_send(enum fota_msg_type msg)
{
	struct fota_msg fota_msg = { .type = msg };
	int err = zbus_chan_pub(&fota_chan, &fota_msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
	/* Expect the event we just sent */
	event_expect(msg);
}

static void invoke_nrf_cloud_fota_callback_stub_status(enum nrf_cloud_fota_status status)
{
	test_fota_ctx.status_fn(status, NULL);
}

static void invoke_nrf_cloud_fota_callback_stub_reboot(enum nrf_cloud_fota_reboot_status status)
{
	test_fota_ctx.reboot_fn(status);
}

static void publish_priv_fota_unhandled(void)
{
	struct test_priv_fota_msg msg = { .type = TEST_FOTA_PRIV_MODEM_INITIALIZED };
	int err = zbus_chan_pub(&priv_fota_chan, &msg, K_SECONDS(1));

	TEST_ASSERT_EQUAL(0, err);
}

void test_fota_module_should_publish_ready(void)
{
	event_expect(FOTA_MODULE_READY);
}

void test_fota_module_should_return_no_available_job(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = -EAGAIN;

	event_send(FOTA_POLL_REQUEST);
	event_expect(FOTA_ABORTED);

	/* Expect */
	TEST_ASSERT(nrf_cloud_fota_poll_process_fake.call_count == 1);
}

void test_fota_module_should_succeed(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;
	nrf_cloud_fota_poll_update_apply_fake.return_val = 0;

	/* 1. Poll for update */
	event_send(FOTA_POLL_REQUEST);

	/* 1a. While in POLLING, an unhandled message on the private channel should be ignored */
	publish_priv_fota_unhandled();
	no_events_expect(1);

	/* 1b. FOTA_DOWNLOAD_CANCEL is intentionally ignored by the POLLING
	 *     state (nothing to cancel yet)
	 */
	event_send(FOTA_DOWNLOAD_CANCEL);
	no_events_expect(1);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 2a. NRF_CLOUD_FOTA_SUCCEEDED intentionally emits no fota_chan
	 *     event, the code waits for the reboot_fn callback.
	 */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_SUCCEEDED);
	no_events_expect(1);

	/* 3. Download succeeded, validation needed */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_FMFU_VALIDATION_NEEDED);
	event_expect(FOTA_NETWORK_DISCONNECT_NEEDED);

	/* 4. Application reports the network is ready for the apply step. */
	event_send(FOTA_NETWORK_DISCONNECTED);

	/* 5. Reboot needed */
	invoke_nrf_cloud_fota_callback_stub_reboot(FOTA_REBOOT_SUCCESS);
	event_expect(FOTA_SUCCESS);
}

void test_fota_module_should_fail_on_timeout(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;

	/* 1. Poll for update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download timed out */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_TIMED_OUT);
	event_expect(FOTA_ABORTED);
}

void test_fota_module_should_fail_on_fail(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;

	/* 1. Poll for update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download failed */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_FAILED);
	event_expect(FOTA_ABORTED);
}

void test_fota_module_should_fail_on_cancellation(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;

	/* 1. Poll for update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download canceled */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_CANCELED);
	event_expect(FOTA_ABORTED);
}

void test_fota_module_should_fail_on_rejection(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;

	/* 1. Poll for update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download rejected */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_REJECTED);
	event_expect(FOTA_ABORTED);
}

void test_fota_module_should_restart_after_cancellation(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;
	nrf_cloud_fota_poll_update_apply_fake.return_val = 0;

	/* 1. Poll for first update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download canceled */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_CANCELED);
	event_expect(FOTA_ABORTED);

	/* 4. Poll for second update - should work after cancellation */
	event_send(FOTA_POLL_REQUEST);

	/* 5. Downloading second update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 6. Download succeeded, validation needed */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_FMFU_VALIDATION_NEEDED);
	event_expect(FOTA_NETWORK_DISCONNECT_NEEDED);

	/* 4. Application reports the network is ready for the apply step. */
	event_send(FOTA_NETWORK_DISCONNECTED);

	/* 8. Reboot needed */
	invoke_nrf_cloud_fota_callback_stub_reboot(FOTA_REBOOT_SUCCESS);
	event_expect(FOTA_SUCCESS);
}

void test_fota_module_should_restart_after_rejection(void)
{
	/* Given */
	nrf_cloud_fota_poll_process_fake.return_val = 0;
	nrf_cloud_fota_poll_update_apply_fake.return_val = 0;

	/* 1. Poll for first update */
	event_send(FOTA_POLL_REQUEST);

	/* 2. Downloading update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 3. Download rejected */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_REJECTED);
	event_expect(FOTA_ABORTED);

	/* 4. Poll for second update - should work after rejection */
	event_send(FOTA_POLL_REQUEST);

	/* 5. Downloading second update */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_DOWNLOADING);
	event_expect(FOTA_STARTING);

	/* 6. Validation needed -> network disconnect requested */
	invoke_nrf_cloud_fota_callback_stub_status(NRF_CLOUD_FOTA_FMFU_VALIDATION_NEEDED);
	event_expect(FOTA_NETWORK_DISCONNECT_NEEDED);

	/* 7. Application reports the network is ready */
	event_send(FOTA_NETWORK_DISCONNECTED);

	/* 8. Reboot needed */
	invoke_nrf_cloud_fota_callback_stub_reboot(FOTA_REBOOT_SUCCESS);
	event_expect(FOTA_SUCCESS);
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
