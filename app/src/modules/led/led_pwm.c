/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <string.h>

#include "led.h"
#include "led_pwm.h"
#include "led_effect.h"

LOG_MODULE_REGISTER(app_led_pwm, CONFIG_APP_LED_LOG_LEVEL);

#define PWM_LED0_NODE	DT_ALIAS(pwm_led0)
#define PWM_LED1_NODE	DT_ALIAS(pwm_led1)
#define PWM_LED2_NODE	DT_ALIAS(pwm_led2)

#if DT_NODE_HAS_STATUS(PWM_LED0_NODE, okay)
static const struct pwm_dt_spec led0 = PWM_DT_SPEC_GET(PWM_LED0_NODE);
#else
#error "Unsupported board: pwm-led 0 devicetree alias is not defined"
#endif
#if DT_NODE_HAS_STATUS(PWM_LED1_NODE, okay)
static const struct pwm_dt_spec led1 = PWM_DT_SPEC_GET(PWM_LED1_NODE);
#else
#error "Unsupported board: pwm-led 1 devicetree alias is not defined"
#endif
#if DT_NODE_HAS_STATUS(PWM_LED2_NODE, okay)
static const struct pwm_dt_spec led2 = PWM_DT_SPEC_GET(PWM_LED2_NODE);
#else
#error "Unsupported board: pwm-led 2 devicetree alias is not defined"
#endif

/* Define separate workqueue to offload led PWM handling */
static K_THREAD_STACK_DEFINE(stack_area, CONFIG_APP_LED_PWM_WORKQUEUE_STACK_SIZE);
static struct k_work_q led_pwm_queue;

static struct led_effect effect_on = LED_EFFECT_LED_ON(LED_NOCOLOR());
struct led {
	size_t id;
	struct led_color color;
	const struct led_effect *effect;
	uint16_t effect_step;
	uint16_t effect_substep;

	struct k_work_delayable work;
	struct k_work_sync work_sync;
};

static struct led leds;

static const struct led_effect effect[] = {
	[LED_LTE_CONNECTING]	 = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_NORMAL,
					LED_OFF_PERIOD_NORMAL,
					LED_LTE_CONNECTING_COLOR
				   ),
	[LED_POLL_MODE]		 = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_SHORT,
					LED_OFF_PERIOD_LONG,
					LED_POLL_MODE_COLOR
				   ),
	[LED_LOCATION_SEARCHING] = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_NORMAL,
					LED_OFF_PERIOD_NORMAL,
					LED_LOCATION_SEARCHING_COLOR
				   ),
	[LED_ERROR_SYSTEM_FAULT] = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_ERROR,
					LED_OFF_PERIOD_ERROR,
					LED_ERROR_SYSTEM_FAULT_COLOR
				   ),
	[LED_ERROR_IRRECOVERABLE] = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_SHORT,
					LED_OFF_PERIOD_LONG,
					LED_ERROR_SYSTEM_FAULT_COLOR
				   ),
	[LED_OFF]		 = LED_EFFECT_LED_BREATHE(
					LED_ON_PERIOD_ERROR,
					LED_OFF_PERIOD_ERROR,
					LED_OFF_COLOR
				   ),
};

static int pwm_out(const struct led_color *color)
{
	int err;

	/* RED */
	err = pwm_set_dt(&led0, PWM_USEC(LED_MAX), PWM_USEC(color->c[0]));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	/* GREEN */
	err = pwm_set_dt(&led2, PWM_USEC(LED_MAX), PWM_USEC(color->c[1]));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	/* BLUE */
	err = pwm_set_dt(&led1, PWM_USEC(LED_MAX), PWM_USEC(color->c[2]));
	if (err) {
		LOG_ERR("pwm_set_dt, error:%d", err);
		return err;
	}

	return 0;
}

static int led_pwm_start(void)
{
	int err = pm_device_action_run(led0.dev, PM_DEVICE_ACTION_RESUME);

	if (err) {
		LOG_ERR("PWM enable failed, pm_device_action_run: %d.", err);
		return err;
	}

	return 0;
}

static int led_pwm_stop(void)
{
	k_work_cancel_delayable_sync(&leds.work, &leds.work_sync);

	int err = pm_device_action_run(led0.dev, PM_DEVICE_ACTION_SUSPEND);

	if (err) {
		LOG_ERR("PWM disable failed, pm_device_action_run: %d.", err);
		return err;
	}

	return 0;
}

