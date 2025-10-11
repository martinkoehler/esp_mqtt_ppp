#!/bin/sh
# Example recovery steps (tune to your network).
# WARNING: 'sudo' prompts for password unless NOPASSWD is configured.
#sudo ip route add 192.168.4.0/24 via 192.168.178.50 dev ppp0 2>/dev/null || true
echo Recreate route
sudo ip route add 192.168.4.0/24 via 192.168.178.50 dev ppp0
echo done
sleep 10
