/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <unity.h>
#include <zephyr/fff.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <stdint.h>
#include <stdlib.h>
#include <date_time.h>
#include <time.h>
#include "sgp4_pass_predict.h"

/* missing headers */
struct tm *gmtime_r(const int64_t *timep, struct tm *result);
char *strtok_r(char *str, const char *delim, char **saveptr);

#include "../../../app/src/modules/sgp4/sgp4_pass_predict.c"

DEFINE_FFF_GLOBALS;

/* Thu Feb 12 2026 09:38:29 GMT+0000 */
#define FAKE_TIME_MS 1770889109000LL
int date_time_now(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}

#define LAT_TRD 63.43
#define LON_TRD 10.39
#define ALT_TRD 40.0

static void debug_print_time(const char *label, const int64_t time)
{
	struct tm tm_local;
	char time_str[32];

	gmtime_r(&time, &tm_local);
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_local);
	LOG_DBG("%s: %s", label, time_str);
}

static void debug_print_satrec(struct sat_data *data,  int index)
{
	LOG_DBG("data->satrec[%d].no_kozai: %f", index, data->satrec[index].no_kozai);
	LOG_DBG("data->satrec[%d].ecco: %f", index, data->satrec[index].ecco);
	LOG_DBG("data->satrec[%d].inclo: %f", index, data->satrec[index].inclo);
	LOG_DBG("data->satrec[%d].nodeo: %f", index, data->satrec[index].nodeo);
	LOG_DBG("data->satrec[%d].argpo: %f", index, data->satrec[index].argpo);
	LOG_DBG("data->satrec[%d].mo: %f", index, data->satrec[index].mo);
	LOG_DBG("data->satrec[%d].bstar: %f", index, data->satrec[index].bstar);
	LOG_DBG("data->satrec[%d].jdsatepoch: %f", index, data->satrec[index].jdsatepoch);
	LOG_DBG("data->satrec[%d].jdsatepochF: %f", index, data->satrec[index].jdsatepochF);
}

#define EXPECTED_NEXT_PASS_START_TIME 1770893499LL
#define EXPECTED_NEXT_PASS_END_TIME 1770893629LL
#define EXPECTED_NEXT_PASS_MAX_ELEVATION_TIME 1770893559LL

/* 3GPP TS 36.331: BSTAR = bStarDecimal * 1e-5 * 10^bStarExponent */
#define SIB32_SAT1_BSTAR_DECIMAL	28139
#define SIB32_SAT1_BSTAR_EXPONENT	(-3)
#define SIB32_SAT1_BSTAR_EXPECTED	(28139.0 * 1.0e-5 * 1.0e-3)

void test_bstar_decimal_conversion(void)
{
	double bstar;
	double expected;

	bstar = bStarDecimal2bstar(SIB32_SAT1_BSTAR_DECIMAL, SIB32_SAT1_BSTAR_EXPONENT);
	TEST_ASSERT_FLOAT_WITHIN(1e-8, SIB32_SAT1_BSTAR_EXPECTED, bstar);

	bstar = bStarDecimal2bstar(12345, -4);
	expected = 12345.0 * 1.0e-5 * 1.0e-4;
	TEST_ASSERT_FLOAT_WITHIN(1e-12, expected, bstar);

	bstar = bStarDecimal2bstar(-99999, 0);
	expected = -99999.0 * 1.0e-5;
	TEST_ASSERT_FLOAT_WITHIN(1e-8, expected, bstar);

	bstar = bStarDecimal2bstar(0, 0);
	TEST_ASSERT_FLOAT_WITHIN(1e-15, 0.0, bstar);
}

