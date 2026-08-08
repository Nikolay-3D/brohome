#!/usr/bin/env bash
set -Eeuo pipefail

BROHOME_DIR="${BROHOME_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)}"
export BROHOME_DIR
PYTHON="$BROHOME_DIR/venv/bin/python"
APP="$BROHOME_DIR/bro_home_wifi_v3.py"

cd "$BROHOME_DIR"

echo "Starting BroHome Wi-Fi voice server..."
exec "$PYTHON" -u "$APP"
