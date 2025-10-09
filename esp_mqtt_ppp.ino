/**
 * esp_mqtt_ppp_telnet_health_ppp_hardened_nophase_PATCHED.ino
 *
 * ESP8266 (Wemos D1):
 *  - SoftAP + simple web UI (status, config, boot diagnostics, telemetry, logs)
 *  - PPPoS over UART0 (Serial) as WAN uplink
 *  - TinyMqtt broker on :1883 with time-sliced loop
 *  - Telnet mirror for Serial1 logs on :2048 + early-boot capture
 *  - /logs (HTTP chunked live tail) + /logs.html (auto-reconnecting viewer)
 *  - Health monitor + PPP reconnect backoff (no heavy work in PPP callbacks)
 *
 * Notes (no NAT):
 *  - The device is a PPP client; it does not NAT/forward AP clients to PPP.
 *  - Use AP for setup/telemetry only, or add a route for 192.168.4.0/24 on the PPP peer.
 *
 * PATCHES in this version:
 *  - TelnetLogger::write() no longer calls _client.write() (no socket I/O in ISR/lwIP/PPP contexts).
 *  - Telnet flushing happens in TelnetLogger::loop() only.
 *  - ppp_status_cb() does not log anymore; it only flips flags (logging deferred).
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

// ============================== Config =================================

static const unsigned long LOG_BAUD         = 74880;     // UART1 logs
static const uint16_t      TELNET_PORT      = 2048;      // Telnet logs
static const unsigned long PPP_BAUD         = 115200;    // PPP UART0
static const uint32_t      EARLY_LOG_MS     = 10000;     // capture first 10s

static const int           AP_CHANNEL       = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

static const uint16_t      MQTT_PORT        = 1883;

// AP policy (choose what you want)
static const bool AP_ENABLE             = true;   // set false for PPP-only device
static const bool AP_AUTO_OFF           = false;  // Do not turn AP off after a grace period
static const uint32_t AP_AUTO_OFF_MS    = 10UL * 60UL * 1000UL; // 10 minutes

// EEPROM layout
#define EEPROM_SIZE 256
#define SSID_ADDR   0
#define PASS_ADDR   32
#define DIAG_ADDR   96
#define MAX_SSID    31
#define MAX_PASS    31

static const uint32_t HEALTH_EVERY_MS     = 3000;
static const uint32_t TELEMETRY_EVERY_MS  = 30000;

// ============================== Globals ================================

#define AP_SSID "WemosD1"
#define AP_PASS "0187297154091575"

char ap_ssid[MAX_SSID+1] = AP_SSID;
char ap_pass[MAX_PASS+1] = AP_PASS;

MqttBroker broker(MQTT_PORT);
static unsigned long lastMQTTBurst = 0;
static unsigned long lastMQTTLoopTouchMs = 0;
static bool mqttEverBegan = false;

ESP8266WebServer server(80);
static bool webEverBegan = false;

static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;

static unsigned long lastHealthTickMs = 0;
static unsigned long lastTelemetryMs = 0;

static String g_lastTelLine;
static String g_lastNetifDump;

static unsigned long g_ap_started_ms = 0;

// ---- PPP deferred & backoff state ----
static volatile bool g_ppp_up_flag = false;
static volatile bool g_ppp_err_flag = false;
static volatile int  g_ppp_err_code = 0;

static unsigned long g_ppp_next_reconnect_ms = 0;
static uint16_t      g_ppp_reconnect_backoff_ms = 500;  // start small
static const uint16_t PPP_BACKOFF_MAX_MS = 10000;

// ======================= Persistent Boot Diagnostics ===================

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

void loadBootDiag() {
  BootDiag tmp = {};
  EEPROM.get(DIAG_ADDR, tmp);
  if (tmp.magic == BOOTDIAG_MAGIC) g_bootdiag = tmp;
}
void saveBootDiag() { EEPROM.put(DIAG_ADDR, g_bootdiag); EEPROM.commit(); }

// ============================= Telnet Logger ===========================

#include <WiFiClient.h>
#include <WiFiServer.h>

/**
 * TelnetLogger:
 *  - Mirrors writes to UART1 and a single Telnet client (port TELNET_PORT).
 *  - Captures the first EARLY_LOG_MS of output (RAM ring) for the web UI.
 *  - Maintains an always-on 8KB rolling buffer for HTTP live tail (/logs).
 *  - On Telnet connect: prints persisted Boot Diagnostics + Early Boot Log.
 *
 * PATCH: write() -> no socket I/O; telnet flushing happens in loop().
 */