static void work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	enum pm_device_state led0_pwm_power_state;

	__ASSERT_NO_MSG(pm_device_state_get(led0.dev, &led0_pwm_power_state) == 0);

	if (led0_pwm_power_state == PM_DEVICE_STATE_SUSPENDED) {
		LOG_DBG("PWM is suspended, skipping work_handler");
		return;
	}

	const struct led_effect_step *effect_step =
		&leds.effect->steps[leds.effect_step];
	int substeps_left = effect_step->substep_cnt - leds.effect_substep;

	for (size_t i = 0; i < ARRAY_SIZE(leds.color.c); i++) {
		int diff = (effect_step->color.c[i] - leds.color.c[i]) /
			   substeps_left;
		leds.color.c[i] += diff;
	}

	__ASSERT_NO_MSG(pwm_out(&leds.color) == 0);

	leds.effect_substep++;

	if (leds.effect_substep == effect_step->substep_cnt) {
		leds.effect_substep = 0;
		leds.effect_step++;

		if (leds.effect_step == leds.effect->step_cnt) {
			if (leds.effect->loop_forever) {
				leds.effect_step = 0;
			}
		} else {
			__ASSERT_NO_MSG(leds.effect->steps[leds.effect_step].substep_cnt > 0);
		}
	}

	if (leds.effect_step < leds.effect->step_cnt) {
		int32_t next_delay = leds.effect->steps[leds.effect_step].substep_time;

		k_work_reschedule_for_queue(&led_pwm_queue, &leds.work, K_MSEC(next_delay));
	}
}

static int led_update(struct led *led)
{
	k_work_cancel_delayable_sync(&led->work, &led->work_sync);

	led->effect_step = 0;
	led->effect_substep = 0;

	if (led == NULL) {
		LOG_ERR("Input variable is NULL");
		return -EINVAL;
	}

	if (!led->effect) {
		LOG_DBG("No effect set");
		return -EINVAL;
	}

	if (led->effect->step_cnt == 0 || led->effect->steps == NULL) {
		LOG_DBG("Effect steps or count is not set");
		return -EINVAL;
	}

	int32_t next_delay = led->effect->steps[led->effect_step].substep_time;

	k_work_schedule_for_queue(&led_pwm_queue, &led->work, K_MSEC(next_delay));

	return 0;
}

int led_pwm_set_effect(enum led_state state)
{
	int err;
	enum pm_device_state led0_pwm_power_state;

	if (!pwm_is_ready_dt(&led0)) {
		LOG_ERR("PWM not ready");
		return -ENODEV;
	}

	err = pm_device_state_get(led0.dev, &led0_pwm_power_state);
	if (err) {
		LOG_ERR("Failed to assess leds pwm power state, pm_device_state_get: %d.", err);
		return err;
	}

	LOG_DBG("Power state: %d", led0_pwm_power_state);

	if ((state == LED_OFF) && (led0_pwm_power_state == PM_DEVICE_STATE_ACTIVE)) {
		err = led_pwm_stop();
		if (err) {
			LOG_ERR("Failed to stop leds pwm, led_pwm_stop: %d.", err);
			return err;
		}

		return 0;
	}

	if (led0_pwm_power_state == PM_DEVICE_STATE_SUSPENDED) {
		err = led_pwm_start();
		if (err) {
			LOG_ERR("Failed to start leds pwm, led_pwm_start: %d.", err);
			return err;
		}
	}

	leds.effect = &effect[state];

	err = led_update(&leds);
	if (err) {
		LOG_ERR("Failed to update leds pwm, led_update: %d.", err);
		return err;
	}

	return 0;
}

int led_pwm_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
	int err;
	enum pm_device_state led0_pwm_power_state;

	if (!pwm_is_ready_dt(&led0)) {
		LOG_ERR("PWM not ready");
		return -ENODEV;
	}

	err = pm_device_state_get(led0.dev, &led0_pwm_power_state);
	if (err) {
		LOG_ERR("Failed to assess leds pwm power state, pm_device_state_get: %d.", err);
		return err;
	}

	LOG_DBG("Power state: %d", led0_pwm_power_state);

	if ((!red && !green && !blue) && (led0_pwm_power_state == PM_DEVICE_STATE_ACTIVE)) {
		err = led_pwm_stop();
		if (err) {
			LOG_ERR("Failed to stop leds pwm, led_pwm_stop: %d.", err);
			return err;
		}

		return 0;
	}

	if (led0_pwm_power_state == PM_DEVICE_STATE_SUSPENDED) {
		err = led_pwm_start();
		if (err) {
			LOG_ERR("Failed to start leds pwm, led_pwm_start: %d.", err);
			return err;
		}
	}

	effect_on.steps[0].color.c[0] = red;
	effect_on.steps[0].color.c[1] = green;
	effect_on.steps[0].color.c[2] = blue;

	leds.effect = &effect_on;

	err = led_update(&leds);
	if (err) {
		LOG_ERR("Failed to update leds pwm, led_update: %d.", err);
		return err;
	}

	return 0;
}

static int led_pwm_init(void)
{
	k_work_queue_init(&led_pwm_queue);
	k_work_queue_start(&led_pwm_queue, stack_area,
			   K_THREAD_STACK_SIZEOF(stack_area),
			   K_LOWEST_APPLICATION_THREAD_PRIO,
			   NULL);

	k_work_init_delayable(&leds.work, work_handler);

	return 0;
}

SYS_INIT(led_pwm_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
