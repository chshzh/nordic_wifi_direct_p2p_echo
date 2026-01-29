# Nordic Wi-Fi Direct P2P Echo

![Nordic Semiconductor](https://img.shields.io/badge/Nordic%20Semiconductor-nRF7002-blue)
![NCS Version](https://img.shields.io/badge/NCS-main%20branch-green)
![Platform](https://img.shields.io/badge/Platform-nRF54LM20%20DK%20%2B%20nRF7002EK2-orange)
![License](https://img.shields.io/badge/License-LicenseRef--Nordic--5--Clause-lightgrey)

> **A simple Wi-Fi Direct (P2P) connection demo with UDP echo loopback for Nordic nRF54LM20 DK with nRF7002EB2 Wi-Fi shield**

## 🔍 Overview

This application demonstrates Wi-Fi Direct (P2P) peer-to-peer connection between two Nordic development kits. Wi-Fi Direct allows two devices to connect directly without requiring a traditional access point or router.

After establishing a P2P connection:
- **Group Owner (GO)** runs a UDP echo server
- **Client (CLI)** sends UDP packets to GO and receives echo responses with RTT measurement

### 🎯 Key Features

- **📡 Wi-Fi Direct P2P**: Direct device-to-device connection without infrastructure
- **🔘 Button-Triggered Pairing**: Press BUTTON 0 on both devices simultaneously to start pairing
- **👑 Role Negotiation**: Automatic GO (Group Owner) / Client role assignment based on intent
- **🌐 DHCP Server**: Group Owner automatically provides IP addresses to clients
- **📨 UDP Echo Demo**: Client continuously sends packets to GO, measures round-trip time
- **💡 LED Status Indicators**: Visual feedback for connection status and device role

## 📋 Wi-Fi Direct Concepts

### Device Roles

| Role | Description |
|------|-------------|
| **Group Owner (GO)** | Acts like an access point, assigns IP addresses via DHCP |
| **Client (CLI)** | Connects to the GO, receives IP address via DHCP |

### GO Intent

The GO Intent value (0-15) determines which device becomes the Group Owner:
- **15**: Always become GO
- **0**: Always become Client
- **1-14**: Role negotiated - higher value = more likely to be GO

## 🔧 Hardware Requirements

| Component | Specification | Quantity |
|-----------|---------------|----------|
| **Development Board** | nRF54LM20 DK | 2 |
| **Wi-Fi Shield** | nRF7002 EK2 | 2 |
| **NCS Version** | main branch (with P2P firmware support) | - |
| **USB Cable** | Type-C or Micro USB | 2 |

## 🏗️ Project Architecture

```
nordic_wifi_direct_p2p_echo/
├── src/
│   ├── main.c                 # Main application with button handling, P2P and UDP echo
│   ├── wifi_p2p_utils.c/.h    # Wi-Fi P2P API (find, connect, group management)
│   ├── net_utils.c/.h         # Network utilities (DHCP server, IP configuration)
│   └── udp_utils.c/.h         # UDP echo client/server with RTT measurement
├── boards/
│   └── nrf54lm20dk_nrf54lm20a_cpuapp.conf   # Board-specific config
├── CMakeLists.txt             # Build configuration
├── Kconfig                    # Configuration options
├── prj.conf                   # Main project configuration with P2P support
├── sysbuild.conf              # Sysbuild for nRF70 Wi-Fi
├── overlay-p2p-go.conf            # Overlay for GO role (GO intent: 15)
├── overlay-p2p-cli.conf        # Overlay for Client role (GO intent: 0)
├── west.yml                   # West manifest
├── LICENSE                    # Nordic 5-Clause License
└── README.md                  # This file
```

### Core Modules

- **`main.c`**: Coordinates P2P operations, handles button input, manages LED indicators and UDP echo
- **`wifi_p2p_utils`**: Provides P2P APIs (discovery, connection, group management)
- **`net_utils`**: Network configuration for GO role (IP setup, DHCP server)
- **`udp_utils`**: UDP echo client/server implementation with RTT measurement

## 🚀 Quick Start Guide

### 1. Prerequisites

Ensure you have the Nordic Connect SDK environment configured:
- **nRF Connect SDK main branch** (with P2P firmware support)
  - In VS Code, navigate to: **nRF Connect -> Manage SDKs -> Third-party Git repository**
  - Repository URL: `https://github.com/nrfconnect/sdk-nrf`
  - Select the `main` branch
- nRF54LM20 DK x2 with nRF7002 EK2 shields
- nRF Command Line Tools

### 2. Build Instructions

Navigate to the project directory and build:

```bash
cd /opt/nordic/ncs/myApps/nordic_wifi_direct_p2p_echo
```

#### Build GO Device (Device 1 - Group Owner):
```bash
west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -- \
    -DSHIELD=nrf7002eb2 \
    -DSNIPPET=wifi-p2p \
    -DEXTRA_CONF_FILE=overlay-p2p-go.conf
```

#### Build Client Device (Device 2):
```bash
west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp -- \
    -DSHIELD=nrf7002eb2 \
    -DSNIPPET=wifi-p2p \
    -DEXTRA_CONF_FILE=overlay-p2p-cli.conf
```

#### Build Options

| Option | Description |
|--------|-------------|
| `-DSHIELD=nrf7002eb2` | Use nRF7002 EB2 Wi-Fi shield |
| `-DSNIPPET=wifi-p2p` | Enable Wi-Fi P2P support snippet |
| `-DEXTRA_CONF_FILE=overlay-p2p-go.conf` | Use GO configuration (intent=15) |
| `-DEXTRA_CONF_FILE=overlay-p2p-cli.conf` | Use Client configuration (intent=0) |


### 3. Flash and Deploy

Build and flash each device:

```bash
# Flash first device
west flash
```

## 🎮 Operation Guide

### LED Indicators

| LED | Status | Description |
|-----|--------|-------------|
| **LED0** | On | P2P Discovery in progress |
| **LED1** | On | P2P Connection established |
| **LED2** | On | Device is Group Owner (GO) + UDP Echo Server running |
| **LED3** | On | Device is Client (CLI) + UDP Echo Client running |

### Hardware Controls

| Button | Function |
|--------|----------|
| **BUTTON 0** | Start P2P pairing / Print UDP Echo statistics (when connected) |
| **BUTTON 1** | Stop UDP Echo test |

### Connection Procedure

```
┌─────────────────┐                    ┌─────────────────┐
│   Board 1       │                    │   Board 2       │
│ (GO Intent: 15) │                    │ (GO Intent: 0)  │
└────────┬────────┘                    └────────┬────────┘
         │                                      │
    [Press BTN0]                          [Press BTN0]
         │                                      │
         ▼                                      ▼
    P2P Find ─────────────────────────────► P2P Find
         │                                      │
         ▼                                      ▼
    Device Found ◄────────────────────────► Device Found
         │                                      │
         ▼                                      ▼
    P2P Connect ◄─────────────────────────► P2P Connect
         │                                      │
         ▼                                      ▼
    ┌────────────┐                    ┌────────────┐
    │ Become GO  │                    │ Become CLI │
    └─────┬──────┘                    └─────┬──────┘
          │                                 │
    Setup DHCP ──────────────────────► Get IP via DHCP
          │                                 │
          ▼                                 ▼
    ┌─────────────────────────────────────────────┐
    │         P2P Connection Established!         │
    │                                             │
    │  GO IP:  192.168.88.1                       │
    │  CLI IP: 192.168.88.10 (from DHCP)          │
    └─────────────────────────────────────────────┘
          │                                 │
          ▼                                 ▼
    Start Echo Server ◄─────────────► Start Echo Client
          │                                 │
          │     UDP Packets (port 5001)     │
          │◄────────────────────────────────│
          │                                 │
          │     Echo Response               │
          │────────────────────────────────►│
          │                                 │
          ▼                                 ▼
    ┌─────────────────────────────────────────────┐
    │       UDP Echo Loopback Running!            │
    │                                             │
    │  Client sends packets every 1000ms          │
    │  GO echoes packets back                     │
    │  Client measures RTT (round-trip time)      │
    └─────────────────────────────────────────────┘
```

### Step-by-Step Test Procedure

1. **Power on both devices**
   - Connect USB cables to both nRF54LM20 DK boards
   - Open serial terminal for each device (115200 baud)

2. **Wait for Wi-Fi ready**
   - Both devices will show "Wi-Fi is ready!" message
   - All LEDs should be off initially

3. **Start P2P pairing**
   - Press BUTTON 0 on **both devices simultaneously** (within ~5 seconds)
   - LED0 will turn on on both devices

4. **Observe role assignment**
   - Device with higher GO intent (15) becomes GO → LED2 lights up
   - Device with lower GO intent (0) becomes Client → LED3 lights up

5. **Connection established**
   - LED1 lights up on both devices when connected
   - GO shows DHCP server status
   - Client shows assigned IP address

6. **UDP Echo starts automatically**
   - GO starts UDP echo server on port 5001
   - Client starts sending UDP packets to GO
   - Console shows RTT measurements

7. **Monitor and control**
   - Press BUTTON 0 to print UDP echo statistics
   - Press BUTTON 1 to stop UDP echo test

## ⚙️ Configuration Options

### Kconfig Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `CONFIG_P2P_TARGET_PEER_MAC` | "" | Target peer MAC address filter (format: "xx:xx:xx:xx:xx:xx") |
| `CONFIG_P2P_GO_INTENT` | 15 | GO intent value (0-15) |
| `CONFIG_P2P_DISCOVERY_TIMEOUT` | 30 | Discovery timeout in seconds |
| `CONFIG_P2P_DISCOVERY_WAIT_MS` | 10000 | Time to wait for peer discovery (ms) |
| `CONFIG_P2P_GROUP_FORMATION_TIMEOUT_MS` | 30000 | Timeout for group formation (ms) |
| `CONFIG_P2P_DHCP_TIMEOUT_MS` | 10000 | Timeout for DHCP IP assignment (ms) |
| `CONFIG_P2P_DHCP_START_DELAY_MS` | 5000 | Delay before starting DHCP client (ms) |
| `CONFIG_P2P_CLIENT_CONNECT_DELAY_MS` | 2000 | Delay before starting UDP client (ms) |
| `CONFIG_P2P_OPERATING_CHANNEL` | 11 | Preferred Wi-Fi channel |
| `CONFIG_P2P_OPERATING_FREQUENCY` | 2462 | Preferred frequency in MHz |
| `CONFIG_P2P_GO_IP_ADDRESS` | "192.168.88.1" | GO IP address |
| `CONFIG_P2P_DHCP_SERVER_POOL_START` | "192.168.88.10" | DHCP pool start address |
| `CONFIG_UDP_ECHO_PORT` | 5001 | UDP echo server/client port |
| `CONFIG_UDP_ECHO_INTERVAL_MS` | 1000 | Interval between UDP packets (ms) |
| `CONFIG_UDP_ECHO_PACKET_SIZE` | 64 | Size of UDP packets (bytes) |
| `CONFIG_UDP_ECHO_COUNT` | 100 | Number of packets (0 = infinite) |

### Two-Device Configuration

For reliable pairing, configure different GO intents on each device:

**Device 1 (Always GO):**
```
CONFIG_P2P_TARGET_PEER_MAC="f4:ce:36:00:ae:ec"  # Replace with your CLI's MAC
CONFIG_P2P_GO_INTENT=15
```

**Device 2 (Always Client):**
```
CONFIG_P2P_TARGET_PEER_MAC="f4:ce:36:00:af:12"  # Replace with your GO's MAC
CONFIG_P2P_GO_INTENT=0
```

### Peer MAC Address Filtering

When multiple P2P devices are nearby, you can filter which peer to connect to using `CONFIG_P2P_TARGET_PEER_MAC`:

- **Empty string** (default): Connect to the peer with highest RSSI
- **MAC address string**: Only connect to the peer with the exact MAC address

This is useful in environments with multiple P2P devices to ensure your client connects to the correct GO.

**How to find your GO's MAC address:**
1. Build and flash the GO device
2. Press BUTTON 0 to start P2P discovery
3. Look for the MAC address in the client's terminal output:
   ```
   P2P Device Found:
     MAC: f4:ce:36:00:af:12
   ```
4. Use this MAC address in the overlay configuration


## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/chshzh/nordic_wifi_direct_p2p_echo/issues)
- **Nordic DevZone**: [devzone.nordicsemi.com](https://devzone.nordicsemi.com/)

## 📝 License

Copyright (c) 2026 Nordic Semiconductor ASA

[SPDX-License-Identifier: LicenseRef-Nordic-5-Clause](LICENSE)

---

**⭐ If this project helps you, please consider giving it a star!**
