#/bin/bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <remote-host> <path-to-daemon-binary>"
  exit 1
fi

REMOTE_HOST="$1"
BINARY_PATH="$2"
PORT=32099
REMOTE_INSTALL_DIR="/opt/zfsmgr-daemon"
REMOTE_KEY_DIR="/etc/zfsmgr"
KEY_FILE="${HOME}/.zfsmgr/zfsmgr_daemon_id.pub"

if [[ ! -f "$KEY_FILE" ]]; then
  echo "Public key not found: $KEY_FILE" >&2
  exit 1
fi

ssh "$REMOTE_HOST" <<'SSH_SETUP1'
set -e
sudo mkdir -p "$REMOTE_INSTALL_DIR"
sudo mkdir -p "$REMOTE_KEY_DIR"
SSH_SETUP1

scp "$KEY_FILE" "$REMOTE_HOST":/tmp/zfsmgr_allowed_keys.pub
scp "$BINARY_PATH" "$REMOTE_HOST":/tmp/zfsmgr_daemon.bin

ssh "$REMOTE_HOST" <<'SSH_SETUP2'
set -e
sudo mv /tmp/zfsmgr_daemon.bin "$REMOTE_INSTALL_DIR/zfsmgr_daemon"
sudo chmod 755 "$REMOTE_INSTALL_DIR/zfsmgr_daemon"
sudo mv /tmp/zfsmgr_allowed_keys.pub "$REMOTE_KEY_DIR/allowed_keys"

if [[ $OSTYPE == "linux-gnu" ]]; then
  sudo tee /etc/systemd/system/zfsmgr-daemon.service >/dev/null <<'SERVICE'
[Unit]
Description=ZFSMgr daemon
After=network.target

[Service]
ExecStart=${REMOTE_INSTALL_DIR}/zfsmgr_daemon --port ${PORT}
Restart=always

[Install]
WantedBy=multi-user.target
SERVICE
  sudo systemctl enable --now zfsmgr-daemon.service
  sudo ufw allow ${PORT}/tcp || true
  sudo firewall-cmd --add-port=${PORT}/tcp --permanent && sudo firewall-cmd --reload || true
elif [[ $OSTYPE == "darwin" ]]; then
  sudo tee /Library/LaunchDaemons/com.zfsmgr.daemon.plist >/dev/null <<'LABEL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
  <dict>
    <key>Label</key>
    <string>com.zfsmgr.daemon</string>
    <key>ProgramArguments</key>
    <array>
      <string>${REMOTE_INSTALL_DIR}/zfsmgr_daemon</string>
      <string>--port</string>
      <string>${PORT}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
  </dict>
</plist>
LABEL
  sudo launchctl bootstrap system /Library/LaunchDaemons/com.zfsmgr.daemon.plist || true
  sudo pfctl -f /etc/pf.conf
elif [[ $OSTYPE == *"freebsd"* ]]; then
  sudo service zfsmgr_daemon stop || true
  sudo cp "$REMOTE_INSTALL_DIR/zfsmgr_daemon" /usr/local/bin/
  sudo tee /usr/local/etc/rc.d/zfsmgr_daemon >/dev/null <<'RC'
#!/bin/sh
# PROVIDE: zfsmgr_daemon
# REQUIRE: NETWORKING
# KEYWORD: shutdown

. /etc/rc.subr

name="zfsmgr_daemon"
rcvar="zfsmgr_daemon_enable"
command="${REMOTE_INSTALL_DIR}/zfsmgr_daemon"
pidfile="/var/run/zfsmgr_daemon.pid"

load_rc_config $name
run_rc_command "$1"
RC
  sudo chmod 755 /usr/local/etc/rc.d/zfsmgr_daemon
  sudo sysrc zfsmgr_daemon_enable=YES
  sudo service zfsmgr_daemon start
  sudo pfctl -f /etc/pf.conf
else
  printf "Instalación manual requerida en Windows\n" >&2
fi
SSH_SETUP2
