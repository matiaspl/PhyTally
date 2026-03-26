# TODO

## ESP TX Feature Gaps

- Add `GET /api/v1/events` on the ESP TX so the shared dashboard can use SSE instead of polling fallback.
- Make ATEM, vMix, and TSL connection state real on the ESP TX, including accurate `connected` and `last_error` reporting in status/API payloads.
- Add per-receiver PHY visibility to the hub API instead of relying on ACK flow and RX serial logs to confirm ESP-NOW PHY changes.
- Replace placeholder receiver telemetry fields with real values where possible, especially `battery_percent` and `status_flags`.
- Decide whether OBS `wss://` support is worth adding on the ESP TX; current implementation supports `ws://` only.

## Low-Priority Polish

- Add `/favicon.ico` for the embedded dashboard to remove the browser 404.
- Update docs to reflect that OBS is now implemented on the ESP TX over `ws://`.