class TelnetLogger : public Print {
public:
  TelnetLogger(uint16_t port) : _server(port) {}

  void begin(unsigned long baud) {
    ::Serial1.begin(baud);
    _wantServer = true;
    _captureUntil = millis() + EARLY_LOG_MS;   // early-capture window
    _lastTelnetSeq = 0;
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
      _client = _server.accept();
      _client.setNoDelay(true);

      // Start telnet stream from "now"
      _lastTelnetSeq = logSeq();

      // Greeting
      _client.println("ESP8266 Serial1 log (Telnet). Close client to detach.");
      _client.println();

      // --- Show persisted Boot Diagnostics immediately ---
      _client.println("=== Boot Diagnostics (persisted) ===");
      _client.print("BootCount: "); _client.println((unsigned long)g_bootdiag.bootCount);
      _client.print("Reason   : "); _client.println((unsigned long)g_bootdiag.reason);
      _client.print("exccause : "); _client.println((unsigned long)g_bootdiag.exccause);
      _client.print("epc1     : 0x"); _client.println((unsigned long)g_bootdiag.epc1, HEX);
      _client.print("epc2     : 0x"); _client.println((unsigned long)g_bootdiag.epc2, HEX);
      _client.print("epc3     : 0x"); _client.println((unsigned long)g_bootdiag.epc3, HEX);
      _client.print("excvaddr : 0x"); _client.println((unsigned long)g_bootdiag.excvaddr, HEX);
      _client.print("depc     : 0x"); _client.println((unsigned long)g_bootdiag.depc, HEX);
      _client.print("CPU MHz  : "); _client.println((unsigned long)g_bootdiag.cpuMHz);
      _client.print("Flash KB : "); _client.println((unsigned long)g_bootdiag.flashKB);
      _client.print("Sketch KB: "); _client.println((unsigned long)g_bootdiag.sketchKB);
      _client.print("FreeSK KB: "); _client.println((unsigned long)g_bootdiag.freeSketchKB);
      _client.println();

      // --- Show Early Boot Log (first 10s), if any ---
      String cap;
      getEarlyLog(cap);
      if (cap.length() > 0) {
        _client.println("=== Early Boot Log (first 10s) ===");
        _client.print(cap);
        _client.println("\n=== End Early Boot Log ===");
      } else {
        _client.println("(no early boot log captured)");
      }
      _client.println();
    }

    // Cleanup dead client
    if (_client && !_client.connected()) {
      _client.stop();
    }

    // PATCH: Flush log ring to telnet client from main loop only
    if (_client && _client.connected()) {
      String chunk; uint32_t nextSeq = _lastTelnetSeq;
      copySince(_lastTelnetSeq, chunk, nextSeq, 1024); // throttle ~1KB/loop
      if (!chunk.isEmpty()) {
        (void)_client.print(chunk); // best-effort
        _lastTelnetSeq = nextSeq;
      }
    }
  }

  // ---- Print proxy: write to UART1 + capture buffers (NO socket I/O) ----
  size_t write(uint8_t b) override {
    ::Serial1.write(b);
    captureEarly(&b, 1);
    captureRolling(&b, 1);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t size) override {
    ::Serial1.write(buf, size);
    captureEarly(buf, size);
    captureRolling(buf, size);
    return size;
  }
  using Print::write;

  // Early boot capture (first N ms)
  void getEarlyLog(String& out) const {
    out.reserve(_capCount + 64);
    if (_capCount == 0) return;
    size_t i = _capStart;
    for (size_t n = 0; n < _capCount; ++n) {
      out += (char)_capBuf[i];
      if (++i == CAP_SIZE) i = 0;
    }
  }

  // Rolling log tail API
  uint32_t logSeq() const { return _logTotal; }
  void copySince(uint32_t sinceSeq, String& out, uint32_t& nextSeq, size_t maxChunk = 1024) const {
    uint32_t total = _logTotal;
    int32_t delta = (int32_t)(total - sinceSeq);
    if (delta <= 0) { nextSeq = total; return; }

    size_t avail = (size_t)delta;
    if (avail > LOG_SIZE) { avail = LOG_SIZE; sinceSeq = total - avail; }

    size_t startIndex = (_logWrite + LOG_SIZE - avail) % LOG_SIZE;
    if (avail > maxChunk) avail = maxChunk;

    out.reserve(out.length() + avail);
    size_t i = startIndex;
    for (size_t n = 0; n < avail; ++n) {
      out += (char)_logBuf[i];
      if (++i == LOG_SIZE) i = 0;
    }
    nextSeq = sinceSeq + avail;
  }

