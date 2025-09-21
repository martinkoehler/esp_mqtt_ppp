// esp_mqtt_ppp_nat.ino
// ESP8266 (Wemos D1) softAP + PPPoS over Serial + NAPT (NAT) on PPP + TinyMqtt broker + simple HTTP UI

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "TinyMqtt.h"   // https://github.com/hsaturn/TinyMqtt

#define PORT 1883

// ---- MQTT "time-sliced" polling (keeps Wi-Fi/PPP snappy) ----
#define MQTT_BURST_INTERVAL_MS   2
#define MQTT_BURST_BUDGET_US     1200

MqttBroker broker(PORT);
unsigned long lastMQTTBurst = 0;

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
  #include "user_interface.h"
}

// ----------------- Persisted AP config -----------------
#define EEPROM_SIZE 128
#define SSID_ADDR   0
#define PASS_ADDR   32
#define MAX_SSID    31
#define MAX_PASS    31

char ap_ssid[MAX_SSID+1] = "WemosD1";
char ap_pass[MAX_PASS+1] = "0187297154091575";

// ----------------- NAT (NAPT) config -----------------
#ifndef IP_NAPT_MAX
#define IP_NAPT_MAX     512
#endif
#ifndef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX  32
#endif
static bool napt_inited = false;

// ----------------- AP config -----------------
const int AP_CHAN = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// ----------------- PPPoS bits -----------------
static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;   // pppos_create will fill it

static u32_t ppp_output_cb(ppp_pcb *, u8_t *data, u32_t len, void *) {
  return Serial.write(data, len); // UART0 (Serial) is the PPP link
}

// Utility: dump all netifs so we can see numbers / names / IPs
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

// Find the PPP netif by its lwIP name "pp"
netif* findPPP() {
  for (netif* n = netif_list; n; n = n->next) {
    if (n->name[0] == 'p' && n->name[1] == 'p') return n;
  }
  return nullptr;
}

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

// ----------------- Web UI -----------------
ESP8266WebServer server(80);

void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ap_ssid);
  EEPROM.get(PASS_ADDR, ap_pass);
  if (ap_ssid[0] == 0xFF || ap_ssid[0] == '\0') strcpy(ap_ssid, "WemosD1");
  if (ap_pass[0] == 0xFF || ap_pass[0] == '\0') strcpy(ap_pass, "0187297154091575");
}

void saveAPConfig(const char* ssid, const char* pass) {
  memset(ap_ssid, 0, sizeof(ap_ssid));
  memset(ap_pass, 0, sizeof(ap_pass));
  strncpy(ap_ssid, ssid, MAX_SSID);
  strncpy(ap_pass, pass, MAX_PASS);
  EEPROM.put(SSID_ADDR, ap_ssid);
  EEPROM.put(PASS_ADDR, ap_pass);
  EEPROM.commit();
}

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

void handleReset() {
  saveAPConfig(ap_ssid,ap_pass);
  server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
  delay(500);
  ESP.restart();
}

// ----------------- Bring-up helpers -----------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHAN, false, 4)) {
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n",
                   ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  // Keep Wi-Fi awake for responsiveness
  wifi_set_sleep_type(NONE_SLEEP_T);
  dump_netifs("after softAP");
}

void setupPPP() {
  Serial.begin(115200);   // PPP link over UART0
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

void setupMQTT() {
  broker.begin();  // starts broker on IP_ANY:1883
  Serial1.println("[MQTT] TinyMqtt broker on :1883 (AP+PPP)");
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setap", HTTP_POST, handleSetAP);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  Serial1.println("[WEB] HTTP server started on port 80");
}

// ----------------- Service loops -----------------
static inline void servicePPP() {
  if (!ppp) return;
  static uint8_t buf[512];
  int avail = Serial.available();
  if (avail > 0) {
    int n = Serial.readBytes(buf, (avail > (int)sizeof(buf)) ? sizeof(buf) : avail);
    if (n > 0) {
      pppos_input(ppp, buf, n);
      if (avail > 128) delay(0);
    }
  }
}

static inline void serviceHTTP() { server.handleClient(); }

// Time-sliced broker polling: run short bursts often
static inline void serviceMQTT_timesliced() {
  const unsigned long now = millis();
  if ((uint32_t)(now - lastMQTTBurst) >= MQTT_BURST_INTERVAL_MS) {
    const uint32_t deadline = micros() + MQTT_BURST_BUDGET_US;
    do {
      broker.loopWithBudget(MQTT_BURST_BUDGET_US);
      yield();
    } while ((int32_t)(deadline - micros()) > 0);
    lastMQTTBurst = now;
  }
}

// ----------------- Arduino entry points -----------------
void setup() {
  Serial1.begin(74880);  // UART1 for logs
  delay(100);

  loadAPConfig();
  setupAP();
  setupPPP();
  setupMQTT();
  setupWeb();

  lastMQTTBurst = millis();
}

void loop() {
  servicePPP();
  serviceHTTP();
  serviceMQTT_timesliced();
  yield();
}
