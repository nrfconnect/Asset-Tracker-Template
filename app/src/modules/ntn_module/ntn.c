/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <date_time.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>

#include "ntn.h"

/* Socket state */
static int sock_fd = -1;
static struct sockaddr_storage host_addr;

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;

LOG_MODULE_REGISTER(ntn, CONFIG_APP_NTN_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(NTN_CHAN,
		 struct ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.type = NTN_TIMEOUT)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn);

/* Observe NTN channel */
ZBUS_CHAN_ADD_OBS(NTN_CHAN, ntn, 0);

#define MAX_MSG_SIZE sizeof(struct ntn_msg)

/* State machine states */
enum ntn_module_state {
	STATE_RUNNING,
	STATE_GNSS,
	STATE_NTN,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer ntn_timer;
	struct location_data latest_location;
	bool socket_connected;
};

/* Global state for location handler */
static struct ntn_state_object *g_ntn_state;

/* Forward declarations */
static void location_event_handler(const struct location_event_data *event_data);
static void l4_event_handler(struct net_mgmt_event_callback *cb,
			  unsigned long long event,
			  struct net_if *iface);
static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				   unsigned long long event,
				   struct net_if *iface);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);

/* Forward declarations */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_gnss_entry(void *obj);
static void state_gnss_run(void *obj);
static void state_gnss_exit(void *obj);
static void state_ntn_entry(void *obj);
static void state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				NULL, &states[STATE_GNSS]),
	[STATE_GNSS] = SMF_CREATE_STATE(state_gnss_entry, state_gnss_run, state_gnss_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_NTN] = SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				&states[STATE_RUNNING], NULL),
};

/* Helper function to publish NTN messages */
static void ntn_msg_publish(enum ntn_msg_type type)
{
	int err;
	struct ntn_msg msg = {
		.type = type
	};

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN message, error: %d", err);
		return;
	}
}

/* Work item for handling timer timeout */
static struct k_work timer_work;

static void timer_work_handler(struct k_work *work)
{
	ntn_msg_publish(NTN_TIMEOUT);
}

/* Timer callback for NTN mode timeout */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&timer_work);
}


/* Helper functions */

static int set_ntn_dormant_mode(void)
{
	int err;

	/* Set modem to flight mode without shutting down UICC */
	err = nrf_modem_at_printf("AT+CFUN=44");
	if (err) {
		LOG_ERR("Failed to set AT+CFUN=44, error: %d", err);
		return err;
	}

	return 0;
}

/* Move into lte_lc / conn_mgr*/
static int set_ntn_active_mode(void)
{
	int err;

	/* Set modem to minimum functionality */
	err = nrf_modem_at_printf("AT+CFUN=0");
	if (err) {
		LOG_ERR("Failed to set modem to minimum functionality, error: %d", err);
		return err;
	}

	/* Configure network registration status reporting */
	err = nrf_modem_at_printf("AT+CEREG=5");
	if (err) {
		LOG_ERR("Failed to configure network registration reporting, error: %d", err);
		return err;
	}

	/* Configure network event reporting */
	err = nrf_modem_at_printf("AT+CNEC=24");
	if (err) {
		LOG_ERR("Failed to configure network event reporting, error: %d", err);
		return err;
	}

	/* Configure signaling connection status reporting */
	err = nrf_modem_at_printf("AT+CSCON=3");
	if (err) {
		LOG_ERR("Failed to configure signaling connection reporting, error: %d", err);
		return err;
	}

	/* Configure modem event reporting */
	err = nrf_modem_at_printf("AT%%MDMEV=2");
	if (err) {
		LOG_ERR("Failed to configure modem event reporting, error: %d", err);
		return err;
	}

	/* Configure NTN system mode */
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
	if (err) {
		LOG_ERR("Failed to set NTN system mode, error: %d", err);
		return err;
	}

	/* Configure location using latest GNSS data */
	err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"20.0\",0,0",
				g_ntn_state->latest_location.latitude,
				g_ntn_state->latest_location.longitude);
	if (err) {
		LOG_ERR("Failed to set location, error: %d", err);
		return err;
	}


#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=2,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to set NTN band lock, error: %d", err);
		return err;
	}
#endif

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = nrf_modem_at_printf("AT%%CHSELECT=1,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to set NTN channel, error: %d", err);
		return err;
	}
#endif

#if defined(CONFIG_APP_NTN_APN)
	err = nrf_modem_at_printf("AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_APP_NTN_APN);
	if (err) {
		LOG_ERR("Failed to set NTN APN, error: %d", err);
		return err;
	}
#endif

#if defined(CONFIG_APP_NTN_DISABLE_EPCO)
	err = nrf_modem_at_printf("AT%%XEPCO=0");
	if (err) {
		LOG_ERR("Failed to set XEPCO=0, error: %d", err);
		return err;
	}
#endif

	return 0;
}

