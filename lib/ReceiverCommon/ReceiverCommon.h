#ifndef RECEIVER_COMMON_H
#define RECEIVER_COMMON_H

#include <Arduino.h>

#include "../TallyProtocol/TallyProtocol.h"
#include "../TallyTransport/TallyTransport.h"

namespace receivercommon {

constexpr unsigned long kTelemetryIntervalMs = BEACON_INTERVAL_MS;

struct RuntimeState {
  uint8_t tallyId = 0;
  uint8_t ledBrightness = PHYTALLY_DEFAULT_LED_BRIGHTNESS;
  uint8_t lastAckCommandId = 0;
  uint8_t pendingAckCommandId = 0;
  uint8_t routePriority = PHYTALLY_ROUTE_BEACON_FIRST;
  uint8_t activeTransport = PHYTALLY_TRANSPORT_BEACON_PROBE;
  TallyState currentState = TALLY_OFF;
  unsigned long lastPacketTime = 0;
  unsigned long lastBeaconPacketTime = 0;
  unsigned long lastEspNowPacketTime = 0;
  unsigned long lastTransportSwitchAt = 0;
  int8_t lastRssi = -100;
  bool pendingIdApply = false;
  uint8_t pendingAssignedId = 0;
  bool pendingBrightnessApply = false;
  uint8_t pendingBrightness = PHYTALLY_DEFAULT_LED_BRIGHTNESS;
  bool pendingEspNowPhyApply = false;
  uint8_t pendingEspNowPhyMode = PHYTALLY_ESPNOW_PHY_11B_1M;
  uint8_t espNowPhyMode = PHYTALLY_ESPNOW_PHY_11B_1M;
  uint8_t hubMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool hubMacKnown = false;
  uint8_t currentChannel = 1;
  uint8_t lockedChannel = 0;
  unsigned long lastChannelJump = 0;
  unsigned long lastScanLog = 0;
  unsigned long lastHubLog = 0;
  unsigned long lastTelemetryLog = 0;
  unsigned long identifyUntil = 0;
  bool signalLostLogged = false;
  uint16_t lastEspNowSequence = 0;
};

enum class AppliedCommandAction {
  None,
  SetID,
  SetBrightness,
  SetEspNowPhy,
  Identify,
};

enum class SerialCommandKind {
  None,
  SetID,
  GetInfo,
};

struct SerialCommand {
  SerialCommandKind kind = SerialCommandKind::None;
  uint8_t tallyId = 0;
};

struct FrameResult {
  bool accepted = false;
  bool hubLockChanged = false;
  bool beaconLogDue = false;
  bool commandPresent = false;
  bool commandQueued = false;
  AppliedCommandAction commandAction = AppliedCommandAction::None;
  uint8_t transportKind = PHYTALLY_TRANSPORT_BEACON_PROBE;
  uint8_t beaconTagLen = 0;
  uint8_t commandType = 0;
  uint8_t commandParam = 0;
  uint8_t commandId = 0;
  uint8_t targetMac[6] = {0, 0, 0, 0, 0, 0};
};

struct SignalLossResult {
  bool lost = false;
  bool channelChanged = false;
  bool scanLogDue = false;
};

inline uint8_t SanitizeTallyId(uint8_t tallyId) {
  return tallyId < MAX_TALLIES ? tallyId : 0;
}

inline void FormatMac(const uint8_t *mac, char *out, size_t outLen) {
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

inline bool ShouldLogInterval(unsigned long now, unsigned long &lastAt, unsigned long intervalMs) {
  if (now - lastAt <= intervalMs) {
    return false;
  }
  lastAt = now;
  return true;
}

inline int BuildTelemetryPacket(
    uint8_t *packet,
    size_t packetSize,
    const uint8_t hubMac[6],
    const uint8_t myMac[6],
    uint8_t tallyId,
    int8_t lastRssi,
    uint8_t ledBrightness,
    uint8_t ackCommandId) {
  if (packetSize < 128) {
    return -1;
  }

  int ptr = 0;
  packet[ptr++] = 0x40;
  packet[ptr++] = 0x00;
  packet[ptr++] = 0x00;
  packet[ptr++] = 0x00;
  memcpy(&packet[ptr], hubMac, 6);
  ptr += 6;
  memcpy(&packet[ptr], myMac, 6);
  ptr += 6;
  memcpy(&packet[ptr], hubMac, 6);
  ptr += 6;
  packet[ptr++] = 0x00;
  packet[ptr++] = 0x00;
  packet[ptr++] = 0x00;
  packet[ptr++] = 0x00;

  packet[ptr++] = 0xDD;
  packet[ptr++] = sizeof(TelemetryPayload);

  TelemetryPayload telem = {};
  telem.oui[0] = TELEM_OUI_0;
  telem.oui[1] = TELEM_OUI_1;
  telem.oui[2] = TELEM_OUI_2;
  telem.protocol_version = 0x01;
  telem.tally_id = tallyId;
  telem.battery_percent = 100;
  telem.rssi = lastRssi;
  telem.status_flags = 0;
  telem.led_brightness = ledBrightness;
  telem.ack_command_id = ackCommandId;
  memcpy(&packet[ptr], &telem, sizeof(telem));
  ptr += sizeof(telem);

  return ptr;
}

inline TelemetryPayload BuildTelemetryPayload(
    uint8_t tallyId,
    int8_t lastRssi,
    uint8_t ledBrightness,
    uint8_t ackCommandId) {
  TelemetryPayload telem = {};
  telem.oui[0] = TELEM_OUI_0;
  telem.oui[1] = TELEM_OUI_1;
  telem.oui[2] = TELEM_OUI_2;
  telem.protocol_version = 0x01;
  telem.tally_id = tallyId;
  telem.battery_percent = 100;
  telem.rssi = lastRssi;
  telem.status_flags = 0;
  telem.led_brightness = ledBrightness;
  telem.ack_command_id = ackCommandId;
  return telem;
}

inline int BuildEspNowTelemetryPacket(
    uint8_t *packet,
    size_t packetSize,
    uint16_t sequence,
    uint8_t tallyId,
    int8_t lastRssi,
    uint8_t ledBrightness,
    uint8_t ackCommandId) {
  if (packetSize < sizeof(PhyTallyEspNowTelemetryPacket)) {
    return -1;
  }

  TelemetryPayload telem = BuildTelemetryPayload(
      tallyId,
      lastRssi,
      ledBrightness,
      ackCommandId);
  PhyTallyEspNowTelemetryPacket espNowPacket = {};
  PhyTallyBuildEspNowTelemetryPacket(espNowPacket, telem, sequence);
  memcpy(packet, &espNowPacket, sizeof(espNowPacket));
  return sizeof(espNowPacket);
}

inline bool ShouldSendTelemetry(
    unsigned long now,
    const RuntimeState &state,
    unsigned long packetTimeoutMs = 2000) {
  if (!state.hubMacKnown || state.lastPacketTime == 0) {
    return false;
  }

  if (state.currentState == TALLY_ERROR) {
    return false;
  }

  return now - state.lastPacketTime <= packetTimeoutMs;
}

inline bool IsIdentifyActive(unsigned long now, const RuntimeState &state) {
  return state.identifyUntil != 0 && now < state.identifyUntil;
}

inline bool CanSwitchTransport(
    unsigned long now,
    const RuntimeState &state,
    unsigned long holdDownMs = PHYTALLY_ROUTE_SWITCH_HOLDDOWN_MS) {
  return state.lastTransportSwitchAt == 0 || now - state.lastTransportSwitchAt >= holdDownMs;
}

inline bool AcceptTransport(
    unsigned long now,
    RuntimeState &state,
    uint8_t transportKind,
    unsigned long packetTimeoutMs = 2000,
    unsigned long holdDownMs = PHYTALLY_ROUTE_SWITCH_HOLDDOWN_MS) {
  if (state.activeTransport == transportKind) {
    return true;
  }

  const bool activeHealthy = state.lastPacketTime != 0 && now - state.lastPacketTime <= packetTimeoutMs;
  if (activeHealthy || !CanSwitchTransport(now, state, holdDownMs)) {
    return false;
  }

  state.activeTransport = transportKind;
  state.lastTransportSwitchAt = now;
  return true;
}

inline bool IsBlinkOn(unsigned long now, unsigned long periodMs, unsigned long onMs) {
  if (periodMs == 0 || onMs == 0) {
    return false;
  }
  return (now % periodMs) < onMs;
}

inline bool IsErrorBlinkOn(unsigned long now) {
  return IsBlinkOn(now, PHYTALLY_ERROR_BLINK_PERIOD_MS, PHYTALLY_ERROR_BLINK_ON_MS);
}

inline uint8_t ScaleBrightness8(uint8_t value, uint8_t brightness) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) * brightness + 127) / 255);
}

