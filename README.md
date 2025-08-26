# esp_mqtt_ppp
esp8266 (wemos D1) STA with mqtt broker and ppp connection.
The Wemos opens an WLAN Access point where clients can connect to and access the wemos MQTT Broker.

## Prerequisites
Need uBroker from martinger

## Usage
The Wemos is connected to the host via USB using the built in ch341 chip. The "endpoint" on the host is usually something like /dev/ttyUSB0.
The internal IP of the Wemos is 192.168.4.1 in the 192.168.4.0/24 subnet
The Wemos can be reached via PPP using 192.168.178.50

These settings can be changed in the source code, only.

### Setup
Host <--> Wemos D1 ( <-- Client(s))

### Compile and flash

You need martinger's uMQTTBroker (https://github.com/martin-ger/uMQTTBroker).

### PPP connection

On the host side establish the PPP connection using:
```
stty -F /dev/ttyUSB0 115200 -echo -ixon -ixoff -crtscts raw
sudo pppd /dev/ttyUSB0 file ./options.usb-wemos nodetach
sudo ip route add 192.168.4.0/24 via 192.168.178.50 dev ppp0
```

You can now access the Wemos Webserver via http://192.168.178.50 in order to configure the SSID and password.
After "Save & Reboot" clients can connect to the Wemos using this data.

The Webserver shows the IP of connected client. Due to the `route add` command, these clients can be reached directly from the host (e.g. http://192.168.4.100)
