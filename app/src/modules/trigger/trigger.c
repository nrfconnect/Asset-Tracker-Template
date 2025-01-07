/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include "modules_common.h"
#include "message_channel.h"

/* Register log module */
LOG_MODULE_REGISTER(trigger, CONFIG_APP_TRIGGER_LOG_LEVEL);

void trigger_callback(const struct zbus_channel *chan);

/* Define a ZBUS listener for this module */
ZBUS_LISTENER_DEFINE(trigger, trigger_callback);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(CONFIG_CHAN, trigger, 0);
ZBUS_CHAN_ADD_OBS(CLOUD_CHAN, trigger, 0);
ZBUS_CHAN_ADD_OBS(BUTTON_CHAN, trigger, 0);

/* Forward declarations */
static void trigger_work_fn(struct k_work *work);
static const struct smf_state states[];

/* Delayable work used to schedule triggers */
static K_WORK_DELAYABLE_DEFINE(trigger_work, trigger_work_fn);

/* Zephyr SMF states */
enum state {
	STATE_INIT,
	STATE_CONNECTED,
	STATE_DISCONNECTED,
};

struct state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Trigger interval */
	uint64_t interval_sec;

	/* Button number */
	uint8_t button_number;

	/* Cloud status */
	enum cloud_status status;
};

/* SMF state object variable */
static struct state_object trigger_state;

static void trigger_send(void)
{
	struct network_msg network_msg = {
		.type = NETWORK_QUALITY_SAMPLE_REQUEST,
	};

	int err = zbus_chan_pub(&NETWORK_CHAN, &network_msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

/* Delayed work used to send triggers to the rest of the system */
static void trigger_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_DBG("Sending data sample trigger");

	trigger_send();

	k_work_reschedule(&trigger_work, K_SECONDS(trigger_state.interval_sec));
}

/* Zephyr State Machine framework handlers */

/* STATE_INIT */

static void init_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("init_entry");
}

static void init_run(void *o)
{
	struct state_object *user_object = o;

	LOG_DBG("init_run");

	if ((user_object->chan == &CLOUD_CHAN) &&
	    (user_object->status == CLOUD_CONNECTED_READY_TO_SEND)) {
		LOG_DBG("Cloud connected, going into connected state");
		STATE_SET(trigger_state, STATE_CONNECTED);
		return;
	}
}

/* STATE_CONNECTED */

static void connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("connected_entry");

	k_work_reschedule(&trigger_work, K_NO_WAIT);
}

static void connected_run(void *o)
{
	struct state_object *user_object = o;

	LOG_DBG("connected_run");

	if ((user_object->chan == &CLOUD_CHAN) &&
	    ((user_object->status == CLOUD_CONNECTED_PAUSED) ||
	     (user_object->status == CLOUD_DISCONNECTED))) {
		LOG_DBG("Cloud disconnected/paused, going into disconnected state");
		STATE_SET(trigger_state, STATE_DISCONNECTED);
		return;
	}

	if (user_object->chan == &BUTTON_CHAN) {
		LOG_DBG("Button %d pressed!", user_object->button_number);
		k_work_reschedule(&trigger_work, K_NO_WAIT);
		return;
	}

	if (user_object->chan == &CONFIG_CHAN) {
		LOG_DBG("Configuration update, new interval: %lld", user_object->interval_sec);
		k_work_reschedule(&trigger_work, K_SECONDS(user_object->interval_sec));
		return;
	}
}

/* STATE_DISCONNECTED */

static void disconnected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("disconnected_entry");

	k_work_cancel_delayable(&trigger_work);
}

static void disconnected_run(void *o)
{
	struct state_object *user_object = o;

	LOG_DBG("disconnected_run");

	if (user_object->chan == &CLOUD_CHAN && (user_object->status == CLOUD_CONNECTED_READY_TO_SEND)) {
		STATE_SET(trigger_state, STATE_CONNECTED);
		return;
	}
}

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_INIT] = SMF_CREATE_STATE(
		init_entry,
		init_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_CONNECTED] = SMF_CREATE_STATE(
		connected_entry,
		connected_run,
		NULL,
		NULL,
		NULL
	),
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		disconnected_entry,
		disconnected_run,
		NULL,
		NULL,
		NULL
	)
};

/* Function called when there is a message received on a channel that the module listens to */
void trigger_callback(const struct zbus_channel *chan)
{
	int err;

	if ((chan != &CONFIG_CHAN) &&
	    (chan != &CLOUD_CHAN) &&
	    (chan != &BUTTON_CHAN)) {
		LOG_ERR("Unknown channel");
		return;
	}

	LOG_DBG("Received message on channel %s", zbus_chan_name(chan));

	/* Update the state object with the channel that the message was received on */
	trigger_state.chan = chan;

	/* Copy corresponding data to the state object depending on the incoming channel */
	if (&CONFIG_CHAN == chan) {
		const struct configuration *config = zbus_chan_const_msg(chan);

		if (config->update_interval_present) {
			trigger_state.interval_sec = config->update_interval;
		}
	} else if (&CLOUD_CHAN == chan) {
		const enum cloud_status *status = zbus_chan_const_msg(chan);

		trigger_state.status = *status;
	} else if (&BUTTON_CHAN == chan) {
		const int *button_number = zbus_chan_const_msg(chan);

		trigger_state.button_number = (uint8_t)*button_number;
	}

	LOG_DBG("Running SMF");

	/* State object updated, run SMF */
	err = STATE_RUN(trigger_state);
	if (err) {
		LOG_ERR("smf_run_state, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static int trigger_init(void)
{
	trigger_state.interval_sec = CONFIG_APP_TRIGGER_TIMEOUT_SECONDS;

	STATE_SET_INITIAL(trigger_state, STATE_INIT);

	return 0;
}

/* Initialize module at SYS_INIT() */
SYS_INIT(trigger_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