private:
  // Early-capture ring
  static constexpr size_t CAP_SIZE = 4096;
  uint8_t  _capBuf[CAP_SIZE];
  size_t   _capStart = 0;
  size_t   _capCount = 0;
  unsigned long _captureUntil = 0;

  void captureEarly(const uint8_t* buf, size_t len) {
    if ((int32_t)(millis() - _captureUntil) > 0) return;
    for (size_t i = 0; i < len; ++i) {
      if (_capCount < CAP_SIZE) { _capBuf[_capCount++] = buf[i]; }
      else { _capBuf[_capStart] = buf[i]; _capStart = (_capStart + 1) % CAP_SIZE; }
    }
  }

  // Rolling log ring
  static constexpr size_t LOG_SIZE = 8192;
  uint8_t  _logBuf[LOG_SIZE];
  size_t   _logWrite = 0;
  uint32_t _logTotal = 0;

  void captureRolling(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      _logBuf[_logWrite] = buf[i];
      _logWrite = (_logWrite + 1) % LOG_SIZE;
      _logTotal++;
    }
  }

  // Telnet server
  WiFiServer _server;
  WiFiClient _client;
  bool _serverStarted = false;
  bool _wantServer    = false;

  // PATCH: last position flushed to the telnet client
  uint32_t _lastTelnetSeq = 0;
};

// Replace Serial1 globally with the Telnet logger
TelnetLogger TelnetSerial1(TELNET_PORT);
#define Serial1 TelnetSerial1

// ============================= Netif Utils =============================

void dump_netifs_to(String& out, const char* tag) {
  out += "[NETIF] "; out += tag; out += "\n";
  for (netif* n = netif_list; n; n = n->next) {
    ip4_addr_t ip = *netif_ip4_addr(n); ip4_addr_t gw = *netif_ip4_gw(n); ip4_addr_t mk = *netif_ip4_netmask(n);
    char line[160];
    snprintf(line,sizeof(line),"  #%u %c%c  ip=%s gw=%s mask=%s flags=0x%02x\n",
             n->num,n->name[0],n->name[1], ipaddr_ntoa(&ip), ipaddr_ntoa(&gw), ipaddr_ntoa(&mk), n->flags);
    out += line;
  }
}
void dump_netifs(const char* tag) { String tmp; dump_netifs_to(tmp, tag); Serial1.print(tmp); }

// ============================== AP Bring-up ============================

void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ap_ssid);
  EEPROM.get(PASS_ADDR, ap_pass);
  if (ap_ssid[0]==0xFF || ap_ssid[0]=='\0') strcpy(ap_ssid,AP_SSID);
  if (ap_pass[0]==0xFF || ap_pass[0]=='\0') strcpy(ap_pass,AP_PASS);
  loadBootDiag(); // persisted boot diagnostics
}
void saveAPConfig(const char* ssid,const char* pass){
  memset(ap_ssid,0,sizeof(ap_ssid)); memset(ap_pass,0,sizeof(ap_pass));
  strncpy(ap_ssid,ssid,MAX_SSID); strncpy(ap_pass,pass,MAX_PASS);
  EEPROM.put(SSID_ADDR,ap_ssid); EEPROM.put(PASS_ADDR,ap_pass); EEPROM.commit();
}
void setupAP() {
  if (!AP_ENABLE) {
    WiFi.mode(WIFI_OFF);
    Serial1.println("[AP] disabled by config");
    return;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHANNEL, false, 4)) {
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
    g_ap_started_ms = millis();
  }
  wifi_set_sleep_type(NONE_SLEEP_T);
  dump_netifs("after softAP");
}

// ========================= PPP Bring-up =========================

static u32_t ppp_output_cb(ppp_pcb *, u8_t *data, u32_t len, void *) { return Serial.write(data, len); }

