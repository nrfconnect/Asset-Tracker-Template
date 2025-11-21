/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <modem/location.h>
#include <modem/lte_lc.h>
#include <zephyr/net/wifi_mgmt.h>

#include "app_common.h"
#include "location.h"

LOG_MODULE_REGISTER(location_module_test, LOG_LEVEL_DBG);

DEFINE_FFF_GLOBALS;

/* Define subscriber for this module */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_subscriber);

/* Add observers for the channels */
ZBUS_CHAN_ADD_OBS(LOCATION_CHAN, test_subscriber, 0);

/* Create fakes for required functions */
FAKE_VALUE_FUNC(int, location_init, location_event_handler_t);
FAKE_VALUE_FUNC(int, location_request, const struct location_config *);
FAKE_VALUE_FUNC(int, location_request_cancel);
FAKE_VOID_FUNC(location_cloud_location_ext_result_set, enum location_ext_result,
	       struct location_data *);
FAKE_VALUE_FUNC(int, location_agnss_data_process, const char *, size_t);
FAKE_VALUE_FUNC(int, location_pgps_data_process, const char *, size_t);
FAKE_VALUE_FUNC(int, task_wdt_feed, int);
FAKE_VALUE_FUNC(int, task_wdt_add, uint32_t, task_wdt_callback_t, void *);
FAKE_VALUE_FUNC(int, date_time_set, const struct tm *);
FAKE_VALUE_FUNC(const char *, location_method_str, enum location_method);
FAKE_VALUE_FUNC(int, lte_lc_func_mode_set, enum lte_lc_func_mode);

/* Store the registered event handler */
static location_event_handler_t registered_handler;

/* NRF_MODEM_LIB_ON_INIT creates a structure with the callback.
 * We can access it to invoke the modem init callback in tests.
 */
struct nrf_modem_lib_init_cb {
	void (*callback)(int ret, void *ctx);
	void *context;
};
extern struct nrf_modem_lib_init_cb nrf_modem_hook_location_modem_init_hook;

/* Custom fake for location_init to store the handler */
static int custom_location_init(location_event_handler_t handler)
{
	registered_handler = handler;
	return 0;
}

