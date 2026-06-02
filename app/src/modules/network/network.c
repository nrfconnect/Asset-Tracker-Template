/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/smf.h>

#include <modem/lte_lc.h>
#include "app_common.h"
#ifdef CONFIG_APP_INSPECT_SHELL
#include "app_inspect.h"
#endif /* CONFIG_APP_INSPECT_SHELL */
#include "network.h"

LOG_MODULE_REGISTER(network, CONFIG_APP_NETWORK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

#define TN_PROFILE_ID		0
#define NTN_PROFILE_ID		1
#define LOCATION_VALIDITY_S	CONFIG_APP_NETWORK_LOCATION_MAX_AGE_SECONDS

ZBUS_CHAN_DEFINE(network_chan,
		 struct network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Private channel, internal state machine events only */
enum priv_network_msg_type {
	NETWORK_PRIV_LOCATION_NEEDED = 0x1,
	NETWORK_PRIV_LOCATION_VALID,
	NETWORK_PRIV_GO_IDLE,
	NETWORK_PRIV_MODEM_LOCATION_REQUEST,
};

struct priv_network_msg {
	enum priv_network_msg_type type;
};

ZBUS_CHAN_DEFINE(priv_network_chan,
		 struct priv_network_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_MSG_SUBSCRIBER_DEFINE(network);

ZBUS_CHAN_ADD_OBS(network_chan, network, 0);
ZBUS_CHAN_ADD_OBS(priv_network_chan, network, 0);

#define MAX_MSG_SIZE sizeof(struct network_msg)
#define MAX_PRIV_MSG_SIZE sizeof(struct priv_network_msg)

/* The same msg_buf receives messages from both observed channels, so it must be large enough
 * for the largest of them.
 */
BUILD_ASSERT(MAX_MSG_SIZE >= MAX_PRIV_MSG_SIZE,
	     "msg_buf must fit the largest observed channel message");

/* After requesting a GNSS fix the network module waits
 * CONFIG_APP_NETWORK_AWAITING_LOCATION_TIMEOUT_SECONDS for the location module to deliver it.
 * The location module bounds its own GNSS search by CONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT
 * (milliseconds). If the network timeout is shorter, the NTN attempt is aborted before the GNSS
 * search finishes and a fix that arrives later is discarded, wasting the GNSS energy spent on it.
 * Require the network wait to be at least as long as the location GNSS search.
 */
#if defined(CONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT)
BUILD_ASSERT((CONFIG_APP_NETWORK_AWAITING_LOCATION_TIMEOUT_SECONDS * MSEC_PER_SEC) >=
	     CONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT,
	     "Network GNSS wait timeout must be >= location GNSS search timeout");
#endif /* CONFIG_LOCATION_REQUEST_DEFAULT_GNSS_TIMEOUT */

/* Cached location data. Updated every time a new GNSS location is received. Used by modem
 * when searching for NTN network.
 */
struct ntn_cached_location {
	double lat;
	double lon;
	float alt;
	int64_t set_uptime_ms;
};

struct network_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct ntn_cached_location cached_location;
	bool cached_location_valid;
	bool modem_location_requested;

	bool fresh_location_requested;
	int64_t last_ntn_location_set_uptime_ms;
	struct k_work_delayable tn_search_timeout_work;
	struct k_work_delayable awaiting_location_timeout_work;
	struct k_work_delayable cell_search_timeout_work;

	/* Modem connectivity tracking, updated only from the LTE event handler.
	 *
	 * reg_registered: the modem currently has network registration (home or roaming).
	 * pdn_active:     the default PDN context is currently active.
	 *
	 * Loss of registration (out of coverage, +CEREG: 4/2/0) is treated as a connectivity loss
	 * even while the PDN context is valid.
	 */
	bool reg_registered;
	bool pdn_active;
};

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);

/* The lte_lc event handler is a callback with no user_data, so it cannot be passed the state
 * object. This single pointer, set once at thread start before the handler is registered, bridges
 * the handler to the module's state object - keeping the connectivity flags it maintains in the
 * state object instead of in file-scope variables.
 */
static struct network_state_object *network_state_ctx;

/* State machine definitions */

enum network_module_state {
	STATE_RUNNING,
		STATE_DISCONNECTED,
			STATE_DISCONNECTED_IDLE,
			STATE_DISCONNECTED_TN_SEARCHING,
			STATE_DISCONNECTED_NTN_SEARCHING,
				STATE_DISCONNECTED_NTN_CHECK_LOCATION,
				STATE_DISCONNECTED_NTN_AWAIT_LOCATION,
				STATE_DISCONNECTED_NTN_CELL_SEARCH,
		STATE_CONNECTED,
			STATE_CONNECTED_TN,
			STATE_CONNECTED_NTN,
		STATE_DISCONNECTING,
};

static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_disconnected_entry(void *obj);
static enum smf_state_result state_disconnected_run(void *obj);
static enum smf_state_result state_disconnected_idle_run(void *obj);
static void state_disconnected_tn_searching_entry(void *obj);
static enum smf_state_result state_disconnected_tn_searching_run(void *obj);
static void state_disconnected_ntn_searching_entry(void *obj);
static enum smf_state_result state_disconnected_ntn_searching_run(void *obj);
static void state_disconnected_ntn_check_location_entry(void *obj);
static enum smf_state_result state_disconnected_ntn_check_location_run(void *obj);
static void state_disconnected_ntn_await_location_entry(void *obj);
static enum smf_state_result state_disconnected_ntn_await_location_run(void *obj);
static void state_disconnected_ntn_cell_search_entry(void *obj);
static enum smf_state_result state_disconnected_ntn_cell_search_run(void *obj);
static enum smf_state_result state_connected_run(void *obj);
static void state_disconnecting_entry(void *obj);
static enum smf_state_result state_disconnecting_run(void *obj);

static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, &states[STATE_DISCONNECTED]),
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
				 &states[STATE_DISCONNECTED_IDLE]),
	[STATE_DISCONNECTED_IDLE] =
		SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL,
				 &states[STATE_DISCONNECTED], NULL),
	[STATE_DISCONNECTED_TN_SEARCHING] =
		SMF_CREATE_STATE(state_disconnected_tn_searching_entry,
				 state_disconnected_tn_searching_run, NULL,
				 &states[STATE_DISCONNECTED], NULL),
	[STATE_DISCONNECTED_NTN_SEARCHING] =
		SMF_CREATE_STATE(state_disconnected_ntn_searching_entry,
				 state_disconnected_ntn_searching_run, NULL,
				 &states[STATE_DISCONNECTED],
				 &states[STATE_DISCONNECTED_NTN_CHECK_LOCATION]),
	[STATE_DISCONNECTED_NTN_CHECK_LOCATION] =
		SMF_CREATE_STATE(state_disconnected_ntn_check_location_entry,
				 state_disconnected_ntn_check_location_run, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCHING], NULL),
	[STATE_DISCONNECTED_NTN_AWAIT_LOCATION] =
		SMF_CREATE_STATE(state_disconnected_ntn_await_location_entry,
				 state_disconnected_ntn_await_location_run, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCHING], NULL),
	[STATE_DISCONNECTED_NTN_CELL_SEARCH] =
		SMF_CREATE_STATE(state_disconnected_ntn_cell_search_entry,
				 state_disconnected_ntn_cell_search_run, NULL,
				 &states[STATE_DISCONNECTED_NTN_SEARCHING], NULL),
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(NULL, state_connected_run, NULL,
				 &states[STATE_RUNNING], NULL),
	[STATE_CONNECTED_TN] =
		SMF_CREATE_STATE(NULL, NULL, NULL,
				 &states[STATE_CONNECTED], NULL),
	[STATE_CONNECTED_NTN] =
		SMF_CREATE_STATE(NULL, NULL, NULL,
				 &states[STATE_CONNECTED], NULL),
	[STATE_DISCONNECTING] =
		SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
				 &states[STATE_RUNNING], NULL),
};