// PPP status callback: NO LOGGING HERE (defer to main loop)
static void ppp_status_cb(ppp_pcb *, int err_code, void *) {
  if (err_code == PPPERR_NONE) {
    g_ppp_up_flag = true;
    // (no Serial1 prints here)
  } else {
    g_ppp_err_flag = true;
    g_ppp_up_flag = false;
    g_ppp_err_code = err_code;
    // (no Serial1 prints here)
  }
}

void setupPPP() {
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

// ================================ Web UI ==============================

void handleRoot() {
  String html; html.reserve(10000);
  html += F("<html><head><title>Wemos PPP (no NAT)</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
            "<style>body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:12px;}"
            "pre{background:#111;color:#eee;padding:8px;white-space:pre-wrap;word-break:break-word;border-radius:6px}"
            "a{color:#06c;text-decoration:none}a:hover{text-decoration:underline}"
            "h1,h2{margin:8px 0 6px}</style></head><body>");
  html += F("<h1>Wemos PPP (no NAT)</h1>");
  html += F("<p><a href='/logs.html'>Open Live Logs</a></p>");
  html += F("<p>AP SSID: "); html += ap_ssid; html += F("</p>");

  // PPP IP display
  netif* p = nullptr; for (netif* it = netif_list; it; it = it->next) if (it->name[0]=='p' && it->name[1]=='p') { p = it; break; }
  if (p && netif_is_up(p)) {
    html += F("<p>PPP IP: ");
    html += ipaddr_ntoa(netif_ip4_addr(p));
    html += F("</p>");
  }

  html += F("<p><em>Note:</em> NAT is disabled. AP clients will <strong>not</strong> be forwarded to the PPP link. "
            "Use the AP for device setup/telemetry only, or add a route for 192.168.4.0/24 on your PPP peer if you want reachability from that side.</p>");

  html += F("<h2>Connected AP Clients</h2><ul>");
  struct station_info *stat_info = wifi_softap_get_station_info();
  while (stat_info) {
    IPAddress ip = IPAddress(stat_info->ip.addr);
    html += F("<li>"); html += ip.toString(); html += F("</li>");
    stat_info = STAILQ_NEXT(stat_info, next);
  }
  wifi_softap_free_station_info();
  html += F("</ul>");

  html += F("<h2>Boot Diagnostics (persisted)</h2><pre>");
  html += "BootCount: "; html += String(g_bootdiag.bootCount); html += "\n";
  html += "Reason   : "; html += String(g_bootdiag.reason); html += "\n";
  html += "exccause : "; html += String(g_bootdiag.exccause); html += "\n";
  html += "epc1     : 0x"; html += String(g_bootdiag.epc1, HEX); html += "\n";
  html += "epc2     : 0x"; html += String(g_bootdiag.epc2, HEX); html += "\n";
  html += "epc3     : 0x"; html += String(g_bootdiag.epc3, HEX); html += "\n";
  html += "excvaddr : 0x"; html += String(g_bootdiag.excvaddr, HEX); html += "\n";
  html += "depc     : 0x"; html += String(g_bootdiag.depc, HEX); html += "\n";
  html += "CPU MHz  : "; html += String(g_bootdiag.cpuMHz); html += "\n";
  html += "Flash KB : "; html += String(g_bootdiag.flashKB); html += "\n";
  html += "Sketch KB: "; html += String(g_bootdiag.sketchKB); html += "\n";
  html += "FreeSK KB: "; html += String(g_bootdiag.freeSketchKB); html += "\n";
  html += F("</pre>");

  html += F("<h2>Early Boot Log (first 10s of Serial1)</h2><pre>");
  { String cap; Serial1.getEarlyLog(cap); if (cap.length()==0) html += F("(no capture or window elapsed)"); else html += cap; }
  html += F("</pre>");

  html += F("<h2>Telemetry Snapshot</h2><pre>");
  html += g_lastTelLine; html += F("\n"); html += g_lastNetifDump; html += F("</pre>");

  html += F("<h2>Set AP Parameters</h2>"
            "<form method='POST' action='/setap'>SSID: <input name='ssid' value='");
  html += ap_ssid; html += F("'><br>Password: <input name='pass' value='");
  html += ap_pass; html += F("'><br><input type='submit' value='Save & Reboot'></form>");

  html += F("<h2>Reset</h2><form method='POST' action='/reset'><input type='submit' value='Reboot'></form>");

  html += F("</body></html>");
  server.send(200, "text/html", html);
}
void handleSetAP() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    saveAPConfig(server.arg("ssid").c_str(), server.arg("pass").c_str());
    server.send(200, "text/html", "<html><body><h1>Saved. Rebooting...</h1></body></html>");
    delay(500); ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing ssid/pass");
  }
}
void handleReset() {
  saveAPConfig(ap_ssid, ap_pass);
  server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
  delay(500); ESP.restart();
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setap", HTTP_POST, handleSetAP);
  server.on("/reset", HTTP_POST, handleReset);

  // Live log tail over HTTP
  server.on("/logs", []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "");
    WiFiClient client = server.client();
    uint32_t seq = Serial1.logSeq();
    const unsigned long t0 = millis(), MAX_MS = 20000;
    server.sendContent("=== /logs live tail (20s window) ===\n");
    while (client.connected() && (uint32_t)(millis()-t0) < MAX_MS) {
      String chunk; uint32_t nextSeq = seq;
      Serial1.copySince(seq, chunk, nextSeq, 1024);
      if (chunk.length() > 0) { server.sendContent(chunk); seq = nextSeq; }
      delay(120); yield();
    }
    server.sendContent("\n=== /logs end ===\n");
  });

  // Live log viewer page with auto-reconnect & autoscroll
  server.on("/logs.html", []() {
    String html; html.reserve(6000);
    html += F(
      "<!doctype html><html><head><meta charset='utf-8'/>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>ESP8266 Live Logs</title>"
      "<style>"
      "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;margin:0;background:#0b0b0b;color:#eaeaea}"
      "header{padding:10px 14px;background:#111;position:sticky;top:0;z-index:1;border-bottom:1px solid #222}"
      "button, input[type=checkbox]{font:inherit}"
      "#log{white-space:pre-wrap;word-break:break-word;font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;"
      " background:#0b0b0b;padding:12px;margin:0;min-height:calc(100vh - 58px)}"
      ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
      ".pill{padding:4px 8px;background:#1c1c1c;border:1px solid #2b2b2b;border-radius:999px}"
      "</style></head><body>"
      "<header class='row'>"
        "<div class='pill'>/logs live tail</div>"
        "<button id='btn' onclick='toggle()'>Stop</button>"
        "<label><input id='auto' type='checkbox' checked/> Auto-scroll</label>"
        "<span id='status' class='pill'>connecting…</span>"
      "</header>"
      "<pre id='log'></pre>"
      "<script>"
      "let running=true, ctrl=null, auto=true;"
      "const logEl=document.getElementById('log');"
      "const btn=document.getElementById('btn');"
      "const st=document.getElementById('status');"
      "document.getElementById('auto').addEventListener('change',e=>auto=e.target.checked);"
      "function toggle(){running=!running;btn.textContent=running?'Stop':'Start';if(running) start(); else if(ctrl){ctrl.abort();ctrl=null;st.textContent='stopped';}}"
      "function append(t){logEl.textContent+=t; if(auto) logEl.scrollTop=logEl.scrollHeight;}"
      "async function start(){"
        "try{st.textContent='connecting…';ctrl=new AbortController();"
          "const resp=await fetch('/logs',{signal:ctrl.signal});"
          "if(!resp.body){append('\\n(streaming not supported by this browser)\\n');return;}"
          "st.textContent='streaming';const reader=resp.body.getReader();const dec=new TextDecoder();"
          "for(;;){const {value,done}=await reader.read(); if(done) break; append(dec.decode(value));}"
          "st.textContent='reconnecting…'; setTimeout(()=>{ if(running) start(); }, 500);"
        "}catch(e){st.textContent='error: '+(e&&e.message?e.message:e); setTimeout(()=>{ if(running) start(); }, 1000);}"
      "}"
      "window.addEventListener('load', start);"
      "</script></body></html>"
    );
    server.send(200, "text/html", html);
  });

  server.begin(); webEverBegan = true;
  Serial1.println("[WEB] HTTP server started on port 80");
}

