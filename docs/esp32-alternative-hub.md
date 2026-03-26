# ESP32 Alternative Hub

This document covers the maintained ESP32/ESP32-S3 alternative hub firmware built
from `src/tx/`.

## Goal

The alternative hub is a self-contained PhyTally runtime that:

- ingests switcher state locally on the MCU
- serves configuration over HTTP
- advertises an mDNS hostname in the form `phytally-hub-<mac6>.local`
- drives one active RF route at a time
- supports a configurable priority order between beacon/probe and ESP-NOW

`phytally-hub/` remains the primary Linux-hosted deployment path. This firmware is
an add-on route for embedded-only deployments and bench/portable rigs.

## Supported Builds

- `pio run -e tally_tx` for the original ESP32 build
- `pio run -e tally_tx_esp32s3` for the preferred ESP32-S3 build
- `pio run -e tally_tx_esp32s3_espnow_proto` for the ESP-NOW-only prototype build

Receiver-side prototype builds are also available:

- `pio run -e tally_rx_esp32s3_espnow_proto`
- `pio run -e tally_rx_esp32c6_espnow_proto`

## Current Scope

The alternative hub currently implements:

- USB-NCM switcher backhaul on the ESP32-S3 build
- ATEM tally ingest
- vMix tally ingest
- TSL 3.1 UDP tally ingest
- OBS tally ingest over `obs-websocket` v5 using `ws://`
- simulation mode
- OTA `SET_ID`, `SET_BRIGHTNESS`, and `IDENTIFY` commands with command-id ack tracking
- OTA `SET_ESPNOW_PHY` command flow with receiver ack tracking
- receiver telemetry over beacon/probe or ESP-NOW

## RF Route Policy

- One route is active at a time.
- Default route priority is beacon/probe first.
- ESP-NOW acts as fallback when beacon/probe telemetry goes stale.
- Route priority can be inverted in config to prefer ESP-NOW first.
- Prototype builds force ESP-NOW and disable beacon/probe processing.

## Wireless Transport Modes

The embedded TX exposes the following transport modes through the UI and API:

- `auto`: Prefer the configured route priority and fail over to the other path when telemetry on the active path goes stale.
- `beacon_probe`: Force classic PhyTally management-frame airtime only.
- `espnow`: Force ESP-NOW only.

Practical tradeoffs:

- `auto`: Best default for normal use. It improves resilience, but it is not ideal for debugging because the active path may switch while you are testing.
- `beacon_probe`: Best when you want classic PhyTally behavior, compatibility with the existing beacon/probe receiver path, and results that line up naturally with Wi-Fi survey observations. It is still subject to ordinary 2.4 GHz management-frame congestion and updates more slowly on the embedded TX than on the Linux hub.
- `espnow`: Best when you want a dedicated ESP-NOW air path or you are preparing for LR-oriented work. It is less representative of the original beacon/probe wire format, and Wi-Fi survey data is only an indirect indicator of ESP-NOW conditions.

Notes:

- Only one route is active at a time even when `auto` is selected.
- Wi-Fi survey can temporarily disturb the active path because the radio is shared.
- For repeatable RF testing, force `beacon_probe` or `espnow` instead of using `auto`.

## ESP-NOW PHY Modes

When the active path is `espnow`, the embedded TX also exposes `espnow_phy_mode` through `/api/v1/config`, `/api/v1/status`, and `POST /api/v1/wireless/espnow/phy`.

Available modes:

- `lr_250k`: maximum range bias, lowest throughput, highest airtime cost.
- `lr_500k`: LR-capable but faster than `lr_250k`; a good first LR setting.
- `11b_1m`: the default non-LR baseline; robust and predictable.
- `11b_2m`: a moderate step up from `11b_1m` while staying in the 802.11b family.
- `11b_11m`: highest-rate 802.11b option; best when you want more throughput and have link margin.
- `11g_6m`: low-end OFDM option; useful when you want to compare against an OFDM PHY instead of DSSS/CCK or LR.

Important limitation:

