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

#include "../../../app/src/modules/sgp4/sgp4_pass_predict.c"

/* missing headers */
struct tm *gmtime_r(const int64_t *timep, struct tm *result);
char *strtok_r(char *str, const char *delim, char **saveptr);

DEFINE_FFF_GLOBALS;

static struct sat_data sat_data_tle;

/* Thu Feb 12 2026 13:32:09 GMT+0000 */
#define FAKE_TIME_MS 1770903129082LL

int date_time_now(int64_t *time)
{
	*time = FAKE_TIME_MS;
	return 0;
}

static void debug_print_next_pass(struct sat_data *data)
{
	struct tm start_tm, end_tm, max_elevation_tm;
	int64_t start_time = data->next_pass.start_time_ms / 1000;
	int64_t end_time = data->next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data->next_pass.max_elevation_time_ms / 1000;

	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);

	LOG_DBG("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_DBG("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_DBG("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
}

void test_jd_from_unix_time_ms(void)
{
	double jd, jdfract, jdcombined;
	jd_from_unix_time_ms(FAKE_TIME_MS, &jd, &jdfract);
	jdcombined = jd + jdfract;
	TEST_ASSERT_EQUAL(FAKE_TIME_MS, (int64_t)((jdcombined - 2440587.5) * 86400000LL));
}

void test_nextpass_tle_data(void)
{
	/* Epoch time: Thu Feb 12 2026 13:32:09 GMT+0000 */
	char line1[] = "1 99999U 25356A   26026.11000000  .00002000  00000-0  10000-3 0  0001";
	char line2[] = "2 99999   0.0000 000.0000 0005000 180.0000 180.0000 15.09000000 00007";
	static struct sat_data *data = &sat_data_tle;
	int err = sat_data_init_tle(data, line1, line2);
	TEST_ASSERT_EQUAL(0, err);

	err = sat_data_calculate_next_pass(data, 0, 0.0, 0.0, 0.0, FAKE_TIME_MS);
	TEST_ASSERT_EQUAL(0, err);

	int64_t start_time = data->next_pass.start_time_ms / 1000;
	int64_t end_time = data->next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data->next_pass.max_elevation_time_ms / 1000;
	struct tm start_tm, end_tm, max_elevation_tm;

	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);
	LOG_DBG("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_DBG("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_DBG("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
	TEST_ASSERT_EQUAL(0, err);
}


void test_parse_at_sibconfig(void)
{
	struct sat_data satellite;
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",1,0,0,2097151,0,2097151,8388,2592442259,10000,-3,595296,5,,,,,";
	TEST_ASSERT_EQUAL(0, sat_data_init_atsid32(&satellite, atsib32));
	TEST_ASSERT_EQUAL(0, sat_data_set_name(&satellite, "01A2D101"));
	TEST_ASSERT_EQUAL(0, sat_data_calculate_next_pass(&satellite, 0, 0.0, 0.0, 0.0, FAKE_TIME_MS));
	debug_print_next_pass(&satellite);
}

void test_parse_at_sibconfig_full(void)
{
	struct sat_data satellite;
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",1,0,0,2097151,0,2097151,8388,2592442259,10000,-3,595296,5,10,20,30,40,50";
	TEST_ASSERT_EQUAL(0, sat_data_init_atsid32(&satellite, atsib32));
	TEST_ASSERT_EQUAL(0, sat_data_set_name(&satellite, "01A2D101"));
	TEST_ASSERT_EQUAL(0, sat_data_calculate_next_pass(&satellite, 0, 0.0, 0.0, 0.0, FAKE_TIME_MS));
	debug_print_next_pass(&satellite);
}

void test_atsib_parser_single_sib(void)
{
	struct sat_data_sib32 *sib32;
	char cell_id[9];
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",1,909,13,14,15,16,17,18,5000000000,-5,99,100,10,20,30,40,50";
	
	int sibcount = parse_sibconfig32_at(atsib32, cell_id, NULL);
	TEST_ASSERT_EQUAL(1, sibcount);
	TEST_ASSERT_EQUAL(0, strncmp(cell_id, "01A2D101", 8));
	sib32 = malloc(sizeof(struct sat_data_sib32) * sibcount);
	sibcount = parse_sibconfig32_at(atsib32, cell_id, sib32);
	TEST_ASSERT_EQUAL(1, sibcount);
	TEST_ASSERT_EQUAL(909, sib32[0].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[0].inclination);
	TEST_ASSERT_EQUAL(14, sib32[0].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[0].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[0].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[0].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[0].meanMotion);
	TEST_ASSERT_EQUAL(5000000000, sib32[0].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[0].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[0].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[0].serviceStart);
	TEST_ASSERT_EQUAL(10, sib32[0].elevationAngleLeft);
	TEST_ASSERT_EQUAL(20, sib32[0].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[0].referencePointLongitude);
	TEST_ASSERT_EQUAL(40, sib32[0].referencePointLatitude);
	TEST_ASSERT_EQUAL(50, sib32[0].radius);
	free(sib32);
}

void test_atsib_parser_single_sib_empty_end(void)
{
	struct sat_data_sib32 *sib32;
	char cell_id[9];
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",1,909,13,14,15,16,17,18,5000000000,-5,99,100,,,30,40,,";
	
	int sibcount = parse_sibconfig32_at(atsib32, cell_id, NULL);
	TEST_ASSERT_EQUAL(1, sibcount);
	TEST_ASSERT_EQUAL(0, strncmp(cell_id, "01A2D101", 8));
	sib32 = malloc(sizeof(struct sat_data_sib32) * sibcount);
	sibcount = parse_sibconfig32_at(atsib32, cell_id, sib32);
	TEST_ASSERT_EQUAL(1, sibcount);
	TEST_ASSERT_EQUAL(909, sib32[0].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[0].inclination);
	TEST_ASSERT_EQUAL(14, sib32[0].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[0].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[0].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[0].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[0].meanMotion);
	TEST_ASSERT_EQUAL(5000000000, sib32[0].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[0].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[0].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[0].serviceStart);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[0].elevationAngleLeft);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[0].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[0].referencePointLongitude);
	TEST_ASSERT_EQUAL(40, sib32[0].referencePointLatitude);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[0].radius);
	free(sib32);
}