/* Common helper functions for message handling */
static int wait_for_message(enum location_msg_type expected_type, struct location_msg *received_msg)
{
	const struct zbus_channel *chan;
	int err = zbus_sub_wait_msg(&test_subscriber, &chan, received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL_PTR(&LOCATION_CHAN, chan);
	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(expected_type, received_msg->type);

	return err;
}

static void consume_published_message(enum location_msg_type expected_type)
{
	struct location_msg consumed_msg;

	wait_for_message(expected_type, &consumed_msg);
}

static void verify_search_done_follows(void)
{
	struct location_msg received_msg;

	wait_for_message(LOCATION_SEARCH_DONE, &received_msg);
}

/* Common timing helpers */
static void wait_for_initialization(void)
{
	k_sleep(K_MSEC(100));
}

static void wait_for_processing(void)
{
	k_sleep(K_MSEC(100));
}

/* Helper for publishing and consuming test messages */
static void publish_and_consume_message(enum location_msg_type msg_type)
{
	struct location_msg msg = { .type = msg_type };

	zbus_chan_pub(&LOCATION_CHAN, &msg, K_NO_WAIT);
	wait_for_processing();
	consume_published_message(msg_type);
}

/* Helper function to verify location status messages */
static void verify_location_status(enum location_msg_type expected_status)
{
	struct location_msg received_msg;

	wait_for_message(expected_status, &received_msg);
}

/* Helper function to verify cellular cloud request payload */
static void verify_cellular_cloud_request(const struct lte_lc_cells_info *expected_cells)
{
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify cellular data payload */
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mcc,
			  received_msg.cloud_request.current_cell.mcc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mnc,
			  received_msg.cloud_request.current_cell.mnc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.id,
			  received_msg.cloud_request.current_cell.id);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.tac,
			  received_msg.cloud_request.current_cell.tac);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.earfcn,
			  received_msg.cloud_request.current_cell.earfcn);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.rsrp,
			  received_msg.cloud_request.current_cell.rsrp);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.rsrq,
			  received_msg.cloud_request.current_cell.rsrq);
	TEST_ASSERT_EQUAL(expected_cells->ncells_count,
			  received_msg.cloud_request.ncells_count);
	TEST_ASSERT_EQUAL(expected_cells->gci_cells_count,
			  received_msg.cloud_request.gci_cells_count);

	/* Verify neighbor cells if present */
	for (int i = 0; i < expected_cells->ncells_count; i++) {
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].earfcn,
			received_msg.cloud_request.neighbor_cells[i].earfcn);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].phys_cell_id,
			received_msg.cloud_request.neighbor_cells[i].phys_cell_id);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].rsrp,
			received_msg.cloud_request.neighbor_cells[i].rsrp);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].rsrq,
			received_msg.cloud_request.neighbor_cells[i].rsrq);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].time_diff,
			received_msg.cloud_request.neighbor_cells[i].time_diff);
	}

	/* Verify GCI cells if present */
	for (int i = 0; i < expected_cells->gci_cells_count; i++) {
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].mcc,
			received_msg.cloud_request.gci_cells[i].mcc);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].mnc,
			received_msg.cloud_request.gci_cells[i].mnc);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].id,
			received_msg.cloud_request.gci_cells[i].id);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].tac,
			received_msg.cloud_request.gci_cells[i].tac);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].earfcn,
			received_msg.cloud_request.gci_cells[i].earfcn);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].rsrp,
			received_msg.cloud_request.gci_cells[i].rsrp);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].rsrq,
			received_msg.cloud_request.gci_cells[i].rsrq);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].timing_advance,
			received_msg.cloud_request.gci_cells[i].timing_advance);
	}

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* Helper function to verify Wi-Fi cloud request payload */
static void verify_wifi_cloud_request(const struct wifi_scan_info *expected_wifi)
{
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify Wi-Fi data payload */
	TEST_ASSERT_EQUAL(expected_wifi->cnt, received_msg.cloud_request.wifi_cnt);

	/* Verify access points */
	for (int i = 0; i < expected_wifi->cnt; i++) {
		TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_wifi->ap_info[i].mac,
					     received_msg.cloud_request.wifi_aps[i].mac,
					     expected_wifi->ap_info[i].mac_length);
		TEST_ASSERT_EQUAL(expected_wifi->ap_info[i].rssi,
				  received_msg.cloud_request.wifi_aps[i].rssi);
		TEST_ASSERT_EQUAL(expected_wifi->ap_info[i].mac_length,
				  received_msg.cloud_request.wifi_aps[i].mac_length);
	}

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* Helper function to verify combined cellular and Wi-Fi cloud request payload */
static void verify_combined_cloud_request(const struct lte_lc_cells_info *expected_cells,
					   const struct wifi_scan_info *expected_wifi)
{
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify cellular data is present */
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mcc,
			  received_msg.cloud_request.current_cell.mcc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.id,
			  received_msg.cloud_request.current_cell.id);
	TEST_ASSERT_EQUAL(expected_cells->ncells_count,
			  received_msg.cloud_request.ncells_count);

	/* Verify Wi-Fi data is present */
	TEST_ASSERT_EQUAL(expected_wifi->cnt, received_msg.cloud_request.wifi_cnt);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_wifi->ap_info[0].mac,
				     received_msg.cloud_request.wifi_aps[0].mac,
				     expected_wifi->ap_info[0].mac_length);

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* Helper function to verify A-GNSS request payload */
static void verify_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *expected_agnss)
{
	struct location_msg received_msg;
	wait_for_message(LOCATION_AGNSS_REQUEST, &received_msg);

	/* Verify A-GNSS data payload */
	TEST_ASSERT_EQUAL(expected_agnss->data_flags, received_msg.agnss_request.data_flags);
	TEST_ASSERT_EQUAL(expected_agnss->system_count, received_msg.agnss_request.system_count);

	/* Verify system data */
	for (int i = 0; i < expected_agnss->system_count; i++) {
		TEST_ASSERT_EQUAL(expected_agnss->system[i].system_id,
				  received_msg.agnss_request.system[i].system_id);
		TEST_ASSERT_EQUAL(expected_agnss->system[i].sv_mask_ephe,
				  received_msg.agnss_request.system[i].sv_mask_ephe);
		TEST_ASSERT_EQUAL(expected_agnss->system[i].sv_mask_alm,
				  received_msg.agnss_request.system[i].sv_mask_alm);
	}
}

/* Helper function to verify GNSS location data payload */
static void verify_gnss_location_data(const struct location_data *expected_location)
{
	struct location_msg received_msg;
	int err;
	const struct zbus_channel *chan;

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &received_msg, K_MSEC(100));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(LOCATION_GNSS_DATA, received_msg.type);

	/* Verify GNSS location data payload */
	TEST_ASSERT_EQUAL_DOUBLE(expected_location->latitude, received_msg.gnss_data.latitude);
	TEST_ASSERT_EQUAL_DOUBLE(expected_location->longitude, received_msg.gnss_data.longitude);
	TEST_ASSERT_EQUAL_DOUBLE(expected_location->accuracy, received_msg.gnss_data.accuracy);
	TEST_ASSERT_EQUAL(expected_location->datetime.valid, received_msg.gnss_data.datetime.valid);

	if (expected_location->datetime.valid) {
		TEST_ASSERT_EQUAL(expected_location->datetime.year,
				  received_msg.gnss_data.datetime.year);
		TEST_ASSERT_EQUAL(expected_location->datetime.month,
				  received_msg.gnss_data.datetime.month);
		TEST_ASSERT_EQUAL(expected_location->datetime.day,
				  received_msg.gnss_data.datetime.day);
		TEST_ASSERT_EQUAL(expected_location->datetime.hour,
				  received_msg.gnss_data.datetime.hour);
		TEST_ASSERT_EQUAL(expected_location->datetime.minute,
				  received_msg.gnss_data.datetime.minute);
		TEST_ASSERT_EQUAL(expected_location->datetime.second,
				  received_msg.gnss_data.datetime.second);
		TEST_ASSERT_EQUAL(expected_location->datetime.ms,
				  received_msg.gnss_data.datetime.ms);
	}

	err = zbus_sub_wait_msg(&test_subscriber, &chan, &received_msg, K_MSEC(100));
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_EQUAL(LOCATION_SEARCH_DONE, received_msg.type);
}

