// esp_mqtt_ppp_fixed.ino
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
  #include "lwip/ip4_addr.h"
  #include "lwip/napt.h"
  #include "user_interface.h"
}

#define EEPROM_SIZE 128
#define SSID_ADDR   0
#define PASS_ADDR   32
#define MAX_SSID    31
#define MAX_PASS    31

// ===== NAT (NAPT) config =====
#ifndef IP_NAPT_MAX
#define IP_NAPT_MAX     512   // number of NAT table entries
#endif
#ifndef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX  32    // number of explicit port maps (if you add any)
#endif
static bool napt_inited = false;
static bool portmap_set = false;

char ap_ssid[MAX_SSID+1] = "APSSID";
char ap_pass[MAX_PASS+1] = "APPW12345670";
const int AP_CHAN = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// ===== MQTT Broker =====
class MyBroker : public uMQTTBroker {
public:
  bool onConnect(IPAddress addr, uint16_t client_count) override {
    Serial1.printf("[MQTT] +client %s (n=%u)\n", addr.toString().c_str(), client_count);
    return true; // accept all clients
  }
  void onDisconnect(IPAddress addr, String client_id) override {
    Serial1.printf("[MQTT] -client %s id=%s\n", addr.toString().c_str(), client_id.c_str());
  }
  bool onAuth(String username, String password, String client_id) override {
    // no auth
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

    // ---- NAT enable on the PPP (WAN) interface ----
    if (!napt_inited) {
      ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);
      napt_inited = true;
      Serial1.printf("[NAT] NAPT initialized (max=%d, portmaps=%d)\n",
                     IP_NAPT_MAX, IP_PORTMAP_MAX);
    }

    u32_t ppp_addr = ip4_addr_get_u32(netif_ip4_addr(&ppp_netif));
    if (ip_napt_enable(ppp_addr, 1) == ERR_OK) {
      Serial1.printf("[NAT] enabled on PPP address %s\n",
                     ipaddr_ntoa(netif_ip_addr4(&ppp_netif)));
    } else {
      Serial1.println("[NAT] enable failed");
    }

    // ---- Port-forward PPP:1883 -> AP:1883 (MQTT) ----
    if (!portmap_set) {
      ip4_addr_t ap4;
      IP4_ADDR(&ap4, ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]);
      u32_t ap_addr = ip4_addr_get_u32(&ap4);

      // Forward MQTT (TCP)
      ip_portmap_add(IP_PROTO_TCP, ppp_addr, 1883, ap_addr, 1883);

      // Optional: expose HTTP status page via PPP as well
      // ip_portmap_add(IP_PROTO_TCP, ppp_addr, 80, ap_addr, 80);

      portmap_set = true;
      Serial1.println("[NAT] portmap PPP:1883 -> 192.168.4.1:1883 set");
    }

    Serial1.println("[NAT] enabled on PPP (outbound masquerade)");
  } else {
    Serial1.printf("[PPP] err=%d -> reconnect\n", err_code);
    ppp_connect(ppp, 0);
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
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHAN, false, 4)) { // limit to 4 clients to save power
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
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
  broker.init(); // must be followed by broker.loop() in loop()
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

// ====== Power optimisation =====
void tuneAP() {
  softap_config conf{};
  wifi_softap_get_config(&conf);
  conf.beacon_interval = 400;   // default 100 ms; 200–400 is a good balance
  wifi_softap_set_config_current(&conf);
  system_update_cpu_freq(80);   // lowest stable clock
}

void setup() {
  Serial1.begin(74880);         // TX-only UART1 for logs
  delay(100);
  loadAPConfig();
  setupAP();
  // WiFi.setOutputPower(12.0f); // optional
  tuneAP();
  setupPPP();
  setupMQTT();
  setupWeb();
  wifi_set_sleep_type(LIGHT_SLEEP_T);
}

void loop() {
  // Service PPP input coming from Serial0 (pppos)
  if (ppp) {
    static uint8_t buf[256];
    int avail = Serial.available();
    if (avail > 0) {
      int n = Serial.readBytes(buf, (avail > (int)sizeof(buf)) ? sizeof(buf) : avail);
      if (n > 0) {
        pppos_input(ppp, buf, n);
        if (avail > 128) delay(0); // brief cooperative idle after bursts
      }
    }
  }

  // Service HTTP
  server.handleClient();

  // *** Service MQTT broker (CRUCIAL) ***
  broker.loop();

  yield();
}
