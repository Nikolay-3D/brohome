#!/usr/bin/env bash
set -Eeuo pipefail

SERVICE_NAME="brohome-wifi.service"
PACKAGE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_USER="${SUDO_USER:-}"
INSTALL_DIR=""
SKIP_PACKAGES=0

usage() {
  cat <<'EOF'
Usage: sudo ./install.sh [options]

Options:
  --user USER          Linux user that will run BroHome (default: user who ran sudo)
  --install-dir PATH   Installation directory (default: USER_HOME/brohome)
  --skip-packages      Do not run apt-get; useful when dependencies are installed
  -h, --help           Show this help
EOF
}

while (($#)); do
  case "$1" in
    --user)
      INSTALL_USER="${2:?--user requires a value}"
      shift 2
      ;;
    --install-dir)
      INSTALL_DIR="${2:?--install-dir requires a value}"
      shift 2
      ;;
    --skip-packages)
      SKIP_PACKAGES=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this installer through sudo." >&2
  exit 1
fi

if [[ -z "$INSTALL_USER" ]]; then
  echo "Cannot determine the regular user. Run: sudo ./install.sh --user YOUR_NAME" >&2
  exit 1
fi

if ! id "$INSTALL_USER" >/dev/null 2>&1; then
  echo "Linux user does not exist: $INSTALL_USER" >&2
  exit 1
fi

USER_HOME="$(getent passwd "$INSTALL_USER" | cut -d: -f6)"
USER_GROUP="$(id -gn "$INSTALL_USER")"
INSTALL_DIR="${INSTALL_DIR:-$USER_HOME/brohome}"
VENV_PYTHON="$INSTALL_DIR/venv/bin/python"

if [[ "$INSTALL_DIR" != /* || "$INSTALL_DIR" == "/" ]]; then
  echo "Installation directory must be a safe absolute path." >&2
  exit 1
fi

for required in \
  "$PACKAGE_DIR/app/bro_home_wifi_v3.py" \
  "$PACKAGE_DIR/app/models/vosk-ru/am/final.mdl" \
  "$PACKAGE_DIR/vendor/wheels"; do
  if [[ ! -e "$required" ]]; then
    echo "Package is incomplete; missing: $required" >&2
    exit 1
  fi
done

PRESERVE_DIR="$(mktemp -d)"
trap 'rm -rf -- "$PRESERVE_DIR"' EXIT

if [[ $SKIP_PACKAGES -eq 0 ]]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y \
    alsa-utils \
    ca-certificates \
    curl \
    espeak-ng \
    python3 \
    python3-pip \
    python3-venv
fi

if [[ "$(uname -m)" != "x86_64" ]]; then
  echo "This offline package currently supports Linux x86_64 only." >&2
  exit 1
fi

if ! python3 - <<'PY'
import sys

if not ((3, 10) <= sys.version_info[:2] <= (3, 12)):
    raise SystemExit(
        "Supported Python versions are 3.10, 3.11, and 3.12; found "
        + sys.version.split()[0]
    )
PY
then
  exit 1
fi

systemctl stop "$SERVICE_NAME" 2>/dev/null || true

for name in channels.json devices.json; do
  if [[ -f "$INSTALL_DIR/$name" ]]; then
    cp -a -- "$INSTALL_DIR/$name" "$PRESERVE_DIR/$name"
  fi
done

if [[ -d "$INSTALL_DIR" ]]; then
  BACKUP_DIR="$INSTALL_DIR.backup-$(date +%Y%m%d-%H%M%S)"
  cp -a -- "$INSTALL_DIR" "$BACKUP_DIR"
  echo "Existing installation backup: $BACKUP_DIR"
  rm -rf -- "$INSTALL_DIR"
fi

install -d -m 0755 -o "$INSTALL_USER" -g "$USER_GROUP" "$INSTALL_DIR"
cp -a -- "$PACKAGE_DIR/app/." "$INSTALL_DIR/"
install -m 0644 "$PACKAGE_DIR/requirements.txt" "$INSTALL_DIR/requirements.txt"
touch "$INSTALL_DIR/.brohome-installation"

for name in channels.json devices.json; do
  if [[ -f "$PRESERVE_DIR/$name" ]]; then
    cp -a -- "$PRESERVE_DIR/$name" "$INSTALL_DIR/$name"
  fi
done

chown -R "$INSTALL_USER:$USER_GROUP" "$INSTALL_DIR"
chmod 0755 "$INSTALL_DIR/start_brohome_wifi.sh"
chmod 0644 "$INSTALL_DIR"/*.json

rm -rf -- "$INSTALL_DIR/venv"
runuser -u "$INSTALL_USER" -- python3 -m venv "$INSTALL_DIR/venv"
runuser -u "$INSTALL_USER" -- \
  "$VENV_PYTHON" -m pip install \
  --no-index \
  --find-links "$PACKAGE_DIR/vendor/wheels" \
  --requirement "$PACKAGE_DIR/requirements.txt"

runuser -u "$INSTALL_USER" -- "$VENV_PYTHON" -m py_compile "$INSTALL_DIR"/*.py
runuser -u "$INSTALL_USER" -- \
  "$VENV_PYTHON" -c 'import speech_recognition; import vosk; print("Python dependencies: OK")'

if [[ ! -f /etc/default/brohome-wifi ]]; then
  install -m 0644 \
    "$PACKAGE_DIR/brohome-wifi.env.example" \
    /etc/default/brohome-wifi
fi

cat >"/etc/systemd/system/$SERVICE_NAME" <<EOF
[Unit]
Description=BroHome Wi-Fi Voice Assistant
Wants=network-online.target
After=network-online.target sound.target
StartLimitIntervalSec=60
StartLimitBurst=10

[Service]
Type=simple
User=$INSTALL_USER
Group=$USER_GROUP
SupplementaryGroups=audio
WorkingDirectory=$INSTALL_DIR
Environment=HOME=$USER_HOME
Environment=BROHOME_DIR=$INSTALL_DIR
Environment=PYTHONUNBUFFERED=1
Environment=PATH=$INSTALL_DIR/venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
EnvironmentFile=-/etc/default/brohome-wifi
ExecStart=$INSTALL_DIR/start_brohome_wifi.sh
Restart=always
RestartSec=5
TimeoutStopSec=10
KillSignal=SIGINT

[Install]
WantedBy=multi-user.target
EOF

usermod -a -G audio "$INSTALL_USER"
systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"

sleep 8
if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  systemctl status "$SERVICE_NAME" --no-pager || true
  journalctl -u "$SERVICE_NAME" -n 50 --no-pager || true
  echo "BroHome service failed to start." >&2
  exit 1
fi

echo
echo "BroHome installed successfully."
echo "Directory: $INSTALL_DIR"
echo "Service:   $SERVICE_NAME"
echo "Status:    systemctl status $SERVICE_NAME --no-pager"
echo "Logs:      journalctl -u $SERVICE_NAME -f"