inline bool IsValidEspNowPhyMode(uint8_t mode) {
  return mode >= PHYTALLY_ESPNOW_PHY_LR_250K && mode <= PHYTALLY_ESPNOW_PHY_11G_6M;
}

inline SerialCommand ReadSerialCommand(Stream &serial) {
  if (!serial.available()) {
    return {};
  }

  String line = serial.readStringUntil('\n');
  line.trim();

  if (line.startsWith("SET_ID=")) {
    const int newId = line.substring(7).toInt();
    if (newId >= 0 && newId < MAX_TALLIES) {
      SerialCommand command;
      command.kind = SerialCommandKind::SetID;
      command.tallyId = static_cast<uint8_t>(newId);
      return command;
    }
    return {};
  }

  if (line == "GET_INFO") {
    SerialCommand command;
    command.kind = SerialCommandKind::GetInfo;
    return command;
  }

  return {};
}

inline FrameResult ApplyHubPayload(
    const HubPayload &hub,
    const uint8_t sourceMac[6],
    int8_t rssi,
    const uint8_t myMac[6],
    unsigned long now,
    RuntimeState &state,
    bool rejectHubSwitchWhileLocked,
    uint8_t transportKind,
    uint16_t espNowSequence = 0) {
  FrameResult result;
  result.transportKind = transportKind;

  if (transportKind == PHYTALLY_TRANSPORT_ESPNOW &&
      espNowSequence != 0 &&
      espNowSequence == state.lastEspNowSequence) {
    return result;
  }

  if (!AcceptTransport(now, state, transportKind)) {
    return result;
  }

  if (rejectHubSwitchWhileLocked &&
      state.hubMacKnown &&
      memcmp(state.hubMac, sourceMac, 6) != 0 &&
      now - state.lastPacketTime < 3000) {
    return result;
  }

  const bool hubChanged = !state.hubMacKnown || memcmp(state.hubMac, sourceMac, 6) != 0;
  memcpy(state.hubMac, sourceMac, 6);
  state.hubMacKnown = true;
  state.lastRssi = rssi;
  state.signalLostLogged = false;
  state.lastPacketTime = now;
  if (transportKind == PHYTALLY_TRANSPORT_ESPNOW) {
    state.lastEspNowPacketTime = now;
    state.lastEspNowSequence = espNowSequence;
  } else {
    state.lastBeaconPacketTime = now;
  }

  if (state.tallyId < MAX_TALLIES) {
    state.currentState = static_cast<TallyState>(hub.states[state.tallyId]);
  }

  result.accepted = true;
  result.hubLockChanged = hubChanged || state.lockedChannel != state.currentChannel;
  if (result.hubLockChanged) {
    state.lockedChannel = state.currentChannel;
  }

  if (hub.cmd_type != 0) {
    result.commandPresent = true;
    result.commandType = hub.cmd_type;
    result.commandParam = hub.cmd_param;
    result.commandId = hub.cmd_id;
    memcpy(result.targetMac, hub.target_mac, sizeof(result.targetMac));

    const bool targetMatch = memcmp(hub.target_mac, myMac, 6) == 0;
    const bool seenCommandId = hub.cmd_id != 0 &&
        (hub.cmd_id == state.lastAckCommandId || hub.cmd_id == state.pendingAckCommandId);

    if (targetMatch &&
        !seenCommandId &&
        hub.cmd_type == HUB_CMD_SET_ID &&
        hub.cmd_param < MAX_TALLIES) {
      if (hub.cmd_param == state.tallyId) {
        state.lastAckCommandId = hub.cmd_id;
        state.pendingAckCommandId = 0;
      } else {
        state.pendingAssignedId = hub.cmd_param;
        state.pendingIdApply = true;
        state.pendingAckCommandId = hub.cmd_id;
        result.commandQueued = true;
        result.commandAction = AppliedCommandAction::SetID;
      }
    } else if (targetMatch &&
               !seenCommandId &&
               hub.cmd_type == HUB_CMD_SET_BRIGHTNESS) {
      if (hub.cmd_param == state.ledBrightness) {
        state.lastAckCommandId = hub.cmd_id;
        state.pendingAckCommandId = 0;
      } else {
        state.pendingBrightness = hub.cmd_param;
        state.pendingBrightnessApply = true;
        state.pendingAckCommandId = hub.cmd_id;
        result.commandQueued = true;
        result.commandAction = AppliedCommandAction::SetBrightness;
      }
    } else if (targetMatch &&
               !seenCommandId &&
               hub.cmd_type == HUB_CMD_SET_ESPNOW_PHY &&
               IsValidEspNowPhyMode(hub.cmd_param)) {
      if (hub.cmd_param == state.espNowPhyMode) {
        state.lastAckCommandId = hub.cmd_id;
        state.pendingAckCommandId = 0;
      } else {
        state.pendingEspNowPhyMode = hub.cmd_param;
        state.pendingEspNowPhyApply = true;
        state.pendingAckCommandId = hub.cmd_id;
        result.commandQueued = true;
        result.commandAction = AppliedCommandAction::SetEspNowPhy;
      }
    } else if (targetMatch &&
               !seenCommandId &&
               hub.cmd_type == HUB_CMD_IDENTIFY) {
      state.identifyUntil = now + PHYTALLY_IDENTIFY_DURATION_MS;
      state.lastAckCommandId = hub.cmd_id;
      state.pendingAckCommandId = 0;
      result.commandQueued = true;
      result.commandAction = AppliedCommandAction::Identify;
    }
  } else if (ShouldLogInterval(now, state.lastHubLog, 5000)) {
    result.beaconLogDue = true;
    result.beaconTagLen = sizeof(HubPayload);
  }

  return result;
}

