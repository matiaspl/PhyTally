# PhyTally Linux Hub

## Goal

Run the PhyTally hub on a Linux edge host (VM, Raspberry Pi, or similar) with:

- Ethernet (or stable management connectivity) for switcher / API access
- one **dedicated** 2.4 GHz radio for PhyTally beacon / probe airtime
- Linux radio backends instead of the legacy ESP32 Wi-Fi stack
- **Wire compatibility** with existing receiver firmware (`lib/TallyProtocol`, `lib/ReceiverCommon`)

The embedded ESP32/ESP32-S3 alternative hub in `src/tx/` remains a secondary path beside the Linux hub; see `docs/esp32-alternative-hub.md` for its current scope and limits.

## Why a Linux hub

- No ESP32 AP+STA channel coupling for production Wi-Fi uplink
- More CPU/RAM for telemetry parsing, logging, and the web UI
- Normal systemd packaging and config under `/etc/phytally/`
- Room for future multi-node designs (still design-only; see Phase 4 below)

## Architecture

The Go service in `phytally-hub/` is split into five layers:

1. **`internal/protocol`** — Go mirror of the C++ wire format: `HubPayload`, `TelemetryPayload`, OUIs, tally states, command types.

2. **`internal/hub`** — Tally state, receiver map, simulation, OTA command queue with monotonic `cmd_id`, telemetry merge, switcher goroutines, SSE subscribers.

3. **`internal/radio`** — `radio.Backend` implementations:
   - **`stub`** — no air I/O; API/dashboard testing
   - **`hostapd`** — managed AP + vendor IE updates via hostapd control; telemetry from a monitor interface
   - **`rawinject`** — monitor-mode injection of beacons and broadcast probe responses; optional radiotap rate control; ath9k spectral when available

4. **`internal/switcher`** — ATEM, vMix, OBS (obs-websocket v5), TSL 3.1.

5. **`internal/api`** — REST under `/api/v1`, **`GET /api/v1/events`** (SSE), and the **embedded** dashboard static files from `internal/api/web/` (compiled into the binary with `embed`).

## Execution plan (status)

### Phase 1: Linux service scaffold

- Go service with REST + dashboard + simulation.

**Status:** implemented.

### Phase 2: Radio proof of concept

- Fixed 2.4 GHz channel, PhyTally-compatible vendor IE on beacons (and probe responses where applicable), decode receiver probe telemetry.

**Status:** implemented (`hostapd` and `rawinject`).

### Phase 3: Feature parity and packaging

- ATEM / vMix / OBS / TSL, OTA `SET_ID` / `SET_BRIGHTNESS` / `IDENTIFY`, persistent config, systemd unit.

**Status:** implemented (`phytally-hub/deploy/systemd/`, `scripts/install-systemd.sh`).

### Phase 4: Multi-node range extension

- Controller + multiple edge radios, IP fan-out, roaming — **design only**.

## Deployment assumptions

- Dedicated 2.4 GHz NIC for PhyTally (do not share with normal client Wi-Fi in `rawinject` mode)
- Dashboard and API served by the Linux process (default listen per config)
- Receivers unchanged: promiscuous decode of beacons/probe responses with OUI `11:22:33`; probe telemetry with OUI `11:22:44`

## Current limits

- Spectral data feeds **survey** only
- Survey may **briefly pause** tally airtime while the radio is repurposed
- TSL mapping is intentionally simple: display address `0..15` → tally IDs `1..16`; control bit `0` = PROGRAM, bit `1` = PREVIEW

## On-hardware validation

When bringing up new hardware (e.g. Pi + USB Wi-Fi):

1. Confirm the NIC supports the chosen backend (`hostapd` vs `rawinject`) on your kernel.
2. Run `phytally-hub` with a known config channel and BSSID/SSID as applicable.
3. Verify an ESP32/ESP8266 receiver locks, updates LEDs from `states[]`, and returns telemetry with plausible RSSI / `ack_command_id` after OTA commands.

Further protocol reference: [README.md](../README.md) (Protocol Technical Details) and [WIRESHARK.md](../WIRESHARK.md).
