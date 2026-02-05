#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
    sat_prediction.c
    SGP4.c
    celestrak_client.c
    sgp4_pass_predict.c
)

target_include_directories(app PRIVATE .)

# Add required Zephyr libraries
target_link_libraries(app PRIVATE
    zephyr_interface
)

# Add required include paths
target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/include
    ${ZEPHYR_BASE}/subsys/net/lib/sockets
    ${NRF_DIR}/include
    ${NRF_DIR}/subsys/net/lib
)
