#!/usr/bin/env bash
# install_sudoers.sh — drop the protohud sudoers rule into /etc/sudoers.d/
# so the Pi Settings menu items work without an interactive password prompt.
#
# Usage:  sudo bash scripts/install_sudoers.sh [user]
#         (defaults to the SUDO_USER, or "serin" if invoked directly as root)
#
# The granted commands are intentionally narrow — only the system-mutation
# tools the System > Pi Settings menu and scripts/restart.sh actually call:
#   hostnamectl, timedatectl, apt-get, poweroff, reboot,
#   systemctl {start,stop,restart} protohud.service
# (start/stop let scripts/restart.sh manage the service from a non-root context
#  such as a GPIO-button handler.)
#
# If you don't have a `protohud.service` unit yet, the Restart ProtoHUD menu
# item will fail with a "Failed" notif — that's expected; create the unit
# (or remove that one line from the sudoers file) when you're ready.

set -euo pipefail
if [[ $EUID -ne 0 ]]; then
  echo "must be run as root: sudo bash scripts/install_sudoers.sh"
  exit 1
fi

USER_NAME="${1:-${SUDO_USER:-serin}}"
DST="/etc/sudoers.d/protohud"

cat >"${DST}.tmp" <<EOF
${USER_NAME} ALL=(ALL) NOPASSWD: \
    /usr/bin/hostnamectl, \
    /usr/bin/timedatectl, \
    /usr/bin/apt-get, \
    /usr/sbin/poweroff, \
    /usr/sbin/reboot, \
    /usr/bin/systemctl start protohud.service, \
    /usr/bin/systemctl stop protohud.service, \
    /usr/bin/systemctl restart protohud.service
EOF

# visudo validates the syntax before activating, so a typo can't lock you out.
visudo -cf "${DST}.tmp"
mv "${DST}.tmp" "${DST}"
chmod 0440 "${DST}"
echo "installed ${DST} for user '${USER_NAME}'"
