#!/usr/bin/env bash
set -u

SERVICE="brohome-wifi.service"
failed=0

check() {
  local label="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    printf '[OK]   %s\n' "$label"
  else
    printf '[FAIL] %s\n' "$label"
    failed=1
  fi
}

echo 'BroHome server diagnostics'
echo
printf 'Architecture: %s\n' "$(uname -m)"
printf 'Python:       %s\n' "$(python3 --version 2>&1 || echo 'not found')"
printf 'IP addresses: %s\n' "$(hostname -I 2>/dev/null || echo 'not available')"
echo

check 'x86-64 architecture' test "$(uname -m)" = 'x86_64'
check 'Python 3 is installed' command -v python3
check 'ALSA playback tool is installed' command -v aplay
check 'BroHome service file exists' test -f "/etc/systemd/system/$SERVICE"
check 'BroHome service is active' systemctl is-active --quiet "$SERVICE"
check 'TCP port 5001 is listening' sh -c "ss -ltn 2>/dev/null | grep -q ':5001 '"

echo
if command -v aplay >/dev/null 2>&1; then
  echo 'Detected ALSA devices:'
  aplay -l 2>/dev/null || true
  echo
fi

echo 'Last BroHome log lines:'
journalctl -u "$SERVICE" -n 30 --no-pager 2>/dev/null || true

echo
if [[ $failed -eq 0 ]]; then
  echo 'All basic checks passed.'
else
  echo 'One or more checks failed. Use the messages above and docs/TROUBLESHOOTING.md.'
fi
exit "$failed"