/* Helper function to simulate location events through the registered handler */
static void simulate_location_event(const struct location_event_data *event_data)
{
	registered_handler(event_data);
}

void setUp(void)
{
	/* Reset all fakes */
	RESET_FAKE(location_init);
	RESET_FAKE(location_request);
	RESET_FAKE(location_request_cancel);
	RESET_FAKE(location_cloud_location_ext_result_set);
	RESET_FAKE(location_agnss_data_process);
	RESET_FAKE(location_pgps_data_process);
	RESET_FAKE(task_wdt_feed);
	RESET_FAKE(task_wdt_add);
	RESET_FAKE(date_time_set);
	RESET_FAKE(location_method_str);

	/* Set up custom fakes */
	location_init_fake.custom_fake = custom_location_init;
	location_method_str_fake.return_val = "TEST_METHOD";

	/* Set default return values */
	location_init_fake.return_val = 0;
	location_request_fake.return_val = 0;
	location_request_cancel_fake.return_val = 0;
	location_agnss_data_process_fake.return_val = 0;
	location_pgps_data_process_fake.return_val = 0;
	task_wdt_feed_fake.return_val = 0;
	task_wdt_add_fake.return_val = 1;
	date_time_set_fake.return_val = 0;

	const struct zbus_channel *chan;
	struct location_msg received_msg;

	while (zbus_sub_wait_msg(&test_subscriber, &chan, &received_msg, K_NO_WAIT) == 0) {
		/* Purge all messages from the channel */
	}

	/* Initialize the location module by calling the modem init callback
	 * via the hook structure
	 */
	nrf_modem_hook_location_modem_init_hook.callback(0,
		nrf_modem_hook_location_modem_init_hook.context);

	wait_for_initialization();
}

/* Test location library initialization */
void test_module_init(void)
{
	/* Verify that location_init was called */
	TEST_ASSERT_EQUAL(1, location_init_fake.call_count);
	TEST_ASSERT_NOT_NULL(registered_handler);
}

/* Test location search trigger handling */
void test_location_search_trigger(void)
{
	struct location_msg msg = {
		.type = LOCATION_SEARCH_TRIGGER
	};

	/* Reset location_request fake */
	RESET_FAKE(location_request);
	location_request_fake.return_val = 0;

	/* Publish trigger message */
	zbus_chan_pub(&LOCATION_CHAN, &msg, K_NO_WAIT);

	wait_for_processing();

	/* Verify location request was made */
	TEST_ASSERT_EQUAL(1, location_request_fake.call_count);
	TEST_ASSERT_NULL(location_request_fake.arg0_val); /* No config provided */
}

/* Test basic location event handler */
void test_location_event_handler_basic(void)
{
	struct location_data mock_location = {
		.latitude = 63.421,
		.longitude = 10.437,
		.accuracy = 5.0,
		.datetime.valid = true,
		.datetime.year = 2025,
		.datetime.month = 1,
		.datetime.day = 15,
		.datetime.hour = 12,
		.datetime.minute = 30,
		.datetime.second = 45,
		.datetime.ms = 0
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_LOCATION,
		.method = LOCATION_METHOD_GNSS,
		.location = mock_location
	};

	/* Wait for module initialization */
	wait_for_initialization();

	/* Simulate location event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	wait_for_processing();

	/* Verify GNSS location data was published first for GNSS events */
	verify_gnss_location_data(&mock_location);
}

