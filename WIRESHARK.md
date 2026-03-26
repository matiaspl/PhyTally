# PhyTally Wireshark Verification Guide

Use this guide to confirm **hub → receiver** vendor IE payloads and optionally **receiver → hub** telemetry on the air.

## 1. Capture setup

1. Put a Wi-Fi interface into **monitor mode** on the **same 2.4 GHz channel** as the PhyTally hub (default **channel 1** unless configured otherwise).
2. Start the capture.

On macOS, if the built-in interface shows `0 packets captured`, the hub may still be transmitting; try another adapter or the fallback checks in [section 6](#6-fallback-checks).

## 2. Which packets to look for

### Legacy ESP32 hub (`tally_tx`)

The ESP32 hub advertises a normal AP with SSID **`PhyTally`** (or **`PhyTally-Config`** in setup mode). Beacons are easy to filter:

```
wlan.mgt.subtype == 8 && wlan.ssid == "PhyTally"
```

Config / captive portal:

```
wlan.mgt.subtype == 8 && wlan.ssid == "PhyTally-Config"
```

The same vendor IE is also attached to **probe responses** from that AP.

### Linux hub (`phytally-hub`)

- **`hostapd` backend:** You still get a managed-mode AP with an SSID from your hostapd config; filter on that SSID and subtype beacon (8), or filter on the **BSSID** you configured.
- **`rawinject` backend:** Injected beacons may **not** match the legacy `PhyTally` SSID filter. Prefer filtering on the **known BSSID** from hub status / config, or on the presence of vendor IE data with OUI `11:22:33`:

```
wlan.tag.vendor.oui == 11:22:33
```

Telemetry from receivers is **probe requests** (subtype 4) carrying a second vendor OUI **`11:22:44`**.

## 3. Hub vendor IE (tag 221 / 0xDD)

In a **beacon** or **probe response**:

- **Tag:** Vendor Specific (221)
- **OUI:** `11:22:33`
- **Next byte:** `0x01` (protocol version; receivers require this match)

The **vendor-specific content** that follows is the packed hub tally + command block (same layout as `HubPayload` in `lib/TallyProtocol/TallyProtocol.h`):

| Offset (after OUI + version) | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0–15 | Tally states | 16 | Index `0..15` = tally IDs **1..16**. `0` off, `1` preview, `2` program, `3` error. |
| 16–21 | Target MAC | 6 | OTA command destination (`00:00:00:00:00:00` when idle). |
| 22 | Cmd type | 1 | `0` none, `1` SET_ID, `2` REBOOT (reserved; not handled on RX), `3` SET_BRIGHTNESS, `4` IDENTIFY. |
| 23 | Cmd param | 1 | For SET_ID: new tally index `0..15`. For SET_BRIGHTNESS: brightness byte. |
| 24 | Cmd id | 1 | Non-zero sequence for dedup on Linux hub; legacy ESP32 hub often leaves `0`. |

The **802.11 IE length** field (the byte after `0xDD`) is the length of the **vendor payload** starting at the OUI; for the full struct it is **29** bytes (`sizeof(HubPayload)`).

## 4. Telemetry vendor IE (probe request)

Receivers send **probe requests** (`wlan.fc.type_subtype == 4`) with vendor IE:

- **OUI:** `11:22:44`
- **Body:** `TelemetryPayload`: protocol `0x01`, `tally_id`, `battery_percent`, `rssi`, `status_flags` (LE 16-bit), `led_brightness`, `ack_command_id`.

Firmware currently uses placeholder **`battery_percent = 100`** and **`status_flags = 0`**.

## 5. Automated check (tshark)

Example: dump vendor data for frames from the hub BSSID:

```bash
tshark -i <interface> -I -Y 'wlan.addr == <HUB_BSSID>' -T fields -e wlan.tag.vendor.data
```

Replace `<interface>` and `<HUB_BSSID>`. Decode is easier in the Wireshark GUI by expanding the vendor IE tree.

## 6. Fallback checks

If monitor capture is unreliable:

- **Linux hub:** open `http://<hub-host>/` and use status / telemetry counts from the API (`GET /api/v1/status`).
- **Legacy ESP32 hub:** same via its IP (often `192.168.4.1` in config mode); serial logs may show `Hub AP Started`, `TELEM RX`.
- **Receiver:** serial logs such as hub lock, telemetry transmit, and signal loss while scanning.

Together these confirm decode of hub vendor IEs and probe-back telemetry without relying only on Wireshark.
