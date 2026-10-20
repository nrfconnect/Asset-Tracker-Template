/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/app_version.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>
#include <modem/nrf_modem_lib.h>
#include <modem/at_monitor.h>
#include <nrf_modem_at.h>
#include <modem/modem_info.h>
#include <modem/at_parser.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>

#include "app_common.h"
#include "ntn.h"
#include "network.h"
#include "location.h"

/* Register log module. "ntn" is used by the NTN library. */
LOG_MODULE_REGISTER(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Cellular profile used by the network module for the terrestrial network. Profile 0 owns
 * PDN contexts 0-9, including the default context used by the rest of the application.
 */
#define TN_PROFILE_ID		0
#define NTN_PROFILE_ID		1

/* Iridium access technology bit in AT%CELLULARPRFL, not part of enum lte_lc_act */
#define NTN_PROFILE_ACT_IRIDIUM	BIT(3)

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)					\
	X(network_chan,		struct network_msg)	\
	X(location_chan,	struct location_msg)	\
	X(ntn_chan,		struct ntn_msg)

/* Private messages from callbacks and timers to the state machine */
enum priv_ntn_msg_type {
	/* The modem is registered to the NTN network */
	PRIV_NTN_REGISTERED,
	/* The default PDN context was activated (new attach) */
	PRIV_NTN_PDN_ACTIVATED,
	/* The default PDN context was resumed (profile/CFUN restore, not yet on-air) */
	PRIV_NTN_PDN_RESUMED,
	/* The default PDN connection is no longer active */
	PRIV_NTN_PDN_DOWN,
	/* RRC connected mode (+CSCON) */
	PRIV_NTN_RRC_CONNECTED,
	/* RRC idle mode */
	PRIV_NTN_RRC_IDLE,
	/* The modem requests a location update for NTN */
	PRIV_NTN_LOCATION_REQUEST,
	/* No terrestrial network connection within CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS */
	PRIV_NTN_TN_TIMEOUT,
	/* Time to search for a terrestrial network again */
	PRIV_NTN_TN_RETRY,
	/* No NTN registration and PDN connection within the connection timeout */
	PRIV_NTN_CONNECTION_TIMEOUT,
	/* +CEREG searching (2): refresh the connection timeout while the modem is still trying */
	PRIV_NTN_SEARCHING,
	/* CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS elapsed since the last NTN uplink */
	PRIV_NTN_UPLINK_DUE,
};

struct priv_ntn_msg {
	enum priv_ntn_msg_type type;
};

#define PRIV_CHANNEL_LIST(X) \
	X(priv_ntn_chan,	struct priv_ntn_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE	MAX(MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST), \
			    MAX_MSG_SIZE_FROM_LIST(PRIV_CHANNEL_LIST))

/* Add the ntn_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, ntn_subscriber, 0);

CHANNEL_LIST(ADD_OBSERVERS)

ZBUS_CHAN_DEFINE(ntn_chan,
		 struct ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(priv_ntn_chan,
		 struct priv_ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(ntn_subscriber),
		 ZBUS_MSG_INIT(0)
);

/* Set while the module waits for NTN registration and PDN connection. Modem events are only
 * forwarded to the state machine while set.
 */
static atomic_t ntn_connecting;

/* Serving cell from +CEREG and LTE_LC_EVT_CELL_UPDATE, as modem_info cannot read it on NTN */
static struct lte_lc_cell serving_cell;
static bool serving_cell_valid;

/* Signal quality reported by AT%SQP? */
struct sqp {
	int rsrp_dbm;
	int rsrq_db;
	int sinr_db;
	int rssi_dbm;
	int phy_cell_id;
	int earfcn;
};

#define SQP_RSRP_UNKNOWN	255
#define SQP_RSRQ_UNKNOWN	255
#define SQP_SINR_UNKNOWN	127
#define SQP_RSSI_UNKNOWN	99

#define AT_CEREG_REG_STATUS_INDEX	1
#define AT_CEREG_TAC_INDEX		2
#define AT_CEREG_CELL_ID_INDEX		3

#define TN_FALLBACK_TIMEOUT	K_SECONDS(CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS)
#define TN_RETRY_INTERVAL	K_MINUTES(CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES)
#define CONNECTION_TIMEOUT	K_SECONDS(CONFIG_APP_NTN_CONNECTION_TIMEOUT_SECONDS)
#define REGISTERED_TIMEOUT	K_SECONDS(CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS)

static void tn_timeout_work_fn(struct k_work *work);
static void tn_retry_work_fn(struct k_work *work);
static void connection_timeout_work_fn(struct k_work *work);
static void uplink_interval_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(tn_timeout_work, tn_timeout_work_fn);
static K_WORK_DELAYABLE_DEFINE(tn_retry_work, tn_retry_work_fn);
static K_WORK_DELAYABLE_DEFINE(connection_timeout_work, connection_timeout_work_fn);
static K_WORK_DELAYABLE_DEFINE(uplink_interval_work, uplink_interval_work_fn);

static void cereg_mon(const char *notif);

/* lte_lc does not report an unchanged registration status. When the modem resumes from
 * LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG on the same cell, the registration is only visible in the
 * +CEREG notification itself.
 */
AT_MONITOR(ntn_cereg_monitor, "+CEREG", cereg_mon, PAUSED);

/* State machine */

/* NTN module states */
enum ntn_module_state {
	/* The module is running */
	STATE_RUNNING,
		/* The network module owns the modem and uses the terrestrial network */
		STATE_TN,
		/* Waiting for the network module to release the modem */
		STATE_SUSPENDING,
		/* The module owns the modem and uses NTN */
		STATE_NTN,
			/* CFUN=45 between uplinks, or GNSS active while waiting for a fix */
			STATE_NTN_IDLE,
			/* The modem is connecting to NTN to send a location */
			STATE_NTN_CONNECTING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct ntn_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Last terrestrial network connection status reported by the network module */
	bool tn_connected;

