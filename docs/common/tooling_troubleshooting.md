# Troubleshooting
General overview of tools used to troubleshoot the template and/or modem/network behavior.

## Prerequisites
It's recommended to complete the Nordic Developer Academy lessons:
- [Debugging and troubleshooting](https://academy.nordicsemi.com/courses/nrf-connect-sdk-intermediate/lessons/lesson-2-debugging/)
- [Cellular IoT Fundamentals](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)

For more information about debugging applications based on [nRF Connect SDK](https://github.com/nrfconnect/sdk-nrf), refer to:
- [nRF Connect SDK Debugging Guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/test_and_optimize/debugging.html)
- [Zephyr Debugging Guide](https://docs.zephyrproject.org/latest/develop/debug/index.html)

# Shell Commands
The template provides several shell commands for controlling and monitoring device behavior. Connect to the device's UART interface using either:
- Your preferred terminal application (e.g., `screen`, `minicom`, `terraterm`)
- [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop) Serial terminal application

## Available Commands
Run `help` to list all available commands:
```
uart:~$ help
Available commands:
  at                 : Execute an AT command
  att_button_press   : Asset Tracker Template Button CMDs
  att_cloud_publish  : Asset Tracker Template Cloud CMDs
  att_network        : Asset Tracker Template Network CMDs
  mflt_nrf           : Memfault nRF Connect SDK Test Commands
  ...
```

### Common Shell Commands Examples

#### Cloud Publishing
```
uart:~$ att_cloud_publish TEMP "24"
Sending on payload channel: {"messageType":"DATA","appId":"TEMP","data":"24","ts":1744359144653} (68 bytes)
```

#### AT Command Execution
```
uart:~$ at AT+CGSN
+CGSN: "123456789012345"
OK
```

# Debugging Tools

## Low Power Profiling
Profile power consumption using the Power Profiler Kit (PPK):
1. Connect PPK to your device
2. Use [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop) Power Profiler application
3. Configure sampling rate and capture duration
4. Analyze power consumption patterns

For detailed power profiling guidance:
- [Power Profiler Kit User Guide](https://docs.nordicsemi.com/bundle/ug_ppk2/page/UG/ppk/PPK_user_guide_Intro.html)

## GDB Debugging
Debug the template using GDB via west commands:

```bash
# Attach GDB, skip rebuilding application
west attach --skip-rebuild
```

Common GDB commands:
```
(gdb) tui enable
(gdb) break main
(gdb) continue
(gdb) backtrace
(gdb) print variable_name
(gdb) next
(gdb) step
```

For more information, see:
- [West Debugging Guide](https://docs.zephyrproject.org/latest/develop/west/build-flash-debug.html#debugging-west-debug-west-debugserver)
- [nRF Connect SDK Debugging](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/ug_debugging.html)
- [GDB Manual](https://man7.org/linux/man-pages/man1/gdb.1.html)

## SEGGER SystemView
Analyze thread execution and scheduling using [SEGGER SystemView](https://www.segger.com/products/development-tools/systemview/).

### Configuration
Add to `prj.conf`:
```
CONFIG_TRACING=y
CONFIG_USE_SEGGER_RTT=y
CONFIG_SEGGER_SYSTEMVIEW=y
CONFIG_SEGGER_SYSTEMVIEW_BOOT_ENABLE=n
CONFIG_SEGGER_SYSVIEW_POST_MORTEM_MODE=n
```

Or build with predefined configuration:
```bash
west build -p -b <board> -S rtt-tracing
```

Note: Disabling optimizations increases memory usage. Ensure sufficient space in your application.

## Thread Analysis
Monitor and optimize stack sizes using the Thread Analyzer:

Add to `prj.conf`:
```
CONFIG_THREAD_ANALYZER=y
CONFIG_THREAD_ANALYZER_USE_LOG=y
CONFIG_THREAD_ANALYZER_AUTO=y
CONFIG_THREAD_ANALYZER_AUTO_INTERVAL=30
CONFIG_THREAD_ANALYZER_AUTO_STACK_SIZE=1024
CONFIG_THREAD_NAME=y
```

[Zephyr Thread Analyzer](https://docs.zephyrproject.org/latest/services/debugging/thread-analyzer.html)

## TF-M Logging
Enable secure image logging by building with:
```bash
west build -p -b <board> -S tfm-enable-share-uart
```

Secure faults will display:
- Fault frame information
- Non-secure SP and LR registers
- Violation details

For more information:
- [TF-M Documentation](https://tf-m-user-guide.trustedfirmware.org/)
- [nRF Connect SDK TF-M Guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/security/tfm/index.html)

## Memfault Remote Debugging
Prerequisites:
 - Registered to Memfault
 - Memfault project setup and project key retrieved

The template supports remote debugging via [Memfault](https://memfault.com/).

Remote debugging enables the device to send metrics suchs as LTE, GNSS and memory statistics as well as coredump captures on crashes to analyse problems across single or fleet of devices once they occur.

To build the application with support for Memfault you need to build with the Memfault overlay `overlay-memfault.conf`. If you want to capture and send modem traces to Memfault on coredumps, you can include the overlay `overlay-publish-modem-traces-to-memfault.conf`.

**Important Note on Data Usage**: Enabling Memfault will increase your device's data usage. This is especially true when using the modem trace upload feature, which can send upwards of 1MB of modem trace data in case of application crashes. Consider this when planning your data usage and costs.

For detailed build instructions and how to supply the project key, refer to the [Getting Started Guide](getting_started.md).

**IMPORTANT** In order to properly use Memfault and be able to decode metrics and coredumps sent from the device, you need to upload the ELF file located in the build folder of the template once you have built the application.

### Setup
1. Register at [Memfault](https://app.memfault.com/register-nordic)
2. Complete the [Remote Debugging with Memfault](https://academy.nordicsemi.com/courses/nrf-connect-sdk-intermediate/lessons/lesson-2-debugging/topic/exercise-4-remote-debugging-with-memfault/) exercise

### Testing Faults
Trigger test faults using shell commands:
```
uart:~$ mflt_nrf test hardfault
uart:~$ mflt_nrf test assert
uart:~$ mflt_nrf test usagefault
```

For more information:
- [Memfault Sample](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/debug/memfault/README.html)
- [Memfault Integration](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/debug/memfault_ncs.html)

## Modem Tracing
Capture and analyze modem behavior for LTE, IP, modem issues.

### UART Tracing
Build with:
```bash
west build -p -b <board> -S nrf91-modem-trace-uart
```

Capture traces using:
```bash
# Using nRF Connect for Desktop Cellular Monitor
# or
nrfutil trace lte --input-serialport /dev/ttyACM1 --output-pcapng trace
```

### RTT Tracing
Build with:
```bash
west build -p -b <board> -S nrf91-modem-trace-rtt
```

Capture traces:
```bash
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -RTTChannel 1 modem_trace.bin
```

### Combined RTT Logging
For simultaneous modem traces and application logs:

Add to `prj.conf`:
```
CONFIG_USE_SEGGER_RTT=y
CONFIG_LOG_BACKEND_RTT=y
CONFIG_SHELL_BACKEND_RTT=y
CONFIG_SHELL_BACKEND_RTT_BUFFER=1
```

Capture in separate terminals:
```bash
# Terminal 1 - Modem traces
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -RTTChannel 1 modem_trace.bin

# Terminal 2 - Application logs
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -RTTChannel 0 terminal.txt
```

Convert binary traces:
```bash
nrfutil trace lte --input-file modemtraces.bin --output-wireshark
```

For more information:
- [nRF Connect SDK Modem Tracing](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrfxlib/nrf_modem/doc/modem_trace.html)

# Common Issues and Solutions

## Network Connection Issues

### Symptoms
- Device fails to connect to network
- Frequent disconnections
- Poor signal strength

### Debugging Steps
1. Check modem traces for connection attempts
2. Verify SIM card status
3. Monitor signal strength using `att_network status`
4. Check for proper antenna connection

### Common Solutions
- Ensure proper antenna connection
- Verify SIM card is active and valid
- Check network coverage in your area
- Reset modem using AT commands if needed
