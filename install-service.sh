#!/bin/sh
# Install telegramshell as a systemd user service for the current user.
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR="$SCRIPT_DIR"
BIN_SRC="$REPO_DIR/telegramshell"
ENV_SRC="$REPO_DIR/.env"

INSTALL_DIR="$HOME/.local/bin"
CONFIG_DIR="$HOME/.config/telegramshell"
UNIT_DIR="$HOME/.config/systemd/user"
SERVICE="$UNIT_DIR/telegramshell.service"
BIN_DST="$INSTALL_DIR/telegramshell"
ENV_DST="$CONFIG_DIR/.env"

# Build the binary if it does not exist.
if [ ! -x "$BIN_SRC" ]; then
    echo "Binary not found, building..."
    ( cd "$REPO_DIR" && ${MAKE:-make} )
fi
if [ ! -x "$BIN_SRC" ]; then
    echo "error: binary missing at $BIN_SRC (build failed?)" >&2
    exit 1
fi

# Require a .env with configuration.
if [ ! -f "$ENV_SRC" ]; then
    echo "error: no .env found in $REPO_DIR" >&2
    echo "Create one, e.g.:" >&2
    echo "  TELEGRAM_BOT_TOKEN=123456:ABC-your-token" >&2
    echo "  ALLOWED_CHAT_ID=987654321" >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR" "$CONFIG_DIR" "$UNIT_DIR"

echo "Installing binary -> $BIN_DST"
install -m 0755 "$BIN_SRC" "$BIN_DST"

echo "Installing .env   -> $ENV_DST"
install -m 0600 "$ENV_SRC" "$ENV_DST"

cat > "$SERVICE" <<'EOF'
[Unit]
Description=Telegram Shell Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=%h/.config/telegramshell
EnvironmentFile=%h/.config/telegramshell/.env
ExecStart=%h/.local/bin/telegramshell
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF

echo "Reloading systemd user daemon..."
systemctl --user daemon-reload

echo "Enabling and starting telegramshell.service..."
systemctl --user enable --now telegramshell.service

echo
systemctl --user status telegramshell.service --no-pager || true

# Hint about linger so the service survives logout.
if command -v loginctl >/dev/null 2>&1; then
    if loginctl show-user "$USER" 2>/dev/null | grep -q "Linger=yes"; then
        :
    else
        echo
        echo "Tip: to keep the service running after you log out, enable linger:"
        echo "  loginctl enable-linger $USER"
    fi
fi