static int set_gnss_active_mode(void)
{
	int err;

	/* Set modem to offline mode */
	err = nrf_modem_at_printf("AT+CFUN=4");
	if (err) {
		LOG_ERR("Failed to set modem to offline mode, error: %d", err);
		return err;
	}

	/* Configure GNSS system mode */
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
	if (err) {
		LOG_ERR("Failed to set GNSS system mode, error: %d", err);
		return err;
	}

	/* Activate GNSS mode */
	err = nrf_modem_at_printf("AT+CFUN=31");
	if (err) {
		LOG_ERR("Failed to activate GNSS mode, error: %d", err);
		return err;
	}

	return 0;
}

static int set_gnss_inactive_mode(void)
{
	int err;

	/* Set modem to CFUN=30 mode when exiting GNSS state */
	err = nrf_modem_at_printf("AT+CFUN=30");
	if (err) {
		LOG_ERR("Failed to set modem to CFUN=30 mode, error: %d", err);
		return;
	}
}

/* Socket functions */
static int sock_open_and_connect(void)
{
	int err;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_APP_NTN_SERVER_PORT);
	
	inet_pton(AF_INET, CONFIG_APP_NTN_SERVER_ADDR, &server4->sin_addr);

	/* Create UDP socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);
		return -errno;
	}

	/* Connect socket */
	err = connect(sock_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(sock_fd);
		sock_fd = -1;
		return -errno;
	}

	return 0;
}

static int sock_send_gnss_data(const struct location_data *gnss_data)
{
	int err;
	char message[256];

	if (sock_fd < 0) {
		LOG_ERR("Socket not connected");
		return -ENOTCONN;
	}

	/* Format GNSS data as string */
	snprintf(message, sizeof(message),
		"GNSS: lat=%.6f, lon=%.6f, acc=%.2f, time=%04d-%02d-%02d %02d:%02d:%02d",
		gnss_data->latitude, gnss_data->longitude, gnss_data->accuracy,
		gnss_data->datetime.year, gnss_data->datetime.month, gnss_data->datetime.day,
		gnss_data->datetime.hour, gnss_data->datetime.minute, gnss_data->datetime.second);

	/* Send data */
	err = send(sock_fd, message, strlen(message), 0);
	if (err < 0) {
		LOG_ERR("Failed to send GNSS data, error: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent GNSS data payload of %d bytes", strlen(message));
	return 0;
}


/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	
	LOG_INF("Initializing NTN module");

	k_work_init(&timer_work, timer_work_handler);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);
	k_timer_start(&state->ntn_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);

	err = location_init(location_event_handler);
	if (err) {
		LOG_ERR("Failed to initialize location library: %d", err);
		return;
	}

	/* Setup handler for Zephyr NET Connection Manager events */
	net_mgmt_init_event_callback(&l4_cb, &l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	/* Setup handler for Zephyr NET Connection Manager Connectivity layer */
	net_mgmt_init_event_callback(&conn_cb, &connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

	/* Connecting to the configured connectivity layer. */
	LOG_INF("Bringing network interface up and connecting to the network");

	err = conn_mgr_all_if_up(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_up, error: %d", err);
		return;
	}

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);
}

static void state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_TIMEOUT) {
			/* Timer expired, restart timer and transition to GNSS mode */
			k_timer_start(&state->ntn_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
			LOG_INF("Timer timeout, transitioning to GNSS mode");
		}
	}
}

static void state_gnss_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	/* Close socket if it was open */
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
		state->socket_connected = false;
	}

	LOG_INF("Entering GNSS mode");

	err = set_gnss_active_mode();
	if (err) {
		LOG_ERR("Unable to set GNSS mode");
		return;
	}

	err = location_request(NULL);
	if (err) {
		LOG_ERR("Failed to request location: %d", err);
		return;
	}
}

static void state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SEARCH_DONE) {
			/* Location search completed, transition to NTN mode */
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);
		}
	}
}

static void state_gnss_exit(void *obj)
{
	int err;

	LOG_INF("Exiting GNSS mode");

	set_gnss_inactive_mode();
}

static void state_ntn_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_INF("Entering NTN mode");

	err = set_ntn_active_mode();
	if (err) {
		return;
	}

	/* Connect to network */
	err = conn_mgr_all_if_connect(true);
	if (err) {
		LOG_ERR("Failed to connect to network, error: %d", err);
		return;
	}

}

static void state_ntn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_NETWORK_CONNECTED) {
			LOG_DBG("Received NTN_NETWORK_CONNECTED, setting up socket");
			/* Network is connected, set up socket */
			err = sock_open_and_connect();
			if (err) {
				LOG_ERR("Failed to connect socket, error: %d", err);
				state->socket_connected = false;
			} else {
				LOG_DBG("Socket connected successfully");
				state->socket_connected = true;
				/* Send initial GNSS data if available */
				if (state->latest_location.datetime.valid) {
					LOG_DBG("Sending initial GNSS data");
					err = sock_send_gnss_data(&state->latest_location);
					if (err) {
						LOG_ERR("Failed to send initial GNSS data, error: %d", err);
					} else {
						LOG_DBG("Initial GNSS data sent successfully");
					}
				} else {
					LOG_DBG("No valid GNSS data available to send initially");
				}
			}
			err = set_ntn_dormant_mode();
			if (err) {
				return;
			}
		}
	}
}

