#include "UsbNcmNetif.h"

#if defined(PHYTALLY_USE_USB_NCM_BACKHAUL) && PHYTALLY_USE_USB_NCM_BACKHAUL

#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <esp_mac.h>

extern "C" {
#include "class/net/net_device.h"
#include "apps/dhcpserver/dhcpserver.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#include "lwip/igmp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "tusb.h"
}

extern "C" {
uint8_t tud_network_mac_address[6] = {0};
}

namespace {

// Static addressing: ESP32 is 192.168.7.1/24, host gets 192.168.7.2+
static const ip4_addr_t USB_STATIC_IP  = { PP_HTONL(LWIP_MAKEU32(192, 168, 7, 1)) };
static const ip4_addr_t USB_STATIC_MASK = { PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0)) };
static const ip4_addr_t USB_STATIC_GW   = { PP_HTONL(LWIP_MAKEU32(192, 168, 7, 1)) };

bool s_started = false;
bool s_beginOk = false;
bool s_usbMounted = false;
bool s_usbEverMounted = false;
bool s_networkReady = false;
bool s_linkUp = false;
bool s_dhcpsRunning = false;
bool s_netifAdded = false;
bool s_descriptorLoaded = false;
bool s_interfaceEnabled = false;
String s_hostnameLabel;
String s_hostname;
String s_macString;
uint8_t s_interfaceStringIndex = 0;
uint8_t s_macStringIndex = 0;
uint16_t s_descriptorLength = 0;
String s_lastUsbEvent = "none";
unsigned long s_lastUsbEventAtMs = 0;
unsigned long s_lastDhcpChangeAtMs = 0;
unsigned long s_lastHealthLogAt = 0;
unsigned long s_lastTraceLogAt = 0;
unsigned long s_lastHostNotifyAtMs = 0;
String s_lastError;
netif s_usbNetif;
dhcps_t *s_dhcps = nullptr;
volatile unsigned long s_networkInitCount = 0;
volatile unsigned long s_networkRxCount = 0;
volatile unsigned long s_networkRxDroppedCount = 0;
volatile unsigned long s_networkInputErrorCount = 0;
volatile unsigned long s_networkTxCount = 0;
volatile unsigned long s_networkTxTimeoutCount = 0;
volatile unsigned long s_networkTxIfDownCount = 0;
QueueHandle_t s_receivedFrames = nullptr;
QueueHandle_t s_transmitFrames = nullptr;
constexpr UBaseType_t kRxFrameQueueDepth = 32;
constexpr UBaseType_t kTxFrameQueueDepth = 128;

struct UsbTxFrame {
  uint16_t length;
  uint8_t data[];
};

IPAddress ipFromNetif(const ip4_addr_t *addr) {
  if (!addr) {
    return IPAddress();
  }
  return IPAddress(ip4_addr1(addr), ip4_addr2(addr), ip4_addr3(addr), ip4_addr4(addr));
}

void setLastError(const String &error) {
  s_lastError = error;
  if (error.length() > 0) {
    Serial.println("USB-NCM ERROR: " + error);
  }
}

void clearLastError() {
  s_lastError = "";
}

const char *usbEventName(int32_t eventId) {
  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
      return "started";
    case ARDUINO_USB_STOPPED_EVENT:
      return "stopped";
    case ARDUINO_USB_SUSPEND_EVENT:
      return "suspend";
    case ARDUINO_USB_RESUME_EVENT:
      return "resume";
    default:
      return "unknown";
  }
}

void recordDhcpTransition(const char *stateLabel) {
  s_lastDhcpChangeAtMs = millis();
  Serial.printf("USB-NCM DHCP-server: %s\n", stateLabel);
}

void stopDhcpServer() {
  if (!s_dhcpsRunning || !s_dhcps) {
    return;
  }
  LOCK_TCPIP_CORE();
  dhcps_stop(s_dhcps, &s_usbNetif);
  UNLOCK_TCPIP_CORE();
  s_dhcpsRunning = false;
  recordDhcpTransition("stopped");
}

