# ESP-NOW Prototype

This prototype isolates the ESP-NOW air path from the beacon/probe route so it
can be measured on its own.

## Builds

- TX: `pio run -e tally_tx_esp32s3_espnow_proto`
- RX (ESP32-S3): `pio run -e tally_rx_esp32s3_espnow_proto`
- RX (ESP32-C6): `pio run -e tally_rx_esp32c6_espnow_proto`

These builds define `PHYTALLY_FORCE_ESPNOW=1`, which:

- starts the runtime on ESP-NOW immediately
- disables beacon/probe receive processing on receivers
- keeps hub payload and telemetry semantics identical to the dual-stack path

## Packet Contract

The prototype packet contract lives in `lib/TallyTransport/TallyTransport.h`.

### Hub To Receiver

- Message type: `PHYTALLY_ESPNOW_MSG_HUB_STATE`
- Header:
  - `magic`
  - `version`
  - `message_type`
  - `sequence`
  - `payload_len`
- Payload: `HubPayload`

### Receiver To Hub

- Message type: `PHYTALLY_ESPNOW_MSG_TELEMETRY`
- Header:
  - `magic`
  - `version`
  - `message_type`
  - `sequence`
  - `payload_len`
- Payload: `TelemetryPayload`

## Behavior

- `HubPayload` still carries tally states plus targeted OTA commands.
- `TelemetryPayload.ack_command_id` is still the command completion signal.
- Sequence numbers are used for simple duplicate suppression on the receiver side.
- The prototype intentionally preserves the existing tally and command semantics so
  comparisons stay focused on transport behavior rather than application changes.
