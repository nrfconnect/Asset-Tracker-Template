# Asset Tracker Template Developer Academy Course - Summary

## Course Completion

Congratulations! This directory contains a complete Developer Academy course on the Asset Tracker Template for nRF91 Series devices.

## Course Structure

### Lessons (6 lessons, ~10-12 hours total)

1. **Lesson 1: Introduction to Asset Tracker Template** (90-120 min)
   - Overview and architecture
   - Development environment setup
   - Building, flashing, and provisioning
   - First data transmission to nRF Cloud

2. **Lesson 2: Architecture - Zbus Communication** (90-120 min)
   - Message-based communication fundamentals
   - Zbus channels and message types
   - Message subscribers and listeners
   - Publishing and receiving messages
   - Custom event implementation

3. **Lesson 3: Architecture - State Machine Framework** (90-120 min)
   - State machine design principles
   - SMF run-to-completion model
   - State definitions and transitions
   - Hierarchical state machines
   - Integration with zbus

4. **Lesson 4: Working with Core Modules** (120-150 min)
   - Main module: Business logic coordinator
   - Network module: LTE connectivity
   - Cloud module: nRF Cloud CoAP
   - Location module: GNSS and positioning
   - Other modules: Storage, FOTA, Environmental, Power, LED, Button
   - Module interactions and configuration

5. **Lesson 5: Cloud Connectivity and Data Management** (120-150 min)
   - CoAP protocol for IoT
   - CBOR encoding and data efficiency
   - Device shadows and configuration
   - Data flow and storage
   - Firmware over-the-air (FOTA) updates
   - Debugging cloud connectivity

6. **Lesson 6: Customization and Adding New Features** (120-180 min)
   - Adding environmental sensors
   - Creating custom modules
   - Extending existing modules
   - Power optimization strategies
   - Alternative cloud backends (MQTT)
   - Production best practices

### Exercises (12 exercises)

Each lesson includes hands-on exercises:

- **Lesson 1:** Build, flash, and provision (1 exercise)
- **Lesson 2:** Add custom zbus events, multi-channel logging (2 exercises)
- **Lesson 3:** Analyze state machines, add new states (2 exercises)
- **Lesson 4:** Configure modules, modify business logic (2 exercises)
- **Lesson 5:** Send custom data, implement buffering (2 exercises)
- **Lesson 6:** Add sensors, create custom modules (2 exercises)

## Learning Objectives Achieved

By completing this course, students will be able to:

### Technical Skills

✅ **Architecture Understanding**
- Explain the modular architecture of the Asset Tracker Template
- Understand event-driven design and message-based communication
- Work with zbus for inter-module communication
- Implement state machines using SMF

✅ **Module Development**
- Configure and customize existing modules
- Create new modules from templates
- Extend modules with new functionality
- Integrate modules with the system

✅ **Cloud Integration**
- Connect devices to nRF Cloud via CoAP
- Understand CBOR encoding and benefits
- Work with device shadows for configuration
- Implement FOTA updates
- Send custom data to the cloud

✅ **Customization**
- Add new sensors to the system
- Modify business logic for specific use cases
- Implement power optimization
- Configure alternative cloud backends

✅ **Production Readiness**
- Apply best practices for production deployment
- Implement robust error handling
- Configure security features
- Test and debug effectively

### Conceptual Understanding

✅ **IoT Architecture** - Understand modular, event-driven IoT systems
✅ **Cellular IoT** - Grasp LTE-M/NB-IoT operation and power management
✅ **Cloud Connectivity** - Understand IoT cloud platforms and protocols
✅ **Firmware Management** - Comprehend OTA update processes
✅ **Real-time Systems** - Work with Zephyr RTOS concepts

## Course Materials Included

### Documentation