// ================================ MQTT ================================
static inline void serviceMQTT() {
  const unsigned long now = millis();
  broker.loop();
  lastMQTTBurst = now; lastMQTTLoopTouchMs = now;
  yield();
};

void setupMQTT() { broker.begin(); mqttEverBegan = true; lastMQTTLoopTouchMs = millis();
  Serial1.printf("[MQTT] TinyMqtt broker on :%u (PPP, no NAT)\n", MQTT_PORT); }

// ============================= Health Monitor =========================

const char* rstReasonToStr(uint32_t r) {
  switch (r) { case 0:return "REASON_DEFAULT_RST"; case 1:return "REASON_WDT_RST";
    case 2:return "REASON_EXCEPTION_RST"; case 3:return "REASON_SOFT_WDT_RST";
    case 4:return "REASON_SOFT_RESTART"; case 5:return "REASON_DEEP_SLEEP_AWAKE";
    case 6:return "REASON_EXT_SYS_RST"; default:return "UNKNOWN"; }
}
netif* findPPP() { for (netif* n = netif_list; n; n = n->next) if (n->name[0]=='p'&&n->name[1]=='p') return n; return nullptr; }

static void ensureAPUp() {
  if (!AP_ENABLE) return; // honor config: do not auto-recreate AP if disabled
  const bool apMode = (WiFi.getMode() & WIFI_AP);
  if (!apMode) { Serial1.println("[HEALTH][AP] AP mode lost -> reconfig"); setupAP(); return; }
  if (WiFi.softAPIP() == IPAddress((uint32_t)0)) { Serial1.println("[HEALTH][AP] AP IP invalid -> reconfig"); setupAP(); }
}
static void maybeTurnOffAP() {
  if (!AP_ENABLE || !AP_AUTO_OFF) return;
  if (!(WiFi.getMode() & WIFI_AP)) return;
  if (g_ap_started_ms == 0) return;
  if ((uint32_t)(millis() - g_ap_started_ms) < AP_AUTO_OFF_MS) return;

  Serial1.println("[AP] auto-off: disabling AP (PPP remains active)");
  WiFi.mode(WIFI_OFF); // fully disable AP (can switch to STA later if desired)
}
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
  if ((uint32_t)(now - lastMQTTLoopTouchMs) > 10000) { Serial1.println("[HEALTH][MQTT] no loop activity >10s -> restart broker"); broker.begin(); lastMQTTLoopTouchMs = now; }
}
static void ensureWebUp() { /* intentional no-op */ }
static void ensureTelnetUp() { Serial1.loop(); }

