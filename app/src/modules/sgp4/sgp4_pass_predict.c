/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include "sgp4_pass_predict.h"
#include "SGP4.h"
#include <math.h>
#include <date_time.h>
#include <modem/at_monitor.h>
#include <zephyr/logging/log.h>

#define DEG2RAD (3.14159265358979323846 / 180.0)
#define RAD2DEG (180.0 / 3.14159265358979323846)
#define XKMPER 6378.137 // Earth radius in km
#define F (1.0/298.257223563) // Flattening

LOG_MODULE_REGISTER(sgp4_pass_predict, 4);

/* missing headers */
struct tm *gmtime_r(const int64_t *timep, struct tm *result);
char *strtok_r(char *str, const char *delim, char **saveptr);

/* Internal datetime structure */
struct datetime {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	double second;
};

/* 3GPP TS 36.331 ephemeris parameters format */
struct sat_data_sib32 {
	int64_t satelliteId;
	int64_t epochStar;
	int64_t meanMotion;
	int64_t eccentricity;
	int64_t inclination;
	int64_t rightAscension;
	int64_t argumentPerigee;
	int64_t meanAnomaly;
	int64_t bStarDecimal;
	int64_t bStarExponent;
	int64_t serviceStart;
	int64_t elevationAngleLeft;
	int64_t elevationAngleRight;
	int64_t referencePointLongitude;
	int64_t referencePointLatitude;
	int64_t radius;
};

/* SIBCONFIG 32 AT ephemeris struct field order */
enum sib_ephemeris_field {
	FIELD_SATELLITE_ID = 0,
	FIELD_INCLINATION = 1,
	FIELD_ARG_PERIGEE = 2,
	FIELD_RIGHT_ASCENSION = 3,
	FIELD_MEAN_ANOMALY = 4,
	FIELD_ECCENTRICITY = 5,
	FIELD_MEAN_MOTION = 6,
	FIELD_B_STAR_DECIMAL = 7,
	FIELD_B_STAR_EXPONENT = 8,
	FIELD_EPOCH_STAR = 9,
	FIELD_SERVICE_START = 10,
	FIELD_ELEVATION_ANGLE_LEFT = 11,
	FIELD_ELEVATION_ANGLE_RIGHT = 12,
	FIELD_REFERENCE_POINT_LONGITUDE = 13,
	FIELD_REFERENCE_POINT_LATITUDE = 14,
	FIELD_RADIUS = 15,
	FIELD_COUNT = 16,
};

/* Converting parameters to ASN1 format (see 3GPP TS 36.331 )*/
static double meanMotion2no_kozai(int64_t meanMotion) {
	/* Rev/day to rad/s */
	return meanMotion * (99.99999999 / 17179869183) / (1440.0 / (2.0 * pi));
}
static double eccentricity2ecco(int64_t eccentricity) {
	return eccentricity * (0.9999999 / 16777215.0);
}
static double inclination2inclo(int64_t inclination) {
	return inclination * (180.0 / 2097151) * deg2rad;
}
static double rightAscension2nodeo(int64_t rightAscension) {
	return rightAscension * (360.0 / 4194303) * deg2rad;
}
static double argumentPerigee2argpo(int64_t argumentPerigee) {
	return argumentPerigee * (360.0 / 4194303) * deg2rad;
}
static double meanAnomaly2mo(int64_t meanAnomaly) {
	return meanAnomaly * (360.0 / 4194303) * deg2rad;
}
static double bStarDecimal2bstar(int64_t mantissa, int64_t exponent) {
	return (double)(mantissa * 1.0e-6 * pow(10.0, exponent));
}

static int weekday_from_timestamp_ms(int64_t timestamp_ms)
{
	/* 86400000 = milliseconds in one day */	
	int64_t days_since_epoch = timestamp_ms / 86400000UL;
	/* Thursday (1970-01-01) -> add 3 so Monday = 0 */
	return (days_since_epoch + 3) % 7;
}