void test_bstar_sib32_matches_tle(void)
{
	int err;
	static struct sat_data sib_data;
	static struct sat_data tle_data;
	const char *atsib32 =
		"SIBCONFIG: 32,\"00000001\",2,1,1138123,1529334,1391197,758633,"
		"13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,"
		"1399132,2534648,10028,2572728485,19572,-3,121275,,11,11,,,";
	char line1[] = "1 60550U 24149CL  26041.62762187  .00003124  00000+0  27307-3 0  9999";
	char line2[] = "2 60550  97.6859 119.6343 0008059 130.5773 229.6152 14.97467371 81125";

	memset(&sib_data, 0, sizeof(sib_data));
	memset(&tle_data, 0, sizeof(tle_data));

	err = sat_data_init_atsib32(&sib_data, atsib32);
	TEST_ASSERT_EQUAL(0, err);

	err = sat_data_init_tle(&tle_data, line1, line2);
	TEST_ASSERT_EQUAL(0, err);

	TEST_ASSERT_FLOAT_WITHIN(1e-8, SIB32_SAT1_BSTAR_EXPECTED, sib_data.satrec[0].bstar);

	/* TLE BSTAR = 0.27307 * 10^(-3) = 2.7307e-4 */
	TEST_ASSERT_FLOAT_WITHIN(1e-8, 2.7307e-4, tle_data.satrec[0].bstar);
}

void test_nextpass_atsib_sateliot_1(void)
{
	int err;
	struct sat_data data = {0};
	const char *atsib32 = "SIBCONFIG: 32,\"00000001\",2,1,1138123,1529334,1391197,758633,"
		"13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,1399132,2534648,"
		"10028,2572728485,19572,-3,121275,,11,11,,,";

	err = sat_data_init_atsib32(&data, atsib32);
	TEST_ASSERT_EQUAL(0, err);
	debug_print_satrec(&data, 0);
	err = sat_data_calculate_next_pass(&data, 0, LAT_TRD, LON_TRD, ALT_TRD, FAKE_TIME_MS,
		SGP4_DEFAULT_MIN_ELEVATION_DEG);
	TEST_ASSERT_EQUAL(0, err);
	LOG_INF("Next pass: %lld", data.next_pass.start_time_ms);
	LOG_INF("End time: %lld", data.next_pass.end_time_ms);
	LOG_INF("Max elevation time: %lld", data.next_pass.max_elevation_time_ms);

	int64_t start_time = data.next_pass.start_time_ms / 1000;
	int64_t end_time = data.next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data.next_pass.max_elevation_time_ms / 1000;

	debug_print_time("data.next_pass.start_time", start_time);
	debug_print_time("data.next_pass.end_time", end_time);
	debug_print_time("data.next_pass.max_elevation_time", max_elevation_time);

	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_START_TIME, start_time);
	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_END_TIME, end_time);
	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_MAX_ELEVATION_TIME, max_elevation_time);
}

void test_nextpass_tle_sateliot_1(void)
{
	int err;
	char line1[] = "1 60550U 24149CL  26041.62762187  .00003124  00000+0  27307-3 0  9999";
	char line2[] = "2 60550  97.6859 119.6343 0008059 130.5773 229.6152 14.97467371 81125";
	struct sat_data data = {0};

	err = sat_data_init_tle(&data, line1, line2);
	TEST_ASSERT_EQUAL(0, err);
	debug_print_satrec(&data, 0);
	err = sat_data_calculate_next_pass(&data, 0, LAT_TRD, LON_TRD, ALT_TRD, FAKE_TIME_MS,
		SGP4_DEFAULT_MIN_ELEVATION_DEG);
	TEST_ASSERT_EQUAL(0, err);
	LOG_INF("Next pass: %lld", data.next_pass.start_time_ms);
	LOG_INF("End time: %lld", data.next_pass.end_time_ms);
	LOG_INF("Max elevation time: %lld", data.next_pass.max_elevation_time_ms);

	int64_t start_time = data.next_pass.start_time_ms / 1000;
	int64_t end_time = data.next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data.next_pass.max_elevation_time_ms / 1000;

	debug_print_time("data.next_pass.start_time", start_time);
	debug_print_time("data.next_pass.end_time", end_time);
	debug_print_time("data.next_pass.max_elevation_time", max_elevation_time);
	LOG_DBG("TLEs:\n%s\n%s", line1, line2);
	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_START_TIME, start_time);
	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_END_TIME, end_time);
	TEST_ASSERT_EQUAL(EXPECTED_NEXT_PASS_MAX_ELEVATION_TIME, max_elevation_time);
}

extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
