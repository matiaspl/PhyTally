#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>

extern "C" {
#include <user_interface.h>
}

#include "../../lib/ReceiverCommon/ReceiverCommon.h"

#define TALLY_LED 16
#define TALLY_LED_ACTIVE LOW
#define EEPROM_SIZE 16
#define EEPROM_TALLY_ID_ADDR 0
#define EEPROM_BRIGHTNESS_ADDR 1
#define EEPROM_ACK_COMMAND_ID_ADDR 2
#define EEPROM_CONFIG_MAGIC_ADDR 15
#define EEPROM_CONFIG_MAGIC_VALUE 0x42

struct RxControl {
  signed rssi : 8;
  unsigned rate : 4;
  unsigned is_group : 1;
  unsigned : 1;
  unsigned sig_mode : 2;
  unsigned legacy_length : 12;
  unsigned damatch0 : 1;
  unsigned damatch1 : 1;
  unsigned bssidmatch0 : 1;
  unsigned bssidmatch1 : 1;
  unsigned mcs : 7;
  unsigned cwb : 1;
  unsigned ht_length : 16;
  unsigned smoothing : 1;
  unsigned not_sounding : 1;
  unsigned : 1;
  unsigned aggregation : 1;
  unsigned stbc : 2;
  unsigned fec_coding : 1;
  unsigned sgi : 1;
  unsigned rxend_state : 8;
  unsigned ampdu_cnt : 8;
  unsigned channel : 4;
  unsigned : 12;
};

struct SnifferPacket {
  RxControl rx_ctrl;
  uint8_t payload[112];
  uint16_t cnt;
  uint16_t len;
} __attribute__((packed));

receivercommon::RuntimeState rxState;
uint8_t myMac[6] = {0};
unsigned long lastTelemetryAt = 0;

void writeTallyLedRaw(bool on) {
  digitalWrite(TALLY_LED, on ? TALLY_LED_ACTIVE : !TALLY_LED_ACTIVE);
}

void setTallyLedLevel(uint8_t level, unsigned long now) {
  if (level == 0) {
    writeTallyLedRaw(false);
    return;
  }
  if (level >= 255) {
    writeTallyLedRaw(true);
    return;
  }

  const uint8_t phase = static_cast<uint8_t>(now % 16);
  const uint8_t threshold = static_cast<uint8_t>((static_cast<uint16_t>(level) * 16 + 127) / 255);
  writeTallyLedRaw(phase < threshold);
}

uint8_t faultVisibleBrightness() {
  return rxState.ledBrightness < 128 ? 128 : rxState.ledBrightness;
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  rxState.tallyId = receivercommon::SanitizeTallyId(EEPROM.read(EEPROM_TALLY_ID_ADDR));
  if (EEPROM.read(EEPROM_CONFIG_MAGIC_ADDR) != EEPROM_CONFIG_MAGIC_VALUE) {
    rxState.ledBrightness = PHYTALLY_DEFAULT_LED_BRIGHTNESS;
    rxState.lastAckCommandId = 0;
    EEPROM.write(EEPROM_BRIGHTNESS_ADDR, rxState.ledBrightness);
    EEPROM.write(EEPROM_ACK_COMMAND_ID_ADDR, 0);
    EEPROM.write(EEPROM_CONFIG_MAGIC_ADDR, EEPROM_CONFIG_MAGIC_VALUE);
    EEPROM.commit();
    return;
  }

  rxState.ledBrightness = EEPROM.read(EEPROM_BRIGHTNESS_ADDR);
  rxState.lastAckCommandId = EEPROM.read(EEPROM_ACK_COMMAND_ID_ADDR);
}

void saveTallyId(uint8_t newId) {
  EEPROM.write(EEPROM_TALLY_ID_ADDR, newId);
  EEPROM.commit();
}

void saveBrightness(uint8_t brightness) {
  EEPROM.write(EEPROM_BRIGHTNESS_ADDR, brightness);
  EEPROM.write(EEPROM_CONFIG_MAGIC_ADDR, EEPROM_CONFIG_MAGIC_VALUE);
  EEPROM.commit();
}

