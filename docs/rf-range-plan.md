# ESP32 RF Range Follow-Up

The ESP32 beacon/probe path is being deferred while the project pivots to a
VM-hosted Linux hub.

## Observed Problem

- ESP32 TX to ESP32 RX range is only a few meters in the current setup.

## Working Hypotheses

1. TX power is left at the platform default instead of being forced to the
   highest supported level.
2. The TX runs `AP+STA` and is pinned to the infrastructure Wi-Fi channel,
   which inherits local channel congestion.
3. Receiver behavior is fully passive promiscuous sniffing, so there are no
   ACKs or retransmits to recover weak links.
4. Board-level antenna, power, cable noise, or enclosure effects may be
   dominating the RF performance.

## Deferred Validation Steps

1. Force max TX power on both TX and RX test builds.
2. Compare range in configuration mode versus live `AP+STA` mode.
3. Test fixed clean channels `1`, `6`, and `11`.
4. Record RSSI at the RX serial console while increasing separation distance.
5. Repeat with an external-antenna ESP32 module to separate firmware and RF
   hardware effects.

## Current Direction

- Move the hub to Linux/VM infrastructure first.
- Keep the ESP32 RX receiver protocol unchanged so the Linux hub can be
  validated against existing hardware.
