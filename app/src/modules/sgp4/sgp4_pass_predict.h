/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */


 #ifndef SGP4_PASS_PREDICT_H
 #define SGP4_PASS_PREDICT_H
 
 #include "SGP4.h"
 #include <stdint.h>
 
 #define MAX_SATELLITES 4
 #define SGP4_DEFAULT_MIN_ELEVATION_DEG 50.0
 
 struct next_pass {
	 int64_t start_time_ms;
	 int64_t end_time_ms;
	 double max_elevation;
	 int64_t max_elevation_time_ms;  /* Time at which maximum elevation occurs */
 };
 
 enum sat_status {
	 SAT_STATUS_ACTIVE = 0,
	 SAT_STATUS_INACTIVE = 1,
	 SAT_STATUS_ERROR = 2,
 };
 
 /* Satellite data structure*/
 struct sat_data {
	 char sat_name[30];
	 char cell_id[9];
	 int64_t satellite_ids[MAX_SATELLITES];
	 enum sat_status status;
	 uint8_t sat_count;
	 struct ElsetRec satrec[MAX_SATELLITES];
	 int64_t epoch_unix_ms;
	 struct next_pass next_pass;
 };
 
 /**
  * @brief Initialize the satellite data with AT SIB32 parameters
  *
  * @param sat_data The satellite data structure
  * @param atsib32 The AT SIB32 parameters from modem
  *
  * @return 0 if the satellite data is initialized successfully, negative error code otherwise
  */
 int sat_data_init_atsib32(struct sat_data *sat_data, const char *atsib32);
 
 /**
  * @brief Initialize the satellite data with Two-line element set
  *
  * @param sat_data The satellite data structure
  * @param line1 The first line of the TLE
  * @param line2 The second line of the TLE
  *
  * @return 0 if the satellite data is initialized successfully, negative error code otherwise
  */
 int sat_data_init_tle(struct sat_data *sat_data, const char *line1, const char *line2);

/**
 * @brief Initialize a specific satellite slot with a single TLE
 *
 * @param sat_data The satellite data structure
 * @param line1 The first line of the TLE
 * @param line2 The second line of the TLE
 * @param sat_index Destination index in the satellite array
 *
 * @return 0 if the satellite data is initialized successfully, negative error code otherwise
 */
int sat_data_init_tle_at(struct sat_data *sat_data, const char *line1, const char *line2,
	 int sat_index);
 
 /**
  * @brief Calculate the next pass of the satellite
  *
  * @param sat_data The satellite data structure
  * @param sat_index The index of the satellite
  * @param lat The latitude of the ue in degrees
  * @param lon The longitude of the ue in degrees
  * @param alt The altitude of the ue in meters
  * @param start_time_ms Unix timestamp of when to start the search
  *
  * @return 0 if the next pass is calculated successfully, negative error code otherwise
  */
 int sat_data_calculate_next_pass(struct sat_data *sat_data, int sat_index,
	 double lat, double lon, double alt, int64_t start_time_ms,
	 double min_elevation_deg);
 
 
 /**
  * @brief Set the name of the satellite
  *
  * @param sat_data The satellite data structure
  * @param name The name of the satellite
  *
  * @return 0 if the name is set successfully, negative error code otherwise
  */
 int sat_data_set_name(struct sat_data *sat_data, const char *name);
 
 #endif