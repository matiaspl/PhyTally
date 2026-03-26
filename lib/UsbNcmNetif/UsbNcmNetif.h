#ifndef USB_NCM_NETIF_H
#define USB_NCM_NETIF_H

#include <Arduino.h>

#if defined(PHYTALLY_USE_USB_NCM_BACKHAUL) && PHYTALLY_USE_USB_NCM_BACKHAUL

namespace usbncm {

struct Status {
  bool enabled;
  bool initStarted;
  bool beginOk;
  bool descriptorLoaded;
  bool interfaceEnabled;
  bool netifAdded;
  bool usbMounted;
  bool linkUp;
  bool dhcpStarted;
  bool hasIp;
  uint16_t descriptorLength;
  IPAddress ip;
  IPAddress gateway;
  IPAddress netmask;
  String hostname;
  String mac;
  String lastUsbEvent;
  unsigned long lastUsbEventAtMs;
  unsigned long lastDhcpChangeAtMs;
  String lastError;
};

bool begin(const String &hostnameLabel);
void loop();
Status status();
bool hasIp();
IPAddress localIP();
IPAddress gatewayIP();
IPAddress subnetMask();
String hostname();
String macString();

}  // namespace usbncm

#else

namespace usbncm {

struct Status {
  bool enabled = false;
  bool initStarted = false;
  bool beginOk = false;
  bool descriptorLoaded = false;
  bool interfaceEnabled = false;
  bool netifAdded = false;
  bool usbMounted = false;
  bool linkUp = false;
  bool dhcpStarted = false;
  bool hasIp = false;
  uint16_t descriptorLength = 0;
  IPAddress ip;
  IPAddress gateway;
  IPAddress netmask;
  String hostname;
  String mac;
  String lastUsbEvent;
  unsigned long lastUsbEventAtMs = 0;
  unsigned long lastDhcpChangeAtMs = 0;
  String lastError;
};

inline bool begin(const String &) {
  return false;
}

inline void loop() {}

inline Status status() {
  return Status{};
}

inline bool hasIp() {
  return false;
}

inline IPAddress localIP() {
  return IPAddress();
}

inline IPAddress gatewayIP() {
  return IPAddress();
}

inline IPAddress subnetMask() {
  return IPAddress();
}

inline String hostname() {
  return String();
}

inline String macString() {
  return String();
}

}  // namespace usbncm

#endif

#endif