	/* Last known location */
	struct ntn_location location;

	/* Sample timestamp of the location reported to nRF Cloud */
	int64_t location_timestamp;

	/* Uptime when the location was stored, 0 if no location is available */
	int64_t location_uptime;

	/* Uptime of the last location sent over NTN, 0 if none has been sent */
	int64_t last_uplink_uptime;

	/* Send the next available location regardless of the uplink interval */
	bool uplink_requested;

	/* Return to the terrestrial network when the ongoing NTN uplink is done */
	bool tn_retry_pending;

	/* NTN connection progress */
	bool registered;
	bool pdn_activated_pending;
	bool uplink_attempted;
	bool rrc_connected;
	int64_t rrc_connected_uptime;
	/* On the PDN resume path, the uplink waits for an RRC connection after this uptime */
	int64_t uplink_rrc_required_after;
	int64_t pdn_resumed_time;
	int service_sock_fd;

	/* Terrestrial network system mode, restored when handing the modem back */
	enum lte_lc_system_mode tn_system_mode;
	enum lte_lc_system_mode_preference tn_system_mode_pref;
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_tn_entry(void *obj);
static enum smf_state_result state_tn_run(void *obj);
static void state_tn_exit(void *obj);
static void state_suspending_entry(void *obj);
static enum smf_state_result state_suspending_run(void *obj);
static void state_ntn_entry(void *obj);
static enum smf_state_result state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);
static void state_ntn_idle_entry(void *obj);
static enum smf_state_result state_ntn_idle_run(void *obj);
static void state_ntn_connecting_entry(void *obj);
static enum smf_state_result state_ntn_connecting_run(void *obj);
static void state_ntn_connecting_exit(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_TN]),
	[STATE_TN] =
		SMF_CREATE_STATE(state_tn_entry, state_tn_run, state_tn_exit,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_SUSPENDING] =
		SMF_CREATE_STATE(state_suspending_entry, state_suspending_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_NTN] =
		SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_NTN_IDLE]),
	[STATE_NTN_IDLE] =
		SMF_CREATE_STATE(state_ntn_idle_entry, state_ntn_idle_run, NULL,
				 &states[STATE_NTN],
				 NULL),
	[STATE_NTN_CONNECTING] =
		SMF_CREATE_STATE(state_ntn_connecting_entry, state_ntn_connecting_run,
				 state_ntn_connecting_exit,
				 &states[STATE_NTN],
				 NULL),
};

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* Callbacks and timers */

static void priv_msg_send(enum priv_ntn_msg_type type)
{
	int err;
	const struct priv_ntn_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&priv_ntn_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void tn_timeout_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_TN_TIMEOUT);
}

static void tn_retry_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_TN_RETRY);
}

static void connection_timeout_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_CONNECTION_TIMEOUT);
}

static void uplink_interval_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_UPLINK_DUE);
}

static void service_request_sock_close(struct ntn_state_object *state)
{
	if (state->service_sock_fd >= 0) {
		(void)zsock_close(state->service_sock_fd);
		state->service_sock_fd = -1;
	}
}

static int service_request_sock_open(struct ntn_state_object *state)
{
	int err;
	struct sockaddr_in server = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_APP_NTN_SERVICE_REQUEST_PORT),
	};

	service_request_sock_close(state);

	err = net_addr_pton(AF_INET, CONFIG_APP_NTN_SERVICE_REQUEST_ADDR, &server.sin_addr);
	if (err) {
		LOG_ERR("Invalid service request address");

		return err;
	}

	state->service_sock_fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (state->service_sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);

		return -errno;
	}

	err = zsock_connect(state->service_sock_fd, (struct sockaddr *)&server, sizeof(server));
	if (err < 0) {
		LOG_ERR("Failed to connect UDP socket, error: %d", errno);
		service_request_sock_close(state);

		return err;
	}

	return 0;
}

static int service_request_trigger(struct ntn_state_object *state)
{
	int err;
	static const char dummy[] = "Dummy";

	err = service_request_sock_open(state);
	if (err) {
		return err;
	}

	/* Service-request trigger only; do not use SO_SENDCB or send timeouts here. */
	err = zsock_send(state->service_sock_fd, dummy, sizeof(dummy) - 1, 0);
	if (err < 0) {
		LOG_ERR("Failed to send service request dummy, error: %d", errno);
		service_request_sock_close(state);

		return -errno;
	}

	LOG_DBG("Service request dummy sent (%zu bytes)", sizeof(dummy) - 1);

	return 0;
}

static void uplink_interval_schedule(const struct ntn_state_object *state)
{
	int64_t elapsed_ms;
	int64_t interval_ms;
	int64_t delay_ms;

	elapsed_ms = k_uptime_get() - state->last_uplink_uptime;
	interval_ms = (int64_t)CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS * MSEC_PER_SEC;
	delay_ms = interval_ms - elapsed_ms;

	if (delay_ms < 0) {
		delay_ms = 0;
	}

	(void)k_work_reschedule(&uplink_interval_work, K_MSEC(delay_ms));
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (!atomic_get(&ntn_connecting)) {
		return;
	}

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			priv_msg_send(PRIV_NTN_REGISTERED);
		}

		break;
	case LTE_LC_EVT_CELL_UPDATE:
		if (evt->cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
			serving_cell = evt->cell;
			serving_cell_valid = true;
		}

		break;
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_DBG("PDN activated, cid: %d", evt->pdn.cid);
			priv_msg_send(PRIV_NTN_PDN_ACTIVATED);

			break;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN resumed, cid: %d", evt->pdn.cid);
			priv_msg_send(PRIV_NTN_PDN_RESUMED);

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN down, cid: %d", evt->pdn.cid);
			priv_msg_send(PRIV_NTN_PDN_DOWN);

			break;
		default:
			break;
		}

		break;
	case LTE_LC_EVT_RRC_UPDATE:
		if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
			priv_msg_send(PRIV_NTN_RRC_CONNECTED);
		} else if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			priv_msg_send(PRIV_NTN_RRC_IDLE);
		}

		break;
	default:
		break;
	}
}

