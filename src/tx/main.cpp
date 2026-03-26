#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncUDP.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <FastLED.h>
#if PHYTALLY_USE_USB_NCM_BACKHAUL
#include <WebServer.h>
#else
#include <ESPAsyncWebServer.h>
#endif
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#if PHYTALLY_USE_USB_NCM_BACKHAUL
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "lwip/tcpip.h"
}
#endif
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include "../../lib/TallyProtocol/TallyProtocol.h"
#include "../../lib/TallyTransport/TallyTransport.h"
#include "../../lib/UsbNcmNetif/UsbNcmNetif.h"

#ifndef PHYTALLY_ENABLE_ESPNOW
#define PHYTALLY_ENABLE_ESPNOW 1
#endif

#ifndef PHYTALLY_FORCE_ESPNOW
#define PHYTALLY_FORCE_ESPNOW 0
#endif

#ifndef PHYTALLY_USE_USB_NCM_BACKHAUL
#define PHYTALLY_USE_USB_NCM_BACKHAUL 0
#endif

#ifndef PHYTALLY_HAS_STATUS_LED
#define PHYTALLY_HAS_STATUS_LED 1
#endif

#ifndef PHYTALLY_TX_NEOPIXEL_BRIGHTNESS
#define PHYTALLY_TX_NEOPIXEL_BRIGHTNESS 51
#endif

// --- Configuration & Persistence ---
Preferences preferences;
char wifiSsid[33] = "";
char wifiPass[65] = "";
char atemIpStr[16] = "0.0.0.0";
char vmixIpStr[16] = "0.0.0.0";
char obsUrl[128] = "";
char obsPassword[65] = "";
char hostnameOverride[33] = "";
uint16_t tslListenPort = 9800;
bool atemEnabled = false;
bool vmixEnabled = false;
bool tslEnabled = false;
bool obsEnabled = false;

enum PhyTallyWirelessTransportMode : uint8_t {
    PHYTALLY_WIRELESS_TRANSPORT_AUTO = 0,
    PHYTALLY_WIRELESS_TRANSPORT_BEACON_PROBE = PHYTALLY_TRANSPORT_BEACON_PROBE,
    PHYTALLY_WIRELESS_TRANSPORT_ESPNOW = PHYTALLY_TRANSPORT_ESPNOW,
};

// --- Network State ---
#define STATUS_LED 2
const char *apSsid = "PhyTally-Config";
const char *apPassword = ""; 
const char *hubSsid = "PhyTally";
const char *hubPassword = "password123";
bool configMode = false;
bool telemetryRadioActive = false;
wifi_event_id_t wifiEventHandlerId = ARDUINO_EVENT_MAX;
unsigned long lastStaReconnectAttempt = 0;
unsigned long lastWifiSurveyAt = 0;
unsigned long wifiSurveyStartedAt = 0;
bool wifiSurveyInProgress = false;
bool wifiSurveyStartRequested = false;
uint8_t lastKnownStaChannel = TALLY_WIFI_CHANNEL;
String lastWifiSurveyJson = "{\"scanned_at_ms\":0,\"network_count\":0,\"scanning\":false,\"channel_suggestion\":null,\"networks\":[]}";
DNSServer dnsServer;
#if PHYTALLY_USE_USB_NCM_BACKHAUL
WebServer server(80);
#else
AsyncWebServer server(80);
#endif
volatile bool httpServerStarted = false;
bool httpServerStartQueued = false;
unsigned long usbHttpServerEarliestAt = 0;
bool mdnsStarted = false;
String mdnsHostLabel;
String uiIndexHtml;
String uiDashboardJs;
String uiDashboardCss;
#if PHYTALLY_USE_USB_NCM_BACKHAUL
TaskHandle_t usbNcmLoopTaskHandle = nullptr;
#endif

void startUsbHttpServer() {
    Serial.println("HTTP server starting on USB-NCM");
    server.begin();
    httpServerStarted = true;
    httpServerStartQueued = false;
    Serial.println("HTTP server ready on USB-NCM");
}

#if PHYTALLY_USE_USB_NCM_BACKHAUL
void usbNcmLoopTask(void *) {
    for (;;) {
        usbncm::loop();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

TallyState tallyStates[MAX_TALLIES];
AsyncUDP udp;
WiFiUDP tslUdp;

void updateNativeVendorIE();
void telemetry_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type);
#if ESP_IDF_VERSION_MAJOR >= 5
void espnow_rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
void espnow_rx_cb(const uint8_t *macAddr, const uint8_t *data, int len);
#endif
void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
String quoteJson(const String &value);
uint8_t nextCommandIdValue();

// --- ATEM State ---
ATEMstd atemSwitcher;
unsigned long lastAtemCheck = 0;

// --- vMix State ---
WiFiClient vmixClient;
unsigned long lastVmixRetry = 0;

// --- TSL State ---
unsigned long lastTslPacketAt = 0;

// --- OBS State ---
WiFiClient obsWsClient;
bool obsConnected = false;
String obsLastError;
unsigned long lastObsRetry = 0;
unsigned long lastObsPollAt = 0;
int obsNextRequestId = 0;

// --- Simulation State ---
bool testMode = false;
unsigned long lastTestStep = 0;
int testStep = 0;

#if defined(PHYTALLY_NEOPIXEL_PIN)
CRGB txIndicatorPixel[1];
bool txIndicatorPixelReady = false;
#endif

CRGB txSimulationColor(unsigned long now) {
    switch (tallyStates[0]) {
        case TALLY_PREVIEW:
            return CRGB(0, 64, 0);
        case TALLY_PROGRAM:
            return CRGB(96, 0, 0);
        case TALLY_ERROR:
            return ((now / 250) % 2) == 0 ? CRGB(96, 48, 0) : CRGB::Black;
        case TALLY_OFF:
        default:
            return CRGB::Black;
    }
}

void updateTxBoardIndicator() {
#if defined(PHYTALLY_NEOPIXEL_PIN)
    if (!txIndicatorPixelReady) {
        return;
    }

    txIndicatorPixel[0] = testMode ? txSimulationColor(millis()) : CRGB::Black;
    FastLED.show();
#endif
}

void handleSimulation() {
    if (!testMode) return;
    if (millis() - lastTestStep > 1000) {
        lastTestStep = millis();
        testStep++;
        int stateIdx = testStep % 3;
        TallyState state = TALLY_OFF;
        if (stateIdx == 1) state = TALLY_PREVIEW;
        else if (stateIdx == 2) state = TALLY_PROGRAM;
        for (int i = 0; i < MAX_TALLIES; i++) tallyStates[i] = state;
        updateNativeVendorIE();
        updateTxBoardIndicator();
    }
}

// --- Telemetry State ---
unsigned long ledOffTime = 0;

struct ReceiverStatus {
    uint8_t mac[6];
    uint8_t id;
    uint8_t battery;
    uint8_t brightnessPercent;
    int8_t rssi;
    unsigned long lastSeen;
    uint8_t lastTransport;
    bool online;
};
ReceiverStatus receivers[MAX_TALLIES * 2]; // Keep more if needed
int receiverCount = 0;

struct TelemetryDebugState {
    unsigned long packetCount;
    unsigned long lastSeen;
    int8_t lastAirRssi;
    int8_t lastReportedHubRssi;
    uint8_t lastReceiverId;
    uint8_t lastTransport;
    uint8_t lastReceiverMac[6];
    bool havePacket;
};
TelemetryDebugState telemetryDebug = {0, 0, -127, -127, 0, PHYTALLY_TRANSPORT_BEACON_PROBE, {0}, false};

// --- Command State ---
uint8_t cmdTargetMac[6] = {0};
uint8_t cmdType = 0;
uint8_t cmdParam = 0;
uint8_t cmdId = 0;
uint8_t nextCmdId = 1;
unsigned long cmdUntil = 0;
String lastCmdStatus = "None";
struct PendingHubCommand {
    uint8_t targetMac[6];
    uint8_t commandType;
    uint8_t commandParam;
};
constexpr size_t kPendingHubCommandMax = MAX_TALLIES * 2;
PendingHubCommand pendingHubCommands[kPendingHubCommandMax];
size_t pendingHubCommandCount = 0;
bool espNowPhySyncPending = false;
uint8_t espNowPhyPendingMode = PHYTALLY_ESPNOW_PHY_11B_1M;
size_t espNowPhySyncExpectedCount = 0;
size_t espNowPhySyncAckedCount = 0;
String espNowPhySyncFailureReason;

// --- Route State ---
uint8_t routePriority = PHYTALLY_FORCE_ESPNOW ? PHYTALLY_ROUTE_ESPNOW_FIRST : PHYTALLY_ROUTE_BEACON_FIRST;
uint8_t wirelessTransportMode = PHYTALLY_FORCE_ESPNOW ? PHYTALLY_WIRELESS_TRANSPORT_ESPNOW : PHYTALLY_WIRELESS_TRANSPORT_AUTO;
uint8_t activeTransport = PHYTALLY_FORCE_ESPNOW ? PHYTALLY_TRANSPORT_ESPNOW : PHYTALLY_TRANSPORT_BEACON_PROBE;
unsigned long lastTransportSwitchAt = 0;
unsigned long lastBeaconTelemetryAt = 0;
unsigned long lastEspNowTelemetryAt = 0;
uint16_t hubEspNowSequence = 1;
bool espNowReady = false;
uint8_t espNowPhyMode = PHYTALLY_ESPNOW_PHY_11B_1M;
const uint8_t kEspNowBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Vendor IE Structure ---
typedef struct {
    uint8_t element_id;      // 0xDD
    uint8_t length;          // sizeof(HubPayload)
    HubPayload payload;
} __attribute__((packed)) tally_vendor_ie_t;

tally_vendor_ie_t currentIE;

void formatMac(const uint8_t *mac, char *out, size_t outLen) {
    snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

String macToString(const uint8_t *mac) {
    char macStr[18];
    formatMac(mac, macStr, sizeof(macStr));
    return String(macStr);
}

String sanitizeHostnameLabel(const String &input) {
    String output;
    output.reserve(32);
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output += c;
        } else if (c == '-' || c == '_' || c == ' ') {
            if (output.length() == 0 || output[output.length() - 1] == '-') {
                continue;
            }
            output += '-';
        }
        if (output.length() >= 32) {
            break;
        }
    }
    while (output.endsWith("-")) {
        output.remove(output.length() - 1);
    }
    return output;
}

bool isValidEspNowPhyMode(uint8_t mode) {
    return mode >= PHYTALLY_ESPNOW_PHY_LR_250K && mode <= PHYTALLY_ESPNOW_PHY_11G_6M;
}

bool isCommandActive() {
    return cmdUntil > 0 && millis() < cmdUntil;
}

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

bool parseEspNowPhyMode(const String &value, uint8_t *modeOut) {
    if (!modeOut) {
        return false;
    }
    if (value == "lr_250k") {
        *modeOut = PHYTALLY_ESPNOW_PHY_LR_250K;
        return true;
    }
    if (value == "lr_500k") {
        *modeOut = PHYTALLY_ESPNOW_PHY_LR_500K;
        return true;
    }
    if (value == "11b_1m") {
        *modeOut = PHYTALLY_ESPNOW_PHY_11B_1M;
        return true;
    }
    if (value == "11b_2m") {
        *modeOut = PHYTALLY_ESPNOW_PHY_11B_2M;
        return true;
    }
    if (value == "11b_11m") {
        *modeOut = PHYTALLY_ESPNOW_PHY_11B_11M;
        return true;
    }
    if (value == "11g_6m") {
        *modeOut = PHYTALLY_ESPNOW_PHY_11G_6M;
        return true;
    }
    return false;
}

String supportedEspNowPhyModesJson() {
    return "[\"lr_250k\",\"lr_500k\",\"11b_1m\",\"11b_2m\",\"11b_11m\",\"11g_6m\"]";
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

String buildEspNowPhyJson() {
    return String("{\"mode\":") + quoteJson(String(espNowPhyModeName(espNowPhyMode))) +
        ",\"pending_mode\":" + (espNowPhySyncPending ? quoteJson(String(espNowPhyModeName(espNowPhyPendingMode))) : String("null")) +
        ",\"sync_pending\":" + String(espNowPhySyncPending ? "true" : "false") +
        ",\"queued_receiver_commands\":" + String(static_cast<unsigned>(pendingHubCommandCount)) +
        ",\"sync_expected_receivers\":" + String(static_cast<unsigned>(espNowPhySyncExpectedCount)) +
        ",\"sync_acked_receivers\":" + String(static_cast<unsigned>(espNowPhySyncAckedCount)) +
        ",\"supported_modes\":" + supportedEspNowPhyModesJson() +
        "}";
}

bool applyLocalEspNowPhyMode(uint8_t mode, const char *reason, bool persist, String *errorMessage) {
    if (!isValidEspNowPhyMode(mode)) {
        if (errorMessage) {
            *errorMessage = "Invalid ESP-NOW PHY mode.";
        }
        return false;
    }

    const uint8_t protocolBitmap = espNowProtocolBitmapForMode(mode);
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, protocolBitmap);
    if (err != ESP_OK) {
        if (errorMessage) {
            *errorMessage = String("Failed to set ESP-NOW protocol bitmap: err=") + String((int)err);
        }
        Serial.printf("ESPNOW PHY APPLY FAILED: protocol err=%d mode=%s reason=%s\n",
            (int)err, espNowPhyModeName(mode), reason ? reason : "unknown");
        return false;
    }

#if PHYTALLY_ENABLE_ESPNOW
    if (espNowReady) {
        err = esp_wifi_config_espnow_rate(WIFI_IF_STA, espNowPhyRateForMode(mode));
        if (err != ESP_OK) {
            if (errorMessage) {
                *errorMessage = String("Failed to set ESP-NOW rate: err=") + String((int)err);
            }
            Serial.printf("ESPNOW PHY APPLY FAILED: rate err=%d mode=%s reason=%s\n",
                (int)err, espNowPhyModeName(mode), reason ? reason : "unknown");
            return false;
        }
    }
#endif

    espNowPhyMode = mode;
    if (persist) {
        preferences.putUChar("espnowPhyMode", espNowPhyMode);
    }

    Serial.printf("ESPNOW PHY MODE: %s protocol=0x%02X reason=%s\n",
        espNowPhyModeName(espNowPhyMode),
        (unsigned int)protocolBitmap,
        reason ? reason : "unknown");
    return true;
}

String defaultHostnameLabel() {
    uint8_t baseMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, baseMac);
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%02x%02x%02x", baseMac[3], baseMac[4], baseMac[5]);
    return String("phytally-hub-") + suffix;
}

String configuredHostnameLabel() {
    String configured = sanitizeHostnameLabel(String(hostnameOverride));
    if (configured.length() == 0) {
        return defaultHostnameLabel();
    }
    return configured;
}

String currentMdnsHost() {
    return configuredHostnameLabel() + ".local";
}

bool parseMacAddress(const String &macStr, uint8_t output[6]);
String normalizeObsUrl(const String &raw);
bool hasConfiguredSwitcher();
void disableSimulationForLiveSwitcher(const char *reason);

String currentTslListenAddr() {
    return String(":") + String(tslListenPort);
}

bool hasConfiguredSwitcher() {
    return (atemEnabled && strlen(atemIpStr) > 0 && strcmp(atemIpStr, "0.0.0.0") != 0) ||
        (vmixEnabled && strlen(vmixIpStr) > 0 && strcmp(vmixIpStr, "0.0.0.0") != 0) ||
        tslEnabled ||
        (obsEnabled && normalizeObsUrl(String(obsUrl)).length() > 0);
}

void disableSimulationForLiveSwitcher(const char *reason) {
    if (!testMode || !hasConfiguredSwitcher()) {
        return;
    }

    testMode = false;
    memset(tallyStates, 0, sizeof(tallyStates));
    updateNativeVendorIE();
    updateTxBoardIndicator();
    Serial.printf("Simulation disabled: %s\n", reason ? reason : "live switcher configured");
}