```
devacademy/
├── README.md                      # Course overview
├── COURSE_SUMMARY.md              # This file
├── lessons/                       # 6 comprehensive lessons
│   ├── lesson1_introduction.md
│   ├── lesson2_zbus_communication.md
│   ├── lesson3_state_machine_framework.md
│   ├── lesson4_core_modules.md
│   ├── lesson5_cloud_connectivity.md
│   └── lesson6_customization.md
└── exercises/                     # 12 hands-on exercises
    ├── README.md                  # Exercise overview
    ├── lesson1/exercise1/         # Build and provision
    ├── lesson2/exercise1/         # Add custom zbus event
    ├── lesson2/exercise2/         # Multi-channel logging
    ├── lesson3/exercise1/         # Analyze state machine
    ├── lesson3/exercise2/         # Add new state
    ├── lesson4/exercise1/         # Configure modules
    ├── lesson4/exercise2/         # Modify business logic
    ├── lesson5/exercise1/         # Send custom data
    ├── lesson5/exercise2/         # Implement buffering
    ├── lesson6/exercise1/         # Add new sensor
    └── lesson6/exercise2/         # Create custom module
```

### Teaching Materials

Each lesson includes:
- **Comprehensive theory** - Detailed explanations with examples
- **Code snippets** - Real implementation examples
- **Diagrams** - Visual representations of concepts
- **Best practices** - Industry-standard approaches
- **Troubleshooting** - Common issues and solutions
- **References** - Links to additional resources

Each exercise includes:
- **Clear objectives** - What students will achieve
- **Step-by-step instructions** - Detailed implementation guide
- **Verification steps** - How to test the implementation
- **Expected results** - What success looks like
- **Troubleshooting** - Help with common issues
- **Bonus challenges** - Extensions for advanced students

## Prerequisites

### Knowledge Prerequisites

- Basic C programming
- Understanding of embedded systems concepts
- Familiarity with command-line tools
- (Recommended) Completion of:
  - Cellular IoT Fundamentals Course
  - nRF Connect SDK Fundamentals Course

### Hardware Prerequisites

- Thingy:91 X (recommended) or nRF9151 DK
- USB cable for programming and power
- (Optional) External debugger for advanced debugging

### Software Prerequisites

- nRF Connect SDK v3.1.0 or later
- nRF Connect for VS Code extension
- nRF Command Line Tools
- Git

### Cloud Prerequisites

- nRF Cloud account (free tier available)
- Active cellular data plan (if using real SIM)

## Target Audience

This course is designed for:

- **Embedded developers** transitioning to cellular IoT
- **IoT engineers** working with Nordic nRF91 Series
- **Product developers** building asset tracking applications
- **System architects** designing cellular IoT solutions
- **Technical leads** evaluating the Asset Tracker Template

## Teaching Recommendations

### Course Delivery

**Self-Paced:**
- Students work through lessons independently
- Estimated 2-3 weeks with 4-6 hours/week
- Support via DevZone forums

**Instructor-Led:**
- 3-day intensive workshop
- Day 1: Lessons 1-2 (Architecture fundamentals)
- Day 2: Lessons 3-4 (Modules and state machines)
- Day 3: Lessons 5-6 (Cloud and customization)

**Hybrid:**
- Pre-work: Lesson 1 (setup and provisioning)
- Workshop: Lessons 2-5 (architecture and implementation)
- Post-work: Lesson 6 (customization project)

### Assessment

**Formative Assessment:**
- Exercise completion and verification
- Code review by instructors or peers
- Discussion of design decisions

**Summative Assessment:**
- Final project: Build a custom IoT application
- Criteria:
  - Adds at least one custom sensor
  - Implements custom business logic
  - Successfully communicates with nRF Cloud
  - Demonstrates power optimization
  - Production-ready code quality

## Integration with Other Courses

### Prerequisites Courses

- **Cellular IoT Fundamentals** - Provides cellular background
- **nRF Connect SDK Fundamentals** - Covers SDK basics

### Follow-up Courses

- **nRF Connect SDK Intermediate** - Advanced SDK features
- **Power Optimization** - Deep dive into power management
- **Production Deployment** - Manufacturing and field deployment

### Complementary Courses

- **Bluetooth Low Energy Fundamentals** - For multi-radio devices
- **Matter Fundamentals** - For smart home applications
- **Thread Fundamentals** - For mesh networking

