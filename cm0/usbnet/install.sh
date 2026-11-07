#!/bin/sh
# install.sh [--uninstall] - make the CM0's USB port network + serial at every boot.
#
#   sudo sh install.sh            # then reboot
#   sudo sh install.sh --uninstall  # back to the image's g_serial, then reboot
#
# Installs fwmame-usb-gadget (+ its two services) and a NetworkManager rule
# that leaves usb0 alone, and removes g_serial from cmdline.txt's
# modules-load (dwc2 stays). The first install saves cmdline.txt as
# cmdline.txt.fwmame-orig; --uninstall puts it back. config.txt (dwc2
# peripheral overlay, GPIO2 low / GPIO3 high) is the image's and is not touched.
set -e
cd "$(dirname "$0")"
[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
CMDLINE=/boot/firmware/cmdline.txt
NMCONF=/etc/NetworkManager/conf.d/90-fwmame-usb0.conf

if [ "$1" = --uninstall ]; then
  systemctl disable fwmame-usb-dhcp.service fwmame-usb-gadget.service || true
  rm -f /etc/systemd/system/fwmame-usb-dhcp.service /etc/systemd/system/fwmame-usb-gadget.service \
        /usr/local/sbin/fwmame-usb-gadget "$NMCONF"
  systemctl daemon-reload
  [ -f $CMDLINE.fwmame-orig ] && cp $CMDLINE.fwmame-orig $CMDLINE
  echo "uninstalled; cmdline.txt: $(cat $CMDLINE)"
  echo "reboot to get g_serial back"
  exit 0
fi

install -m 0755 fwmame-usb-gadget /usr/local/sbin/fwmame-usb-gadget
install -m 0644 fwmame-usb-gadget.service fwmame-usb-dhcp.service /etc/systemd/system/
install -m 0644 90-fwmame-usb0.conf "$NMCONF"
[ -f $CMDLINE.fwmame-orig ] || cp $CMDLINE $CMDLINE.fwmame-orig
sed -i 's/modules-load=dwc2,g_serial/modules-load=dwc2/' $CMDLINE
systemctl daemon-reload
systemctl enable fwmame-usb-gadget.service fwmame-usb-dhcp.service
echo "installed; cmdline.txt: $(cat $CMDLINE)"
echo "reboot to apply"
