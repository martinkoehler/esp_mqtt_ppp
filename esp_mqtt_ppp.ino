/**
 * esp_mqtt_ppp_nat_telnet_health_refactored.ino
 *
 * ESP8266 (Wemos D1) roles:
 *  - SoftAP with simple web UI (config + client list)
 *  - PPPoS over UART0 (Serial) as WAN uplink
 *  - NAPT (NAT) on PPP interface for AP clients
 *  - Embedded TinyMqtt broker on port 1883
 *  - Telnet mirror for Serial1 logs on 192.168.4.1:2048
 *  - Lightweight health monitor (AP/PPP/NAT/WEB/MQTT) + periodic telemetry
 *  - Boot diagnostics (reset reason, exception registers)
 *
 * Architecture:
 *  - Configuration section (edit here)
 *  - TelnetLogger (wraps Serial1 and mirrors to a single telnet client)
 *  - Netif utilities (find/dump)
 *  - AP bring-up
 *  - PPP bring-up + callbacks
 *  - NAT enabling
 *  - Web UI
 *  - MQTT (time-sliced loop)
 *  - Health monitor (AP/PPP/MQTT/WEB/Telnet + telemetry)
 *  - setup()/loop()
 *
 * NOTES
 *  - PPP uses Serial (UART0) @ 115200.
 *  - Logs use Serial1 (UART1) @ 74880 mirrored to telnet:2048.
 *  - TinyMqtt broker listens on ALL interfaces:1883 (AP + PPP).
 *  - AP credentials are persisted in EEPROM.
 */

// ============================== Includes ===============================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "TinyMqtt.h"   // https://github.com/hsaturn/TinyMqtt

extern "C" {
  #include "lwip/opt.h"
  #include "lwip/err.h"
  #include "lwip/ip_addr.h"
  #include "lwip/netif.h"
  #include "netif/ppp/ppp.h"
  #include "netif/ppp/pppos.h"
  #include "lwip/etharp.h"
  #include "lwip/ip4.h"
  #include "lwip/ip4_addr.h"
  #include "lwip/napt.h"
  #include "user_interface.h"       // system_get_rst_info, wifi_* helpers
}

// ============================== Config =================================

// --- Serial / Telnet
static const unsigned long LOG_BAUD         = 74880;   // UART1 baud for logs
static const uint16_t      TELNET_PORT      = 2048;    // telnet log port
static const unsigned long PPP_BAUD         = 115200;  // UART0 baud (PPP)

// --- AP defaults (can be changed via Web UI and saved to EEPROM)
static const int           AP_CHANNEL       = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// --- MQTT
static const uint16_t      MQTT_PORT        = 1883;
#define MQTT_BURST_INTERVAL_MS   2          // broker loop slice period
#define MQTT_BURST_BUDGET_US     1200       // per-slice budget in µs

// --- EEPROM layout for AP SSID/PASS
#define EEPROM_SIZE 128
#define SSID_ADDR   0
#define PASS_ADDR   32
#define MAX_SSID    31
#define MAX_PASS    31

// --- NAT table sizes
#ifndef IP_NAPT_MAX
#define IP_NAPT_MAX     512
#endif
#ifndef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX  32
#endif

// --- Health monitor cadence
static const uint32_t HEALTH_EVERY_MS     = 3000;   // health run period
static const uint32_t TELEMETRY_EVERY_MS  = 30000;  // telemetry period

// ============================== Globals ================================

// AP credentials (persisted)
char ap_ssid[MAX_SSID+1] = "WemosD1";
char ap_pass[MAX_PASS+1] = "0187297154091575";

// NAT
static bool napt_inited = false;

// MQTT broker
MqttBroker broker(MQTT_PORT);
static unsigned long lastMQTTBurst = 0;
static unsigned long lastMQTTLoopTouchMs = 0; // liveness marker
static bool mqttEverBegan = false;

// Web server
ESP8266WebServer server(80);
static bool webEverBegan = false;

// PPP state
static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;   // pppos_create fills this

// Health monitor
static unsigned long lastHealthTickMs = 0;
static unsigned long lastTelemetryMs = 0;
static uint32_t      healthRuns = 0;

// ============================= Telnet Logger ===========================

#include <WiFiClient.h>
#include <WiFiServer.h>

