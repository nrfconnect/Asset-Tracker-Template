# Project instructions

## On-target tests / device serial output

When running or monitoring anything that involves a device's serial port output
(UART logs, on-target pytest runs under `tests/on_target/`, etc.) in the
background, always give the log file path and a `tail -f <path>` command so the
output can be watched live in a separate terminal, in addition to reporting
back when the background command completes.
