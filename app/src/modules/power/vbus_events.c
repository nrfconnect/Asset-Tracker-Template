/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <modem/nrf_modem_lib.h>
#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
#include <modem/nrf_modem_lib_trace.h>
#endif

LOG_MODULE_REGISTER(vbus_events, CONFIG_APP_POWER_LOG_LEVEL);

/* VBUS event source (npm13xx charger device) */
static const struct device *const charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
static const struct device *const pmic_dev = DEVICE_DT_GET(DT_NODELABEL(pmic_main));
static const struct device *const uart0_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *const uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* Modem initialization synchronization */
K_SEM_DEFINE(modem_init_sem, 0, 1);

/* VBUS state tracking */
static bool vbus_present;

static int uart_disable(void)
{
	int err;

	if (!device_is_ready(uart0_dev) || !device_is_ready(uart1_dev))
	{
		LOG_ERR("UART devices are not ready");
		return -ENODEV;
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	err = nrf_modem_lib_trace_level_set(NRF_MODEM_LIB_TRACE_LEVEL_OFF);
	if (err)
	{
		LOG_ERR("nrf_modem_lib_trace_level_set, error: %d", err);
		return err;
	}
#endif

	/* Allow outstanding UART transfers to complete before suspend. */
	k_busy_wait(100 * USEC_PER_MSEC);

	err = pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_SUSPEND);
	if (err && (err != -EALREADY))
	{
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	err = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_SUSPEND);
	if (err && (err != -EALREADY))
	{
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	LOG_DBG("UART devices disabled");

	return 0;
}

static int uart_enable(void)
{
	int err;

	if (!device_is_ready(uart0_dev) || !device_is_ready(uart1_dev))
	{
		LOG_ERR("UART devices are not ready");
		return -ENODEV;
	}

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
	err = nrf_modem_lib_trace_level_set(CONFIG_NRF_MODEM_LIB_TRACE_LEVEL);
	if (err)
	{
		LOG_ERR("nrf_modem_lib_trace_level_set, error: %d", err);
		return err;
	}
#endif

	err = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_RESUME);
	if (err && (err != -EALREADY))
	{
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	err = pm_device_action_run(uart1_dev, PM_DEVICE_ACTION_RESUME);
	if (err && (err != -EALREADY))
	{
		LOG_ERR("pm_device_action_run, error: %d", err);
		return err;
	}

	LOG_DBG("UART devices enabled");

	return 0;
}

static int vbus_is_present(void)
{
	int err;
	struct sensor_value value;

	err = sensor_attr_get(charger_dev,
			      (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			      &value);
	if (err)
	{
		LOG_ERR("sensor_attr_get, error: %d", err);
		return err;
	}

	return value.val1 != 0;
}

static int sync_uart_to_vbus_status(void)
{
	int present;

	if (!IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED))
	{
		return 0;
	}

	present = vbus_is_present();
	if (present < 0)
	{
		return present;
	}

	vbus_present = present;

	return present ? uart_enable() : uart_disable();
}

/**
 * @brief GPIO callback for VBUS detected/removed events from the nPM13xx MFD.
 *
 * Invoked from ISR context; keep logic minimal.
 */
static void event_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	int err;

	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
		LOG_DBG("VBUS detected");
		vbus_present = true;

		if (IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED)) {
			err = uart_enable();
			if (err) {
				LOG_ERR("uart_enable, error: %d", err);
			}
		}
	}

	if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
		LOG_DBG("VBUS removed");
		vbus_present = false;

		if (IS_ENABLED(CONFIG_APP_POWER_DISABLE_UART_ON_VBUS_REMOVED)) {
			err = uart_disable();
			if (err) {
				LOG_ERR("uart_disable, error: %d", err);
			}
		}
	}
}

/**
 * @brief Modem library initialization callback.
 *
 * Called by NRF_MODEM_LIB_ON_INIT when the modem library completes
 * its initialization. Release semaphore to allow UART setup to proceed.
 *
 * @param info Initialization callback info (unused).
 */
static void modem_lib_init_cb(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret)
	{
		LOG_ERR("Modem init failed: %d", ret);
		return;
	}

	k_sem_give(&modem_init_sem);
}

NRF_MODEM_LIB_ON_INIT(modem_init_hook, modem_lib_init_cb, NULL);

/**
 * @brief Subscribe to VBUS detected/removed events via the nPM13xx MFD.
 *
 * @param device nPM13xx MFD parent device (pmic_main).
 * @param event_cb gpio_callback struct (must persist for the lifetime of the subscription).
 * @return 0 on success; negative errno on error.
 */
static int subscribe_to_vbus_events(const struct device *device, struct gpio_callback *event_cb)
{
	int err;

	gpio_init_callback(event_cb, event_callback,
			   BIT(NPM13XX_EVENT_VBUS_DETECTED) | BIT(NPM13XX_EVENT_VBUS_REMOVED));

	err = mfd_npm13xx_add_callback(device, event_cb);
	if (err) {
		LOG_ERR("mfd_npm13xx_add_callback, error: %d", err);
		return err;
	}

	return 0;
}

/**
 * @brief Thread entry point for VBUS event initialization.
 *
 * Waits for modem library initialization to complete before setting up
 * charger event handlers. This sequencing prevents UART conflicts during
 * modem init.
 *
 * @param p1 Unused.
 * @param p2 Unused.
 * @param p3 Unused.
 */
static void vbus_init_thread(void *p1, void *p2, void *p3)
{
	int ret;
	static struct gpio_callback event_cb;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Wait for modem library initialization */
	LOG_DBG("Waiting for modem library initialization");
	ret = k_sem_take(&modem_init_sem, K_FOREVER);
	if (ret) {
		LOG_ERR("Failed to wait for modem init: %d", ret);
		return;
	}

	LOG_DBG("Modem library initialized; setting up VBUS events");

	ret = subscribe_to_vbus_events(pmic_dev, &event_cb);
	if (ret) {
		LOG_ERR("subscribe_to_vbus_events, error: %d", ret);
		return;
	}

	ret = sync_uart_to_vbus_status();
	if (ret) {
		LOG_ERR("Failed to sync UART to VBUS status: %d", ret);
		return;
	}

	LOG_INF("VBUS event module initialized");
}

/* Thread definition - runs at startup and blocks on modem init */
K_THREAD_DEFINE(vbus_init_thread_id,
		1024,
		vbus_init_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
