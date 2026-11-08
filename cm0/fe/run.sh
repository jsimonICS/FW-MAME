#!/bin/sh
# run.sh <game> [fwmame options] - start a game on the FW2's CRT from a fresh CM0 boot.
#
#   ~/fwmame/run.sh                        # game menu: pick games on the FW2 screen
#   ~/fwmame/run.sh mappy                  # one game, until Ctrl+C
#   ~/fwmame/run.sh 1942 --seconds 600
#
# Expects the FW2 display app (fw2/apps/fwmame) to be running, so the fbuf
# FPGA design is loaded and the buttons are forwarded. Then, as root:
#   - stops fwcm0-bridge (it owns SPI0, the UART and GPIO21; a plain stop
#     takes 90 s because it ignores SIGTERM, so kill it);
#   - reloads spidev with 64 KB buffers, so a frame is one SPI transfer;
#   - pins the CPU governor to performance (the SPI clock follows the core
#     clock, which drops to 287.5 MHz at idle under ondemand);
#   - runs the frontend with ~/fwmame/roms/<game>.zip.
# None of this survives a reboot; run it again after one.
set -e
G=${1:-menu}                     # no game, or "menu": the FW2 display app's game menu
[ $# -gt 0 ] && shift
D=$(cd "$(dirname "$0")" && pwd)
# Always stop it: with fbuf loaded the bridge can't reach its FPGA mailbox,
# fails, and systemd restarts it every 5 s ("activating", which is-active
# doesn't count), reopening SPI0, the UART and GPIO21 each time.
sudo systemctl kill -s KILL fwcm0-bridge 2>/dev/null || true
sudo systemctl stop fwcm0-bridge
if [ "$(cat /sys/module/spidev/parameters/bufsiz)" -lt 65536 ]; then
  sudo modprobe -r spidev
  sudo modprobe spidev bufsiz=65536
  sleep 1
fi
for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
  echo performance | sudo tee "$c" >/dev/null
done
if [ "$G" = menu ]; then
  exec sudo "$D/src/fe/fwmame" "$D/cores/mame2003_plus_libretro.so" --menu "$D/roms" "$@"
fi
exec sudo "$D/src/fe/fwmame" "$D/cores/mame2003_plus_libretro.so" "$D/roms/$G.zip" "$@"
