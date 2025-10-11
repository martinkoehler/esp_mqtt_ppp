/**
 * esp_mqtt_ppp_web_clean_with_clients.ino
 *
 * ESP8266 (Wemos D1):
 *  - Optional SoftAP + simple web UI (status, AP client IPs, config, boot diagnostics, telemetry)
 *  - PPPoS over UART0 (Serial) as WAN uplink
 *  - TinyMqtt broker on :1883 with time-sliced loop
 *  - Health monitor + PPP reconnect backoff (no work in PPP callbacks)
 *
 * Notes (no NAT):
 *  - The device is a PPP client; it does not NAT/forward AP clients to PPP.
 *  - Use AP for setup/telemetry only, or add a route for 192.168.4.0/24 on the PPP peer.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "TinyMqtt.h"

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
  #include "user_interface.h"
}

// ============================== Build-time config ==============================

// Set to 1 to include AP + web config for SSID/password; set to 0 to strip AP code.
#ifndef AP_ENABLE
#define AP_ENABLE 1
#endif

// ============================== Runtime config ================================

static const unsigned long LOG_BAUD     = 74880;     // UART1 debug prints
static const unsigned long PPP_BAUD     = 115200;    // PPP UART0

static const uint16_t      MQTT_PORT    = 1883;

#if AP_ENABLE
static const int AP_CHANNEL = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);
#define AP_SSID "WemosD1"
#define AP_PASS "0187297154091575"
#endif

// EEPROM layout (kept minimal; also used for BootDiag persistence)
#define EEPROM_SIZE 256
#define SSID_ADDR   0
#define PASS_ADDR   32
#define DIAG_ADDR   96
#define MAX_SSID    31
#define MAX_PASS    31

static const uint32_t HEALTH_EVERY_MS    = 3000;
static const uint32_t TELEMETRY_EVERY_MS = 30000;

// ============================== Globals =======================================

MqttBroker broker(MQTT_PORT);
static unsigned long lastMQTTLoopTouchMs = 0;
static bool mqttEverBegan = false;

ESP8266WebServer server(80);

// PPP
static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;

// Health / telemetry
static unsigned long lastHealthTickMs = 0;
static unsigned long lastTelemetryMs  = 0;

static String g_lastTelLine;
static String g_lastNetifDump;

// PPP deferred/backoff state
static volatile bool g_ppp_up_flag  = false;
static volatile bool g_ppp_err_flag = false;
static volatile int  g_ppp_err_code = 0;

static unsigned long g_ppp_next_reconnect_ms = 0;
static uint16_t      g_ppp_reconnect_backoff_ms = 500;  // start small
static const uint16_t PPP_BACKOFF_MAX_MS = 10000;

#if AP_ENABLE
// AP credentials (persisted)
char ap_ssid[MAX_SSID+1] = AP_SSID;
char ap_pass[MAX_PASS+1] = AP_PASS;
#endif

// ======================= Persistent Boot Diagnostics ==========================

struct BootDiag {
  uint32_t magic;       // 0xB00DDA7A
  uint32_t reason;
  uint32_t exccause;
  uint32_t epc1, epc2, epc3, excvaddr, depc;
  uint32_t flashKB, cpuMHz, sketchKB, freeSketchKB;
  uint32_t bootCount;
};
static const uint32_t BOOTDIAG_MAGIC = 0xB00DDA7A;
static BootDiag g_bootdiag = {};

static void loadBootDiag() {
  BootDiag tmp = {};
  EEPROM.get(DIAG_ADDR, tmp);
  if (tmp.magic == BOOTDIAG_MAGIC) g_bootdiag = tmp;
}
static void saveBootDiag() { EEPROM.put(DIAG_ADDR, g_bootdiag); EEPROM.commit(); }

// ============================= Netif Utils ====================================

static void dump_netifs_to(String& out, const char* tag) {
  out += "[NETIF] ";
  out += tag;
  out += "\n";

  for (netif* n = netif_list; n; n = n->next) {
#if LWIP_IPV4
    const ip4_addr_t* ip = netif_ip4_addr(n);
    const ip4_addr_t* gw = netif_ip4_gw(n);
    const ip4_addr_t* mk = netif_ip4_netmask(n);

    // Re-entrant conversion buffers (enough for IPv4 "255.255.255.255" + NUL)
    char ipbuf[16], gwbuf[16], mkbuf[16];
    ip4addr_ntoa_r(ip, ipbuf, sizeof(ipbuf));
    ip4addr_ntoa_r(gw, gwbuf, sizeof(gwbuf));
    ip4addr_ntoa_r(mk, mkbuf, sizeof(mkbuf));
#else
    // Fallback (shouldn't happen on ESP8266/lwIP2, which is IPv4)
    const char* ipbuf = "0.0.0.0";
    const char* gwbuf = "0.0.0.0";
    const char* mkbuf = "0.0.0.0";
#endif

    char line[192];
    // Keep your original layout; add quick state hints (UP/LINK) at the end
    snprintf(line, sizeof(line),
             "  #%u %c%c  ip=%s gw=%s mask=%s flags=0x%02x%s%s\n",
             n->num, n->name[0], n->name[1],
             ipbuf, gwbuf, mkbuf, n->flags,
             netif_is_up(n) ? " UP" : "",
             netif_is_link_up(n) ? " LINK" : "");

    out += line;
  }
}



static void dump_netifs(const char* tag) { String tmp; dump_netifs_to(tmp, tag); Serial1.print(tmp); }

// ============================== AP Bring-up ===================================

#if AP_ENABLE
static void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ap_ssid);
  EEPROM.get(PASS_ADDR, ap_pass);
  if (ap_ssid[0]==0xFF || ap_ssid[0]=='\0') strcpy(ap_ssid,AP_SSID);
  if (ap_pass[0]==0xFF || ap_pass[0]=='\0') strcpy(ap_pass,AP_PASS);
  loadBootDiag(); // persisted boot diagnostics
}
static void saveAPConfig(const char* ssid,const char* pass){
  memset(ap_ssid,0,sizeof(ap_ssid)); memset(ap_pass,0,sizeof(ap_pass));
  strncpy(ap_ssid,ssid,MAX_SSID); strncpy(ap_pass,pass,MAX_PASS);
  EEPROM.put(SSID_ADDR,ap_ssid); EEPROM.put(PASS_ADDR,ap_pass); EEPROM.commit();
}
static void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHANNEL, false, 4)) {   // max 4 clients
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  wifi_set_sleep_type(NONE_SLEEP_T);
  dump_netifs("after softAP");
}
#else
static void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  loadBootDiag(); // still persist boot diagnostics even without AP config
}
#endif

// ========================= PPP Bring-up =======================================

static u32_t ppp_output_cb(ppp_pcb *, u8_t *data, u32_t len, void *) { return Serial.write(data, len); }

// PPP status callback: NO LOGGING HERE (defer to main loop)
static void ppp_status_cb(ppp_pcb *, int err_code, void *) {
  if (err_code == PPPERR_NONE) {
    g_ppp_up_flag = true;
  } else {
    g_ppp_err_flag = true;
    g_ppp_up_flag = false;
    g_ppp_err_code = err_code;
  }
}

static void setupPPP() {
  Serial.begin(PPP_BAUD); delay(50);
  ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, nullptr);
  if (!ppp) { Serial1.println("[PPP] create FAILED"); return; }
#if defined(PPPAUTHTYPE_NONE)
  ppp_set_auth(ppp, PPPAUTHTYPE_NONE, "", "");
#endif
  ppp_set_default(ppp);
  ppp_connect(ppp, 0);
  Serial1.println("[PPP] connecting...");
}

// ========================= Safe AP client list (IPs) ==========================

#if AP_ENABLE
static void buildClientIPsHTML(String& out) {
  // Only attempt if AP is active
  if (!(WiFi.getMode() & WIFI_AP)) { out += F("<p>AP not active.</p>"); return; }

  const int count = wifi_softap_get_station_num();
  out += F("<h3>Connected AP Clients</h3>");
  out += F("<p>Count: "); out += String(count); out += F("</p>");

  if (count <= 0) { out += F("<ul></ul>"); return; }

  // Copy IPs from SDK list into a temporary array, then free the list immediately.
  struct station_info* list = wifi_softap_get_station_info();
  int copied = 0;
  IPAddress* ips = (count > 0) ? new IPAddress[count] : nullptr;

  for (struct station_info* s = list; s && copied < count; s = STAILQ_NEXT(s, next)) {
    ips[copied++] = IPAddress(s->ip.addr);
  }
  wifi_softap_free_station_info(); // release SDK memory promptly

  out += F("<ul>");
  for (int i = 0; i < copied; ++i) {
    out += F("<li>"); out += ips[i].toString(); out += F("</li>");
  }
  out += F("</ul>");

  delete[] ips;
}
#endif

// ================================ Web UI ======================================

static void handleRoot() {
  String html; html.reserve(9000);
  html += F("<html><head><title>Wemos PPP (no NAT)</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
            "<style>body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:12px;}"
            "pre{background:#111;color:#eee;padding:8px;white-space:pre-wrap;word-break:break-word;border-radius:6px}"
            "a{color:#06c;text-decoration:none}a:hover{text-decoration:underline}"
            "h1,h2,h3{margin:8px 0 6px}</style></head><body>");
  html += F("<h1>Wemos PPP (no NAT)</h1>");

#if AP_ENABLE
  html += F("<h2>Access Point</h2>");
  html += F("<p>AP SSID: "); html += ap_ssid; html += F("</p>");
  buildClientIPsHTML(html);
#else
  html += F("<p>AP: <em>disabled at compile time</em></p>");
#endif

  // PPP IP display
  netif* p = nullptr; for (netif* it = netif_list; it; it = it->next) if (it->name[0]=='p' && it->name[1]=='p') { p = it; break; }
  if (p && netif_is_up(p)) {
    html += F("<h2>PPP Link</h2><p>PPP IP: ");
    html += ipaddr_ntoa(netif_ip4_addr(p));
    html += F("</p>");
  }

  html += F("<p><em>Note:</em> NAT is disabled. AP clients will <strong>not</strong> be forwarded to the PPP link. "
            "Use the AP for device setup/telemetry only, or add a route for 192.168.4.0/24 on your PPP peer if you want reachability from that side.</p>");

  html += F("<h2>Boot Diagnostics (persisted)</h2><pre>");
  html += "BootCount: " + String(g_bootdiag.bootCount) + "\n";
  html += "Reason   : " + String(g_bootdiag.reason) + "\n";
  html += "exccause : " + String(g_bootdiag.exccause) + "\n";
  html += "epc1     : 0x" + String(g_bootdiag.epc1, HEX) + "\n";
  html += "epc2     : 0x" + String(g_bootdiag.epc2, HEX) + "\n";
  html += "epc3     : 0x" + String(g_bootdiag.epc3, HEX) + "\n";
  html += "excvaddr : 0x" + String(g_bootdiag.excvaddr, HEX) + "\n";
  html += "depc     : 0x" + String(g_bootdiag.depc, HEX) + "\n";
  html += "CPU MHz  : " + String(g_bootdiag.cpuMHz) + "\n";
  html += "Flash KB : " + String(g_bootdiag.flashKB) + "\n";
  html += "Sketch KB: " + String(g_bootdiag.sketchKB) + "\n";
  html += "FreeSK KB: " + String(g_bootdiag.freeSketchKB) + "\n";
  html += F("</pre>");

  html += F("<h2>Telemetry Snapshot</h2><pre>");
  html += g_lastTelLine; html += F("\n"); html += g_lastNetifDump; html += F("</pre>");

#if AP_ENABLE
  html += F("<h2>Set AP Parameters</h2>"
            "<form method='POST' action='/setap'>SSID: <input name='ssid' value='");
  html += ap_ssid; html += F("'><br>Password: <input name='pass' value='");
  html += ap_pass; html += F("'><br><input type='submit' value='Save & Reboot'></form>");
#endif

  html += F("<h2>Reset</h2><form method='POST' action='/reset'><input type='submit' value='Reboot'></form>");

  html += F("</body></html>");
  server.send(200, "text/html", html);
}

#if AP_ENABLE
static void handleSetAP() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    saveAPConfig(server.arg("ssid").c_str(), server.arg("pass").c_str());
    server.send(200, "text/html", "<html><body><h1>Saved. Rebooting...</h1></body></html>");
    delay(500); ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing ssid/pass");
  }
}
#endif

static void handleReset() {
#if AP_ENABLE
  // Persist current AP config across reboot
  saveAPConfig(ap_ssid, ap_pass);
#else
  EEPROM.commit(); // ensure BootDiag stays persisted
#endif
  server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
  delay(500); ESP.restart();
}

static void setupWeb() {
  server.on("/", handleRoot);
#if AP_ENABLE
  server.on("/setap", HTTP_POST, handleSetAP);
#endif
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  Serial1.println("[WEB] HTTP server started on port 80");
}

// ================================ MQTT ========================================

static inline void serviceMQTT() {
  broker.loop();
  lastMQTTLoopTouchMs = millis();
  // yield();
}

static void setupMQTT() {
  broker.begin();
  mqttEverBegan = true;
  lastMQTTLoopTouchMs = millis();
  Serial1.printf("[MQTT] TinyMqtt broker on :%u (PPP, no NAT)\n", MQTT_PORT);
}

// ============================= Health Monitor =================================

static const char* rstReasonToStr(uint32_t r) {
  switch (r) { case 0:return "REASON_DEFAULT_RST"; case 1:return "REASON_WDT_RST";
    case 2:return "REASON_EXCEPTION_RST"; case 3:return "REASON_SOFT_WDT_RST";
    case 4:return "REASON_SOFT_RESTART"; case 5:return "REASON_DEEP_SLEEP_AWAKE";
    case 6:return "REASON_EXT_SYS_RST"; default:return "UNKNOWN"; }
}
static netif* findPPP() { for (netif* n = netif_list; n; n = n->next) if (n->name[0]=='p'&&n->name[1]=='p') return n; return nullptr; }

#if AP_ENABLE
static void ensureAPUp() {
  const bool apMode = (WiFi.getMode() & WIFI_AP);
  if (!apMode) { Serial1.println("[HEALTH][AP] AP mode lost -> reconfig"); setupAP(); return; }
  if (WiFi.softAPIP() == IPAddress((uint32_t)0)) { Serial1.println("[HEALTH][AP] AP IP invalid -> reconfig"); setupAP(); }
}
#else
static inline void ensureAPUp() {}
#endif

static void ppp_schedule_reconnect(unsigned long now) {
  if (g_ppp_next_reconnect_ms) return;
  g_ppp_next_reconnect_ms = now + g_ppp_reconnect_backoff_ms;
  uint16_t prev=g_ppp_reconnect_backoff_ms;
  g_ppp_reconnect_backoff_ms = (g_ppp_reconnect_backoff_ms < PPP_BACKOFF_MAX_MS) ? (g_ppp_reconnect_backoff_ms*2) : PPP_BACKOFF_MAX_MS;
  Serial1.printf("[PPP] reconnect scheduled in %u ms (next backoff %u ms)\n", prev, g_ppp_reconnect_backoff_ms);
}

static void ensurePPPUp() {
  netif* n = findPPP();
  if (!ppp) { Serial1.println("[HEALTH][PPP] pcb null -> recreate"); setupPPP(); return; }
  if (!n || !netif_is_up(n)) { Serial1.println("[HEALTH][PPP] netif down/missing -> schedule reconnect"); ppp_schedule_reconnect(millis()); return; }
}

static void servicePPPDeferred() {
  const unsigned long now = millis();

  if (g_ppp_up_flag) {
    g_ppp_up_flag = false;
    g_ppp_reconnect_backoff_ms = 500;
    Serial1.println("[PPP] UP event consumed");
    dump_netifs("PPP UP");
  }
  if (g_ppp_err_flag) {
    g_ppp_err_flag = false;
    Serial1.printf("[PPP] error event: code=%d\n", g_ppp_err_code);
    ppp_schedule_reconnect(now);
  }
  if (g_ppp_next_reconnect_ms && (int32_t)(now - g_ppp_next_reconnect_ms) >= 0) {
    g_ppp_next_reconnect_ms = 0;
    if (ppp) { Serial1.println("[PPP] reconnecting now…"); ppp_connect(ppp, 0); }
    else { Serial1.println("[PPP] pcb null; recreating…"); setupPPP(); }
  }
}

static void ensureMQTTUp() {
  if (!mqttEverBegan) return;
  const unsigned long now = millis();
  if ((uint32_t)(now - lastMQTTLoopTouchMs) > 10000) {
    Serial1.println("[HEALTH][MQTT] no loop activity >10s -> restart broker");
    broker.begin();
    lastMQTTLoopTouchMs = now;
  }
}

static void logTelemetry() {
  uint32_t freeHeap=ESP.getFreeHeap(), maxBlk=ESP.getMaxFreeBlockSize(); uint8_t frag=ESP.getHeapFragmentation();
#if AP_ENABLE
  int staCount=wifi_softap_get_station_num();
#else
  int staCount=0;
#endif
  netif* p=findPPP(); const char* pppState=(p&&netif_is_up(p))?"UP":"DOWN";
#if AP_ENABLE
  const char* apState=(WiFi.getMode()&WIFI_AP)?"UP":"DOWN";
#else
  const char* apState="DISABLED";
#endif
  char line[200]; snprintf(line,sizeof(line),"[TEL] up=%lus heap=%lu maxblk=%lu frag=%u%% AP=%s STA=%d PPP=%s IP=%s",
           millis()/1000UL,(unsigned long)freeHeap,(unsigned long)maxBlk,(unsigned)frag,apState,staCount,pppState, p?ipaddr_ntoa(netif_ip4_addr(p)):"0.0.0.0");
  g_lastTelLine=line; String netifs; dump_netifs_to(netifs,"periodic"); g_lastNetifDump=netifs;
  Serial1.println(g_lastTelLine); Serial1.print(g_lastNetifDump);
}

static void serviceHealth() {
  const unsigned long now = millis();
  if ((uint32_t)(now - lastHealthTickMs) < HEALTH_EVERY_MS) return;
  lastHealthTickMs = now;

  ensureAPUp();
  ensurePPPUp();
  servicePPPDeferred();
  ensureMQTTUp();

  if ((uint32_t)(now - lastTelemetryMs) >= TELEMETRY_EVERY_MS) { lastTelemetryMs = now; logTelemetry(); }
}

// ============================ Service Loops ===================================

static inline void servicePPP() {
  if (!ppp) return;
  static uint8_t buf[512];
  int avail = Serial.available();
  while (avail > 0) {
    int take = (avail > (int)sizeof(buf)) ? sizeof(buf) : avail;
    int n = Serial.readBytes(buf, take);
    if (n > 0) { pppos_input(ppp, buf, n); }
    avail = Serial.available(); delay(0);
  }
}
static inline void serviceHTTP() { server.handleClient(); }

// ============================== Arduino =======================================

void setup() {
  Serial1.begin(LOG_BAUD); delay(100);
  WiFi.persistent(false);
  wifi_set_sleep_type(NONE_SLEEP_T);

  loadAPConfig(); // also loads persisted boot diag

  // Persist boot diagnostics for THIS boot
  struct rst_info* ri = system_get_rst_info();
  g_bootdiag.magic=BOOTDIAG_MAGIC; g_bootdiag.bootCount=g_bootdiag.bootCount+1;
  g_bootdiag.reason=ri->reason; g_bootdiag.exccause=ri->exccause;
  g_bootdiag.epc1=ri->epc1; g_bootdiag.epc2=ri->epc2; g_bootdiag.epc3=ri->epc3;
  g_bootdiag.excvaddr=ri->excvaddr; g_bootdiag.depc=ri->depc;
  g_bootdiag.cpuMHz=ESP.getCpuFreqMHz(); g_bootdiag.flashKB=ESP.getFlashChipSize()/1024;
  g_bootdiag.sketchKB=ESP.getSketchSize()/1024; g_bootdiag.freeSketchKB=ESP.getFreeSketchSpace()/1024;
  saveBootDiag();

#if AP_ENABLE
  setupAP();
#else
  WiFi.mode(WIFI_OFF);
  Serial1.println("[AP] disabled at compile time");
#endif

  setupPPP();
  setupMQTT();
  setupWeb();

  // Log boot diag
  Serial1.printf("[BOOT] Reason=%s (%u)\n", rstReasonToStr(ri->reason), ri->reason);
  if (ri->reason==REASON_EXCEPTION_RST || ri->reason==REASON_WDT_RST || ri->reason==REASON_SOFT_WDT_RST) {
    Serial1.printf("[BOOT] exccause=%u epc1=%08x epc2=%08x epc3=%08x excvaddr=%08x depc=%08x\n",
                   ri->exccause, ri->epc1, ri->epc2, ri->epc3, ri->excvaddr, ri->depc);
  }
  Serial1.printf("[BOOT] SDK:%s CPU:%uMHz Flash:%uKB Sketch:%uKB FreeSketch:%uKB BootCount:%u\n",
                 system_get_sdk_version(), ESP.getCpuFreqMHz(),
                 ESP.getFlashChipSize()/1024, ESP.getSketchSize()/1024, ESP.getFreeSketchSpace()/1024,
                 g_bootdiag.bootCount);

  lastMQTTLoopTouchMs = millis();
  lastHealthTickMs    = lastMQTTLoopTouchMs;
  lastTelemetryMs     = lastMQTTLoopTouchMs;
}

void loop() {
  servicePPP();
  serviceHTTP();
  serviceMQTT();
  serviceHealth();
  yield();
}
