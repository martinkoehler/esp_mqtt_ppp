#include <ESP8266WiFi.h>
#include <uMQTTBroker.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
extern "C" {
  #include "lwip/opt.h"
  #include "lwip/err.h"
  #include "lwip/ip_addr.h"
  #include "lwip/netif.h"
  #include "netif/ppp/ppp.h"
  #include "netif/ppp/pppos.h"
  #include "lwip/etharp.h"
  #include "lwip/ip4.h"
  #include "lwip/ip4_route.h"
  #include "lwip/ip4_nat.h"     // <-- Add this header for NAT
}

#define EEPROM_SIZE 128
#define SSID_ADDR   0
#define PASS_ADDR   32
#define MAX_SSID    31
#define MAX_PASS    31

char ap_ssid[MAX_SSID+1] = "APSSID";
char ap_pass[MAX_PASS+1] = "APPW12345670";
const int AP_CHAN = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// ===== MQTT Broker =====
class MyBroker : public uMQTTBroker {
public:
  bool onConnect(IPAddress addr, uint16_t client_count) override {
    Serial1.printf("[MQTT] +client %s (n=%u)\n", addr.toString().c_str(), client_count);
    return true;
  }
  void onDisconnect(IPAddress addr, String client_id) override {
    Serial1.printf("[MQTT] -client %s id=%s\n", addr.toString().c_str(), client_id.c_str());
  }
  bool onAuth(String username, String password, String client_id) override {
    return true;
  }
  void onData(String topic, const char *data, uint32_t length) override {
    static char buf[256];
    uint32_t n = (length < sizeof(buf)-1) ? length : (sizeof(buf)-1);
    memcpy(buf, data, n);
    buf[n] = '\0';
    Serial1.printf("[MQTT] %s => %s\n", topic.c_str(), buf);
  }
} broker;

// ===== PPPoS bits =====
static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;
static struct netif *ap_netif = nullptr; // for NAT

static u32_t ppp_output_cb(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx) {
  return Serial.write(data, len);
}

static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
  if (err_code == PPPERR_NONE) {
    const ip_addr_t* ip = netif_ip_addr4(&ppp_netif);
    const ip_addr_t* gw = netif_ip_gw4(&ppp_netif);
    const ip_addr_t* mk = netif_ip_netmask4(&ppp_netif);
    Serial1.printf("[PPP] UP  ip=%s gw=%s mask=%s\n",
                   ipaddr_ntoa(ip), ipaddr_ntoa(gw), ipaddr_ntoa(mk));
    // Setup NAT if PPP is up
    if (ap_netif) {
      ip_napt_enable(netif_ip4_addr(&ppp_netif)->addr, 1); // Enable NAT
      Serial1.println("[NAT] enabled on PPP interface");
    }
  } else {
    Serial1.printf("[PPP] status err=%d\n", err_code);
  }
}

// ===== Web server =====
ESP8266WebServer server(80);

void loadAPConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SSID_ADDR, ap_ssid);
  EEPROM.get(PASS_ADDR, ap_pass);
  if (ap_ssid[0] == 0xFF || ap_ssid[0] == '\0') strcpy(ap_ssid, "APSSID");
  if (ap_pass[0] == 0xFF || ap_pass[0] == '\0') strcpy(ap_pass, "APPW12345670");
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
  String html = "<html><head><title>Wemos AP Config</title></head><body>";
  html += "<h1>Wemos AP Config</h1>";
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

  html += "<h2>Reset Wemos</h2>";
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
  server.send(200, "text/html", "<html><body><h1>Rebooting...</h1></body></html>");
  delay(500);
  ESP.restart();
}

void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHAN, false, 8)) {
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  // Get the AP netif pointer for NAT
  ap_netif = netif_list;
  while (ap_netif) {
    if (ip4_addr_cmp(netif_ip4_addr(ap_netif), (ip4_addr*)&ap_ip)) break;
    ap_netif = ap_netif->next;
  }
  if (ap_netif) {
    Serial1.println("[AP] netif found for NAT");
  } else {
    Serial1.println("[AP] netif NOT found, NAT may not work");
  }
}

void setupPPP() {
  Serial.begin(115200);
  delay(50);
  ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, nullptr);
  if (!ppp) {
    Serial1.println("[PPP] create FAILED");
    return;
  }
#if defined(PPPAUTHTYPE_NONE)
  ppp_set_auth(ppp, PPPAUTHTYPE_NONE, "", "");
#endif
  ppp_set_default(ppp);
  ppp_connect(ppp, 0);
  Serial1.println("[PPP] connecting...");
}

void setupMQTT() {
  broker.init();
  broker.publish("wemos/status", "online", 0, true);
  Serial1.println("[MQTT] broker up on :1883");
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setap", HTTP_POST, handleSetAP);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  Serial1.println("[WEB] HTTP server started on port 80");
}

void setup() {
  Serial1.begin(74880);
  delay(100);
  loadAPConfig();
  setupAP();
  setupPPP();
  setupMQTT();
  setupWeb();

  // Enable global IP forwarding (required for routing)
  extern int ip_forward; // lwIP global
  ip_forward = 1;
  Serial1.println("[ROUTER] IP forwarding enabled");
}

void loop() {
  if (ppp) {
    static uint8_t buf[256];
    int avail = Serial.available();
    if (avail > 0) {
      int n = Serial.readBytes(buf, (avail > (int)sizeof(buf)) ? sizeof(buf) : avail);
      if (n > 0) {
        pppos_input(ppp, buf, n);
      }
    }
  }
  server.handleClient();
  delay(1);
}
