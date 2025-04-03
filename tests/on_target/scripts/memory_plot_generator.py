#!/usr/bin/env python3

import pandas as pd
import plotly.express as px
from datetime import datetime
import os
import sys

def append_to_memory_csv(filename, used, total, current_date, output_dir):
    """Append memory data to a CSV file."""
    data = {'Date': current_date, 'Used (B)': used, 'Total (B)': total, 'Usage (%)': (used/total)*100}
    df = pd.DataFrame([data])

    csv_path = os.path.join(output_dir, filename)

    if os.path.exists(csv_path):
        print(f"CSV exists at {csv_path}, appending data")
        # Read existing data
        existing_df = pd.read_csv(csv_path)
        print(f"Existing data:\n{existing_df}")

        # Combine existing and new data
        combined_df = pd.concat([existing_df, df], ignore_index=True)
        print(f"Combined data:\n{combined_df}")

        # Write back the complete dataset
        combined_df.to_csv(csv_path, index=False)
    else:
        print(f"Creating new CSV at {csv_path}")
        df.to_csv(csv_path, index=False)

def append_to_csv(ram_used, ram_total, flash_used, flash_total, output_dir):
    """Append memory usage data to CSV files with timestamps."""
    current_date = datetime.now().strftime('%Y-%m-%d %H:%M:%S')

    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    # Append to RAM and Flash CSVs
    append_to_memory_csv("ram_history.csv", ram_used, ram_total, current_date, output_dir)
    append_to_memory_csv("flash_history.csv", flash_used, flash_total, current_date, output_dir)

def generate_memory_plots(output_dir):
    """Generate HTML plots from the CSV data."""
    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    # RAM Plot
    ram_csv = os.path.join(output_dir, "ram_history.csv")
    if os.path.exists(ram_csv):
        df_ram = pd.read_csv(ram_csv)
        df_ram['Date'] = pd.to_datetime(df_ram['Date'])

        # Calculate y-axis range for RAM
        y_min_ram = 0  # Start from 0
        y_max_ram = df_ram['Total (B)'].max() * 1.1  # 10% margin above max total

        fig_ram = px.scatter(df_ram, x='Date', y=['Used (B)', 'Total (B)'],
                           title='RAM Usage History - Asset Tracker Template',
                           labels={'value': 'Bytes', 'variable': 'Metric'})
        # Add markers
        fig_ram.update_traces(mode='markers', marker=dict(size=10))
        # Set y-axis range
        fig_ram.update_layout(yaxis=dict(range=[y_min_ram, y_max_ram]))
        fig_ram.write_html(os.path.join(output_dir, "ram_history_plot.html"))

    # Flash Plot
    flash_csv = os.path.join(output_dir, "flash_history.csv")
    if os.path.exists(flash_csv):
        df_flash = pd.read_csv(flash_csv)
        df_flash['Date'] = pd.to_datetime(df_flash['Date'])

        # Calculate y-axis range for Flash
        y_min_flash = 0  # Start from 0
        y_max_flash = df_flash['Total (B)'].max() * 1.1  # 10% margin above max total

        fig_flash = px.scatter(df_flash, x='Date', y=['Used (B)', 'Total (B)'],
                             title='Flash Usage History - Asset Tracker Template',
                             labels={'value': 'Bytes', 'variable': 'Metric'})
        # Add markers
        fig_flash.update_traces(mode='markers', marker=dict(size=10))
        # Set y-axis range
        fig_flash.update_layout(yaxis=dict(range=[y_min_flash, y_max_flash]))
        fig_flash.write_html(os.path.join(output_dir, "flash_history_plot.html"))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output_directory>")
        sys.exit(1)
    output_dir = sys.argv[1]
    generate_memory_plots(output_dir)