void startDhcpServer() {
  if (!s_netifAdded || s_dhcpsRunning || !s_dhcps) {
    if (!s_netifAdded) {
      setLastError("Cannot start DHCP server before USB netif is added.");
    }
    return;
  }
  LOCK_TCPIP_CORE();
  err_t err = dhcps_start(s_dhcps, &s_usbNetif, USB_STATIC_IP);
  if (err == ERR_OK) {
    // Register a no-op new-lease callback. The DHCP server calls this after
    // sending an ACK with no null check — leaving it unset (NULL) crashes at
    // PC=0x00000000 via udp_input when the host completes DHCP handshake.
    dhcps_set_new_lease_cb(s_dhcps, [](void *, uint8_t *, uint8_t *) {}, nullptr);
    s_dhcpsRunning = true;
  }
  UNLOCK_TCPIP_CORE();
  if (err == ERR_OK) {
    recordDhcpTransition("started (pool 192.168.7.2+)");
    return;
  }
  setLastError("dhcps_start failed with err=" + String(static_cast<int>(err)));
}

void setLinkState(bool up) {
  s_linkUp = up;
  tud_network_link_state(BOARD_TUD_RHPORT, up);
  if (up) {
    // Prime the first USB OUT transfer. Without an initial renew, the NCM
    // class never hands the first host packet to tud_network_recv_cb().
    tud_network_recv_renew();
  }
  Serial.printf("USB-NCM link state: %s\n", up ? "up" : "down");
  if (!s_netifAdded) {
    return;
  }
  LOCK_TCPIP_CORE();
  if (up) {
    netif_set_link_up(&s_usbNetif);
    netif_set_up(&s_usbNetif);
  } else {
    netif_set_link_down(&s_usbNetif);
  }
  UNLOCK_TCPIP_CORE();
  if (up) {
    startDhcpServer();
  } else {
    stopDhcpServer();
  }
}

void sendHostLinkNotifications() {
  s_lastHostNotifyAtMs = millis();
}

err_t usbLinkOutput(struct netif *, struct pbuf *p) {
  // tud_mounted() (not tud_ready()) so packets aren't dropped during the brief
  // window after USB enumeration where tud_ready() is still false.
  // Mirrors the fix in espressif/esp-idf#18079.
  if (!tud_mounted() || !s_linkUp) {
    s_networkTxIfDownCount++;
    return ERR_IF;
  }

  if (!p || p->tot_len == 0 || p->tot_len > CFG_TUD_NET_MTU || !s_transmitFrames) {
    s_networkTxTimeoutCount++;
    return ERR_BUF;
  }

  size_t frameSize = sizeof(UsbTxFrame) + p->tot_len;
  UsbTxFrame *frame = reinterpret_cast<UsbTxFrame *>(malloc(frameSize));
  if (!frame) {
    s_networkTxTimeoutCount++;
    return ERR_BUF;
  }
  frame->length = static_cast<uint16_t>(p->tot_len);
  if (pbuf_copy_partial(p, frame->data, frame->length, 0) != frame->length) {
    free(frame);
    s_networkTxTimeoutCount++;
    return ERR_BUF;
  }

  // linkoutput must NOT block: called from the TCPIP task while holding
  // the core lock. Queue the packet and let loop() drain it when TinyUSB has
  // room for another NTB.
  if (xQueueSend(s_transmitFrames, &frame, 0) != pdPASS) {
    free(frame);
    s_networkTxTimeoutCount++;
    return ERR_TIMEOUT;
  }

  return ERR_OK;
}

// USB NCM passes every packet to the host regardless of MAC address, so
// multicast group management is a no-op — just acknowledge the request.
err_t usbIgmpMacFilter(struct netif *, const ip4_addr_t *, enum netif_mac_filter_action) {
  return ERR_OK;
}

err_t usbNetifInit(struct netif *netif) {
  netif->name[0] = 'u';
  netif->name[1] = 'n';
  netif->mtu = CFG_TUD_NET_MTU;
  // NETIF_FLAG_IGMP enables IGMP/multicast so mDNS can join 224.0.0.251.
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;
  netif->hwaddr_len = sizeof(tud_network_mac_address);
  memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
  // The host-side USB-NCM interface uses tud_network_mac_address. The lwIP
  // netif must advertise a different MAC or the host may reject ARP replies
  // as self-originated and leave 192.168.7.1 unresolved.
  netif->hwaddr[5] ^= 0x01;
  netif->linkoutput = usbLinkOutput;
  netif->output = etharp_output;
  netif->igmp_mac_filter = usbIgmpMacFilter;
  return ERR_OK;
}

