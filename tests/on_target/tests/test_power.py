##########################################################################################
# Copyright (c) 2024 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import time
import json
import types
import pytest
import csv
import pandas as pd
import plotly.express as px
from tests.conftest import get_uarts
from ppk2_api.ppk2_api import PPK2_API
from utils.uart import Uart
from utils.flash_tools import flash_device, reset_device, recover_device
import sys
sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

UART_TIMEOUT = 60 * 30
POWER_TIMEOUT = 60 * 20
MAX_CURRENT_PSM_UA = 10
SAMPLING_INTERVAL = 0.01
CSV_FILE = "power_measurements.csv"
HMTL_PLOT_FILE = "power_measurements_plot.html"

def save_badge_data(average):
    badge_filename = "power_badge.json"
    logger.info(f"Minimum average current measured: {average}uA")
    if average < 0:
        pytest.skip(f"current cant be negative, current average: {average}")
    elif average <= 10:
        color = "green"
    elif average <= 50:
        color = "yellow"
    else:
        color = "red"

    badge_data = {
        "label": "🔗 PSM current uA",
        "message": f"{average}",
        "schemaVersion": 1,
        "color": f"{color}"
    }

    # Save the JSON data to a file
    with open(badge_filename, 'w') as json_file:
        json.dump(badge_data, json_file)

    logger.info(f"Minimum average current saved to {badge_filename}")

def save_measurement_data(samples):
    # Generate timestamps for each sample assuming uniform sampling interval
    timestamps = [round(i * SAMPLING_INTERVAL, 2) for i in range(len(samples))]

    with open(CSV_FILE, 'w', newline='') as csvfile:
        fieldnames = ['Time (s)', 'Current (uA)']
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)

        writer.writeheader()
        for t, current in zip(timestamps, samples):
            writer.writerow({'Time (s)': t, 'Current (uA)': current})

    logger.info(f"Measurement data saved to {CSV_FILE}")

def generate_time_series_html(csv_file, date_column, value_column, output_file="time_series_plot.html"):
    """
    Generates an HTML file with an interactive time series plot from a CSV file.

    Parameters:
    - csv_file (str): Path to the CSV file containing the time series data.
    - date_column (str): Name of the column containing date or time information.
    - value_column (str): Name of the column containing the values to plot.
    - output_file (str): Name of the output HTML file (default is "time_series_plot.html").

    Returns:
    - str: The path to the generated HTML file.
    """
    # Load the CSV file
    df = pd.read_csv(csv_file, parse_dates=[date_column])

    title = "OOB Current Consumption Plot\n\n"
    note_text = "Note: Low power state is reached after ~10 min, earlier than that uart is active (drawing ≳500uA)"
    title += f"<br><span style='font-size:12px;color:gray;'>{note_text}</span>"

    # Create an interactive Plotly line chart
    fig = px.line(df, x=date_column, y=value_column, title=title)

    # Save the plot to an HTML file
    fig.write_html(output_file)

    logger.info(f"HTML file generated: {output_file}")
    return output_file