static void cereg_mon(const char *notif)
{
	int err;
	int status;
	size_t count;
	char tac[8];
	char cid[12];
	size_t len;
	struct at_parser parser;

	err = at_parser_init(&parser, notif);
	if (err) {
		return;
	}

	err = at_parser_num_get(&parser, AT_CEREG_REG_STATUS_INDEX, &status);
	if (err) {
		return;
	}

	err = at_parser_cmd_count_get(&parser, &count);
	if (!err && (count > AT_CEREG_CELL_ID_INDEX)) {
		len = sizeof(tac);
		if (!at_parser_string_get(&parser, AT_CEREG_TAC_INDEX, tac, &len)) {
			len = sizeof(cid);
			if (!at_parser_string_get(&parser, AT_CEREG_CELL_ID_INDEX, cid, &len)) {
				serving_cell.tac = strtoul(tac, NULL, 16);
				serving_cell.id = strtoul(cid, NULL, 16);
				serving_cell_valid = true;
			}
		}
	}

	if ((status == LTE_LC_NW_REG_REGISTERED_HOME) ||
	    (status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
		priv_msg_send(PRIV_NTN_REGISTERED);

		return;
	}

	if (status == LTE_LC_NW_REG_SEARCHING) {
		priv_msg_send(PRIV_NTN_SEARCHING);
	}
}

static void ntn_evt_handler(const struct ntn_evt *evt)
{
	if ((evt->type == NTN_EVT_LOCATION_REQUEST) && evt->location_request.requested) {
		LOG_DBG("Location requested by the modem, accuracy: %d m",
			evt->location_request.accuracy);

		priv_msg_send(PRIV_NTN_LOCATION_REQUEST);
	}
}

/* Configure both cellular profiles. Required before CFUN=45 (OFFLINE_KEEP_REG).
 * Must be done while the modem is in CFUN=0 or CFUN=4, for example in NRF_MODEM_LIB_ON_INIT.
 */
static int cellular_profiles_configure(void)
{
	int err;
	struct lte_lc_cellular_profile tn_profile = {
		.id = TN_PROFILE_ID,
		.act = LTE_LC_ACT_LTEM | LTE_LC_ACT_NBIOT,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};

	err = lte_lc_cellular_profile_configure(&tn_profile);
	if (err) {
		LOG_ERR("Failed to configure TN cellular profile, error: %d", err);

		return err;
	}

	err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,%d,%d,%d", NTN_PROFILE_ID,
				  (int)NTN_PROFILE_ACT_IRIDIUM, LTE_LC_UICC_PHYSICAL);
	if (err) {
		LOG_ERR("Failed to configure Iridium NTN cellular profile, error: %d", err);

		return err;
	}

	LOG_INF("Cellular profiles configured (TN: profile %d, NTN: profile %d)",
		TN_PROFILE_ID, NTN_PROFILE_ID);

	return 0;
}

static void on_modem_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret) {
		return;
	}

	(void)cellular_profiles_configure();
}

NRF_MODEM_LIB_ON_INIT(ntn_modem_init_hook, on_modem_init, NULL);

/* Helpers */

static void ntn_status_notify(enum ntn_msg_type type)
{
	int err;
	const struct ntn_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&ntn_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void network_request_send(enum network_msg_type type)
{
	int err;
	const struct network_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void location_request_send(enum location_msg_type type)
{
	int err;
	const struct location_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static bool location_is_valid(const struct ntn_state_object *state)
{
	if (state->location_uptime == 0) {
		return false;
	}

	if (CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS == 0) {
		return true;
	}

	return (k_uptime_get() - state->location_uptime) <
	       ((int64_t)CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS * MSEC_PER_SEC);
}

/* Remaining location validity in seconds as expected by ntn_location_set(), 0 is infinite */
static uint32_t location_validity_left(const struct ntn_state_object *state)
{
	int64_t left_ms;

	if (CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS == 0) {
		return 0;
	}

	left_ms = ((int64_t)CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS * MSEC_PER_SEC) -
		  (k_uptime_get() - state->location_uptime);

	return (uint32_t)MAX(1, left_ms / MSEC_PER_SEC);
}

static bool uplink_is_due(const struct ntn_state_object *state)
{
	if (state->uplink_requested || (state->last_uplink_uptime == 0)) {
		return true;
	}

	return (k_uptime_get() - state->last_uplink_uptime) >=
	       ((int64_t)CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS * MSEC_PER_SEC);
}

static void location_store(struct ntn_state_object *state, const struct ntn_location *location,
			   int64_t timestamp)
{
	state->location = *location;
	state->location_timestamp = timestamp;
	state->location_uptime = k_uptime_get();

	LOG_DBG("Location stored: lat: %f, lon: %f, alt: %f, acc: %f",
		state->location.latitude, state->location.longitude,
		(double)state->location.altitude, (double)state->location.accuracy);
}

static void location_store_from_gnss(struct ntn_state_object *state,
				     const struct location_msg *msg)
{
	const struct ntn_location location = {
		.latitude = msg->gnss_data.latitude,
		.longitude = msg->gnss_data.longitude,
		.altitude = msg->gnss_data.details.gnss.pvt_data.altitude,
		.accuracy = msg->gnss_data.accuracy,
	};

	location_store(state, &location, msg->timestamp);
}

/* Modes from which the system mode and cellular profile can be changed */
static bool func_mode_is_offline(enum lte_lc_func_mode mode)
{
	return (mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG) ||
	       (mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON) ||
	       (mode == LTE_LC_FUNC_MODE_OFFLINE) ||
	       (mode == LTE_LC_FUNC_MODE_POWER_OFF);
}

static int set_ntn_offline_mode(void)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	if ((mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG) ||
	    (mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON)) {
		return 0;
	}

	LOG_DBG("Setting CFUN=45 after NTN session (was %d)", mode);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);
	}

	return err;
}