static void network_status_notify(enum network_msg_type status)
{
	int err;
	struct network_msg msg = { .type = status };

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void network_msg_send(const struct network_msg *msg)
{
	int err;

	err = zbus_chan_pub(&network_chan, msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void priv_msg_send(enum priv_network_msg_type type)
{
	int err;
	struct priv_network_msg msg = { .type = type };

	err = zbus_chan_pub(&priv_network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub priv, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void timer_cancel_all(struct network_state_object *state_object)
{
	__ASSERT_NO_MSG(state_object != NULL);

	(void)k_work_cancel_delayable(&state_object->tn_search_timeout_work);
	(void)k_work_cancel_delayable(&state_object->awaiting_location_timeout_work);
	(void)k_work_cancel_delayable(&state_object->cell_search_timeout_work);
}

static void tn_search_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("TN search timeout");
	network_status_notify(NETWORK_TN_SEARCH_FAILED);
}

static void ntn_search_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("NTN cell search timeout");
	network_status_notify(NETWORK_NTN_SEARCH_FAILED);
}

static void awaiting_location_timeout_handler(struct k_work *work)
{
	struct network_msg msg = { .type = NETWORK_GNSS_LOCATION_FAILED };

	ARG_UNUSED(work);

	LOG_WRN("GNSS location wait timeout");
	network_msg_send(&msg);
}

static int func_mode_offline(void)
{
	int err;

	/* First try to go offline while keeping regitration. */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_WRN("lte_lc_func_mode_set (offline keep reg), error: %d, attempting fallback",
			err);

		/* Fall back to offline if there was no registration to preserve. */
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set (offline), error: %d", err);

			return err;
		}

	}

	return 0;
}

static int sys_mode_set(enum lte_lc_system_mode mode)
{
	int err;

	err = func_mode_offline();
	if (err) {
		LOG_ERR("func_mode_offline, error: %d", err);

		return err;
	}

	err = lte_lc_system_mode_set(mode, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set (%d), error: %d", mode, err);

		return err;
	}

	return 0;
}

static int sys_mode_set_if_needed(enum lte_lc_system_mode mode)
{
	int err;
	enum lte_lc_system_mode current;
	enum lte_lc_system_mode_preference preference;

	err = lte_lc_system_mode_get(&current, &preference);
	if (err == 0 && current == mode) {
		return 0;
	}

	return sys_mode_set(mode);
}

static int network_disconnect(struct network_state_object *state_object)
{
	int err;

	timer_cancel_all(state_object);

	err = func_mode_offline();
	if (err) {
		LOG_ERR("func_mode_offline, error: %d", err);

		return err;
	}

	return 0;
}

static int configure_cellular_profiles(void)
{
	int err;
	struct lte_lc_cellular_profile tn_profile = {
		.id = TN_PROFILE_ID,
		.act = LTE_LC_ACT_LTEM | LTE_LC_ACT_NBIOT,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};
	struct lte_lc_cellular_profile ntn_profile = {
		.id = NTN_PROFILE_ID,
		.act = LTE_LC_ACT_NTN,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};

	err = lte_lc_cellular_profile_configure(&tn_profile);
	if (err) {
		LOG_ERR("lte_lc_cellular_profile_configure TN, error: %d", err);

		return err;
	}

	err = lte_lc_cellular_profile_configure(&ntn_profile);
	if (err) {
		LOG_ERR("lte_lc_cellular_profile_configure NTN, error: %d", err);

		return err;
	}

	return 0;
}

static int start_search(void)
{
	int err;

	err = lte_lc_connect_async(lte_lc_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async, error: %d", err);

		return err;
	}

	return 0;
}

static bool cached_location_is_fresh(const struct network_state_object *state_object)
{
	int64_t age_ms;

	if (!state_object->cached_location_valid) {
		return false;
	}

	age_ms = k_uptime_get() - state_object->cached_location.set_uptime_ms;

	return age_ms <= ((int64_t)CONFIG_APP_NETWORK_LOCATION_MAX_AGE_SECONDS * MSEC_PER_SEC);
}

/* Decide whether the cached location can be used to start an NTN cell search, or whether a
 * fresh GNSS fix must be acquired first.
 */
static bool cached_location_is_usable(const struct network_state_object *state_object)
{
	if (!cached_location_is_fresh(state_object)) {
		return false;
	}

	if (state_object->modem_location_requested) {
		int64_t since_last_set_ms =
			k_uptime_get() - state_object->last_ntn_location_set_uptime_ms;
		int64_t debounce_ms =
			(int64_t)(CONFIG_APP_NETWORK_LOCATION_REQUEST_DEBOUNCE_SECONDS *
			MSEC_PER_SEC);

		/* Within the debounce window re-use the cache; outside it force a fresh fix. */
		return since_last_set_ms <= debounce_ms;
	}

	return true;
}

static int set_and_cache_location(struct network_state_object *state_object,
			  double lat, double lon, float alt)
{
	int err;

	state_object->cached_location.lat = lat;
	state_object->cached_location.lon = lon;
	state_object->cached_location.alt = alt;
	state_object->cached_location.set_uptime_ms = k_uptime_get();
	state_object->cached_location_valid = true;

	err = ntn_location_set(lat, lon, alt, LOCATION_VALIDITY_S);
	if (err) {
		LOG_ERR("ntn_location_set, error: %d", err);

		return err;
	}

	state_object->last_ntn_location_set_uptime_ms = k_uptime_get();
	state_object->modem_location_requested = false;

	return 0;
}

bool network_in_ntn_mode(void)
{
	int err;
	enum lte_lc_system_mode mode;
	enum lte_lc_system_mode_preference preference;

	err = lte_lc_system_mode_get(&mode, &preference);
	if (err) {
		LOG_ERR("lte_lc_system_mode_get, error: %d", err);

		return false;
	}

	return mode == LTE_LC_SYSTEM_MODE_NTN_NBIOT;
}

static void send_connected(void)
{
	if (network_in_ntn_mode()) {
		network_status_notify(NETWORK_CONNECTED_NTN);
	} else {
		network_status_notify(NETWORK_CONNECTED_TN);
	}
}

static bool nw_reg_status_is_registered(enum lte_lc_nw_reg_status status)
{
	switch (status) {
	case LTE_LC_NW_REG_REGISTERED_HOME:
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
	case LTE_LC_NW_REG_RX_ONLY_REGISTERED_HOME:
	case LTE_LC_NW_REG_RX_ONLY_REGISTERED_ROAMING:
		return true;
	default:
		return false;
	}
}

static void disconnect_and_idle(struct network_state_object *state_object)
{
	int err;

	err = network_disconnect(state_object);
	if (err) {
		LOG_ERR("network_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}

	smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
}

/* Event handlers */

static void ntn_evt_handler(const struct ntn_evt *evt)
{
	if (evt->type != NTN_EVT_LOCATION_REQUEST) {
		return;
	}

	if (evt->location_request.requested) {
		LOG_DBG("Modem requested NTN location update (accuracy %u m)",
			evt->location_request.accuracy);
		priv_msg_send(NETWORK_PRIV_MODEM_LOCATION_REQUEST);
	}
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	struct network_state_object *state_object = network_state_ctx;

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS: {
		bool is_registered;

		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected");
			network_status_notify(NETWORK_UICC_FAILURE);

			break;
		}

		is_registered = nw_reg_status_is_registered(evt->nw_reg_status);

		if (state_object->reg_registered && !is_registered) {
			LOG_DBG("Registration lost (status %d)", evt->nw_reg_status);
			network_status_notify(NETWORK_DISCONNECTED);
		} else if (!state_object->reg_registered && is_registered &&
			   state_object->pdn_active) {
			/* Regained registration while the PDN context is still active (a coverage
			 * blip that did not tear down the bearer). The modem emits no new PDN
			 * activation in this case, so re-assert connectivity here to resume the
			 * paused session over the same bearer.
			 */
			LOG_DBG("Registration regained with active PDN, resuming");
			send_connected();
		}

		state_object->reg_registered = is_registered;

		break;
	}
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			state_object->pdn_active = true;

			send_connected();

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
		case LTE_LC_EVT_PDN_SUSPENDED:
			state_object->pdn_active = false;

			network_status_notify(NETWORK_DISCONNECTED);

			break;
		case LTE_LC_EVT_PDN_RESUMED:
			state_object->pdn_active = true;

			send_connected();

			break;
		default:
			break;
		}

		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt.type == LTE_LC_MODEM_EVT_SEARCH_DONE) {
			if (network_in_ntn_mode()) {
				network_status_notify(NETWORK_NTN_SEARCH_FAILED);
			} else {
				network_status_notify(NETWORK_TN_SEARCH_FAILED);
			}
		}

		if (evt->modem_evt.type == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			if (network_in_ntn_mode()) {
				network_status_notify(NETWORK_NTN_LIGHT_SEARCH_DONE);
			} else {
				network_status_notify(NETWORK_TN_LIGHT_SEARCH_DONE);
			}
		}

		break;
	case LTE_LC_EVT_PSM_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_PSM_PARAMS,
			.psm_cfg = evt->psm_cfg,
		};

		network_msg_send(&msg);

		break;
	}
	case LTE_LC_EVT_EDRX_UPDATE: {
		struct network_msg msg = {
			.type = NETWORK_EDRX_PARAMS,
			.edrx_cfg = evt->edrx_cfg,
		};

		network_msg_send(&msg);

		break;
	}
	default:
		break;
	}
}

