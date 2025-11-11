# Asset Tracker Template - Developer Academy Course

## Overview

This course provides comprehensive training on the Asset Tracker Template (ATT) for nRF91 Series devices. The Asset Tracker Template is a modular, event-driven framework built on nRF Connect SDK and Zephyr RTOS, designed for battery-powered IoT applications with cloud connectivity, location tracking, and sensor data collection.

## Prerequisites

Before starting this course, you should:

- Complete the [Cellular IoT Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- Have basic knowledge of C programming
- Be familiar with nRF Connect SDK development environment
- Have completed the [nRF Connect SDK Fundamentals Course](https://academy.nordicsemi.com/courses/nrf-connect-sdk-fundamentals/) (recommended)

## Hardware Requirements

This course requires one of the following hardware platforms:

- **Thingy:91 X** - Recommended for full feature access including sensors and RGB LED
- **nRF9151 DK** - Alternative development kit with full debugging capabilities
- **nRF9160 DK** or **nRF9161 DK** - Also supported

## Software Requirements

- nRF Connect SDK v3.1.0 or later
- nRF Connect for VS Code extension
- nRF Cloud account (free tier available)

## Course Structure

This course consists of six lessons covering the Asset Tracker Template from fundamentals to advanced customization:

### Lesson 1: Introduction to Asset Tracker Template
- Overview of the Asset Tracker Template
- System architecture and design principles
- Setting up your development environment
- Building and flashing your first application
- Provisioning and connecting to nRF Cloud
- **Exercise 1:** Build, flash, and provision the Asset Tracker Template

### Lesson 2: Architecture - Zbus Communication
- Understanding event-driven architecture
- Zbus channels and message types
- Message subscribers and listeners
- Publishing and receiving messages
- Inter-module communication patterns
- **Exercise 1:** Add a custom zbus event to the power module
- **Exercise 2:** Subscribe to multiple channels in the main module

### Lesson 3: Architecture - State Machine Framework
- Introduction to State Machine Framework (SMF)
- Run-to-completion model
- State definition and transitions
- Hierarchical state machines
- State machine context and execution
- **Exercise 1:** Analyze the network module state machine
- **Exercise 2:** Add a new state to an existing module

### Lesson 4: Working with Core Modules
- Main module: Business logic coordinator
- Network module: LTE connectivity management
- Cloud module: nRF Cloud CoAP communication
- Location module: GNSS and positioning services
- Environmental module: Sensor data collection
- Power module: Battery monitoring
- **Exercise 1:** Configure module behavior using Kconfig
- **Exercise 2:** Modify the main module's business logic

### Lesson 5: Cloud Connectivity and Data Management
- nRF Cloud integration and provisioning
- CoAP communication protocol
- Data encoding with CBOR
- Storage module and data buffering
- Firmware over-the-air (FOTA) updates
- **Exercise 1:** Send custom data to nRF Cloud
- **Exercise 2:** Implement data buffering for offline operation

### Lesson 6: Customization and Adding New Features
- Adding environmental sensors
- Creating custom modules
- Extending existing modules
- Power optimization techniques
- Alternative cloud backends (MQTT)
- **Exercise 1:** Add a new sensor to the environmental module
- **Exercise 2:** Create a custom module from the dummy template

## Learning Objectives

By the end of this course, you will be able to:

1. Understand the modular architecture of the Asset Tracker Template
2. Work with zbus for inter-module communication
3. Implement and modify state machines using SMF
4. Configure and customize existing modules
5. Add new sensors and functionality
6. Integrate with nRF Cloud for data collection and device management
7. Implement power-efficient IoT applications
8. Create custom modules following ATT design patterns

## Additional Resources

- [Asset Tracker Template Documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)
- [nRF Connect SDK Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/latest/)
- [Nordic Developer Zone](https://devzone.nordicsemi.com/)

## Course Materials

- **Lessons:** Detailed theory and explanations in the `lessons/` directory
- **Exercises:** Hands-on coding exercises in the `exercises/` directory
- **Solutions:** Complete exercise solutions in the `exercises/<lesson>/solution/` directories

## Getting Started

1. Review the prerequisites and ensure you have completed the required courses
2. Set up your hardware and software according to the requirements
3. Clone the Asset Tracker Template repository
4. Start with Lesson 1 and work through the course sequentially

## Support

If you encounter issues or have questions:

- Check the [troubleshooting guide](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/page/common/tooling_troubleshooting.html)
- Visit [Nordic Developer Zone](https://devzone.nordicsemi.com/)
- Review the [Asset Tracker Template documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)

Good luck with your learning journey!

