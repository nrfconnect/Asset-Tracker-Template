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
#include <date_time.h>
#include <time.h>
#include "sgp4_pass_predict.h"

struct tm *gmtime_r(const int64_t *timep,
		    struct tm *result);


DEFINE_FFF_GLOBALS;

LOG_MODULE_REGISTER(sgp4_pass_test, 4);

static struct sat_data sat_data_tle;
static struct sat_data sat_data_sib32;

#define FAKE_TIME_MS 1770122096000

int date_time_now(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}


void test_nextpass_tle_data(void)
{
	char line1[] = "1 99999U 25356A   26026.11000000  .00002000  00000-0  10000-3 0  0001";
	char line2[] = "2 99999   0.0000 000.0000 0005000 180.0000 180.0000 15.09000000 00007";
	static struct sat_data *data = &sat_data_tle;
	int err = sat_data_init_tle(data, line1, line2);
	TEST_ASSERT_EQUAL(0, err);

	err = sat_data_get_next_pass(data, 0.0, 0.0, 0.0, FAKE_TIME_MS);
	int64_t start_time = data->next_pass.start_time_ms / 1000;
	int64_t end_time = data->next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data->next_pass.max_elevation_time_ms / 1000;
	struct tm start_tm, end_tm, max_elevation_tm;

	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);
	LOG_INF("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_INF("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_INF("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
	TEST_ASSERT_EQUAL(0, err);
}

void test_nextpass_sib32_data(void)
{
	/* Test parameters generated from above TLE */
	struct sat_data_sib32 sib32 = {
		.epochStar = 595296,
		.meanMotion = 2592442259,
		.eccentricity = 8388,
		.inclination = 0,
		.rightAscension = 0,
		.argumentPerigee = 2097151,
		.meanAnomaly = 2097151,
		.bStarDecimal = 10000,
		.bStarExponent = -3,
	};
	static struct sat_data *data = &sat_data_sib32;
	int err = sat_data_init_sib32(data, &sib32);
	TEST_ASSERT_EQUAL(0, err);

	err = sat_data_get_next_pass(data, 0.0, 0.0, 0.0, FAKE_TIME_MS);
	int64_t start_time = data->next_pass.start_time_ms / 1000;
	int64_t end_time = data->next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data->next_pass.max_elevation_time_ms / 1000;
	struct tm start_tm, end_tm, max_elevation_tm;
	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);
	LOG_INF("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_INF("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_INF("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
	TEST_ASSERT_EQUAL(0, err);
}

void test_params(void)
{
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.jdsatepoch - sat_data_tle.satrec.jdsatepoch < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.jdsatepochF - sat_data_tle.satrec.jdsatepochF < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.no_kozai - sat_data_tle.satrec.no_kozai < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.ecco - sat_data_tle.satrec.ecco < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.inclo - sat_data_tle.satrec.inclo < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.nodeo - sat_data_tle.satrec.nodeo < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.argpo - sat_data_tle.satrec.argpo < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.mo - sat_data_tle.satrec.mo < 0.000001);
	TEST_ASSERT_TRUE(sat_data_sib32.satrec.bstar - sat_data_tle.satrec.bstar < 0.000001);
}

extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
