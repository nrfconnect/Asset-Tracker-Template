/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>

#include "message_channel.h"
#include "led_pwm.h"
#include "led.h"

/* Register log module */
LOG_MODULE_REGISTER(led, CONFIG_APP_LED_LOG_LEVEL);

void led_callback(const struct zbus_channel *chan);

/* Register listener - led_callback will be called everytime a channel that the module listens on
 * receives a new message.
 */
ZBUS_LISTENER_DEFINE(led, led_callback);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(ERROR_CHAN, led, 0);
ZBUS_CHAN_ADD_OBS(CONFIG_CHAN, led, 0);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, led, 0);
ZBUS_CHAN_ADD_OBS(TRIGGER_MODE_CHAN, led, 0);
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, led, 0);

/* Zephyr SMF states */
enum state {
	STATE_RUNNING,
	/* Sub-state to STATE_RUNNING */
	STATE_LED_SET,
	/* Sub-state to STATE_RUNNING */
	STATE_LED_NOT_SET,
	/* Sub-state to STATE_LED_NOT_SET */
	STATE_POLL,
	/* Sub-state to STATE_LED_NOT_SET */
	STATE_NORMAL,
	STATE_ERROR,
};

/* Forward declarations */
static void led_pattern_update_work_fn(struct k_work *work);
static const struct smf_state states[];

/* Definition used to specify LED patterns that should hold forever. */
#define HOLD_FOREVER -1

/* List of LED patterns supported in the UI module. */
static struct led_pattern {
	/* Variable used to construct a linked list of led patterns. */
	sys_snode_t header;

	/* LED state. */
	enum led_state led_state;

	/* Duration of the LED state. */
	int16_t duration_sec;

	/* RGB values for the LED state. */
	uint8_t red;
	uint8_t green;
	uint8_t blue;

} led_pattern_list[LED_PATTERN_COUNT];

struct s_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Trigger mode */
	enum trigger_mode mode;

	/* Network status */
	enum network_msg_type status;

	/* Network status */
	enum location_status location_status;

	/* LED color */
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	/* Error type */
	enum error_type type;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;
};

/* SMF state object variable */
static struct s_object state_object;

/* Linked list used to schedule multiple LED pattern transitions. */
static sys_slist_t pattern_transition_list = SYS_SLIST_STATIC_INIT(&pattern_transition_list);

/* Delayed work that is used to display and transition to the correct LED pattern depending on the
 * internal state of the module.
 */
static K_WORK_DELAYABLE_DEFINE(led_pattern_update_work, led_pattern_update_work_fn);

char *led_state_name(enum led_state state)
{
	switch (state) {
	case LED_OFF:
		return "LED_OFF";
	case LED_CONFIGURED:
		return "LED_CONFIGURED";
	case LED_POLL_MODE:
		return "LED_POLL_MODE";
	case LED_LOCATION_SEARCHING:
		return "LED_LOCATION_SEARCHING";
	case LED_LTE_CONNECTING:
		return "LED_LTE_CONNECTING";
	case LED_ERROR_SYSTEM_FAULT:
		return "LED_ERROR_SYSTEM_FAULT";
	case LED_ERROR_IRRECOVERABLE:
		return "LED_ERROR_IRRECOVERABLE";
	default:
		return "LED_UNKNOWN";
	}
}

/* Function that clears LED pattern transition list. */
static void transition_list_clear(void)
{
	struct led_pattern *transition = NULL;
	struct led_pattern *next_transition = NULL;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&pattern_transition_list,
					  transition,
					  next_transition,
					  header) {
		sys_slist_remove(&pattern_transition_list, NULL, &transition->header);
	};
}

/* Function that appends a LED state and a corresponding duration to the
 * LED pattern transition list. If the LED state is set to LED_CONFIGURED, the RGB values are
 * also appended to the list and used to set the desired color.
 */
static void transition_list_append(enum led_state led_state, int16_t duration_sec,
				   uint8_t red, uint8_t green, uint8_t blue)
{
	led_pattern_list[led_state].led_state = led_state;
	led_pattern_list[led_state].duration_sec = duration_sec;

	if (led_state == LED_CONFIGURED) {
		led_pattern_list[led_state].red = red;
		led_pattern_list[led_state].green = green;
		led_pattern_list[led_state].blue = blue;
	}

	sys_slist_append(&pattern_transition_list, &led_pattern_list[led_state].header);
}

