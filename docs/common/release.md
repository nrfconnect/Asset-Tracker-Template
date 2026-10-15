# Release artifacts

This page provides an overview of all firmware deliverables and artifacts that the Asset Tracker Template generates.
Each firmware variant is optimized for specific use cases and hardware platforms.

## Downloading releases

Pre-built firmware binaries and release notes are published on the [Asset Tracker Template releases](https://github.com/nrfconnect/Asset-Tracker-Template/releases) page on GitHub.

Pick the firmware that matches your hardware platform and the configuration you want from the following tables, download it from the release page, and flash it to your device with `west` or `nrfutil`.

## Firmware deliverables

The Asset Tracker Template generates multiple firmware artifacts for different hardware platforms and configurations. Each artifact serves specific development, testing, or production needs.

### Primary release artifacts

Each firmware variant includes the following set of artifacts:

| Artifact suffix | Description | Use case |
|---------------------|----------------|--------------|
| `-nrf91.hex` | Full merged image with b0, MCUboot, the signed application, and the provisioning data. Same contents as `build/merged.hex` from a local sysbuild after running `west ncs-mergehex --no-rebuild`. | Programming the entire internal flash with a J-Link debugger |
| `-nrf91.elf` | ELF file with debug symbols | Debugging, coredump analysis, `addr2line` |
| `-nrf91.config` | Build configuration snapshot | Inspecting the Kconfig options used for the build |
| `-nrf91-update-signed.hex` | Signed application-only hex without a bootloader | Updating only the application slot with a debugger |
| `-nrf91-update-signed.bin` | Signed application-only binary | FOTA updates using nRF Cloud |
| `-nrf91-dfu.zip` | DFU package | Serial DFU updates over USB, without a debugger |

#### Standard firmware variants

The following table lists the standard firmware variants:

| Artifact name | Hardware platform | Description | Use case |
|-------------------|----------------------|----------------|--------------|
| `asset-tracker-template-{VERSION}-thingy91x-nrf91.*` | Thingy:91 X (nRF9151) | Standard production firmware with all core features enabled, including Memfault coredump capture and fleet metrics through nRF Cloud CoAP | Production deployment on Thingy:91 X devices |
| `asset-tracker-template-{VERSION}-nrf9151dk-nrf91.*` | nRF9151 DK | Development kit firmware with the same feature set as the Thingy:91 X standard build | Development and testing on nRF9151 DK |

#### Debug variant

The debug artifact is the standard Thingy:91 X firmware with the modem-trace-to-Memfault overlay applied at build time. Memfault is already enabled in the standard build. The debug variant adds continuous modem trace capture to external flash and uploads those traces together with the coredump when the application crashes. This is not the same as the `-mtrace-` variants, which stream the modem trace live over UART 1 for bench-side analysis.

| Artifact name | Hardware platform | Description | Use case |
|-------------------|----------------------|----------------|--------------|
| `asset-tracker-template-{VERSION}-debug-thingy91x-nrf91.*` | Thingy:91 X (nRF9151) | Standard firmware plus modem trace capture to external flash, about 1 MB, and upload to Memfault on crash | Investigating crashes in deployed devices where you need the cellular context around the failure |

> [!CAUTION]
> The debug artifact is built with `CONFIG_NRF_CLOUD_COAP_SEC_TAG=2147483667` and `CONFIG_NRF_CLOUD_COAP_JWT_SEC_TAG=16842753`, so Nordic tools can decrypt its nRF Cloud DTLS traffic, as described in [Decrypting DTLS traffic in modem traces](tooling_troubleshooting.md#decrypting-dtls-traffic-in-modem-traces). Use it for debugging only, never for production.
> There is no equivalent nRF9151 DK artifact, because the DK ships with the developer security tag empty, so you must [provision the CoAP CA certificate](tooling_troubleshooting.md#provisioning-the-coap-ca-certificate-on-the-nrf9151-dk) by hand first.

#### Specialized configuration variants

The following table lists the specialized configuration variants:

| Artifact name | Hardware platform | Description | Use case |
|-------------------|----------------------|----------------|--------------|
| `asset-tracker-template-{VERSION}-mtrace-thingy91x-nrf91.*` | Thingy:91 X (nRF9151) | Firmware with modem trace output enabled through UART 1 | Cellular connectivity debugging and analysis |
| `asset-tracker-template-{VERSION}-mtrace-nrf9151dk-nrf91.*` | nRF9151 DK | Firmware with modem trace output enabled through UART 1  | Cellular connectivity debugging and analysis |
| `asset-tracker-template-{VERSION}-ext-gnss-nrf9151dk-nrf91.*` | nRF9151 DK | Firmware configured for external GNSS antenna | Testing with external GNSS antenna setup |
| `asset-tracker-template-{VERSION}-buffer-flash-thingy91x-nrf91.*` | Thingy:91 X (nRF9151) | LittleFS storage backend with buffered sampling, plus modem trace capture to external flash and upload to Memfault on crash | Testing persistent flash buffering with cellular context around crashes |

> [!CAUTION]
> Like the debug artifact, the buffer-flash artifact is built with `CONFIG_NRF_CLOUD_COAP_SEC_TAG=2147483667` and `CONFIG_NRF_CLOUD_COAP_JWT_SEC_TAG=16842753`, so Nordic tools can decrypt its nRF Cloud DTLS traffic, as described in [Decrypting DTLS traffic in modem traces](tooling_troubleshooting.md#decrypting-dtls-traffic-in-modem-traces). Use it for debugging only, never for production.

### Configuration overlays

The firmware variants are built using different configuration overlays that modify the base functionality:

| Overlay file | Purpose | Key features |
|------------------|-------------|------------------|
| `overlay-storage-minimal.conf` | Minimal storage configuration | Reduced memory footprint with storage for one sample and immediate sending |
| `overlay-modem-trace-over-uart.conf` and `overlay-modem-trace-shmem.overlay` | Modem tracing over UART1 | UART trace backend in Kconfig and 16 KiB `cpucell_cpuapp_ipc_shm_trace` in SRAM |
| `overlay-upload-modem-traces-to-memfault.conf` and `overlay-upload-modem-traces-to-memfault.overlay` | Modem trace to Memfault | Memfault modem trace upload, on-flash trace partition, and trace shmem, which is mutually exclusive with the UART overlay |

### Hardware platform support

The template supports two hardware platforms.

#### Thingy:91 X

The Thingy:91 X is the primary platform and has the following properties:

- **SoC**: nRF9151.
- **Features**: Full sensor suite, battery operation, and compact form factor.
- **Target use**: Production IoT deployments and prototyping.
- **Memory reports**: ROM and RAM usage reports generated for optimization.

#### nRF9151 DK

The nRF9151 DK has the following properties:

- **SoC**: nRF9151.
- **Features**: On-board J-Link debugger, external antenna connectors, and Arduino-compatible headers.
- **Target use**: Development, testing, and prototyping.
