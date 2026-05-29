#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# setup_companion_ap.sh — make the Raspberry Pi CM5 broadcast its own Wi-Fi
# network so the ProtoHUD companion app (phone) can join it directly, no
# infrastructure AP required.
#
# After running this, the Pi serves SSID "ProtoHUD" on 192.168.50.1 and the
# companion control server is reachable at  http://192.168.50.1:8780
# (set "network_control.enabled": true in config/config.json).
#
# Uses NetworkManager (default on Raspberry Pi OS Bookworm/Trixie), which makes
# an AP a one-liner and coexists with a wired/secondary uplink. Re-runnable.
#
# Usage:
#   sudo ./scripts/setup_companion_ap.sh [SSID] [PASSPHRASE] [IFACE]
# Defaults:
#   SSID="ProtoHUD"  PASSPHRASE="protohud24"  IFACE=wlan0
#
# To tear down:   sudo nmcli connection down ProtoHUD-AP
#                 sudo nmcli connection delete ProtoHUD-AP
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SSID="${1:-ProtoHUD}"
PASS="${2:-protohud24}"
IFACE="${3:-wlan0}"
CON="ProtoHUD-AP"
AP_IP="192.168.50.1/24"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo)." >&2; exit 1
fi

if (( ${#PASS} < 8 )); then
  echo "WPA passphrase must be at least 8 characters." >&2; exit 1
fi

if ! command -v nmcli >/dev/null 2>&1; then
  cat >&2 <<'EOF'
nmcli (NetworkManager) not found. This script targets Raspberry Pi OS
Bookworm/Trixie where NetworkManager is the default. On a hostapd-based setup,
configure hostapd + dnsmasq manually to serve SSID "ProtoHUD" on 192.168.50.1.
EOF
  exit 1
fi

echo "Creating Wi-Fi access point '${SSID}' on ${IFACE} (${AP_IP})…"

# Remove any prior copy so the script is idempotent.
nmcli connection delete "${CON}" >/dev/null 2>&1 || true

nmcli connection add type wifi ifname "${IFACE}" con-name "${CON}" \
  autoconnect yes ssid "${SSID}"

nmcli connection modify "${CON}" \
  802-11-wireless.mode ap \
  802-11-wireless.band bg \
  ipv4.method shared \
  ipv4.addresses "${AP_IP}" \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "${PASS}"

nmcli connection up "${CON}"

cat <<EOF

✓ Access point is up.
    SSID:        ${SSID}
    Passphrase:  ${PASS}
    Pi address:  ${AP_IP%/*}

Next:
  1. In config/config.json set  "network_control": { "enabled": true }
  2. Restart protohud.
  3. On the phone, join Wi-Fi "${SSID}", open the companion app, and connect to
     http://${AP_IP%/*}:8780  (this is the app's default AP host).

This AP autostarts on boot. 'ipv4.method shared' also hands out DHCP + NAT, so
the phone gets an address automatically and (if the Pi has another uplink) can
still reach the internet.
EOF
