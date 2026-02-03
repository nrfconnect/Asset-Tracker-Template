/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <stdint.h>
#include "sgp4_pass_predict.h"
#include "SGP4.h"
#include <math.h>
#include <date_time.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sgp4_pass_predict, 4);

/* Internal datetime structure */
struct datetime {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	double second;
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
	*jd = (double)(unix_time_ms / 86400000) + 2440587.5;
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
	
	/* Deduct 86400 * weekday to get the unix time of last monday */
	unix_time_ms -= weekday * 86400000;
	datetime_from_unix_time_ms(unix_time_ms, &dt_mon_midnight);

	/* Set the time to midnight */
	dt_mon_midnight.hour = 0;
	dt_mon_midnight.minute = 0;
	dt_mon_midnight.second = 0;

	unix_time_ms = datetime_to_ts(&dt_mon_midnight);
	unix_time_ms -= (epochStar * 1000);
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

int sat_data_init_tle(struct sat_data *sat_data, char *line1, char *line2)
{
	memset(sat_data, 0, sizeof(struct sat_data));
	twoline2rv(line1, line2, 'v', 'e', 'a', wgs84, &sat_data->satrec);
	if (sat_data->satrec.error) {
		return sat_data->satrec.error;
	}
	return 0;
}

int sat_data_init_sib32(struct sat_data *sat_data, struct sat_data_sib32 *sib32)
{
	bool success = false;
	memset(sat_data, 0, sizeof(struct sat_data));
	sat_data->satrec.no_kozai = meanMotion2no_kozai(sib32->meanMotion);
	sat_data->satrec.ecco = eccentricity2ecco(sib32->eccentricity);
	sat_data->satrec.inclo = inclination2inclo(sib32->inclination);
	sat_data->satrec.nodeo = rightAscension2nodeo(sib32->rightAscension);
	sat_data->satrec.argpo = argumentPerigee2argpo(sib32->argumentPerigee);
	sat_data->satrec.mo = meanAnomaly2mo(sib32->meanAnomaly);
	sat_data->satrec.bstar = bStarDecimal2bstar(sib32->bStarDecimal, sib32->bStarExponent);

	int err = epochStar2jd_satepoch(sib32->epochStar, &sat_data->satrec.jdsatepoch, &sat_data->satrec.jdsatepochF);
	if (err) {
		return err;
	}
	success = sgp4init('i', &sat_data->satrec);
	if (!success) {
		return sat_data->satrec.error;
	}
	return 0;
}

int sat_data_get_next_pass(struct sat_data *sat_data, double lat, double lon, double alt, int64_t start_time_ms)
{
	double r_station_ecef[3];
	double r[3], v[3];
	double jd_start, jdfrac_start;
	bool in_pass = false;
	double max_el = 0;
	int64_t pass_start = 0;

	geodetic_to_ecef(lat, lon, alt, r_station_ecef);

	jd_from_unix_time_ms(start_time_ms, &jd_start, &jdfrac_start);

	double jd_epoch = sat_data->satrec.jdsatepoch + sat_data->satrec.jdsatepochF;
	double jd_current_start = jd_start + jdfrac_start;
	double minutes_offset_start = (jd_current_start - jd_epoch) * 1440.0;

	/* Check next 24 hours (1440 minutes) */
	for (int i = 0; i < 1440; i++) {
		double minutes_since_epoch = minutes_offset_start + i;
		if (sgp4(&sat_data->satrec, minutes_since_epoch, r, v)) {
			double current_jd = jd_current_start + (double)i / 1440.0;
			double gmst = gstime(current_jd);
			double elevation;
			calculate_look_angle(r, r_station_ecef, lat, lon, gmst, &elevation);

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
	}

	if (in_pass) {
		/* Pass continues beyond 24h? */
		sat_data->next_pass.start_time_ms = pass_start;
		sat_data->next_pass.end_time_ms = start_time_ms + (1440 * 60 * 1000);
		sat_data->next_pass.max_elevation = max_el;
		return 0;
	}

	return -1; /* No pass found */
}