void saveAckCommandId(uint8_t ackCommandId) {
  EEPROM.write(EEPROM_ACK_COMMAND_ID_ADDR, ackCommandId);
  EEPROM.write(EEPROM_CONFIG_MAGIC_ADDR, EEPROM_CONFIG_MAGIC_VALUE);
  EEPROM.commit();
}

void configureLowBandwidthTelemetry() {
  wifi_set_phy_mode(PHY_MODE_11B);
  wifi_set_user_fixed_rate(FIXED_RATE_MASK_ALL, RATE_11B1M);
  Serial.println("LOWBW PROBES: 11b @ 1 Mbps");
}

void enterRadioMode() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(10);

  wifi_set_opmode_current(STATION_MODE);
  wifi_set_sleep_type(NONE_SLEEP_T);
  wifi_station_disconnect();
  system_phy_set_max_tpw(82);
  configureLowBandwidthTelemetry();
  wifi_set_channel(rxState.currentChannel);
  wifi_get_macaddr(STATION_IF, myMac);
  wifi_set_promiscuous_rx_cb(nullptr);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb([](uint8_t *buf, uint16_t len) {
    if (len < sizeof(SnifferPacket)) {
      return;
    }

    const auto *packet = reinterpret_cast<const SnifferPacket *>(buf);
    const unsigned long now = millis();
    const receivercommon::FrameResult result = receivercommon::ProcessHubFrame(
        packet->payload,
        packet->len,
        packet->rx_ctrl.rssi,
        myMac,
        now,
        rxState,
        true);
    if (!result.accepted) {
      return;
    }

    if (result.hubLockChanged) {
      char macStr[18];
      receivercommon::FormatMac(rxState.hubMac, macStr, sizeof(macStr));
      Serial.printf("HUB LOCK: HUB=%s CH=%u RSSI=%d\n",
                    macStr,
                    (unsigned int)rxState.lockedChannel,
                    (int)rxState.lastRssi);
    }

    if (result.commandPresent) {
      Serial.printf(
          "COMMAND DETECTED: Type=%u Param=%u for MAC %02X:%02X:%02X:%02X:%02X:%02X "
          "(My MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
          (unsigned int)result.commandType,
          (unsigned int)result.commandParam,
          result.targetMac[0], result.targetMac[1], result.targetMac[2],
          result.targetMac[3], result.targetMac[4], result.targetMac[5],
          myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
      if (result.commandQueued) {
        switch (result.commandAction) {
          case receivercommon::AppliedCommandAction::SetID:
            Serial.printf("MATCH! Queuing new ID: %u for apply in main loop...\n",
                          (unsigned int)(result.commandParam + 1));
            break;
          case receivercommon::AppliedCommandAction::SetBrightness:
            Serial.printf("MATCH! Queuing brightness: %u/255 for apply in main loop...\n",
                          (unsigned int)result.commandParam);
            break;
          case receivercommon::AppliedCommandAction::Identify:
            Serial.println("MATCH! Starting identify pattern...");
            break;
          case receivercommon::AppliedCommandAction::None:
          default:
            break;
        }
      }
      return;
    }

    if (result.beaconLogDue) {
      Serial.printf("HUB BEACON: Len=%u, ID=%u, RSSI=%d\n",
                    (unsigned int)result.beaconTagLen,
                    (unsigned int)(rxState.tallyId + 1),
                    (int)rxState.lastRssi);
    }
  });
  wifi_promiscuous_enable(1);
}

void sendTelemetry() {
  const unsigned long now = millis();
  if (!receivercommon::ShouldSendTelemetry(now, rxState)) {
    return;
  }

  uint8_t packet[128];
  const int packetLen = receivercommon::BuildTelemetryPacket(
      packet,
      sizeof(packet),
      rxState.hubMac,
      myMac,
      rxState.tallyId,
      rxState.lastRssi,
      rxState.ledBrightness,
      rxState.lastAckCommandId);
  if (packetLen <= 0) {
    return;
  }

  const int err = wifi_send_pkt_freedom(packet, packetLen, true);
  if (err != 0) {
    Serial.printf("TELEM TX FAILED: err=%d\n", err);
    return;
  }

  if (receivercommon::ShouldLogInterval(now, rxState.lastTelemetryLog, 5000)) {
    char macStr[18];
    receivercommon::FormatMac(rxState.hubMac, macStr, sizeof(macStr));
    Serial.printf("TELEM TX: HUB=%s CH=%u ID=%u HUB_RSSI=%d\n",
                  macStr,
                  (unsigned int)rxState.currentChannel,
                  (unsigned int)(rxState.tallyId + 1),
                  (int)rxState.lastRssi);
  }
}