bool parseListenPortValue(const String &value, uint16_t *portOut) {
    if (!portOut) {
        return false;
    }
    String trimmed = value;
    trimmed.trim();
    if (trimmed.length() == 0) {
        return false;
    }

    int colonIndex = trimmed.lastIndexOf(':');
    String portText = colonIndex >= 0 ? trimmed.substring(colonIndex + 1) : trimmed;
    portText.trim();
    if (portText.length() == 0) {
        return false;
    }

    for (size_t i = 0; i < portText.length(); i++) {
        if (portText[i] < '0' || portText[i] > '9') {
            return false;
        }
    }

    long parsed = portText.toInt();
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }

    *portOut = static_cast<uint16_t>(parsed);
    return true;
}

String normalizeObsUrl(const String &raw) {
    String value = raw;
    value.trim();
    if (value.length() == 0) {
        return String();
    }
    if (value.indexOf("://") >= 0) {
        return value;
    }
    return String("ws://") + value;
}

bool parseObsEndpoint(const String &raw, String *hostOut, uint16_t *portOut, String *pathOut, String *errorMessage) {
    if (!hostOut || !portOut || !pathOut) {
        return false;
    }

    String normalized = normalizeObsUrl(raw);
    if (normalized.length() == 0) {
        if (errorMessage) *errorMessage = "Empty OBS URL.";
        return false;
    }

    int schemeSep = normalized.indexOf("://");
    if (schemeSep < 0) {
        if (errorMessage) *errorMessage = "Invalid OBS URL.";
        return false;
    }

    String scheme = normalized.substring(0, schemeSep);
    scheme.toLowerCase();
    if (scheme != "ws") {
        if (errorMessage) *errorMessage = "Only ws:// OBS URLs are supported on ESP TX.";
        return false;
    }

    String remainder = normalized.substring(schemeSep + 3);
    int slash = remainder.indexOf('/');
    String hostPort = slash >= 0 ? remainder.substring(0, slash) : remainder;
    String path = slash >= 0 ? remainder.substring(slash) : String("/");
    if (hostPort.length() == 0) {
        if (errorMessage) *errorMessage = "OBS URL is missing host.";
        return false;
    }
    if (hostPort.indexOf('[') >= 0 || hostPort.indexOf(']') >= 0) {
        if (errorMessage) *errorMessage = "IPv6 OBS URLs are not supported on ESP TX.";
        return false;
    }

    int colon = hostPort.lastIndexOf(':');
    String host = colon > 0 ? hostPort.substring(0, colon) : hostPort;
    uint16_t port = 4455;
    if (colon > 0) {
        String portText = hostPort.substring(colon + 1);
        uint16_t parsedPort = 0;
        if (!parseListenPortValue(portText, &parsedPort)) {
            if (errorMessage) *errorMessage = "OBS URL has an invalid port.";
            return false;
        }
        port = parsedPort;
    }

    host.trim();
    if (host.length() == 0) {
        if (errorMessage) *errorMessage = "OBS URL is missing host.";
        return false;
    }

    *hostOut = host;
    *portOut = port;
    *pathOut = path.length() > 0 ? path : String("/");
    return true;
}

String sha256Base64(const String &text) {
    uint8_t digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(text.c_str()), text.length(), digest, 0);
    return base64::encode(digest, sizeof(digest));
}

String websocketAcceptValue(const String &key) {
    static const char *kWebsocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    String source = key + kWebsocketGuid;
    uint8_t digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char *>(source.c_str()), source.length(), digest);
    return base64::encode(digest, sizeof(digest));
}

String obsAuthResponse(const String &password, const String &salt, const String &challenge) {
    return sha256Base64(sha256Base64(password + salt) + challenge);
}

void setObsDisconnected(const String &errorMessage) {
    if (obsWsClient.connected()) {
        obsWsClient.stop();
    }
    bool wasConnected = obsConnected;
    obsConnected = false;
    if (errorMessage.length() > 0) {
        obsLastError = errorMessage;
    }
    if (wasConnected || errorMessage.length() > 0) {
        Serial.println("OBS: " + (errorMessage.length() > 0 ? errorMessage : String("disconnected")));
    }
}

bool writeObsBytes(const uint8_t *data, size_t length, String *errorMessage) {
    size_t offset = 0;
    while (offset < length) {
        size_t written = obsWsClient.write(data + offset, length - offset);
        if (written == 0) {
            if (errorMessage) *errorMessage = "OBS socket write failed.";
            return false;
        }
        offset += written;
    }
    return true;
}

bool readObsBytes(uint8_t *data, size_t length, String *errorMessage) {
    size_t offset = 0;
    unsigned long startedAt = millis();
    while (offset < length) {
        int available = obsWsClient.available();
        if (available <= 0) {
            if (!obsWsClient.connected()) {
                if (errorMessage) *errorMessage = "OBS socket closed.";
                return false;
            }
            if (millis() - startedAt > 5000) {
                if (errorMessage) *errorMessage = "OBS socket read timed out.";
                return false;
            }
            delay(1);
            continue;
        }
        int chunk = obsWsClient.read(data + offset, length - offset);
        if (chunk <= 0) {
            if (errorMessage) *errorMessage = "OBS socket read failed.";
            return false;
        }
        offset += static_cast<size_t>(chunk);
    }
    return true;
}

bool sendObsFrame(uint8_t opcode, const uint8_t *payload, size_t payloadLen, String *errorMessage) {
    uint8_t header[14];
    size_t headerLen = 0;
    header[headerLen++] = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
    if (payloadLen < 126) {
        header[headerLen++] = static_cast<uint8_t>(0x80 | payloadLen);
    } else if (payloadLen <= 0xFFFF) {
        header[headerLen++] = 0x80 | 126;
        header[headerLen++] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
        header[headerLen++] = static_cast<uint8_t>(payloadLen & 0xFF);
    } else {
        header[headerLen++] = 0x80 | 127;
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[headerLen++] = static_cast<uint8_t>((static_cast<uint64_t>(payloadLen) >> shift) & 0xFF);
        }
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(header + headerLen, mask, sizeof(mask));
    headerLen += sizeof(mask);

    if (!writeObsBytes(header, headerLen, errorMessage)) {
        return false;
    }

    if (payloadLen == 0) {
        return true;
    }

    uint8_t *masked = static_cast<uint8_t *>(malloc(payloadLen));
    if (!masked) {
        if (errorMessage) *errorMessage = "OBS frame allocation failed.";
        return false;
    }
    for (size_t index = 0; index < payloadLen; index++) {
        masked[index] = payload[index] ^ mask[index % sizeof(mask)];
    }
    bool ok = writeObsBytes(masked, payloadLen, errorMessage);
    free(masked);
    return ok;
}

bool sendObsJson(const JsonDocument &doc, String *errorMessage) {
    String payload;
    serializeJson(doc, payload);
    return sendObsFrame(0x1, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length(), errorMessage);
}

bool readObsJson(JsonDocument &doc, String *errorMessage) {
    for (;;) {
        uint8_t header[2];
        if (!readObsBytes(header, sizeof(header), errorMessage)) {
            return false;
        }

        uint8_t opcode = static_cast<uint8_t>(header[0] & 0x0F);
        uint64_t payloadLen = static_cast<uint64_t>(header[1] & 0x7F);
        bool masked = (header[1] & 0x80) != 0;

        if (payloadLen == 126) {
            uint8_t ext[2];
            if (!readObsBytes(ext, sizeof(ext), errorMessage)) {
                return false;
            }
            payloadLen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (payloadLen == 127) {
            uint8_t ext[8];
            if (!readObsBytes(ext, sizeof(ext), errorMessage)) {
                return false;
            }
            payloadLen = 0;
            for (size_t i = 0; i < sizeof(ext); i++) {
                payloadLen = (payloadLen << 8) | ext[i];
            }
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !readObsBytes(mask, sizeof(mask), errorMessage)) {
            return false;
        }

        if (payloadLen > 65535) {
            if (errorMessage) *errorMessage = "OBS websocket frame too large.";
            return false;
        }

        char *payload = static_cast<char *>(malloc(static_cast<size_t>(payloadLen) + 1));
        if (!payload) {
            if (errorMessage) *errorMessage = "OBS payload allocation failed.";
            return false;
        }
        if (payloadLen > 0 && !readObsBytes(reinterpret_cast<uint8_t *>(payload), static_cast<size_t>(payloadLen), errorMessage)) {
            free(payload);
            return false;
        }
        for (size_t index = 0; index < payloadLen; index++) {
            if (masked) {
                payload[index] ^= mask[index % sizeof(mask)];
            }
        }
        payload[payloadLen] = '\0';

        if (opcode == 0x9) {
            bool pongOk = sendObsFrame(0xA, reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(payloadLen), errorMessage);
            free(payload);
            if (!pongOk) {
                return false;
            }
            continue;
        }
        if (opcode == 0xA) {
            free(payload);
            continue;
        }
        if (opcode == 0x8) {
            free(payload);
            if (errorMessage) *errorMessage = "OBS websocket closed.";
            return false;
        }
        if (opcode != 0x1) {
            free(payload);
            continue;
        }

        DeserializationError error = deserializeJson(doc, payload);
        free(payload);
        if (error) {
            if (errorMessage) *errorMessage = "Invalid OBS JSON payload.";
            return false;
        }
        return true;
    }
}

bool obsRequest(const char *requestType, JsonDocument &responseDoc, String *errorMessage) {
    obsNextRequestId++;
    String requestId = String(obsNextRequestId);

    JsonDocument requestDoc;
    requestDoc["op"] = 6;
    requestDoc["d"]["requestType"] = requestType;
    requestDoc["d"]["requestId"] = requestId;
    if (!sendObsJson(requestDoc, errorMessage)) {
        return false;
    }

    unsigned long startedAt = millis();
    while (millis() - startedAt < 5000) {
        JsonDocument messageDoc;
        if (!readObsJson(messageDoc, errorMessage)) {
            return false;
        }
        if ((messageDoc["op"] | -1) != 7) {
            continue;
        }
        if (String(messageDoc["d"]["requestId"] | "") != requestId) {
            continue;
        }
        bool ok = messageDoc["d"]["requestStatus"]["result"] | false;
        if (!ok) {
            String comment = messageDoc["d"]["requestStatus"]["comment"] | "request failed";
            if (errorMessage) *errorMessage = String("OBS ") + requestType + ": " + comment;
            return false;
        }
        responseDoc.clear();
        responseDoc.set(messageDoc["d"]["responseData"]);
        return true;
    }

    if (errorMessage) *errorMessage = String("OBS ") + requestType + " timed out.";
    return false;
}

bool connectObs() {
    String host;
    String path;
    uint16_t port = 0;
    String errorMessage;
    if (!parseObsEndpoint(String(obsUrl), &host, &port, &path, &errorMessage)) {
        setObsDisconnected(errorMessage);
        return false;
    }

    obsWsClient.stop();
    obsWsClient.setTimeout(5000);
    if (!obsWsClient.connect(host.c_str(), port)) {
        setObsDisconnected(String("OBS connect failed: ") + host + ":" + String(port));
        return false;
    }

    uint8_t keyBytes[16];
    esp_fill_random(keyBytes, sizeof(keyBytes));
    String key = base64::encode(keyBytes, sizeof(keyBytes));
    String request = String("GET ") + path + " HTTP/1.1\r\n" +
        "Host: " + host + ":" + String(port) + "\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Version: 13\r\n" +
        "Sec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!writeObsBytes(reinterpret_cast<const uint8_t *>(request.c_str()), request.length(), &errorMessage)) {
        setObsDisconnected(errorMessage);
        return false;
    }

    String statusLine = obsWsClient.readStringUntil('\n');
    statusLine.trim();
    if (statusLine.indexOf("101") < 0) {
        setObsDisconnected(String("OBS websocket upgrade failed: ") + statusLine);
        return false;
    }

    bool upgradeOk = false;
    bool acceptOk = false;
    while (obsWsClient.connected()) {
        String header = obsWsClient.readStringUntil('\n');
        header.trim();
        if (header.length() == 0) {
            break;
        }
        int colon = header.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        String name = header.substring(0, colon);
        String value = header.substring(colon + 1);
        name.trim();
        value.trim();
        String normalizedValue = value;
        name.toLowerCase();
        normalizedValue.toLowerCase();
        if (name == "upgrade" && normalizedValue == "websocket") {
            upgradeOk = true;
        }
        if (name == "sec-websocket-accept" && value == websocketAcceptValue(key)) {
            acceptOk = true;
        }
    }
    if (!upgradeOk || !acceptOk) {
        setObsDisconnected("OBS websocket upgrade headers were invalid.");
        return false;
    }

    JsonDocument helloDoc;
    if (!readObsJson(helloDoc, &errorMessage)) {
        setObsDisconnected(errorMessage);
        return false;
    }
    if ((helloDoc["op"] | -1) != 0) {
        setObsDisconnected(String("Unexpected OBS hello opcode: ") + String(helloDoc["op"] | -1));
        return false;
    }

    JsonDocument identifyDoc;
    identifyDoc["op"] = 1;
    identifyDoc["d"]["rpcVersion"] = 1;
    identifyDoc["d"]["eventSubscriptions"] = 0;
    const char *salt = helloDoc["d"]["authentication"]["salt"] | "";
    const char *challenge = helloDoc["d"]["authentication"]["challenge"] | "";
    if (strlen(salt) > 0 && strlen(challenge) > 0) {
        identifyDoc["d"]["authentication"] = obsAuthResponse(String(obsPassword), String(salt), String(challenge));
    }
    if (!sendObsJson(identifyDoc, &errorMessage)) {
        setObsDisconnected(errorMessage);
        return false;
    }

    JsonDocument identifiedDoc;
    if (!readObsJson(identifiedDoc, &errorMessage)) {
        setObsDisconnected(errorMessage);
        return false;
    }
    if ((identifiedDoc["op"] | -1) != 2) {
        setObsDisconnected(String("Unexpected OBS identify opcode: ") + String(identifiedDoc["op"] | -1));
        return false;
    }

    obsConnected = true;
    obsLastError = "";
    obsNextRequestId = 0;
    lastObsPollAt = 0;
    Serial.println("OBS connected: " + normalizeObsUrl(String(obsUrl)));
    return true;
}

int obsSceneUuidToTallyId(JsonArray scenes, const String &sceneUuid) {
    if (sceneUuid.length() == 0) {
        return 0;
    }

    int validSceneCount = 0;
    for (JsonVariant sceneVariant : scenes) {
        JsonObject scene = sceneVariant.as<JsonObject>();
        if (!scene.isNull() && String(scene["sceneUuid"] | "").length() > 0) {
            validSceneCount++;
        }
    }
    if (validSceneCount <= 0) {
        return 0;
    }

    int fallbackIndex = 0;
    for (JsonVariant sceneVariant : scenes) {
        JsonObject scene = sceneVariant.as<JsonObject>();
        String uuid = scene["sceneUuid"] | "";
        if (uuid.length() == 0) {
            continue;
        }
        int sceneIndex = scene["sceneIndex"].is<int>() ? scene["sceneIndex"].as<int>() : fallbackIndex;
        fallbackIndex++;
        if (uuid != sceneUuid) {
            continue;
        }
        if (sceneIndex < 0 || sceneIndex >= validSceneCount) {
            sceneIndex = fallbackIndex - 1;
        }
        int reverseIndex = validSceneCount - 1 - sceneIndex;
        if (reverseIndex >= 0 && reverseIndex < MAX_TALLIES) {
            return reverseIndex + 1;
        }
        return 0;
    }

    return 0;
}

