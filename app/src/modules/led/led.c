/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "led.h"
#include "location.h"
#include "network.h"

#define PWM_LED0	DT_ALIAS(pwm_led0)
#define PWM_LED1	DT_ALIAS(pwm_led1)
#define PWM_LED2	DT_ALIAS(pwm_led2)

#if DT_NODE_HAS_STATUS(PWM_LED0, okay)
static const struct pwm_dt_spec pwm_led0 = PWM_DT_SPEC_GET(PWM_LED0);
#else
#error "Unsupported board: pwm-led 0 devicetree alias is not defined"
#endif
#if DT_NODE_HAS_STATUS(PWM_LED1, okay)
static const struct pwm_dt_spec pwm_led1 = PWM_DT_SPEC_GET(PWM_LED1);
#else
#error "Unsupported board: pwm-led 1 devicetree alias is not defined"
#endif
#if DT_NODE_HAS_STATUS(PWM_LED2, okay)
static const struct pwm_dt_spec pwm_led2 = PWM_DT_SPEC_GET(PWM_LED2);
#else
#error "Unsupported board: pwm-led 2 devicetree alias is not defined"
#endif

/* Register log module */
LOG_MODULE_REGISTER(led, CONFIG_APP_LED_LOG_LEVEL);

static void led_callback(const struct zbus_channel *chan);

/* Register listener - led_callback will be called everytime a channel that the module listens on
 * receives a new message.
 */
ZBUS_LISTENER_DEFINE(led, led_callback);

ZBUS_CHAN_DEFINE(LED_CHAN,
		 struct led_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Observe channels */
ZBUS_CHAN_ADD_OBS(LED_CHAN, led, 0);

static struct k_work_delayable blink_work;

/* Structure to hold all LED state variables */
struct led_state {
	struct led_msg current_state;
	bool is_on;
	int repetitions;
};

static struct led_state led_state;
static void blink_timer_handler(struct k_work *work);

static int pwm_out(const struct led_msg *led_msg, bool force_off)
{
	int err;

	#define PWM_PERIOD PWM_USEC(255)

	/* If force_off is true, turn off all LEDs regardless of led_msg values */
	uint8_t red = force_off ? 0 : led_msg->red;
	uint8_t green = force_off ? 0 : led_msg->green;
	uint8_t blue = force_off ? 0 : led_msg->blue;

	if (!pwm_is_ready_dt(&pwm_led0)) {
		LOG_ERR("Error: PWM device %s is not ready\n", pwm_led0.dev->name);
		return -ENODEV;
	}

	/* RED */
	err = pwm_set_dt(&pwm_led0, PWM_PERIOD, PWM_USEC(red));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	/* GREEN */
	err = pwm_set_dt(&pwm_led1, PWM_PERIOD, PWM_USEC(green));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	/* BLUE */
	err = pwm_set_dt(&pwm_led2, PWM_PERIOD, PWM_USEC(blue));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	return 0;
}

/* Timer work handler for LED blinking */
static void blink_timer_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	led_state.is_on = !led_state.is_on;

	/* Update LED state */
	err = pwm_out(&led_state.current_state, !led_state.is_on);
	if (err) {
		LOG_ERR("pwm_out, error: %d", err);
		SEND_FATAL_ERROR();
	}

	/* If LED just turned off, we completed one cycle */
	if (!led_state.is_on && led_state.repetitions > 0) {
		led_state.repetitions--;
		if (led_state.repetitions == 0) {
			/* We're done, don't schedule next toggle */
			return;
		}
	}

	/* Schedule next toggle */
	uint32_t next_delay = led_state.is_on ?
		led_state.current_state.duration_on_msec :
		led_state.current_state.duration_off_msec;

	err = k_work_schedule(&blink_work, K_MSEC(next_delay));
	if (err < 0) {
		LOG_ERR("k_work_schedule, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

/* Function called when there is a message received on a channel that the module listens to */
static void led_callback(const struct zbus_channel *chan)
{
	if (&LED_CHAN == chan) {
		int err;
		const struct led_msg *led_msg = zbus_chan_const_msg(chan);

		/* Cancel any existing blink timer */
		(void)k_work_cancel_delayable(&blink_work);

		/* Store the new LED state */
		memcpy(&led_state.current_state, led_msg, sizeof(struct led_msg));

		/* Set up repetitions */
		led_state.repetitions = led_msg->repetitions;

		/* If repetitions is 0, turn LED off. Otherwise LED on */
		led_state.is_on = (led_state.repetitions != 0);

		err = pwm_out(led_msg, !led_state.is_on);
		if (err) {
			LOG_ERR("pwm_out, error: %d", err);
			SEND_FATAL_ERROR();
		}

		/* Schedule first toggle if LED should be blinking */
		if (led_state.is_on) {
			err = k_work_schedule(&blink_work, K_MSEC(led_msg->duration_on_msec));
			if (err < 0) {
				LOG_ERR("k_work_schedule, error: %d", err);
				SEND_FATAL_ERROR();
			}
		}
	}
}

static int led_init(void)
{
	k_work_init_delayable(&blink_work, blink_timer_handler);

	return 0;
}

/* Initialize module at SYS_INIT() */
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