/**
 * TelnetLogger
 *  - Presents a Print-compatible interface that mirrors writes to:
 *      - UART1 (hardware Serial1)
 *      - A single telnet client on 192.168.4.1:TELNET_PORT
 *  - Start with begin(baud), call loop() frequently (cheap).
 */
class TelnetLogger : public Print {
public:
  TelnetLogger(uint16_t port) : _server(port) {}

  void begin(unsigned long baud) {
    ::Serial1.begin(baud);
    _wantServer = true;               // start server once AP is up
  }

  void loop() {
    // Lazy-start server when AP exists
    if (_wantServer && !_serverStarted && (WiFi.getMode() & WIFI_AP)) {
      _server.begin();
      _server.setNoDelay(true);
      _serverStarted = true;
    }
    // Accept/rotate single client
    if (_serverStarted && _server.hasClient()) {
      if (_client && _client.connected()) _client.stop();
      _client = _server.available();
      _client.setNoDelay(true);
      _client.println("ESP8266 Serial1 log (Telnet). Close client to detach.");
    }
    // Cleanup dead client
    if (_client && !_client.connected()) {
      _client.stop();
    }
  }

  // Print proxy: mirror a byte/buffer to UART1 and telnet client
  size_t write(uint8_t b) override {
    ::Serial1.write(b);
    if (_client && _client.connected()) _client.write(&b, 1);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t size) override {
    ::Serial1.write(buf, size);
    if (_client && _client.connected()) _client.write(buf, size);
    return size;
  }
  using Print::write;

private:
  WiFiServer _server;
  WiFiClient _client;
  bool _serverStarted = false;
  bool _wantServer    = false;
};

// Replace Serial1 globally with the Telnet logger
TelnetLogger TelnetSerial1(TELNET_PORT);
#define Serial1 TelnetSerial1

// ============================= Netif Utils =============================

/** Dump all netifs: #num, name, IPv4, GW, MASK, flags */
void dump_netifs(const char* tag) {
  Serial1.printf("[NETIF] %s\n", tag);
  for (netif* n = netif_list; n; n = n->next) {
    ip4_addr_t ip = *netif_ip4_addr(n);
    ip4_addr_t gw = *netif_ip4_gw(n);
    ip4_addr_t mk = *netif_ip4_netmask(n);
    Serial1.printf("  #%u %c%c  ip=%s gw=%s mask=%s flags=0x%02x\n",
                   n->num, n->name[0], n->name[1],
                   ipaddr_ntoa(&ip), ipaddr_ntoa(&gw), ipaddr_ntoa(&mk),
                   n->flags);
  }
}

/** Find the PPP netif (lwIP name "pp") */
netif* findPPP() {
  for (netif* n = netif_list; n; n = n->next) {
    if (n->name[0] == 'p' && n->name[1] == 'p') return n;
  }
  return nullptr;
}

// ============================== AP Bring-up ============================

/** Load persisted AP SSID/PASS from EEPROM (or defaults if blank) */
void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ap_ssid);
  EEPROM.get(PASS_ADDR, ap_pass);
  if (ap_ssid[0] == 0xFF || ap_ssid[0] == '\0') strcpy(ap_ssid, "WemosD1");
  if (ap_pass[0] == 0xFF || ap_pass[0] == '\0') strcpy(ap_pass, "0187297154091575");
}

/** Persist AP SSID/PASS to EEPROM */
void saveAPConfig(const char* ssid, const char* pass) {
  memset(ap_ssid, 0, sizeof(ap_ssid));
  memset(ap_pass, 0, sizeof(ap_pass));
  strncpy(ap_ssid, ssid, MAX_SSID);
  strncpy(ap_pass, pass, MAX_PASS);
  EEPROM.put(SSID_ADDR, ap_ssid);
  EEPROM.put(PASS_ADDR, ap_pass);
  EEPROM.commit();
}

/** Configure and start SoftAP (idempotent) */
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHANNEL, false, 4)) {
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n",
                   ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  // Keep Wi-Fi awake for responsiveness
  wifi_set_sleep_type(NONE_SLEEP_T);
  dump_netifs("after softAP");
}

// ========================= PPP / NAT Bring-up =========================

