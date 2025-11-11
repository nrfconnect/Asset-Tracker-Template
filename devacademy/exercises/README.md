# Asset Tracker Template - Course Exercises

## Overview

This directory contains hands-on exercises for the Asset Tracker Template Developer Academy course. Each lesson has one or more exercises that reinforce the concepts learned.

## Exercise Structure

Each exercise is organized as follows:

```
exercises/
└── lesson<N>/
    └── exercise<M>/
        ├── README.md           # Exercise instructions
        ├── base/               # Starting code (if applicable)
        └── solution/           # Complete solution
```

## Exercise List

### Lesson 1: Introduction to Asset Tracker Template

- **Exercise 1:** Build, Flash, and Provision Your First Application
  - Build the Asset Tracker Template for your hardware
  - Flash the firmware to your device
  - Provision the device with nRF Cloud
  - Verify data transmission to the cloud

### Lesson 2: Architecture - Zbus Communication

- **Exercise 1:** Add Custom Zbus Event
  - Add VBUS connected/disconnected events to the power module
  - Handle these events in the main module
  - Trigger LED patterns in response

- **Exercise 2:** Subscribe to Multiple Channels
  - Create a logging module that observes multiple channels
  - Log all messages flowing through the system
  - Use for debugging and understanding system behavior

### Lesson 3: Architecture - State Machine Framework

- **Exercise 1:** Analyze the Network Module State Machine
  - Document all states and transitions
  - Create a state diagram
  - Trace through a complete connection sequence

- **Exercise 2:** Add a New State
  - Add a backoff state to handle connection retries
  - Implement exponential backoff logic
  - Test state transitions

### Lesson 4: Working with Core Modules

- **Exercise 1:** Configure Module Behavior
  - Modify sampling intervals
  - Change network mode (LTE-M vs NB-IoT)
  - Configure location method priorities
  - Observe the effects of configuration changes

- **Exercise 2:** Modify Main Module Business Logic
  - Implement conditional sampling based on sensor readings
  - Add custom LED patterns for different conditions
  - Test the modified behavior

### Lesson 5: Cloud Connectivity and Data Management

- **Exercise 1:** Send Custom Data to nRF Cloud
  - Create a custom sensor or alert message
  - Send data using CoAP
  - View the data in nRF Cloud web UI

- **Exercise 2:** Implement Data Buffering
  - Configure the storage module for buffering
  - Test offline operation
  - Verify data is sent when connection is restored

### Lesson 6: Customization and Adding New Features

- **Exercise 1:** Add a New Sensor
  - Add support for BMM350 magnetometer
  - Integrate with the environmental module
  - Send magnetometer data to nRF Cloud

- **Exercise 2:** Create a Custom Module
  - Create an accelerometer module from the dummy template
  - Implement motion detection
  - Integrate with the main module

## Prerequisites

Before starting the exercises:

1. **Hardware Setup**
   - Thingy:91 X or nRF9151 DK connected
   - USB cable for power and programming
   - (Optional) External debugger

2. **Software Setup**
   - nRF Connect SDK v3.1.0 or later installed
   - nRF Connect for VS Code extension installed
   - Asset Tracker Template cloned and working

3. **Cloud Setup**
   - nRF Cloud account created
   - Device claimed and provisioned
   - Able to view device data in nRF Cloud

## Exercise Workflow

For each exercise:

1. **Read the instructions** in the exercise's `README.md`
2. **Start with the base code** (if provided)
3. **Implement the solution** following the steps
4. **Test your implementation** on actual hardware
5. **Compare with the solution** (if you get stuck)
6. **Verify the expected behavior**

## Getting Help

If you encounter issues:

1. **Check the lesson material** - Review the relevant lesson content
2. **Review the solution** - Compare your implementation
3. **Check logs** - Enable debug logging for relevant modules
4. **DevZone** - Ask questions on [DevZone](https://devzone.nordicsemi.com/)
5. **Documentation** - Refer to [ATT documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)

## Tips for Success

✅ **Read thoroughly** - Understand what you're implementing before coding
✅ **Test incrementally** - Test each step as you implement it
✅ **Use logging** - Add LOG statements to understand code flow
✅ **Consult documentation** - Refer to SDK and Zephyr docs as needed
✅ **Experiment** - Try variations to deepen understanding

## Building Exercises

Most exercises involve modifying the Asset Tracker Template code:

```bash
# Navigate to the app directory
cd asset-tracker-template/project/app

# Build for your board
west build -p -b thingy91x/nrf9151/ns

# Flash to device
west flash --erase

# Or for Thingy:91 X with serial bootloader
west thingy91x-dfu
```

## Monitoring Logs

To view device logs:

```bash
# Using screen
screen /dev/ttyACM0 115200

# Or using nRF Connect for Desktop Terminal
# Launch from nRF Connect for Desktop
```

## Exercise Solutions

Solutions are provided in each exercise's `solution/` directory. However:

⚠️ **Try to solve exercises yourself first** - Learning happens through problem-solving
⚠️ **Use solutions as a reference** - Not as a copy-paste source
⚠️ **Understand the solution** - Don't just copy code, understand why it works

## Course Progression

Exercises are designed to build on each other:

1. **Lesson 1** - Get familiar with the template
2. **Lessons 2-3** - Understand architecture (zbus, SMF)
3. **Lessons 4-5** - Work with modules and cloud
4. **Lesson 6** - Customize and extend

Complete exercises in order for the best learning experience.

## Additional Practice

After completing the course exercises, try:

- **Create your own use case** - Build something specific to your needs
- **Contribute to ATT** - Submit improvements to the repository
- **Help others** - Answer questions on DevZone
- **Explore advanced topics** - Deep dive into specific areas

Happy learning!

