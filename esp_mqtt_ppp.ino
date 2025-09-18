// esp_mqtt_ppp_umqttbroker.ino
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include<sMQTTBroker.h>

#define PORT 1883

sMQTTBroker broker;




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
#define IP_NAPT_MAX     512
#endif
#ifndef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX  32
#endif
static bool napt_inited = false;

// ===== AP config =====
char ap_ssid[MAX_SSID+1] = "WemosD1";
char ap_pass[MAX_PASS+1] = "0187297154091575";
const int AP_CHAN = 6;
IPAddress ap_ip(192,168,4,1), ap_gw(192,168,4,1), ap_mask(255,255,255,0);

// ===== MQTT Broker (uMQTTBroker) =====
// uMQTTBroker binds to IP_ANY:1883, so it will listen on AP + PPP automatically.
//uMQTTBroker broker;

// ===== PPPoS bits =====
static ppp_pcb *ppp = nullptr;
static struct netif ppp_netif;

static u32_t ppp_output_cb(ppp_pcb *, u8_t *data, u32_t len, void *) {
  return Serial.write(data, len); // UART0 (Serial) is the PPP link
}

static void ppp_status_cb(ppp_pcb *, int err_code, void *) {
  if (err_code == PPPERR_NONE) {
    const ip_addr_t* ip = netif_ip_addr4(&ppp_netif);
    const ip_addr_t* gw = netif_ip_gw4(&ppp_netif);
    const ip_addr_t* mk = netif_ip_netmask4(&ppp_netif);
    Serial1.printf("[PPP] UP  ip=%s gw=%s mask=%s\n",
                   ipaddr_ntoa(ip), ipaddr_ntoa(gw), ipaddr_ntoa(mk));

    // Enable NAT on PPP netif
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
    Serial1.println("[NAT] outbound masquerade active");
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

// ===== AP bring-up =====
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
}

// ===== PPP bring-up =====
void setupPPP() {
  // Faster UART helps PPP a lot; if your PC side supports it, 230400 is even better.
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

// ===== MQTT bring-up (uMQTTBroker) =====
void setupMQTT() {
  broker.init(PORT);
  Serial1.println("[MQTT] MQTTBroker up on :1883 (AP+PPP)");
}

// ===== Web server bring-up =====
void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setap", HTTP_POST, handleSetAP);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  Serial1.println("[WEB] HTTP server started on port 80");
}

// ===== Optional tweaks =====
void tuneAP() {
  // Keep defaults; avoid LIGHT_SLEEP while brokering MQTT.
}

void setup() {
  Serial1.begin(74880);  // UART1 for logs
  delay(100);
  loadAPConfig();
  setupAP();
  tuneAP();
  setupPPP();
  setupMQTT();
  setupWeb();
}

void loop() {
  // Feed PPP input from UART0 (pppos)
  if (ppp) {
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

  // Service HTTP
  server.handleClient();
  // MQTT Broker
  broker.update();
  yield();
}