/** PPP lower-layer output: write to UART0 (Serial) */
static u32_t ppp_output_cb(ppp_pcb *, u8_t *data, u32_t len, void *) {
  return Serial.write(data, len); // PPP link = Serial (UART0)
}

/** Enable NAPT on the PPP netif (safe to re-assert) */
static void enable_nat_on_ppp_if_available() {
  netif* n = findPPP();
  if (!n) {
    Serial1.println("[NAT] PPP netif not found yet");
    return;
  }
  if (!napt_inited) {
    ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);
    napt_inited = true;
    Serial1.printf("[NAT] napt_init done (max=%d portmaps=%d)\n", IP_NAPT_MAX, IP_PORTMAP_MAX);
  }
  err_t er = ip_napt_enable_no(n->num, 1);
  Serial1.printf("[NAT] ip_napt_enable_no(ppp #%u) -> %d\n", n->num, (int)er);
  if (er == ERR_OK) {
    Serial1.printf("[NAT] enabled on PPP address %s\n", ipaddr_ntoa(netif_ip_addr4(n)));
  }
}

/** PPP status callback: log, enable NAT, reconnect on error */
static void ppp_status_cb(ppp_pcb *, int err_code, void *) {
  if (err_code == PPPERR_NONE) {
    const ip_addr_t* ip = netif_ip_addr4(&ppp_netif);
    const ip_addr_t* gw = netif_ip_gw4(&ppp_netif);
    const ip_addr_t* mk = netif_ip_netmask4(&ppp_netif);
    Serial1.printf("[PPP] UP  ip=%s gw=%s mask=%s\n",
                   ipaddr_ntoa(ip), ipaddr_ntoa(gw), ipaddr_ntoa(mk));
    dump_netifs("PPP UP (before NAPT)");
    enable_nat_on_ppp_if_available();
    dump_netifs("PPP UP (after  NAPT)");
  } else {
    Serial1.printf("[PPP] err=%d -> reconnect\n", err_code);
    ppp_connect(ppp, 0);
  }
}

/** Create/connect PPPoS on Serial (default route) */
void setupPPP() {
  Serial.begin(PPP_BAUD);   // PPP link over UART0
  delay(50);

  ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, nullptr);
  if (!ppp) {
    Serial1.println("[PPP] create FAILED");
    return;
  }
#if defined(PPPAUTHTYPE_NONE)
  ppp_set_auth(ppp, PPPAUTHTYPE_NONE, "", "");
#endif
  ppp_set_default(ppp);   // make PPP the default route
  ppp_connect(ppp, 0);
  Serial1.println("[PPP] connecting...");
}

// ================================ Web UI ==============================

/** Simple status/config page */
void handleRoot() {
  String html = "<html><head><title>Wemos AP/PPP NAT</title></head><body>";
  html += "<h1>Wemos AP/PPP NAT</h1>";
  html += "<p>AP SSID: " + String(ap_ssid) + "</p>";

  html += "<h2>Connected AP Clients</h2><ul>";
  struct station_info *stat_info = wifi_softap_get_station_info();
  while (stat_info) {
    IPAddress ip = IPAddress(stat_info->ip.addr);
    html += "<li>" + ip.toString() + "</li>";
    stat_info = STAILQ_NEXT(stat_info, next);
  }
  wifi_softap_free_station_info();
  html += "</ul>";

  html += "<h2>Set AP Parameters</h2>";
  html += "<form method='POST' action='/setap'>";
  html += "SSID: <input name='ssid' value='" + String(ap_ssid) + "'><br>";
  html += "Password: <input name='pass' value='" + String(ap_pass) + "'><br>";
  html += "<input type='submit' value='Save & Reboot'>";
  html += "</form>";

  html += "<h2>Reset</h2>";
  html += "<form method='POST' action='/reset'><input type='submit' value='Reboot'></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

/** Save AP config and reboot */
void handleSetAP() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    saveAPConfig(server.arg("ssid").c_str(), server.arg("pass").c_str());
    server.send(200, "text/html", "<html><body><h1>Saved. Rebooting...</h1></body></html>");
    delay(500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing ssid/pass");
  }
}

/** Manual reboot */
void handleReset() {
  saveAPConfig(ap_ssid, ap_pass); // persist current values (no-op if unchanged)
  server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
  delay(500);
  ESP.restart();
}