bool pollObsTallies(String *errorMessage) {
    JsonDocument responseDoc;
    if (!obsRequest("GetSceneList", responseDoc, errorMessage)) {
        return false;
    }

    JsonArray scenes = responseDoc["scenes"].as<JsonArray>();
    if (scenes.isNull()) {
        if (errorMessage) *errorMessage = "OBS GetSceneList response missing scenes.";
        return false;
    }

    String programSceneUuid = responseDoc["currentProgramSceneUuid"] | "";
    if (programSceneUuid.length() == 0) {
        if (errorMessage) *errorMessage = "OBS GetSceneList response missing currentProgramSceneUuid.";
        return false;
    }
    String previewSceneUuid = responseDoc["currentPreviewSceneUuid"] | "";

    TallyState nextTallies[MAX_TALLIES];
    memset(nextTallies, 0, sizeof(nextTallies));

    int programTallyId = obsSceneUuidToTallyId(scenes, programSceneUuid);
    if (programTallyId >= 1 && programTallyId <= MAX_TALLIES) {
        nextTallies[programTallyId - 1] = TALLY_PROGRAM;
    }
    int previewTallyId = obsSceneUuidToTallyId(scenes, previewSceneUuid);
    if (previewTallyId >= 1 && previewTallyId <= MAX_TALLIES && nextTallies[previewTallyId - 1] != TALLY_PROGRAM) {
        nextTallies[previewTallyId - 1] = TALLY_PREVIEW;
    }

    bool changed = false;
    for (int index = 0; index < MAX_TALLIES; index++) {
        if (tallyStates[index] != nextTallies[index]) {
            tallyStates[index] = nextTallies[index];
            changed = true;
        }
    }
    if (changed) {
        updateNativeVendorIE();
    }

    return true;
}

void handleObs() {
    if (!obsEnabled || configMode) {
        if (obsConnected || obsWsClient.connected()) {
            setObsDisconnected(String());
        }
        return;
    }

    String normalizedUrl = normalizeObsUrl(String(obsUrl));
    if (normalizedUrl.length() == 0) {
        setObsDisconnected("OBS enabled but URL is empty.");
        return;
    }

    if (!obsWsClient.connected() || !obsConnected) {
        if (millis() - lastObsRetry < 3000) {
            return;
        }
        lastObsRetry = millis();
        connectObs();
        return;
    }

    if (millis() - lastObsPollAt < 500) {
        return;
    }
    lastObsPollAt = millis();

    String errorMessage;
    if (!pollObsTallies(&errorMessage)) {
        setObsDisconnected(errorMessage);
    }
}

bool parseBrightnessPayload(const JsonDocument &doc, uint8_t *rawBrightness, uint8_t *brightnessPercent) {
    if (!rawBrightness || !brightnessPercent) {
        return false;
    }

    if (!doc["brightness_percent"].isNull()) {
        int percent = doc["brightness_percent"] | -1;
        if (percent < 0 || percent > 100) {
            return false;
        }
        *brightnessPercent = static_cast<uint8_t>(percent);
        *rawBrightness = static_cast<uint8_t>((percent * 255 + 50) / 100);
        return true;
    }

    if (!doc["brightness"].isNull()) {
        int raw = doc["brightness"] | -1;
        if (raw < 0 || raw > 255) {
            return false;
        }
        *rawBrightness = static_cast<uint8_t>(raw);
        *brightnessPercent = static_cast<uint8_t>((raw * 100 + 127) / 255);
        return true;
    }

    return false;
}

void rememberReceiverBrightnessPercent(const String &macStr, uint8_t brightnessPercent) {
    uint8_t mac[6];
    if (!parseMacAddress(macStr, mac)) {
        return;
    }
    for (int i = 0; i < receiverCount; i++) {
        if (memcmp(receivers[i].mac, mac, sizeof(mac)) == 0) {
            receivers[i].brightnessPercent = brightnessPercent;
            return;
        }
    }
}

bool usesUsbNcmBackhaul() {
    return PHYTALLY_USE_USB_NCM_BACKHAUL != 0;
}

bool isBeaconTransportActive() {
    return activeTransport == PHYTALLY_TRANSPORT_BEACON_PROBE;
}

bool isEspNowTransportActive() {
    return activeTransport == PHYTALLY_TRANSPORT_ESPNOW;
}

bool canSwitchTransportNow() {
    return lastTransportSwitchAt == 0 ||
        millis() - lastTransportSwitchAt >= PHYTALLY_ROUTE_SWITCH_HOLDDOWN_MS;
}

uint8_t preferredTransport() {
    return PhyTallyPreferredTransport(routePriority);
}

uint8_t fallbackTransport() {
    return PhyTallyFallbackTransport(routePriority);
}

const char *wirelessTransportModeName(uint8_t mode) {
    switch (mode) {
        case PHYTALLY_WIRELESS_TRANSPORT_BEACON_PROBE:
            return "beacon_probe";
        case PHYTALLY_WIRELESS_TRANSPORT_ESPNOW:
            return "espnow";
        case PHYTALLY_WIRELESS_TRANSPORT_AUTO:
        default:
            return "auto";
    }
}

bool parseWirelessTransportMode(const String &value, uint8_t *modeOut) {
    if (value == "auto") {
        *modeOut = PHYTALLY_WIRELESS_TRANSPORT_AUTO;
        return true;
    }
    if (value == "beacon_probe") {
        *modeOut = PHYTALLY_WIRELESS_TRANSPORT_BEACON_PROBE;
        return true;
    }
    if (value == "espnow") {
        *modeOut = PHYTALLY_WIRELESS_TRANSPORT_ESPNOW;
        return true;
    }
    return false;
}

String supportedWirelessTransportsJson() {
    if (PHYTALLY_FORCE_ESPNOW) {
        return "[\"espnow\"]";
    }
    return "[\"auto\",\"beacon_probe\",\"espnow\"]";
}

uint8_t desiredTransportForMode(uint8_t mode) {
    if (PHYTALLY_FORCE_ESPNOW) {
        return PHYTALLY_TRANSPORT_ESPNOW;
    }
    switch (mode) {
        case PHYTALLY_WIRELESS_TRANSPORT_BEACON_PROBE:
            return PHYTALLY_TRANSPORT_BEACON_PROBE;
        case PHYTALLY_WIRELESS_TRANSPORT_ESPNOW:
            return PHYTALLY_TRANSPORT_ESPNOW;
        case PHYTALLY_WIRELESS_TRANSPORT_AUTO:
        default:
            return preferredTransport();
    }
}

String buildWirelessTransportJson() {
    return String("{\"transport\":") + quoteJson(String(wirelessTransportModeName(wirelessTransportMode))) +
        ",\"active_transport\":" + quoteJson(String(PhyTallyTransportName(activeTransport))) +
        ",\"route_priority\":" + quoteJson(String(routePriority == PHYTALLY_ROUTE_ESPNOW_FIRST ? "espnow_first" : "beacon_first")) +
        ",\"supported_transports\":" + supportedWirelessTransportsJson() +
        ",\"espnow_phy_mode\":" + quoteJson(String(espNowPhyModeName(espNowPhyMode))) +
        ",\"espnow_phy_pending_mode\":" + (espNowPhySyncPending ? quoteJson(String(espNowPhyModeName(espNowPhyPendingMode))) : String("null")) +
        ",\"espnow_phy_sync_pending\":" + String(espNowPhySyncPending ? "true" : "false") +
        ",\"espnow_phy_queued_receiver_commands\":" + String(static_cast<unsigned>(pendingHubCommandCount)) +
        ",\"espnow_phy_sync_expected_receivers\":" + String(static_cast<unsigned>(espNowPhySyncExpectedCount)) +
        ",\"espnow_phy_sync_acked_receivers\":" + String(static_cast<unsigned>(espNowPhySyncAckedCount)) +
        ",\"supported_espnow_phy_modes\":" + supportedEspNowPhyModesJson() +
        "}";
}

void clearActiveCommand(const char *reason) {
    cmdType = 0;
    cmdParam = 0;
    cmdId = 0;
    cmdUntil = 0;
    memset(cmdTargetMac, 0, sizeof(cmdTargetMac));
    lastCmdStatus = reason;
}

void clearPendingCommandsOfType(uint8_t commandType) {
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < pendingHubCommandCount; readIndex++) {
        const PendingHubCommand &pending = pendingHubCommands[readIndex];
        if (pending.commandType == commandType) {
            continue;
        }
        if (writeIndex != readIndex) {
            pendingHubCommands[writeIndex] = pending;
        }
        writeIndex++;
    }
    pendingHubCommandCount = writeIndex;
}

void failEspNowPhySync(const String &reason) {
    espNowPhySyncFailureReason = reason;
    clearPendingCommandsOfType(HUB_CMD_SET_ESPNOW_PHY);
}

String pendingCommandLabel(uint8_t commandType, uint8_t commandParam) {
    switch (commandType) {
        case HUB_CMD_SET_ID:
            return "SET_ID=" + String(commandParam + 1);
        case HUB_CMD_SET_BRIGHTNESS:
            return "SET_BRIGHTNESS=" + String(commandParam);
        case HUB_CMD_IDENTIFY:
            return "IDENTIFY";
        case HUB_CMD_SET_ESPNOW_PHY:
            return "SET_ESPNOW_PHY=" + String(espNowPhyModeName(commandParam));
        case HUB_CMD_REBOOT:
            return "REBOOT";
        case HUB_CMD_NONE:
        default:
            return "COMMAND";
    }
}

bool enqueuePendingHubCommand(const uint8_t targetMac[6], uint8_t commandType, uint8_t commandParam) {
    if (!targetMac || pendingHubCommandCount >= kPendingHubCommandMax) {
        return false;
    }
    PendingHubCommand &slot = pendingHubCommands[pendingHubCommandCount++];
    memcpy(slot.targetMac, targetMac, sizeof(slot.targetMac));
    slot.commandType = commandType;
    slot.commandParam = commandParam;
    return true;
}

void activatePendingHubCommand(const PendingHubCommand &pending) {
    memcpy(cmdTargetMac, pending.targetMac, sizeof(cmdTargetMac));
    cmdType = pending.commandType;
    cmdParam = pending.commandParam;
    cmdId = nextCommandIdValue();
    cmdUntil = millis() + 10000;
    lastCmdStatus = "Active: " + pendingCommandLabel(cmdType, cmdParam) + " for " + macToString(cmdTargetMac);
    updateNativeVendorIE();
    Serial.println(lastCmdStatus);
}

void pumpPendingHubCommands() {
    if (isCommandActive()) {
        return;
    }

    if (espNowPhySyncPending && espNowPhySyncFailureReason.length() > 0) {
        lastCmdStatus = "ESP-NOW PHY sync failed: " + espNowPhySyncFailureReason;
        espNowPhySyncPending = false;
        espNowPhySyncExpectedCount = 0;
        espNowPhySyncAckedCount = 0;
        espNowPhySyncFailureReason = "";
        Serial.println(lastCmdStatus);
        return;
    }

    if (pendingHubCommandCount > 0) {
        PendingHubCommand pending = pendingHubCommands[0];
        if (pendingHubCommandCount > 1) {
            memmove(&pendingHubCommands[0], &pendingHubCommands[1], sizeof(PendingHubCommand) * (pendingHubCommandCount - 1));
        }
        pendingHubCommandCount--;
        activatePendingHubCommand(pending);
        return;
    }

    if (espNowPhySyncPending &&
        espNowPhySyncExpectedCount > 0 &&
        espNowPhySyncAckedCount >= espNowPhySyncExpectedCount) {
        String errorMessage;
        if (applyLocalEspNowPhyMode(espNowPhyPendingMode, "receiver_sync_complete", true, &errorMessage)) {
            lastCmdStatus = "ESP-NOW PHY sync complete: " + String(espNowPhyModeName(espNowPhyPendingMode));
            espNowPhySyncPending = false;
            espNowPhySyncExpectedCount = 0;
            espNowPhySyncAckedCount = 0;
        } else {
            lastCmdStatus = "ESP-NOW PHY sync failed: " + errorMessage;
            espNowPhySyncPending = false;
            espNowPhySyncExpectedCount = 0;
            espNowPhySyncAckedCount = 0;
            Serial.println(lastCmdStatus);
        }
    }
}

uint8_t nextCommandIdValue() {
    if (nextCmdId == 0) {
        nextCmdId = 1;
    }
    return nextCmdId++;
}

String jsonEscape(const String &value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        switch (c) {
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

String quoteJson(const String &value) {
    return "\"" + jsonEscape(value) + "\"";
}

const char *boolToJson(bool value) {
    return value ? "true" : "false";
}

String loadUiAsset(const char *path) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.printf("UI asset load failed: %s\n", path);
        return String();
    }

    String content;
    content.reserve(file.size() + 1);
    while (file.available()) {
        content += static_cast<char>(file.read());
    }
    file.close();
    Serial.printf("UI asset loaded: %s (%u bytes)\n", path, static_cast<unsigned>(content.length()));
    return content;
}

void inlineUiAssets() {
#if PHYTALLY_USE_USB_NCM_BACKHAUL
    return;
#endif
    if (uiIndexHtml.length() == 0 || uiDashboardCss.length() == 0 || uiDashboardJs.length() == 0) {
        return;
    }

    uiIndexHtml.replace(
        "<link rel=\"stylesheet\" href=\"/dashboard.css\">",
        String("<style>\n") + uiDashboardCss + "\n</style>");
    uiIndexHtml.replace(
        "<script src=\"/dashboard.js\"></script>",
        String("<script>\n") + uiDashboardJs + "\n</script>");
    Serial.printf("UI assets inlined into index.html (%u bytes)\n", static_cast<unsigned>(uiIndexHtml.length()));
}

int clampRssiScore(int rssi) {
    if (rssi < -95) rssi = -95;
    if (rssi > -30) rssi = -30;
    return rssi + 96; // -95 => 1, -30 => 66
}

int channelOverlapWeight(int observedChannel, int candidateChannel) {
    int distance = abs(observedChannel - candidateChannel);
    switch (distance) {
        case 0: return 100;
        case 1: return 55;
        case 2: return 25;
        case 3: return 10;
        case 4: return 3;
        default: return 0;
    }
}

String buildChannelSuggestionJson(int count) {
    int channelScores[13] = {0};
    int configuredChannel = 0;
    int configuredRssi = -127;

    for (int i = 0; i < count; i++) {
        int observedChannel = WiFi.channel(i);
        if (observedChannel < 1 || observedChannel > 13) continue;

        int rssiScore = clampRssiScore(WiFi.RSSI(i));
        for (int candidate = 1; candidate <= 13; candidate++) {
            channelScores[candidate - 1] += rssiScore * channelOverlapWeight(observedChannel, candidate);
        }

        String observedSsid = WiFi.SSID(i);
        if (strlen(wifiSsid) > 0 && observedSsid == String(wifiSsid) && WiFi.RSSI(i) > configuredRssi) {
            configuredRssi = WiFi.RSSI(i);
            configuredChannel = observedChannel;
        }
    }

    const uint8_t preferredChannels[] = {1, 6, 11};
    int recommendedChannel = preferredChannels[0];
    int recommendedScore = channelScores[recommendedChannel - 1];
    for (uint8_t channel : preferredChannels) {
        int score = channelScores[channel - 1];
        if (score < recommendedScore) {
            recommendedChannel = channel;
            recommendedScore = score;
        }
    }

    int topChannels[3] = {1, 6, 11};
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (channelScores[topChannels[j] - 1] < channelScores[topChannels[i] - 1]) {
                int tmp = topChannels[i];
                topChannels[i] = topChannels[j];
                topChannels[j] = tmp;
            }
        }
    }

    String json = "{";
    json += "\"recommended_channel\":" + String(recommendedChannel) + ",";
    json += "\"recommended_score\":" + String(recommendedScore) + ",";
    json += "\"configured_ssid\":" + quoteJson(String(wifiSsid)) + ",";
    json += "\"configured_ssid_channel\":";
    if (configuredChannel > 0) {
        json += String(configuredChannel);
    } else {
        json += "null";
    }
    json += ",";
    json += "\"top_non_overlapping\":[";
    for (int i = 0; i < 3; i++) {
        if (i > 0) json += ",";
        int channel = topChannels[i];
        json += String("{\"channel\":") + String(channel) +
            ",\"score\":" + String(channelScores[channel - 1]) + "}";
    }
    json += "],";
    json += "\"channel_scores\":[";
    for (int channel = 1; channel <= 13; channel++) {
        if (channel > 1) json += ",";
        json += String("{\"channel\":") + String(channel) +
            ",\"score\":" + String(channelScores[channel - 1]) + "}";
    }
    json += "]}";
    return json;
}