static int gnss_mode_enter(void)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	LOG_DBG("GNSS mode enter, current CFUN: %d", mode);

	/* Already in GNSS mode, leaving it would abort an ongoing search */
	if (mode == LTE_LC_FUNC_MODE_ACTIVATE_GNSS) {
		return 0;
	}

	if (mode != LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);

			return err;
		}
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set(GPS), error: %d", err);

		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_GNSS), error: %d", err);

		return err;
	}

	return 0;
}

static void gnss_search_trigger(void)
{
	int err;

	err = gnss_mode_enter();
	if (err) {
		LOG_ERR("Failed to enable GNSS, error: %d", err);
	}

	LOG_DBG("Requesting GNSS location");

	location_request_send(LOCATION_GNSS_SEARCH_TRIGGER);
}

static int channel_select_set(void)
{
	int err;

	err = nrf_modem_at_printf("AT%%FREQRANGES=1,7,,1,\"%d\",\"\"",
				  CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to select NTN channel %d, error: %d",
			CONFIG_APP_NTN_CHANNEL_SELECT, err);
	}

	return err;
}

/* Keep searching for the NTN cell, sleeping 2 s between searches */
static int periodic_search_set(void)
{
	int err;
	const struct lte_lc_periodic_search_cfg search_cfg = {
		.pattern_count = 1,
		.loop = true,
		.return_to_pattern = 0,
		.band_optimization = 0,
		.patterns[0] = {
			.type = LTE_LC_PERIODIC_SEARCH_PATTERN_TABLE,
			.table = {
				.val_1 = 2,
				.val_2 = -1,
				.val_3 = -1,
				.val_4 = -1,
				.val_5 = -1,
			},
		},
	};

	err = lte_lc_periodic_search_set(&search_cfg);
	if (err) {
		LOG_ERR("lte_lc_periodic_search_set, error: %d", err);
	}

	return err;
}

static int system_mode_ntn_set(void)
{
	int err;

	/* Iridium NTN TDD NB-IoT is not supported by lte_lc. Omitted parameters default to 0,
	 * so lte_lc_system_mode_set() clears this mode again.
	 */
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,0,1");
	if (err) {
		LOG_ERR("Failed to set NTN system mode, error: %d", err);
	}

	return err;
}

/* Switch to the NTN cellular profile and start connecting */
static int ntn_mode_enter(const struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	if (!func_mode_is_offline(mode)) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);

			return err;
		}
	}

	err = system_mode_ntn_set();
	if (err) {
		return err;
	}

	err = ntn_location_set(state->location.latitude, state->location.longitude,
			       state->location.altitude, location_validity_left(state));
	if (err) {
		LOG_ERR("ntn_location_set, error: %d", err);

		return err;
	}

	err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%d\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to lock NTN band %d, error: %d", CONFIG_APP_NTN_BANDLOCK, err);

		return err;
	}

	err = channel_select_set();
	if (err) {
		return err;
	}

	err = periodic_search_set();
	if (err) {
		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_LTE), error: %d", err);

		return err;
	}

	return 0;
}

/* Best effort: each step is attempted even if a previous one failed, so that the network module
 * gets the modem back in a state as close to the terrestrial configuration as possible.
 */
static void tn_mode_restore(const struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode = LTE_LC_FUNC_MODE_NORMAL;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);
	}

	LOG_DBG("TN restore, current CFUN: %d", mode);

	/* Not CFUN=0: with CONFIG_LTE_LC_PDN_DEFAULTS_OVERRIDE, lte_lc re-runs AT+CGDCONT on CID 0
	 * from the CFUN hook, which fails while the NTN system mode is active.
	 */
	if (!func_mode_is_offline(mode)) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);
		}
	}

	err = nrf_modem_at_printf("AT%%XBANDLOCK=0");
	if (err) {
		LOG_ERR("Failed to remove band lock, error: %d", err);
	}

	err = nrf_modem_at_printf("AT%%FREQRANGES=0");
	if (err) {
		LOG_ERR("Failed to clear frequency ranges, error: %d", err);
	}

	err = lte_lc_periodic_search_clear();
	if (err) {
		LOG_ERR("lte_lc_periodic_search_clear, error: %d", err);
	}

	err = lte_lc_system_mode_set(state->tn_system_mode, state->tn_system_mode_pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set, error: %d", err);
	}

	/* lte_lc_connect_async() skips CFUN changes when already registered, so leave CFUN=45
	 * explicitly when handing the modem back to the network module.
	 */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_LTE), error: %d", err);
	}
}

static int rsrp_dbm_to_idx(int dbm)
{
	if (dbm >= SQP_RSRP_UNKNOWN) {
		return LTE_LC_CELL_RSRP_INVALID;
	}

	return dbm + 141;
}