static int is_leap_year(int year) {
	return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Number of days in each month (non-leap year) */
static const int days_in_month[13] = {
	0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static void datetime_from_unix_time_ms(uint64_t timestamp, struct datetime *dt)
{
	timestamp = timestamp / 1000;
	/* Extract time of day first (fast path) */
	uint32_t seconds_in_day = timestamp % 86400;
	dt->second = seconds_in_day % 60;
	seconds_in_day /= 60;
	dt->minute = seconds_in_day % 60;
	dt->hour = seconds_in_day / 60;

	/* Now calculate days since 1970-01-01 */
	uint64_t days = timestamp / 86400;

	/* Start from 1970 */
	int y = 1970;

	/* Fast forward whole years */
	while (days >= 365) {
		int days_in_year = 365 + is_leap_year(y);
		if (days < days_in_year) break;
		days -= days_in_year;
		y++;
	}

	dt->year = y;

	/* Now find month and day */
	int m = 1;
	while (days >= days_in_month[m]) {
		int dim = days_in_month[m];
		if (m == 2 && is_leap_year(y)) dim = 29;

		if (days < dim) break;

		days -= dim;
		m++;
	}

	dt->month = m;
	dt->day = days + 1;   /* days is 0-based within month */
}

static uint64_t datetime_to_ts(struct datetime *dt)
{
	uint64_t days = (dt->year-1970ULL)*365 + (dt->year-1969)/4 - (dt->year-1901)/100 + (dt->year-1601)/400;
	static const int cumdays[13] = {0,0,31,59,90,120,151,181,212,243,273,304,334};
	days += cumdays[dt->month] + (dt->month>2 && ((dt->year%4==0 && dt->year%100) || dt->year%400==0));
	days += dt->day - 1;
	return days*86400000 + dt->hour*3600000 + dt->minute*60000 + dt->second*1000;
}

static void jd_from_unix_time_ms(int64_t unix_time_ms, double *jd, double *jdfract)
{
	*jd = (unix_time_ms / 86400000) + 2440587.5;
	*jdfract = (unix_time_ms % 86400000) / 86400000.0;
}

static void eci_to_ecef(double* r_eci, double gmst, double* r_ecef)
{
	r_ecef[0] = r_eci[0] * cos(gmst) + r_eci[1] * sin(gmst);
	r_ecef[1] = -r_eci[0] * sin(gmst) + r_eci[1] * cos(gmst);
	r_ecef[2] = r_eci[2];
}
    
static void ecef_to_topocentric(double* r_ecef, double lat, double lon, double* r_sez)
{
	double sl = sin(lat);
	double cl = cos(lat);
	double slo = sin(lon);
	double clo = cos(lon);
    
	double dx = r_ecef[0];
	double dy = r_ecef[1];
	double dz = r_ecef[2];
    
	r_sez[0] = sl * clo * dx + sl * slo * dy - cl * dz; // South
	r_sez[1] = -slo * dx + clo * dy; // East
	r_sez[2] = cl * clo * dx + cl * slo * dy + sl * dz; // Zenith
}


static void calculate_look_angle(double* r_eci, double* r_station_ecef, double lat, double lon, double gmst, double* elevation)
{
	double r_sat_ecef[3];
	eci_to_ecef(r_eci, gmst, r_sat_ecef);
    
	double r_range_ecef[3];
	r_range_ecef[0] = r_sat_ecef[0] - r_station_ecef[0];
	r_range_ecef[1] = r_sat_ecef[1] - r_station_ecef[1];
	r_range_ecef[2] = r_sat_ecef[2] - r_station_ecef[2];
    
	double r_sez[3];
	ecef_to_topocentric(r_range_ecef, lat, lon, r_sez);
    
	double range = sqrt(r_sez[0]*r_sez[0] + r_sez[1]*r_sez[1] + r_sez[2]*r_sez[2]);
	*elevation = asin(r_sez[2] / range) * RAD2DEG;
}
    

/* Convert the epochStar to sgp4 Julian day format
 *
 * epochStar is the time offset in seconds from the beginning of the
 * current week (Monday 00:00:00 UTC) to the epoch time.
 * jdsatepoch is the Julian day of the epoch
 * jdsatepochF is the fractional day of the epoch
 */
static int epochStar2jd_satepoch(int epochStar, double *jdsatepoch, double *jdsatepochF)
{
	int weekday;
	struct datetime dt_mon_midnight;
	struct datetime dt_mon_ts;
	int64_t unix_time_ms;

	int err = date_time_now(&unix_time_ms);
	if (err) {
		return err;
	}

	datetime_from_unix_time_ms(unix_time_ms, &dt_mon_ts);
	
	/* Find current weekday */
	weekday = weekday_from_timestamp_ms(unix_time_ms);
	LOG_DBG("weekday: %d unix_time_ms1: %lld", weekday, unix_time_ms);
	
	/* Deduct 86400 * weekday to get the unix time of last monday */
	unix_time_ms -= (weekday * 86400000UL);
	LOG_DBG("unix_time_ms2: %lld", unix_time_ms);
	datetime_from_unix_time_ms(unix_time_ms, &dt_mon_midnight);

	/* Set the time to midnight */
	dt_mon_midnight.hour = 0;
	dt_mon_midnight.minute = 0;
	dt_mon_midnight.second = 0;

	unix_time_ms = datetime_to_ts(&dt_mon_midnight);
	LOG_DBG("unix_time_ms3: %lld", unix_time_ms);
	unix_time_ms -= (epochStar * 1000UL);
	datetime_from_unix_time_ms(unix_time_ms, &dt_mon_midnight);
	jd_from_unix_time_ms(unix_time_ms, jdsatepoch, jdsatepochF);

	return 0;
}

static void geodetic_to_ecef(double lat, double lon, double alt, double* ecef)
{
	double C = 1.0 / sqrt(1.0 - F * (2.0 - F) * sin(lat) * sin(lat));
	double S = C * (1.0 - F) * (1.0 - F);
    
	ecef[0] = (XKMPER * C + alt) * cos(lat) * cos(lon);
	ecef[1] = (XKMPER * C + alt) * cos(lat) * sin(lon);
	ecef[2] = (XKMPER * S + alt) * sin(lat);
}

static char *next_token(char **saveptr, char *delim)
{
	char *tok = strtok_r(NULL, delim, saveptr);
	if (!tok) {
		LOG_ERR("No token found");
		return NULL;
	}
	return tok;
}

static int count_leading_commas(const char *s) {
    int n = 0;
    while (*s++ == ',') n++;
    return n;
}


static int parse_ephemeris_struct(char **saveptr, struct sat_data_sib32 *sib32)
{
	char *tok;
	int idx = 0;
	int commas;
	int64_t tmp[FIELD_COUNT] = { [0 ... FIELD_COUNT-1] = -INT64_MAX };
	tok = strtok_r(NULL, ",", saveptr);
	while (tok != NULL) {
		tmp[idx++] = strtoll(tok, NULL, 10);
		commas = count_leading_commas(*saveptr);
		for (int i = 0; i < commas; i++) {
			if (idx >= FIELD_COUNT) {
				break;
			}
			tmp[idx++] = -INT64_MAX;
		}
		if (idx >= FIELD_COUNT) {
			break;
		}
		tok = strtok_r(NULL, ",", saveptr);
	}
	sib32->satelliteId = tmp[FIELD_SATELLITE_ID];
	sib32->inclination = tmp[FIELD_INCLINATION];
	sib32->argumentPerigee = tmp[FIELD_ARG_PERIGEE];
	sib32->rightAscension = tmp[FIELD_RIGHT_ASCENSION];
	sib32->meanAnomaly = tmp[FIELD_MEAN_ANOMALY];
	sib32->eccentricity = tmp[FIELD_ECCENTRICITY];
	sib32->meanMotion = tmp[FIELD_MEAN_MOTION];
	sib32->bStarDecimal = tmp[FIELD_B_STAR_DECIMAL];
	sib32->bStarExponent = tmp[FIELD_B_STAR_EXPONENT];
	sib32->epochStar = tmp[FIELD_EPOCH_STAR];
	sib32->serviceStart = tmp[FIELD_SERVICE_START];
	sib32->elevationAngleLeft = tmp[FIELD_ELEVATION_ANGLE_LEFT];
	sib32->elevationAngleRight = tmp[FIELD_ELEVATION_ANGLE_RIGHT];
	sib32->referencePointLongitude = tmp[FIELD_REFERENCE_POINT_LONGITUDE];
	sib32->referencePointLatitude = tmp[FIELD_REFERENCE_POINT_LATITUDE];
	sib32->radius = tmp[FIELD_RADIUS];
	return 0;

}

static int parse_sibconfig32_at(const char *atsid32, char *cell_id, struct sat_data_sib32 *sib32)
{
	int err = 0;
	char buf[512];

	if (strlen(atsid32) > sizeof(buf)) {
		LOG_ERR("AT SID32 too long");
		return -1;
	}
	memcpy(buf, atsid32, strlen(atsid32));
	buf[strlen(atsid32)] = '\0';
	char *saveptr;
	char *tok;

	/* Parse the SIBCONFIG header */
	tok = strtok_r(buf, " ", &saveptr);
	if (!tok) {
		LOG_ERR("Failed to parse SIBCONFIG notification, no token found");
		return -1;
	}

	if (strcmp(tok, "SIBCONFIG:") != 0) {
		LOG_ERR("Not a SIBCONFIG string");
		return -1;
	}
	/* Parse the SIB number */
	tok = next_token(&saveptr, ",");
	int sibnr = strtol(tok, NULL, 10);
	if (sibnr != 32) {
		LOG_ERR("Not a SIBCONFIG 32 string");
		return -1;
	}

	/* Parse the cell ID */
	tok = next_token(&saveptr, "\"");
	strncpy(cell_id, tok, 8);
	cell_id[8] = '\0';

	/* Parse ephemeris struct count */
	tok = next_token(&saveptr, ",");
	int sib_count = strtol(tok, NULL, 10);

	if (sib32 == NULL) {
		return sib_count;
	}

	for (int i = 0; i < sib_count; i++) {
		err = parse_ephemeris_struct(&saveptr, &sib32[i]);
		if (err) {
			LOG_ERR("Failed to parse ephemeris struct");
			return err;
		}
	}
	return sib_count;
}

static int sat_data_init_sib32(struct sat_data *sat_data, struct sat_data_sib32 *sib32, int index)
{
	bool success = false;
	sat_data->satrec[index].no_kozai = meanMotion2no_kozai(sib32->meanMotion);
	sat_data->satrec[index].ecco = eccentricity2ecco(sib32->eccentricity);
	sat_data->satrec[index].inclo = inclination2inclo(sib32->inclination);
	sat_data->satrec[index].nodeo = rightAscension2nodeo(sib32->rightAscension);
	sat_data->satrec[index].argpo = argumentPerigee2argpo(sib32->argumentPerigee);
	sat_data->satrec[index].mo = meanAnomaly2mo(sib32->meanAnomaly);
	sat_data->satrec[index].bstar = bStarDecimal2bstar(sib32->bStarDecimal, sib32->bStarExponent);

	int err = epochStar2jd_satepoch(sib32->epochStar, &sat_data->satrec[index].jdsatepoch, &sat_data->satrec[index].jdsatepochF);
	if (err) {
		return err;
	}
	success = sgp4init('i', &sat_data->satrec[index]);
	if (!success) {
		return sat_data->satrec[index].error;
	}
	return 0;
}

int sat_data_calculate_next_pass(struct sat_data *sat_data, int sat_index, double lat, double lon, double alt, int64_t start_time_ms)
{
	double r_station_ecef[3];
	double r[3], v[3];
	double jd_start, jdfrac_start;
	bool in_pass = false;
	double max_el = 0;
	int64_t pass_start = 0;

	geodetic_to_ecef(lat, lon, alt, r_station_ecef);

	jd_from_unix_time_ms(start_time_ms, &jd_start, &jdfrac_start);

	double jd_epoch = sat_data->satrec[sat_index].jdsatepoch + sat_data->satrec[sat_index].jdsatepochF;
	LOG_DBG("jd_epoch: %f", jd_epoch);
	double jd_current_start = jd_start + jdfrac_start;
	double minutes_offset_start = (jd_current_start - jd_epoch) * 1440.0;

	/* Check next 24 hours (1440 minutes) */
	for (int i = 0; i < 1440; i++) {
		double minutes_since_epoch = minutes_offset_start + i;
		if (!(sgp4(&sat_data->satrec[sat_index], minutes_since_epoch, r, v))) {
			continue;
		}
		double current_jd = jd_current_start + (i / 1440.0);
		double gmst = gstime(current_jd);
		double elevation;
		calculate_look_angle(r, r_station_ecef, lat, lon, gmst, &elevation);
		LOG_DBG("epoch: %f, elevation: %f,\n r[0]: %f, r[1]: %f, r[2]: %f", minutes_since_epoch, elevation, r[0], r[1], r[2]);
		if (elevation > 40.0) {
			if (!in_pass) {
				in_pass = true;
				pass_start = start_time_ms + (i * 60 * 1000);
				max_el = elevation;
				sat_data->next_pass.max_elevation_time_ms = start_time_ms + (i * 60 * 1000);
			} else {
				if (elevation > max_el) {
					max_el = elevation;
					sat_data->next_pass.max_elevation_time_ms = start_time_ms + (i * 60 * 1000);
				}
			}
		} else {
		if (in_pass) {
			/* Pass ended */
			sat_data->next_pass.start_time_ms = pass_start;
			sat_data->next_pass.end_time_ms = start_time_ms + (i * 60 * 1000);
			sat_data->next_pass.max_elevation = max_el;
			return 0; /* Pass found */
		}}
	}

	if (in_pass) {
		/* Pass continues beyond 24h? */
		sat_data->next_pass.start_time_ms = pass_start;
		sat_data->next_pass.end_time_ms = start_time_ms + (1440 * 60 * 1000);
		sat_data->next_pass.max_elevation = max_el;
		return 0;
	}
	LOG_ERR("No pass found");
	return -1; /* No pass found */
}

 
int sat_data_init_atsid32(struct sat_data *sat_data, const char *atsid32)
{
	int err = 0;
	char cell_id[9];
	struct sat_data_sib32 sib32[MAX_SATELLITES];

	int sib_count = parse_sibconfig32_at(atsid32, cell_id, NULL);
	if (sib_count < 0) {
		LOG_ERR("Failed to parse SIBCONFIG notification, error: %d", sib_count);
		return -1;
	}

	sib_count = parse_sibconfig32_at(atsid32, cell_id, &sib32[0]);
	if (sib_count < 0) {
		LOG_ERR("Failed to initialize satellite data, error: %d", err);
		sat_data->status = SAT_STATUS_ERROR;
		return -1;
	}

	for (int i = 0; i < sib_count; i++) {
		err = sat_data_init_sib32(sat_data, &sib32[i], i);
		if (err) {
			LOG_ERR("Failed to initialize satellite data, error: %d", err);
			sat_data->status = SAT_STATUS_ERROR;
			return err;
		}
	}

	return 0;
}

int sat_data_set_name(struct sat_data *satellite, const char *name)
{
	if (strlen(name) > sizeof(satellite->sat_name)) {
		LOG_ERR("Name too long, max length is %d", sizeof(satellite->sat_name));
		return -EINVAL;
	}
	memcpy(satellite->sat_name, name, strlen(name));
	return 0;
}

int sat_data_init_tle(struct sat_data *sat_data, char *line1, char *line2)
{
	memset(sat_data, 0, sizeof(struct sat_data));
	twoline2rv(line1, line2, 'v', 'e', 'i', wgs84, &sat_data->satrec[0]);
	if (sat_data->satrec[0].error) {
		return sat_data->satrec[0].error;
	}
	return 0;
}