uint8_t getCurrentRadioChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &secondary) != ESP_OK) {
        return 0;
    }
    return primary;
}

void logHubRadioState(const char *label) {
    uint8_t apMac[6];
    char macStr[18];
    esp_wifi_get_mac(WIFI_IF_AP, apMac);
    formatMac(apMac, macStr, sizeof(macStr));
    Serial.printf("%s SSID=%s AP_MAC=%s CH=%u\n",
        label,
        configMode ? apSsid : hubSsid,
        macStr,
        getCurrentRadioChannel());
}

void startTelemetryRadio() {
    if (telemetryRadioActive) return;
    esp_wifi_set_promiscuous_rx_cb(&telemetry_rx_cb);
    esp_wifi_set_promiscuous(true);
    telemetryRadioActive = true;
}

void stopTelemetryRadio() {
    if (!telemetryRadioActive) return;
    esp_wifi_set_promiscuous(false);
    telemetryRadioActive = false;
}

bool connectToInfrastructureWiFi(unsigned long timeoutMs) {
    if (strlen(wifiSsid) == 0) return false;

    Serial.printf("Connecting to Wi-Fi: %s ", wifiSsid);
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.begin(wifiSsid, wifiPass);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    lastStaReconnectAttempt = 0;
    if (WiFi.channel() >= 1 && WiFi.channel() <= 13) {
        lastKnownStaChannel = WiFi.channel();
    }
    Serial.println("Wi-Fi CONNECTED!");
    Serial.println("IP: " + WiFi.localIP().toString());
    return true;
}

uint8_t getPreferredApChannel() {
    uint8_t channel = WiFi.channel();
    if (channel >= 1 && channel <= 13) {
        lastKnownStaChannel = channel;
        return channel;
    }
    return lastKnownStaChannel;
}

void startHubApOnCurrentStaChannel() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    configMode = false;
    dnsServer.stop();

    int channel = getPreferredApChannel();
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.softAP(hubSsid, hubPassword, channel, 1, 1);
    updateNativeVendorIE();
    startTelemetryRadio();

    Serial.printf("Hub beacon interface on STA Channel %d (hidden, no clients)\n", channel);
    logHubRadioState("Hub Beacon Ready:");
}

bool startHubApOnChannel(uint8_t channel) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    configMode = false;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    delay(100);
    if (!WiFi.softAP(hubSsid, hubPassword, channel, 1, 1)) {
        return false;
    }
    updateNativeVendorIE();
    startTelemetryRadio();
    Serial.printf("Hub beacon interface moved to Channel %u (hidden, no clients)\n", static_cast<unsigned>(channel));
    logHubRadioState("Hub Beacon Ready:");
    return true;
}

void startConfigPortal() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    configMode = true;
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.softAP(apSsid, apPassword);
    dnsServer.start(53, "*", WiFi.softAPIP());
    updateNativeVendorIE();
    startTelemetryRadio();
    logHubRadioState("Config AP Ready:");
}

bool startConfigPortalOnChannel(uint8_t channel) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    configMode = true;
    WiFi.softAPdisconnect(true);
    delay(100);
    if (!WiFi.softAP(apSsid, apPassword, channel)) {
        return false;
    }
    dnsServer.start(53, "*", WiFi.softAPIP());
    updateNativeVendorIE();
    startTelemetryRadio();
    logHubRadioState("Config AP Ready:");
    return true;
}

bool applyRadioChannel(uint8_t channel, bool persist, String *errorMessage) {
    if (channel < 1 || channel > 13) {
        if (errorMessage) {
            *errorMessage = "Invalid radio channel.";
        }
        return false;
    }

    lastKnownStaChannel = channel;
    if (persist) {
        preferences.putUChar("radioChannel", lastKnownStaChannel);
    }

    if (!usesUsbNcmBackhaul() && WiFi.status() == WL_CONNECTED) {
        if (errorMessage) {
            *errorMessage = "Radio channel follows the infrastructure uplink while Wi-Fi backhaul is connected.";
        }
        return false;
    }

    stopTelemetryRadio();
    const esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        if (errorMessage) {
            *errorMessage = "esp_wifi_set_channel failed: " + String(static_cast<int>(err));
        }
        startTelemetryRadio();
        return false;
    }

    const bool apOk = configMode ? startConfigPortalOnChannel(channel) : startHubApOnChannel(channel);
    if (!apOk) {
        if (errorMessage) {
            *errorMessage = "Failed to restart Wi-Fi AP on the requested channel.";
        }
        return false;
    }

    if (errorMessage) {
        *errorMessage = "";
    }
    return true;
}

#if !PHYTALLY_USE_USB_NCM_BACKHAUL
void sendJson(AsyncWebServerRequest *request, int statusCode, const String &body) {
    request->send(statusCode, "application/json", body);
}

void sendJsonMessage(AsyncWebServerRequest *request, int statusCode, const char *status, const String &message) {
    sendJson(request, statusCode, String("{\"status\":") + quoteJson(status) + ",\"message\":" + quoteJson(message) + "}");
}

void sendUiAsset(AsyncWebServerRequest *request, const char *path, const char *contentType, const String *inlineContent = nullptr) {
    if (!inlineContent || inlineContent->length() == 0) {
        sendJsonMessage(request, 500, "error", "UI asset is unavailable.");
        return;
    }
    request->send(200, contentType, *inlineContent);
}
#else
void sendJson(int statusCode, const String &body) {
    server.send(statusCode, "application/json", body);
}

void sendJsonMessage(int statusCode, const char *status, const String &message) {
    sendJson(statusCode, String("{\"status\":") + quoteJson(status) + ",\"message\":" + quoteJson(message) + "}");
}

void sendUiAsset(const char *path, const char *contentType, const String *inlineContent = nullptr) {
    server.sendHeader("Cache-Control", "no-store");
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        if (file) {
            server.streamFile(file, contentType);
            file.close();
            return;
        }
    }
    if (inlineContent && inlineContent->length() > 0) {
        server.send(200, contentType, *inlineContent);
        return;
    }
    sendJsonMessage(500, "error", "UI asset is unavailable.");
}
#endif

bool parseMacAddress(const String &macStr, uint8_t output[6]) {
    int values[6];
    if (6 != sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) {
        return false;
    }
    for (int i = 0; i < 6; i++) output[i] = (uint8_t)values[i];
    return true;
}

void setActiveTransport(uint8_t transportKind, const String &reason, bool ignoreHolddown = false) {
    if (activeTransport == transportKind) {
        return;
    }
    if (!ignoreHolddown && !canSwitchTransportNow()) {
        return;
    }
    activeTransport = transportKind;
    lastTransportSwitchAt = millis();
    Serial.printf("ROUTE SWITCH: active=%s reason=%s\n",
                  PhyTallyTransportName(activeTransport),
                  reason.c_str());
    updateNativeVendorIE();
}

bool applyWirelessTransportMode(uint8_t mode, const String &reason, bool persist, String *errorMessage = nullptr) {
    if (PHYTALLY_FORCE_ESPNOW && mode != PHYTALLY_WIRELESS_TRANSPORT_ESPNOW) {
        if (errorMessage != nullptr) {
            *errorMessage = "This firmware forces ESP-NOW transport.";
        }
        return false;
    }

    if (mode > PHYTALLY_WIRELESS_TRANSPORT_ESPNOW) {
        if (errorMessage != nullptr) {
            *errorMessage = "Unsupported wireless transport.";
        }
        return false;
    }

    wirelessTransportMode = mode;
    if (persist) {
        preferences.putUChar("wirelessMode", wirelessTransportMode);
    }

    setActiveTransport(desiredTransportForMode(wirelessTransportMode), reason, true);
    if (activeTransport == desiredTransportForMode(wirelessTransportMode)) {
        updateNativeVendorIE();
    }
    Serial.printf("WIRELESS TRANSPORT: mode=%s active=%s reason=%s\n",
                  wirelessTransportModeName(wirelessTransportMode),
                  PhyTallyTransportName(activeTransport),
                  reason.c_str());
    return true;
}

void evaluateTransportRoute() {
    if (PHYTALLY_FORCE_ESPNOW) {
        if (activeTransport != PHYTALLY_TRANSPORT_ESPNOW) {
            activeTransport = PHYTALLY_TRANSPORT_ESPNOW;
        }
        return;
    }

    if (wirelessTransportMode == PHYTALLY_WIRELESS_TRANSPORT_BEACON_PROBE) {
        setActiveTransport(PHYTALLY_TRANSPORT_BEACON_PROBE, "manual_transport_beacon_probe", true);
        return;
    }

    if (wirelessTransportMode == PHYTALLY_WIRELESS_TRANSPORT_ESPNOW) {
        setActiveTransport(PHYTALLY_TRANSPORT_ESPNOW, "manual_transport_espnow", true);
        return;
    }

    if (receiverCount == 0) {
        return;
    }

    const unsigned long now = millis();
    const bool beaconHealthy = lastBeaconTelemetryAt != 0 && (now - lastBeaconTelemetryAt) <= 6000;
    const bool espNowHealthy = lastEspNowTelemetryAt != 0 && (now - lastEspNowTelemetryAt) <= 6000;

    if (activeTransport == PHYTALLY_TRANSPORT_BEACON_PROBE) {
        if (!beaconHealthy && espNowHealthy) {
            setActiveTransport(PHYTALLY_TRANSPORT_ESPNOW, "beacon_telemetry_stale");
        }
        return;
    }

    if (!espNowHealthy && beaconHealthy) {
        setActiveTransport(PHYTALLY_TRANSPORT_BEACON_PROBE, "espnow_telemetry_stale");
    }
}

bool queueCommand(const String &macStr, uint8_t commandType, uint8_t commandParam, const String &label) {
    uint8_t targetMac[6];
    if (!parseMacAddress(macStr, targetMac)) {
        return false;
    }

    if (!enqueuePendingHubCommand(targetMac, commandType, commandParam)) {
        lastCmdStatus = "Queue full: " + label + " for " + macStr;
        Serial.println(lastCmdStatus);
        return false;
    }

    if (!isCommandActive()) {
        pumpPendingHubCommands();
    } else {
        lastCmdStatus = "Queued: " + label + " for " + macStr;
        Serial.println(lastCmdStatus);
    }
    return true;
}

bool queueRemoteIdCommand(const String &macStr, uint8_t tallyIdOneBased) {
    if (tallyIdOneBased < 1 || tallyIdOneBased > MAX_TALLIES) return false;
    return queueCommand(macStr, HUB_CMD_SET_ID, tallyIdOneBased - 1, "SET_ID=" + String(tallyIdOneBased));
}

bool queueRemoteBrightnessCommand(const String &macStr, uint8_t brightness) {
    return queueCommand(macStr, HUB_CMD_SET_BRIGHTNESS, brightness, "SET_BRIGHTNESS=" + String(brightness));
}

bool queueRemoteIdentifyCommand(const String &macStr) {
    return queueCommand(macStr, HUB_CMD_IDENTIFY, 0, "IDENTIFY");
}

bool queueRemoteEspNowPhyCommand(const String &macStr, uint8_t mode) {
    if (!isValidEspNowPhyMode(mode)) {
        return false;
    }
    return queueCommand(macStr, HUB_CMD_SET_ESPNOW_PHY, mode, "SET_ESPNOW_PHY=" + String(espNowPhyModeName(mode)));
}

size_t queueEspNowPhyModeForOnlineReceivers(uint8_t mode) {
    size_t queued = 0;
    for (int i = 0; i < receiverCount; i++) {
        if (!receivers[i].online) {
            continue;
        }
        String mac = macToString(receivers[i].mac);
        if (queueRemoteEspNowPhyCommand(mac, mode)) {
            queued++;
        }
    }
    return queued;
}

bool applyEspNowPhyMode(uint8_t mode, const char *reason, bool persist, String *errorMessage) {
    if (!isValidEspNowPhyMode(mode)) {
        if (errorMessage) {
            *errorMessage = "Invalid ESP-NOW PHY mode.";
        }
        return false;
    }

    if (espNowPhySyncPending) {
        if (errorMessage) {
            *errorMessage = "Another ESP-NOW PHY sync is already in progress.";
        }
        return false;
    }

    if (mode == espNowPhyMode) {
        if (persist) {
            preferences.putUChar("espnowPhyMode", espNowPhyMode);
        }
        return true;
    }

    const size_t queued = queueEspNowPhyModeForOnlineReceivers(mode);
    if (queued == 0) {
        return applyLocalEspNowPhyMode(mode, reason, persist, errorMessage);
    }

    espNowPhySyncPending = true;
    espNowPhyPendingMode = mode;
    espNowPhySyncExpectedCount = queued;
    espNowPhySyncAckedCount = 0;
    espNowPhySyncFailureReason = "";
    lastCmdStatus = "Queued ESP-NOW PHY sync to " + String(espNowPhyModeName(mode)) +
        " for " + String(static_cast<unsigned>(queued)) + " receiver(s)";
    Serial.println(lastCmdStatus);
    if (!isCommandActive()) {
        pumpPendingHubCommands();
    }
    return true;
}

String buildReceiverJson(const ReceiverStatus &receiver) {
    return String("{\"mac\":") + quoteJson(macToString(receiver.mac)) +
        ",\"tally_id\":" + String(receiver.id + 1) +
        ",\"online\":" + boolToJson(receiver.online) +
        ",\"battery_percent\":" + String(receiver.battery) +
        ",\"brightness_percent\":" + String(receiver.brightnessPercent) +
        ",\"hub_rssi\":" + String(receiver.rssi) +
        ",\"transport\":" + quoteJson(String(PhyTallyTransportName(receiver.lastTransport))) +
        ",\"last_seen_ms_ago\":" + String(millis() - receiver.lastSeen) +
        "}";
}

String buildReceiversArrayJson() {
    String json = "[";
    for (int i = 0; i < receiverCount; i++) {
        if (i > 0) json += ",";
        json += buildReceiverJson(receivers[i]);
    }
    json += "]";
    return json;
}

