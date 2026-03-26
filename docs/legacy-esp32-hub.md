# Legacy ESP32 Hub

This document covers the older ESP32 hub transmitter in `src/tx/` and the
matching filesystem assets in `data/`.

Status: reference-only notes for the older captive-portal-first behavior.

Use the Linux hub in `phytally-hub/` for the primary maintained path, or see
`docs/esp32-alternative-hub.md` for the maintained embedded alternative route.

## Legacy Hub Hardware

- Device: ESP32 development board such as ESP32-WROOM-32
- Power: USB 5 V
- Network: production Wi-Fi uplink to the same network as the ATEM, vMix, or
  OBS host

## Build And Flash

```bash
pio run -e tally_tx -t upload
pio run -e tally_tx -t uploadfs
```

## First-Time Configuration

1. Power on the ESP32 hub.
2. On first boot it creates `PhyTally-Config`.
3. Connect a laptop or phone to that network.
4. Open `http://192.168.4.1/` if the captive portal does not appear.
5. Enter the production Wi-Fi credentials.
6. Enter the switcher endpoint and enable the matching integration.
7. Save and reboot.

## Notes

- This path is no longer the primary target for feature work.
- Dashboard/UI changes may land on the Linux hub first and never be mirrored
  here.
- RF behavior investigations for the ESP32 hub are tracked separately in
  `docs/rf-range-plan.md`.
