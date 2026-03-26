#ifndef TALLY_PROTOCOL_H
#define TALLY_PROTOCOL_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stdint.h>
#endif

// Configuration
#define TALLY_WIFI_CHANNEL 1
#define MAX_TALLIES 16
#define BEACON_INTERVAL_MS 100

// Vendor Specific Information Element (IE) - Tag 221 (0xDD)
#define VENDOR_OUI_0 0x11
#define VENDOR_OUI_1 0x22
#define VENDOR_OUI_2 0x33

// Telemetry OUI
#define TELEM_OUI_0 0x11
#define TELEM_OUI_1 0x22
#define TELEM_OUI_2 0x44

// Tally State Definitions
enum TallyState : uint8_t {
    TALLY_OFF = 0,
    TALLY_PREVIEW = 1, // Green
    TALLY_PROGRAM = 2, // Red
    TALLY_ERROR = 3    // Flashing/Yellow
};

enum HubCommandType : uint8_t {
    HUB_CMD_NONE = 0,
    HUB_CMD_SET_ID = 1,
    HUB_CMD_REBOOT = 2,
    HUB_CMD_SET_BRIGHTNESS = 3,
    HUB_CMD_IDENTIFY = 4,
    HUB_CMD_SET_ESPNOW_PHY = 5
};

enum PhyTallyEspNowPhyMode : uint8_t {
    PHYTALLY_ESPNOW_PHY_LR_250K = 0,
    PHYTALLY_ESPNOW_PHY_LR_500K = 1,
    PHYTALLY_ESPNOW_PHY_11B_1M = 2,
    PHYTALLY_ESPNOW_PHY_11B_2M = 3,
    PHYTALLY_ESPNOW_PHY_11B_11M = 4,
    PHYTALLY_ESPNOW_PHY_11G_6M = 5
};

#define PHYTALLY_DEFAULT_LED_BRIGHTNESS 255
#define PHYTALLY_IDENTIFY_DURATION_MS 4000
#define PHYTALLY_ERROR_BLINK_PERIOD_MS 500
#define PHYTALLY_ERROR_BLINK_ON_MS 250

// Hub to Receiver Packet (Broadcast via Beacon)
struct HubPayload {
    uint8_t oui[3];
    uint8_t protocol_version;
    uint8_t states[MAX_TALLIES];
    // Command Section (targeted at a specific MAC)
    uint8_t target_mac[6];
    uint8_t cmd_type;
    uint8_t cmd_param;
    uint8_t cmd_id;
} __attribute__((packed));

// Receiver to Hub Packet (Unicast via Probe Request)
struct TelemetryPayload {
    uint8_t oui[3];
    uint8_t protocol_version;
    uint8_t tally_id;
    uint8_t battery_percent;
    int8_t  rssi;
    uint16_t status_flags;
    uint8_t led_brightness;
    uint8_t ack_command_id;
} __attribute__((packed));

#endif