static void led_pattern_update_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int err;
	struct led_pattern *next_pattern;
	static enum led_state previous_led_state = LED_PATTERN_COUNT;
	sys_snode_t *node = sys_slist_get(&pattern_transition_list);

	if (node == NULL) {
		LOG_ERR("Cannot find any more LED pattern transitions");
		return;
	}

	next_pattern = CONTAINER_OF(node, struct led_pattern, header);

	/* Prevent the same LED led_state from being scheduled twice in a row unless the state is
	 * LED_CONFIGURED which means that the user has set a new color.
	 */
	if (next_pattern->led_state != previous_led_state ||
	    next_pattern->led_state == LED_CONFIGURED) {

		if (next_pattern->led_state == LED_CONFIGURED) {

			LOG_DBG("Setting LED configuration: red: %d, green: %d, blue: %d",
			next_pattern->red, next_pattern->green, next_pattern->blue);

			err = led_pwm_set_rgb(next_pattern->red,
					      next_pattern->green,
					      next_pattern->blue);
			if (err) {
				LOG_ERR("Failed to set LED configuration");
				SEND_FATAL_ERROR();
				return;
			}

		} else {

			LOG_DBG("Setting LED effect: %s", led_state_name(next_pattern->led_state));

			err = led_pwm_set_effect(next_pattern->led_state);
			if (err) {
				LOG_ERR("Failed to set LED effect");
				SEND_FATAL_ERROR();
				return;
			}
		}

		previous_led_state = next_pattern->led_state;
	}

	/* Even if the LED state is not updated due a match with the previous state a LED pattern
	 * update is scheduled. This will prolong the pattern until the LED pattern transition
	 * list is cleared.
	 */
	if (next_pattern->duration_sec > 0) {
		k_work_reschedule(&led_pattern_update_work, K_SECONDS(next_pattern->duration_sec));
	}
}

static bool is_rgb_off(uint8_t red, uint8_t green, uint8_t blue)
{
	return (red == 0 && green == 0 && blue == 0);
}

static void on_network_disconnected(void)
{
	transition_list_clear();
	transition_list_append(LED_LTE_CONNECTING, HOLD_FOREVER, 0, 0, 0);

	k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
}

/* Zephyr State Machine framework handlers */

/* HSM states:
 *
 * - STATE_RUNNING: Initial state, the module is running
 *	- STATE_LED_SET: LED is configured by the user
 *	- STATE_LED_NOT_SET: LED is not configured by the user, operational pattern is displayed
 *		- STATE_POLL: Poll pattern or location search pattern is displayed depending on
 			      the location status
 *		- STATE_NORMAL: Led is off or location search pattern is displayed depending on
 *				the location status
 * - STATE_ERROR: An error has occured
 */

/* STATE_RUNNING */

static void running_run(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("running_run");

	if (&ERROR_CHAN == user_object->chan) {
		smf_set_state(SMF_CTX(user_object), &states[STATE_ERROR]);
		return;
	}

	if (&NETWORK_CHAN == user_object->chan && user_object->status == NETWORK_DISCONNECTED) {
		on_network_disconnected();
		return;
	}

	if (&NETWORK_CHAN == user_object->chan && user_object->status == NETWORK_CONNECTED) {

		/* If the network is connected, we just reenter the same state */
		smf_set_state(SMF_CTX(user_object), &states[STATE_RUNNING]);
		return;
	}
}

/* STATE_LED_SET */

static void led_set_entry(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("state_led_set");

	transition_list_clear();
	transition_list_append(LED_CONFIGURED,
			       HOLD_FOREVER,
			       user_object->red,
			       user_object->green,
			       user_object->blue);

	k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
}

static void led_set_running(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("led_set_running");

	if ((&CONFIG_CHAN == user_object->chan) &&
	    is_rgb_off(user_object->red, user_object->green, user_object->blue)) {

		if (user_object->mode == TRIGGER_MODE_NORMAL) {
			smf_set_state(SMF_CTX(user_object), &states[STATE_NORMAL]);
			return;
		} else if (user_object->mode == TRIGGER_MODE_POLL) {
			smf_set_state(SMF_CTX(user_object), &states[STATE_POLL]);
			return;
		}
	}

	if ((&CONFIG_CHAN == user_object->chan) &&
	    !is_rgb_off(user_object->red, user_object->green, user_object->blue)) {

		smf_set_state(SMF_CTX(user_object), &states[STATE_LED_SET]);
		return;
	}
}

/* STATE_LED_NOT_SET */

static void led_not_set_running(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("led_not_set_running");

	if ((&CONFIG_CHAN == user_object->chan) &&
	    !is_rgb_off(user_object->red, user_object->green, user_object->blue)) {

		smf_set_state(SMF_CTX(user_object), &states[STATE_LED_SET]);
		return;
	}
}

/* STATE_POLL */

static void poll_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("poll_entry");

	transition_list_clear();
	transition_list_append(LED_POLL_MODE, HOLD_FOREVER, 0, 0, 0);

	k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
}

static void poll_running(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("poll_running");

	if ((&TRIGGER_MODE_CHAN == user_object->chan) && user_object->mode == TRIGGER_MODE_NORMAL) {
		smf_set_state(SMF_CTX(user_object), &states[STATE_NORMAL]);
		return;
	}

	if ((&LOCATION_CHAN == user_object->chan) && user_object->location_status == LOCATION_SEARCH_STARTED) {

		transition_list_clear();
		transition_list_append(LED_LOCATION_SEARCHING, HOLD_FOREVER, 0, 0, 0);

		k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
		return;
	}

	if ((&LOCATION_CHAN == user_object->chan) && user_object->location_status == LOCATION_SEARCH_DONE) {

		smf_set_state(SMF_CTX(user_object), &states[STATE_POLL]);
		return;
	}
}