static int sqp_read(struct sqp *sqp)
{
	int n;

	if (sqp == NULL) {
		return -EINVAL;
	}

	n = nrf_modem_at_scanf("AT%SQP?", "%%SQP: %d,%d,%d,%d,%d,%d",
			       &sqp->rsrp_dbm, &sqp->rsrq_db, &sqp->sinr_db, &sqp->rssi_dbm,
			       &sqp->phy_cell_id, &sqp->earfcn);
	if (n < 4) {
		LOG_DBG("AT%%SQP? not parsed: %d", n);

		return -ENODATA;
	}

	if (n < 5) {
		sqp->phy_cell_id = -1;
	}

	if (n < 6) {
		sqp->earfcn = -1;
	}

	LOG_DBG("NTN %%SQP: RSRP %d dBm, RSRQ %d dB, SINR %d dB, RSSI %d dBm, PCI %d, EARFCN %d",
		sqp->rsrp_dbm, sqp->rsrq_db, sqp->sinr_db, sqp->rssi_dbm, sqp->phy_cell_id,
		sqp->earfcn);

	return 0;
}

static void sqp_apply(struct modem_param_info *mpi, const struct sqp *sqp)
{
	if (sqp->rsrp_dbm < SQP_RSRP_UNKNOWN) {
		mpi->network.rsrp.value = (uint16_t)rsrp_dbm_to_idx(sqp->rsrp_dbm);
	}

	if (sqp->earfcn > 0 && serving_cell_valid) {
		serving_cell.earfcn = (uint32_t)sqp->earfcn;
	}
}

static int lte_param_fetch(struct lte_param *param)
{
	enum modem_info_data_type data_type = modem_info_data_type_get(param->type);
	int ret;

	if (data_type == MODEM_INFO_DATA_TYPE_STRING) {
		ret = modem_info_string_get(param->type, param->value_string,
					    sizeof(param->value_string));
	} else if (data_type == MODEM_INFO_DATA_TYPE_NUM_INT) {
		ret = modem_info_short_get(param->type, &param->value);
	} else {
		return -EINVAL;
	}

	return (ret < 0) ? ret : 0;
}

/* Fields not listed are left zeroed or empty */
static void lte_param_default_set(struct lte_param *param)
{
	switch (param->type) {
	case MODEM_INFO_CUR_BAND:
		param->value = CONFIG_APP_NTN_BANDLOCK;
		break;
	case MODEM_INFO_NBIOT_MODE:
		param->value = 1;
		break;
	default:
		param->value = 0;
		param->value_string[0] = '\0';
		break;
	}
}

static int mcc_mnc_from_operator(struct lte_param *operator, struct lte_param *mcc,
				     struct lte_param *mnc)
{
	memcpy(mcc->value_string, operator->value_string, 3);

	if (sizeof(operator->value_string) - 1U < 6U) {
		mnc->value_string[0] = '0';
		memcpy(&mnc->value_string[1], &operator->value_string[3], 2);
	} else {
		memcpy(mnc->value_string, &operator->value_string[3], 3);
	}

	mcc->value = (uint16_t)strtol(mcc->value_string, NULL, 10);
	mnc->value = (uint16_t)strtol(mnc->value_string, NULL, 10);

	return 0;
}

static int cereg_cell_query(uint32_t *cell_id, uint32_t *tac)
{
	char buf[128];
	char str[12];
	size_t len;
	int err;
	struct at_parser parser;

	err = nrf_modem_at_cmd(buf, sizeof(buf), "AT+CEREG?");
	if (err) {
		return -EIO;
	}

	err = at_parser_init(&parser, buf);
	if (err) {
		return err;
	}

	if (tac) {
		len = sizeof(str);
		err = at_parser_string_get(&parser, AT_CEREG_TAC_INDEX, str, &len);
		if (err) {
			return err;
		}

		*tac = strtoul(str, NULL, 16);
	}

	if (cell_id) {
		len = sizeof(str);
		err = at_parser_string_get(&parser, AT_CEREG_CELL_ID_INDEX, str, &len);
		if (err) {
			return err;
		}

		*cell_id = strtoul(str, NULL, 16);
	}

	return 0;
}

static void serving_cell_apply(struct modem_param_info *mpi)
{
	uint32_t cell_id;
	uint32_t tac;

	if (serving_cell_valid) {
		mpi->network.cellid_dec = serving_cell.id;
		mpi->network.area_code.value = (uint16_t)serving_cell.tac;

		return;
	}

	if (cereg_cell_query(&cell_id, &tac) == 0) {
		mpi->network.cellid_dec = cell_id;
		mpi->network.area_code.value = (uint16_t)tac;
	}
}

static int shadow_modem_params_get(struct modem_param_info *mpi)
{
	struct lte_param *params[] = {
		&mpi->network.current_band,
		&mpi->network.sup_band,
		&mpi->network.ip_address,
		&mpi->network.ue_mode,
		&mpi->network.current_operator,
		&mpi->network.cellid_hex,
		&mpi->network.area_code,
		&mpi->network.lte_mode,
		&mpi->network.nbiot_mode,
		&mpi->network.gps_mode,
		&mpi->network.apn,
		&mpi->network.rsrp,
		&mpi->sim.iccid,
		&mpi->sim.imsi,
		&mpi->device.modem_fw,
		&mpi->device.battery,
		&mpi->device.imei,
	};
	struct sqp sqp;
	int err;

	err = modem_info_params_init(mpi);
	if (err) {
		return err;
	}

	err = modem_info_params_get(mpi);
	if (err) {
		LOG_DBG("modem_info_params_get failed (%d), fetching shadow modem fields", err);

		for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
			err = lte_param_fetch(params[i]);
			if (err) {
				LOG_DBG("modem_info type %d not available (%d), using default",
					params[i]->type, err);
				lte_param_default_set(params[i]);
			}
		}

		serving_cell_apply(mpi);

		(void)mcc_mnc_from_operator(&mpi->network.current_operator,
						&mpi->network.mcc, &mpi->network.mnc);
	}

	/* Prefer %SQP over +CESQ for RSRP on NTN */
	if (sqp_read(&sqp) == 0) {
		sqp_apply(mpi, &sqp);
	}

	return 0;
}