void test_atsib_parser_multiple_sibs(void)
{
	struct sat_data_sib32 *sib32;
	char cell_id[9];
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",2,909,13,14,15,16,17,18,5000000000,-5,99,100,10,20,30,40,50,808,13,14,15,16,17,18,5000000000,-5,99,100,10,20,30,40,50";
	
	int sibcount = parse_sibconfig32_at(atsib32, cell_id, NULL);
	TEST_ASSERT_EQUAL(2, sibcount);
	TEST_ASSERT_EQUAL(0, strncmp(cell_id, "01A2D101", 8));
	sib32 = malloc(sizeof(struct sat_data_sib32) * sibcount);
	sibcount = parse_sibconfig32_at(atsib32, cell_id, sib32);
	TEST_ASSERT_EQUAL(2, sibcount);

	TEST_ASSERT_EQUAL(909, sib32[0].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[0].inclination);
	TEST_ASSERT_EQUAL(14, sib32[0].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[0].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[0].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[0].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[0].meanMotion);
	TEST_ASSERT_EQUAL(5000000000, sib32[0].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[0].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[0].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[0].serviceStart);
	TEST_ASSERT_EQUAL(10, sib32[0].elevationAngleLeft);
	TEST_ASSERT_EQUAL(20, sib32[0].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[0].referencePointLongitude);
	TEST_ASSERT_EQUAL(40, sib32[0].referencePointLatitude);
	TEST_ASSERT_EQUAL(50, sib32[0].radius);

	TEST_ASSERT_EQUAL(808, sib32[1].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[1].inclination);
	TEST_ASSERT_EQUAL(14, sib32[1].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[1].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[1].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[1].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[1].meanMotion);
	TEST_ASSERT_EQUAL(5000000000, sib32[1].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[1].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[1].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[1].serviceStart);
	TEST_ASSERT_EQUAL(10, sib32[1].elevationAngleLeft);
	TEST_ASSERT_EQUAL(20, sib32[1].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[1].referencePointLongitude);
	TEST_ASSERT_EQUAL(40, sib32[1].referencePointLatitude);
	TEST_ASSERT_EQUAL(50, sib32[1].radius);
	free(sib32);
}