static enum smf_state_result handle_search_stop(struct network_state_object *state_object)
{
	int err;

	timer_cancel_all(state_object);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set, error: %d", err);
		SEND_FATAL_ERROR();
	}

	smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

	return SMF_EVENT_HANDLED;
}

/* State handlers */

/* STATE_RUNNING */

static void state_running_entry(void *obj)
{
	int err;
	struct network_state_object *state_object = obj;

	LOG_DBG("%s", __func__);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	err = configure_cellular_profiles();
	if (err) {
		SEND_FATAL_ERROR();

		return;
	}

	lte_lc_register_handler(lte_lc_evt_handler);
	ntn_register_handler(ntn_evt_handler);

	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	/* Location module waits for CFUN before publishing LOCATION_MODULE_READY. */
	err = lte_lc_normal();
	if (err) {
		LOG_ERR("lte_lc_normal, error: %d", err);
		SEND_FATAL_ERROR();

		return;
	}

	k_work_init_delayable(&state_object->tn_search_timeout_work, tn_search_timeout_handler);
	k_work_init_delayable(&state_object->awaiting_location_timeout_work,
			      awaiting_location_timeout_handler);
	k_work_init_delayable(&state_object->cell_search_timeout_work, ntn_search_timeout_handler);

	LOG_DBG("Network NTN module started");
}