static int shadow_info_update(const char *app_ver)
{
	struct modem_param_info mpi = {0};
	struct nrf_cloud_svc_info_fota fota;
	struct nrf_cloud_svc_info svc_inf;
	struct nrf_cloud_modem_info mdm_inf;
	struct nrf_cloud_device_status ds;
	int err;

	err = shadow_modem_params_get(&mpi);
	if (err) {
		return err;
	}

	mdm_inf = (struct nrf_cloud_modem_info){
		.device = IS_ENABLED(CONFIG_NRF_CLOUD_SEND_DEVICE_STATUS)
				  ? NRF_CLOUD_INFO_SET
				  : NRF_CLOUD_INFO_NO_CHANGE,
		.network = IS_ENABLED(CONFIG_NRF_CLOUD_SEND_DEVICE_STATUS_NETWORK)
				   ? NRF_CLOUD_INFO_SET
				   : NRF_CLOUD_INFO_NO_CHANGE,
		.sim = IS_ENABLED(CONFIG_NRF_CLOUD_SEND_DEVICE_STATUS_SIM)
			       ? NRF_CLOUD_INFO_SET
			       : NRF_CLOUD_INFO_NO_CHANGE,
		.mpi = &mpi,
		.application_version = app_ver,
	};

	fota = (struct nrf_cloud_svc_info_fota){0};
	svc_inf = (struct nrf_cloud_svc_info){
		.ui = NULL,
		.fota = IS_ENABLED(CONFIG_NRF_CLOUD_SEND_SERVICE_INFO_FOTA) ? &fota : NULL,
	};

	if (svc_inf.fota) {
		fota.bootloader = IS_ENABLED(CONFIG_NRF_CLOUD_FOTA_TYPE_BOOT_SUPPORTED);
		fota.application = IS_ENABLED(CONFIG_NRF_CLOUD_FOTA_TYPE_APP_SUPPORTED);
		fota.modem = IS_ENABLED(CONFIG_NRF_CLOUD_FOTA_TYPE_MODEM_DELTA_SUPPORTED);
		fota.modem_full = IS_ENABLED(CONFIG_NRF_CLOUD_FOTA_TYPE_MODEM_FULL_SUPPORTED);
		fota.smp = IS_ENABLED(CONFIG_NRF_CLOUD_FOTA_TYPE_SMP_SUPPORTED);
	}

	ds = (struct nrf_cloud_device_status){
		.modem = &mdm_inf,
		.svc = &svc_inf,
		.conn_inf = IS_ENABLED(CONFIG_NRF_CLOUD_SEND_DEVICE_STATUS_CONN_INF)
				    ? NRF_CLOUD_INFO_SET
				    : NRF_CLOUD_INFO_NO_CHANGE,
	};

	return nrf_cloud_coap_shadow_device_status_update(&ds);
}

/* Send the last known location to nRF Cloud. The CoAP session is paused by the cloud module
 * while the network module is suspended, so it can be resumed here without a new DTLS
 * handshake. It is paused again afterwards, so that the cloud module can resume it once the
 * terrestrial network is available.
 */
static int cloud_location_send(const struct ntn_state_object *state)
{
	int err;
	int pause_err;
	const struct nrf_cloud_gnss_data gnss_data = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = state->location_timestamp,
		.pvt = {
			.lat = state->location.latitude,
			.lon = state->location.longitude,
			.accuracy = state->location.accuracy,
			.alt = state->location.altitude,
			.has_alt = 1,
		},
	};

	err = nrf_cloud_coap_resume();
	if (err) {
		LOG_WRN("nrf_cloud_coap_resume, error: %d", err);

		if (!IS_ENABLED(CONFIG_APP_NTN_CLOUD_FULL_CONNECT)) {
			return err;
		}

		LOG_INF("Connecting to nRF Cloud over NTN");

		err = nrf_cloud_coap_connect(APP_VERSION_STRING);
		if (err) {
			LOG_ERR("nrf_cloud_coap_connect, error: %d", err);

			return err;
		}
	}

	err = shadow_info_update(APP_VERSION_STRING);
	if (err) {
		LOG_WRN("shadow_info_update, error: %d", err);
	} else {
		LOG_DBG("Device shadow info sections sent over NTN");
	}

	/* Confirmable, so that the location has been delivered before the session is saved */
	err = nrf_cloud_coap_location_send(&gnss_data, true);
	if (err) {
		LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
	} else {
		LOG_INF("Location sent to nRF Cloud over NTN");
	}

	pause_err = nrf_cloud_coap_pause();
	if (pause_err) {
		LOG_WRN("nrf_cloud_coap_pause, error: %d", pause_err);
	}

	return err;
}

static void uplink_finish(struct ntn_state_object *state, int err)
{
	service_request_sock_close(state);
	state->pdn_resumed_time = 0;
	state->pdn_activated_pending = false;

	state->last_uplink_uptime = k_uptime_get();

	ntn_status_notify(err ? NTN_UPLINK_FAILED : NTN_UPLINK_SENT);

	smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);
}

static void uplink_perform(struct ntn_state_object *state)
{
	int err;

	if (state->uplink_attempted) {
		return;
	}

	state->uplink_attempted = true;
	state->pdn_resumed_time = 0;
	state->pdn_activated_pending = false;

	err = cloud_location_send(state);

	uplink_finish(state, err);
}

