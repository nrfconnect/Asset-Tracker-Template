# Asset Tracker Template - Developer Academy Course

Welcome to the **Asset Tracker Template Developer Academy Course**! This comprehensive, hands-on course will teach you how to build professional IoT applications using Nordic Semiconductor's Asset Tracker Template (ATT) framework.

## Course Overview

The Asset Tracker Template is a production-ready framework for developing cellular IoT applications on nRF91 Series devices. This course goes beyond basic examples to teach you the architectural patterns, design principles, and best practices used in professional embedded IoT development.

**What you'll learn:**
- Understand the modular, event-driven architecture of ATT
- Master the State Machine Framework (SMF) for predictable behavior
- Use Zbus for loose-coupled inter-module communication
- Customize and extend the template for your own applications
- Build real-world IoT solutions with confidence

**Who this course is for:**
- Developers familiar with C programming
- Those who have completed the [Cellular IoT Fundamentals](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/) course
- Anyone wanting to build production-quality cellular IoT applications

## Prerequisites

Before starting this course, you should have:

- ✅ Completed the [Cellular IoT Fundamentals Course](https://academy.nordicsemi.com/courses/cellular-iot-fundamentals/)
- ✅ Basic knowledge of C programming
- ✅ Familiarity with embedded systems concepts
- ✅ nRF Connect SDK v3.0.0 or later installed
- ✅ One of the following hardware platforms:
  - [Thingy:91 X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)
  - [nRF9151 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9151-DK)

**Installation Resources:**
- [nRF Connect SDK Installation Guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)
- [Getting Started with Asset Tracker Template](../docs/common/getting_started.md)

## Course Structure

This course consists of 5 progressive lessons, each building on the previous one:

### [Lesson 1: Introduction to Asset Tracker Template](lesson-1/lesson.md)
**Duration:** 1.5 hours

Get familiar with the Asset Tracker Template and understand its role in cellular IoT development. Learn how to set up, build, and run your first application.

- Overview of ATT architecture and features
- Setting up your development environment
- Building and running the application
- Provisioning to nRF Cloud
- Understanding the project structure

**Exercise:** Build and deploy the template to your device, provision it to nRF Cloud, and verify data transmission.

---

### [Lesson 2: Understanding the Modular Architecture](lesson-2/lesson.md)
**Duration:** 2 hours

Dive deep into the modular design that makes ATT maintainable and extensible. Learn how modules are structured and how they interact.

- Module structure and responsibilities
- Understanding the core modules (Main, Network, Cloud, Storage)
- Module initialization and lifecycle
- Threading and task management
- Watchdog integration

**Exercise:** Trace the flow of a button press through multiple modules to understand inter-module communication.

---

### [Lesson 3: State Machines with SMF](lesson-3/lesson.md)
**Duration:** 2.5 hours

Master the State Machine Framework (SMF) used throughout ATT for predictable, maintainable behavior.

- Introduction to State Machine Framework (SMF)
- Run-to-completion model
- Hierarchical state machines
- State transitions and event handling
- Analyzing existing state machines (Network, Cloud modules)

**Exercises:**
1. Visualize and understand the Network module state machine
2. Add a new state to an existing module
3. Debug state machine behavior

---

### [Lesson 4: Inter-Module Communication with Zbus](lesson-4/lesson.md)
**Duration:** 2 hours

Learn how Zbus enables loose-coupled communication between modules and how to leverage it in your applications.

- Zbus channels and messages
- Message subscribers vs. listeners
- Publishing and receiving messages
- Message routing patterns
- Best practices for event-driven design

**Exercises:**
1. Add a new Zbus event to notify about system events
2. Create a message subscriber to handle custom events
3. Implement LED feedback based on custom messages

---

### [Lesson 5: Customizing and Extending Modules](lesson-5/lesson.md)
**Duration:** 3 hours

Put everything together by customizing the template for real-world applications. Learn how to add sensors, create modules, and integrate with cloud services.

- Adding custom sensors to the Environmental module
- Creating your own module from scratch
- Integrating with different cloud backends (MQTT)
- Configuration management with Kconfig
- Best practices for maintainability

**Exercises:**
1. Add a new environmental sensor (magnetometer)
2. Create a custom module for your specific use case
3. Extend cloud integration with custom data types

---

## Learning Outcomes

By the end of this course, you will be able to:

✓ **Understand** the architectural patterns used in professional IoT applications  
✓ **Implement** state machines for complex, predictable behavior  
✓ **Design** modular, maintainable embedded systems using Zbus messaging  
✓ **Customize** the Asset Tracker Template for your specific use cases  
✓ **Build** production-ready cellular IoT applications with confidence  
✓ **Debug** and troubleshoot complex multi-module applications  
✓ **Apply** best practices from Nordic's reference implementation  

## Course Resources

### Documentation
- [Asset Tracker Template GitHub Repository](https://github.com/nrfconnect/Asset-Tracker-Template)
- [ATT Architecture Documentation](../docs/common/architecture.md)
- [ATT Customization Guide](../docs/common/customization.md)
- [Zephyr State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html)
- [Zephyr Zbus Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html)

### Support
- [Nordic DevZone](https://devzone.nordicsemi.com) - Technical support forum
- [nRF Connect SDK Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/index.html)

## Getting Help

If you encounter issues during the course:

1. **Check the documentation** - Most common issues are covered in the [Troubleshooting Guide](../docs/common/tooling_troubleshooting.md)
2. **Review the example code** - All exercises have complete solution implementations
3. **Ask on DevZone** - The community is active and helpful
4. **Check the GitHub issues** - Known issues and workarounds are documented

## Ready to Start?

Great! Let's begin with [Lesson 1: Introduction to Asset Tracker Template](lesson-1/lesson.md).

---

**Course Version:** 1.0  
**Last Updated:** November 2025  
**Compatible with:** nRF Connect SDK v3.0.0+  
**Supported Hardware:** Thingy:91 X, nRF9151 DK