String buildStatusJson() {
    uint8_t apMac[6];
    esp_wifi_get_mac(WIFI_IF_AP, apMac);
    usbncm::Status usbStatus = usbncm::status();

    String json = "{";
    json += "\"mode\":{\"config\":" + String(boolToJson(configMode)) + "},";
    json += "\"simulation\":{\"enabled\":" + String(boolToJson(testMode)) + "},";
    json += "\"network\":{";
    if (usesUsbNcmBackhaul()) {
        json += "\"backhaul_mode\":\"usb_ncm\",";
        json += "\"uplink_ready\":" + String(boolToJson(usbStatus.hasIp)) + ",";
        json += "\"uplink_ip\":" + quoteJson(usbStatus.ip.toString()) + ",";
        json += "\"usb_init_started\":" + String(boolToJson(usbStatus.initStarted)) + ",";
        json += "\"usb_begin_ok\":" + String(boolToJson(usbStatus.beginOk)) + ",";
        json += "\"usb_descriptor_loaded\":" + String(boolToJson(usbStatus.descriptorLoaded)) + ",";
        json += "\"usb_interface_enabled\":" + String(boolToJson(usbStatus.interfaceEnabled)) + ",";
        json += "\"usb_netif_added\":" + String(boolToJson(usbStatus.netifAdded)) + ",";
        json += "\"usb_mounted\":" + String(boolToJson(usbStatus.usbMounted)) + ",";
        json += "\"usb_link_up\":" + String(boolToJson(usbStatus.linkUp)) + ",";
        json += "\"usb_dhcp_started\":" + String(boolToJson(usbStatus.dhcpStarted)) + ",";
        json += "\"usb_descriptor_length\":" + String(usbStatus.descriptorLength) + ",";
        json += "\"usb_ip\":" + quoteJson(usbStatus.ip.toString()) + ",";
        json += "\"usb_gateway\":" + quoteJson(usbStatus.gateway.toString()) + ",";
        json += "\"usb_netmask\":" + quoteJson(usbStatus.netmask.toString()) + ",";
        json += "\"usb_hostname\":" + quoteJson(usbStatus.hostname) + ",";
        json += "\"usb_mac\":" + quoteJson(usbStatus.mac) + ",";
        json += "\"usb_last_event\":" + quoteJson(usbStatus.lastUsbEvent) + ",";
        json += "\"usb_last_event_ms_ago\":";
        json += usbStatus.lastUsbEventAtMs == 0 ? String("null") : String(millis() - usbStatus.lastUsbEventAtMs);
        json += ",";
        json += "\"usb_last_dhcp_change_ms_ago\":";
        json += usbStatus.lastDhcpChangeAtMs == 0 ? String("null") : String(millis() - usbStatus.lastDhcpChangeAtMs);
        json += ",";
        json += "\"usb_last_error\":" + quoteJson(usbStatus.lastError) + ",";
    } else {
        json += "\"backhaul_mode\":\"wifi_sta\",";
        json += "\"uplink_ready\":" + String(boolToJson(WiFi.status() == WL_CONNECTED)) + ",";
        json += "\"wifi_connected\":" + String(boolToJson(WiFi.status() == WL_CONNECTED)) + ",";
        json += "\"wifi_ip\":" + quoteJson(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + ",";
        json += "\"uplink_ip\":" + quoteJson(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + ",";
    }
    json += "\"mdns_host\":" + quoteJson(currentMdnsHost()) + ",";
    json += "\"ap_ssid\":" + quoteJson(String(configMode ? apSsid : hubSsid)) + ",";
    json += "\"ap_mac\":" + quoteJson(macToString(apMac)) + ",";
    json += "\"radio_channel\":" + String(getCurrentRadioChannel());
    json += "},";
    json += "\"switchers\":{";
    json += "\"atem\":{\"enabled\":" + String(boolToJson(atemEnabled)) + ",\"supported\":true,\"ip\":" + quoteJson(String(atemIpStr)) + ",\"connected\":false,\"last_error\":\"\"},";
    json += "\"vmix\":{\"enabled\":" + String(boolToJson(vmixEnabled)) + ",\"supported\":true,\"ip\":" + quoteJson(String(vmixIpStr)) + ",\"connected\":" + String(boolToJson(vmixClient.connected())) + ",\"last_error\":\"\"},";
    json += "\"tsl\":{\"enabled\":" + String(boolToJson(tslEnabled)) + ",\"supported\":true,\"listen_port\":" + String(tslListenPort) + ",\"listen_addr\":" + quoteJson(currentTslListenAddr()) + ",\"connected\":" + String(boolToJson(tslEnabled && lastTslPacketAt > 0 && millis() - lastTslPacketAt < 5000)) + ",\"last_error\":\"\"},";
    json += "\"obs\":{\"enabled\":" + String(boolToJson(obsEnabled)) + ",\"supported\":true,\"url\":" + quoteJson(String(obsUrl)) + ",\"password_set\":" + String(boolToJson(strlen(obsPassword) > 0)) + ",\"connected\":" + String(boolToJson(obsConnected)) + ",\"last_error\":" + quoteJson(obsLastError) + "}";
    json += "},";
    json += "\"wireless\":" + buildWirelessTransportJson() + ",";
    json += "\"route\":{";
    json += "\"wireless_transport\":" + quoteJson(String(wirelessTransportModeName(wirelessTransportMode))) + ",";
    json += "\"active_transport\":" + quoteJson(String(PhyTallyTransportName(activeTransport))) + ",";
    json += "\"priority\":" + quoteJson(String(routePriority == PHYTALLY_ROUTE_ESPNOW_FIRST ? "espnow_first" : "beacon_first")) + ",";
    json += "\"beacon_last_telem_ms_ago\":";
    json += lastBeaconTelemetryAt == 0 ? String("null") : String(millis() - lastBeaconTelemetryAt);
    json += ",";
    json += "\"espnow_last_telem_ms_ago\":";
    json += lastEspNowTelemetryAt == 0 ? String("null") : String(millis() - lastEspNowTelemetryAt);
    json += "},";
    json += "\"telemetry\":{";
    json += "\"packet_count\":" + String(telemetryDebug.packetCount) + ",";
    if (telemetryDebug.havePacket) {
        json += "\"last_receiver\":{";
        json += "\"mac\":" + quoteJson(macToString(telemetryDebug.lastReceiverMac)) + ",";
        json += "\"tally_id\":" + String(telemetryDebug.lastReceiverId + 1) + ",";
        json += "\"age_ms\":" + String(millis() - telemetryDebug.lastSeen) + ",";
        json += "\"receiver_reported_hub_rssi\":" + String(telemetryDebug.lastReportedHubRssi) + ",";
        json += "\"air_rssi\":" + String(telemetryDebug.lastAirRssi) + ",";
        json += "\"transport\":" + quoteJson(String(PhyTallyTransportName(telemetryDebug.lastTransport)));
        json += "}";
    } else {
        json += "\"last_receiver\":null";
    }
    json += "},";
    json += "\"command\":{";
    json += "\"status\":" + quoteJson(lastCmdStatus) + ",";
    json += "\"active\":" + String(boolToJson(cmdUntil > 0 && millis() < cmdUntil)) + ",";
    if (cmdUntil > 0 && millis() < cmdUntil) {
        json += "\"target_mac\":" + quoteJson(macToString(cmdTargetMac)) + ",";
        json += "\"tally_id\":" + String(cmdParam + 1) + ",";
        json += "\"command_id\":" + String(cmdId) + ",";
        json += "\"command_type\":" + String(cmdType) + ",";
        json += "\"expires_in_ms\":" + String(cmdUntil - millis());
    } else {
        json += "\"target_mac\":null,\"tally_id\":null,\"command_id\":0,\"command_type\":0,\"expires_in_ms\":0";
    }
    json += "},";
    json += "\"tallies\":[";
    for (int i = 0; i < MAX_TALLIES; i++) {
        if (i > 0) json += ",";
        json += String((int)tallyStates[i]);
    }
    json += "],";
    json += "\"receivers\":" + buildReceiversArrayJson();
    json += "}";
    return json;
}

String buildDiscoveryJson() {
    return String("{") +
        "\"name\":\"PhyTally REST API\"," +
        "\"version\":\"v1\"," +
        "\"dashboard\":\"/\"," +
        "\"status\":" + quoteJson("/api/v1/status") + "," +
        "\"config\":" + quoteJson("/api/v1/config") + "," +
        "\"wireless_transport\":" + quoteJson("/api/v1/wireless/transport") + "," +
        "\"wireless_espnow_phy\":" + quoteJson("/api/v1/wireless/espnow/phy") + "," +
        "\"wifi_survey\":" + quoteJson("/api/v1/wifi/survey") + "," +
        "\"simulation\":" + quoteJson("/api/v1/simulation") + "," +
        "\"receivers\":" + quoteJson("/api/v1/receivers") + "," +
        "\"assign_id\":" + quoteJson("/api/v1/receivers/assign-id") + "," +
        "\"set_brightness\":" + quoteJson("/api/v1/receivers/set-brightness") + "," +
        "\"identify\":" + quoteJson("/api/v1/receivers/identify") +
        "}";
}

const char *authModeToString(wifi_auth_mode_t authMode) {
    switch (authMode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa-psk";
        case WIFI_AUTH_WPA2_PSK: return "wpa2-psk";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa-wpa2-psk";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-enterprise";
        case WIFI_AUTH_WPA3_PSK: return "wpa3-psk";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2-wpa3-psk";
        case WIFI_AUTH_WAPI_PSK: return "wapi-psk";
        default: return "unknown";
    }
}

String serializeWifiSurveyResults(int count, bool scanning) {
    String json = "{";
    json += "\"scanned_at_ms\":" + String(lastWifiSurveyAt) + ",";
    json += "\"scan_started_at_ms\":" + String(wifiSurveyStartedAt) + ",";
    json += "\"scanning\":" + String(boolToJson(scanning)) + ",";
    json += "\"network_count\":" + String(count < 0 ? 0 : count) + ",";
    json += "\"channel_suggestion\":" + buildChannelSuggestionJson(count < 0 ? 0 : count) + ",";
    json += "\"networks\":[";

    if (count > 0) {
        for (int i = 0; i < count; i++) {
            if (i > 0) json += ",";
            String ssid = WiFi.SSID(i);
            wifi_auth_mode_t authMode = WiFi.encryptionType(i);
            json += "{";
            json += "\"ssid\":" + quoteJson(ssid) + ",";
            json += "\"bssid\":" + quoteJson(WiFi.BSSIDstr(i)) + ",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"channel\":" + String(WiFi.channel(i)) + ",";
            json += "\"secure\":" + String(boolToJson(authMode != WIFI_AUTH_OPEN)) + ",";
            json += "\"auth_mode\":" + quoteJson(String(authModeToString(authMode))) + ",";
            json += "\"hidden\":" + String(boolToJson(ssid.length() == 0));
            json += "}";
        }
    }

    json += "]}";
    return json;
}

void finishWifiSurveyWithRestore() {
    if (configMode) {
        startConfigPortal();
    } else {
        startHubApOnCurrentStaChannel();
    }
}

void startWifiSurvey() {
    if (wifiSurveyInProgress) return;
    if (configMode) {
        dnsServer.stop();
    }
    WiFi.softAPdisconnect(true);
    delay(100);
    stopTelemetryRadio();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(100);
    WiFi.scanDelete();
    wifiSurveyStartedAt = millis();
    wifiSurveyInProgress = true;
    lastWifiSurveyJson = String("{\"scanned_at_ms\":") + String(lastWifiSurveyAt) +
        ",\"scan_started_at_ms\":" + String(wifiSurveyStartedAt) +
        ",\"scanning\":true,\"network_count\":0,\"channel_suggestion\":null,\"networks\":[]}";
    Serial.println("Starting async Wi-Fi site survey...");
    WiFi.scanNetworks(true, true);
}

String buildWifiSurveyStatusJson() {
    return lastWifiSurveyJson;
}

void handleWifiSurveyLoop() {
    if (wifiSurveyStartRequested && !wifiSurveyInProgress) {
        wifiSurveyStartRequested = false;
        startWifiSurvey();
    }

    if (!wifiSurveyInProgress) return;

    int surveyStatus = WiFi.scanComplete();
    if (surveyStatus == WIFI_SCAN_RUNNING) return;

    if (surveyStatus >= 0) {
        lastWifiSurveyAt = millis();
        lastWifiSurveyJson = serializeWifiSurveyResults(surveyStatus, false);
        wifiSurveyInProgress = false;
        WiFi.scanDelete();
        finishWifiSurveyWithRestore();
        Serial.printf("Wi-Fi survey complete: %d networks found.\n", surveyStatus);
        return;
    }

    if (surveyStatus == WIFI_SCAN_FAILED) {
        wifiSurveyInProgress = false;
        WiFi.scanDelete();
        lastWifiSurveyJson = String("{\"scanned_at_ms\":") + String(lastWifiSurveyAt) +
            ",\"scan_started_at_ms\":" + String(wifiSurveyStartedAt) +
            ",\"scanning\":false,\"network_count\":0,\"channel_suggestion\":null,\"error\":\"scan_failed\",\"networks\":[]}";
        finishWifiSurveyWithRestore();
        Serial.println("Wi-Fi survey failed.");
    }
}

#if !PHYTALLY_USE_USB_NCM_BACKHAUL
String *appendRequestBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        String *body = new String();
        body->reserve(total);
        request->_tempObject = body;
    }

    String *body = reinterpret_cast<String*>(request->_tempObject);
    if (!body) return nullptr;

    for (size_t i = 0; i < len; i++) {
        body->concat((char)data[i]);
    }

    if (index + len < total) return nullptr;

    request->_tempObject = nullptr;
    return body;
}
#endif

void updateNativeVendorIE() {
    memcpy(currentIE.payload.states, tallyStates, MAX_TALLIES);
    
    if (cmdUntil > 0 && millis() < cmdUntil) {
        memcpy(currentIE.payload.target_mac, cmdTargetMac, 6);
        currentIE.payload.cmd_type = cmdType;
        currentIE.payload.cmd_param = cmdParam;
        currentIE.payload.cmd_id = cmdId;
    } else {
        memset(currentIE.payload.target_mac, 0, 6);
        currentIE.payload.cmd_type = 0;
        currentIE.payload.cmd_param = 0;
        currentIE.payload.cmd_id = 0;
        if (cmdUntil > 0) {
            if (espNowPhySyncPending && cmdType == HUB_CMD_SET_ESPNOW_PHY) {
                failEspNowPhySync("command expired for " + macToString(cmdTargetMac));
            }
            clearActiveCommand("Expired");
            Serial.println("OTA Command Expired.");
        }
    }

    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, NULL);

#if PHYTALLY_ENABLE_ESPNOW
    if (isEspNowTransportActive() && espNowReady) {
        PhyTallyEspNowHubPacket packet = {};
        PhyTallyBuildEspNowHubPacket(packet, currentIE.payload, hubEspNowSequence++);
        esp_now_send(kEspNowBroadcastMac, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
    } else
#endif
    {
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, &currentIE);
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, &currentIE);
    }

    if (cmdUntil > 0) {
        Serial.printf("HUB CMD: Type=%d Param=%d ID=%d via=%s for MAC %02X:%02X:%02X...\n",
            cmdType, cmdParam, cmdId, PhyTallyTransportName(activeTransport),
            currentIE.payload.target_mac[0], currentIE.payload.target_mac[1], currentIE.payload.target_mac[2]);
    }
}

// --- Captive Portal Handler ---
#if !PHYTALLY_USE_USB_NCM_BACKHAUL
class CaptiveRequestHandler : public AsyncWebHandler {
public:
    CaptiveRequestHandler() {}
    virtual ~CaptiveRequestHandler() {}
    bool canHandle(AsyncWebServerRequest *request) {
        // Redirect if the host is not our IP (e.g. apple.com, google.com)
        return !request->host().equals("192.168.4.1");
    }
    void handleRequest(AsyncWebServerRequest *request) {
        request->redirect("http://192.168.4.1/");
    }
};
#endif

