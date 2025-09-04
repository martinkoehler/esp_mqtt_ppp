#include <ESP8266WiFi.h>
#include <uMQTTBroker.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
extern "C" {
  #include "lwip/opt.h"
  #include "lwip/err.h"
  #include "lwip/ip_addr.h"
  #include "lwip/netif.h"
  #include "lwip/ip4.h"
  #include "lwip/ip4_addr.h"
  #include "lwip/napt.h"
  #include "lwip/etharp.h"
  #include "user_interface.h"
  #include "lwip/sio.h"
  #include "lwip/slipif.h"
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

// ===== SLIP bits =====
static struct netif slip_netif;
static bool slip_started = false;

// lwIP expects a sio_fd_t for slipif
static sio_fd_t slip_sio_fd;

static err_t slipif_init_cb(struct netif *netif) {
  // Set up IP, netmask, gateway, etc. here.
  IP4_ADDR(&netif->ip_addr,   10,0,0,2); // Your ESP’s IP over SLIP
  IP4_ADDR(&netif->netmask,   255,255,255,0);
  IP4_ADDR(&netif->gw,        10,0,0,1); // Host’s IP over SLIP
  return slipif_init(netif);
}

void setupSLIP() {
  Serial.begin(115200);
  delay(50);

  // lwIP: open serial for slipif
  slip_sio_fd = sio_open(0); // 0 = Serial, 1 = Serial1; adjust if needed
  if (!slip_sio_fd) {
    Serial1.println("[SLIP] sio_open FAILED");
    return;
  }
  netif_add(&slip_netif,
            IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY,
            &slip_sio_fd,
            slipif_init_cb,
            ip_input);
  netif_set_up(&slip_netif);
  netif_set_link_up(&slip_netif);
  slip_started = true;

  Serial1.println("[SLIP] interface up");

  // ---- NAT enable on the SLIP (WAN) interface ----
  if (!napt_inited) {
    ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);
    napt_inited = true;
    Serial1.printf("[NAT] NAPT initialized (max=%d, portmaps=%d)\n", IP_NAPT_MAX, IP_PORTMAP_MAX);
  }
  u32_t slip_addr = ip4_addr_get_u32(netif_ip4_addr(&slip_netif));
  if (ip_napt_enable(slip_addr, 1) == ERR_OK) {
    Serial1.printf("[NAT] enabled on SLIP address %s\n", ipaddr_ntoa(netif_ip_addr4(&slip_netif)));
  } else {
    Serial1.println("[NAT] enable failed");
  }
  Serial1.println("[NAT] enabled on SLIP (outbound masquerade)");
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
  if (!WiFi.softAP(ap_ssid, ap_pass, AP_CHAN, false, 4)) { // instead of 8 to save power
    Serial1.println("[AP] start FAILED");
  } else {
    Serial1.printf("[AP] %s up at %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  }
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

// ====== Power optimisation =====
void tuneAP() {
  softap_config conf{};
  wifi_softap_get_config(&conf);
  conf.beacon_interval = 400;   // default 100 ms; 200–400 is a good balance
  wifi_softap_set_config_current(&conf);
  system_update_cpu_freq(80);   // use lowest stable clock
}

void setup() {
  Serial1.begin(74880);
  delay(100);
  loadAPConfig();
  setupAP();
  //WiFi.setOutputPower(12.0f);   // adjust after site test
  tuneAP();                     // set beacon interval
  setupSLIP();
  setupMQTT();
  setupWeb();
  wifi_set_sleep_type(LIGHT_SLEEP_T);
}

void loop() {
  // SLIP polling is handled by lwIP stack, nothing to do here for serial
  server.handleClient();
  yield();
}
