#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <date_time.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "SGP4.h"
#include "sat_prediction.h"
#include "celestrak_client.h"

LOG_MODULE_REGISTER(sat_prediction, CONFIG_APP_LOG_LEVEL);

#define TLE_BUFFER_SIZE 1024

// Helper to convert Degrees to Radians (and back)
#define DEG2RAD (3.14159265358979323846 / 180.0)
#define RAD2DEG (180.0 / 3.14159265358979323846)
#define XKMPER 6378.137 // Earth radius in km
#define F (1.0/298.257223563) // Flattening

typedef struct {
    char name[30];
    char line1[80];
    char line2[80];
} Satellite;

#define MAX_SATS 20 // Reduced from 100 to save RAM
static Satellite sats[MAX_SATS];
static int num_sats = 0;

// Coordinate conversion functions
static void GeodeticToECEF(double lat, double lon, double alt, double* ecef) {
    double C = 1.0 / sqrt(1.0 - F * (2.0 - F) * sin(lat) * sin(lat));
    double S = C * (1.0 - F) * (1.0 - F);

    ecef[0] = (XKMPER * C + alt) * cos(lat) * cos(lon);
    ecef[1] = (XKMPER * C + alt) * cos(lat) * sin(lon);
    ecef[2] = (XKMPER * S + alt) * sin(lat);
}

static void ECItoECEF(double* r_eci, double gmst, double* r_ecef) {
    r_ecef[0] = r_eci[0] * cos(gmst) + r_eci[1] * sin(gmst);
    r_ecef[1] = -r_eci[0] * sin(gmst) + r_eci[1] * cos(gmst);
    r_ecef[2] = r_eci[2];
}

