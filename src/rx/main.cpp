#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../../lib/ReceiverCommon/ReceiverCommon.h"

#ifndef PHYTALLY_NEOPIXEL_PIN
#define PHYTALLY_NEOPIXEL_PIN 13
#endif

#ifndef PHYTALLY_STATUS_LED_PIN
#define PHYTALLY_STATUS_LED_PIN 2
#endif

#ifndef PHYTALLY_HAS_STATUS_LED
#define PHYTALLY_HAS_STATUS_LED 1
#endif

#ifndef PHYTALLY_ENABLE_ESPNOW
#define PHYTALLY_ENABLE_ESPNOW 1
#endif

#ifndef PHYTALLY_FORCE_ESPNOW
#define PHYTALLY_FORCE_ESPNOW 0
#endif

#define NUM_LEDS 1

CRGB leds[NUM_LEDS];
Preferences prefs;
receivercommon::RuntimeState rxState;
unsigned long ledOffTime = 0;
uint8_t myMac[6] = {0};
uint16_t espNowTelemetrySequence = 1;
const uint8_t kEspNowBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

const char *espNowPhyModeName(uint8_t mode) {
  switch (mode) {
    case PHYTALLY_ESPNOW_PHY_LR_250K:
      return "lr_250k";
    case PHYTALLY_ESPNOW_PHY_LR_500K:
      return "lr_500k";
    case PHYTALLY_ESPNOW_PHY_11B_2M:
      return "11b_2m";
    case PHYTALLY_ESPNOW_PHY_11B_11M:
      return "11b_11m";
    case PHYTALLY_ESPNOW_PHY_11G_6M:
      return "11g_6m";
    case PHYTALLY_ESPNOW_PHY_11B_1M:
    default:
      return "11b_1m";
  }
}

uint8_t espNowProtocolBitmapForMode(uint8_t mode) {
  uint8_t bitmap = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  if (mode == PHYTALLY_ESPNOW_PHY_LR_250K || mode == PHYTALLY_ESPNOW_PHY_LR_500K) {
    bitmap |= WIFI_PROTOCOL_LR;
  }
  return bitmap;
}

wifi_phy_rate_t espNowPhyRateForMode(uint8_t mode) {
  switch (mode) {
    case PHYTALLY_ESPNOW_PHY_LR_250K:
      return WIFI_PHY_RATE_LORA_250K;
    case PHYTALLY_ESPNOW_PHY_LR_500K:
      return WIFI_PHY_RATE_LORA_500K;
    case PHYTALLY_ESPNOW_PHY_11B_2M:
      return WIFI_PHY_RATE_2M_L;
    case PHYTALLY_ESPNOW_PHY_11B_11M:
      return WIFI_PHY_RATE_11M_L;
    case PHYTALLY_ESPNOW_PHY_11G_6M:
      return WIFI_PHY_RATE_6M;
    case PHYTALLY_ESPNOW_PHY_11B_1M:
    default:
      return WIFI_PHY_RATE_1M_L;
  }
}

bool applyEspNowPhyMode(uint8_t mode) {
  if (!receivercommon::IsValidEspNowPhyMode(mode)) {
    Serial.printf("ESPNOW PHY INVALID: %u\n", (unsigned int)mode);
    return false;
  }

  const esp_err_t protocolErr = esp_wifi_set_protocol(WIFI_IF_STA, espNowProtocolBitmapForMode(mode));
  if (protocolErr != ESP_OK) {
    Serial.printf("ESPNOW PROTOCOL FAILED: err=%d mode=%s\n", (int)protocolErr, espNowPhyModeName(mode));
    return false;
  }

  const esp_err_t rateErr = esp_wifi_config_espnow_rate(WIFI_IF_STA, espNowPhyRateForMode(mode));
  if (rateErr != ESP_OK) {
    Serial.printf("ESPNOW RATE FAILED: err=%d mode=%s\n", (int)rateErr, espNowPhyModeName(mode));
    return false;
  }

  rxState.espNowPhyMode = mode;
  Serial.printf("ESPNOW PHY MODE: %s\n", espNowPhyModeName(mode));
  return true;
}

const char *telemetryPhyLabel(uint8_t transport) {
#if PHYTALLY_ENABLE_ESPNOW
  if (PHYTALLY_FORCE_ESPNOW || transport == PHYTALLY_TRANSPORT_ESPNOW) {
    return espNowPhyModeName(rxState.espNowPhyMode);
  }
#endif
  return "11b_1m_probe";
}

