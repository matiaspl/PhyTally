# Receiver Board Porting Notes

This project now has three receiver implementations:

- [src/rx/main.cpp](../src/rx/main.cpp): classic ESP32 + external WS2812 on `GPIO 13`
- [src/rx8266/main.cpp](../src/rx8266/main.cpp): ESP8266 + single red tally LED on `GPIO 16`
- [src/rx_esp32c6/main.cpp](../src/rx_esp32c6/main.cpp): ESP32-C6 + built-in NeoPixel, default `GPIO 8`

The shared RX runtime is documented separately in [rx-event-loop.md](rx-event-loop.md).

When adapting to a new board, the radio/protocol behavior should stay the same. The parts that usually need board-specific changes are below.

## 1. Pick The Right Base

- Start from [src/rx/main.cpp](../src/rx/main.cpp) for ESP32-family Arduino boards with standard `esp_wifi` support.
- Start from [src/rx_esp32c6/main.cpp](../src/rx_esp32c6/main.cpp) for newer ESP32 variants that use built-in RGB LEDs or native USB CDC.
- Start from [src/rx8266/main.cpp](../src/rx8266/main.cpp) only for ESP8266, because the Wi-Fi APIs and persistence model differ.

## 2. Create A Dedicated PlatformIO Env

Add a new env in [platformio.ini](../platformio.ini) instead of overloading an existing one.

What usually changes:
- `platform`
- `board`
- `framework`
- `build_src_filter`
- optional `build_flags`

Typical examples:
- external LED board: define the LED pin in code
- built-in NeoPixel board: pass a board-specific pin in `build_flags`
- native USB board: enable `ARDUINO_USB_CDC_ON_BOOT=1`

For the ESP32-C6 Super Mini target, the built-in RGB LED pin is kept configurable:

```ini
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DPHYTALLY_NEOPIXEL_PIN=8
```

If your board uses a different NeoPixel pin, change only `PHYTALLY_NEOPIXEL_PIN`.

## 3. Keep The RF Behavior Intact

Do not change these unless you are intentionally redesigning the protocol:

- promiscuous receive of management frames
- beacon/probe-response parsing of the vendor IE
- low-bandwidth telemetry mode
- probe request telemetry uplink
- channel scan fallback when hub lock is lost

The receiver side should still:
- lock to the hub channel when valid frames arrive
- send telemetry every 2 seconds
- fall back to channel sweep when the hub disappears

## 4. LED Output Is The Main Board-Specific Part

The tally state model is the same on every board:

- `OFF`: no light
- `PREVIEW`: green blink
- `PROGRAM`: solid red
- `ERROR`: yellow or fast error blink

Implementation choices by board:

- external WS2812/NeoPixel: FastLED or `neopixelWrite`
- single-color LED: map states to `off`, `solid`, and blink patterns
- active-low LED: invert the write logic in one helper function only

Keep the board-specific LED handling behind helpers like:

- `setTallyLed(...)`
- `setActivityLed(...)`
- `showPixel(...)`

That keeps the packet and radio logic portable.

## 5. Serial Console Differences

Boards differ a lot here:

- older ESP32 boards often use UART serial over a USB bridge
- ESP32-C6 boards often expose native USB CDC
- ESP8266 boards usually behave like classic UART devices

The receiver firmware expects a simple serial config interface:

- `GET_INFO`
- `SET_ID=<n>`

If serial seems dead on a new board, check:
- native USB CDC boot flag
- selected upload/monitor port
- whether the board resets when the monitor opens

## 6. Persistence And Safe ID Apply

Changing tally ID from an OTA command should not reboot directly inside the promiscuous callback.

Preferred pattern:
- callback stores the pending ID
- main loop applies it
- then the device reboots

That pattern is already used in:
- [src/rx8266/main.cpp](../src/rx8266/main.cpp)
- [src/rx_esp32c6/main.cpp](../src/rx_esp32c6/main.cpp)

## 7. First Bring-Up Checklist

After adding a new board target:

1. Build the new env successfully.
2. Flash it and confirm the serial banner prints MAC and tally ID.
3. Verify `LOWBW PROBES: 11b @ 1 Mbps` appears.
4. Confirm the device enters scan mode until the hub is found.
5. Confirm `HUB LOCK` and `TELEM TX` appear in logs.
6. Verify the receiver appears in the Linux hub API.
7. Trigger an OTA tally ID change and confirm it survives reboot.

## 8. Common Failure Modes

- No LED output:
  wrong pin, wrong LED type, or active-low logic not accounted for
- Build works but serial is silent:
  USB CDC not enabled for native-USB boards
- Receives beacons but never shows in hub API:
  telemetry TX path or low-bandwidth rate setup is wrong
- Device only works on one channel:
  channel sweep logic was removed or channel setting API differs
- OTA ID change crashes:
  reboot or NVS write is happening inside the promiscuous callback