static void ECEFtoTopocentric(double* r_ecef, double lat, double lon, double* r_sez) {
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

static void CalculateLookAngle(double* r_eci, double* r_station_ecef, double lat, double lon, double gmst, double* elevation) {
    double r_sat_ecef[3];
    ECItoECEF(r_eci, gmst, r_sat_ecef);

    double r_range_ecef[3];
    r_range_ecef[0] = r_sat_ecef[0] - r_station_ecef[0];
    r_range_ecef[1] = r_sat_ecef[1] - r_station_ecef[1];
    r_range_ecef[2] = r_sat_ecef[2] - r_station_ecef[2];

    double r_sez[3];
    ECEFtoTopocentric(r_range_ecef, lat, lon, r_sez);

    double range = sqrt(r_sez[0]*r_sez[0] + r_sez[1]*r_sez[1] + r_sez[2]*r_sez[2]);
    *elevation = asin(r_sez[2] / range) * RAD2DEG;
}

// Parse TLEs from a string buffer
static int ParseTLEs(const char* buffer, Satellite* sats, int max_sats) {
    int count = 0;
    const char* ptr = buffer;
    char line[100];

    while (count < max_sats && *ptr) {
        // Read Name
        int i = 0;
        while (*ptr && *ptr != '\n' && *ptr != '\r' && i < sizeof(line) - 1) {
            line[i++] = *ptr++;
        }
        line[i] = '\0';
        while (*ptr == '\n' || *ptr == '\r') ptr++; // Skip newlines

        if (strlen(line) == 0) continue;

        strncpy(sats[count].name, line, sizeof(sats[count].name) - 1);
        sats[count].name[sizeof(sats[count].name) - 1] = '\0';

        // Read Line 1
        i = 0;
        while (*ptr && *ptr != '\n' && *ptr != '\r' && i < sizeof(line) - 1) {
            line[i++] = *ptr++;
        }
        line[i] = '\0';
        while (*ptr == '\n' || *ptr == '\r') ptr++;

        strncpy(sats[count].line1, line, sizeof(sats[count].line1) - 1);
        sats[count].line1[sizeof(sats[count].line1) - 1] = '\0';

        // Read Line 2
        i = 0;
        while (*ptr && *ptr != '\n' && *ptr != '\r' && i < sizeof(line) - 1) {
            line[i++] = *ptr++;
        }
        line[i] = '\0';
        while (*ptr == '\n' || *ptr == '\r') ptr++;

        strncpy(sats[count].line2, line, sizeof(sats[count].line2) - 1);
        sats[count].line2[sizeof(sats[count].line2) - 1] = '\0';

        count++;
    }
    return count;
}

void sat_prediction_init(void) {
    LOG_INF("Satellite prediction module initialized");
    // Load initial TLEs if available (e.g. from settings or hardcoded)
}

static char tle_buffer[TLE_BUFFER_SIZE];

static const char* sateliot_catnrs[] = {
    "60550",  // SATELIOT_1
    "60534",  // SATELIOT_2
    "60552",  // SATELIOT_3
    "60537"   // SATELIOT_4
};

static void fetch_all_sateliot_tles(void) {
    char *all_tles = k_malloc(TLE_BUFFER_SIZE * 4);  // Space for all 4 satellites
    size_t total_len = 0;

    if (!all_tles) {
        LOG_ERR("Failed to allocate memory for TLEs");
        return;
    }

    all_tles[0] = '\0';

    /* Fetch TLE for each satellite */
    for (int i = 0; i < ARRAY_SIZE(sateliot_catnrs); i++) {
        size_t bytes_written;
        int err = celestrak_fetch_tle(sateliot_catnrs[i], tle_buffer, sizeof(tle_buffer), &bytes_written);
        if (err == 0 && bytes_written > 0) {
            /* Add satellite name if not present in TLE */
            if (strstr(tle_buffer, "SATELIOT") == NULL) {
                snprintf(all_tles + total_len, TLE_BUFFER_SIZE * 4 - total_len,
                        "SATELIOT_%d\n%s", i + 1, tle_buffer);
            } else {
                strncpy(all_tles + total_len, tle_buffer, TLE_BUFFER_SIZE * 4 - total_len);
            }
            total_len = strlen(all_tles);
            LOG_DBG("Fetched TLE for SATELIOT_%d", i + 1);
        } else {
            LOG_WRN("Failed to fetch TLE for SATELIOT_%d", i + 1);
        }
    }

    if (total_len > 0) {
        LOG_INF("Successfully fetched TLEs from Celestrak");
        LOG_DBG("Received TLE data:\n%s", all_tles);
        
        /* Parse the received TLEs */
        num_sats = ParseTLEs(all_tles, sats, MAX_SATS);
        if (num_sats > 0) {
            LOG_INF("Successfully loaded %d satellites from Celestrak", num_sats);
            k_free(all_tles);
            return;
        }
        LOG_ERR("Failed to parse TLE data from Celestrak");
    }

    k_free(all_tles);
}

void sat_prediction_update_tles(void) {
    /* Try to fetch latest TLEs from Celestrak */
    fetch_all_sateliot_tles();
    
    if (num_sats > 0) {
        return;
    }

    /* Fallback to hardcoded TLEs */
    LOG_WRN("Using hardcoded TLE data");
    const char* tles =
        "SATELIOT_1\n"
        "1 60550U 24149CL  26008.94984709  .00002490  00000+0  21989-3 0  9998\n"
        "2 60550  97.6904  87.4798 0005444 241.0002 119.0675 14.97258067 76238\n"
        "SATELIOT_2\n"
        "1 60534U 24149BU  26005.62737643  .00004209  00000+0  38443-3 0  9998\n"
        "2 60534  97.7004  83.5143 0001239  94.7269 265.4093 14.95569685 75664\n"
        "SATELIOT_3\n"
        "1 60552U 24149CN  26005.45183079  .00002914  00000+0  25637-3 0  9999\n"
        "2 60552  97.6993  84.6768 0003945 276.0991  83.9782 14.97278944 75720\n"
        "SATELIOT_4\n"
        "1 60537U 24149BX  26005.47117506  .00003071  00000+0  27955-3 0  9996\n"
        "2 60537  97.6905  82.9976 0001688 184.0001 176.1208 14.95900003 75645\n";

    num_sats = ParseTLEs(tles, sats, MAX_SATS);
    LOG_INF("Loaded %d satellites from hardcoded data", num_sats);
}

static int FindNextPass(Satellite* sat, double lat, double lon, double alt, int64_t start_time_ms, sat_prediction_pass_t* pass) {
    ElsetRec satrec;
    twoline2rv(sat->line1, sat->line2, 'c', 'm', 'i', wgs72, &satrec);
    if (satrec.error != 0) return -1;

    double r_station_ecef[3];
    GeodeticToECEF(lat, lon, alt, r_station_ecef);

    double r[3], v[3];

    // Convert start_time_ms to JD
    time_t start_time = start_time_ms / 1000;
    struct tm t_start;
    gmtime_r(&start_time, &t_start);

    double jd_start, jdfrac_start;
    jday(t_start.tm_year + 1900, t_start.tm_mon + 1, t_start.tm_mday, t_start.tm_hour, t_start.tm_min, t_start.tm_sec, &jd_start, &jdfrac_start);

    double jd_epoch = satrec.jdsatepoch + satrec.jdsatepochF;
    double jd_current_start = jd_start + jdfrac_start;
    double minutes_offset_start = (jd_current_start - jd_epoch) * 1440.0;

    bool in_pass = false;
    double max_el = 0;
    int64_t pass_start = 0;

    // Check next 24 hours (1440 minutes)
    for (int i = 0; i < 1440; i++) {
        double minutes_since_epoch = minutes_offset_start + i;
        if (sgp4(&satrec, minutes_since_epoch, r, v)) {
             double current_jd = jd_current_start + (double)i / 1440.0;
             double gmst = gstime(current_jd);
             double elevation;
             CalculateLookAngle(r, r_station_ecef, lat, lon, gmst, &elevation);

             if (elevation > 40.0) {
                     if (!in_pass) {
                         in_pass = true;
                         pass_start = start_time_ms + (i * 60 * 1000);
                         max_el = elevation;
                         pass->max_elevation_time_ms = start_time_ms + (i * 60 * 1000);
                     } else {
                         if (elevation > max_el) {
                             max_el = elevation;
                             pass->max_elevation_time_ms = start_time_ms + (i * 60 * 1000);
                         }
                     }
             } else {
                 if (in_pass) {
                     // Pass ended
                     pass->start_time_ms = pass_start;
                     pass->end_time_ms = start_time_ms + (i * 60 * 1000);
                     pass->max_elevation = max_el;
                     strncpy(pass->sat_name, sat->name, sizeof(pass->sat_name));
                     return 0; // Found a pass
                 }
             }
        }
    }

    if (in_pass) {
        // Pass continues beyond 24h?
        pass->start_time_ms = pass_start;
        pass->end_time_ms = start_time_ms + (1440 * 60 * 1000);
        pass->max_elevation = max_el;
        strncpy(pass->sat_name, sat->name, sizeof(pass->sat_name));
        return 0;
    }

    return -1; // No pass found
}

int sat_prediction_get_next_pass(double lat, double lon, double alt, sat_prediction_pass_t *best_pass) {
    if (num_sats == 0) sat_prediction_update_tles();

    int64_t now_ms;
    if (date_time_now(&now_ms) != 0) return -1;

    sat_prediction_pass_t current_pass;
    bool found = false;

    // Convert lat/lon to radians for internal calc
    double lat_rad = lat * DEG2RAD;
    double lon_rad = lon * DEG2RAD;
    double alt_km = alt / 1000.0;

    for (int i = 0; i < num_sats; i++) {
        if (FindNextPass(&sats[i], lat_rad, lon_rad, alt_km, now_ms, &current_pass) == 0) {
            if (!found || current_pass.start_time_ms < best_pass->start_time_ms) {
                *best_pass = current_pass;
                found = true;
            }
        }
    }

    return found ? 0 : -1;
}

/*
 * Legacy/Debug functions and thread removed to avoid hardcoded coordinates.
 * Prediction should be driven by the application (e.g. ntn.c) using valid GNSS coordinates.
 */

int sat_prediction_get_next_pass_with_tle(double lat, double lon, double alt,
                                        const char *tle_name,
                                        const char *tle_line1,
                                        const char *tle_line2,
                                        sat_prediction_pass_t *pass) {
    if (!tle_name || !tle_line1 || !tle_line2 || !pass) {
        return -EINVAL;
    }

    int64_t now_ms;
    if (date_time_now(&now_ms) != 0) {
        return -1;
    }

    // Create temporary satellite structure
    Satellite sat;
    strncpy(sat.name, tle_name, sizeof(sat.name) - 1);
    sat.name[sizeof(sat.name) - 1] = '\0';
    strncpy(sat.line1, tle_line1, sizeof(sat.line1) - 1);
    sat.line1[sizeof(sat.line1) - 1] = '\0';
    strncpy(sat.line2, tle_line2, sizeof(sat.line2) - 1);
    sat.line2[sizeof(sat.line2) - 1] = '\0';

    // Convert lat/lon to radians for internal calc
    double lat_rad = lat * DEG2RAD;
    double lon_rad = lon * DEG2RAD;
    double alt_km = alt / 1000.0;

    return FindNextPass(&sat, lat_rad, lon_rad, alt_km, now_ms, pass);
}

void sat_prediction_process(void) {
    // No-op
}
