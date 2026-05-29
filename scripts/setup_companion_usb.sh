#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# setup_companion_usb.sh — expose a USB-ethernet gadget on the Pi's USB-C OTG
# port so a tethered phone/laptop gets a wired, point-to-point link to the
# helmet. The companion control server is then reachable at
#   http://192.168.42.1:8780
#
# This is the "USB tethering" path the companion app offers alongside the
# broadcast-Wi-Fi (AP) path. It needs no radios — handy for noisy RF environments
# or for bench bring-up.
#
# Mechanism: the libcomposite / g_ether USB gadget. The CM5's USB-C port must be
# in peripheral (device) mode for this to work — verify your carrier board routes
# the OTG port to the phone, not a host hub.
#
# Usage:   sudo ./scripts/setup_companion_usb.sh
# Removes: sudo modprobe -r g_ether   (and delete the dhcp/IP drop-ins below)
#
# NOTE: Android does not auto-configure RNDIS/ECM tethered links the way a laptop
# does. On the phone you typically must accept the "USB ethernet" prompt; some
# Android builds expose it under Settings → Connections → More → USB. The app's
# USB-tether help screen walks through this.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

GADGET_IP="192.168.42.1"
GADGET_CIDR="${GADGET_IP}/24"
USB_IFACE="usb0"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo)." >&2; exit 1
fi

echo "Loading g_ether USB-ethernet gadget…"
# g_ether presents both ECM (Linux/macOS) and RNDIS (Windows/Android) configs.
modprobe g_ether || {
  echo "Could not load g_ether. Ensure 'dtoverlay=dwc2' is in /boot/firmware/config.txt" >&2
  echo "and 'modules-load=dwc2' is appended to /boot/firmware/cmdline.txt, then reboot." >&2
  exit 1
}

# Give the gadget interface a moment to appear, then assign a static address.
for _ in $(seq 1 10); do
  ip link show "${USB_IFACE}" >/dev/null 2>&1 && break
  sleep 0.5
done

if ! ip link show "${USB_IFACE}" >/dev/null 2>&1; then
  echo "Interface ${USB_IFACE} did not appear. Is the OTG port in device mode?" >&2
  exit 1
fi

ip addr flush dev "${USB_IFACE}" || true
ip addr add "${GADGET_CIDR}" dev "${USB_IFACE}"
ip link set "${USB_IFACE}" up

# Hand the tethered device an address so it needs zero manual config. dnsmasq is
# the lightest option; fall back to a static-only link if it's not installed.
if command -v dnsmasq >/dev/null 2>&1; then
  pkill -f "dnsmasq.*${USB_IFACE}" 2>/dev/null || true
  dnsmasq \
    --interface="${USB_IFACE}" \
    --bind-interfaces \
    --dhcp-range=192.168.42.10,192.168.42.50,255.255.255.0,1h \
    --dhcp-option=3 \
    --dhcp-option=6 \
    --except-interface=lo \
    --pid-file=/run/protohud-usb-dnsmasq.pid
  echo "dnsmasq serving DHCP on ${USB_IFACE}."
else
  echo "dnsmasq not installed — link is static-only."
  echo "On the tethered device set IP 192.168.42.2/24 manually."
fi

cat <<EOF

✓ USB-ethernet gadget is up on ${USB_IFACE} (${GADGET_IP}).

Next:
  1. In config/config.json set  "network_control": { "enabled": true }
  2. Restart protohud.
  3. Plug the phone into the Pi's USB-C OTG port, accept any "USB ethernet"
     prompt on the phone, then in the companion app connect to
     http://${GADGET_IP}:8780  (this is the app's default USB-tether host).

To make this persist across reboots, add 'dtoverlay=dwc2' to
/boot/firmware/config.txt and run this script from a systemd unit / rc.local.
EOF