static enum smf_state_result state_running_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &priv_network_chan) {
		const struct priv_network_msg *msg =
			(const struct priv_network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_PRIV_MODEM_LOCATION_REQUEST) {
			int err;

			if (cached_location_is_fresh(state_object)) {
				err = ntn_location_set(state_object->cached_location.lat,
						       state_object->cached_location.lon,
						       state_object->cached_location.alt,
						       LOCATION_VALIDITY_S);
				if (err) {
					LOG_ERR("ntn_location_set, error: %d", err);
				} else {
					state_object->last_ntn_location_set_uptime_ms =
						k_uptime_get();
					state_object->modem_location_requested = false;
				}
			} else {
				/* No fresh fix to provide. Acquiring a new one requires leaving
				 * NTN, so flag the request. The next NTN search will request fix.
				 */
				LOG_DBG("No fresh cached location, deferring location request");

				state_object->modem_location_requested = true;
			}

			return SMF_EVENT_HANDLED;
		}
	}

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		case NETWORK_UICC_FAILURE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED */

static void state_disconnected_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	network_status_notify(NETWORK_DISCONNECTED);
}

static enum smf_state_result state_disconnected_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg =
			(const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECTED:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED_TN:
			timer_cancel_all(state_object);
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_TN]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED_NTN:
			timer_cancel_all(state_object);
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED_NTN]);

			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP:
			return handle_search_stop(state_object);
		case NETWORK_DISCONNECT:
			disconnect_and_idle(state_object);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_IDLE */

