# esp_mqtt_ppp
esp8266 (wemos D1) STA with mqtt broker and ppp connection.
The Wemos opens an WLAN Access point where clients can connect to and access the wemos MQTT Broker.

## Prerequisites
Need uMQTTBroker from [martin-ger](https://github.com/martin-ger/uMQTTBroker) and lwip with NAT support

You can use the arduino IDE to compile & flash.
Tested with Board manager "ESP8266 by ESP8266 community" version 3.1.2 and "lwip v2 High bandwidth". The board I seelcted is "Lolin(Wemos) D1 R2 & mini"
For the uMQTTBroker library it is the easiest way to download the .zip file from github and include in the arduino IDE via Sketch -> Include library -> Add .ZIP Library.


## Usage
The Wemos is connected to the host via USB using the built in ch341 chip. The "endpoint" on the host is usually something like /dev/ttyUSB0.
The internal IP of the Wemos is 192.168.4.1 in the 192.168.4.0/24 subnet
The Wemos can be reached via PPP using 192.168.178.50

These settings can be changed in the source code, only.

### Setup
Host <--> Wemos D1 ( <-- Client(s))

### Compile and flash

You need martinger's uMQTTBroker (https://github.com/martin-ger/uMQTTBroker) and lwip with NAT support.

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

To make this persistent:
#### PPP Configuration File
Move the PPP options from ./etc/ppp/peers/wemos to /etc/ppp/peers/wemos:

```bash
sudo cp ./etc/ppp/peers/wemos /etc/ppp/peers/wemos
```
#### Serial Port Setup
The stty settings must be applied before PPP starts. You can do this in a script or with the connect option in PPP.

Option A: Use a Wrapper Script

Copy the script form ./usr/local/sbin/ppp-wemos.sh to /usr/local/sbin/ppp-wemos.sh:

```bash
sudo cp  ./usr/local/sbin/ppp-wemos.sh /usr/local/sbin/ppp-wemos.sh
```
#### Routing Automation
To automatically add the route when the PPP interface comes up, use the ip-up script:

Copy the script from ./etc/ppp/ip-up.d/wemos-route to /etc/ppp/ip-up.d/wemos-route
```bash
sudo cp  ./etc/ppp/ip-up.d/wemos-route /etc/ppp/ip-up.d/wemos-route
```

#### Auto-Start at Boot
Use a systemd service to bring up the connection at boot.
Copy etc/systemd/system/ppp-wemos.service to /etc/systemd/system/ppp-wemos.service

```bash
sudo cp ./etc/systemd/system/ppp-wemos.service /etc/systemd/system/ppp-wemos.service
```
Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ppp-wemos
```

## NAT Routing

If you want the clients of the Wemos to access other hosts of your network e.g. your router via the host, this works thanks to NAT support  of the Wemos code.

However usually your host does not forward packages, so this must be enabled.
On linux use:

```sh
sudo sysctl -w net.ipv4.ip_forward=1
```
Make IP forwarding permanent (survives reboot):
Edit /etc/sysctl.conf and ensure this line is present and not commented out:

```Code
net.ipv4.ip_forward=1
```
Then reload the settings:
```sh
sudo sysctl -p
```

The  host must know where to forward packages and needs to do NATing, too:

### Install iptables-persistent
```sh
sudo apt update
sudo apt install iptables-persistent
```

During installation, it may ask if you want to save existing rules—choose "Yes".

### Add iptables Rules
Run these commands to add your NAT and forwarding rules in the firewall:

```sh
sudo iptables -t nat -A POSTROUTING -o enp0s31f6 -j MASQUERADE
sudo iptables -A FORWARD -i ppp0 -o enp0s31f6 -j ACCEPT
sudo iptables -A FORWARD -i enp0s31f6 -o ppp0 -m state --state RELATED,ESTABLISHED -j ACCEPT
```
### Save the Rules
Save the current IPv4 rules:

```sh
sudo netfilter-persistent save
```
or

```sh
sudo iptables-save > /etc/iptables/rules.v4
```

### Ensure IP Forwarding Is Persistent
Edit /etc/sysctl.conf, make sure this line is present and uncommented:

```Code
net.ipv4.ip_forward=1
```
Then apply:

```sh
sudo sysctl -p
```
### (Optional) Reload and Test
To reload rules without rebooting:

```sh
sudo netfilter-persistent reload
```
or
```sh
sudo iptables-restore < /etc/iptables/rules.v4
```
