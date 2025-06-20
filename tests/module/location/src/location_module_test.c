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

/* Store the registered event handler */
static location_event_handler_t registered_handler;

/* Custom fake for location_init to store the handler */
static int custom_location_init(location_event_handler_t handler)
{
	registered_handler = handler;
	return 0;
}

/* Helper function to verify location status messages */
static void verify_location_status(enum location_msg_type expected_status)
{
	struct location_msg received_msg;
	int err;

	err = zbus_chan_read(&LOCATION_CHAN, &received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(expected_status, received_msg.type);
}

/* Helper function to verify cellular cloud request payload */
static void verify_cellular_cloud_request(const struct lte_lc_cells_info *expected_cells)
{
	struct location_msg received_msg;
	int err;

	err = zbus_chan_read(&LOCATION_CHAN, &received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(LOCATION_CLOUD_REQUEST, received_msg.type);

	/* Verify cellular data payload */
	TEST_ASSERT_NOT_NULL(received_msg.cloud_request.cell_data);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mcc,
			  received_msg.cloud_request.cell_data->current_cell.mcc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mnc,
			  received_msg.cloud_request.cell_data->current_cell.mnc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.id,
			  received_msg.cloud_request.cell_data->current_cell.id);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.tac,
			  received_msg.cloud_request.cell_data->current_cell.tac);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.earfcn,
			  received_msg.cloud_request.cell_data->current_cell.earfcn);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.phys_cell_id,
			  received_msg.cloud_request.cell_data->current_cell.phys_cell_id);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.rsrp,
			  received_msg.cloud_request.cell_data->current_cell.rsrp);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.rsrq,
			  received_msg.cloud_request.cell_data->current_cell.rsrq);
	TEST_ASSERT_EQUAL(expected_cells->ncells_count,
			  received_msg.cloud_request.cell_data->ncells_count);
	TEST_ASSERT_EQUAL(expected_cells->gci_cells_count,
			  received_msg.cloud_request.cell_data->gci_cells_count);

	/* Verify neighbor cells if present */
	for (int i = 0; i < expected_cells->ncells_count; i++) {
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].earfcn,
			received_msg.cloud_request.cell_data->neighbor_cells[i].earfcn);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].phys_cell_id,
			received_msg.cloud_request.cell_data->neighbor_cells[i].phys_cell_id);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].rsrp,
			received_msg.cloud_request.cell_data->neighbor_cells[i].rsrp);
		TEST_ASSERT_EQUAL(expected_cells->neighbor_cells[i].rsrq,
			received_msg.cloud_request.cell_data->neighbor_cells[i].rsrq);
	}

	/* Verify GCI cells if present */
	for (int i = 0; i < expected_cells->gci_cells_count; i++) {
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].mcc,
			received_msg.cloud_request.cell_data->gci_cells[i].mcc);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].mnc,
			received_msg.cloud_request.cell_data->gci_cells[i].mnc);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].id,
			received_msg.cloud_request.cell_data->gci_cells[i].id);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].tac,
			received_msg.cloud_request.cell_data->gci_cells[i].tac);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].earfcn,
			received_msg.cloud_request.cell_data->gci_cells[i].earfcn);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].phys_cell_id,
			received_msg.cloud_request.cell_data->gci_cells[i].phys_cell_id);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].rsrp,
			received_msg.cloud_request.cell_data->gci_cells[i].rsrp);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].rsrq,
			received_msg.cloud_request.cell_data->gci_cells[i].rsrq);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].timing_advance,
			received_msg.cloud_request.cell_data->gci_cells[i].timing_advance);
		TEST_ASSERT_EQUAL(expected_cells->gci_cells[i].measurement_time,
			received_msg.cloud_request.cell_data->gci_cells[i].measurement_time);
	}
}