static void state_ntn_exit(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	/* Close socket if it was open */
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
		state->socket_connected = false;
	}
}


/* Network event handlers */
static void l4_event_handler(struct net_mgmt_event_callback *cb,
			  unsigned long long event,
			  struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_DBG("Network connectivity established, publishing NTN_NETWORK_CONNECTED");
		ntn_msg_publish(NTN_NETWORK_CONNECTED);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_DBG("Network connectivity lost");
		break;
	default:
		break;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				   unsigned long long event,
				   struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		LOG_ERR("NET_EVENT_CONN_IF_FATAL_ERROR");
		return;
	}
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
		}
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
		} else if (evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE");
		}
		break;
	default:
		break;
	}
}

static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	struct tm gnss_time = {
		.tm_year = pvt_data->datetime.year - 1900,
		.tm_mon = pvt_data->datetime.month - 1,
		.tm_mday = pvt_data->datetime.day,
		.tm_hour = pvt_data->datetime.hour,
		.tm_min = pvt_data->datetime.minute,
		.tm_sec = pvt_data->datetime.seconds,
	};

	date_time_set(&gnss_time);
}

static void location_print_data_details(enum location_method method,
				    const struct location_data_details *details)
{
	LOG_DBG("Elapsed method time: %d ms", details->elapsed_time_method);

#if defined(CONFIG_LOCATION_METHOD_GNSS)
	if (method == LOCATION_METHOD_GNSS) {
		LOG_DBG("Satellites tracked: %d", details->gnss.satellites_tracked);
		LOG_DBG("Satellites used: %d", details->gnss.satellites_used);
		LOG_DBG("Elapsed GNSS time: %d ms", details->gnss.elapsed_time_gnss);
		LOG_DBG("GNSS execution time: %d ms", details->gnss.pvt_data.execution_time);
	}
#endif
}

static void location_event_handler(const struct location_event_data *event_data)
{
	struct ntn_msg msg;

	if (!g_ntn_state) {
		LOG_ERR("NTN state not initialized");
		return;
	}

	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		LOG_INF("Got location: lat: %f, lon: %f, acc: %f, method: %s",
			(double)event_data->location.latitude,
			(double)event_data->location.longitude,
			(double)event_data->location.accuracy,
			location_method_str(event_data->method));

		if (event_data->method == LOCATION_METHOD_GNSS) {
			struct nrf_modem_gnss_pvt_data_frame pvt_data =
				event_data->location.details.gnss.pvt_data;
			if (event_data->location.datetime.valid) {
				/* GNSS is the most accurate time source -  use it. */
				apply_gnss_time(&pvt_data);
			} else {
				/* this should not happen */
				LOG_WRN("Got GNSS location without valid time data");
			}

			/* Store latest GNSS data and send location search done message */
			g_ntn_state->latest_location = event_data->location;
			ntn_msg_publish(NTN_LOCATION_SEARCH_DONE);
		}
		break;

	case LOCATION_EVT_TIMEOUT:
		LOG_WRN("Getting location timed out");
		break;

	case LOCATION_EVT_ERROR:
		LOG_WRN("Getting location failed");
		LOG_WRN("Used method: %s (%d)", location_method_str(event_data->method),
					    event_data->method);
		location_print_data_details(event_data->method, &event_data->error.details);
		break;

	case LOCATION_EVT_FALLBACK:
		LOG_DBG("Location request fallback has occurred:");
		LOG_DBG("Failed method: %s (%d)", location_method_str(event_data->method),
					      event_data->method);
		LOG_DBG("New method: %s (%d)", location_method_str(
					    event_data->fallback.next_method),
					    event_data->fallback.next_method);
		LOG_DBG("Cause: %s",
			(event_data->fallback.cause == LOCATION_EVT_TIMEOUT) ? "timeout" :
			(event_data->fallback.cause == LOCATION_EVT_ERROR) ? "error" :
			"unknown");
		location_print_data_details(event_data->method, &event_data->fallback.details);
		break;

	case LOCATION_EVT_RESULT_UNKNOWN:
		LOG_DBG("Location result unknown");
		break;

	default:
		break;
	}
}

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("NTN watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	struct ntn_state_object ntn_state = { 0 };
	g_ntn_state = &ntn_state;

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		return;
	}

	/* Initialize state machine */
	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			return;
		}

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn, &ntn_state.chan, ntn_state.msg_buf, K_FOREVER);
		if (err) {
			LOG_ERR("Failed to receive message, error: %d", err);
			continue;
		}

		/* Run state machine */
		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("Failed to run state machine, error: %d", err);
			continue;
		}
	}
}

K_THREAD_DEFINE(ntn_module_thread_id,
		CONFIG_APP_NTN_THREAD_STACK_SIZE,
		ntn_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