void handleSerialConfig() {
  const receivercommon::SerialCommand command = receivercommon::ReadSerialCommand(Serial);
  switch (command.kind) {
    case receivercommon::SerialCommandKind::SetID:
      saveTallyId(command.tallyId);
      Serial.printf("OK:ID=%u\n", (unsigned int)command.tallyId);
      delay(200);
      ESP.restart();
      return;
    case receivercommon::SerialCommandKind::GetInfo:
      Serial.printf("INFO:ID=%u,VER=8266-1.0\n", (unsigned int)rxState.tallyId);
      return;
    case receivercommon::SerialCommandKind::None:
    default:
      return;
  }
}

void renderTallyState() {
  const unsigned long now = millis();
  uint8_t level = 0;

  if (receivercommon::IsIdentifyActive(now, rxState)) {
    const bool pulseOn = ((now / 80) % 2) == 0;
    const uint8_t identifyLevel = faultVisibleBrightness();
    setTallyLedLevel(pulseOn ? identifyLevel : 0, now);
    return;
  }

  switch (rxState.currentState) {
    case TALLY_OFF:
      level = 0;
      break;
    case TALLY_PREVIEW:
      level = (now % 400) < 100 ? rxState.ledBrightness : 0;
      break;
    case TALLY_PROGRAM:
      level = rxState.ledBrightness;
      break;
    case TALLY_ERROR:
    default:
      level = receivercommon::IsErrorBlinkOn(now) ? faultVisibleBrightness() : 0;
      break;
  }
  setTallyLedLevel(level, now);
}

void applyPendingConfigIfNeeded() {
  if (rxState.pendingBrightnessApply) {
    noInterrupts();
    const uint8_t brightness = rxState.pendingBrightness;
    rxState.pendingBrightnessApply = false;
    interrupts();

    rxState.ledBrightness = brightness;
    saveBrightness(brightness);
    rxState.lastAckCommandId = rxState.pendingAckCommandId;
    rxState.pendingAckCommandId = 0;
    saveAckCommandId(rxState.lastAckCommandId);
    Serial.printf("APPLY BRIGHTNESS: %u/255\n", (unsigned int)brightness);
  }

  if (!rxState.pendingIdApply) {
    return;
  }

  noInterrupts();
  const uint8_t newId = rxState.pendingAssignedId;
  rxState.pendingIdApply = false;
  interrupts();

  saveTallyId(newId);
  saveAckCommandId(rxState.pendingAckCommandId);
  rxState.lastAckCommandId = rxState.pendingAckCommandId;
  rxState.pendingAckCommandId = 0;
  Serial.printf("APPLY ID: %u and rebooting...\n", (unsigned int)(newId + 1));
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(TALLY_LED, OUTPUT);
  writeTallyLedRaw(false);

  loadConfig();
  enterRadioMode();

  Serial.printf(
      "\n--- PhyTally Receiver ESP8266 v1.0 ---\nID: %u\nMAC: %02X:%02X:%02X:%02X:%02X:%02X\nREADY_FOR_CONFIG\n",
      (unsigned int)(rxState.tallyId + 1),
      myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
}

void loop() {
  handleSerialConfig();
  applyPendingConfigIfNeeded();

  const unsigned long now = millis();
  const bool wasSignalLost = rxState.signalLostLogged;
  const receivercommon::SignalLossResult signalLoss = receivercommon::HandleSignalLoss(now, rxState);
  if (signalLoss.lost) {
    if (!wasSignalLost) {
      Serial.println("HUB LOST: entering channel scan");
    }
    if (signalLoss.channelChanged) {
      wifi_set_channel(rxState.currentChannel);
    }
    if (signalLoss.scanLogDue) {
      Serial.printf("SCAN: CH=%u\n", (unsigned int)rxState.currentChannel);
    }
  }

  if (now - lastTelemetryAt > 2000) {
    lastTelemetryAt = now;
    sendTelemetry();
  }

  renderTallyState();
  delay(10);
}