/* Helper function to verify Wi-Fi cloud request payload */
static void verify_wifi_cloud_request(const struct wifi_scan_info *expected_wifi)
{
	struct location_msg received_msg;
	int err;

	err = zbus_chan_read(&LOCATION_CHAN, &received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(LOCATION_CLOUD_REQUEST, received_msg.type);

	/* Verify Wi-Fi data payload */
	TEST_ASSERT_NOT_NULL(received_msg.cloud_request.wifi_data);
	TEST_ASSERT_EQUAL(expected_wifi->cnt, received_msg.cloud_request.wifi_data->cnt);

	/* Verify access points */
	for (int i = 0; i < expected_wifi->cnt; i++) {
		TEST_ASSERT_EQUAL(expected_wifi->ap_info[i].ssid_length,
				  received_msg.cloud_request.wifi_data->ap_info[i].ssid_length);
		TEST_ASSERT_EQUAL_STRING_LEN(expected_wifi->ap_info[i].ssid,
					     received_msg.cloud_request.wifi_data->ap_info[i].ssid,
					     expected_wifi->ap_info[i].ssid_length);
		TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_wifi->ap_info[i].mac,
					     received_msg.cloud_request.wifi_data->ap_info[i].mac,
					     expected_wifi->ap_info[i].mac_length);
		TEST_ASSERT_EQUAL(expected_wifi->ap_info[i].channel,
				  received_msg.cloud_request.wifi_data->ap_info[i].channel);
		TEST_ASSERT_EQUAL(expected_wifi->ap_info[i].rssi,
				  received_msg.cloud_request.wifi_data->ap_info[i].rssi);
	}
}

/* Helper function to verify combined cellular and Wi-Fi cloud request payload */
static void verify_combined_cloud_request(const struct lte_lc_cells_info *expected_cells,
					   const struct wifi_scan_info *expected_wifi)
{
	struct location_msg received_msg;
	int err;

	err = zbus_chan_read(&LOCATION_CHAN, &received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(LOCATION_CLOUD_REQUEST, received_msg.type);

	/* Verify both cellular and Wi-Fi data are present */
	TEST_ASSERT_NOT_NULL(received_msg.cloud_request.cell_data);
	TEST_ASSERT_NOT_NULL(received_msg.cloud_request.wifi_data);

	/* Verify cellular data */
	TEST_ASSERT_EQUAL(expected_cells->current_cell.mcc,
			  received_msg.cloud_request.cell_data->current_cell.mcc);
	TEST_ASSERT_EQUAL(expected_cells->current_cell.id,
			  received_msg.cloud_request.cell_data->current_cell.id);
	TEST_ASSERT_EQUAL(expected_cells->ncells_count,
			  received_msg.cloud_request.cell_data->ncells_count);

	/* Verify Wi-Fi data */
	TEST_ASSERT_EQUAL(expected_wifi->cnt, received_msg.cloud_request.wifi_data->cnt);
	TEST_ASSERT_EQUAL_STRING_LEN(expected_wifi->ap_info[0].ssid,
				     received_msg.cloud_request.wifi_data->ap_info[0].ssid,
				     expected_wifi->ap_info[0].ssid_length);
}

/* Helper function to verify A-GNSS request payload */
static void verify_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *expected_agnss)
{
	struct location_msg received_msg;
	int err;

	err = zbus_chan_read(&LOCATION_CHAN, &received_msg, K_MSEC(100));

	TEST_ASSERT_EQUAL(0, err);
	TEST_ASSERT_EQUAL(LOCATION_AGNSS_REQUEST, received_msg.type);

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));
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

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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
	k_sleep(K_MSEC(100));

	/* Simulate location event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

	/* Verify location done message was published */
	verify_location_status(LOCATION_SEARCH_DONE);
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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate cloud location request event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate cloud location request event with multiple cells */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate cloud location request event with GCI cells */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate Wi-Fi location request event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate combined location request event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate A-GNSS assistance request event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate location error event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate location timeout event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

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

	/* Wait for module initialization */
	k_sleep(K_MSEC(100));

	/* Simulate location search started event */
	simulate_location_event(&mock_event);

	/* Give the module time to process */
	k_sleep(K_MSEC(100));

	/* Verify location search started message was published */
	verify_location_status(LOCATION_SEARCH_STARTED);
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