@pytest.fixture(scope="module")
def thingy91x_ppk2():
    '''
    This fixture sets up ppk measurement tool.
    '''
    ppk2s_connected = PPK2_API.list_devices()
    ppk2s_connected.sort()
    if len(ppk2s_connected) == 2:
        ppk2_port = ppk2s_connected[0]
        ppk2_serial = ppk2s_connected[1]
        logger.info(f"Found PPK2 at port: {ppk2_port}, serial: {ppk2_serial}")
    elif len(ppk2s_connected) == 0:
        pytest.skip("No ppk found")
    else:
        pytest.skip(f"PPK should list 2 ports, but found {ppk2s_connected}")

    ppk2_dev = PPK2_API(ppk2_port, timeout=1, write_timeout=1, exclusive=True)

    # get modifier might fail, retry 15 times
    for _ in range(15):
        try:
            ppk2_dev.get_modifiers()
            break
        except Exception as e:
            logger.error(f"Failed to get modifiers: {e}")
            time.sleep(5)
    else:
        pytest.skip("Failed to get ppk modifiers after 10 attempts")


    ppk2_dev.set_source_voltage(3300)
    ppk2_dev.use_ampere_meter()  # set ampere meter mode
    ppk2_dev.toggle_DUT_power("ON")  # enable DUT power

    time.sleep(10)
    for i in range(10):
        try:
            all_uarts = get_uarts()
            if not all_uarts:
                logger.error("No UARTs found")
            log_uart_string = all_uarts[0]
            break
        except Exception:
            ppk2_dev.toggle_DUT_power("OFF")  # disable DUT power
            time.sleep(2)
            ppk2_dev.toggle_DUT_power("ON")  # enable DUT power
            time.sleep(5)
            continue
    else:
        pytest.skip("NO uart after 10 attempts")

    t91x_uart = Uart(log_uart_string, timeout=UART_TIMEOUT)

    yield types.SimpleNamespace(ppk2_dev=ppk2_dev, t91x_uart=t91x_uart)

    t91x_uart.stop()
    recover_device()
    ppk2_dev.stop_measuring()

@pytest.mark.dut_ppk
def test_power(thingy91x_ppk2, hex_file):
    flash_device(os.path.abspath(hex_file))
    reset_device()
    try:
        thingy91x_ppk2.t91x_uart.wait_for_str("Connected to Cloud", timeout=120)
    except AssertionError:
        pytest.skip("Device unable to connect to cloud, skip ppk test")

    thingy91x_ppk2.ppk2_dev.start_measuring()

    start = time.time()
    min_rolling_average = float('inf')
    rolling_average = float('inf')
    samples_list = []
    last_log_time = start
    # Initialize an empty pandas Series to store samples over time
    samples_series = pd.Series(dtype='float64')
    while time.time() < start + POWER_TIMEOUT:
        try:
            read_data = thingy91x_ppk2.ppk2_dev.get_data()
            if read_data != b'':
                ppk_samples, _ = thingy91x_ppk2.ppk2_dev.get_samples(read_data)
                sample = sum(ppk_samples)/len(ppk_samples)
                sample = round(sample, 2)
                samples_list.append(sample)

                # Append the new sample to the Pandas Series
                samples_series = pd.concat([samples_series, pd.Series([sample])], ignore_index=True)

                # Log and store every 3 seconds
                current_time = time.time()
                if current_time - last_log_time >= 3:
                    # Calculate rolling average over the last 3 seconds
                    window_size = int(3 / SAMPLING_INTERVAL)
                    rolling_average_series = samples_series.rolling(window=window_size).mean()
                    rolling_average = rolling_average_series.iloc[-1]  # Get the last rolling average value
                    rolling_average = round(rolling_average, 2) if not pd.isna(rolling_average) else rolling_average
                    logger.info(f"Average current over last 3 secs: {rolling_average} uA")

                    if rolling_average < min_rolling_average:
                        min_rolling_average = rolling_average

                    last_log_time = current_time

        except Exception as e:
            logger.error(f"Catching exception: {e}")
            pytest.skip("Something went wrong, unable to perform power measurements")

        if rolling_average < MAX_CURRENT_PSM_UA and rolling_average > 0:
            logger.info("psm target reached for more than 3 secs")
            break
        time.sleep(SAMPLING_INTERVAL)  # lower time between sampling -> less samples read in one sampling period
    else:
        save_badge_data(min_rolling_average)
        save_measurement_data(samples_list)
        generate_time_series_html(CSV_FILE, 'Time (s)', 'Current (uA)', HMTL_PLOT_FILE)
        pytest.fail(f"PSM target not reached after {POWER_TIMEOUT} seconds, only reached {min_rolling_average} uA")
    save_badge_data(min_rolling_average)
    save_measurement_data(samples_list)
    generate_time_series_html(CSV_FILE, 'Time (s)', 'Current (uA)', HMTL_PLOT_FILE)