const char *receivePhyLabel(uint8_t transport) {
#if PHYTALLY_ENABLE_ESPNOW
  if (PHYTALLY_FORCE_ESPNOW || transport == PHYTALLY_TRANSPORT_ESPNOW) {
    return espNowPhyModeName(rxState.espNowPhyMode);
  }
#endif
  return "beacon_probe";
}

CRGB scaledColor(const CRGB &baseColor) {
  CRGB scaled = baseColor;
  scaled.nscale8_video(rxState.ledBrightness);
  return scaled;
}

CRGB identifyColor(unsigned long now) {
  const uint8_t identifyBrightness = rxState.ledBrightness < 96 ? 96 : rxState.ledBrightness;
  return CHSV(static_cast<uint8_t>(now / 8), 255, identifyBrightness);
}

void configureLowBandwidthTelemetry() {
  esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
  if (err != ESP_OK) {
    Serial.printf("LOWBW PROTOCOL FAILED: err=%d\n", (int)err);
    return;
  }

  err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
  if (err != ESP_OK) {
    Serial.printf("LOWBW RATE FAILED: err=%d\n", (int)err);
    return;
  }

  Serial.println("LOWBW PROBES: 11b @ 1 Mbps");
}

void sendTelemetry() {
  const unsigned long now = millis();
  if (!receivercommon::ShouldSendTelemetry(now, rxState)) {
    return;
  }

#if PHYTALLY_ENABLE_ESPNOW
  if (PHYTALLY_FORCE_ESPNOW || rxState.activeTransport == PHYTALLY_TRANSPORT_ESPNOW) {
    uint8_t packet[sizeof(PhyTallyEspNowTelemetryPacket)];
    const int packetLen = receivercommon::BuildEspNowTelemetryPacket(
        packet,
        sizeof(packet),
        espNowTelemetrySequence++,
        rxState.tallyId,
        rxState.lastRssi,
        rxState.ledBrightness,
        rxState.lastAckCommandId);
    if (packetLen <= 0) {
      return;
    }

    const esp_err_t err = esp_now_send(kEspNowBroadcastMac, packet, packetLen);
    if (err != ESP_OK) {
      Serial.printf("ESPNOW TELEM TX FAILED: err=%d\n", (int)err);
      return;
    }

    if (receivercommon::ShouldLogInterval(now, rxState.lastTelemetryLog, 5000)) {
      char macStr[18];
      receivercommon::FormatMac(rxState.hubMac, macStr, sizeof(macStr));
      Serial.printf("ESPNOW TELEM TX: HUB=%s CH=%u PHY=%s ID=%u HUB_RSSI=%d\n",
                    macStr,
                    (unsigned int)rxState.currentChannel,
                    telemetryPhyLabel(PHYTALLY_TRANSPORT_ESPNOW),
                    (unsigned int)(rxState.tallyId + 1),
                    (int)rxState.lastRssi);
    }
    return;
  }
#endif

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

  const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, packet, packetLen, true);
  if (err != ESP_OK) {
    Serial.printf("TELEM TX FAILED: err=%d\n", (int)err);
    return;
  }

  if (receivercommon::ShouldLogInterval(now, rxState.lastTelemetryLog, 5000)) {
    char macStr[18];
    receivercommon::FormatMac(rxState.hubMac, macStr, sizeof(macStr));
    Serial.printf("TELEM TX: HUB=%s CH=%u PHY=%s ID=%u HUB_RSSI=%d\n",
                  macStr,
                  (unsigned int)rxState.currentChannel,
                  telemetryPhyLabel(PHYTALLY_TRANSPORT_BEACON_PROBE),
                  (unsigned int)(rxState.tallyId + 1),
                  (int)rxState.lastRssi);
  }
}