static void uplink_try_when_ready(struct ntn_state_object *state);

static void service_request_if_needed(struct ntn_state_object *state)
{
	int err;

	if (!state->registered) {
		return;
	}

	if (!state->pdn_activated_pending && (state->pdn_resumed_time == 0)) {
		return;
	}

	err = service_request_trigger(state);
	if (err) {
		LOG_WRN("service_request_trigger, error: %d", err);
	}
}

static void data_path_ready(struct ntn_state_object *state)
{
	const bool resume_path = state->pdn_resumed_time > 0;

	/* PDN resume: user data needs a service request and usually a new RRC connection after
	 * the resume. PDN activation on first attach may reuse the RRC connection that stayed up
	 * through registration, which is still valid as long as it never went idle.
	 */
	if (resume_path) {
		state->uplink_rrc_required_after = k_uptime_get();
		LOG_DBG("PDN resumed path ready, service request and RRC after resume required");
	} else {
		state->uplink_rrc_required_after = 0;
		LOG_DBG("PDN activated path ready");
	}

	service_request_if_needed(state);
	uplink_try_when_ready(state);
}

/* CoAP runs only when registered, RRC is connected, and PDN is active (activated or resumed). */
static void uplink_try_when_ready(struct ntn_state_object *state)
{
	const bool resume_path = state->pdn_resumed_time > 0;
	const bool activate_path = state->pdn_activated_pending;

	if (state->uplink_attempted || !state->registered || !state->rrc_connected) {
		return;
	}

	if (resume_path && state->uplink_rrc_required_after != 0 &&
	    state->rrc_connected_uptime < state->uplink_rrc_required_after) {
		return;
	}

	if (!resume_path && !activate_path) {
		return;
	}

	if (resume_path && (state->service_sock_fd < 0)) {
		return;
	}

	if (resume_path) {
		LOG_DBG("RRC connected %d ms after PDN resumed",
			(int32_t)(k_uptime_get() - state->pdn_resumed_time));
	} else {
		LOG_DBG("RRC connected, PDN activated path ready for CoAP");
	}

	uplink_perform(state);
}

static void pdn_activated_handle(struct ntn_state_object *state)
{
	state->pdn_activated_pending = true;

	LOG_DBG("PDN activated");

	if (state->registered) {
		data_path_ready(state);
	}
}

/* State handlers */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	lte_lc_register_handler(lte_lc_evt_handler);
	ntn_register_handler(ntn_evt_handler);
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state->msg_buf;

		if (msg->type == LOCATION_GNSS_DATA) {
			location_store_from_gnss(state, msg);
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SET) {
			location_store(state, &msg->location, NRF_CLOUD_NO_TIMESTAMP);
		}
	}

	return SMF_EVENT_HANDLED;
}

static void state_tn_entry(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	if (!state->tn_connected) {
		(void)k_work_reschedule(&tn_timeout_work, TN_FALLBACK_TIMEOUT);
	}
}

static enum smf_state_result state_tn_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECTED:
			(void)k_work_cancel_delayable(&tn_timeout_work);

			return SMF_EVENT_HANDLED;
		case NETWORK_DISCONNECTED:
			/* Keeps the original deadline if the timer is already running */
			(void)k_work_schedule(&tn_timeout_work, TN_FALLBACK_TIMEOUT);

			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_DONE:
			if (IS_ENABLED(CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE) &&
			    !state->tn_connected) {
				LOG_INF("No terrestrial network found, falling back to NTN");
				smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);
			}

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		if ((msg->type == PRIV_NTN_TN_TIMEOUT) && !state->tn_connected) {
			LOG_INF("No terrestrial network for %d s, falling back to NTN",
				CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS);
			smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);

			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_FALLBACK_START_REQUEST) {
			smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_tn_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&tn_timeout_work);
}

static void state_suspending_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	err = lte_lc_system_mode_get(&state->tn_system_mode, &state->tn_system_mode_pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_get, error: %d", err);

		state->tn_system_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
		state->tn_system_mode_pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
	}

	network_request_send(NETWORK_SUSPEND);
}

static enum smf_state_result state_suspending_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_SUSPENDED) {
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_entry(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	state->tn_retry_pending = false;
	state->uplink_requested = true;

	(void)k_work_reschedule(&tn_retry_work, TN_RETRY_INTERVAL);

	ntn_status_notify(NTN_FALLBACK_STARTED);
}

static enum smf_state_result state_ntn_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		if (msg->type == PRIV_NTN_TN_RETRY) {
			state->tn_retry_pending = true;

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_exit(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&tn_retry_work);
	(void)k_work_cancel_delayable(&uplink_interval_work);

	/* A GNSS search started from NTN idle would otherwise block location requests on TN
	 * until it times out. Ignored by the location module when no search is active.
	 */
	location_request_send(LOCATION_SEARCH_CANCEL);

	tn_mode_restore(state);

	LOG_INF("Searching for a terrestrial network");

	network_request_send(NETWORK_RESUME);
	ntn_status_notify(NTN_FALLBACK_STOPPED);
}

static void state_ntn_idle_entry(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	if (state->tn_retry_pending) {
		smf_set_state(SMF_CTX(state), &states[STATE_TN]);

		return;
	}

	if (state->uplink_requested && location_is_valid(state)) {
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);

		return;
	}

	if (uplink_is_due(state) && location_is_valid(state)) {
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);

		return;
	}

	/* Stay at CFUN=45 until the next uplink or until the location is stale */
	if (location_is_valid(state)) {
		LOG_DBG("Location valid, NTN idle at CFUN=45 until next uplink");
		uplink_interval_schedule(state);

		return;
	}

	gnss_search_trigger();
}