/** Bind routes and start HTTP server */
void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setap", HTTP_POST, handleSetAP);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  webEverBegan = true;
  Serial1.println("[WEB] HTTP server started on port 80");
}

// ================================ MQTT ================================

/** Time-sliced broker loop to keep Wi-Fi/PPP latency low */
static inline void serviceMQTT_timesliced() {
  const unsigned long now = millis();
  if ((uint32_t)(now - lastMQTTBurst) >= MQTT_BURST_INTERVAL_MS) {
    const uint32_t deadline = micros() + MQTT_BURST_BUDGET_US;
    do {
      broker.loopWithBudget(MQTT_BURST_BUDGET_US);
      yield();                           // feed Wi-Fi/WDT
    } while ((int32_t)(deadline - micros()) > 0);
    lastMQTTBurst = now;
    lastMQTTLoopTouchMs = now;           // liveness marker
  }
}

/** Start MQTT broker on all interfaces */
void setupMQTT() {
  broker.begin();  // IP_ANY:MQTT_PORT
  mqttEverBegan = true;
  lastMQTTLoopTouchMs = millis();
  Serial1.printf("[MQTT] TinyMqtt broker on :%u (AP+PPP)\n", MQTT_PORT);
}

// ============================= Health Monitor =========================

/** Human-readable reset reason text */
const char* rstReasonToStr(uint32_t r) {
  switch (r) {
    case 0:  return "REASON_DEFAULT_RST";
    case 1:  return "REASON_WDT_RST";
    case 2:  return "REASON_EXCEPTION_RST";
    case 3:  return "REASON_SOFT_WDT_RST";
    case 4:  return "REASON_SOFT_RESTART";
    case 5:  return "REASON_DEEP_SLEEP_AWAKE";
    case 6:  return "REASON_EXT_SYS_RST";
    default: return "UNKNOWN";
  }
}

/** (Re)assert AP if needed; also re-init if IP went 0.0.0.0 */
static void ensureAPUp() {
  const bool apMode = (WiFi.getMode() & WIFI_AP);
  if (!apMode) {
    Serial1.println("[HEALTH][AP] AP mode lost -> reconfig");
    setupAP();
    return;
  }
  if (WiFi.softAPIP() == IPAddress((uint32_t)0)) {
    Serial1.println("[HEALTH][AP] AP IP invalid -> reconfig");
    setupAP();
  }
}

/** Ensure PPP is present/up; reconnect or recreate as needed; pin NAT */
static void ensurePPPUp() {
  netif* n = findPPP();
  if (!ppp) {
    Serial1.println("[HEALTH][PPP] ppp is null -> recreate");
    setupPPP();
    return;
  }
  if (!n) {
    Serial1.println("[HEALTH][PPP] PPP netif missing -> reconnect");
    ppp_connect(ppp, 0);
    return;
  }
  if (!netif_is_up(n)) {
    Serial1.println("[HEALTH][PPP] netif down -> reconnect");
    ppp_connect(ppp, 0);
    return;
  }
  // Keep NAT pinned to PPP (idempotent)
  enable_nat_on_ppp_if_available();
}

/** Restart MQTT if its loop hasn't been serviced for >10s (heuristic) */
static void ensureMQTTUp() {
  if (!mqttEverBegan) return;
  const unsigned long now = millis();
  if ((uint32_t)(now - lastMQTTLoopTouchMs) > 10000) {
    Serial1.println("[HEALTH][MQTT] no loop activity >10s -> restart broker");
    broker.begin();
    lastMQTTLoopTouchMs = now;
  }
}

/** Web server: idempotent re-assert begin every N health cycles */
static void ensureWebUp() {
  if (!webEverBegan) return;
  static uint8_t counter = 0;
  if ((++counter % 10) == 0) {     // ~ every 30s at default cadence
    Serial1.println("[HEALTH][WEB] re-assert server.begin()");
    server.begin();
  }
}

/** Telnet server/client upkeep (cheap) */
static void ensureTelnetUp() {
  Serial1.loop();
}