void recordTelemetryPacket(const uint8_t *srcMac, const TelemetryPayload &telem, int8_t airRssi, uint8_t transportKind) {
    int foundIdx = -1;
    for (int i = 0; i < receiverCount; i++) {
        if (memcmp(receivers[i].mac, srcMac, 6) == 0) {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx == -1 && receiverCount < (MAX_TALLIES * 2)) {
        foundIdx = receiverCount++;
        memcpy(receivers[foundIdx].mac, srcMac, 6);
        receivers[foundIdx].brightnessPercent = 100;
    }

    if (foundIdx == -1) {
        return;
    }

    receivers[foundIdx].id = telem.tally_id;
    receivers[foundIdx].battery = telem.battery_percent;
    receivers[foundIdx].rssi = telem.rssi;
    receivers[foundIdx].lastSeen = millis();
    receivers[foundIdx].lastTransport = transportKind;
    receivers[foundIdx].online = true;
    telemetryDebug.packetCount++;
    telemetryDebug.lastSeen = receivers[foundIdx].lastSeen;
    telemetryDebug.lastAirRssi = airRssi;
    telemetryDebug.lastReportedHubRssi = telem.rssi;
    telemetryDebug.lastReceiverId = telem.tally_id;
    telemetryDebug.lastTransport = transportKind;
    memcpy(telemetryDebug.lastReceiverMac, srcMac, 6);
    telemetryDebug.havePacket = true;

    if (transportKind == PHYTALLY_TRANSPORT_ESPNOW) {
        lastEspNowTelemetryAt = millis();
    } else {
        lastBeaconTelemetryAt = millis();
    }

#if PHYTALLY_HAS_STATUS_LED
    digitalWrite(STATUS_LED, HIGH);
    ledOffTime = millis() + 50;
#endif

    if (cmdUntil > 0 &&
        cmdId != 0 &&
        memcmp(cmdTargetMac, srcMac, sizeof(cmdTargetMac)) == 0 &&
        telem.ack_command_id == cmdId) {
        if (espNowPhySyncPending && cmdType == HUB_CMD_SET_ESPNOW_PHY) {
            espNowPhySyncAckedCount++;
        }
        clearActiveCommand("Acked");
        updateNativeVendorIE();
    }

    static unsigned long lastTelemetryLog = 0;
    if (millis() - lastTelemetryLog > 5000) {
        lastTelemetryLog = millis();
        char macStr[18];
        formatMac(srcMac, macStr, sizeof(macStr));
        Serial.printf("TELEM RX VIA=%s: RX=%s ID=%u HubRSSI=%d AirRSSI=%d Ack=%u Count=%lu\n",
            PhyTallyTransportName(transportKind),
            macStr,
            (unsigned int)(telem.tally_id + 1),
            (int)telem.rssi,
            (int)airRssi,
            (unsigned int)telem.ack_command_id,
            telemetryDebug.packetCount);
    }
}

void telemetry_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    // Probe Request (0x40)
    if (payload[0] != 0x40) return;

    // Start parsing IEs after the 24-byte header
    int offset = 24; 
    while (offset < len) {
        if (offset + 2 > len) break;
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        if (offset + 2 + tagLen > len) break;

        if (tag == 0xDD && tagLen >= sizeof(TelemetryPayload)) {
            if (payload[offset + 2] == TELEM_OUI_0 &&
                payload[offset + 3] == TELEM_OUI_1 &&
                payload[offset + 4] == TELEM_OUI_2) {

                TelemetryPayload telem = {};
                memcpy(&telem, &payload[offset + 2], sizeof(telem));
                uint8_t *srcMac = &payload[10];
                bool isZero = true;
                for(int m=0; m<6; m++) if(srcMac[m] != 0) isZero = false;
                if(isZero) break;
                recordTelemetryPacket(srcMac, telem, pkt->rx_ctrl.rssi, PHYTALLY_TRANSPORT_BEACON_PROBE);
                break;
            }
        }
        offset += 2 + tagLen;
    }
}

#if PHYTALLY_ENABLE_ESPNOW
#if ESP_IDF_VERSION_MAJOR >= 5
void espnow_rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !info->src_addr || len < static_cast<int>(sizeof(PhyTallyEspNowTelemetryPacket))) {
        return;
    }
    const auto *packet = reinterpret_cast<const PhyTallyEspNowTelemetryPacket *>(data);
    if (!PhyTallyValidateEspNowHeader(packet->header, PHYTALLY_ESPNOW_MSG_TELEMETRY, sizeof(TelemetryPayload))) {
        return;
    }
    const int8_t airRssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    recordTelemetryPacket(info->src_addr, packet->payload, airRssi, PHYTALLY_TRANSPORT_ESPNOW);
}
#else
void espnow_rx_cb(const uint8_t *macAddr, const uint8_t *data, int len) {
    if (!macAddr || len < static_cast<int>(sizeof(PhyTallyEspNowTelemetryPacket))) {
        return;
    }
    const auto *packet = reinterpret_cast<const PhyTallyEspNowTelemetryPacket *>(data);
    if (!PhyTallyValidateEspNowHeader(packet->header, PHYTALLY_ESPNOW_MSG_TELEMETRY, sizeof(TelemetryPayload))) {
        return;
    }
    recordTelemetryPacket(macAddr, packet->payload, -127, PHYTALLY_TRANSPORT_ESPNOW);
}
#endif
#endif

void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event != ARDUINO_EVENT_WIFI_AP_STACONNECTED || configMode) return;

    uint8_t *mac = info.wifi_ap_staconnected.mac;
    char macStr[18];
    formatMac(mac, macStr, sizeof(macStr));
    Serial.printf("LIVE AP CLIENT REJECTED: MAC=%s AID=%u\n",
        macStr,
        (unsigned int)info.wifi_ap_staconnected.aid);
    esp_wifi_deauth_sta(info.wifi_ap_staconnected.aid);
}

void refreshMdns() {
    if (usesUsbNcmBackhaul()) {
        return;
    }

    String hostname = configuredHostnameLabel();
    bool networkReady = usesUsbNcmBackhaul() ? usbncm::hasIp() : WiFi.status() == WL_CONNECTED;

    if (!networkReady) {
        if (mdnsStarted) {
#if PHYTALLY_USE_USB_NCM_BACKHAUL
            LOCK_TCPIP_CORE();
#endif
            MDNS.end();
#if PHYTALLY_USE_USB_NCM_BACKHAUL
            UNLOCK_TCPIP_CORE();
#endif
            mdnsStarted = false;
            mdnsHostLabel = "";
        }
        return;
    }

    if (mdnsStarted && mdnsHostLabel == hostname) {
        return;
    }

    if (mdnsStarted) {
#if PHYTALLY_USE_USB_NCM_BACKHAUL
        LOCK_TCPIP_CORE();
#endif
        MDNS.end();
#if PHYTALLY_USE_USB_NCM_BACKHAUL
        UNLOCK_TCPIP_CORE();
#endif
        mdnsStarted = false;
    }

    bool mdnsOk = false;
#if PHYTALLY_USE_USB_NCM_BACKHAUL
    LOCK_TCPIP_CORE();
#endif
    mdnsOk = MDNS.begin(hostname.c_str());
    if (mdnsOk) {
        MDNS.addService("http", "tcp", 80);
    }
#if PHYTALLY_USE_USB_NCM_BACKHAUL
    UNLOCK_TCPIP_CORE();
#endif
    if (mdnsOk) {
        mdnsStarted = true;
        mdnsHostLabel = hostname;
        Serial.printf("mDNS READY: http://%s.local\n", hostname.c_str());
    } else {
        mdnsHostLabel = "";
        Serial.println("mDNS INIT FAILED");
    }
}

#if PHYTALLY_ENABLE_ESPNOW
void initEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESPNOW INIT FAILED");
        espNowReady = false;
        return;
    }
    esp_now_register_recv_cb(espnow_rx_cb);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kEspNowBroadcastMac, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(kEspNowBroadcastMac)) {
        esp_now_add_peer(&peer);
    }
    espNowReady = true;
    String errorMessage;
    if (!applyLocalEspNowPhyMode(espNowPhyMode, "espnow_init", false, &errorMessage)) {
        Serial.printf("ESPNOW PHY INIT FAILED: %s\n", errorMessage.c_str());
    }
    Serial.println("ESPNOW READY");
}
#endif

bool parseTSLPacket(const uint8_t *packet, size_t len, int *receiverIndex, TallyState *state) {
    if (!packet || len < 2) {
        return false;
    }
    const uint8_t header = packet[0];
    if (header < 0x80) {
        return false;
    }
    const int address = header - 0x80;
    if (address < 0 || address >= MAX_TALLIES) {
        return false;
    }

    const uint8_t control = packet[1];
    TallyState parsedState = TALLY_OFF;
    if ((control & 0x01) != 0) {
        parsedState = TALLY_PROGRAM;
    } else if ((control & 0x02) != 0) {
        parsedState = TALLY_PREVIEW;
    }

    *receiverIndex = address;
    *state = parsedState;
    return true;
}

void handleTSL() {
    if (!tslEnabled || configMode) {
        return;
    }
    int packetLen = tslUdp.parsePacket();
    while (packetLen > 0) {
        uint8_t packet[64];
        const int readLen = tslUdp.read(packet, sizeof(packet));
        int tallyIndex = -1;
        TallyState newState = TALLY_OFF;
        if (readLen > 0 && parseTSLPacket(packet, static_cast<size_t>(readLen), &tallyIndex, &newState)) {
            lastTslPacketAt = millis();
            if (tallyStates[tallyIndex] != newState) {
                tallyStates[tallyIndex] = newState;
                updateNativeVendorIE();
            }
        }
        packetLen = tslUdp.parsePacket();
    }
}

void handleVmix() {
    if (!vmixEnabled || strcmp(vmixIpStr, "0.0.0.0") == 0 || configMode) return;
    if (!vmixClient.connected()) {
        if (millis() - lastVmixRetry > 5000) {
            lastVmixRetry = millis();
            if (vmixClient.connect(vmixIpStr, 8099)) {
                vmixClient.print("SUBSCRIBE TALLY\r\n");
            }
        }
        return;
    }
    while (vmixClient.available()) {
        String line = vmixClient.readStringUntil('\n');
        line.trim();
        if (line.startsWith("TALLY OK ")) {
            String states = line.substring(9);
            bool changed = false;
            for (int i = 0; i < states.length() && i < MAX_TALLIES; i++) {
                char s = states[i];
                TallyState newState = TALLY_OFF;
                if (s == '1') newState = TALLY_PROGRAM;
                else if (s == '2') newState = TALLY_PREVIEW;
                
                if (tallyStates[i] != newState) {
                    tallyStates[i] = newState;
                    changed = true;
                }
            }
            if (changed) updateNativeVendorIE();
        }
    }
}

void handleAtem() {
    if (!atemEnabled || strcmp(atemIpStr, "0.0.0.0") == 0 || configMode) return;
    atemSwitcher.runLoop();
    if (millis() - lastAtemCheck > 50) {
        lastAtemCheck = millis();
        bool changed = false;
        for (int i = 0; i < MAX_TALLIES; i++) {
            TallyState newState = TALLY_OFF;
            if (atemSwitcher.getProgramTally(i + 1)) newState = TALLY_PROGRAM;
            else if (atemSwitcher.getPreviewTally(i + 1)) newState = TALLY_PREVIEW;
            
            if (tallyStates[i] != newState) {
                tallyStates[i] = newState;
                changed = true;
            }
        }
        if (changed) updateNativeVendorIE();
    }
}

void loadSavedConfiguration() {
    preferences.begin("phytally", false);
    if (preferences.isKey("ssid")) {
        preferences.getString("ssid", wifiSsid, sizeof(wifiSsid));
    }
    if (preferences.isKey("pass")) {
        preferences.getString("pass", wifiPass, sizeof(wifiPass));
    }
    if (preferences.isKey("atemIp")) {
        preferences.getString("atemIp", atemIpStr, sizeof(atemIpStr));
    }
    if (preferences.isKey("vmixIp")) {
        preferences.getString("vmixIp", vmixIpStr, sizeof(vmixIpStr));
    }
    if (preferences.isKey("obsUrl")) {
        preferences.getString("obsUrl", obsUrl, sizeof(obsUrl));
    }
    if (preferences.isKey("obsPass")) {
        preferences.getString("obsPass", obsPassword, sizeof(obsPassword));
    }
    if (preferences.isKey("hostname")) {
        preferences.getString("hostname", hostnameOverride, sizeof(hostnameOverride));
    }
    lastKnownStaChannel = preferences.getUChar("radioChannel", TALLY_WIFI_CHANNEL);
    if (lastKnownStaChannel < 1 || lastKnownStaChannel > 13) {
        lastKnownStaChannel = TALLY_WIFI_CHANNEL;
    }
    atemEnabled = preferences.getBool("atemEnabled", false);
    vmixEnabled = preferences.getBool("vmixEnabled", false);
    tslEnabled = preferences.getBool("tslEnabled", false);
    obsEnabled = preferences.getBool("obsEnabled", false);
    tslListenPort = preferences.getUShort("tslPort", 9800);
    routePriority = preferences.getUChar("routePriority", PHYTALLY_FORCE_ESPNOW ? PHYTALLY_ROUTE_ESPNOW_FIRST : PHYTALLY_ROUTE_BEACON_FIRST);
    wirelessTransportMode = preferences.getUChar("wirelessMode", PHYTALLY_FORCE_ESPNOW ? PHYTALLY_WIRELESS_TRANSPORT_ESPNOW : PHYTALLY_WIRELESS_TRANSPORT_AUTO);
    if (wirelessTransportMode > PHYTALLY_WIRELESS_TRANSPORT_ESPNOW) {
        wirelessTransportMode = PHYTALLY_FORCE_ESPNOW ? PHYTALLY_WIRELESS_TRANSPORT_ESPNOW : PHYTALLY_WIRELESS_TRANSPORT_AUTO;
    }
    espNowPhyMode = preferences.getUChar("espnowPhyMode", PHYTALLY_ESPNOW_PHY_11B_1M);
    if (!isValidEspNowPhyMode(espNowPhyMode)) {
        espNowPhyMode = PHYTALLY_ESPNOW_PHY_11B_1M;
    }
    activeTransport = desiredTransportForMode(wirelessTransportMode);
}

