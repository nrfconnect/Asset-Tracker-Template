#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Convert PEM certificates to defines for use during run-time provisioning of credentials.
message(WARNING "Credentials are exposed in non-secure memory. This should be avoided in production.")

# Define the directory where certificate include files will be stored.
set(app_certs_binary_dir ${APPLICATION_BINARY_DIR}/creds)

function(process_certificate definition_name file_name)
	set(cert_file ${CMAKE_CURRENT_SOURCE_DIR}/${file_name})
	if(EXISTS ${cert_file})
		message(STATUS "${file_name} found")

		get_filename_component(file_base_name ${file_name} NAME_WE)
		set(inc_file_name ${file_base_name}.inc)

		set(inc_file_path ${app_certs_binary_dir}/${inc_file_name})
		generate_inc_file_for_target(app ${cert_file} ${inc_file_path})

		# Add the directory containing the generated .inc file to the include path
		zephyr_include_directories(${app_certs_binary_dir})

		# Define a compiler macro with the path to the generated .inc file,
		# allowing it to be included in the source code.
		zephyr_compile_definitions(-D${definition_name}="${inc_file_path}")
	endif()
endfunction()

process_certificate("ATT_MQTT_CA_CERT" "mqtt.nordicsemi.academy.pem")