inline FrameResult ProcessHubFrame(
    const uint8_t *frame,
    int frameLen,
    int8_t rssi,
    const uint8_t myMac[6],
    unsigned long now,
    RuntimeState &state,
    bool rejectHubSwitchWhileLocked) {
  FrameResult result;

  if (frameLen < 36) {
    return result;
  }
  if (frame[0] != 0x80 && frame[0] != 0x50) {
    return result;
  }

  int offset = 36;
  while (offset + 2 <= frameLen) {
    const uint8_t tag = frame[offset];
    const uint8_t tagLen = frame[offset + 1];
    if (offset + 2 + tagLen > frameLen) {
      break;
    }

    if (tag == 0xDD &&
        tagLen >= sizeof(HubPayload) - 1 &&
        frame[offset + 2] == VENDOR_OUI_0 &&
        frame[offset + 3] == VENDOR_OUI_1 &&
        frame[offset + 4] == VENDOR_OUI_2 &&
        frame[offset + 5] == 0x01) {
      if (rejectHubSwitchWhileLocked &&
          state.hubMacKnown &&
          memcmp(state.hubMac, &frame[10], 6) != 0 &&
          now - state.lastPacketTime < 3000) {
        break;
      }

      HubPayload hub = {};
      size_t copyLen = tagLen;
      if (copyLen > sizeof(HubPayload)) {
        copyLen = sizeof(HubPayload);
      }
      memcpy(&hub, &frame[offset + 2], copyLen);
      result = ApplyHubPayload(
          hub,
          &frame[10],
          rssi,
          myMac,
          now,
          state,
          rejectHubSwitchWhileLocked,
          PHYTALLY_TRANSPORT_BEACON_PROBE);
      if (result.accepted) {
        result.beaconTagLen = tagLen;
      }
      return result;
    }

    offset += 2 + tagLen;
  }

  return result;
}

