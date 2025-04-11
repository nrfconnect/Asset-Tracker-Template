# Troubleshooting
General overview of tools used to troubleshoot the template and/or modem/network behavior.
It's recommended to complete the Nordic Developer Academy lesson: [Debugging and troubleshooting](https://academy.nordicsemi.com/courses/nrf-connect-sdk-intermediate/lessons/lesson-2-debugging/) for tips in general how to debug applications based on [nRF Connect SDK](https://github.com/nrfconnect/sdk-nrf).

# Shell
To control and get information about certain aspects of the template, shell can be used.
To use shell, connect to the devices UART interface either via your own terminal or the [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop) Serial terminal application.
Here is an example of how shell can be used to send a message to [nRF Cloud](https://nrfcloud.com):

```
uart:~$ att_cloud_publish TEMP "24"
Sending on payload channel: {"messageType":"DATA","appId":"TEMP","data":"24","ts":1744359144653} (68 bytes)
```

The following command "help" lists all the available commands in the template:

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

The commands prefixed *att* are commands that are specific to the template.
These shell commands are implemented in shell modules corresponding to the aspect of the template they control.
For example, the network module implements the file `network_shell.c` and is located in the module folder.

# Low Power Profiling
The Power consumption of the device can be profiled using PPK.

# Debugging using West (GDB)
The template can be easily debugged using GDB via the west commands `west attach`.
The following example shows how GDB can be used to set a breakpoint and print a backtrace once the breakpoint is hit:

```
Insert terminal session showing how its used here.
```

Fore more information about debugging with west, see [West debugging](https://docs.zephyrproject.org/latest/develop/west/build-flash-debug.html#debugging-west-debug-west-debugserver)

# Debugging using SEGGER SystemView
[Segger SystemView](https://www.segger.com/products/development-tools/systemview/) is a real-time analysis tool that can be used to analyse thread execution and scheduling in the device.
To build the template with RTT tracing to Segger SystemView set the following options in your build configuration:

```
CONFIG_TRACING=y
CONFIG_USE_SEGGER_RTT=y
CONFIG_SEGGER_SYSTEMVIEW=y
CONFIG_SEGGER_SYSTEMVIEW_BOOT_ENABLE=n
CONFIG_SEGGER_SYSVIEW_POST_MORTEM_MODE=n

```

Or build with the predefined RTT trace snippet:

```
west build -p -b <board> -S rtt-tracing
```

Note that this snippet disables debug optimizations and that it may not be enough space in your application to use it as disabling optimizations increases the overall memory use of the application.

# Thread Analyser
The Trace analyzer subsystem is useful when optimizing the applications stack sizes.
Add the following options to your build configurations to get stack information printed every 30 seconds:

```
CONFIG_THREAD_ANALYZER=y
CONFIG_THREAD_ANALYZER_USE_LOG=y
CONFIG_THREAD_ANALYZER_AUTO=y
CONFIG_THREAD_ANALYZER_AUTO_INTERVAL=30
CONFIG_THREAD_ANALYZER_AUTO_STACK_SIZE=1024
CONFIG_THREAD_NAME=y
```

# TF-M logging
To get logs from the secure image TF-M of the application forwarded to UART0 (application UART output) you can build with the following snippet:

```
west build -p -b <board> -S tfm-enable-share-uart
```

In case of secure faults, the fault frame information will be printed with accompanied information of the violating non-secure SP and LR registers.

# Debugging using Memfault (Remote debugging)
The template implements overlays to enable remote debugging to [Memfault](https://memfault.com/).
To get familiar with Memfault and how to do remote debugging, its recommended to go through the Nordic Developer Academy excersise: [Remote Debugging with Memfault](https://academy.nordicsemi.com/courses/nrf-connect-sdk-intermediate/lessons/lesson-2-debugging/topic/exercise-4-remote-debugging-with-memfault/)

Refer to the [Getting Started](getting_started.md) section to see how the template can be built with overlays enabling Memfault functionality.
If you want to test/simulate faults on the device to test Memfault, the template implements Memfault shell commands that can be used to trigger typical faults.
Here is an example of triggering a usagefault:

```
insert example here
```

Fore more information on NCSs Memfault implementation, refer to the Memfault NCS sample documentation, integration as well as the templates CI Memfault on-target tests:

* [Memfault sample](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/debug/memfault/README.html)
* [Memfault integration](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/debug/memfault_ncs.html)
* [CI tests](Asset-Tracker-Template/tests/on_target/tests/test_functional/test_memfault.py)

To use Memfault, you will need to register and setup a project.
This can be done via the following link, [Memfault registration page](https://app.memfault.com/register-nordic)

# Debugging using Modem traces
Modem traces can be captured over UART to debug LTE, IP, and modem issues.
This can be done by including a snippet in the build command specifying which medium you want to trace to.
This build command enables modem tracing over UART1:

```
west build -p -b <board> -S nrf91-modem-trace-uart
```

This build commands enables modem tracing over RTT:

```
west build -p -b <board> -S nrf91-modem-trace-uart
```

To capture modem traces over UART on a host computer either [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/) Cellular Monitor application can be used nRFUtil CLI:

```
nrfutil trace lte --input-serialport /dev/ttyACM1 --output-pcapng trace
```

for RTT traces you can use the RTT logger to capture the modem traces and later convert them to PCAP using nRF Util or the Cellular Monitor:

```
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -RTTChannel 1 modem_trace.bin
```

If you also want to route logging via RTT you can capture both modem traces and application logs at the same time using debuggers RTT multichannel functionality.
To do so you build the application with the RTT modem trace snippet and add the following options to the project configurations:

```
CONFIG_USE_SEGGER_RTT=y
CONFIG_LOG_BACKEND_RTT=y
CONFIG_SHELL_BACKEND_RTT=y
CONFIG_SHELL_BACKEND_RTT_BUFFER=1
```

This will split RTT tracing and logging into two channels, the you can call two separate commands to trace on both channels:

Terminal 1:

```
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -RTTChannel 1 modem_trace.bin
```

Terminal 2:

```
JLinkRTTLogger -Device NRF9160_XXAA -If SWD -Speed 50000 -RTTChannel 0 terminal.txt
```

Just remember that the modem trace is captured in a binary format that needs to be converted by either nRF Util or the Cellular Monitor application.
To convert a binary modem trace using nRF Util you can use the following command:

```
nrfutil trace lte --input-file modemtraces.bin --output-wireshark
```

Its recommended to go through the [Nordic Developer Academy Debugging with a Modem Trace excersise](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/lessons/lesson-7-cellular-fundamentals/) to familiarize yourself with modem tracing on the nRF91 Series.

# Common issues

## Not getting connected to network
- problem (description, UART/trace output)  -
- suggested tooling/debuggin to find the source of the issue - (modem trace / memfault)
- Suggested fixes -
- Link to similar devzone cases -

## Cloud connection timeout
- problem (description, UART/trace output)  -
- suggested tooling/debuggin to find the source of the issue - (modem trace / memfault)
- Suggested fixes -
- Link to similar devzone cases -