void test_atsib_parser_multiple_sibs_empty_end(void)
{
	struct sat_data_sib32 *sib32;
	char cell_id[9];
	const char *atsib32 = "SIBCONFIG: 32,\"01A2D101\",2,909,13,14,15,16,17,18,5000000000,-5,99,100,10,20,30,,,808,13,14,15,16,17,18,3000000000,-5,99,100,10,20,30,40,";
	
	int sibcount = parse_sibconfig32_at(atsib32, cell_id, NULL);
	TEST_ASSERT_EQUAL(2, sibcount);
	TEST_ASSERT_EQUAL(0, strncmp(cell_id, "01A2D101", 8));
	sib32 = malloc(sizeof(struct sat_data_sib32) * sibcount);
	sibcount = parse_sibconfig32_at(atsib32, cell_id, sib32);
	TEST_ASSERT_EQUAL(2, sibcount);

	TEST_ASSERT_EQUAL(909, sib32[0].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[0].inclination);
	TEST_ASSERT_EQUAL(14, sib32[0].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[0].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[0].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[0].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[0].meanMotion);
	TEST_ASSERT_EQUAL(5000000000, sib32[0].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[0].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[0].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[0].serviceStart);
	TEST_ASSERT_EQUAL(10, sib32[0].elevationAngleLeft);
	TEST_ASSERT_EQUAL(20, sib32[0].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[0].referencePointLongitude);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[0].referencePointLatitude);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[0].radius);

	TEST_ASSERT_EQUAL(808, sib32[1].satelliteId);
	TEST_ASSERT_EQUAL(13, sib32[1].inclination);
	TEST_ASSERT_EQUAL(14, sib32[1].argumentPerigee);
	TEST_ASSERT_EQUAL(15, sib32[1].rightAscension);
	TEST_ASSERT_EQUAL(16, sib32[1].meanAnomaly);
	TEST_ASSERT_EQUAL(17, sib32[1].eccentricity);
	TEST_ASSERT_EQUAL(18, sib32[1].meanMotion);
	TEST_ASSERT_EQUAL(3000000000, sib32[1].bStarDecimal);
	TEST_ASSERT_EQUAL(-5, sib32[1].bStarExponent);
	TEST_ASSERT_EQUAL(99, sib32[1].epochStar);
	TEST_ASSERT_EQUAL(100, sib32[1].serviceStart);
	TEST_ASSERT_EQUAL(10, sib32[1].elevationAngleLeft);
	TEST_ASSERT_EQUAL(20, sib32[1].elevationAngleRight);
	TEST_ASSERT_EQUAL(30, sib32[1].referencePointLongitude);
	TEST_ASSERT_EQUAL(40, sib32[1].referencePointLatitude);
	TEST_ASSERT_EQUAL(-INT64_MAX, sib32[1].radius);
	free(sib32);
}

#define LAT_TRD 63.446827
#define LON_TRD 10.421906
#define ALT_TRD 40

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

void test_nextpass_atsib_sateliot_1(void)
{
	struct sat_data data;
	const char *atsib32 = "SIBCONFIG: 32,\"00000001\",2,1,1138123,1529334,1391197,758633,13719,2572629918,28139,-3,120679,,11,11,,,,3,1138188,1686202,1399132,2534648,10028,2572728485,19572,-3,121275,,11,11,,,";
	
	int err = sat_data_init_atsid32(&data, atsib32);
	TEST_ASSERT_EQUAL(0, err);
	debug_print_satrec(&data, 0);
	err = sat_data_calculate_next_pass(&data, 0, LAT_TRD, LON_TRD, ALT_TRD, FAKE_TIME_MS + 10000000);
	TEST_ASSERT_EQUAL(0, err);

	int64_t start_time = data.next_pass.start_time_ms / 1000;
	int64_t end_time = data.next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data.next_pass.max_elevation_time_ms / 1000;
	struct tm start_tm, end_tm, max_elevation_tm;

	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);
	LOG_DBG("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_DBG("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_DBG("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
	TEST_ASSERT_EQUAL(0, err);
}

void test_nextpass_tle_sateliot_1(void)
{
	char line1[] = "1 60550U 24149CL  26041.62762187  .00003124  00000+0  27307-3 0  9999";
	char line2[] = "2 60550  97.6859 119.6343 0008059 130.5773 229.6152 14.97467371 81125";
	static struct sat_data *data = &sat_data_tle;
	int err = sat_data_init_tle(data, line1, line2);
	TEST_ASSERT_EQUAL(0, err);
	debug_print_satrec(data, 0);
	err = sat_data_calculate_next_pass(data, 0, LAT_TRD, LON_TRD, ALT_TRD, FAKE_TIME_MS);
	TEST_ASSERT_EQUAL(0, err);

	int64_t start_time = data->next_pass.start_time_ms / 1000;
	int64_t end_time = data->next_pass.end_time_ms / 1000;
	int64_t max_elevation_time = data->next_pass.max_elevation_time_ms / 1000;
	int64_t now_time;
	date_time_now(&now_time);
	now_time = now_time / 1000;
	struct tm start_tm, end_tm, max_elevation_tm, nowtime;

	gmtime_r(&now_time, &nowtime);
	gmtime_r(&start_time, &start_tm);
	gmtime_r(&end_time, &end_tm);
	gmtime_r(&max_elevation_time, &max_elevation_tm);
	LOG_DBG("data->next_pass.start_time: %s", asctime(&start_tm));
	LOG_DBG("data->next_pass.end_time: %s", asctime(&end_tm));
	LOG_DBG("data->next_pass.max_elevation_time: %s", asctime(&max_elevation_tm));
	LOG_DBG("now_time: %s", asctime(&nowtime));
	LOG_DBG("TLEs:\n%s\n%s", line1, line2);
}

extern int unity_main(void);

int main(void)
{
	/* use the runner from test_runner_generate() */
	(void)unity_main();

	return 0;
}