inline FrameResult ProcessEspNowHubPacket(
    const uint8_t *packet,
    int packetLen,
    int8_t rssi,
    const uint8_t sourceMac[6],
    const uint8_t myMac[6],
    unsigned long now,
    RuntimeState &state,
    bool rejectHubSwitchWhileLocked) {
  FrameResult result;
  if (packetLen < static_cast<int>(sizeof(PhyTallyEspNowHubPacket))) {
    return result;
  }

  const auto *espNowPacket = reinterpret_cast<const PhyTallyEspNowHubPacket *>(packet);
  if (!PhyTallyValidateEspNowHeader(
          espNowPacket->header,
          PHYTALLY_ESPNOW_MSG_HUB_STATE,
          sizeof(HubPayload))) {
    return result;
  }

  return ApplyHubPayload(
      espNowPacket->payload,
      sourceMac,
      rssi,
      myMac,
      now,
      state,
      rejectHubSwitchWhileLocked,
      PHYTALLY_TRANSPORT_ESPNOW,
      espNowPacket->header.sequence);
}

inline SignalLossResult HandleSignalLoss(
    unsigned long now,
    RuntimeState &state,
    uint8_t maxChannel = 13,
    unsigned long packetTimeoutMs = 2000,
    unsigned long channelJumpMs = 300,
    unsigned long scanLogMs = 1000) {
  SignalLossResult result;

  if (now - state.lastPacketTime <= packetTimeoutMs) {
    return result;
  }

  result.lost = true;
  state.currentState = TALLY_ERROR;
  if (!state.signalLostLogged) {
    state.signalLostLogged = true;
  }

  if (now - state.lastChannelJump > channelJumpMs) {
    state.lastChannelJump = now;
    state.currentChannel = (state.currentChannel % maxChannel) + 1;
    result.channelChanged = true;
    if (now - state.lastScanLog > scanLogMs) {
      state.lastScanLog = now;
      result.scanLogDue = true;
    }
  }

  return result;
}

}  // namespace receivercommon

#endif