static enum smf_state_result state_disconnected_idle_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECT: /* Fall through */
		case NETWORK_SEARCH_STOP:
			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT_TN:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_TN_SEARCHING]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT_NTN:
			state_object->fresh_location_requested = msg->fresh_location;

			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_NTN_SEARCHING]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_TN_SEARCHING */

static void state_disconnected_tn_searching_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	int err;

	LOG_DBG("state_disconnected_tn_searching_entry");

	err = sys_mode_set_if_needed(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS);
	if (err) {
		network_status_notify(NETWORK_TN_SEARCH_FAILED);

		return;
	}

	err = start_search();
	if (err) {
		network_status_notify(NETWORK_TN_SEARCH_FAILED);

		return;
	}

	(void)k_work_schedule(
		&state_object->tn_search_timeout_work,
		K_SECONDS(CONFIG_APP_NETWORK_TN_SEARCH_TIMEOUT_SECONDS));
}

static enum smf_state_result state_disconnected_tn_searching_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECT_TN:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP:
			return handle_search_stop(state_object);
		case NETWORK_DISCONNECT:
		case NETWORK_TN_SEARCH_FAILED:
			disconnect_and_idle(state_object);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED_TN:
			return SMF_EVENT_PROPAGATE;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_NTN_SEARCHING */