void setup() {
    Serial.begin(115200);
#if PHYTALLY_HAS_STATUS_LED
    pinMode(STATUS_LED, OUTPUT);
#endif
#if defined(PHYTALLY_NEOPIXEL_PIN)
    FastLED.addLeds<NEOPIXEL, PHYTALLY_NEOPIXEL_PIN>(txIndicatorPixel, 1);
    FastLED.setBrightness(PHYTALLY_TX_NEOPIXEL_BRIGHTNESS);
    FastLED.clear(true);
    txIndicatorPixelReady = true;
#endif

    loadSavedConfiguration();

    if (usesUsbNcmBackhaul()) {
        Serial.println("Switcher backhaul: USB-NCM");
    } else {
        Serial.printf("Loaded SSID: '%s'\n", wifiSsid);
    }
    Serial.printf("ATEM: %s (%s)\n", atemIpStr, atemEnabled ? "ON" : "OFF");
    Serial.printf("vMix: %s (%s)\n", vmixIpStr, vmixEnabled ? "ON" : "OFF");
    Serial.printf("OBS: %s (%s)\n", obsUrl, obsEnabled ? "ON" : "OFF");
    Serial.printf("TSL: %s (%s)\n", currentTslListenAddr().c_str(), tslEnabled ? "ON" : "OFF");

    if (!hasConfiguredSwitcher()) {
        testMode = true;
        Serial.println("Test Mode: AUTO-ENABLED");
    }

    for (int i = 0; i < (MAX_TALLIES * 2); i++) {
        receivers[i].online = false;
        receivers[i].brightnessPercent = 100;
    }

    // Init IE structure
    currentIE.element_id = 0xDD;
    currentIE.length = sizeof(HubPayload);
    currentIE.payload.oui[0] = VENDOR_OUI_0;
    currentIE.payload.oui[1] = VENDOR_OUI_1;
    currentIE.payload.oui[2] = VENDOR_OUI_2;
    currentIE.payload.protocol_version = 0x01;
    memset(currentIE.payload.states, 0, MAX_TALLIES);
    memset(currentIE.payload.target_mac, 0, 6);
    currentIE.payload.cmd_type = 0;
    currentIE.payload.cmd_param = 0;
    currentIE.payload.cmd_id = 0;
    updateTxBoardIndicator();

    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(!usesUsbNcmBackhaul());
    WiFi.persistent(false);
    if (wifiEventHandlerId == ARDUINO_EVENT_MAX) {
        wifiEventHandlerId = WiFi.onEvent(handleWiFiEvent);
    }

    if (usesUsbNcmBackhaul()) {
        if (usbncm::begin(configuredHostnameLabel())) {
            usbncm::Status usbStatus = usbncm::status();
            Serial.printf("USB-NCM BACKHAUL INITIALIZED: hostname=%s mac=%s netif_added=%s desc_len=%u\n",
                          usbStatus.hostname.c_str(),
                          usbStatus.mac.c_str(),
                          usbStatus.netifAdded ? "true" : "false",
                          static_cast<unsigned>(usbStatus.descriptorLength));
            Serial.println("USB-NCM awaiting host link and DHCP");
        } else {
            Serial.println("USB-NCM BACKHAUL INIT FAILED");
            usbncm::Status usbStatus = usbncm::status();
            if (usbStatus.lastError.length() > 0) {
                Serial.println("USB-NCM failure detail: " + usbStatus.lastError);
            }
        }
        if (!usbNcmLoopTaskHandle) {
            xTaskCreatePinnedToCore(
                usbNcmLoopTask,
                "usb_ncm_loop",
                4096,
                nullptr,
                3,
                &usbNcmLoopTaskHandle,
                ARDUINO_RUNNING_CORE
            );
            if (usbNcmLoopTaskHandle) {
                Serial.println("USB-NCM service task started");
            } else {
                Serial.println("USB-NCM service task start failed");
            }
        }
        startHubApOnCurrentStaChannel();
        if (atemEnabled) {
            IPAddress ip;
            if (ip.fromString(atemIpStr)) {
                atemSwitcher.begin(ip);
            }
        }
    } else if (connectToInfrastructureWiFi(30000)) {
        startHubApOnCurrentStaChannel();
        refreshMdns();
        if (atemEnabled) {
            IPAddress ip;
            if (ip.fromString(atemIpStr)) {
                atemSwitcher.begin(ip);
                atemSwitcher.connect();
            }
        }
    } else {
        Serial.println("\nWi-Fi FAILED. Starting Configuration Mode.");
        startConfigPortal();
    }

    if(!LittleFS.begin(true)){
        Serial.println("LittleFS Mount Failed");
    }
    uiIndexHtml = loadUiAsset("/index.html");
    uiDashboardJs = loadUiAsset("/dashboard.js");
    uiDashboardCss = loadUiAsset("/dashboard.css");
    inlineUiAssets();

#if PHYTALLY_ENABLE_ESPNOW
    initEspNow();
#endif

    if (tslEnabled) {
        tslUdp.begin(tslListenPort);
    }

    #if PHYTALLY_USE_USB_NCM_BACKHAUL
    server.on("/", HTTP_GET, []() {
        sendUiAsset("/index.html", "text/html", &uiIndexHtml);
    });

    server.on("/api/v1/status", HTTP_GET, []() {
        sendJson(200, buildStatusJson());
    });

    server.on("/api/v1/receivers", HTTP_GET, []() {
        sendJson(200, String("{\"receivers\":") + buildReceiversArrayJson() + "}");
    });

    server.on("/api/v1/simulation", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error || !doc["enabled"].is<bool>()) {
            sendJsonMessage(400, "error", "Body must contain boolean field 'enabled'.");
            return;
        }

        testMode = doc["enabled"].as<bool>();
        if (!testMode) {
            memset(tallyStates, 0, MAX_TALLIES);
        }
        lastTestStep = 0;
        updateNativeVendorIE();
        updateTxBoardIndicator();
        sendJsonMessage(200, "ok", testMode ? "Simulation enabled." : "Simulation disabled.");
    });

    server.on("/api/v1/receivers/assign-id", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        int tallyId = doc["tally_id"] | 0;
        if (mac.length() == 0 || tallyId < 1 || tallyId > MAX_TALLIES) {
            sendJsonMessage(400, "error", "Body must contain 'mac' and 1-based 'tally_id'.");
            return;
        }
        if (!queueRemoteIdCommand(mac, (uint8_t)tallyId)) {
            sendJsonMessage(400, "error", "Invalid MAC or tally_id.");
            return;
        }

        sendJson(202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("SET_ID queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) +
            ",\"tally_id\":" + String(tallyId) + "}");
    });

    server.on("/api/v1/receivers/set-brightness", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        uint8_t rawBrightness = 0;
        uint8_t brightnessPercent = 0;
        if (mac.length() == 0 || !parseBrightnessPayload(doc, &rawBrightness, &brightnessPercent)) {
            sendJsonMessage(400, "error", "Body must contain 'mac' and either 'brightness' 0..255 or 'brightness_percent' 0..100.");
            return;
        }
        if (!queueRemoteBrightnessCommand(mac, rawBrightness)) {
            sendJsonMessage(400, "error", "Invalid MAC or brightness.");
            return;
        }
        rememberReceiverBrightnessPercent(mac, brightnessPercent);
        sendJson(202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("SET_BRIGHTNESS queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) +
            ",\"brightness\":" + String(rawBrightness) +
            ",\"brightness_percent\":" + String(brightnessPercent) + "}");
    });

    server.on("/api/v1/receivers/identify", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        if (mac.length() == 0) {
            sendJsonMessage(400, "error", "Body must contain 'mac'.");
            return;
        }
        if (!queueRemoteIdentifyCommand(mac)) {
            sendJsonMessage(400, "error", "Invalid MAC.");
            return;
        }
        sendJson(202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("IDENTIFY queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) + "}");
    });

    server.on("/api/v1/config", HTTP_GET, []() {
        String json = "{";
        json += "\"last_survey_at_ms\":" + String(lastWifiSurveyAt) + ",";
        json += "\"wireless_transport\":" + quoteJson(String(wirelessTransportModeName(wirelessTransportMode))) + ",";
        json += "\"espnow_phy_mode\":" + quoteJson(String(espNowPhyModeName(espNowPhyMode))) + ",";
        json += "\"radio_channel\":" + String(getCurrentRadioChannel()) + ",";
        json += "\"atem_ip\":" + quoteJson(String(atemIpStr)) + ",";
        json += "\"atem_enabled\":" + String(boolToJson(atemEnabled)) + ",";
        json += "\"vmix_ip\":" + quoteJson(String(vmixIpStr)) + ",";
        json += "\"vmix_enabled\":" + String(boolToJson(vmixEnabled)) + ",";
        json += "\"tsl_enabled\":" + String(boolToJson(tslEnabled)) + ",";
        json += "\"tsl_listen_addr\":" + quoteJson(currentTslListenAddr()) + ",";
        json += "\"obs_url\":" + quoteJson(String(obsUrl)) + ",";
        json += "\"obs_password_set\":" + String(boolToJson(strlen(obsPassword) > 0)) + ",";
        json += "\"obs_enabled\":" + String(boolToJson(obsEnabled));
        json += "}";
        sendJson(200, json);
    });

    server.on("/api/v1/wireless/transport", HTTP_GET, []() {
        sendJson(200, buildWirelessTransportJson());
    });

    server.on("/api/v1/wireless/transport", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        String value = doc["transport"] | "";
        uint8_t mode = PHYTALLY_WIRELESS_TRANSPORT_AUTO;
        if (value.length() == 0 || !parseWirelessTransportMode(value, &mode)) {
            sendJsonMessage(400, "error", "Body must contain 'transport' set to 'auto', 'beacon_probe', or 'espnow'.");
            return;
        }

        String errorMessage;
        if (!applyWirelessTransportMode(mode, "api_wireless_transport", true, &errorMessage)) {
            sendJsonMessage(409, "error", errorMessage);
            return;
        }

        sendJson(200, String("{\"status\":\"ok\",\"message\":") +
            quoteJson("Wireless transport updated.") +
            ",\"transport\":" + buildWirelessTransportJson() + "}");
    });

    server.on("/api/v1/wireless/espnow/phy", HTTP_GET, []() {
        sendJson(200, buildEspNowPhyJson());
    });

    server.on("/api/v1/wireless/espnow/phy", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        String value = doc["mode"] | "";
        uint8_t mode = PHYTALLY_ESPNOW_PHY_11B_1M;
        if (value.length() == 0 || !parseEspNowPhyMode(value, &mode)) {
            sendJsonMessage(400, "error", "Body must contain 'mode' set to one of: lr_250k, lr_500k, 11b_1m, 11b_2m, 11b_11m, 11g_6m.");
            return;
        }

        String errorMessage;
        if (!applyEspNowPhyMode(mode, "api_espnow_phy", true, &errorMessage)) {
            sendJsonMessage(409, "error", errorMessage);
            return;
        }

        sendJson(200, String("{\"status\":\"ok\",\"message\":") +
            quoteJson("ESP-NOW PHY mode updated.") +
            ",\"espnow_phy\":" + buildEspNowPhyJson() + "}");
    });

    server.on("/api/v1/wifi/survey", HTTP_GET, []() {
        sendJson(200, buildWifiSurveyStatusJson());
    });

    server.on("/api/v1/wifi/survey", HTTP_POST, []() {
        if (!usesUsbNcmBackhaul() && !configMode) {
            sendJsonMessage(409, "error", "Wi-Fi survey is only available in configuration mode.");
            return;
        }
        if (!wifiSurveyInProgress && !wifiSurveyStartRequested) {
            wifiSurveyStartRequested = true;
        }
        sendJson(202, String("{\"status\":\"accepted\",\"message\":\"Wi-Fi survey queued.\",\"survey\":") +
            buildWifiSurveyStatusJson() + "}");
    });

    server.on("/api/v1/config", HTTP_PUT, []() {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));
        if (error) {
            sendJsonMessage(400, "error", "Invalid JSON body.");
            return;
        }

        if (!doc["hostname"].isNull()) {
            String value = sanitizeHostnameLabel(doc["hostname"].as<String>());
            value.toCharArray(hostnameOverride, sizeof(hostnameOverride));
            preferences.putString("hostname", value);
        }
        if (!doc["radio_channel"].isNull()) {
            int value = doc["radio_channel"].as<int>();
            if (value >= 1 && value <= 13) {
                String errorMessage;
                if (!applyRadioChannel(static_cast<uint8_t>(value), true, &errorMessage)) {
                    sendJsonMessage(409, "error", errorMessage);
                    return;
                }
            } else {
                sendJsonMessage(400, "error", "Invalid radio_channel.");
                return;
            }
        }
        if (!doc["wireless_transport"].isNull()) {
            String value = doc["wireless_transport"].as<String>();
            uint8_t mode = PHYTALLY_WIRELESS_TRANSPORT_AUTO;
            if (!parseWirelessTransportMode(value, &mode)) {
                sendJsonMessage(400, "error", "Invalid wireless_transport.");
                return;
            }
            String errorMessage;
            if (!applyWirelessTransportMode(mode, "config_update", true, &errorMessage)) {
                sendJsonMessage(409, "error", errorMessage);
                return;
            }
        }
        if (!doc["espnow_phy_mode"].isNull()) {
            String value = doc["espnow_phy_mode"].as<String>();
            uint8_t mode = PHYTALLY_ESPNOW_PHY_11B_1M;
            if (!parseEspNowPhyMode(value, &mode)) {
                sendJsonMessage(400, "error", "Invalid espnow_phy_mode.");
                return;
            }
            String errorMessage;
            if (!applyEspNowPhyMode(mode, "config_update", true, &errorMessage)) {
                sendJsonMessage(409, "error", errorMessage);
                return;
            }
        }
        if (!doc["route_priority"].isNull()) {
            String value = doc["route_priority"].as<String>();
            routePriority = value == "espnow_first" ? PHYTALLY_ROUTE_ESPNOW_FIRST : PHYTALLY_ROUTE_BEACON_FIRST;
            preferences.putUChar("routePriority", routePriority);
            if (wirelessTransportMode == PHYTALLY_WIRELESS_TRANSPORT_AUTO) {
                setActiveTransport(desiredTransportForMode(wirelessTransportMode), "route_priority_update", true);
            }
        }
        if (!doc["atem_ip"].isNull()) {
            String value = doc["atem_ip"].as<String>();
            value.toCharArray(atemIpStr, sizeof(atemIpStr));
            preferences.putString("atemIp", value);
        }
        if (!doc["vmix_ip"].isNull()) {
            String value = doc["vmix_ip"].as<String>();
            value.toCharArray(vmixIpStr, sizeof(vmixIpStr));
            preferences.putString("vmixIp", value);
        }
        if (!doc["atem_enabled"].isNull()) {
            atemEnabled = doc["atem_enabled"].as<bool>();
            preferences.putBool("atemEnabled", atemEnabled);
        }
        if (!doc["vmix_enabled"].isNull()) {
            vmixEnabled = doc["vmix_enabled"].as<bool>();
            preferences.putBool("vmixEnabled", vmixEnabled);
        }
        if (!doc["obs_url"].isNull()) {
            String value = normalizeObsUrl(doc["obs_url"].as<String>());
            value.toCharArray(obsUrl, sizeof(obsUrl));
            preferences.putString("obsUrl", value);
            setObsDisconnected(String());
        }
        if (!doc["obs_password"].isNull()) {
            String value = doc["obs_password"].as<String>();
            value.toCharArray(obsPassword, sizeof(obsPassword));
            preferences.putString("obsPass", value);
            setObsDisconnected(String());
        }
        if (!doc["obs_enabled"].isNull()) {
            obsEnabled = doc["obs_enabled"].as<bool>();
            preferences.putBool("obsEnabled", obsEnabled);
            if (!obsEnabled) {
                setObsDisconnected(String());
            }
        }
        if (!doc["tsl_enabled"].isNull()) {
            tslEnabled = doc["tsl_enabled"].as<bool>();
            preferences.putBool("tslEnabled", tslEnabled);
        }
        if (!doc["tsl_listen_port"].isNull()) {
            tslListenPort = static_cast<uint16_t>(doc["tsl_listen_port"].as<int>());
            preferences.putUShort("tslPort", tslListenPort);
        }
        if (!doc["tsl_listen_addr"].isNull()) {
            uint16_t parsedPort = 0;
            if (parseListenPortValue(doc["tsl_listen_addr"].as<String>(), &parsedPort)) {
                tslListenPort = parsedPort;
                preferences.putUShort("tslPort", tslListenPort);
            }
        }

        disableSimulationForLiveSwitcher("config_update");
        sendJsonMessage(200, "ok", "Configuration applied.");
    });

    server.on("/api/v1", HTTP_GET, []() {
        sendJson(200, buildDiscoveryJson());
    });

    server.on("/dashboard.css", HTTP_GET, []() {
        sendUiAsset("/dashboard.css", "text/css", &uiDashboardCss);
    });

    server.on("/dashboard.js", HTTP_GET, []() {
        sendUiAsset("/dashboard.js", "application/javascript", &uiDashboardJs);
    });

    server.on("/ui-selftest.txt", HTTP_GET, []() {
        server.send(200, "text/plain", "phytally-usb-web-selftest\n");
    });

    server.on("/ui-size-test", HTTP_GET, []() {
        int requested = server.hasArg("bytes") ? server.arg("bytes").toInt() : 1024;
        if (requested < 1) {
            requested = 1;
        }
        if (requested > 65536) {
            requested = 65536;
        }
        String body;
        body.reserve(static_cast<unsigned int>(requested));
        for (int i = 0; i < requested; ++i) {
            body += 'A';
        }
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain", body);
    });

    server.onNotFound([]() {
        sendJsonMessage(404, "error", "Not found.");
    });
    #else
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendUiAsset(request, "/index.html", "text/html", &uiIndexHtml);
    });

    server.on("/api/v1/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, buildStatusJson());
    });

    server.on("/api/v1/receivers", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, String("{\"receivers\":") + buildReceiversArrayJson() + "}");
    });

    server.on("/api/v1/simulation", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error || !doc["enabled"].is<bool>()) {
            sendJsonMessage(request, 400, "error", "Body must contain boolean field 'enabled'.");
            return;
        }

        testMode = doc["enabled"].as<bool>();
        if (!testMode) {
            memset(tallyStates, 0, MAX_TALLIES);
        }
        lastTestStep = 0;
        updateNativeVendorIE();
        updateTxBoardIndicator();
        sendJsonMessage(request, 200, "ok", testMode ? "Simulation enabled." : "Simulation disabled.");
    });

    server.on("/api/v1/receivers/assign-id", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        int tallyId = doc["tally_id"] | 0;
        if (mac.length() == 0 || tallyId < 1 || tallyId > MAX_TALLIES) {
            sendJsonMessage(request, 400, "error", "Body must contain 'mac' and 1-based 'tally_id'.");
            return;
        }
        if (!queueRemoteIdCommand(mac, (uint8_t)tallyId)) {
            sendJsonMessage(request, 400, "error", "Invalid MAC or tally_id.");
            return;
        }

        sendJson(request, 202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("SET_ID queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) +
            ",\"tally_id\":" + String(tallyId) + "}");
    });

    server.on("/api/v1/receivers/set-brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        uint8_t rawBrightness = 0;
        uint8_t brightnessPercent = 0;
        if (mac.length() == 0 || !parseBrightnessPayload(doc, &rawBrightness, &brightnessPercent)) {
            sendJsonMessage(request, 400, "error", "Body must contain 'mac' and either 'brightness' 0..255 or 'brightness_percent' 0..100.");
            return;
        }
        if (!queueRemoteBrightnessCommand(mac, rawBrightness)) {
            sendJsonMessage(request, 400, "error", "Invalid MAC or brightness.");
            return;
        }
        rememberReceiverBrightnessPercent(mac, brightnessPercent);
        sendJson(request, 202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("SET_BRIGHTNESS queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) +
            ",\"brightness\":" + String(rawBrightness) +
            ",\"brightness_percent\":" + String(brightnessPercent) + "}");
    });

    server.on("/api/v1/receivers/identify", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        String mac = doc["mac"] | "";
        if (mac.length() == 0) {
            sendJsonMessage(request, 400, "error", "Body must contain 'mac'.");
            return;
        }
        if (!queueRemoteIdentifyCommand(mac)) {
            sendJsonMessage(request, 400, "error", "Invalid MAC.");
            return;
        }
        sendJson(request, 202, String("{\"status\":\"queued\",\"message\":") +
            quoteJson("IDENTIFY queued for " + mac) +
            ",\"mac\":" + quoteJson(mac) + "}");
    });

    server.on("/api/v1/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"last_survey_at_ms\":" + String(lastWifiSurveyAt) + ",";
        json += "\"wireless_transport\":" + quoteJson(String(wirelessTransportModeName(wirelessTransportMode))) + ",";
        json += "\"espnow_phy_mode\":" + quoteJson(String(espNowPhyModeName(espNowPhyMode))) + ",";
        json += "\"radio_channel\":" + String(getCurrentRadioChannel()) + ",";
        json += "\"atem_ip\":" + quoteJson(String(atemIpStr)) + ",";
        json += "\"atem_enabled\":" + String(boolToJson(atemEnabled)) + ",";
        json += "\"vmix_ip\":" + quoteJson(String(vmixIpStr)) + ",";
        json += "\"vmix_enabled\":" + String(boolToJson(vmixEnabled)) + ",";
        json += "\"tsl_enabled\":" + String(boolToJson(tslEnabled)) + ",";
        json += "\"tsl_listen_addr\":" + quoteJson(currentTslListenAddr()) + ",";
        json += "\"obs_url\":" + quoteJson(String(obsUrl)) + ",";
        json += "\"obs_password_set\":" + String(boolToJson(strlen(obsPassword) > 0)) + ",";
        json += "\"obs_enabled\":" + String(boolToJson(obsEnabled));
        json += "}";
        sendJson(request, 200, json);
    });

    server.on("/api/v1/wireless/transport", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, buildWirelessTransportJson());
    });

    server.on("/api/v1/wireless/transport", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        String value = doc["transport"] | "";
        uint8_t mode = PHYTALLY_WIRELESS_TRANSPORT_AUTO;
        if (value.length() == 0 || !parseWirelessTransportMode(value, &mode)) {
            sendJsonMessage(request, 400, "error", "Body must contain 'transport' set to 'auto', 'beacon_probe', or 'espnow'.");
            return;
        }

        String errorMessage;
        if (!applyWirelessTransportMode(mode, "api_wireless_transport", true, &errorMessage)) {
            sendJsonMessage(request, 409, "error", errorMessage);
            return;
        }

        sendJson(request, 200, String("{\"status\":\"ok\",\"message\":") +
            quoteJson("Wireless transport updated.") +
            ",\"transport\":" + buildWirelessTransportJson() + "}");
    });

    server.on("/api/v1/wireless/espnow/phy", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, buildEspNowPhyJson());
    });

    server.on("/api/v1/wireless/espnow/phy", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        String value = doc["mode"] | "";
        uint8_t mode = PHYTALLY_ESPNOW_PHY_11B_1M;
        if (value.length() == 0 || !parseEspNowPhyMode(value, &mode)) {
            sendJsonMessage(request, 400, "error", "Body must contain 'mode' set to one of: lr_250k, lr_500k, 11b_1m, 11b_2m, 11b_11m, 11g_6m.");
            return;
        }

        String errorMessage;
        if (!applyEspNowPhyMode(mode, "api_espnow_phy", true, &errorMessage)) {
            sendJsonMessage(request, 409, "error", errorMessage);
            return;
        }

        sendJson(request, 200, String("{\"status\":\"ok\",\"message\":") +
            quoteJson("ESP-NOW PHY mode updated.") +
            ",\"espnow_phy\":" + buildEspNowPhyJson() + "}");
    });

    server.on("/api/v1/wifi/survey", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, buildWifiSurveyStatusJson());
    });

    server.on("/api/v1/wifi/survey", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!usesUsbNcmBackhaul() && !configMode) {
            sendJsonMessage(request, 409, "error", "Wi-Fi survey is only available in configuration mode.");
            return;
        }
        if (!wifiSurveyInProgress && !wifiSurveyStartRequested) {
            wifiSurveyStartRequested = true;
        }
        sendJson(request, 202, String("{\"status\":\"accepted\",\"message\":\"Wi-Fi survey queued.\",\"survey\":") +
            buildWifiSurveyStatusJson() + "}");
    });

    server.on("/api/v1/config", HTTP_PUT, [](AsyncWebServerRequest *request) {
    }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        String *body = appendRequestBody(request, data, len, index, total);
        if (!body) return;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, *body);
        delete body;
        if (error) {
            sendJsonMessage(request, 400, "error", "Invalid JSON body.");
            return;
        }

        if (!doc["hostname"].isNull()) {
            String value = sanitizeHostnameLabel(doc["hostname"].as<String>());
            value.toCharArray(hostnameOverride, sizeof(hostnameOverride));
            preferences.putString("hostname", value);
        }
        if (!doc["radio_channel"].isNull()) {
            int value = doc["radio_channel"].as<int>();
            if (value >= 1 && value <= 13) {
                String errorMessage;
                if (!applyRadioChannel(static_cast<uint8_t>(value), true, &errorMessage)) {
                    sendJsonMessage(request, 409, "error", errorMessage);
                    return;
                }
            } else {
                sendJsonMessage(request, 400, "error", "Invalid radio_channel.");
                return;
            }
        }
        if (!doc["wireless_transport"].isNull()) {
            String value = doc["wireless_transport"].as<String>();
            uint8_t mode = PHYTALLY_WIRELESS_TRANSPORT_AUTO;
            if (!parseWirelessTransportMode(value, &mode)) {
                sendJsonMessage(request, 400, "error", "Invalid wireless_transport.");
                return;
            }
            String errorMessage;
            if (!applyWirelessTransportMode(mode, "config_update", true, &errorMessage)) {
                sendJsonMessage(request, 409, "error", errorMessage);
                return;
            }
        }
        if (!doc["espnow_phy_mode"].isNull()) {
            String value = doc["espnow_phy_mode"].as<String>();
            uint8_t mode = PHYTALLY_ESPNOW_PHY_11B_1M;
            if (!parseEspNowPhyMode(value, &mode)) {
                sendJsonMessage(request, 400, "error", "Invalid espnow_phy_mode.");
                return;
            }
            String errorMessage;
            if (!applyEspNowPhyMode(mode, "config_update", true, &errorMessage)) {
                sendJsonMessage(request, 409, "error", errorMessage);
                return;
            }
        }
        if (!doc["route_priority"].isNull()) {
            String value = doc["route_priority"].as<String>();
            routePriority = value == "espnow_first" ? PHYTALLY_ROUTE_ESPNOW_FIRST : PHYTALLY_ROUTE_BEACON_FIRST;
            preferences.putUChar("routePriority", routePriority);
            if (wirelessTransportMode == PHYTALLY_WIRELESS_TRANSPORT_AUTO) {
                setActiveTransport(desiredTransportForMode(wirelessTransportMode), "route_priority_update", true);
            }
        }
        if (!doc["atem_ip"].isNull()) {
            String value = doc["atem_ip"].as<String>();
            value.toCharArray(atemIpStr, sizeof(atemIpStr));
            preferences.putString("atemIp", value);
        }
        if (!doc["vmix_ip"].isNull()) {
            String value = doc["vmix_ip"].as<String>();
            value.toCharArray(vmixIpStr, sizeof(vmixIpStr));
            preferences.putString("vmixIp", value);
        }
        if (!doc["atem_enabled"].isNull()) {
            atemEnabled = doc["atem_enabled"].as<bool>();
            preferences.putBool("atemEnabled", atemEnabled);
        }
        if (!doc["vmix_enabled"].isNull()) {
            vmixEnabled = doc["vmix_enabled"].as<bool>();
            preferences.putBool("vmixEnabled", vmixEnabled);
        }
        if (!doc["obs_url"].isNull()) {
            String value = normalizeObsUrl(doc["obs_url"].as<String>());
            value.toCharArray(obsUrl, sizeof(obsUrl));
            preferences.putString("obsUrl", value);
            setObsDisconnected(String());
        }
        if (!doc["obs_password"].isNull()) {
            String value = doc["obs_password"].as<String>();
            value.toCharArray(obsPassword, sizeof(obsPassword));
            preferences.putString("obsPass", value);
            setObsDisconnected(String());
        }
        if (!doc["obs_enabled"].isNull()) {
            obsEnabled = doc["obs_enabled"].as<bool>();
            preferences.putBool("obsEnabled", obsEnabled);
            if (!obsEnabled) {
                setObsDisconnected(String());
            }
        }
        if (!doc["tsl_enabled"].isNull()) {
            tslEnabled = doc["tsl_enabled"].as<bool>();
            preferences.putBool("tslEnabled", tslEnabled);
        }
        if (!doc["tsl_listen_port"].isNull()) {
            tslListenPort = static_cast<uint16_t>(doc["tsl_listen_port"].as<int>());
            preferences.putUShort("tslPort", tslListenPort);
        }
        if (!doc["tsl_listen_addr"].isNull()) {
            uint16_t parsedPort = 0;
            if (parseListenPortValue(doc["tsl_listen_addr"].as<String>(), &parsedPort)) {
                tslListenPort = parsedPort;
                preferences.putUShort("tslPort", tslListenPort);
            }
        }

        disableSimulationForLiveSwitcher("config_update");
        sendJsonMessage(request, 200, "ok", "Configuration applied.");
    });

    server.on("/api/v1", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendJson(request, 200, buildDiscoveryJson());
    });

    server.on("/dashboard.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendUiAsset(request, "/dashboard.css", "text/css", &uiDashboardCss);
    });

    server.on("/dashboard.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        sendUiAsset(request, "/dashboard.js", "application/javascript", &uiDashboardJs);
    });

    server.on("/ui-selftest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "phytally-usb-web-selftest\n");
    });

    server.on("/ui-size-test", HTTP_GET, [](AsyncWebServerRequest *request) {
        int requested = request->hasArg("bytes") ? request->arg("bytes").toInt() : 1024;
        if (requested < 1) {
            requested = 1;
        }
        if (requested > 65536) {
            requested = 65536;
        }
        AsyncWebServerResponse *response = request->beginResponse(
            "text/plain",
            static_cast<size_t>(requested),
            [requested](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                if (index >= static_cast<size_t>(requested)) {
                    return 0;
                }
                size_t remaining = static_cast<size_t>(requested) - index;
                size_t chunkLen = remaining < maxLen ? remaining : maxLen;
                memset(buffer, 'A', chunkLen);
                return chunkLen;
            });
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        sendJsonMessage(request, 404, "error", "Not found.");
    });
    #endif

    if (usesUsbNcmBackhaul()) {
        Serial.println("SETUP checkpoint: routes registered");
    }