/* Test GNSS location data is sent to cloud when GNSS location is obtained */
void test_gnss_location_data_sent_to_cloud(void)
{
	struct nrf_modem_gnss_pvt_data_frame mock_pvt_data = {
		.latitude = 63.421,   /* 63.421 degrees */
		.longitude = 10.437,  /* 10.437 degrees */
		.accuracy = 5.0,      /* 5.0 meters */
		.datetime = {
			.year = 2025,
			.month = 1,
			.day = 15,
			.hour = 12,
			.minute = 30,
			.seconds = 45
		},
		.execution_time = 30000
	};
	struct location_data mock_location = {
		.latitude = 63.421,
		.longitude = 10.437,
		.accuracy = 5.0,
		.datetime.valid = true,
		.datetime.year = 2025,
		.datetime.month = 1,
		.datetime.day = 15,
		.datetime.hour = 12,
		.datetime.minute = 30,
		.datetime.second = 45,
		.datetime.ms = 0,
		.details.gnss.pvt_data = mock_pvt_data,
		.details.gnss.satellites_tracked = 8,
		.details.gnss.satellites_used = 2,  /* Adjust to match test expectation */
		.details.gnss.elapsed_time_gnss = 30000
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_LOCATION,
		.method = LOCATION_METHOD_GNSS,
		.location = mock_location
	};

	/* Simulate GNSS location event */
	simulate_location_event(&mock_event);

	/* Verify GNSS location data */
	verify_gnss_location_data(&mock_location);

	wait_for_processing();

	/* Verify date_time_set was called with GNSS time */
	TEST_ASSERT_EQUAL(1, date_time_set_fake.call_count);
}