static void logTelemetry() {
  uint32_t freeHeap=ESP.getFreeHeap(), maxBlk=ESP.getMaxFreeBlockSize(); uint8_t frag=ESP.getHeapFragmentation();
  int staCount=wifi_softap_get_station_num();
  netif* p=findPPP(); const char* pppState=(p&&netif_is_up(p))?"UP":"DOWN"; const char* apState=(WiFi.getMode()&WIFI_AP)?"UP":"DOWN";
  char line[200]; snprintf(line,sizeof(line),"[TEL] up=%lus heap=%lu maxblk=%lu frag=%u%% AP=%s STA=%d PPP=%s IP=%s",
           millis()/1000UL,(unsigned long)freeHeap,(unsigned long)maxBlk,(unsigned)frag,apState,staCount,pppState, p?ipaddr_ntoa(netif_ip4_addr(p)):"0.0.0.0");
  g_lastTelLine=line; String netifs; dump_netifs_to(netifs,"periodic"); g_lastNetifDump=netifs;
  Serial1.println(g_lastTelLine); Serial1.print(g_lastNetifDump);
}
static void serviceHealth() {
  const unsigned long now = millis();
  if ((uint32_t)(now - lastHealthTickMs) < HEALTH_EVERY_MS) return;
  lastHealthTickMs = now;
  ensureAPUp(); maybeTurnOffAP(); ensurePPPUp(); servicePPPDeferred(); ensureWebUp(); ensureMQTTUp(); ensureTelnetUp();
  if ((uint32_t)(now - lastTelemetryMs) >= TELEMETRY_EVERY_MS) { lastTelemetryMs = now; logTelemetry(); }
}

// ============================ Service Loops ============================

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

// ============================== Arduino ================================

void setup() {
  Serial1.begin(LOG_BAUD); delay(100);
  WiFi.persistent(false); wifi_set_sleep_type(NONE_SLEEP_T);

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

  setupAP(); setupPPP(); setupMQTT(); setupWeb();

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

  lastMQTTBurst = millis(); lastMQTTLoopTouchMs = lastMQTTBurst;
  lastHealthTickMs = lastMQTTBurst; lastTelemetryMs = lastMQTTBurst;
}

void loop() {
  servicePPP(); serviceHTTP(); serviceMQTT();
  Serial1.loop(); serviceHealth(); yield();
}