- The embedded TX stages receiver PHY changes over OTA before switching its own local ESP-NOW PHY.
- Receiver telemetry does not yet report current PHY through a dedicated status field; use RX serial logs when you need to confirm the applied receiver mode.

## HTTP Configuration

The firmware exposes the familiar `/api/v1` JSON surface with additional fields for:

- `hostname`
- `route_priority`
- `tsl_enabled`
- `tsl_listen_port`
- `wireless_transport`
- `espnow_phy_mode`
- `obs_url`
- `obs_password`
- `obs_enabled`

Command endpoints:

- `POST /api/v1/receivers/assign-id`
- `POST /api/v1/receivers/set-brightness`
- `POST /api/v1/receivers/identify`
- `POST /api/v1/wireless/transport`
- `POST /api/v1/wireless/espnow/phy`

Current API limitations compared to the Linux hub:

- no `GET /api/v1/events` SSE stream; the shared dashboard polls instead
- no `POST /api/v1/tallies/set`
- no per-receiver current PHY inventory yet
- OBS is `ws://` only on the embedded TX; `wss://` is not implemented

## USB / NCM Notes

The `tally_tx_esp32s3` build now uses TinyUSB CDC-NCM as its switcher-side
uplink:

- the board presents a USB-NCM network interface to the host and serves the UI
  and API at `http://192.168.7.1/`
- the board runs the USB-side DHCP service for that point-to-point link; hosts
  that fail to pick it up can be configured manually as `192.168.7.2/24`
- ATEM, vMix, TSL, and OBS traffic ride the USB link rather than a Wi-Fi STA uplink
- the 2.4 GHz radio stays dedicated to PhyTally airtime and optional Wi-Fi
  surveys
- on the ESP32-S3 build, USB-NCM is the intended management plane

The USB-NCM status surface under `/api/v1/status` exposes staged bring-up fields
that are intended to be checked in order:

- `usb_init_started`
- `usb_begin_ok`
- `usb_descriptor_loaded`
- `usb_interface_enabled`
- `usb_netif_added`
- `usb_mounted`
- `usb_link_up`
- `usb_dhcp_started`
- `uplink_ready`
- `usb_ip`, `usb_gateway`, `usb_netmask`, `usb_hostname`, `usb_mac`
- `usb_last_event`, `usb_last_event_ms_ago`, `usb_last_dhcp_change_ms_ago`
- `usb_last_error`

The original `tally_tx` build for classic ESP32 boards still retains the older
Wi-Fi STA backhaul because those targets do not expose the S3 USB-OTG path.

## USB-NCM Debug Flow

When USB-NCM is not reachable, treat it as a staged bring-up problem rather than
starting with mDNS.

1. Confirm the firmware reaches USB init over the debug UART.
Expected log sequence:
- `USB-NCM init: ...`
- `USB-NCM interface enabled; waiting for descriptor callback`
- `USB-NCM descriptor ready: ...`
- `USB-NCM BACKHAUL INITIALIZED: ...`

2. Check whether the host ever enumerates the native USB device.
Expected host evidence:
- a new USB device appears in macOS System Information / `system_profiler SPUSBDataType`
- a new USB Ethernet or NCM-style interface appears in `ifconfig` / `networksetup -listallhardwareports`

3. Use the staged status fields to classify the failure.
- If `usb_mounted` never becomes true, the failure is before DHCP. Check the S3 native USB port, cable quality, and host-side enumeration first.
- If `usb_mounted` and `usb_link_up` are true but `uplink_ready` is false, the USB data path is alive and the problem is usually host DHCP or Internet Sharing.
- If `uplink_ready` is true but `phytally-hub-<mac6>.local` does not resolve, treat it as an mDNS issue rather than a USB-NCM issue.

4. Validate HTTP by IP before relying on mDNS.
- Once `uplink_ready` is true, use `usb_ip` from `/api/v1/status`.
- Only after HTTP by IP works should `.local` be treated as the next check.

5. Use the debug UART to separate firmware init from host enumeration.
- If the board prints `USB-NCM awaiting host link and DHCP` but the Mac never shows a new USB device or network interface, the firmware stack initialized and the failure is on the physical connection or host USB-enumeration path.
