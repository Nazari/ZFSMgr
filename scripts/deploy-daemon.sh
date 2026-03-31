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
REMOTE_SUDO_PASSWORD="${REMOTE_SUDO_PASSWORD:-}"

shq() {
  local s="$1"
  s="${s//\'/\'\"\'\"\'}"
  printf "'%s'" "$s"
}

REMOTE_ENV="REMOTE_INSTALL_DIR=$(shq "$REMOTE_INSTALL_DIR") REMOTE_KEY_DIR=$(shq "$REMOTE_KEY_DIR") PORT=$(shq "$PORT")"
if [[ -n "$REMOTE_SUDO_PASSWORD" ]]; then
  REMOTE_ENV+=" REMOTE_SUDO_PASSWORD=$(shq "$REMOTE_SUDO_PASSWORD")"
fi

if [[ ! -f "$KEY_FILE" ]]; then
  echo "Public key not found: $KEY_FILE" >&2
  exit 1
fi

REMOTE_ARCH="$(ssh "$REMOTE_HOST" 'uname -m' 2>/dev/null || true)"
LOCAL_ARCH_INFO="$(file -b "$BINARY_PATH" 2>/dev/null || true)"
if [[ -n "$REMOTE_ARCH" && -n "$LOCAL_ARCH_INFO" ]]; then
  if [[ "$REMOTE_ARCH" == "x86_64" && "$LOCAL_ARCH_INFO" != *"x86_64"* ]]; then
    echo "Remote host is x86_64 but $BINARY_PATH is not. Rebuild the daemon for x86_64 or as a universal binary." >&2
    exit 1
  fi
  if [[ "$REMOTE_ARCH" == "arm64" && "$LOCAL_ARCH_INFO" != *"arm64"* ]]; then
    echo "Remote host is arm64 but $BINARY_PATH is not. Rebuild the daemon for arm64 or as a universal binary." >&2
    exit 1
  fi
fi

ssh "$REMOTE_HOST" "$REMOTE_ENV bash -s" <<'SSH_SETUP1'
set -e
rsudo() {
  if [[ -n "${REMOTE_SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "$REMOTE_SUDO_PASSWORD" | sudo -S -p '' "$@"
  else
    sudo "$@"
  fi
}

rsudo mkdir -p "$REMOTE_INSTALL_DIR"
rsudo mkdir -p "$REMOTE_KEY_DIR"

SSH_SETUP1

scp "$KEY_FILE" "$REMOTE_HOST":/tmp/zfsmgr_allowed_keys.pub
scp "$BINARY_PATH" "$REMOTE_HOST":/tmp/zfsmgr_daemon.bin

ssh "$REMOTE_HOST" "$REMOTE_ENV bash -s" <<'SSH_SETUP2'
set -e
rsudo() {
  if [[ -n "${REMOTE_SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "$REMOTE_SUDO_PASSWORD" | sudo -S -p '' "$@"
  else
    sudo "$@"
  fi
}

rsudo_stream() {
  if [[ -n "${REMOTE_SUDO_PASSWORD:-}" ]]; then
    { printf '%s\n' "$REMOTE_SUDO_PASSWORD"; cat; } | sudo -S -p '' "$@"
  else
    sudo "$@"
  fi
}

rsudo mv /tmp/zfsmgr_daemon.bin "$REMOTE_INSTALL_DIR/zfsmgr_daemon"
rsudo chmod 755 "$REMOTE_INSTALL_DIR/zfsmgr_daemon"
rsudo mv /tmp/zfsmgr_allowed_keys.pub "$REMOTE_KEY_DIR/allowed_keys"

if [[ $OSTYPE == linux-gnu* ]]; then
  rsudo_stream tee /etc/systemd/system/zfsmgr-daemon.service >/dev/null <<SERVICE
[Unit]
Description=ZFSMgr daemon
After=network.target

[Service]
ExecStart=${REMOTE_INSTALL_DIR}/zfsmgr_daemon --port ${PORT}
Restart=always

[Install]
WantedBy=multi-user.target
SERVICE
  rsudo chown root:root /etc/systemd/system/zfsmgr-daemon.service
  rsudo chmod 644 /etc/systemd/system/zfsmgr-daemon.service
  rsudo systemctl enable --now zfsmgr-daemon.service
  rsudo ufw allow ${PORT}/tcp || true
  rsudo firewall-cmd --add-port=${PORT}/tcp --permanent && rsudo firewall-cmd --reload || true
elif [[ $OSTYPE == darwin* ]]; then
  rsudo_stream tee /Library/LaunchDaemons/com.zfsmgr.daemon.plist >/dev/null <<LABEL
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
  rsudo chown root:wheel /Library/LaunchDaemons/com.zfsmgr.daemon.plist
  rsudo chmod 644 /Library/LaunchDaemons/com.zfsmgr.daemon.plist
  rsudo launchctl bootstrap system /Library/LaunchDaemons/com.zfsmgr.daemon.plist || true
  rsudo pfctl -f /etc/pf.conf
elif [[ $OSTYPE == *freebsd* ]]; then
  rsudo service zfsmgr_daemon stop || true
  rsudo cp "$REMOTE_INSTALL_DIR/zfsmgr_daemon" /usr/local/bin/
  rsudo_stream tee /usr/local/etc/rc.d/zfsmgr_daemon >/dev/null <<RC
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
  rsudo chmod 755 /usr/local/etc/rc.d/zfsmgr_daemon
  rsudo sysrc zfsmgr_daemon_enable=YES
  rsudo service zfsmgr_daemon start
  rsudo pfctl -f /etc/pf.conf
else
  printf "Instalación manual requerida en Windows\n" >&2
fi
SSH_SETUP2
