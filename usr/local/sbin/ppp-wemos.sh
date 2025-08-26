#!/bin/bash
stty -F /dev/ttyUSB0 115200 -echo -ixon -ixoff -crtscts raw
exec /usr/sbin/pppd /dev/ttyUSB0 call wemos

