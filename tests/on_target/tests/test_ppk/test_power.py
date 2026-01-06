##########################################################################################
# Copyright (c) 2025 Nordic Semiconductor
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
POWER_TIMEOUT = 60 * 5

MAX_CURRENT_PSM_UA = 10
GREEN_THRESOLD_CURRENT_UA = MAX_CURRENT_PSM_UA  # ≤ 10μA (good PSM current)
YELLOW_THRESOLD_CURRENT_UA = 100  # ≤ 100μA (concerning but possible)
RED_THRESOLD_CURRENT_UA = 1000  # ≤ 1000μA (bad but still plausible)
# Readings above 1000uA are probably due to PPK crapping out, test is skipped in that case

SAMPLING_INTERVAL = 0.01
CSV_FILE = "power_measurements.csv"
HMTL_PLOT_FILE = "power_measurements_plot.html"
SEGGER = os.getenv('SEGGER')


def save_badge_data(average):
    badge_filename = "power_badge.json"
    logger.info(f"Minimum average current measured: {average}uA")
    color = "green"
    if average < 0:
        pytest.fail(f"Current can't be negative, current average: {average}")
    elif average <= GREEN_THRESOLD_CURRENT_UA:
        color = "green"
    elif average <= YELLOW_THRESOLD_CURRENT_UA:
        color = "yellow"
    elif average <= RED_THRESOLD_CURRENT_UA:
        color = "red"
    else:
        pytest.fail(f"Test failed due to unreliable PPK reading: {average} uA")

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
    df = pd.read_csv(csv_file)

    title = "Asset Tracket Template Current Consumption Plot\n\n"
    note_text = "Note: Measures are taken with PPK2"
    title += f"<br><span style='font-size:12px;color:gray;'>{note_text}</span>"

    # Create an interactive Plotly line chart
    fig = px.line(df, x=date_column, y=value_column, title=title)

    # Save the plot to an HTML file
    fig.write_html(output_file)

    logger.info(f"HTML file generated: {output_file}")
    return output_file

def check_ppk_serial_operational(s):
    '''
    Check if the PPK serial is operational by writing "device" and waiting for "devices"
    '''
    for _ in range(5):
        try:
            s.write("device\r\n")
            s.wait_for_str("devices", timeout=2)
            break
        except Exception:
            continue
    else:
        pytest.fail("PPK device is not responding")

def get_ppk2_serials():
    '''
    Get the serial ports of the PPK2 devices
    '''
    ppk2s_connected = PPK2_API.list_devices()
    ppk2s_connected.sort()
    if len(ppk2s_connected) == 2:
        ppk2_port = ppk2s_connected[0]
        ppk2_serial = ppk2s_connected[1]
        logger.info(f"Found PPK2 at port: {ppk2_port}, serial: {ppk2_serial}")
        return ppk2_port, ppk2_serial
    elif len(ppk2s_connected) == 0:
        pytest.fail("No ppk found")
    else:
        pytest.fail(f"PPK should list 2 ports, but found {ppk2s_connected}")

@pytest.fixture(scope="module")