/* Test external cloud location request handling */
void test_cloud_location_request(void)
{
	struct lte_lc_cell mock_cell = {
		.mcc = 242,
		.mnc = 1,
		.id = 12345,
		.tac = 678,
		.earfcn = 1234,
		.timing_advance = 0,
		.timing_advance_meas_time = 0,
		.measurement_time = 0,
		.phys_cell_id = 123,
		.rsrp = -80,
		.rsrq = -10
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_cell,
		.ncells_count = 0,
		.gci_cells_count = 0
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location request event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published */
	verify_cellular_cloud_request(&mock_cells_info);
}

/* Test cellular location with multiple neighbor cells */
void test_cloud_location_request_multiple_cells(void)
{
	struct lte_lc_cell mock_current_cell = {
		.mcc = 242,
		.mnc = 1,
		.id = 12345,
		.tac = 678,
		.earfcn = 1234,
		.timing_advance = 0,
		.timing_advance_meas_time = 0,
		.measurement_time = 0,
		.phys_cell_id = 123,
		.rsrp = -80,
		.rsrq = -10
	};
	struct lte_lc_ncell mock_neighbor_cells[3] = {
		{
			.earfcn = 1235,
			.phys_cell_id = 124,
			.rsrp = -85,
			.rsrq = -12,
			.time_diff = 100
		},
		{
			.earfcn = 1236,
			.phys_cell_id = 125,
			.rsrp = -90,
			.rsrq = -15,
			.time_diff = 200
		},
		{
			.earfcn = 1237,
			.phys_cell_id = 126,
			.rsrp = -95,
			.rsrq = -18,
			.time_diff = 300
		}
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_current_cell,
		.ncells_count = 3,
		.neighbor_cells = mock_neighbor_cells,
		.gci_cells_count = 0
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location request event with multiple cells */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	wait_for_processing();

	/* Verify cloud location request message was published */
	verify_cellular_cloud_request(&mock_cells_info);
}

/* Test cellular location with GCI cells */
void test_cloud_location_request_with_gci_cells(void)
{
	struct lte_lc_cell mock_current_cell = {
		.mcc = 242,
		.mnc = 1,
		.id = 12345,
		.tac = 678,
		.earfcn = 1234,
		.timing_advance = 0,
		.timing_advance_meas_time = 0,
		.measurement_time = 0,
		.phys_cell_id = 123,
		.rsrp = -80,
		.rsrq = -10
	};
	struct lte_lc_cell mock_gci_cells[3] = {
		{
			.mcc = 242,
			.mnc = 1,
			.id = 0x01234567,
			.tac = 0x0102,
			.earfcn = 999999,
			.timing_advance = 65534,
			.timing_advance_meas_time = 18446744073709551614U,
			.measurement_time = 18446744073709551614U,
			.phys_cell_id = 123,
			.rsrp = 127,
			.rsrq = -127
		},
		{
			.mcc = 242,
			.mnc = 1,
			.id = 0x02345678,
			.tac = 0x0102,
			.earfcn = 888888,
			.timing_advance = 65533,
			.timing_advance_meas_time = 18446744073709551613U,
			.measurement_time = 18446744073709551613U,
			.phys_cell_id = 124,
			.rsrp = 126,
			.rsrq = -126
		},
		{
			.mcc = 242,
			.mnc = 2,
			.id = 0x03456789,
			.tac = 0x0103,
			.earfcn = 777777,
			.timing_advance = 65532,
			.timing_advance_meas_time = 18446744073709551612U,
			.measurement_time = 18446744073709551612U,
			.phys_cell_id = 125,
			.rsrp = 125,
			.rsrq = -125
		}
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_current_cell,
		.ncells_count = 0,
		.gci_cells_count = 3,
		.gci_cells = mock_gci_cells
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location request event with GCI cells */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published */
	verify_cellular_cloud_request(&mock_cells_info);
}

/* Test Wi-Fi location request handling */
void test_wifi_location_request(void)
{
	struct wifi_scan_result mock_wifi_aps[4] = {
		{
			.ssid_length = 8,
			.ssid = "TestAP_1",
			.mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 6,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -45
		},
		{
			.ssid_length = 8,
			.ssid = "TestAP_2",
			.mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 11,
			.security = WIFI_SECURITY_TYPE_SAE,
			.rssi = -52
		},
		{
			.ssid_length = 8,
			.ssid = "TestAP_3",
			.mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x77},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_5_GHZ,
			.channel = 44,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -38
		},
		{
			.ssid_length = 8,
			.ssid = "TestAP_4",
			.mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x88},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_5_GHZ,
			.channel = 149,
			.security = WIFI_SECURITY_TYPE_SAE,
			.rssi = -41
		}
	};
	struct wifi_scan_info mock_wifi_info = {
		.ap_info = mock_wifi_aps,
		.cnt = 4
	};
	struct location_data_cloud mock_cloud_request = {
		.wifi_data = &mock_wifi_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_WIFI,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate Wi-Fi location request event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published */
	verify_wifi_cloud_request(&mock_wifi_info);
}

/* Test combined cellular and Wi-Fi location request */
void test_combined_location_request(void)
{
	struct lte_lc_cell mock_cell = {
		.mcc = 242,
		.mnc = 1,
		.id = 54321,
		.tac = 987,
		.earfcn = 2345,
		.timing_advance = 50,
		.timing_advance_meas_time = 1000,
		.measurement_time = 2000,
		.phys_cell_id = 234,
		.rsrp = -75,
		.rsrq = -8
	};
	struct lte_lc_ncell mock_neighbor_cells[2] = {
		{
			.earfcn = 2346,
			.phys_cell_id = 235,
			.rsrp = -82,
			.rsrq = -11,
			.time_diff = 150
		},
		{
			.earfcn = 2347,
			.phys_cell_id = 236,
			.rsrp = -88,
			.rsrq = -14,
			.time_diff = 250
		}
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_cell,
		.ncells_count = 2,
		.neighbor_cells = mock_neighbor_cells,
		.gci_cells_count = 0
	};
	struct wifi_scan_result mock_wifi_aps[2] = {
		{
			.ssid_length = 10,
			.ssid = "CombinedAP1",
			.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 1,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -42
		},
		{
			.ssid_length = 10,
			.ssid = "CombinedAP2",
			.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_5_GHZ,
			.channel = 44,
			.security = WIFI_SECURITY_TYPE_SAE,
			.rssi = -35
		}
	};
	struct wifi_scan_info mock_wifi_info = {
		.ap_info = mock_wifi_aps,
		.cnt = 2
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info,
		.wifi_data = &mock_wifi_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_WIFI_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate combined location request event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published */
	verify_combined_cloud_request(&mock_cells_info, &mock_wifi_info);
}

/* Test A-GNSS assistance request handling */
void test_agnss_request(void)
{
	struct nrf_modem_gnss_agnss_data_frame mock_agnss_request = {
		.data_flags = NRF_MODEM_GNSS_AGNSS_GPS_UTC_REQUEST |
			      NRF_MODEM_GNSS_AGNSS_KLOBUCHAR_REQUEST,
		.system_count = 1,
		.system = {
			{
				.system_id = NRF_MODEM_GNSS_SYSTEM_GPS,
				.sv_mask_ephe = 0x1234,
				.sv_mask_alm = 0x5678
			}
		}
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_GNSS_ASSISTANCE_REQUEST,
		.method = LOCATION_METHOD_GNSS,
		.agnss_request = mock_agnss_request
	};

	wait_for_initialization();

	/* Simulate A-GNSS assistance request event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify A-GNSS request message was published */
	verify_agnss_request(&mock_agnss_request);
}

/* Test location error handling */
void test_location_error_handling(void)
{
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_ERROR,
		.method = LOCATION_METHOD_GNSS
	};

	wait_for_initialization();

	/* Simulate location error event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify location done message was published (error is treated as completion) */
	verify_location_status(LOCATION_SEARCH_DONE);
}

/* Test location timeout handling */
void test_location_timeout_handling(void)
{
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_TIMEOUT,
		.method = LOCATION_METHOD_GNSS
	};

	wait_for_initialization();

	/* Simulate location timeout event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify location done message was published (timeout is treated as completion) */
	verify_location_status(LOCATION_SEARCH_DONE);
}

/* Test location search started event */
void test_location_search_started(void)
{
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_STARTED,
		.method = LOCATION_METHOD_GNSS
	};

	wait_for_initialization();

	/* Simulate location search started event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify location search started message was published */
	verify_location_status(LOCATION_SEARCH_STARTED);
}