## Additional Resources

### Documentation

- [Asset Tracker Template Documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)
- [nRF Connect SDK Documentation](https://docs.nordicsemi.com/bundle/ncs-latest/)
- [Zephyr RTOS Documentation](https://docs.zephyrproject.org/latest/)
- [nRF Cloud Documentation](https://docs.nordicsemi.com/bundle/nrf-cloud/)

### Community

- [Nordic DevZone](https://devzone.nordicsemi.com/)
- [Asset Tracker Template GitHub](https://github.com/nrfconnect/Asset-Tracker-Template)
- [nRF Connect SDK GitHub](https://github.com/nrfconnect/sdk-nrf)

### Tools

- [nRF Connect for Desktop](https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop)
- [nRF Connect for VS Code](https://marketplace.visualstudio.com/items?itemName=nordic-semiconductor.nrf-connect)
- [nRF Cloud](https://nrfcloud.com/)

## Course Maintenance

### Version Information

- **Course Version:** 1.0
- **Asset Tracker Template Version:** Compatible with main branch
- **nRF Connect SDK Version:** v3.1.0 or later
- **Last Updated:** November 2025

### Maintenance Schedule

- **Quarterly reviews** - Check for SDK updates and deprecated features
- **Annual updates** - Major revisions for new SDK versions
- **Continuous improvements** - Based on student feedback

### Feedback

We welcome feedback to improve this course:
- Report issues on the Asset Tracker Template GitHub repository
- Discuss on Nordic DevZone
- Contribute improvements via pull requests

## Credits

This course was developed to support the Nordic Developer Academy and the Asset Tracker Template project.

### Contributors

- Nordic Semiconductor documentation team
- Asset Tracker Template development team
- DevAcademy course designers
- Community contributors

### Acknowledgments

Special thanks to:
- The Zephyr Project for the excellent RTOS
- The nRF Connect SDK team for the comprehensive SDK
- The Nordic community for continuous feedback and support

## License

This course content follows the same license as the Asset Tracker Template project.
See [LICENSE](../LICENSE) for details.

## Next Steps

### For Students

1. **Complete the course** - Work through all lessons and exercises
2. **Build a project** - Create your own IoT application
3. **Share your work** - Post on DevZone, contribute to the repository
4. **Help others** - Answer questions, share your experience

### For Instructors

1. **Review the course** - Familiarize yourself with all materials
2. **Set up environment** - Ensure all tools and hardware are available
3. **Customize** - Adapt to your specific audience and time constraints
4. **Gather feedback** - Collect student feedback for improvements

### For Developers

1. **Explore the template** - Dive deeper into specific modules
2. **Contribute** - Submit improvements and new features
3. **Extend** - Build upon the template for your products
4. **Document** - Share your use cases and solutions

## Course Status

✅ **Complete** - All lessons and exercises created
✅ **Ready for Delivery** - Can be taught immediately
✅ **Documentation Complete** - All materials included
✅ **Exercises Verified** - All exercises have clear instructions

## Summary

The Asset Tracker Template Developer Academy course provides comprehensive training on building production-ready cellular IoT applications with Nordic nRF91 Series devices. Through 6 detailed lessons and 12 hands-on exercises, students gain practical experience with:

- Modern IoT architecture patterns (modular, event-driven)
- Professional development tools and workflows
- Cloud connectivity and data management
- Power optimization for battery-powered devices
- Customization and extension for specific use cases

Students completing this course will be well-equipped to develop, customize, and deploy cellular IoT applications using the Asset Tracker Template.

**Thank you for using this course! We hope it provides value to your learning or teaching experience.**

---

For questions, support, or contributions:
- 📧 [Nordic DevZone](https://devzone.nordicsemi.com/)
- 💻 [GitHub Repository](https://github.com/nrfconnect/Asset-Tracker-Template)
- 📚 [Official Documentation](https://docs.nordicsemi.com/bundle/asset-tracker-template-latest/)