bool addUsbNetif() {
  if (s_netifAdded) {
    return true;
  }

  if (!s_receivedFrames) {
    s_receivedFrames = xQueueCreate(kRxFrameQueueDepth, sizeof(struct pbuf *));
    if (!s_receivedFrames) {
      setLastError("xQueueCreate failed for USB RX frame queue.");
      return false;
    }
  }
  if (!s_transmitFrames) {
    s_transmitFrames = xQueueCreate(kTxFrameQueueDepth, sizeof(UsbTxFrame *));
    if (!s_transmitFrames) {
      setLastError("xQueueCreate failed for USB TX frame queue.");
      return false;
    }
  }

  memset(&s_usbNetif, 0, sizeof(s_usbNetif));
  LOCK_TCPIP_CORE();
  netif *added = netif_add(
      &s_usbNetif,
      &USB_STATIC_IP, &USB_STATIC_MASK, &USB_STATIC_GW,
      nullptr, usbNetifInit, tcpip_input);
  if (!added) {
    UNLOCK_TCPIP_CORE();
    setLastError("netif_add failed for USB-NCM interface.");
    return false;
  }

  netif_set_hostname(&s_usbNetif, s_hostname.c_str());
  netif_set_default(&s_usbNetif);
  netif_set_up(&s_usbNetif);
  UNLOCK_TCPIP_CORE();
  s_netifAdded = true;
  Serial.printf("USB-NCM netif added: name=%c%c ip=192.168.7.1/24 hostname=%s\n",
                s_usbNetif.name[0],
                s_usbNetif.name[1],
                s_hostname.c_str());
  return true;
}

uint16_t loadNcmDescriptor(uint8_t *dst, uint8_t *itf) {
  uint8_t epNotif = tinyusb_get_free_in_endpoint();
  if (epNotif == 0) {
    s_descriptorLoaded = false;
    s_descriptorLength = 0;
    setLastError("No free IN endpoint for NCM notifications.");
    return 0;
  }
  uint8_t epIn = tinyusb_get_free_in_endpoint();
  if (epIn == 0) {
    s_descriptorLoaded = false;
    s_descriptorLength = 0;
    setLastError("No free IN endpoint for NCM data.");
    return 0;
  }
  uint8_t epOut = tinyusb_get_free_out_endpoint();
  if (epOut == 0) {
    s_descriptorLoaded = false;
    s_descriptorLength = 0;
    setLastError("No free OUT endpoint for NCM data.");
    return 0;
  }

  const uint8_t descriptor[] = {
    TUD_CDC_NCM_DESCRIPTOR(
      *itf,
      s_interfaceStringIndex,
      s_macStringIndex,
      static_cast<uint8_t>(0x80 | epNotif),
      64,
      epOut,
      static_cast<uint8_t>(0x80 | epIn),
      CFG_TUD_NET_ENDPOINT_SIZE,
      CFG_TUD_NET_MTU)
  };

  s_descriptorLength = sizeof(descriptor);
  s_descriptorLoaded = true;
  Serial.printf(
      "USB-NCM descriptor ready: itf=%u notif_ep=0x%02X out_ep=0x%02X in_ep=0x%02X len=%u\n",
      static_cast<unsigned>(*itf),
      static_cast<unsigned>(0x80 | epNotif),
      static_cast<unsigned>(epOut),
      static_cast<unsigned>(0x80 | epIn),
      static_cast<unsigned>(s_descriptorLength));
  *itf += 2;
  memcpy(dst, descriptor, sizeof(descriptor));
  return s_descriptorLength;
}

void handleUsbEvent(void *, esp_event_base_t eventBase, int32_t eventId, void *) {
  if (eventBase != ARDUINO_USB_EVENTS) {
    return;
  }

  s_lastUsbEvent = usbEventName(eventId);
  s_lastUsbEventAtMs = millis();
  Serial.printf("USB-NCM event: %s\n", s_lastUsbEvent.c_str());

  // Only update bools here — no lwIP calls. LOCK_TCPIP_CORE() deadlocks
  // in the ESP event loop task context. loop() drives setLinkState().
  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
      s_usbMounted = true;
      s_usbEverMounted = true;
      s_networkReady = true;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT:
      s_usbMounted = false;
      s_networkReady = false;
      break;
    default:
      break;
  }
}

}  // namespace