/** Periodic telemetry: heap, fragmentation, AP/PPP state, netifs. */
static void logTelemetry() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t maxBlk   = ESP.getMaxFreeBlockSize();
  uint8_t  frag     = ESP.getHeapFragmentation();
  int staCount      = wifi_softap_get_station_num();

  netif* p = findPPP();
  const char* pppState = (p && netif_is_up(p)) ? "UP" : "DOWN";
  const char* apState  = (WiFi.getMode() & WIFI_AP) ? "UP" : "DOWN";

  Serial1.printf("[TEL] up=%lus heap=%lu maxblk=%lu frag=%u%% AP=%s STA=%d PPP=%s IP=%s\n",
    millis()/1000UL,
    (unsigned long)freeHeap,
    (unsigned long)maxBlk,
    (unsigned)frag,
    apState,
    staCount,
    pppState,
    p ? ipaddr_ntoa(netif_ip4_addr(p)) : "0.0.0.0"
  );
  dump_netifs("periodic");
}

/** Run all health checks on a fixed cadence (non-blocking) */
static void serviceHealth() {
  const unsigned long now = millis();
  if ((uint32_t)(now - lastHealthTickMs) < HEALTH_EVERY_MS) return;
  lastHealthTickMs = now;
  healthRuns++;

  ensureAPUp();
  ensurePPPUp();
  ensureWebUp();
  ensureMQTTUp();
  ensureTelnetUp();

  if ((uint32_t)(now - lastTelemetryMs) >= TELEMETRY_EVERY_MS) {
    lastTelemetryMs = now;
    logTelemetry();
  }
}

// ============================ Service Loops ============================

/** Feed PPP input from Serial (UART0) into lwIP PPPoS */
static inline void servicePPP() {
  if (!ppp) return;
  static uint8_t buf[512];
  int avail = Serial.available();
  if (avail > 0) {
    int n = Serial.readBytes(buf, (avail > (int)sizeof(buf)) ? sizeof(buf) : avail);
    if (n > 0) {
      pppos_input(ppp, buf, n);
      if (avail > 128) delay(0); // relieve WDT on big bursts
    }
  }
}

/** Handle Web requests (cheap if no client) */
static inline void serviceHTTP() {
  server.handleClient();
}

// ============================== Arduino ================================

void setup() {
  // Start logging (mirrored to telnet)
  Serial1.begin(LOG_BAUD);
  delay(100);

  // Defensive Wi-Fi settings
  WiFi.persistent(false);             // reduce flash wear
  wifi_set_sleep_type(NONE_SLEEP_T);  // keep Wi-Fi responsive

  // Load AP config and bring everything up
  loadAPConfig();
  setupAP();
  setupPPP();
  setupMQTT();
  setupWeb();

  // Boot diagnostics: reset reason + exception registers if relevant
  struct rst_info* ri = system_get_rst_info();
  Serial1.printf("[BOOT] ResetReason=%s (%u)\n", rstReasonToStr(ri->reason), ri->reason);
  if (ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_WDT_RST || ri->reason == REASON_SOFT_WDT_RST) {
    Serial1.printf("[BOOT] exccause=%u epc1=%08x epc2=%08x epc3=%08x excvaddr=%08x depc=%08x\n",
                   ri->exccause, ri->epc1, ri->epc2, ri->epc3, ri->excvaddr, ri->depc);
  }
  Serial1.printf("[BOOT] SDK:%s CPU:%uMHz Flash:%uKB Sketch:%uKB FreeSketch:%uKB\n",
                 system_get_sdk_version(), ESP.getCpuFreqMHz(),
                 ESP.getFlashChipSize()/1024, ESP.getSketchSize()/1024, ESP.getFreeSketchSpace()/1024);

  // Initialize scheduling anchors
  lastMQTTBurst     = millis();
  lastMQTTLoopTouchMs = lastMQTTBurst;
  lastHealthTickMs  = lastMQTTBurst;
  lastTelemetryMs   = lastMQTTBurst;
}

void loop() {
  servicePPP();               // feed PPPoS
  serviceHTTP();              // serve HTTP
  serviceMQTT_timesliced();   // time-sliced MQTT broker
  Serial1.loop();             // telnet upkeep (cheap)
  serviceHealth();            // AP/PPP/NAT/WEB/MQTT checks + telemetry
  yield();                    // feed Wi-Fi/WDT
}