void logAcceptedFrame(const receivercommon::FrameResult &result, unsigned long now) {
  if (PHYTALLY_HAS_STATUS_LED) {
    digitalWrite(PHYTALLY_STATUS_LED_PIN, HIGH);
    ledOffTime = now + 20;
  }

  if (result.hubLockChanged) {
    char macStr[18];
    receivercommon::FormatMac(rxState.hubMac, macStr, sizeof(macStr));
    Serial.printf("HUB LOCK: HUB=%s CH=%u RSSI=%d VIA=%s PHY=%s\n",
                  macStr,
                  (unsigned int)rxState.lockedChannel,
                  (int)rxState.lastRssi,
                  PhyTallyTransportName(result.transportKind),
                  receivePhyLabel(result.transportKind));
  }

  if (result.commandPresent) {
    Serial.printf(
        "COMMAND DETECTED VIA=%s: Type=%u Param=%u for MAC %02X:%02X:%02X:%02X:%02X:%02X "
        "(My MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
        PhyTallyTransportName(result.transportKind),
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
        case receivercommon::AppliedCommandAction::SetEspNowPhy:
          Serial.printf("MATCH! Queuing ESP-NOW PHY mode: %s for apply in main loop...\n",
                        espNowPhyModeName(result.commandParam));
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
    Serial.printf("HUB PACKET VIA=%s: Len=%u, ID=%u, RSSI=%d\n",
                  PhyTallyTransportName(result.transportKind),
                  (unsigned int)result.beaconTagLen,
                  (unsigned int)(rxState.tallyId + 1),
                  (int)rxState.lastRssi);
  }
}

void handleSerialConfig() {
  const receivercommon::SerialCommand command = receivercommon::ReadSerialCommand(Serial);
  switch (command.kind) {
    case receivercommon::SerialCommandKind::SetID:
      prefs.putUChar("tallyId", command.tallyId);
      Serial.printf("OK:ID=%u\n", (unsigned int)command.tallyId);
      delay(200);
      ESP.restart();
      return;
    case receivercommon::SerialCommandKind::GetInfo:
      Serial.printf("INFO:ID=%u,VER=1.2\n", (unsigned int)rxState.tallyId);
      return;
    case receivercommon::SerialCommandKind::None:
    default:
      return;
  }
}

void promiscuousRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) {
    return;
  }

  const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
  const unsigned long now = millis();
  const receivercommon::FrameResult result = receivercommon::ProcessHubFrame(
      pkt->payload,
      pkt->rx_ctrl.sig_len,
      pkt->rx_ctrl.rssi,
      myMac,
      now,
      rxState,
      false);
  if (!result.accepted) {
    return;
  }

  logAcceptedFrame(result, now);
}

#if PHYTALLY_ENABLE_ESPNOW
void espNowRxCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !info->src_addr || !info->rx_ctrl) {
    return;
  }

  const unsigned long now = millis();
  const receivercommon::FrameResult result = receivercommon::ProcessEspNowHubPacket(
      data,
      len,
      info->rx_ctrl->rssi,
      info->src_addr,
      myMac,
      now,
      rxState,
      false);
  if (!result.accepted) {
    return;
  }

  logAcceptedFrame(result, now);
}

void initEspNow() {
  const esp_err_t protocolErr = esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  if (protocolErr != ESP_OK) {
    Serial.printf("ESPNOW PROTOCOL FAILED: err=%d\n", (int)protocolErr);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESPNOW INIT FAILED");
    return;
  }
  esp_now_register_recv_cb(espNowRxCb);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kEspNowBroadcastMac, sizeof(peer.peer_addr));
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(kEspNowBroadcastMac)) {
    esp_now_add_peer(&peer);
  }
  applyEspNowPhyMode(rxState.espNowPhyMode);
  Serial.println("ESPNOW READY");
}
#endif