namespace usbncm {

bool begin(const String &hostnameLabel) {
  if (s_started) {
    return s_beginOk;
  }

  s_started = true;
  s_beginOk = false;
  s_hostnameLabel = hostnameLabel;
  s_hostname = hostnameLabel + ".local";
  clearLastError();

  esp_read_mac(tud_network_mac_address, ESP_MAC_WIFI_STA);
  tud_network_mac_address[0] |= 0x02;
  tud_network_mac_address[0] &= 0xFE;

  char macLabel[13];
  snprintf(
    macLabel,
    sizeof(macLabel),
    "%02X%02X%02X%02X%02X%02X",
    tud_network_mac_address[0],
    tud_network_mac_address[1],
    tud_network_mac_address[2],
    tud_network_mac_address[3],
    tud_network_mac_address[4],
    tud_network_mac_address[5]);
  s_macString = String(macLabel);
  Serial.printf("USB-NCM init: hostname=%s mac=%s\n", s_hostname.c_str(), s_macString.c_str());

  s_dhcps = dhcps_new();
  if (!s_dhcps) {
    setLastError("dhcps_new() failed.");
    return false;
  }

  s_interfaceStringIndex = tinyusb_add_string_descriptor("PhyTally USB-NCM");
  s_macStringIndex = tinyusb_add_string_descriptor(s_macString.c_str());
  Serial.printf("USB-NCM string descriptors: interface=%u mac=%u\n",
                static_cast<unsigned>(s_interfaceStringIndex),
                static_cast<unsigned>(s_macStringIndex));

  esp_err_t interfaceErr =
      tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, loadNcmDescriptor);
  s_interfaceEnabled = interfaceErr == ESP_OK;
  if (interfaceErr != ESP_OK) {
    setLastError("tinyusb_enable_interface failed with err=" + String(static_cast<int>(interfaceErr)));
    return false;
  }
  Serial.println("USB-NCM interface enabled; waiting for descriptor callback");

  if (!addUsbNetif()) {
    return false;
  }

  USB.productName("PhyTally Hub");
  USB.manufacturerName("PhyTally");
  USB.onEvent(handleUsbEvent);
  USB.begin();
  Serial.println("USB-NCM USB.begin() requested");
  s_beginOk = true;
  return true;
}

void loop() {
  if (!s_netifAdded) {
    return;
  }

  // Drive link state from s_usbMounted. All lwIP calls live here because
  // LOCK_TCPIP_CORE() deadlocks in the ESP event loop task context.
  // Also reconcile s_usbMounted with tud_mounted() as a fallback for
  // missed STARTED/STOPPED events (e.g. cable replug).
  if (s_beginOk) {
    const bool actualMounted = tud_mounted();
    if (actualMounted && !s_usbMounted && s_usbEverMounted) {
      s_usbMounted = true;
    }

    const bool linkReady = s_usbMounted;
    const unsigned long now = millis();
    const unsigned long txQueued = s_transmitFrames ? uxQueueMessagesWaiting(s_transmitFrames) : 0;
    if (now - s_lastTraceLogAt >= 1000) {
      s_lastTraceLogAt = now;
      Serial.printf(
          "USB-NCM trace: event=%s mounted=%s actual=%s ready=%s link=%s dhcp=%s init_cb=%lu rx_cb=%lu rx_drop=%lu in_err=%lu tx=%lu tx_q=%lu tx_to=%lu tx_if=%lu\n",
          s_lastUsbEvent.c_str(),
          s_usbMounted ? "true" : "false",
          actualMounted ? "true" : "false",
          s_networkReady ? "true" : "false",
          s_linkUp ? "true" : "false",
          s_dhcpsRunning ? "true" : "false",
          s_networkInitCount,
          s_networkRxCount,
          s_networkRxDroppedCount,
          s_networkInputErrorCount,
          s_networkTxCount,
          txQueued,
          s_networkTxTimeoutCount,
          s_networkTxIfDownCount);
    }

    UsbTxFrame *txFrame = nullptr;
    while (s_transmitFrames && xQueuePeek(s_transmitFrames, &txFrame, 0) == pdPASS) {
      if (!txFrame) {
        xQueueReceive(s_transmitFrames, &txFrame, 0);
        continue;
      }
      if (!tud_mounted() || !tud_network_can_xmit(txFrame->length)) {
        break;
      }
      if (xQueueReceive(s_transmitFrames, &txFrame, 0) != pdPASS) {
        continue;
      }
      s_networkTxCount++;
      tud_network_xmit(txFrame, txFrame->length);
      free(txFrame);
    }

    struct pbuf *pendingFrame = nullptr;
    while (s_receivedFrames && xQueueReceive(s_receivedFrames, &pendingFrame, 0) == pdPASS) {
      LOCK_TCPIP_CORE();
      err_t err = ethernet_input(pendingFrame, &s_usbNetif);
      UNLOCK_TCPIP_CORE();
      if (err != ERR_OK) {
        s_networkInputErrorCount++;
        static unsigned long s_lastInputErrorLogAt = 0;
        const unsigned long nowMs = millis();
        if (nowMs - s_lastInputErrorLogAt >= 1000) {
          s_lastInputErrorLogAt = nowMs;
          Serial.printf("USB-NCM input error: err=%d len=%u tot_len=%u\n",
                        static_cast<int>(err),
                        static_cast<unsigned>(pendingFrame->len),
                        static_cast<unsigned>(pendingFrame->tot_len));
        }
        pbuf_free(pendingFrame);
      }
      tud_network_recv_renew();
    }

    if (linkReady && !s_linkUp) {
      Serial.println("USB-NCM trace: promoting ready state to link up");
      setLinkState(true);
    } else if (!linkReady && s_linkUp) {
      Serial.println("USB-NCM trace: demoting link state to down");
      setLinkState(false);
    }

    if (s_linkUp && s_networkRxCount == 0 && now - s_lastHostNotifyAtMs >= 1000) {
      sendHostLinkNotifications();
    }
  }

  const unsigned long now = millis();

  if (now - s_lastHealthLogAt < 5000) {
    return;
  }
  s_lastHealthLogAt = now;

  if (s_beginOk && !s_usbMounted) {
    Serial.println("USB-NCM waiting for host USB enumeration. Check the native USB port and cable.");
    return;
  }

  if (s_usbMounted && !s_dhcpsRunning) {
    Serial.println("USB-NCM link is up but DHCP server is not running.");
  }
}