static enum smf_state_result state_ntn_idle_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state->msg_buf;

		switch (msg->type) {
		case LOCATION_GNSS_DATA:
			location_store_from_gnss(state, msg);

			if (uplink_is_due(state)) {
				smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
			}

			return SMF_EVENT_HANDLED;
		case LOCATION_SEARCH_DONE:
			/* No fix, retry the search when the next uplink is due */
			if (!location_is_valid(state)) {
				uplink_interval_schedule(state);
			}

			return SMF_EVENT_HANDLED;
		default:
			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_UPLINK_REQUEST:
			state->uplink_requested = true;

			if (location_is_valid(state)) {
				smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
			} else {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		case NTN_FALLBACK_STOP_REQUEST:
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case PRIV_NTN_TN_RETRY:
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_LOCATION_REQUEST:
			if (!location_is_valid(state)) {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_UPLINK_DUE:
			if (location_is_valid(state)) {
				smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
			} else {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_connecting_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&uplink_interval_work);

	state->uplink_requested = false;
	state->registered = false;
	state->pdn_activated_pending = false;
	state->uplink_attempted = false;
	state->rrc_connected = false;
	state->rrc_connected_uptime = 0;
	state->uplink_rrc_required_after = 0;
	state->pdn_resumed_time = 0;
	state->service_sock_fd = -1;

	serving_cell_valid = false;
	(void)memset(&serving_cell, 0, sizeof(serving_cell));

	atomic_set(&ntn_connecting, 1);
	at_monitor_resume(&ntn_cereg_monitor);

	err = ntn_mode_enter(state);
	if (err) {
		LOG_ERR("Failed to connect to NTN, error: %d", err);

		ntn_status_notify(NTN_UPLINK_FAILED);
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);

		return;
	}

	(void)k_work_reschedule(&connection_timeout_work, CONNECTION_TIMEOUT);
}

static enum smf_state_result state_ntn_connecting_run(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case PRIV_NTN_REGISTERED:
			if (state->registered) {
				return SMF_EVENT_HANDLED;
			}

			LOG_INF("Registered to NTN network");

			state->registered = true;

			(void)k_work_reschedule(&connection_timeout_work, REGISTERED_TIMEOUT);

			if (state->pdn_activated_pending || (state->pdn_resumed_time > 0)) {
				data_path_ready(state);
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_PDN_RESUMED:
			/* PDN resume happens during CFUN activation, often before registration and
			 * RRC.
			 */
			state->pdn_resumed_time = k_uptime_get();

			LOG_DBG("PDN resumed, triggering service request");

			if (state->registered) {
				data_path_ready(state);
			} else {
				err = service_request_trigger(state);
				if (err) {
					LOG_WRN("service_request_trigger, error: %d", err);
				}
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_PDN_ACTIVATED:
			pdn_activated_handle(state);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_PDN_DOWN:
			state->pdn_activated_pending = false;
			service_request_sock_close(state);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_RRC_CONNECTED:
			state->rrc_connected = true;
			state->rrc_connected_uptime = k_uptime_get();

			if (state->registered) {
				LOG_DBG("RRC connected");
				uplink_try_when_ready(state);
			} else {
				LOG_DBG("RRC connected, waiting for NTN registration");
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_SEARCHING:
			(void)k_work_reschedule(&connection_timeout_work, CONNECTION_TIMEOUT);

			LOG_DBG("NTN search in progress, connection timeout refreshed");

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_RRC_IDLE:
			state->rrc_connected = false;

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_LOCATION_REQUEST:
			if (location_is_valid(state)) {
				err = ntn_location_set(state->location.latitude,
						       state->location.longitude,
						       state->location.altitude,
						       location_validity_left(state));
				if (err) {
					LOG_ERR("ntn_location_set, error: %d", err);
				}
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_CONNECTION_TIMEOUT:
			/* The timeout was extended after the message was queued */
			if (k_work_delayable_is_pending(&connection_timeout_work)) {
				return SMF_EVENT_HANDLED;
			}

			LOG_WRN("NTN connection timed out, registered: %d, RRC: %d, PDN act: %d, "
				"PDN resumed: %d",
				state->registered, state->rrc_connected,
				state->pdn_activated_pending, state->pdn_resumed_time > 0);

			/* Avoid an immediate reconnect from NTN idle when nothing has been sent */
			state->last_uplink_uptime = k_uptime_get();

			ntn_status_notify(NTN_UPLINK_FAILED);
			smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_UPLINK_REQUEST:
			return SMF_EVENT_HANDLED;
		case NTN_FALLBACK_STOP_REQUEST:
			LOG_INF("NTN stop requested, aborting NTN connection");

			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_connecting_exit(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&connection_timeout_work);

	at_monitor_pause(&ntn_cereg_monitor);
	atomic_clear(&ntn_connecting);

	service_request_sock_close(state);

	(void)set_ntn_offline_mode();
}

static void tn_connection_status_retain(struct ntn_state_object *state)
{
	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			state->tn_connected = true;
		} else if (msg->type == NETWORK_DISCONNECTED) {
			state->tn_connected = false;
		}
	}
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	/* Place state object in .data section to ensure it is captured in coredumps
	 * and can be inspected by external tools during state analysis.
	 */
	__attribute__((section(".data"))) static struct ntn_state_object ntn_state;

	LOG_DBG("NTN module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&ntn_subscriber, &ntn_state.chan, ntn_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		tn_connection_status_retain(&ntn_state);

		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(ntn_module_thread_id,
		CONFIG_APP_NTN_THREAD_STACK_SIZE,
		ntn_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
