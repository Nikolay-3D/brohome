#!/usr/bin/env bash
set -Eeuo pipefail

SERVICE_NAME="brohome-wifi.service"
PURGE=0
INSTALL_DIR=""

while (($#)); do
  case "$1" in
    --purge)
      PURGE=1
      shift
      ;;
    --install-dir)
      INSTALL_DIR="${2:?--install-dir requires a value}"
      shift 2
      ;;
    *)
      echo "Usage: sudo ./uninstall.sh [--purge --install-dir PATH]" >&2
      exit 2
      ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this script through sudo." >&2
  exit 1
fi

systemctl disable --now "$SERVICE_NAME" 2>/dev/null || true
rm -f -- "/etc/systemd/system/$SERVICE_NAME"
systemctl daemon-reload

if [[ $PURGE -eq 1 ]]; then
  if [[ 
    -z "$INSTALL_DIR"
    || "$INSTALL_DIR" != /*
    || "$INSTALL_DIR" == "/"
    || ! -f "$INSTALL_DIR/.brohome-installation"
  ]]; then
    echo "--purge requires the exact BroHome directory containing .brohome-installation." >&2
    exit 1
  fi
  rm -rf -- "$INSTALL_DIR"
  echo "Removed application directory: $INSTALL_DIR"
else
  echo "Service removed. Application files and user settings were preserved."
fi
