#!/bin/sh
# usbnet_test.sh - live test of USB networking on the CM0, nothing persistent.
#
# Swaps the image's g_serial gadget for a configfs composite gadget:
#   - RNDIS network (usb0) with Microsoft OS descriptors, so Windows binds its
#     built-in RNDIS driver without an .inf;
#   - ACM serial (ttyGS0), so the PC keeps a COM-port console during the test.
# usb0 gets 10.12.194.1/28 and a DHCP server (dnsmasq, no gateway, no DNS),
# so the PC adapter picks up 10.12.194.2-14 by itself.
#
# Unless /tmp/usbnet-keep exists after REVERT_S seconds, it tears the gadget
# down and loads g_serial again (the image's normal state). A reboot also
# restores g_serial. Run detached as root, because it ends the COM-port
# console it was started from:
#
#   sudo setsid nohup sh /tmp/usbnet_test.sh >/dev/null 2>&1 </dev/null &
#   sudo sh /tmp/usbnet_test.sh revert      # tear down now
#
# Log: /tmp/usbnet.log
REVERT_S=${REVERT_S:-600}
G=/sys/kernel/config/usb_gadget/fwmame
ADDR=10.12.194.1/28
PIDF=/tmp/usbnet-dnsmasq.pid

teardown() {
  [ -f $PIDF ] && kill "$(cat $PIDF)" 2>/dev/null; rm -f $PIDF
  if [ -d $G ]; then
    echo "" > $G/UDC 2>/dev/null
    rm -f $G/os_desc/c.1 $G/configs/c.1/rndis.usb0 $G/configs/c.1/acm.usb0
    rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 \
          $G/functions/rndis.usb0 $G/functions/acm.usb0 $G/strings/0x409 $G 2>/dev/null
  fi
  modprobe g_serial
  sleep 2
  systemctl restart serial-getty@ttyGS0
}

if [ "$1" = revert ]; then
  exec >>/tmp/usbnet.log 2>&1
  echo "--- revert by hand $(date)"
  teardown
  exit 0
fi

exec >/tmp/usbnet.log 2>&1
set -x
sleep 2                                   # let the starting shell return
modprobe -r g_serial
modprobe libcomposite
mkdir -p $G && cd $G || { teardown; exit 1; }
echo 0x1d6b > idVendor                    # Linux Foundation
echo 0x0104 > idProduct                   # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB
echo 0xEF > bDeviceClass                  # composite with interface association descriptors
echo 0x02 > bDeviceSubClass
echo 0x01 > bDeviceProtocol
mkdir -p strings/0x409
echo "fwmame-cm0" > strings/0x409/serialnumber
echo "FW-MAME" > strings/0x409/manufacturer
echo "FW2 CM0 USB network + serial" > strings/0x409/product
mkdir -p configs/c.1/strings/0x409
echo "RNDIS + ACM" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower
echo 1 > os_desc/use
echo 0xcd > os_desc/b_vendor_code
echo MSFT100 > os_desc/qw_sign
mkdir -p functions/rndis.usb0
echo 02:46:57:4d:00:01 > functions/rndis.usb0/host_addr   # fixed MACs: Windows keeps one adapter
echo 02:46:57:4d:00:02 > functions/rndis.usb0/dev_addr
[ -e functions/rndis.usb0/class ] && { echo ef > functions/rndis.usb0/class
  echo 04 > functions/rndis.usb0/subclass; echo 01 > functions/rndis.usb0/protocol; }
echo RNDIS > functions/rndis.usb0/os_desc/interface.rndis/compatible_id
echo 5162001 > functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id
mkdir -p functions/acm.usb0
ln -s functions/rndis.usb0 configs/c.1/   # RNDIS must be the first function for Windows
ln -s functions/acm.usb0 configs/c.1/
ln -s configs/c.1 os_desc/
ls /sys/class/udc > UDC
sleep 2
nmcli device set usb0 managed no
ip addr add $ADDR dev usb0
ip link set usb0 up
dnsmasq --interface=usb0 --bind-interfaces --port=0 --pid-file=$PIDF --log-dhcp \
        --dhcp-range=10.12.194.2,10.12.194.14,255.255.255.240,10m \
        --dhcp-option=3 --dhcp-option=6 --dhcp-leasefile=/tmp/usbnet.leases
systemctl restart serial-getty@ttyGS0
ip addr show usb0
cat /sys/class/udc/*/state

sleep "$REVERT_S"
if [ -f /tmp/usbnet-keep ]; then
  echo "keep flag found: leaving the gadget up"
  exit 0
fi
echo "--- auto revert $(date)"
teardown