#if !PHYTALLY_USE_USB_NCM_BACKHAUL
    if (configMode) {
        server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
    }
#endif

#if PHYTALLY_USE_USB_NCM_BACKHAUL
    Serial.println("SETUP checkpoint: deferring server.start until USB-NCM link is up");
#else
    server.begin();
    httpServerStarted = true;
#endif
    if (usesUsbNcmBackhaul()) {
        Serial.println("SETUP checkpoint: server.begin returned");
        Serial.println("SETUP COMPLETE");
    }
}

void loop() {
    // Activity LED management
#if PHYTALLY_HAS_STATUS_LED
    if (ledOffTime > 0 && millis() > ledOffTime) {
        digitalWrite(STATUS_LED, LOW);
        ledOffTime = 0;
    }
#endif

    if (!usesUsbNcmBackhaul() || !usbNcmLoopTaskHandle) {
        usbncm::loop();
    }

    pumpPendingHubCommands();

    if (usesUsbNcmBackhaul()) {
        if (usbncm::hasIp()) {
            if (usbHttpServerEarliestAt == 0) {
                usbHttpServerEarliestAt = millis() + 3000;
                Serial.println("USB-NCM link is up; delaying HTTP server start by 3s");
            }
            if (!httpServerStarted && !httpServerStartQueued && millis() >= usbHttpServerEarliestAt) {
                httpServerStartQueued = true;
                startUsbHttpServer();
            }
        } else {
            usbHttpServerEarliestAt = 0;
        }
#if PHYTALLY_USE_USB_NCM_BACKHAUL
        if (httpServerStarted) {
            server.handleClient();
        }
#endif
    }

    handleWifiSurveyLoop();

    if (usesUsbNcmBackhaul()) {
        static bool logged100ms = false;
        static bool logged500ms = false;
        static bool logged1000ms = false;
        const unsigned long now = millis();
        if (!logged100ms && now >= 100) {
            logged100ms = true;
            Serial.println("LOOP heartbeat: reached 100ms");
        }
        if (!logged500ms && now >= 500) {
            logged500ms = true;
            Serial.println("LOOP heartbeat: reached 500ms");
        }
        if (!logged1000ms && now >= 1000) {
            logged1000ms = true;
            Serial.println("LOOP heartbeat: reached 1000ms");
        }
    }

    if (configMode) {
        dnsServer.processNextRequest();
    } else {
        if (testMode) {
            handleSimulation();
        } else {
            handleAtem();
            handleVmix();
            handleTSL();
            handleObs();
        }

        static unsigned long lastUpdate = 0;
        if (millis() - lastUpdate > 1000) {
            lastUpdate = millis();
            if (usesUsbNcmBackhaul()) {
                Serial.println("LOOP heartbeat: entering 1s maintenance tick");
            }
            
            // Refresh Vendor IE (Beacon/ProbeResp)
            updateNativeVendorIE();
            evaluateTransportRoute();
            refreshMdns();

            for (int i = 0; i < receiverCount; i++) {
                if (receivers[i].online && (millis() - receivers[i].lastSeen > 60000)) {
                    receivers[i].online = false;
                }
            }

            if (!usesUsbNcmBackhaul() && WiFi.status() == WL_CONNECTED) {
                int staChannel = WiFi.channel();
                if (staChannel >= 1 && staChannel <= 13) {
                    lastKnownStaChannel = staChannel;
                }
                uint8_t currentChan;
                wifi_second_chan_t secondChan;
                esp_wifi_get_channel(&currentChan, &secondChan);
                if (currentChan != staChannel) {
                    esp_wifi_set_channel(staChannel, WIFI_SECOND_CHAN_NONE);
                }
            } else if (!usesUsbNcmBackhaul() && strlen(wifiSsid) > 0 && millis() - lastStaReconnectAttempt > 5000) {
                lastStaReconnectAttempt = millis();
                Serial.println("Wi-Fi link lost. Retrying STA connection...");
                WiFi.reconnect();
            }
        }
    }
}
