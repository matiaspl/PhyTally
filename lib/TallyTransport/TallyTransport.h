#ifndef TALLY_TRANSPORT_H
#define TALLY_TRANSPORT_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

#include "../TallyProtocol/TallyProtocol.h"

#define PHYTALLY_ESPNOW_MAGIC 0xA5
#define PHYTALLY_ESPNOW_VERSION 0x01
#define PHYTALLY_ROUTE_SWITCH_HOLDDOWN_MS 5000

enum PhyTallyTransportKind : uint8_t {
    PHYTALLY_TRANSPORT_BEACON_PROBE = 1,
    PHYTALLY_TRANSPORT_ESPNOW = 2,
};

enum PhyTallyRoutePriority : uint8_t {
    PHYTALLY_ROUTE_BEACON_FIRST = 0,
    PHYTALLY_ROUTE_ESPNOW_FIRST = 1,
};

enum PhyTallyEspNowMessageType : uint8_t {
    PHYTALLY_ESPNOW_MSG_HUB_STATE = 1,
    PHYTALLY_ESPNOW_MSG_TELEMETRY = 2,
};

struct PhyTallyEspNowHeader {
    uint8_t magic;
    uint8_t version;
    uint8_t message_type;
    uint8_t reserved;
    uint16_t sequence;
    uint16_t payload_len;
} __attribute__((packed));

struct PhyTallyEspNowHubPacket {
    PhyTallyEspNowHeader header;
    HubPayload payload;
} __attribute__((packed));

struct PhyTallyEspNowTelemetryPacket {
    PhyTallyEspNowHeader header;
    TelemetryPayload payload;
} __attribute__((packed));

inline const char *PhyTallyTransportName(uint8_t transportKind) {
    switch (transportKind) {
        case PHYTALLY_TRANSPORT_ESPNOW:
            return "espnow";
        case PHYTALLY_TRANSPORT_BEACON_PROBE:
        default:
            return "beacon_probe";
    }
}

inline uint8_t PhyTallyPreferredTransport(uint8_t routePriority) {
    return routePriority == PHYTALLY_ROUTE_ESPNOW_FIRST
        ? PHYTALLY_TRANSPORT_ESPNOW
        : PHYTALLY_TRANSPORT_BEACON_PROBE;
}

inline uint8_t PhyTallyFallbackTransport(uint8_t routePriority) {
    return routePriority == PHYTALLY_ROUTE_ESPNOW_FIRST
        ? PHYTALLY_TRANSPORT_BEACON_PROBE
        : PHYTALLY_TRANSPORT_ESPNOW;
}

inline void PhyTallyInitEspNowHeader(
    PhyTallyEspNowHeader &header,
    uint8_t messageType,
    uint16_t sequence,
    uint16_t payloadLen) {
    header.magic = PHYTALLY_ESPNOW_MAGIC;
    header.version = PHYTALLY_ESPNOW_VERSION;
    header.message_type = messageType;
    header.reserved = 0;
    header.sequence = sequence;
    header.payload_len = payloadLen;
}

inline bool PhyTallyValidateEspNowHeader(
    const PhyTallyEspNowHeader &header,
    uint8_t expectedType,
    size_t expectedPayloadLen) {
    return header.magic == PHYTALLY_ESPNOW_MAGIC &&
        header.version == PHYTALLY_ESPNOW_VERSION &&
        header.message_type == expectedType &&
        header.payload_len == expectedPayloadLen;
}

inline void PhyTallyBuildEspNowHubPacket(
    PhyTallyEspNowHubPacket &packet,
    const HubPayload &payload,
    uint16_t sequence) {
    PhyTallyInitEspNowHeader(
        packet.header,
        PHYTALLY_ESPNOW_MSG_HUB_STATE,
        sequence,
        sizeof(HubPayload));
    memcpy(&packet.payload, &payload, sizeof(HubPayload));
}

inline void PhyTallyBuildEspNowTelemetryPacket(
    PhyTallyEspNowTelemetryPacket &packet,
    const TelemetryPayload &payload,
    uint16_t sequence) {
    PhyTallyInitEspNowHeader(
        packet.header,
        PHYTALLY_ESPNOW_MSG_TELEMETRY,
        sequence,
        sizeof(TelemetryPayload));
    memcpy(&packet.payload, &payload, sizeof(TelemetryPayload));
}

#endif