static void state_disconnected_ntn_searching_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	int err;

	LOG_DBG("state_disconnected_ntn_searching_entry");

	timer_cancel_all(state_object);

	err = sys_mode_set_if_needed(LTE_LC_SYSTEM_MODE_NTN_NBIOT);
	if (err) {
		network_status_notify(NETWORK_NTN_SEARCH_FAILED);
		priv_msg_send(NETWORK_PRIV_GO_IDLE);

		return;
	}
}

static enum smf_state_result state_disconnected_ntn_searching_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECT_NTN:
			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_STOP:
			return handle_search_stop(state_object);
		case NETWORK_DISCONNECT:
			disconnect_and_idle(state_object);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	if (state_object->chan == &priv_network_chan) {
		const struct priv_network_msg *msg =
			(const struct priv_network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_PRIV_GO_IDLE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_NTN_CHECK_LOCATION */

static void state_disconnected_ntn_check_location_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	int err;

	LOG_DBG("state_disconnected_ntn_check_location_entry");

	if (state_object->fresh_location_requested) {
		state_object->fresh_location_requested = false;

		LOG_DBG("Fresh GNSS fix requested with this NTN connect");
		priv_msg_send(NETWORK_PRIV_LOCATION_NEEDED);

		return;
	}

	if (!cached_location_is_usable(state_object)) {
		priv_msg_send(NETWORK_PRIV_LOCATION_NEEDED);

		return;
	}

	err = ntn_location_set(state_object->cached_location.lat,
			       state_object->cached_location.lon,
			       state_object->cached_location.alt,
			       LOCATION_VALIDITY_S);
	if (err) {
		network_status_notify(NETWORK_NTN_SEARCH_FAILED);
		priv_msg_send(NETWORK_PRIV_GO_IDLE);

		return;
	}

	state_object->last_ntn_location_set_uptime_ms = k_uptime_get();

	priv_msg_send(NETWORK_PRIV_LOCATION_VALID);
}

static enum smf_state_result state_disconnected_ntn_check_location_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &priv_network_chan) {
		const struct priv_network_msg *msg =
			(const struct priv_network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_PRIV_LOCATION_NEEDED:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_NTN_AWAIT_LOCATION]);

			return SMF_EVENT_HANDLED;
		case NETWORK_PRIV_LOCATION_VALID:
			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_NTN_CELL_SEARCH]);

			return SMF_EVENT_HANDLED;
		case NETWORK_PRIV_GO_IDLE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_NTN_AWAIT_LOCATION */

static void state_disconnected_ntn_await_location_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	struct network_msg msg = { .type = NETWORK_GNSS_LOCATION_FAILED};
	int err;

	LOG_DBG("state_disconnected_ntn_await_location_entry");

	err = sys_mode_set_if_needed(LTE_LC_SYSTEM_MODE_GPS);
	if (err) {
		network_msg_send(&msg);

		return;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set (activate GNSS), error: %d", err);
		network_msg_send(&msg);

		return;
	}

	network_status_notify(NETWORK_GNSS_LOCATION_REQ);

	(void)k_work_schedule(
		&state_object->awaiting_location_timeout_work,
		K_SECONDS(CONFIG_APP_NETWORK_AWAITING_LOCATION_TIMEOUT_SECONDS));
}