/* Test that non-GNSS location events don't send GNSS location data */
void test_cellular_location_no_gnss_data_sent(void)
{
	struct location_data mock_location = {
		.latitude = 63.421,
		.longitude = 10.437,
		.accuracy = 500.0,
		.datetime.valid = false
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_LOCATION,
		.method = LOCATION_METHOD_CELLULAR,
		.location = mock_location
	};

	wait_for_initialization();

	/* Simulate cellular location event */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify only location done message was published (no GNSS data) */
	verify_location_status(LOCATION_SEARCH_DONE);

	/* Verify date_time_set was NOT called for cellular location */
	TEST_ASSERT_EQUAL(0, date_time_set_fake.call_count);
}

/* Test location search cancellation when location search is inactive */
void test_location_cancel_when_inactive(void)
{
	struct location_msg msg = {
		.type = LOCATION_SEARCH_CANCEL
	};

	/* Publish cancel message while in inactive state */
	zbus_chan_pub(&LOCATION_CHAN, &msg, K_NO_WAIT);

	/* Give the module time to process */
	wait_for_processing();

	/* Verify that location_request_cancel was NOT called since we're inactive */
	TEST_ASSERT_EQUAL(0, location_request_cancel_fake.call_count);

	/* Verify that location_request was NOT called */
	TEST_ASSERT_EQUAL(0, location_request_fake.call_count);
}

/* Test location search cancellation when location search is active */
void test_location_cancel_when_active(void)
{
	struct location_msg trigger_msg = {
		.type = LOCATION_SEARCH_TRIGGER
	};
	struct location_msg cancel_msg = {
		.type = LOCATION_SEARCH_CANCEL
	};

	/* Start location search */
	zbus_chan_pub(&LOCATION_CHAN, &trigger_msg, K_NO_WAIT);
	wait_for_processing();

	/* Consume the trigger message that was published */
	consume_published_message(LOCATION_SEARCH_TRIGGER);

	/* Verify location search started */
	TEST_ASSERT_EQUAL(1, location_request_fake.call_count);

	/* Cancel location search */
	zbus_chan_pub(&LOCATION_CHAN, &cancel_msg, K_NO_WAIT);
	wait_for_processing();

	/* Consume the cancel message that was published */
	consume_published_message(LOCATION_SEARCH_CANCEL);

	/* Verify location_request_cancel was called */
	TEST_ASSERT_EQUAL(1, location_request_cancel_fake.call_count);

	/* Verify location search done status was sent */
	verify_location_status(LOCATION_SEARCH_DONE);
}

/* Test multiple cancel requests during active search */
void test_location_multiple_cancel_requests(void)
{
	/* Start location search */
	publish_and_consume_message(LOCATION_SEARCH_TRIGGER);

	/* Send first cancel */
	publish_and_consume_message(LOCATION_SEARCH_CANCEL);

	/* Verify first cancel was processed */
	TEST_ASSERT_EQUAL(1, location_request_cancel_fake.call_count);
	verify_location_status(LOCATION_SEARCH_DONE);

	/* Send second cancel (should be ignored as we're now inactive) */
	publish_and_consume_message(LOCATION_SEARCH_CANCEL);

	/* Verify no additional cancel calls were made */
	TEST_ASSERT_EQUAL(1, location_request_cancel_fake.call_count);
}

/* Test cancellation followed by new search trigger */
void test_location_cancel_then_new_search(void)
{
	/* Start first location search */
	publish_and_consume_message(LOCATION_SEARCH_TRIGGER);
	TEST_ASSERT_EQUAL(1, location_request_fake.call_count);

	/* Cancel location search */
	publish_and_consume_message(LOCATION_SEARCH_CANCEL);
	TEST_ASSERT_EQUAL(1, location_request_cancel_fake.call_count);
	verify_location_status(LOCATION_SEARCH_DONE);

	/* Start new location search after cancellation */
	publish_and_consume_message(LOCATION_SEARCH_TRIGGER);

	/* Verify second location request was made */
	TEST_ASSERT_EQUAL(2, location_request_fake.call_count);
}

