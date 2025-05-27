#ifndef SAT_PREDICTION_H
#define SAT_PREDICTION_H

#include <zephyr/kernel.h>

typedef struct {
    int64_t start_time_ms;
    int64_t end_time_ms;
    double max_elevation;
    int64_t max_elevation_time_ms;  /* Time at which maximum elevation occurs */
    char sat_name[30];
} sat_prediction_pass_t;

/**
 * @brief Initialize the satellite prediction module.
 */
void sat_prediction_init(void);

/**
 * @brief Get the next satellite pass based on the provided location.
 *
 * The pass information includes:
 * - start_time_ms: Time when satellite rises above 40° elevation
 * - end_time_ms: Time when satellite drops below 40° elevation
 * - max_elevation: Maximum elevation angle reached during the pass
 * - max_elevation_time_ms: Time when the maximum elevation occurs
 * - sat_name: Name of the satellite
 *
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @param alt Altitude in meters
 * @param pass Pointer to a structure to fill with pass information
 * @return 0 if a pass is found, negative error code otherwise.
 */
int sat_prediction_get_next_pass(double lat, double lon, double alt, sat_prediction_pass_t *pass);

/**
 * @brief Run satellite predictions based on current time and loaded TLEs.
 */
void sat_prediction_process(void);

/**
 * @brief Update TLEs (placeholder for download logic).
 */
void sat_prediction_update_tles(void);

/**
 * @brief Get the next satellite pass using provided TLE data.
 *
 * The pass information includes:
 * - start_time_ms: Time when satellite rises above 40° elevation
 * - end_time_ms: Time when satellite drops below 40° elevation
 * - max_elevation: Maximum elevation angle reached during the pass
 * - max_elevation_time_ms: Time when the maximum elevation occurs
 * - sat_name: Name of the satellite
 *
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @param alt Altitude in meters
 * @param tle_name Name of the satellite
 * @param tle_line1 First line of the TLE
 * @param tle_line2 Second line of the TLE
 * @param pass Pointer to a structure to fill with pass information
 * @return 0 if a pass is found, negative error code otherwise.
 */
int sat_prediction_get_next_pass_with_tle(double lat, double lon, double alt,
                                        const char *tle_name,
                                        const char *tle_line1,
                                        const char *tle_line2,
                                        sat_prediction_pass_t *pass);

#endif
