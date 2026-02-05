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
	enum sat_status status;
	struct ElsetRec satrec[MAX_SATELLITES];
	int64_t epoch_unix_ms;
	struct next_pass next_pass;
};

/**
@brief Initialize the satellite data with 3GPP TS 36.331 ephemeris parameters
@param satellite The satellite data structure
@param sib32 The 3GPP TS 36.331 ephemeris parameters
@return 0 if the satellite data is initialized successfully, negative error code otherwise

*/
//int sat_data_init_sib32(struct sat_data *satellite, struct sat_data_sib32 *sib32);

/**
@brief Initialize the satellite data with AT SID32 parameters
@param satellite The satellite data structure
@param atsid32 The AT SID32 parameters
@return 0 if the satellite data is initialized successfully, negative error code otherwise

*/
int sat_data_init_atsid32(struct sat_data *satellite, const char *atsid32);

/**
@brief Initialize the satellite data with Two-line element set
@param satellite The satellite data structure
@param line1 The first line of the TLE
@param line2 The second line of the TLE
@return 0 if the satellite data is initialized successfully, negative error code otherwise

*/
int sat_data_init_tle(struct sat_data *satellite, char *line1, char *line2);

/**
@brief Calculate the next pass of the satellite
@param satellite The satellite data structure
@param lat The latitude of the ue
@param lon The longitude of the ue
@param alt The altitude of the ue
@param start_time_ms Unix timestamp of when to start the search
@return 0 if the next pass is calculated successfully, negative error code otherwise

*/
int sat_data_calculate_next_pass(struct sat_data *sat_data, int sat_index, 
	double lat, double lon, double alt, int64_t start_time_ms);


/**
@brief Set the name of the satellite
@param satellite The satellite data structure
@param name The name of the satellite
@return 0 if the name is set successfully, negative error code otherwise

*/
int sat_data_set_name(struct sat_data *satellite, const char *name);

#endif