def thingy91x_ppk2():
    ppk2_port, ppk2_serial = get_ppk2_serials()
    shell = Uart(ppk2_serial)
    try:
        check_ppk_serial_operational(shell)
        try:
            shell.write("kernel reboot cold\r\n")
            time.sleep(1)
            shell.stop()
        except Exception:
            pass
        time.sleep(1)
        ppk2_port, ppk2_serial = get_ppk2_serials()
        shell = Uart(ppk2_serial)
        check_ppk_serial_operational(shell)
        time.sleep(1)
        shell.write("kernel uptime\r\n")
        uptime = shell.wait_for_str_re("Uptime: (.*) ms", timeout=2)
        device_uptime_ms = int(uptime[0])
        if device_uptime_ms > 20000:
            pytest.fail("PPK device was not rebooted")
    except Exception as e:
        logger.error(f"Exception when rebooting PPK device: {e}")
    finally:
        shell.stop()

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
        pytest.fail("Failed to get ppk modifiers after 15 attempts")

    ppk2_dev.use_ampere_meter()  # set ampere meter mode
    ppk2_dev.set_source_voltage(3300)
    ppk2_dev.toggle_DUT_power("OFF")  # disable DUT power
    time.sleep(2)
    ppk2_dev.toggle_DUT_power("ON")  # enable DUT power
    ppk2_dev.start_measuring()

    time.sleep(10)
    for _ in range(10):
        try:
            all_uarts = get_uarts()
            if not all_uarts:
                logger.error("No UARTs found")
            log_uart_string = all_uarts[0]
            break
        except Exception as e:
            logger.warning(f"Exception: {e}")
            ppk2_dev.toggle_DUT_power("OFF")
            time.sleep(2)
            ppk2_dev.toggle_DUT_power("ON")
            time.sleep(5)
            continue
    else:
        pytest.fail("NO uart after 10 attempts")

    t91x_uart = Uart(log_uart_string, timeout=UART_TIMEOUT)

    yield types.SimpleNamespace(ppk2_dev=ppk2_dev, t91x_uart=t91x_uart)

    t91x_uart.stop()
    recover_device(serial=SEGGER)
    ppk2_dev.stop_measuring()
    ppk2_dev.toggle_DUT_power("OFF")

@pytest.mark.slow
def test_power(thingy91x_ppk2, debug_hex_file):
    '''
    Test that the device can reach PSM and measure the current consumption

    Current consumption is measured and report generated.
    '''
    flash_device(os.path.abspath(debug_hex_file), serial=SEGGER)
    reset_device(serial=SEGGER)
    try:
        thingy91x_ppk2.t91x_uart.wait_for_str("Connected to Cloud", timeout=120)
    except AssertionError:
        pytest.fail("Device unable to connect to cloud")

    # Disable UART on the device
    thingy91x_ppk2.t91x_uart.write("pm suspend uart@9000\r\n")
    thingy91x_ppk2.t91x_uart.write("pm suspend uart@8000\r\n")

    start = time.time()
    min_rolling_average = float('inf')
    rolling_average = float('inf')
    samples_list = []
    last_log_time = start
    psm_reached = False

    # Initialize an empty pandas Series to store samples over time
    samples_series = pd.Series(dtype='float64')
    while time.time() < start + POWER_TIMEOUT:
        try:
            read_data = thingy91x_ppk2.ppk2_dev.get_data()
            if read_data != b'':
                ppk_samples, _ = thingy91x_ppk2.ppk2_dev.get_samples(read_data)
                sample = sum(ppk_samples) / len(ppk_samples)
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

                    # Check if PSM target has been reached
                    if 0 < rolling_average < MAX_CURRENT_PSM_UA:
                        psm_reached = True

        except Exception as e:
            logger.error(f"Catching exception: {e}")
            pytest.fail("Something went wrong, unable to perform power measurements")

        time.sleep(SAMPLING_INTERVAL)  # lower time between sampling -> less samples read in one sampling period

    # Save measurement data and generate HTML report
    save_badge_data(min_rolling_average)
    save_measurement_data(samples_list)
    generate_time_series_html(CSV_FILE, 'Time (s)', 'Current (uA)', HMTL_PLOT_FILE)

    # Determine test result based on whether PSM was reached
    if psm_reached:
        pass  # Test passes if PSM was reached
    elif min_rolling_average > RED_THRESOLD_CURRENT_UA:
        # Fail test if reading is absurdly high (likely PPK error)
        # Note: save_badge_data won't create file in this case
        pytest.fail(f"Test failed due to unreliable PPK reading: {min_rolling_average} uA")
    elif min_rolling_average > YELLOW_THRESOLD_CURRENT_UA:
        # Fail if current is in red zone (but still plausible)
        pytest.fail(f"PSM target not reached after {POWER_TIMEOUT / 60} minutes, current too high: {min_rolling_average} uA")
    else:
        # Current is between 0 and YELLOW_THRESHOLD but PSM wasn't reached
        pytest.fail(f"PSM target not reached after {POWER_TIMEOUT / 60} minutes, only reached {min_rolling_average} uA")
