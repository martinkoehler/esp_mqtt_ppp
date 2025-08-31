# Bug in ch341 driver
The ch341 driver has a bug which results garbage when connecting e.g. a Wemos D1 mini (ch341 chip).
It is neccessary to patch one line in ch341_configure():

```C
*** source/kernel/ref-ar10-3272_06.86/linux-2.6.32/drivers/usb/serial/ch341.c.orig
--- source/kernel/ref-ar10-3272_06.86/linux-2.6.32/drivers/usb/serial/ch341.c
*************** static int ch341_configure(struct usb_device *dev, struct ch341_private *priv)
*** 212,218 ****
-         r = ch341_control_out(dev, 0x9a, 0x2518, 0x0050);
+         /* Set 8N1 + enable RX/TX: LCR = 0xC3 (see upstream CH341_LCR_* bits) */
+         r = ch341_control_out(dev, 0x9a, 0x2518, 0x00C3);
          if (r < 0)
                  goto out;
```

## Reason
The driver initialises LCR to 0x0050, which (per upstream docs) is TX enable + even-parity, 
but CS5 (bits 0..1 = 0). 
We need 8N1 with RX+TX: LCR = 0xC3 (0x80 RX enable | 0x40 TX enable | 0x03 CS8). 
The LCR registers are written via the paired index 0x2518 (LCR2<<8 | LCR).

in current kernels the LCR bits are
ENABLE_RX=0x80, ENABLE_TX=0x40, STOP2=0x04, ENABLE_PAR=0x08, PAR_EVEN=0x10, 
and CS{5..8} in bits 0..1; so 0xC3 = RX+TX+CS8. 
The registers are LCR=0x18, LCR2=0x25, written together as value=(0x25<<8)|0x18 (i.e., 0x2518).
See (github)[https://github.com/torvalds/linux/blob/master/drivers/usb/serial/ch341.c?utm_source=chatgpt.com)

## Building
I assume you have the freetz-ng toolchain and know the kernel you are building for. The makefile are from
(wCHSoftgroup)[https://github.com/wCHSoftGroup/ch341ser_linux/]

```sh
KDIR=/workspace/freetz-ng.orig/source/kernel/ref-ar10-3272_06.86/linux-2.6.32

# if the tree isn’t prepared yet, generate the needed headers
make -C "$KDIR" ARCH=mips CROSS_COMPILE=mips-linux- oldconfig prepare modules_prepare
```


# Persistence in FREETZ-NG
How to persist `/etc/ppp/peers/wemos` in a FREETZ(-NG) image
By default, `/etc/ppp/peers/wemos` is in RAM and will be lost after reboot unless you include it in your Freetz image or use the Freetz mod filesystem.
Recommended:

Add your custom peer file to your Freetz build (so it’s included in the firmware and survives reboots).
Use rc.custom for runtime scripts.

A. Add /etc/ppp/peers/wemos to your Freetz-NG build:
On your build host, create the file in `root/etc/ppp/peers/` in your Freetz-NG build directory:
```sh
mkdir -p root/etc/ppp/peers
nano root/etc/ppp/peers/wemos
``` 
Build your Freetz(-NG) image as usual.
The file will be included in the firmware and available after every reboot.

B. Alternatively: Use the mod filesystem (for testing or changes after flashing)
Place (or upload) the file to /var/mod/etc/ppp/peers/wemos via SSH, FTP, or the Freetz web interface (File Editor or File Manager plugin).
If you want persistence after reboot, make sure your mod filesystem is enabled and persistent.