void applyPendingConfigIfNeeded() {
  if (rxState.pendingEspNowPhyApply) {
    noInterrupts();
    const uint8_t mode = rxState.pendingEspNowPhyMode;
    rxState.pendingEspNowPhyApply = false;
    interrupts();

    if (applyEspNowPhyMode(mode)) {
      prefs.putUChar("espnowPhyMode", mode);
      prefs.putUChar("ackCmdId", rxState.pendingAckCommandId);
      rxState.lastAckCommandId = rxState.pendingAckCommandId;
      rxState.pendingAckCommandId = 0;
      Serial.printf("APPLY ESP-NOW PHY: %s\n", espNowPhyModeName(mode));
    }
  }

  if (rxState.pendingBrightnessApply) {
    noInterrupts();
    const uint8_t brightness = rxState.pendingBrightness;
    rxState.pendingBrightnessApply = false;
    interrupts();

    rxState.ledBrightness = brightness;
    prefs.putUChar("brightness", brightness);
    rxState.lastAckCommandId = rxState.pendingAckCommandId;
    rxState.pendingAckCommandId = 0;
    Serial.printf("APPLY BRIGHTNESS: %u/255\n", (unsigned int)brightness);
  }

  if (!rxState.pendingIdApply) {
    return;
  }

  noInterrupts();
  const uint8_t newId = rxState.pendingAssignedId;
  rxState.pendingIdApply = false;
  interrupts();

  prefs.putUChar("tallyId", newId);
  prefs.putUChar("ackCmdId", rxState.pendingAckCommandId);
  rxState.lastAckCommandId = rxState.pendingAckCommandId;
  rxState.pendingAckCommandId = 0;
  Serial.printf("APPLY ID: %u and rebooting...\n", (unsigned int)(newId + 1));
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  if (PHYTALLY_HAS_STATUS_LED) {
    pinMode(PHYTALLY_STATUS_LED_PIN, OUTPUT);
    digitalWrite(PHYTALLY_STATUS_LED_PIN, LOW);
  }
  prefs.begin("tally-rx", false);
  rxState.tallyId = receivercommon::SanitizeTallyId(prefs.getUChar("tallyId", 0));
  rxState.ledBrightness = prefs.getUChar("brightness", PHYTALLY_DEFAULT_LED_BRIGHTNESS);
  rxState.espNowPhyMode = prefs.getUChar("espnowPhyMode", PHYTALLY_ESPNOW_PHY_11B_1M);
  if (!receivercommon::IsValidEspNowPhyMode(rxState.espNowPhyMode)) {
    rxState.espNowPhyMode = PHYTALLY_ESPNOW_PHY_11B_1M;
  }
  rxState.lastAckCommandId = prefs.getUChar("ackCmdId", 0);
  rxState.activeTransport = PHYTALLY_FORCE_ESPNOW
      ? PHYTALLY_TRANSPORT_ESPNOW
      : PHYTALLY_TRANSPORT_BEACON_PROBE;

  FastLED.addLeds<NEOPIXEL, PHYTALLY_NEOPIXEL_PIN>(leds, NUM_LEDS);
  leds[0] = CRGB::Black;
  FastLED.show();

  WiFi.mode(WIFI_STA);
  esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  configureLowBandwidthTelemetry();
  esp_wifi_start();
  WiFi.disconnect();

  esp_wifi_get_mac(WIFI_IF_STA, myMac);

#if PHYTALLY_ENABLE_ESPNOW
  initEspNow();
#endif

  Serial.printf(
      "\n--- PhyTally Receiver v1.2 ---\nID: %u\nMAC: %02X:%02X:%02X:%02X:%02X:%02X\nESPNOW_PHY_DEFAULT: %s\nREADY_FOR_CONFIG\n",
      (unsigned int)(rxState.tallyId + 1),
      myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5],
      espNowPhyModeName(rxState.espNowPhyMode));

#if !PHYTALLY_FORCE_ESPNOW
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscuousRxCb);
#endif
  esp_wifi_set_channel(rxState.currentChannel, WIFI_SECOND_CHAN_NONE);
}

void loop() {
  handleSerialConfig();
  applyPendingConfigIfNeeded();

  if (PHYTALLY_HAS_STATUS_LED && ledOffTime > 0 && millis() > ledOffTime) {
    digitalWrite(PHYTALLY_STATUS_LED_PIN, LOW);
    ledOffTime = 0;
  }

  static unsigned long lastTelem = 0;
  const unsigned long now = millis();
  const bool wasSignalLost = rxState.signalLostLogged;
  const receivercommon::SignalLossResult signalLoss = receivercommon::HandleSignalLoss(now, rxState);
  if (signalLoss.lost) {
    if (!wasSignalLost) {
      Serial.println("HUB LOST: entering channel scan");
    }
    if (signalLoss.channelChanged) {
      esp_wifi_set_channel(rxState.currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    if (signalLoss.scanLogDue) {
      Serial.printf("SCAN: CH=%u\n", (unsigned int)rxState.currentChannel);
    }
  }

  if (now - lastTelem >= receivercommon::kTelemetryIntervalMs) {
    lastTelem = now;
    sendTelemetry();
  }

  if (receivercommon::IsIdentifyActive(now, rxState)) {
    leds[0] = identifyColor(now);
  } else {
    switch (rxState.currentState) {
      case TALLY_OFF:
        leds[0] = CRGB::Black;
        break;
      case TALLY_PREVIEW:
        leds[0] = scaledColor(CRGB::Green);
        break;
      case TALLY_PROGRAM:
        leds[0] = scaledColor(CRGB::Red);
        break;
      case TALLY_ERROR:
      default:
        if (receivercommon::IsErrorBlinkOn(now)) {
          leds[0] = scaledColor(CRGB::Yellow);
        } else {
          leds[0] = CRGB::Black;
        }
        break;
    }
  }
  FastLED.show();
  delay(10);
}