/* STATE_NORMAL */

static void normal_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("normal_entry");

	transition_list_clear();
	transition_list_append(LED_OFF, HOLD_FOREVER, 0, 0, 0);

	k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
}

static void normal_running(void *o)
{
	struct s_object *user_object = o;

	LOG_DBG("normal_running");

	if ((&TRIGGER_MODE_CHAN == user_object->chan) && user_object->mode == TRIGGER_MODE_POLL) {
		smf_set_state(SMF_CTX(user_object), &states[STATE_POLL]);
		return;
	}

	if ((&LOCATION_CHAN == user_object->chan) && user_object->location_status == LOCATION_SEARCH_STARTED) {

		transition_list_clear();
		transition_list_append(LED_LOCATION_SEARCHING, HOLD_FOREVER, 0, 0, 0);

		k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
		return;
	}

	if ((&LOCATION_CHAN == user_object->chan) && user_object->location_status == LOCATION_SEARCH_DONE) {

		smf_set_state(SMF_CTX(user_object), &states[STATE_NORMAL]);
		return;
	}
}

/* STATE_ERROR */

static void error_entry(void *o)
{
	struct s_object *user_object = o;

	transition_list_clear();

	if (user_object->type == ERROR_IRRECOVERABLE) {
		transition_list_append(LED_ERROR_IRRECOVERABLE, HOLD_FOREVER, 0, 0, 0);
	} else if (user_object->type == ERROR_FATAL) {
		transition_list_append(LED_ERROR_SYSTEM_FAULT, HOLD_FOREVER, 0, 0, 0);
	}

	k_work_reschedule(&led_pattern_update_work, K_NO_WAIT);
}

/* Construct state table */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(
		NULL,
		running_run,
		NULL,
		NULL,
		&states[STATE_LED_NOT_SET]
	),
	[STATE_LED_SET] = SMF_CREATE_STATE(
		led_set_entry,
		led_set_running,
		NULL,
		&states[STATE_RUNNING],
		NULL
	),
	[STATE_LED_NOT_SET] = SMF_CREATE_STATE(
		NULL,
		led_not_set_running,
		NULL,
		&states[STATE_RUNNING],
		&states[STATE_NORMAL]
	),
	[STATE_POLL] = SMF_CREATE_STATE(
		poll_entry,
		poll_running,
		NULL,
		&states[STATE_LED_NOT_SET],
		NULL
	),
	[STATE_NORMAL] = SMF_CREATE_STATE(
		normal_entry,
		normal_running,
		NULL,
		&states[STATE_LED_NOT_SET],
		NULL
	),
	[STATE_ERROR] = SMF_CREATE_STATE(
		error_entry,
		NULL,
		NULL,
		NULL,
		NULL
	)
};

/* Function called when there is a message received on a channel that the module listens to */
void led_callback(const struct zbus_channel *chan)
{
	int err;

	/* Update the state object with the channel that the message was received on */
	state_object.chan = chan;

	/* Update the state object with the message received on the channel */
	if (&TRIGGER_MODE_CHAN == chan) {
		const enum trigger_mode *mode = zbus_chan_const_msg(chan);

		state_object.mode = *mode;
	}

	if (&NETWORK_CHAN == chan) {
		const struct network_msg *msg = zbus_chan_const_msg(chan);

		state_object.status = msg->type;
	}

	if (&LOCATION_CHAN == chan) {
		const enum location_status *status = zbus_chan_const_msg(chan);

		state_object.location_status = *status;
	}

	if (&CONFIG_CHAN == chan) {
		/* Get LED configuration from channel. */

		const struct configuration *config = zbus_chan_const_msg(chan);

		if (config->led_present == false) {
			LOG_DBG("LED configuration not present");
			return;
		}

		/* Set the changed incoming color, if a color is not present,
		 * the old value will be used
		 */
		state_object.red = (config->led_red_present) ?
				    (uint8_t)config->led_red : state_object.red;
		state_object.green = (config->led_green_present) ?
				      (uint8_t)config->led_green : state_object.green;
		state_object.blue = (config->led_blue_present) ?
				     (uint8_t)config->led_blue : state_object.blue;
	}

	if (&ERROR_CHAN == chan) {
		const enum error_type *type = zbus_chan_const_msg(chan);

		if (*type != ERROR_FATAL && *type != ERROR_IRRECOVERABLE) {
			LOG_DBG("Unknown error type, ignoring");
			return;
		}

		state_object.type = *type;
	}

	/* State object updated, run SMF */
	err = smf_run_state(SMF_CTX(&state_object));
	if (err) {
		LOG_ERR("smf_run_state, error: %d", err);
		SEND_FATAL_ERROR();
		return;
	}
}

static int led_init(void)
{
	smf_set_initial(SMF_CTX(&state_object), &states[STATE_RUNNING]);

	return 0;
}

/* Initialize module at SYS_INIT() */
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