static enum smf_state_result state_disconnected_ntn_await_location_run(void *obj)
{
	struct network_state_object *state_object = obj;
	struct network_msg fail_msg;
	int err;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_GNSS_LOCATION:
			(void)k_work_cancel_delayable(&state_object->awaiting_location_timeout_work);

			err = set_and_cache_location(state_object,
						     msg->location.lat,
						     msg->location.lon,
						     msg->location.alt);
			if (err) {
				fail_msg.type = NETWORK_GNSS_LOCATION_FAILED;

				network_msg_send(&fail_msg);

				return SMF_EVENT_HANDLED;
			}

			smf_set_state(SMF_CTX(state_object),
				      &states[STATE_DISCONNECTED_NTN_CELL_SEARCH]);

			return SMF_EVENT_HANDLED;
		case NETWORK_GNSS_LOCATION_FAILED:
			timer_cancel_all(state_object);

			err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
			if (err) {
				LOG_ERR("lte_lc_func_mode_set (deactivate GNSS), error: %d", err);
				SEND_FATAL_ERROR();
			}

			/* Without a location fix the NTN attempt cannot proceed. */
			network_status_notify(NETWORK_NTN_SEARCH_FAILED);
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTED_NTN_CELL_SEARCH */

static void state_disconnected_ntn_cell_search_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	int err;

	LOG_DBG("state_disconnected_ntn_cell_search_entry");
	timer_cancel_all(state_object);

	err = sys_mode_set_if_needed(LTE_LC_SYSTEM_MODE_NTN_NBIOT);
	if (err) {
		network_status_notify(NETWORK_NTN_SEARCH_FAILED);
		priv_msg_send(NETWORK_PRIV_GO_IDLE);

		return;
	}

	err = start_search();
	if (err) {
		network_status_notify(NETWORK_NTN_SEARCH_FAILED);
		priv_msg_send(NETWORK_PRIV_GO_IDLE);

		return;
	}

	(void)k_work_schedule(
		&state_object->cell_search_timeout_work,
		K_SECONDS(CONFIG_APP_NETWORK_CELL_SEARCH_TIMEOUT_SECONDS));
}

static enum smf_state_result state_disconnected_ntn_cell_search_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_NTN_SEARCH_FAILED:
			disconnect_and_idle(state_object);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECTED_NTN:
			return SMF_EVENT_PROPAGATE;
		default:
			break;
		}
	}

	if (state_object->chan == &priv_network_chan) {
		const struct priv_network_msg *msg =
			(const struct priv_network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_PRIV_GO_IDLE) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_CONNECTED */

static enum smf_state_result state_connected_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		switch (msg->type) {
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);

			return SMF_EVENT_HANDLED;
		case NETWORK_CONNECT_TN:
		case NETWORK_CONNECT_NTN:
			/* Respond to connection request by repeating connected message. */
			send_connected();

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* STATE_DISCONNECTING */

static void state_disconnecting_entry(void *obj)
{
	struct network_state_object *state_object = obj;
	int err;

	LOG_DBG("state_disconnecting_entry");

	err = network_disconnect(state_object);
	if (err) {
		LOG_ERR("network_disconnect, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnecting_run(void *obj)
{
	struct network_state_object *state_object = obj;

	if (state_object->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state_object->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object),  &states[STATE_DISCONNECTED_IDLE]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* WDT callback */

static void network_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Network watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void network_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	static struct network_state_object network_state;

	network_state_ctx = &network_state;

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();

		return;
	}

	smf_set_initial(SMF_CTX(&network_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = zbus_sub_wait_msg(&network, &network_state.chan,
					network_state.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}

		err = smf_run_state(SMF_CTX(&network_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();

			return;
		}
	}
}

K_THREAD_DEFINE(network_module_thread_id,
		CONFIG_APP_NETWORK_THREAD_STACK_SIZE,
		network_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