Status status() {
  Status current;
  current.enabled = true;
  current.initStarted = s_started;
  current.beginOk = s_beginOk;
  current.descriptorLoaded = s_descriptorLoaded;
  current.interfaceEnabled = s_interfaceEnabled;
  current.netifAdded = s_netifAdded;
  current.usbMounted = s_usbMounted;
  current.descriptorLoaded = s_descriptorLoaded;
  current.linkUp = s_linkUp;
  current.dhcpStarted = s_dhcpsRunning;
  current.hasIp = s_linkUp;  // static IP is always 192.168.7.1 when link is up
  current.descriptorLength = s_descriptorLength;
  current.hostname = s_hostname;
  current.mac = s_macString;
  current.lastUsbEvent = s_lastUsbEvent;
  current.lastUsbEventAtMs = s_lastUsbEventAtMs;
  current.lastDhcpChangeAtMs = s_lastDhcpChangeAtMs;
  current.lastError = s_lastError;
  LOCK_TCPIP_CORE();
  current.ip = ipFromNetif(netif_ip4_addr(&s_usbNetif));
  current.gateway = ipFromNetif(netif_ip4_gw(&s_usbNetif));
  current.netmask = ipFromNetif(netif_ip4_netmask(&s_usbNetif));
  UNLOCK_TCPIP_CORE();
  return current;
}

bool hasIp() {
  return s_linkUp;
}

IPAddress localIP() {
  return status().ip;
}

IPAddress gatewayIP() {
  return status().gateway;
}

IPAddress subnetMask() {
  return status().netmask;
}

String hostname() {
  return s_hostname;
}

String macString() {
  return s_macString;
}

}  // namespace usbncm

extern "C" bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
  if (!src || !size || !s_netifAdded) {
    return false;
  }

  s_networkRxCount++;
  if (!s_networkReady) {
    s_networkReady = true;
    Serial.println("USB-NCM received first host packet; data path is ready.");
  }

  if (!s_receivedFrames) {
    return false;
  }

  struct pbuf *packet = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
  if (!packet) {
    s_networkRxDroppedCount++;
    return false;
  }

  pbuf_take(packet, src, size);
  if (xQueueSend(s_receivedFrames, &packet, 0) != pdPASS) {
    s_networkRxDroppedCount++;
    pbuf_free(packet);
    return false;
  }
  return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t) {
  const UsbTxFrame *packet = reinterpret_cast<const UsbTxFrame *>(ref);
  if (!dst || !packet || packet->length == 0 || packet->length > CFG_TUD_NET_MTU) {
    return 0;
  }
  memcpy(dst, packet->data, packet->length);
  return packet->length;
}

extern "C" void tud_network_init_cb(void) {
  s_networkInitCount++;
  s_networkReady = true;
  Serial.printf("USB-NCM callback: tud_network_init_cb count=%lu mounted=%s\n",
                s_networkInitCount,
                tud_mounted() ? "true" : "false");
  // Called from the TinyUSB task (bus reset). Do NOT call lwIP functions here
  // (LOCK_TCPIP_CORE from TinyUSB task context risks deadlock). The loop()
  // tud_mounted() poll will detect the reconnect and call setLinkState(true).
}

#endif
