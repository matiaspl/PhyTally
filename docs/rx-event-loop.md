# Receiver Event Loop

This diagram documents the common receiver runtime used by:

- [src/rx/main.cpp](../src/rx/main.cpp)
- [src/rx_esp32c6/main.cpp](../src/rx_esp32c6/main.cpp)
- [src/rx8266/main.cpp](../src/rx8266/main.cpp)
- [lib/ReceiverCommon/ReceiverCommon.h](../lib/ReceiverCommon/ReceiverCommon.h)

The radio callback and the main loop are intentionally split:

- the promiscuous callback only parses frames and queues lightweight state changes
- persistence, reboot, and LED-side effects happen from the main loop
- telemetry is suppressed while the receiver is stale and channel-scanning

## Flow

```mermaid
flowchart TD
    A[Boot] --> B[Load persisted tally ID and LED brightness]
    B --> C[Configure STA radio and low-bandwidth telemetry]
    C --> D[Enable promiscuous receive]
    D --> E[Set initial channel]
    E --> F[Enter main loop]

    subgraph RXCallback[Promiscuous RX callback]
        G[Management frame received]
        H{Vendor IE matches PhyTally hub?}
        I[Ignore frame]
        J[Update hub MAC, RSSI, lastPacketTime]
        K[Update current tally state from states[tallyId]]
        L{Targeted command for my MAC?}
        M[Queue pending ID apply]
        N[Queue pending brightness apply]
        O[Start identify timer]
        P[Return]

        G --> H
        H -- No --> I
        H -- Yes --> J
        J --> K
        K --> L
        L -- No --> P
        L -- SET_ID --> M
        L -- SET_BRIGHTNESS --> N
        L -- IDENTIFY --> O
        M --> P
        N --> P
        O --> P
    end

    subgraph MainLoop[Main loop]
        Q[Handle serial config]
        R[Apply pending brightness]
        S{Pending ID change?}
        T[Persist ID and reboot]
        U[Handle signal loss]
        V{Hub timed out?}
        W[Set state to ERROR]
        X[Advance channel scan]
        Y{Telemetry interval elapsed?}
        Z{Allowed to send telemetry?}
        AA[Build and send probe telemetry]
        AB[Render tally LED state]
        AC[Short delay]

        Q --> R
        R --> S
        S -- Yes --> T
        S -- No --> U
        U --> V
        V -- Yes --> W
        W --> X
        V -- No --> Y
        X --> Y
        Y -- No --> AB
        Y -- Yes --> Z
        Z -- No --> AB
        Z -- Yes --> AA
        AA --> AB
        AB --> AC
        AC --> Q
    end

    F --> Q
```

## Notes

- `SET_ID` is deferred to the main loop so NVS/EEPROM writes and reboot do not happen inside the Wi-Fi callback.
- `SET_BRIGHTNESS` is also deferred so persistence is handled from normal task context.
- `SET_ESPNOW_PHY` is deferred for the same reason: the receiver updates radio state from normal task context and then acknowledges through telemetry.
- `IDENTIFY` is immediate and time-bounded, because it only updates an in-memory timer.
- Signal loss is declared after about 2 seconds without a valid hub frame.
- In error/scan state the receiver does not send telemetry probes.
- LED rendering is board-specific, but the event-loop structure is shared.
- `HUB_CMD_REBOOT` (`cmd_type == 2`) exists in the shared protocol enum but is **not** handled in `ProcessHubFrame`; receivers currently apply `SET_ID`, `SET_BRIGHTNESS`, `IDENTIFY`, and `SET_ESPNOW_PHY`.
- Linux hub and the current embedded TX assign non-zero `cmd_id` values for OTA command tracking and expect `ack_command_id` in telemetry after apply.
