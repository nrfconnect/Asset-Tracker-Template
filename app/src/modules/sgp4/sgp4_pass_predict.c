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
 #include <math.h>
 #include <date_time.h>
 #include "sgp4_pass_predict.h"
 #include "SGP4.h"

 #include <modem/at_monitor.h>
 #include <zephyr/logging/log.h>

 #define DEG2RAD (3.14159265358979323846 / 180.0)
 #define RAD2DEG (180.0 / 3.14159265358979323846)
 #define XKMPER 6378.137  /* Earth radius in km */
 #define F (1.0/298.257223563) /* Flattening */

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
 static double meanMotion2no_kozai(int64_t meanMotion)
 {
	 /* Rev/day to rad/s */
	 return meanMotion * (99.99999999 / 17179869183) / (1440.0 / (2.0 * pi));
 }
 static double eccentricity2ecco(int64_t eccentricity)
 {
	 return eccentricity * (0.9999999 / 16777215.0);
 }
 static double inclination2inclo(int64_t inclination)
 {
	 return inclination * (180.0 / 2097151) * deg2rad;
 }
 static double rightAscension2nodeo(int64_t rightAscension)
 {
	 return rightAscension * (360.0 / 4194303) * deg2rad;
 }
 static double argumentPerigee2argpo(int64_t argumentPerigee)
 {
	 return argumentPerigee * (360.0 / 4194303) * deg2rad;
 }
 static double meanAnomaly2mo(int64_t meanAnomaly)
 {
	 return meanAnomaly * (360.0 / 4194303) * deg2rad;
 }
 static double bStarDecimal2bstar(int64_t mantissa, int64_t exponent)
 {
	 return (double)(mantissa * 1.0e-5 * pow(10.0, exponent));
 }

 static int weekday_from_timestamp_ms(int64_t timestamp_ms)
 {
	 /* 86400000 = milliseconds in one day */
	 int64_t days_since_epoch = timestamp_ms / 86400000LL;
	 /* Thursday (1970-01-01) -> add 3 so Monday = 0 */
	 return (days_since_epoch + 3) % 7;
 }

 static int is_leap_year(int year)
 {
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

		 if (days < days_in_year) {
			 break;
		 }
		 days -= days_in_year;
		 y++;
	 }

	 dt->year = y;

	 /* Now find month and day */
	 int m = 1;

	 while (days >= days_in_month[m]) {
		 int dim = days_in_month[m];

		 if (m == 2 && is_leap_year(y)) {
			 dim = 29;
		 }

		 if (days < dim) {
			 break;
		 }

		 days -= dim;
		 m++;
	 }

	 dt->month = m;
	 dt->day = days + 1;   /* days is 0-based within month */
 }

 static uint64_t datetime_to_ts(struct datetime *dt)
 {
	 uint64_t days = (dt->year - 1970ULL)*365 +
		 (dt->year - 1969)/4 - (dt->year - 1901)/100 + (dt->year - 1601)/400;
	 static const int cumdays[13] = {
		 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	 };
	 days += cumdays[dt->month] + (dt->month > 2 && (
		 (dt->year % 4 == 0 && dt->year % 100) || dt->year % 400 == 0)
	 );
	 days += dt->day - 1;
	 return days*86400000 + dt->hour*3600000 + dt->minute*60000 + dt->second*1000;
 }

 static void jd_from_unix_time_ms(int64_t unix_time_ms, double *jd, double *jdfract)
 {
	 *jd = (unix_time_ms / 86400000) + 2440587.5;
	 *jdfract = (unix_time_ms % 86400000) / 86400000.0;
 }

 static void eci_to_ecef(double *r_eci, double gmst, double *r_ecef)
 {
	 r_ecef[0] = r_eci[0] * cos(gmst) + r_eci[1] * sin(gmst);
	 r_ecef[1] = -r_eci[0] * sin(gmst) + r_eci[1] * cos(gmst);
	 r_ecef[2] = r_eci[2];
 }

 static void ecef_to_topocentric(double *r_ecef, double lat, double lon, double *r_sez)
 {
	 double sl = sin(lat);
	 double cl = cos(lat);
	 double slo = sin(lon);
	 double clo = cos(lon);

	 double dx = r_ecef[0];
	 double dy = r_ecef[1];
	 double dz = r_ecef[2];

	 r_sez[0] = sl * clo * dx + sl * slo * dy - cl * dz;
	 r_sez[1] = -slo * dx + clo * dy;
	 r_sez[2] = cl * clo * dx + cl * slo * dy + sl * dz;
 }

 static void calculate_look_angle(double *r_eci, double *r_station_ecef, double lat, double lon,
	 double gmst, double *elevation)
 {
	 double r_sat_ecef[3];
	 double r_range_ecef[3];
	 double r_sez[3];
	 double range;

	 eci_to_ecef(r_eci, gmst, r_sat_ecef);

	 r_range_ecef[0] = r_sat_ecef[0] - r_station_ecef[0];
	 r_range_ecef[1] = r_sat_ecef[1] - r_station_ecef[1];
	 r_range_ecef[2] = r_sat_ecef[2] - r_station_ecef[2];

	 ecef_to_topocentric(r_range_ecef, lat, lon, r_sez);

	 range = sqrt(r_sez[0]*r_sez[0] + r_sez[1]*r_sez[1] + r_sez[2]*r_sez[2]);
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
	 int err;
	 int weekday;
	 struct datetime dt_mon_midnight;
	 struct datetime dt_mon_ts;
	 int64_t unix_time_ms;

	 err = date_time_now(&unix_time_ms);
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
	 unix_time_ms += (epochStar * 1000UL);
	 datetime_from_unix_time_ms(unix_time_ms, &dt_mon_midnight);
	 jd_from_unix_time_ms(unix_time_ms, jdsatepoch, jdsatepochF);

	 return 0;
 }

 static void geodetic_to_ecef(double lat, double lon, double alt, double *ecef)
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

 static int count_leading_commas(const char *s)
 {
	 int n = 0;

	 if (s == NULL) {
		 return 0;
	 }
	 while (*s++ == ',') {
		 n++;
	 }
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

 static int parse_sibconfig32_at(const char *atsib32, char *cell_id, struct sat_data_sib32 *sib32)
 {
	 int err;
	 char *buf;
	 int sibnr;
	 int ret;
	 int sib_count;
	 char *saveptr;
	 char *tok;
	 size_t atsib32_len;

	 if (atsib32 == NULL) {
		 LOG_ERR("AT SIB32 is NULL");
		 return -EINVAL;
	 }
	 atsib32_len = strlen(atsib32);

	 buf = malloc(atsib32_len + 1);
	 if (buf == NULL) {
		 LOG_ERR("Failed to allocate SIB32 parse buffer");
		 return -ENOMEM;
	 }
	 memcpy(buf, atsib32, atsib32_len);
	 buf[atsib32_len] = '\0';

	 /* Parse the SIBCONFIG header */
	 tok = strtok_r(buf, " ", &saveptr);
	 if (!tok) {
		 LOG_ERR("Failed to parse SIBCONFIG notification, no token found");
		 ret = -EINVAL;
		 goto cleanup;
	 }

	 if (strcmp(tok, "SIBCONFIG:") != 0) {
		 LOG_ERR("Not a SIBCONFIG string");
		 ret = -EINVAL;
		 goto cleanup;
	 }
	 /* Parse the SIB number */
	 tok = next_token(&saveptr, ",");
	 sibnr = strtol(tok, NULL, 10);
	 LOG_INF("SIB Number is %d",sibnr);
	 if (sibnr != 32) {
		 LOG_ERR("Not a SIBCONFIG 32 string");
		 ret = -EINVAL;
		 goto cleanup;
	 }

	 /* Parse the cell ID */
	 tok = next_token(&saveptr, "\"");
	 if (cell_id != NULL) {
		 strncpy(cell_id, tok, 8);
		 cell_id[8] = '\0';
	 }

	 /* Parse ephemeris struct count */
	 tok = next_token(&saveptr, ",");
	 sib_count = strtol(tok, NULL, 10);
	 LOG_INF("SIB count is %d",sib_count);

	 if (sib32 == NULL) {
		 ret = sib_count;
		 goto cleanup;
	 }

	 for (int i = 0; i < sib_count; i++) {
		 err = parse_ephemeris_struct(&saveptr, &sib32[i]);
		 if (err) {
			 LOG_ERR("Failed to parse ephemeris struct");
			 ret = err;
			 goto cleanup;
		 }
	 }

	 ret = sib_count;

cleanup:
	 free(buf);
	 return ret;
 }

 static int sat_data_init_sib32(struct sat_data *sat_data, struct sat_data_sib32 *sib32, int index)
 {
	 int err;
	 bool success;

	 sat_data->satellite_ids[index] = sib32->satelliteId;
	 sat_data->satrec[index].no_kozai = meanMotion2no_kozai(sib32->meanMotion);
	 sat_data->satrec[index].ecco = eccentricity2ecco(sib32->eccentricity);
	 sat_data->satrec[index].inclo = inclination2inclo(sib32->inclination);
	 sat_data->satrec[index].nodeo = rightAscension2nodeo(sib32->rightAscension);
	 sat_data->satrec[index].argpo = argumentPerigee2argpo(sib32->argumentPerigee);
	 sat_data->satrec[index].mo = meanAnomaly2mo(sib32->meanAnomaly);
	 sat_data->satrec[index].bstar = bStarDecimal2bstar(sib32->bStarDecimal,
		 sib32->bStarExponent);

	 err = epochStar2jd_satepoch(sib32->epochStar, &sat_data->satrec[index].jdsatepoch,
		 &sat_data->satrec[index].jdsatepochF);
	 if (err) {
		 return err;
	 }
	 success = sgp4init('i', &sat_data->satrec[index]);
	 if (!success) {
		 return -sat_data->satrec[index].error;
	 }
	 return 0;
 }

int sat_data_calculate_next_pass(struct sat_data *sat_data, int sat_index, double lat_deg,
	 double lon_deg, double alt_m, int64_t start_time_ms, double min_elevation_deg)
 {
	 double r_station_ecef[3];
	 double r[3], v[3];
	 double jd_start, jdfrac_start;
	 double minutes_since_epoch;
	 double jd_epoch;
	 double jd_current_start;
	 double minutes_offset_start;
	 double current_jd;
	 double gmst;
	 double elevation;
	 double max_el = 0.0;

	 bool in_pass = false;
	 int64_t pass_start = 0;

	 double lat = lat_deg * DEG2RAD;
	 double lon = lon_deg * DEG2RAD;
	 double alt_km = alt_m / 1000.0;


	 if (sat_data == NULL) {
		 LOG_ERR("Satellite data is NULL");
		 return -EINVAL;
	 }
	 if (sat_index < 0 || sat_index >= MAX_SATELLITES) {
		 LOG_ERR("Invalid satellite index: %d", sat_index);
		 return -EINVAL;
	 }
	 if (min_elevation_deg < 0.0 || min_elevation_deg > 90.0) {
		 LOG_ERR("Invalid minimum elevation: %.2f", min_elevation_deg);
		 return -EINVAL;
	 }
	 geodetic_to_ecef(lat, lon, alt_km, r_station_ecef);

	 jd_from_unix_time_ms(start_time_ms, &jd_start, &jdfrac_start);

	 jd_epoch = sat_data->satrec[sat_index].jdsatepoch + sat_data->satrec[sat_index].jdsatepochF;
	 LOG_DBG("jd_epoch: %f", jd_epoch);
	 jd_current_start = jd_start + jdfrac_start;
	 minutes_offset_start = (jd_current_start - jd_epoch) * 1440.0;

	 /* Check next 24 hours with 10 second granularity */
	 for (int i = 0; i < 8640; i++) {
		 minutes_since_epoch = minutes_offset_start + (i*10.0)/60.0;
		 if (!(sgp4(&sat_data->satrec[sat_index], minutes_since_epoch, r, v))) {
			 continue;
		 }
		 current_jd = jd_current_start + (i / 8640.0);
		 gmst = gstime(current_jd);
		 calculate_look_angle(r, r_station_ecef, lat, lon, gmst, &elevation);
		 if (elevation >= min_elevation_deg) {
			 if (!in_pass) {
				 in_pass = true;
				 pass_start = start_time_ms + (i * 10 * 1000);
				 max_el = elevation;
				 sat_data->next_pass.max_elevation_time_ms =
					 start_time_ms + (i * 10 * 1000);
			 } else {
				 if (elevation > max_el) {
					 max_el = elevation;
					 sat_data->next_pass.max_elevation_time_ms =
						 start_time_ms + (i * 10 * 1000);
				 }
			 }
		 } else {
			 if (in_pass) {
				 /* Pass ended */
				 sat_data->next_pass.start_time_ms = pass_start;
				 sat_data->next_pass.end_time_ms = start_time_ms + (i * 10 * 1000);
				 sat_data->next_pass.max_elevation = max_el;
				 return 0; /* Pass found */
			 }
		 }
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

 int sat_data_init_atsib32(struct sat_data *sat_data, const char *atsib32)
 {
	 int err;
	 char cell_id[9];
	 int sib_count;
	 struct sat_data_sib32 sib32[MAX_SATELLITES];

	 if (sat_data == NULL) {
		 LOG_ERR("Satellite data is NULL");
		 return -EINVAL;
	 }

	 memset(sat_data, 0, sizeof(struct sat_data));

	 if (atsib32 == NULL) {
		 LOG_ERR("AT SIB32 is NULL");
		 return -EINVAL;
	 }

	 sib_count = parse_sibconfig32_at(atsib32, cell_id, &sib32[0]);
	 if (sib_count < 0) {
		 LOG_ERR("Failed to initialize satellite data, error: %d", sib_count);
		 sat_data->status = SAT_STATUS_ERROR;
		 return -EINVAL;
	 }

	 if (sib_count == 0) {
		 LOG_ERR("SIB32 does not contain any satellite ephemeris");
		 sat_data->status = SAT_STATUS_ERROR;
		 return -ENODATA;
	 }

	 if (sib_count > MAX_SATELLITES) {
		 LOG_ERR("SIB32 satellite count %d exceeds max supported %d", sib_count,
			 MAX_SATELLITES);
		 sat_data->status = SAT_STATUS_ERROR;
		 return -EOVERFLOW;
	 }

	 memcpy(sat_data->cell_id, cell_id, sizeof(sat_data->cell_id));
	 sat_data->sat_count = sib_count;

	 for (int i = 0; i < sib_count; i++) {
		 err = sat_data_init_sib32(sat_data, &sib32[i], i);
		 if (err) {
			 LOG_ERR("Failed to initialize satellite data, error: %d", err);
			 sat_data->status = SAT_STATUS_ERROR;
			 return err;
		 }
	 }

	 sat_data->status = SAT_STATUS_ACTIVE;

	 return 0;
 }

 int sat_data_set_name(struct sat_data *sat_data, const char *name)
 {
	 if (strlen(name) > sizeof(sat_data->sat_name)) {
		 LOG_ERR("Name too long, max length is %d", sizeof(sat_data->sat_name));
		 return -EINVAL;
	 }
	 memcpy(sat_data->sat_name, name, strlen(name));
	 return 0;
 }

 int sat_data_init_tle(struct sat_data *sat_data, const char *line1, const char *line2)
 {
	 if (sat_data == NULL) {
		 LOG_ERR("Satellite data is NULL");
		 return -EINVAL;
	 }

	 if (line1 == NULL || line2 == NULL) {
		 LOG_ERR("Line1 or line2 is NULL");
		 return -EINVAL;
	 }
	memset(sat_data, 0, sizeof(struct sat_data));

	return sat_data_init_tle_at(sat_data, line1, line2, 0);
 }

int sat_data_init_tle_at(struct sat_data *sat_data, const char *line1, const char *line2, int sat_index)
{
	if (sat_data == NULL) {
		LOG_ERR("Satellite data is NULL");
		return -EINVAL;
	}

	if (line1 == NULL || line2 == NULL) {
		LOG_ERR("Line1 or line2 is NULL");
		return -EINVAL;
	}

	if (sat_index < 0 || sat_index >= MAX_SATELLITES) {
		LOG_ERR("Invalid satellite index: %d", sat_index);
		return -EINVAL;
	}

	memset(&sat_data->satrec[sat_index], 0, sizeof(sat_data->satrec[sat_index]));
	twoline2rv(line1, line2, 'c', 'm', 'i', wgs84, &sat_data->satrec[sat_index]);
	if (sat_data->satrec[sat_index].error) {
		return -sat_data->satrec[sat_index].error;
	}

	if (sat_index >= sat_data->sat_count) {
		sat_data->sat_count = sat_index + 1;
	}

	sat_data->status = SAT_STATUS_ACTIVE;

	return 0;
}