/* Test that cellular-only cloud location request data is correctly copied */
void test_cloud_location_ext_request_cellular_only(void)
{
	struct lte_lc_cell mock_cell = {
		.mcc = 242,
		.mnc = 1,
		.id = 0x12345678,
		.tac = 0x1234,
		.earfcn = 6200,
		.timing_advance = 100,
		.timing_advance_meas_time = 1000,
		.measurement_time = 2000,
		.phys_cell_id = 42,
		.rsrp = -85,
		.rsrq = -12
	};
	struct lte_lc_ncell mock_neighbor_cells[2] = {
		{
			.earfcn = 6201,
			.phys_cell_id = 43,
			.rsrp = -90,
			.rsrq = -15,
			.time_diff = 50
		},
		{
			.earfcn = 6202,
			.phys_cell_id = 44,
			.rsrp = -95,
			.rsrq = -18,
			.time_diff = 100
		}
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_cell,
		.ncells_count = 2,
		.neighbor_cells = mock_neighbor_cells,
		.gci_cells_count = 0
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info,
		.wifi_data = NULL
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location external request event with cellular data only */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published with correct cellular data */
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify current cell data was correctly copied */
	TEST_ASSERT_EQUAL(mock_cell.mcc, received_msg.cloud_request.current_cell.mcc);
	TEST_ASSERT_EQUAL(mock_cell.mnc, received_msg.cloud_request.current_cell.mnc);
	TEST_ASSERT_EQUAL(mock_cell.id, received_msg.cloud_request.current_cell.id);
	TEST_ASSERT_EQUAL(mock_cell.tac, received_msg.cloud_request.current_cell.tac);
	TEST_ASSERT_EQUAL(mock_cell.earfcn, received_msg.cloud_request.current_cell.earfcn);
	TEST_ASSERT_EQUAL(mock_cell.timing_advance,
			  received_msg.cloud_request.current_cell.timing_advance);
	TEST_ASSERT_EQUAL(mock_cell.rsrp, received_msg.cloud_request.current_cell.rsrp);
	TEST_ASSERT_EQUAL(mock_cell.rsrq, received_msg.cloud_request.current_cell.rsrq);

	/* Verify neighbor cell count */
	TEST_ASSERT_EQUAL(2, received_msg.cloud_request.ncells_count);

	/* Verify neighbor cells were correctly copied */
	for (int i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL(mock_neighbor_cells[i].earfcn,
				  received_msg.cloud_request.neighbor_cells[i].earfcn);
		TEST_ASSERT_EQUAL(mock_neighbor_cells[i].phys_cell_id,
				  received_msg.cloud_request.neighbor_cells[i].phys_cell_id);
		TEST_ASSERT_EQUAL(mock_neighbor_cells[i].rsrp,
				  received_msg.cloud_request.neighbor_cells[i].rsrp);
		TEST_ASSERT_EQUAL(mock_neighbor_cells[i].rsrq,
				  received_msg.cloud_request.neighbor_cells[i].rsrq);
		TEST_ASSERT_EQUAL(mock_neighbor_cells[i].time_diff,
				  received_msg.cloud_request.neighbor_cells[i].time_diff);
	}

	/* Verify no Wi-Fi data */
	TEST_ASSERT_EQUAL(0, received_msg.cloud_request.wifi_cnt);
	TEST_ASSERT_EQUAL(0, received_msg.cloud_request.gci_cells_count);

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* Test that Wi-Fi-only cloud location request data is correctly copied */
void test_cloud_location_ext_request_wifi_only(void)
{
	struct wifi_scan_result mock_wifi_aps[3] = {
		{
			.ssid_length = 8,
			.ssid = "TestAP_1",
			.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 1,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -50
		},
		{
			.ssid_length = 8,
			.ssid = "TestAP_2",
			.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x22},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 6,
			.security = WIFI_SECURITY_TYPE_SAE,
			.rssi = -55
		},
		{
			.ssid_length = 8,
			.ssid = "TestAP_3",
			.mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x33},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_5_GHZ,
			.channel = 36,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -60
		}
	};
	struct wifi_scan_info mock_wifi_info = {
		.ap_info = mock_wifi_aps,
		.cnt = 3
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = NULL,
		.wifi_data = &mock_wifi_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_WIFI,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location external request event with Wi-Fi data only */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published with correct Wi-Fi data */
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify Wi-Fi AP count */
	TEST_ASSERT_EQUAL(3, received_msg.cloud_request.wifi_cnt);

	/* Verify all Wi-Fi APs were correctly copied */
	for (int i = 0; i < 3; i++) {
		TEST_ASSERT_EQUAL_HEX8_ARRAY(mock_wifi_aps[i].mac,
					     received_msg.cloud_request.wifi_aps[i].mac,
					     mock_wifi_aps[i].mac_length);
		TEST_ASSERT_EQUAL(mock_wifi_aps[i].rssi,
				  received_msg.cloud_request.wifi_aps[i].rssi);
		TEST_ASSERT_EQUAL(mock_wifi_aps[i].mac_length,
				  received_msg.cloud_request.wifi_aps[i].mac_length);
	}

	/* Verify no cellular data */
	TEST_ASSERT_EQUAL(0, received_msg.cloud_request.ncells_count);
	TEST_ASSERT_EQUAL(0, received_msg.cloud_request.gci_cells_count);

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* Test that combined cellular and Wi-Fi cloud location request data is correctly copied */
void test_cloud_location_ext_request_cellular_and_wifi(void)
{
	struct lte_lc_cell mock_cell = {
		.mcc = 242,
		.mnc = 2,
		.id = 0x87654321,
		.tac = 0x5678,
		.earfcn = 3400,
		.timing_advance = 200,
		.timing_advance_meas_time = 3000,
		.measurement_time = 4000,
		.phys_cell_id = 99,
		.rsrp = -75,
		.rsrq = -9
	};
	struct lte_lc_ncell mock_neighbor_cells[1] = {
		{
			.earfcn = 3401,
			.phys_cell_id = 100,
			.rsrp = -80,
			.rsrq = -11,
			.time_diff = 25
		}
	};
	struct lte_lc_cells_info mock_cells_info = {
		.current_cell = mock_cell,
		.ncells_count = 1,
		.neighbor_cells = mock_neighbor_cells,
		.gci_cells_count = 0
	};
	struct wifi_scan_result mock_wifi_aps[2] = {
		{
			.ssid_length = 10,
			.ssid = "CombinedAP1",
			.mac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_2_4_GHZ,
			.channel = 11,
			.security = WIFI_SECURITY_TYPE_PSK,
			.rssi = -45
		},
		{
			.ssid_length = 10,
			.ssid = "CombinedAP2",
			.mac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x77},
			.mac_length = 6,
			.band = WIFI_FREQ_BAND_5_GHZ,
			.channel = 48,
			.security = WIFI_SECURITY_TYPE_SAE,
			.rssi = -40
		}
	};
	struct wifi_scan_info mock_wifi_info = {
		.ap_info = mock_wifi_aps,
		.cnt = 2
	};
	struct location_data_cloud mock_cloud_request = {
		.cell_data = &mock_cells_info,
		.wifi_data = &mock_wifi_info
	};
	struct location_event_data mock_event = {
		.id = LOCATION_EVT_CLOUD_LOCATION_EXT_REQUEST,
		.method = LOCATION_METHOD_WIFI_CELLULAR,
		.cloud_location_request = mock_cloud_request
	};

	wait_for_initialization();

	/* Simulate cloud location external request event with both cellular and Wi-Fi data */
	simulate_location_event(&mock_event);

	wait_for_processing();

	/* Verify cloud location request message was published with both data types */
	struct location_msg received_msg;
	wait_for_message(LOCATION_CLOUD_REQUEST, &received_msg);

	/* Verify cellular data was correctly copied */
	TEST_ASSERT_EQUAL(mock_cell.mcc, received_msg.cloud_request.current_cell.mcc);
	TEST_ASSERT_EQUAL(mock_cell.mnc, received_msg.cloud_request.current_cell.mnc);
	TEST_ASSERT_EQUAL(mock_cell.id, received_msg.cloud_request.current_cell.id);
	TEST_ASSERT_EQUAL(mock_cell.tac, received_msg.cloud_request.current_cell.tac);
	TEST_ASSERT_EQUAL(mock_cell.earfcn, received_msg.cloud_request.current_cell.earfcn);
	TEST_ASSERT_EQUAL(mock_cell.rsrp, received_msg.cloud_request.current_cell.rsrp);
	TEST_ASSERT_EQUAL(mock_cell.rsrq, received_msg.cloud_request.current_cell.rsrq);
	TEST_ASSERT_EQUAL(1, received_msg.cloud_request.ncells_count);

	/* Verify neighbor cell */
	TEST_ASSERT_EQUAL(mock_neighbor_cells[0].earfcn,
			  received_msg.cloud_request.neighbor_cells[0].earfcn);
	TEST_ASSERT_EQUAL(mock_neighbor_cells[0].phys_cell_id,
			  received_msg.cloud_request.neighbor_cells[0].phys_cell_id);
	TEST_ASSERT_EQUAL(mock_neighbor_cells[0].rsrp,
			  received_msg.cloud_request.neighbor_cells[0].rsrp);
	TEST_ASSERT_EQUAL(mock_neighbor_cells[0].rsrq,
			  received_msg.cloud_request.neighbor_cells[0].rsrq);
	TEST_ASSERT_EQUAL(mock_neighbor_cells[0].time_diff,
			  received_msg.cloud_request.neighbor_cells[0].time_diff);

	/* Verify Wi-Fi data was correctly copied */
	TEST_ASSERT_EQUAL(2, received_msg.cloud_request.wifi_cnt);

	for (int i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_HEX8_ARRAY(mock_wifi_aps[i].mac,
					     received_msg.cloud_request.wifi_aps[i].mac,
					     mock_wifi_aps[i].mac_length);
		TEST_ASSERT_EQUAL(mock_wifi_aps[i].rssi,
				  received_msg.cloud_request.wifi_aps[i].rssi);
		TEST_ASSERT_EQUAL(mock_wifi_aps[i].mac_length,
				  received_msg.cloud_request.wifi_aps[i].mac_length);
	}

	/* Verify GCI cells count */
	TEST_ASSERT_EQUAL(0, received_msg.cloud_request.gci_cells_count);

	/* Verify that search done message follows */
	verify_search_done_follows();
}

/* This is required to be added to each test. That is because unity's
 * main may return nonzero, while zephyr's main currently must
 * return 0 in all cases (other values are reserved).
 */
extern int unity_main(void);

int main(void)
{
	(void)unity_main();

	return 0;
}
