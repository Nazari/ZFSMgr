#/bin/bash
set -euo pipefail

KEY_DIR="${HOME}/.zfsmgr"
mkdir -p "$KEY_DIR"

if [[ -f "$KEY_DIR/zfsmgr_daemon_id" ]]; then
  echo "Key already exists at $KEY_DIR/zfsmgr_daemon_id"
  exit 1
fi

ssh-keygen -t ed25519 -f "$KEY_DIR/zfsmgr_daemon_id" -N "" -q
chmod 600 "$KEY_DIR/zfsmgr_daemon_id"
chmod 644 "$KEY_DIR/zfsmgr_daemon_id.pub"
echo "Keys written to $KEY_DIR"
