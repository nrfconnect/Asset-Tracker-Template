/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */


#ifndef SGP4_PASS_PREDICT_H
#define SGP4_PASS_PREDICT_H

#include "SGP4.h"
#include <stdint.h>

#define DEG2RAD (3.14159265358979323846 / 180.0)
#define RAD2DEG (180.0 / 3.14159265358979323846)
#define XKMPER 6378.137 // Earth radius in km
#define F (1.0/298.257223563) // Flattening

/* 3GPP TS 36.331 ephemeris parameters format */
struct sat_data_sib32 {
    int64_t epochStar;
    int64_t meanMotion;
    int64_t eccentricity;
    int64_t inclination;
    int64_t rightAscension;
    int64_t argumentPerigee;
    int64_t meanAnomaly;
    int64_t bStarDecimal;
    int64_t bStarExponent;
};

struct next_pass {
    int64_t start_time_ms;
    int64_t end_time_ms;
    double max_elevation;
    int64_t max_elevation_time_ms;  /* Time at which maximum elevation occurs */
};

/* Satellite data structure*/
struct sat_data {
    char sat_name[30];
    struct ElsetRec satrec;
    int64_t epoch_unix_ms;
    struct next_pass next_pass;
};

/* Initialize the satellite data with 3GPP TS 36.331 ephemeris parameters */
int sat_data_init_sib32(struct sat_data *sat_data, struct sat_data_sib32 *sib32);

/* Initialize the satellite data with Two-line element set */
int sat_data_init_tle(struct sat_data *sat_data, char *line1, char *line2);

/* Get the next pass of the satellite */
int sat_data_get_next_pass(struct sat_data *sat_data, double lat, double lon, double alt, int64_t start_time_ms);

#endif
