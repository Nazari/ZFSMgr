#!/usr/bin/env python3
"""ZFSMgr GUI.

Aplicacion grafica para gestionar pools ZFS locales/remotos:
- Conexiones SSH (Linux/macOS, y tambien Windows con OpenSSH)
- Conexiones PowerShell Remoting (Windows via WSMan)
- Conexion local con py-libzfs
"""

from __future__ import annotations

import configparser
import base64
import getpass
import hashlib
import json
import os
import queue
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import traceback
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple


def _configure_tk_env_for_frozen_macos() -> None:
    """Set TCL/TK env vars for PyInstaller bundles on macOS before importing tkinter."""
    if sys.platform != "darwin" or not getattr(sys, "frozen", False):
        return
    meipass = getattr(sys, "_MEIPASS", "")
    if not meipass:
        return
    base = Path(meipass)
    # PyInstaller may place Tcl/Tk under different paths depending on Python build.
    candidates_tcl = [base / "tcl", base / "tcl8.6", base / "lib" / "tcl8.6"]
    candidates_tk = [base / "tk", base / "tk8.6", base / "lib" / "tk8.6"]
    for cand in candidates_tcl:
        if cand.exists():
            os.environ.setdefault("TCL_LIBRARY", str(cand))
            break
    for cand in candidates_tk:
        if cand.exists():
            os.environ.setdefault("TK_LIBRARY", str(cand))
            break


_configure_tk_env_for_frozen_macos()

import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, messagebox, ttk


def _add_local_venv_sitepackages() -> None:
    base_dir = Path(__file__).resolve().parent
    venv_lib = base_dir / ".venv" / "lib"
    if not venv_lib.exists():
        return
    for candidate in sorted(venv_lib.glob("python*/site-packages")):
        path = str(candidate)
        if path not in sys.path:
            sys.path.insert(0, path)


_add_local_venv_sitepackages()

from cryptography.fernet import Fernet, InvalidToken
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

try:
    import libzfs  # py-libzfs
except Exception:  # pragma: no cover - entorno sin libzfs
    libzfs = None

try:
    import paramiko
except Exception:  # pragma: no cover - entorno sin paramiko
    paramiko = None

try:
    from pypsrp.client import Client as PSRPClient
except Exception:  # pragma: no cover - entorno sin pypsrp
    PSRPClient = None


APP_DIR = Path(__file__).resolve().parent


def _resource_dir() -> Path:
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", "")
        if meipass:
            return Path(meipass)
        return Path(sys.executable).resolve().parent
    return APP_DIR


def _config_dir() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", str(Path.home() / "AppData" / "Roaming")))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    cfg = base / "ZFSMgr"
    cfg.mkdir(parents=True, exist_ok=True)
    return cfg


RESOURCE_DIR = _resource_dir()
CONFIG_DIR = _config_dir()
CONNECTIONS_FILE = CONFIG_DIR / "connections.ini"
LEGACY_CONNECTIONS_FILE = CONFIG_DIR / "connections.json"
LEGACY_CONNECTIONS_FALLBACK = APP_DIR / "connections.json"
LEGACY_INI_FALLBACK = APP_DIR / "connections.ini"
INSTANCE_LOCK_FILE = CONFIG_DIR / "app.lock"
APP_LOG_FILE = CONFIG_DIR / "application.log"
SSH_EXEC_LOG_FILE = CONFIG_DIR / "ssh_execution.log"
LOG_ROTATE_MAX_BYTES = 5 * 1024 * 1024
LOG_ROTATE_BACKUP_COUNT = 5
COMMAND_TIMEOUT_SECONDS = 20
# PSRP suele tardar bastante mas en abrir sesion/ejecutar.
PSRP_TIMEOUT_SECONDS = 60
# Un refresco completo por conexion puede ejecutar varias comprobaciones remotas
# secuenciales; 45s provoca timeouts falsos en PSRP.
REFRESH_TIMEOUT_SECONDS = 120
WINDOWS_CONN_BLOCK_MSG = "Conexiones Windows desactivadas temporalmente hasta OpenZFS 2.4"

LANG_OPTIONS = {
    "es": "Español",
    "en": "English",
    "zh": "中文",
    "de": "Deutsch",
    "fr": "Français",
    "it": "Italiano",
    "ru": "Русский",
    "pt-BR": "Português (Brasil)",
    "pt": "Português",
    "eu": "Euskara",
    "ca": "Català",
}
LANG_LABEL_TO_CODE = {v: k for k, v in LANG_OPTIONS.items()}


def _create_zfsmgr_icon(size: int = 64) -> tk.PhotoImage:
    img = tk.PhotoImage(width=size, height=size)
    c_bg = "#0f1c2e"
    c_panel = "#1f3554"
    c_stage = "#2a4a6f"
    c_gold = "#e8c76a"
    c_white = "#f3f6fb"
    c_linux = "#f6b73c"
    c_macos = "#a8d4ff"
    c_zfs = "#57c4c0"

    img.put(c_bg, to=(0, 0, size, size))
    pad = max(2, size // 16)
    img.put(c_panel, to=(pad, pad, size - pad, size - pad))
    img.put(c_stage, to=(pad * 2, size - (pad * 4), size - (pad * 2), size - (pad * 2)))

    # Emblema ZFS
    z_left = size // 6
    z_top = size // 6
    z_right = size - size // 6
    z_bot = size // 2
    img.put(c_zfs, to=(z_left, z_top, z_right, z_top + 4))
    img.put(c_zfs, to=(z_left, z_bot - 4, z_right, z_bot))
    for i in range(0, max(1, (z_right - z_left) // 4)):
        x = z_right - (i * 2) - 4
        y = z_top + (i * 2) + 4
        if y < z_bot - 2 and x > z_left + 2:
            img.put(c_zfs, to=(x, y, x + 2, y + 2))

    # Director de orquesta (estilizado)
    cx = size // 2
    head = max(3, size // 14)
    img.put(c_white, to=(cx - head, size // 2 + 2, cx + head, size // 2 + 2 + head))
    img.put(c_white, to=(cx - 1, size // 2 + 2 + head, cx + 1, size - (pad * 5)))
    img.put(c_white, to=(cx - size // 8, size - (pad * 7), cx + size // 8, size - (pad * 7) + 2))
    # Batuta
    bx = cx + size // 10
    by = size // 2 + 2 + head
    for i in range(0, size // 5):
        img.put(c_gold, to=(bx + i, by - i, bx + i + 1, by - i + 1))

    # Linux (izquierda) y macOS (derecha) minimalistas
    lx = size // 5
    ly = size - (pad * 6)
    img.put(c_linux, to=(lx - 3, ly - 3, lx + 3, ly + 3))
    rx = size - size // 5
    ry = size - (pad * 6)
    img.put(c_macos, to=(rx - 3, ry - 3, rx + 3, ry + 3))

    # Marco fino
    img.put(c_gold, to=(0, 0, size, 1))
    img.put(c_gold, to=(0, size - 1, size, size))
    img.put(c_gold, to=(0, 0, 1, size))
    img.put(c_gold, to=(size - 1, 0, size, size))
    return img


def _apply_window_icon(win: tk.Misc) -> None:
    try:
        icon64 = _create_zfsmgr_icon(64)
        icon32 = icon64.subsample(2, 2)
        icon16 = icon64.subsample(4, 4)
        win.iconphoto(True, icon64, icon32, icon16)  # type: ignore[attr-defined]
        setattr(win, "_zfsmgr_icons", (icon64, icon32, icon16))
    except Exception:
        pass
CURRENT_LANG = "es"
I18N: Dict[str, Dict[str, str]] = {}
SSH_LOG_HOOK: Optional[Callable[[str], None]] = None
SSH_BUSY_HOOK: Optional[Callable[[int], None]] = None
_SSH_SESSION_LOCK = threading.Lock()
_SSH_SESSION_CACHE: Dict[str, Any] = {}
_PSRP_SESSION_LOCK = threading.Lock()
_PSRP_SESSION_CACHE: Dict[str, Any] = {}

UI_BG = "#f3f6f8"
UI_PANEL_BG = "#ffffff"
UI_ACCENT = "#1f5f7a"
UI_TEXT = "#1e2a33"
UI_MUTED = "#6b7b88"
UI_BORDER = "#c9d5de"
UI_SELECTION = "#d7ecf7"
UI_ACTION_MOUNT = "#1f7a3f"
UI_ACTION_UMOUNT = "#b54747"
UI_WARNING = "#b06a00"


def _session_profile_key(profile: "ConnectionProfile") -> str:
    return "|".join(
        [
            profile.conn_type,
            profile.id,
            profile.host,
            str(profile.port or 0),
            profile.username or "",
            profile.key_path or "",
            "1" if profile.use_ssl else "0",
            profile.auth or "",
            "1" if profile.use_sudo else "0",
            # Incluye password para invalidar cache si cambia en memoria.
            profile.password or "",
        ]
    )


def _ssh_mux_path(profile: "ConnectionProfile") -> str:
    mux_dir = CONFIG_DIR / "ssh_mux"
    mux_dir.mkdir(parents=True, exist_ok=True)
    base = f"{profile.username or ''}@{profile.host}:{profile.port or 22}"
    tag = hashlib.sha1(base.encode("utf-8")).hexdigest()[:12]
    return str(mux_dir / f"mux_{tag}")


def _resolve_ssh_private_key_path(key_path: str) -> str:
    path = (key_path or "").strip()
    if not path:
        return ""
    expanded = os.path.expanduser(path)
    # Si el usuario selecciona .pub, intentamos usar la privada homonima.
    if expanded.endswith(".pub"):
        candidate = expanded[:-4]
        if os.path.exists(candidate):
            return candidate
    return expanded


def _ssh_common_parts(profile: "ConnectionProfile", include_key: bool = True) -> List[str]:
    parts: List[str] = [
        "ssh",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=12",
        "-o",
        "ControlMaster=auto",
        "-o",
        "ControlPersist=300",
        "-o",
        f"ControlPath={_ssh_mux_path(profile)}",
    ]
    if profile.port and profile.port != 22:
        parts.extend(["-p", str(profile.port)])
    resolved_key = _resolve_ssh_private_key_path(profile.key_path)
    if include_key and resolved_key:
        parts.extend(["-i", shlex.quote(resolved_key)])
    return parts


def _ssh_outer_exec_command(
    profile: "ConnectionProfile",
    remote_command: str,
    *,
    include_key: bool = True,
    allow_password_auth: bool = False,
) -> Optional[str]:
    if profile.conn_type == "LOCAL":
        return remote_command
    if profile.conn_type != "SSH":
        return None
    target = f"{profile.username}@{profile.host}" if profile.username else profile.host
    if allow_password_auth and profile.password:
        ssh_parts: List[str] = _ssh_common_parts(profile, include_key=include_key)
        # Permitir password sin prompt interactivo, manteniendo pubkey si existe.
        ssh_parts.extend(
            [
                "-o",
                "BatchMode=no",
                "-o",
                "PreferredAuthentications=publickey,password,keyboard-interactive",
                "-o",
                "NumberOfPasswordPrompts=1",
            ]
        )
        ssh_parts.append(shlex.quote(target))
        ssh_parts.append(shlex.quote(remote_command))
        ssh_cmd = " ".join(ssh_parts)
        askpass_line = "printf '%s\\n' " + shlex.quote(profile.password)
        return (
            "ask=$(mktemp); "
            "trap 'rm -f \"$ask\"' EXIT; "
            "{ printf '%s\\n' '#!/bin/sh'; "
            f"printf '%s\\n' {shlex.quote(askpass_line)}; "
            "} >\"$ask\"; "
            "chmod 700 \"$ask\"; "
            "DISPLAY=:0 SSH_ASKPASS=\"$ask\" SSH_ASKPASS_REQUIRE=force "
            f"setsid -w {ssh_cmd} </dev/null"
        )
    ssh_parts = _ssh_common_parts(profile, include_key=include_key)
    ssh_parts.append(shlex.quote(target))
    ssh_parts.append(shlex.quote(remote_command))
    return " ".join(ssh_parts)


def _close_all_remote_sessions() -> None:
    with _SSH_SESSION_LOCK:
        clients = list(_SSH_SESSION_CACHE.values())
        _SSH_SESSION_CACHE.clear()
    for client in clients:
        try:
            client.close()
        except Exception:
            pass
    with _PSRP_SESSION_LOCK:
        _PSRP_SESSION_CACHE.clear()

# Whitelist explicita de propiedades editables por tipo de dataset.
# Nota: ademas se permite cualquier user-property (formato "namespace:prop").
EDITABLE_DATASET_PROPS_COMMON = {
    "atime",
    "relatime",
    "readonly",
    "compression",
    "checksum",
    "sync",
    "logbias",
    "primarycache",
    "secondarycache",
    "dedup",
    "copies",
    "acltype",
    "aclinherit",
    "xattr",
    "normalization",
    "casesensitivity",
    "utf8only",
    "keylocation",
    "comment",
}
EDITABLE_DATASET_PROPS_FILESYSTEM = EDITABLE_DATASET_PROPS_COMMON | {
    "mountpoint",
    "canmount",
    "recordsize",
    "quota",
    "reservation",
    "refquota",
    "refreservation",
    "snapdir",
    "exec",
    "setuid",
    "devices",
}
EDITABLE_DATASET_PROPS_VOLUME = EDITABLE_DATASET_PROPS_COMMON | {
    "volsize",
    "volblocksize",
    "reservation",
    "refreservation",
    "snapdev",
    "volmode",
}
EDITABLE_DATASET_PROPS_SNAPSHOT: set[str] = set()


def _load_i18n() -> None:
    global I18N
    locales_dir = RESOURCE_DIR / "locales"
    if not locales_dir.exists():
        locales_dir = APP_DIR / "locales"
    merged: Dict[str, Dict[str, str]] = {}
    for code in LANG_OPTIONS.keys():
        path = locales_dir / f"{code}.json"
        if not path.exists():
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        for key, value in data.items():
            if not isinstance(value, str):
                continue
            merged.setdefault(key, {})[code] = value
    I18N = merged


_load_i18n()


def tr(key: str) -> str:
    msg = I18N.get(key, {})
    return msg.get(CURRENT_LANG) or msg.get("es") or key


def trf(key: str, **kwargs: Any) -> str:
    return tr(key).format(**kwargs)


def ssh_trace(message: str) -> None:
    hook = SSH_LOG_HOOK
    if hook:
        try:
            hook(message)
        except Exception:
            pass


def ssh_busy(delta: int) -> None:
    hook = SSH_BUSY_HOOK
    if hook:
        try:
            hook(delta)
        except Exception:
            pass


def load_preferred_language_from_ini(path: Path) -> Optional[str]:
    if not path.exists():
        return None
    cfg = configparser.ConfigParser()
    try:
        cfg.read(path, encoding="utf-8")
    except Exception:
        return None
    code = cfg.get("meta", "language", fallback="").strip().lower()
    if code in LANG_OPTIONS:
        return code
    return None


def save_preferred_language_to_ini(path: Path, language_code: str) -> None:
    if language_code not in LANG_OPTIONS:
        return
    # Evita crear un INI nuevo antes de migrar legados; solo actualiza si ya existe.
    if not path.exists():
        return
    cfg = configparser.ConfigParser()
    try:
        cfg.read(path, encoding="utf-8")
    except Exception:
        return
    if not cfg.has_section("meta"):
        cfg["meta"] = {}
    cfg["meta"]["version"] = cfg.get("meta", "version", fallback="1")
    cfg["meta"]["language"] = language_code
    try:
        with path.open("w", encoding="utf-8") as fh:
            cfg.write(fh)
    except Exception:
        pass


class PasswordCryptoError(RuntimeError):
    pass


class SingleInstanceLock:
    """Evita ejecutar mas de una instancia simultanea de la aplicacion."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.fd: Optional[Any] = None

    def acquire(self) -> bool:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = open(self.path, "a+")
        try:
            if os.name == "nt":
                import msvcrt

                self.fd.seek(0)
                # Bloquea 1 byte de forma no bloqueante.
                msvcrt.locking(self.fd.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.fd.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.fd.seek(0)
            self.fd.truncate()
            self.fd.write(str(os.getpid()))
            self.fd.flush()
            return True
        except Exception:
            try:
                self.fd.close()
            except Exception:
                pass
            self.fd = None
            return False

    def release(self) -> None:
        if not self.fd:
            return
        try:
            if os.name == "nt":
                import msvcrt

                self.fd.seek(0)
                msvcrt.locking(self.fd.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.fd.fileno(), fcntl.LOCK_UN)
        except Exception:
            pass
        try:
            self.fd.close()
        except Exception:
            pass
        self.fd = None


class ToolTip:
    def __init__(self, widget: tk.Widget, text: str) -> None:
        self.widget = widget
        self.text = text
        self.tipwindow: Optional[tk.Toplevel] = None
        widget.bind("<Enter>", self._show)
        widget.bind("<Leave>", self._hide)

    def _show(self, _event: Any = None) -> None:
        if self.tipwindow or not self.text:
            return
        x = self.widget.winfo_rootx() + 12
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        lbl = ttk.Label(tw, text=self.text, relief="solid", borderwidth=1, padding=(6, 3))
        lbl.pack()
        self.tipwindow = tw

    def _hide(self, _event: Any = None) -> None:
        if self.tipwindow is not None:
            self.tipwindow.destroy()
            self.tipwindow = None


class SecretCipher:
    PREFIX = "encv1$"

    def __init__(self, master_password: str) -> None:
        self.master_password = master_password.encode("utf-8")

    def _derive_key(self, salt: bytes) -> bytes:
        kdf = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=390000,
        )
        return base64.urlsafe_b64encode(kdf.derive(self.master_password))

    def encrypt(self, plain_text: str) -> str:
        if not plain_text:
            return ""
        salt = os.urandom(16)
        key = self._derive_key(salt)
        token = Fernet(key).encrypt(plain_text.encode("utf-8"))
        salt_b64 = base64.urlsafe_b64encode(salt).decode("ascii")
        return f"{self.PREFIX}{salt_b64}${token.decode('ascii')}"

    def decrypt(self, cipher_text: str) -> str:
        if not cipher_text:
            return ""
        if not cipher_text.startswith(self.PREFIX):
            # Compatibilidad con archivos antiguos en texto plano.
            return cipher_text
        parts = cipher_text.split("$", 2)
        if len(parts) != 3:
            raise PasswordCryptoError(tr("crypto_invalid_format"))
        _prefix, salt_b64, token = parts
        try:
            salt = base64.urlsafe_b64decode(salt_b64.encode("ascii"))
            key = self._derive_key(salt)
            return Fernet(key).decrypt(token.encode("ascii")).decode("utf-8")
        except (InvalidToken, ValueError) as exc:
            raise PasswordCryptoError(tr("crypto_invalid_master_password")) from exc


@dataclass
class ConnectionProfile:
    id: str
    name: str
    conn_type: str  # LOCAL | SSH | PSRP
    os_type: str = "Linux"  # Windows | Linux | MacOS
    transport: str = "SSH"  # SSH | PSRP
    host: str = ""
    port: int = 22
    username: str = ""
    password: str = ""
    key_path: str = ""
    use_ssl: bool = False
    auth: str = "ntlm"
    use_sudo: bool = True


@dataclass
class ConnectionState:
    ok: bool = False
    message: str = ""
    sudo_ok: Optional[bool] = None
    zfs_version: Optional[str] = None
    imported: List[Dict[str, str]] = None
    importable: List[Dict[str, str]] = None
    imported_devices: Dict[str, List[Dict[str, Any]]] = None
    imported_status: Dict[str, str] = None
    mounted_datasets: List[Dict[str, str]] = None
    dataset_properties: Dict[str, List[Dict[str, str]]] = None

    def __post_init__(self) -> None:
        if self.imported is None:
            self.imported = []
        if self.importable is None:
            self.importable = []
        if self.imported_devices is None:
            self.imported_devices = {}
        if self.imported_status is None:
            self.imported_status = {}
        if self.mounted_datasets is None:
            self.mounted_datasets = []
        if self.dataset_properties is None:
            self.dataset_properties = {}


class ConnectionStore:
    def __init__(self, path: Path, master_password: str) -> None:
        self.path = path
        self.cipher = SecretCipher(master_password)
        self.connections: List[ConnectionProfile] = []
        self.load()

    def load(self) -> None:
        if self.path.suffix == ".ini" and self.path.exists():
            self._load_ini()
            return
        if self.path.suffix == ".ini" and LEGACY_INI_FALLBACK.exists():
            # Migracion automatica desde el ini historico en el directorio de la app.
            self._load_ini(LEGACY_INI_FALLBACK)
            self.save()
            return
        if self.path.suffix == ".ini" and (LEGACY_CONNECTIONS_FILE.exists() or LEGACY_CONNECTIONS_FALLBACK.exists()):
            self._load_legacy_json()
            self.save()
            return
        if not self.path.exists():
            self.connections = []
            return
        raw = json.loads(self.path.read_text(encoding="utf-8"))
        self.connections = [self._profile_from_mapping(item) for item in raw]

    def save(self) -> None:
        if self.path.suffix != ".ini":
            data = [self._profile_to_mapping(c) for c in self.connections]
            self.path.write_text(json.dumps(data, indent=2), encoding="utf-8")
            return

        cfg = configparser.ConfigParser()
        cfg["meta"] = {"version": "1", "language": CURRENT_LANG if CURRENT_LANG in LANG_OPTIONS else "es"}
        for profile in self.connections:
            sec = f"connection:{profile.id}"
            cfg[sec] = self._profile_to_mapping(profile)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("w", encoding="utf-8") as fh:
            cfg.write(fh)

    def upsert(self, profile: ConnectionProfile) -> None:
        for idx, existing in enumerate(self.connections):
            if existing.id == profile.id:
                self.connections[idx] = profile
                self.save()
                return
        self.connections.append(profile)
        self.save()

    def delete(self, conn_id: str) -> None:
        self.connections = [c for c in self.connections if c.id != conn_id]
        self.save()

    def get(self, conn_id: str) -> Optional[ConnectionProfile]:
        for c in self.connections:
            if c.id == conn_id:
                return c
        return None

    def _load_ini(self, ini_path: Optional[Path] = None) -> None:
        path = ini_path or self.path
        cfg = configparser.ConfigParser()
        cfg.read(path, encoding="utf-8")
        conns: List[ConnectionProfile] = []
        for section in cfg.sections():
            if not section.startswith("connection:"):
                continue
            raw = dict(cfg.items(section))
            raw.setdefault("id", section.split(":", 1)[1])
            conns.append(self._profile_from_mapping(raw))
        self.connections = conns

    def _load_legacy_json(self) -> None:
        try:
            legacy_path = LEGACY_CONNECTIONS_FILE if LEGACY_CONNECTIONS_FILE.exists() else LEGACY_CONNECTIONS_FALLBACK
            raw = json.loads(legacy_path.read_text(encoding="utf-8"))
            self.connections = [self._profile_from_mapping(item) for item in raw]
        except Exception:
            self.connections = []

    def _decode_password(self, raw_password: str) -> str:
        try:
            return self.cipher.decrypt(raw_password)
        except PasswordCryptoError as exc:
            raise ValueError(str(exc)) from exc

    def _decode_username(self, raw_username: str) -> str:
        try:
            return self.cipher.decrypt(raw_username)
        except PasswordCryptoError:
            # El usuario ya no se cifra en el INI; si falla descifrado,
            # tratamos el valor como texto plano.
            return raw_username

    def _profile_from_mapping(self, data: Dict[str, Any]) -> ConnectionProfile:
        conn_type = str(data.get("conn_type", "SSH")).upper()
        os_type = str(data.get("os_type", "Linux"))
        transport = str(data.get("transport", conn_type if conn_type in {"SSH", "PSRP"} else "SSH")).upper()
        use_ssl = str(data.get("use_ssl", "False")).lower() in {"1", "true", "yes", "on"}
        port = int(data.get("port", 22) or 22)
        # Autocorreccion PSRP: 5985<->HTTP, 5986<->HTTPS.
        if conn_type == "PSRP":
            if use_ssl and port == 5985:
                port = 5986
            elif not use_ssl and port == 5986:
                port = 5985
        return ConnectionProfile(
            id=str(data.get("id", str(uuid.uuid4()))),
            name=str(data.get("name", "")),
            conn_type=conn_type,
            os_type=os_type if os_type in {"Windows", "Linux", "MacOS"} else "Linux",
            transport=transport if transport in {"SSH", "PSRP"} else "SSH",
            host=str(data.get("host", "")),
            port=port,
            username=self._decode_username(str(data.get("username", ""))),
            password=self._decode_password(str(data.get("password", ""))),
            key_path=str(data.get("key_path", "")),
            use_ssl=use_ssl,
            auth=str(data.get("auth", "ntlm")) or "ntlm",
            use_sudo=str(data.get("use_sudo", "True")).lower() in {"1", "true", "yes", "on"},
        )

    def _profile_to_mapping(self, profile: ConnectionProfile) -> Dict[str, str]:
        return {
            "id": profile.id,
            "name": profile.name,
            "conn_type": profile.conn_type,
            "os_type": profile.os_type,
            "transport": profile.transport,
            "host": profile.host,
            "port": str(profile.port),
            "username": profile.username,
            "password": self.cipher.encrypt(profile.password),
            "key_path": profile.key_path,
            "use_ssl": str(bool(profile.use_ssl)),
            "auth": profile.auth,
            "use_sudo": str(bool(profile.use_sudo)),
        }


class ExecutorError(RuntimeError):
    pass


def parse_openzfs_version(text: str) -> Optional[Tuple[int, int, int]]:
    if not text:
        return None
    patterns = [
        r"\bzfs(?:-kmod)?[-\s]+(\d+)\.(\d+)(?:\.(\d+))?\b",
        r"\bopenzfs(?:[-\s]+version)?[:\s]+(\d+)\.(\d+)(?:\.(\d+))?\b",
        r"\b(\d+)\.(\d+)(?:\.(\d+))?\b",
    ]
    lower = text.lower()
    for pat in patterns:
        m = re.search(pat, lower)
        if not m:
            continue
        major = int(m.group(1))
        minor = int(m.group(2))
        patch = int(m.group(3) or 0)
        # Evita capturar versiones ajenas (kernel, powershell, etc.).
        if major > 10:
            continue
        return (major, minor, patch)
    return None


def format_openzfs_version(version: Optional[Tuple[int, int, int]]) -> str:
    if not version:
        return "-"
    return f"{version[0]}.{version[1]}.{version[2]}"


def build_send_flag_candidates(version: Optional[Tuple[int, int, int]], recursive: bool) -> List[str]:
    # Para OpenZFS 2.3/2.4 preferimos -wLec (+R si procede).
    # Algunos destinos (p.ej. zfswin) fallan recibiendo streams raw (-w),
    # asi que anadimos fallback no-raw al final.
    if version is None or (version and (version[0], version[1]) >= (2, 3)):
        candidates = [
            "wLecR" if recursive else "wLec",
            "wLeR" if recursive else "wLe",
            "wR" if recursive else "w",
            "LecR" if recursive else "Lec",
            "LeR" if recursive else "Le",
            "R" if recursive else "",
        ]
    elif version and (version[0], version[1]) >= (2, 1):
        candidates = [
            "wLeR" if recursive else "wLe",
            "wR" if recursive else "w",
            "LeR" if recursive else "Le",
            "R" if recursive else "",
        ]
    else:
        candidates = ["wR" if recursive else "w", "R" if recursive else ""]
    deduped: List[str] = []
    for item in candidates:
        if item not in deduped:
            deduped.append(item)
    return deduped


def zpool_import_probe_commands(version: Optional[Tuple[int, int, int]]) -> List[str]:
    # Mantiene compatibilidad: en 2.4 priorizamos -s, en 2.3 probamos "import" primero.
    if version and (version[0], version[1]) >= (2, 4):
        ordered = ["import -s", "import", "import -H -o name"]
    elif version and (version[0], version[1]) >= (2, 3):
        ordered = ["import", "import -s", "import -H -o name"]
    else:
        ordered = ["import", "import -H -o name", "import -s"]
    return ordered


def build_zfs_binary_cmd(binary: str, args: str) -> str:
    # Orden de resolucion:
    # 1) PATH actual
    # 2) OpenZFS en macOS/Homebrew/manual: /usr/local/zfs/bin
    # 3) Ubicacion tradicional en sistemas Unix: /sbin
    return (
        f"(command -v {binary} >/dev/null 2>&1 && {binary} {args}) || "
        f"([ -x /usr/local/zfs/bin/{binary} ] && /usr/local/zfs/bin/{binary} {args}) || "
        f"([ -x /sbin/{binary} ] && /sbin/{binary} {args})"
    )


def build_remote_zfs_cmd(os_type: str, binary: str, args: str) -> str:
    # En Windows remoto via SSH/PowerShell asumimos binarios en PATH.
    if os_type == "Windows":
        return f"{binary} {args}"
    return build_zfs_binary_cmd(binary, args)


def run_with_timeout(fn: Callable[[], Any], timeout_seconds: int, error_message: str) -> Any:
    result_q: queue.Queue[Tuple[bool, Any]] = queue.Queue(maxsize=1)

    def _worker() -> None:
        try:
            result_q.put((True, fn()))
        except Exception as exc:
            result_q.put((False, exc))

    # Daemon para no bloquear la salida del proceso si el worker queda colgado.
    t = threading.Thread(target=_worker, daemon=True)
    t.start()
    try:
        ok, payload = result_q.get(timeout=timeout_seconds)
    except queue.Empty as exc:
        raise ExecutorError(f"{error_message} (timeout {timeout_seconds}s)") from exc
    if ok:
        return payload
    raise payload


def parse_imported_pool_rows(text: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = re.split(r"\s+", line)
        if len(parts) < 6:
            continue
        rows.append(
            {
                "pool": parts[0],
                "size": parts[1],
                "used": parts[2],
                "free": parts[3],
                "compressratio": parts[4],
                "dedup": parts[5],
            }
        )
    return rows


def parse_zfs_mount_rows(text: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = re.split(r"\s+", line, maxsplit=1)
        if len(parts) < 2:
            continue
        ds = parts[0].strip()
        mp = parts[1].strip()
        if not ds or not mp:
            continue
        rows.append({"dataset": ds, "mountpoint": mp})
    return rows


def _format_with_max_four_digits(value: float, unit: str) -> str:
    if value >= 1000:
        txt = f"{value:.0f}"
    elif value >= 100:
        txt = f"{value:.1f}"
    elif value >= 10:
        txt = f"{value:.2f}"
    else:
        txt = f"{value:.3f}"
    txt = txt.rstrip("0").rstrip(".")
    digits = len(txt.replace(".", "").replace("-", ""))
    if digits > 4:
        txt = f"{value:.0f}"
    return f"{txt}{unit}"


def format_bytes_compact(raw_value: str) -> str:
    try:
        value = float(raw_value)
    except Exception:
        return raw_value
    if value < 0:
        return raw_value
    units = ["B", "KB", "MB", "GB", "TB", "PB", "EB"]
    idx = 0
    while value >= 1024 and idx < len(units) - 1:
        value /= 1024.0
        idx += 1
    return _format_with_max_four_digits(value, units[idx])


class BaseExecutor:
    def __init__(self, profile: ConnectionProfile) -> None:
        self.profile = profile
        self._zfs_version: Optional[Tuple[int, int, int]] = None
        self._zfs_version_checked = False

    def check_connection(self) -> Tuple[bool, str]:
        raise NotImplementedError

    def check_sudo(self) -> Optional[bool]:
        return None

    def list_imported_pools(self) -> List[Dict[str, str]]:
        raise NotImplementedError

    def list_importable_pools(self) -> List[Dict[str, str]]:
        raise NotImplementedError

    def list_datasets(self, pool: str) -> List[Dict[str, str]]:
        raise NotImplementedError

    def import_pool(self, pool: str, options: Dict[str, Any]) -> str:
        raise NotImplementedError

    def export_pool(self, pool: str) -> str:
        raise NotImplementedError

    def list_pool_devices(self, pool: str) -> List[Dict[str, Any]]:
        raise NotImplementedError

    def mount_dataset(self, dataset: str) -> str:
        raise NotImplementedError

    def unmount_dataset(self, dataset: str, recursive: bool = False) -> str:
        raise NotImplementedError

    def list_pool_properties(self, pool: str) -> List[Dict[str, str]]:
        raise NotImplementedError

    def pool_status_verbose(self, pool: str) -> str:
        raise NotImplementedError

    def list_mounted_datasets(self) -> List[Dict[str, str]]:
        raise NotImplementedError

    def create_dataset(self, dataset_path: str, options: Dict[str, Any]) -> str:
        raise NotImplementedError

    def list_dataset_properties(self, dataset: str) -> List[Dict[str, str]]:
        raise NotImplementedError

    def list_all_dataset_properties(self, pools: List[str]) -> Dict[str, List[Dict[str, str]]]:
        raise NotImplementedError

    def set_dataset_properties(self, dataset: str, properties: Dict[str, str]) -> str:
        raise NotImplementedError

    def inherit_dataset_properties(self, dataset: str, properties: List[str]) -> str:
        raise NotImplementedError

    def rename_dataset(self, dataset: str, new_name: str) -> str:
        raise NotImplementedError

    def destroy_dataset(self, dataset: str, recursive: bool = False) -> str:
        raise NotImplementedError

    def _detect_zfs_version(self) -> Optional[Tuple[int, int, int]]:
        return None

    def get_zfs_version(self) -> Optional[Tuple[int, int, int]]:
        if not self._zfs_version_checked:
            self._zfs_version_checked = True
            try:
                self._zfs_version = self._detect_zfs_version()
            except Exception:
                self._zfs_version = None
        return self._zfs_version


class LocalExecutor(BaseExecutor):
    def _zpool_cmd(self, args: str, timeout_seconds: Optional[int] = COMMAND_TIMEOUT_SECONDS) -> str:
        import subprocess

        cmd = build_zfs_binary_cmd("zpool", args)
        try:
            proc = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout_seconds)
        except subprocess.TimeoutExpired as exc:
            raise ExecutorError(f"Timeout ejecutando: {cmd}") from exc
        if proc.returncode != 0:
            raise ExecutorError(proc.stderr.strip() or proc.stdout.strip() or f"Fallo ejecutando: {cmd}")
        return proc.stdout

    def _zfs_cmd(self, args: str, timeout_seconds: Optional[int] = COMMAND_TIMEOUT_SECONDS) -> str:
        import subprocess

        cmd = build_zfs_binary_cmd("zfs", args)
        try:
            proc = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout_seconds)
        except subprocess.TimeoutExpired as exc:
            raise ExecutorError(f"Timeout ejecutando: {cmd}") from exc
        if proc.returncode != 0:
            raise ExecutorError(proc.stderr.strip() or proc.stdout.strip() or f"Fallo ejecutando: {cmd}")
        return proc.stdout

    def check_connection(self) -> Tuple[bool, str]:
        if libzfs is not None:
            try:
                with libzfs.ZFS() as zfs:
                    _ = [p.name for p in zfs.pools]
                return True, tr("local_connection_ok_libzfs")
            except Exception as exc:
                return False, trf("local_connection_error_libzfs", error=exc)
        try:
            _ = self._zpool_cmd("list -H -o name")
            return True, tr("local_connection_ok_cli")
        except Exception as exc:
            return False, trf("local_connection_error_no_libzfs", error=exc)

    def _detect_zfs_version(self) -> Optional[Tuple[int, int, int]]:
        try:
            out = self._zfs_cmd("version", timeout_seconds=10)
            return parse_openzfs_version(out)
        except Exception:
            return None

    def check_sudo(self) -> Optional[bool]:
        import subprocess

        try:
            proc = subprocess.run("sudo -n true", shell=True, capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            return False
        return proc.returncode == 0

    def list_imported_pools(self) -> List[Dict[str, str]]:
        output = self._zpool_cmd("list -H -p -o name,size,alloc,free,cap,dedupratio")
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = re.split(r"\s+", line)
            if len(parts) < 6:
                continue
            name, size, used, free = parts[0], parts[1], parts[2], parts[3]
            dedup = parts[5] if len(parts) > 5 else "-"
            try:
                cr = self._zfs_cmd(f"get -H -o value compressratio {shlex.quote(name)}").strip() or "-"
            except Exception:
                cr = "-"
            rows.append(
                {
                    "pool": name,
                    "size": format_bytes_compact(size),
                    "used": format_bytes_compact(used),
                    "free": format_bytes_compact(free),
                    "compressratio": cr,
                    "dedup": dedup,
                }
            )
        return rows

    def list_importable_pools(self) -> List[Dict[str, str]]:
        ver = self.get_zfs_version()
        for args in zpool_import_probe_commands(ver):
            try:
                out = self._zpool_cmd(args)
                if args.endswith("-H -o name"):
                    rows = parse_importable_name_lines(out)
                else:
                    rows = parse_zpool_import_output(out)
                if rows:
                    return rows
            except Exception:
                continue
        return []

    def list_datasets(self, pool: str) -> List[Dict[str, str]]:
        output = self._zfs_cmd(
            f"list -H -p -t filesystem,volume,snapshot -o name,refer,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r {shlex.quote(pool)}"
        )
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = re.split(r"\s+", line)
            if len(parts) < 10:
                continue
            rows.append(
                {
                    "name": parts[0],
                    "size": format_bytes_compact(parts[1]),
                    "used": format_bytes_compact(parts[2]),
                    "compressratio": parts[3],
                    "encryption": parts[4],
                    "creation": parts[5],
                    "referenced": format_bytes_compact(parts[6]),
                    "mounted": parts[7],
                    "mountpoint": parts[8],
                    "canmount": parts[9],
                }
            )
        return rows

    def import_pool(self, pool: str, options: Dict[str, Any]) -> str:
        cmd = build_zpool_import_cmd(pool, options)
        return self._zpool_cmd(cmd)

    def export_pool(self, pool: str) -> str:
        return self._zpool_cmd(f"export {shlex.quote(pool)}")

    def list_pool_devices(self, pool: str) -> List[Dict[str, Any]]:
        out = self._zpool_cmd(f"status -P {shlex.quote(pool)}")
        return parse_zpool_status_devices_tree(out, pool)

    def mount_dataset(self, dataset: str) -> str:
        return self._zfs_cmd(f"mount {shlex.quote(dataset)}")

    def unmount_dataset(self, dataset: str, recursive: bool = False) -> str:
        quoted = shlex.quote(dataset)
        if recursive:
            import subprocess

            cmd = (
                "set -e; "
                f"DATASET={quoted}; "
                "zfs list -H -o name -t filesystem,volume -r \"$DATASET\" "
                "| awk 'NF{d=gsub(/\\//,\"/\",$1); printf \"%08d\\t%s\\n\", d, $1}' "
                "| sort -r | cut -f2- "
                "| while IFS= read -r ds; do [ -n \"$ds\" ] && (zfs umount -f \"$ds\" >/dev/null 2>&1 || zfs unmount -f \"$ds\" >/dev/null 2>&1) || true; done; "
                "LEFT=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds || index($1, ds\"/\")==1 {print $1}' | head -n1)\"; "
                "[ -z \"$LEFT\" ] || { echo \"cannot unmount dataset tree: $DATASET ($LEFT)\" >&2; exit 1; }"
            )
            proc = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=COMMAND_TIMEOUT_SECONDS)
            if proc.returncode != 0:
                raise ExecutorError(proc.stderr.strip() or proc.stdout.strip() or "recursive unmount failed")
            return proc.stdout or "recursive unmount OK"
        try:
            return self._zfs_cmd(f"umount {quoted}")
        except Exception:
            try:
                return self._zfs_cmd(f"unmount {quoted}")
            except Exception as exc2:
                try:
                    return self._zfs_cmd(f"umount -f {quoted}")
                except Exception:
                    try:
                        return self._zfs_cmd(f"unmount -f {quoted}")
                    except Exception as exc4:
                        raise ExecutorError(str(exc4))

    def list_pool_properties(self, pool: str) -> List[Dict[str, str]]:
        output = self._zpool_cmd(f"get -H all {shlex.quote(pool)}")
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = line.split(None, 3)
            if len(parts) < 4:
                continue
            rows.append({"property": parts[1], "value": parts[2], "source": parts[3]})
        return rows

    def pool_status_verbose(self, pool: str) -> str:
        return self._zpool_cmd(f"status -v {shlex.quote(pool)}")

    def list_mounted_datasets(self) -> List[Dict[str, str]]:
        out = self._zfs_cmd("mount")
        return parse_zfs_mount_rows(out)

    def create_dataset(self, dataset_path: str, options: Dict[str, Any]) -> str:
        cmd = build_zfs_create_cmd(dataset_path, options)
        return self._zfs_cmd(cmd)

    def list_dataset_properties(self, dataset: str) -> List[Dict[str, str]]:
        quoted = shlex.quote(dataset)
        try:
            out = self._zfs_cmd(f"get -H -o property,value,source,readonly all {quoted}")
        except Exception:
            out = self._zfs_cmd(f"get -H -o property,value,source all {quoted}")
        return parse_zfs_get_properties(out)

    def list_all_dataset_properties(self, pools: List[str]) -> Dict[str, List[Dict[str, str]]]:
        uniq: List[str] = []
        seen: set[str] = set()
        for pool in pools:
            p = (pool or "").strip()
            if not p or p in seen:
                continue
            seen.add(p)
            uniq.append(p)
        if not uniq:
            return {}
        datasets_arg = " ".join(shlex.quote(p) for p in uniq)
        try:
            out = self._zfs_cmd(f"get -H -o name,property,value,source,readonly all -r {datasets_arg}")
        except Exception:
            out = self._zfs_cmd(f"get -H -o name,property,value,source all -r {datasets_arg}")
        return parse_zfs_get_properties_grouped(out)

    def set_dataset_properties(self, dataset: str, properties: Dict[str, str]) -> str:
        logs: List[str] = []
        for prop, value in properties.items():
            assignment = f"{prop}={value}"
            self._zfs_cmd(f"set {shlex.quote(assignment)} {shlex.quote(dataset)}")
            logs.append(assignment)
        return "\n".join(logs)

    def inherit_dataset_properties(self, dataset: str, properties: List[str]) -> str:
        logs: List[str] = []
        for prop in properties:
            p = (prop or "").strip()
            if not p:
                continue
            self._zfs_cmd(f"inherit {shlex.quote(p)} {shlex.quote(dataset)}")
            logs.append(f"inherit {p}")
        return "\n".join(logs)

    def rename_dataset(self, dataset: str, new_name: str) -> str:
        return self._zfs_cmd(f"rename {shlex.quote(dataset)} {shlex.quote(new_name)}")

    def destroy_dataset(self, dataset: str, recursive: bool = False) -> str:
        flag = "-r " if recursive else ""
        return self._zfs_cmd(f"destroy {flag}{shlex.quote(dataset)}", timeout_seconds=None)


class SSHExecutor(BaseExecutor):
    def _zpool_cmd(self, args: str) -> str:
        return build_remote_zfs_cmd(self.profile.os_type, "zpool", args)

    def _zfs_cmd(self, args: str) -> str:
        return build_remote_zfs_cmd(self.profile.os_type, "zfs", args)

    def _wrap_remote_shell(self, command: str) -> str:
        if self.profile.os_type == "Windows":
            escaped = command.replace('"', '\\"')
            return f'powershell -NoProfile -NonInteractive -Command "{escaped}"'
        return f"sh -lc {shlex.quote(command)}"

    def _strip_sudo_echo(self, text: str) -> str:
        password = (self.profile.password or "").strip()
        if not password:
            return text
        cleaned: List[str] = []
        for line in text.splitlines():
            if line.strip() == password:
                continue
            cleaned.append(line)
        return "\n".join(cleaned)

    def _connect(self):
        if paramiko is None:
            raise ExecutorError("paramiko no instalado")
        key = _session_profile_key(self.profile)
        with _SSH_SESSION_LOCK:
            cached = _SSH_SESSION_CACHE.get(key)
            if cached is not None:
                try:
                    transport = cached.get_transport()
                    if transport is not None and transport.is_active():
                        return cached
                except Exception:
                    pass
                try:
                    cached.close()
                except Exception:
                    pass
                _SSH_SESSION_CACHE.pop(key, None)

        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kwargs: Dict[str, Any] = {
            "hostname": self.profile.host,
            "port": self.profile.port or 22,
            "username": self.profile.username,
            "timeout": 10,
            "look_for_keys": False,
        }
        resolved_key = _resolve_ssh_private_key_path(self.profile.key_path)
        if resolved_key:
            kwargs["key_filename"] = resolved_key
        if self.profile.password:
            kwargs["password"] = self.profile.password
        kwargs["banner_timeout"] = 10
        kwargs["auth_timeout"] = 10
        client.connect(**kwargs)
        with _SSH_SESSION_LOCK:
            _SSH_SESSION_CACHE[key] = client
        return client

    def _invalidate_session(self) -> None:
        key = _session_profile_key(self.profile)
        with _SSH_SESSION_LOCK:
            client = _SSH_SESSION_CACHE.pop(key, None)
        if client is not None:
            try:
                client.close()
            except Exception:
                pass

    def _run(
        self,
        command: str,
        sudo: bool = False,
        timeout_seconds: Optional[int] = COMMAND_TIMEOUT_SECONDS,
    ) -> str:
        ssh_busy(1)
        try:
            client = self._connect()
        except Exception as exc:
            ssh_busy(-1)
            raise ExecutorError(str(exc)) from exc

        try:
            cmd = command
            target = f"{self.profile.username + '@' if self.profile.username else ''}{self.profile.host}:{self.profile.port or 22}"
            if sudo and self.profile.use_sudo:
                if self.profile.password:
                    cmd = f"sudo -S -p '' -k sh -lc {shlex.quote(command)}"
                    ssh_trace(f"{target} $ {cmd}")
                    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout_seconds)
                    stdin.write(self.profile.password + "\n")
                    stdin.flush()
                    try:
                        stdin.close()
                    except Exception:
                        pass
                else:
                    # Sin password configurado: falla rapido sin quedarse esperando prompt.
                    cmd = f"sudo -n sh -lc {shlex.quote(command)}"
                    ssh_trace(f"{target} $ {cmd}")
                    _stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout_seconds)
            else:
                shell_cmd = self._wrap_remote_shell(cmd)
                ssh_trace(f"{target} $ {shell_cmd}")
                _stdin, stdout, stderr = client.exec_command(shell_cmd, timeout=timeout_seconds)

            start = time.monotonic()
            while not stdout.channel.exit_status_ready():
                if timeout_seconds is not None and (time.monotonic() - start > timeout_seconds):
                    try:
                        stdout.channel.close()
                    except Exception:
                        pass
                    raise ExecutorError(trf("error_timeout_remote", cmd=cmd))
                time.sleep(0.1)
            exit_code = stdout.channel.recv_exit_status()
            out = stdout.read().decode("utf-8", errors="replace")
            err = stderr.read().decode("utf-8", errors="replace")
            if sudo and self.profile.use_sudo:
                out = self._strip_sudo_echo(out)
                err = self._strip_sudo_echo(err)
            if exit_code != 0:
                raise ExecutorError(err.strip() or out.strip() or trf("error_executing", cmd=cmd))
            return out
        except Exception as exc:
            if not isinstance(exc, ExecutorError):
                self._invalidate_session()
            raise
        finally:
            ssh_busy(-1)

    def check_connection(self) -> Tuple[bool, str]:
        try:
            if self.profile.os_type == "Windows":
                out = self._run("[System.Environment]::OSVersion.VersionString")
                return True, out.strip().splitlines()[0] if out.strip() else tr("ssh_ok")
            out = self._run("uname -a")
            return True, out.strip().splitlines()[0] if out.strip() else tr("ssh_ok")
        except Exception as exc:
            return False, str(exc)

    def _detect_zfs_version(self) -> Optional[Tuple[int, int, int]]:
        commands = [self._zfs_cmd("version"), "zfs version"]
        for cmd in commands:
            try:
                out = self._run(cmd, sudo=False, timeout_seconds=10)
                ver = parse_openzfs_version(out)
                if ver:
                    return ver
            except Exception:
                continue
        if self.profile.use_sudo:
            for cmd in commands:
                try:
                    out = self._run(cmd, sudo=True, timeout_seconds=10)
                    ver = parse_openzfs_version(out)
                    if ver:
                        return ver
                except Exception:
                    continue
        return None

    def check_sudo(self) -> Optional[bool]:
        try:
            self._run("true", sudo=True)
            return True
        except Exception:
            return False

    def list_imported_pools(self) -> List[Dict[str, str]]:
        output = self._run(self._zpool_cmd("list -H -p -o name,size,alloc,free,cap,dedupratio"), sudo=True)
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = re.split(r"\s+", line)
            if len(parts) < 6:
                continue
            name, size, used, free = parts[0], parts[1], parts[2], parts[3]
            dedup = parts[5] if len(parts) > 5 else "-"
            try:
                cr = self._run(self._zfs_cmd(f"get -H -o value compressratio {shlex.quote(name)}"), sudo=True).strip() or "-"
            except Exception:
                cr = "-"
            rows.append(
                {
                    "pool": name,
                    "size": format_bytes_compact(size),
                    "used": format_bytes_compact(used),
                    "free": format_bytes_compact(free),
                    "compressratio": cr,
                    "dedup": dedup,
                }
            )
        return rows

    def list_importable_pools(self) -> List[Dict[str, str]]:
        ver = self.get_zfs_version()
        for args in zpool_import_probe_commands(ver):
            try:
                out = self._run(self._zpool_cmd(args), sudo=True)
                if args.endswith("-H -o name"):
                    rows = parse_importable_name_lines(out)
                else:
                    rows = parse_zpool_import_output(out)
                if rows:
                    return rows
            except Exception:
                continue
        return []

    def list_datasets(self, pool: str) -> List[Dict[str, str]]:
        output = self._run(
            self._zfs_cmd(
                f"list -H -p -t filesystem,volume,snapshot -o name,refer,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r {shlex.quote(pool)}"
            ),
            sudo=True,
        )
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = re.split(r"\s+", line)
            if len(parts) < 10:
                continue
            rows.append(
                {
                    "name": parts[0],
                    "size": format_bytes_compact(parts[1]),
                    "used": format_bytes_compact(parts[2]),
                    "compressratio": parts[3],
                    "encryption": parts[4],
                    "creation": parts[5],
                    "referenced": format_bytes_compact(parts[6]),
                    "mounted": parts[7],
                    "mountpoint": parts[8],
                    "canmount": parts[9],
                }
            )
        return rows

    def import_pool(self, pool: str, options: Dict[str, Any]) -> str:
        cmd = build_zpool_import_cmd(pool, options)
        return self._run(self._zpool_cmd(cmd), sudo=True)

    def export_pool(self, pool: str) -> str:
        return self._run(self._zpool_cmd(f"export {shlex.quote(pool)}"), sudo=True)

    def list_pool_devices(self, pool: str) -> List[Dict[str, Any]]:
        out = self._run(self._zpool_cmd(f"status -P {shlex.quote(pool)}"), sudo=True)
        return parse_zpool_status_devices_tree(out, pool)

    def mount_dataset(self, dataset: str) -> str:
        return self._run(self._zfs_cmd(f"mount {shlex.quote(dataset)}"), sudo=True)

    def unmount_dataset(self, dataset: str, recursive: bool = False) -> str:
        quoted = shlex.quote(dataset)
        if recursive:
            cmd = (
                "set -e; "
                f"DATASET={quoted}; "
                "zfs list -H -o name -t filesystem,volume -r \"$DATASET\" "
                "| awk 'NF{d=gsub(/\\//,\"/\",$1); printf \"%08d\\t%s\\n\", d, $1}' "
                "| sort -r | cut -f2- "
                "| while IFS= read -r ds; do [ -n \"$ds\" ] && (zfs umount -f \"$ds\" >/dev/null 2>&1 || zfs unmount -f \"$ds\" >/dev/null 2>&1) || true; done; "
                "LEFT=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds || index($1, ds\"/\")==1 {print $1}' | head -n1)\"; "
                "[ -z \"$LEFT\" ] || { echo \"cannot unmount dataset tree: $DATASET ($LEFT)\" >&2; exit 1; }"
            )
            return self._run(cmd, sudo=True)
        try:
            return self._run(self._zfs_cmd(f"umount {quoted}"), sudo=True)
        except Exception:
            try:
                return self._run(self._zfs_cmd(f"unmount {quoted}"), sudo=True)
            except Exception as exc2:
                try:
                    return self._run(self._zfs_cmd(f"umount -f {quoted}"), sudo=True)
                except Exception:
                    try:
                        return self._run(self._zfs_cmd(f"unmount -f {quoted}"), sudo=True)
                    except Exception as exc4:
                        raise ExecutorError(str(exc4))

    def list_pool_properties(self, pool: str) -> List[Dict[str, str]]:
        output = self._run(self._zpool_cmd(f"get -H all {shlex.quote(pool)}"), sudo=True)
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = line.split(None, 3)
            if len(parts) < 4:
                continue
            rows.append({"property": parts[1], "value": parts[2], "source": parts[3]})
        return rows

    def pool_status_verbose(self, pool: str) -> str:
        return self._run(self._zpool_cmd(f"status -v {shlex.quote(pool)}"), sudo=True)

    def list_mounted_datasets(self) -> List[Dict[str, str]]:
        out = self._run(self._zfs_cmd("mount"), sudo=True)
        return parse_zfs_mount_rows(out)

    def create_dataset(self, dataset_path: str, options: Dict[str, Any]) -> str:
        cmd = build_zfs_create_cmd(dataset_path, options)
        return self._run(self._zfs_cmd(cmd), sudo=True)

    def list_dataset_properties(self, dataset: str) -> List[Dict[str, str]]:
        quoted = shlex.quote(dataset)
        try:
            out = self._run(self._zfs_cmd(f"get -H -o property,value,source,readonly all {quoted}"), sudo=True)
        except Exception:
            out = self._run(self._zfs_cmd(f"get -H -o property,value,source all {quoted}"), sudo=True)
        return parse_zfs_get_properties(out)

    def list_all_dataset_properties(self, pools: List[str]) -> Dict[str, List[Dict[str, str]]]:
        uniq: List[str] = []
        seen: set[str] = set()
        for pool in pools:
            p = (pool or "").strip()
            if not p or p in seen:
                continue
            seen.add(p)
            uniq.append(p)
        if not uniq:
            return {}
        datasets_arg = " ".join(shlex.quote(p) for p in uniq)
        try:
            out = self._run(
                self._zfs_cmd(f"get -H -o name,property,value,source,readonly all -r {datasets_arg}"),
                sudo=True,
            )
        except Exception:
            out = self._run(
                self._zfs_cmd(f"get -H -o name,property,value,source all -r {datasets_arg}"),
                sudo=True,
            )
        return parse_zfs_get_properties_grouped(out)

    def set_dataset_properties(self, dataset: str, properties: Dict[str, str]) -> str:
        logs: List[str] = []
        for prop, value in properties.items():
            assignment = f"{prop}={value}"
            self._run(self._zfs_cmd(f"set {shlex.quote(assignment)} {shlex.quote(dataset)}"), sudo=True)
            logs.append(assignment)
        return "\n".join(logs)

    def inherit_dataset_properties(self, dataset: str, properties: List[str]) -> str:
        logs: List[str] = []
        for prop in properties:
            p = (prop or "").strip()
            if not p:
                continue
            self._run(self._zfs_cmd(f"inherit {shlex.quote(p)} {shlex.quote(dataset)}"), sudo=True)
            logs.append(f"inherit {p}")
        return "\n".join(logs)

    def rename_dataset(self, dataset: str, new_name: str) -> str:
        return self._run(self._zfs_cmd(f"rename {shlex.quote(dataset)} {shlex.quote(new_name)}"), sudo=True)

    def destroy_dataset(self, dataset: str, recursive: bool = False) -> str:
        flag = "-r " if recursive else ""
        return self._run(self._zfs_cmd(f"destroy {flag}{shlex.quote(dataset)}"), sudo=True, timeout_seconds=None)


class PSRPExecutor(BaseExecutor):
    def _client(self):
        if PSRPClient is None:
            raise ExecutorError("pypsrp no instalado")
        if not self.profile.password:
            raise ExecutorError("PowerShell Remoting requiere password")

        key = _session_profile_key(self.profile)
        with _PSRP_SESSION_LOCK:
            cached = _PSRP_SESSION_CACHE.get(key)
            if cached is not None:
                return cached

        server = self.profile.host
        kwargs: Dict[str, Any] = {
            "server": server,
            "username": self.profile.username,
            "password": self.profile.password,
            "ssl": self.profile.use_ssl,
            "port": self.profile.port or (5986 if self.profile.use_ssl else 5985),
            "auth": self.profile.auth or "ntlm",
            "cert_validation": False,
            # Evita bloqueos largos en transporte WSMan/HTTP(S).
            "connection_timeout": 8,
            "operation_timeout": 15,
            "read_timeout": 20,
        }
        try:
            client = PSRPClient(**kwargs)
        except TypeError:
            # Compatibilidad con versiones de pypsrp que no exponen todos los timeouts.
            kwargs.pop("read_timeout", None)
            kwargs.pop("operation_timeout", None)
            kwargs.pop("connection_timeout", None)
            client = PSRPClient(**kwargs)
        with _PSRP_SESSION_LOCK:
            _PSRP_SESSION_CACHE[key] = client
        return client

    def _invalidate_session(self) -> None:
        key = _session_profile_key(self.profile)
        with _PSRP_SESSION_LOCK:
            _PSRP_SESSION_CACHE.pop(key, None)

    def _run_ps(self, script: str, timeout_seconds: Optional[int] = PSRP_TIMEOUT_SECONDS) -> str:
        def _psrp_errors_to_text(streams: Any) -> str:
            raw_errors = getattr(streams, "error", []) or []
            parts: List[str] = []
            for item in raw_errors:
                try:
                    msg = getattr(item, "message", None)
                    txt = str(msg if msg is not None else item).strip()
                except Exception:
                    txt = ""
                if txt:
                    parts.append(txt)
            return "\n".join(parts)

        def _exec_ps() -> Tuple[str, Any, bool]:
            client = self._client()
            return client.execute_ps(script)

        def _exec_ps_encoded() -> Tuple[str, Any, bool]:
            client = self._client()
            encoded = base64.b64encode((script or "").encode("utf-16-le")).decode("ascii")
            cmd = f"powershell -NoProfile -NonInteractive -EncodedCommand {encoded}"
            stdout, stderr, rc = client.execute_cmd(cmd)
            if int(rc or 0) != 0:
                raise ExecutorError((stderr or stdout or tr("error_ps_remote")).strip())
            return (stdout or ""), None, False

        target = f"{self.profile.username + '@' if self.profile.username else ''}{self.profile.host}:{self.profile.port or (5986 if self.profile.use_ssl else 5985)}"
        one_line = " ".join((script or "").split())
        ssh_trace(f"{target} $ powershell -NoProfile -NonInteractive -Command {shlex.quote(one_line)}")

        for attempt in (1, 2):
            try:
                def _run_exec() -> Tuple[str, Any, bool]:
                    try:
                        return _exec_ps()
                    except Exception as ex:
                        msg = str(ex)
                        if "Invoke-Expression" in msg or "ParameterBindingValidationException" in msg:
                            return _exec_ps_encoded()
                        raise

                if timeout_seconds is None:
                    output, streams, had_errors = _run_exec()
                else:
                    output, streams, had_errors = run_with_timeout(
                        _run_exec,
                        timeout_seconds,
                        tr("error_timeout_ps_remote"),
                    )
                if had_errors:
                    err = _psrp_errors_to_text(streams)
                    if "Invoke-Expression" in err or "ParameterBindingValidationException" in err:
                        if timeout_seconds is None:
                            output, streams, had_errors = _exec_ps_encoded()
                        else:
                            output, streams, had_errors = run_with_timeout(
                                _exec_ps_encoded,
                                timeout_seconds,
                                tr("error_timeout_ps_remote"),
                            )
                        if not had_errors:
                            return output or ""
                        err = _psrp_errors_to_text(streams)
                    out_text = (output or "").strip()
                    if out_text:
                        # Incluye salida de PowerShell para diagnosticar fallos remotos
                        # donde pypsrp solo entrega un ErrorRecord genérico.
                        err = ((err + "\n") if err else "") + f"[stdout] {out_text}"
                    raise ExecutorError(err or tr("error_ps_remote"))
                return output or ""
            except ExecutorError:
                raise
            except Exception as exc:
                self._invalidate_session()
                if attempt == 1:
                    continue
                raise ExecutorError(str(exc)) from exc
        raise ExecutorError(tr("error_ps_remote"))

    def check_connection(self) -> Tuple[bool, str]:
        try:
            out = self._run_ps("$PSVersionTable.PSVersion.ToString()")
            return True, trf("ps_ok", version=out.strip() or "OK")
        except Exception as exc:
            return False, str(exc)

    def _detect_zfs_version(self) -> Optional[Tuple[int, int, int]]:
        try:
            out = self._run_ps("zfs version", timeout_seconds=15)
            return parse_openzfs_version(out)
        except Exception:
            return None

    def check_sudo(self) -> Optional[bool]:
        script = (
            "$id=[Security.Principal.WindowsIdentity]::GetCurrent();"
            "$p=New-Object Security.Principal.WindowsPrincipal($id);"
            "$p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)"
        )
        try:
            out = self._run_ps(script).strip().lower()
            return out == "true"
        except Exception:
            return False

    def list_imported_pools(self) -> List[Dict[str, str]]:
        script = (
            "$rows = zpool list -H -p -o name,size,alloc,free,cap,dedupratio;"
            "$out = @();"
            "foreach ($line in $rows) {"
            "  $r = ($line -split '\\s+');"
            "  if ($r.Length -lt 6) { continue }"
            "  $name = $r[0]; $size = $r[1]; $used = $r[2]; $free = $r[3]; $dedup = $r[5];"
            "  try { $cr = (zfs get -H -o value compressratio $name).Trim() } catch { $cr = '-' }"
            "  $out += \"$name $size $used $free $cr $dedup\""
            "}"
            "$out -join \"`n\""
        )
        output = self._run_ps(script)
        rows = parse_imported_pool_rows(output)
        for row in rows:
            row["size"] = format_bytes_compact(row.get("size", ""))
            row["used"] = format_bytes_compact(row.get("used", ""))
            row["free"] = format_bytes_compact(row.get("free", ""))
        return rows

    def list_importable_pools(self) -> List[Dict[str, str]]:
        ver = self.get_zfs_version()
        for cmd in zpool_import_probe_commands(ver):
            try:
                out = self._run_ps(f"zpool {cmd}" if not cmd.startswith("import") else f"zpool {cmd}")
                if cmd.endswith("-H -o name"):
                    rows = parse_importable_name_lines(out)
                else:
                    rows = parse_zpool_import_output(out)
                if rows:
                    return rows
            except Exception:
                continue
        return []

    def list_datasets(self, pool: str) -> List[Dict[str, str]]:
        output = self._run_ps(
            f"zfs list -H -p -t filesystem,volume,snapshot -o name,refer,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r {pool}"
        )
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = re.split(r"\s+", line)
            if len(parts) < 10:
                continue
            rows.append(
                {
                    "name": parts[0],
                    "size": format_bytes_compact(parts[1]),
                    "used": format_bytes_compact(parts[2]),
                    "compressratio": parts[3],
                    "encryption": parts[4],
                    "creation": parts[5],
                    "referenced": format_bytes_compact(parts[6]),
                    "mounted": parts[7],
                    "mountpoint": parts[8],
                    "canmount": parts[9],
                }
            )
        return rows

    def import_pool(self, pool: str, options: Dict[str, Any]) -> str:
        cmd = build_zpool_import_cmd(pool, options)
        return self._run_ps(f"zpool {cmd}")

    def export_pool(self, pool: str) -> str:
        return self._run_ps(f"zpool export {pool}")

    def list_pool_devices(self, pool: str) -> List[Dict[str, Any]]:
        out = self._run_ps(f"zpool status -P {pool}")
        return parse_zpool_status_devices_tree(out, pool)

    def mount_dataset(self, dataset: str) -> str:
        return self._run_ps(f"zfs mount {dataset}")

    def unmount_dataset(self, dataset: str, recursive: bool = False) -> str:
        if recursive:
            ds_q = "'" + dataset.replace("'", "''") + "'"
            script = (
                "$ErrorActionPreference='Stop'; "
                f"$ds={ds_q}; "
                "$items = @(zfs list -H -o name -t filesystem,volume -r $ds | "
                "ForEach-Object { [PSCustomObject]@{ name=$_; depth=(($_ -split '/').Count) } } | "
                "Sort-Object depth -Descending); "
                "foreach ($i in $items) { try { zfs umount -f $i.name | Out-Null } catch { try { zfs unmount -f $i.name | Out-Null } catch {} } }; "
                "$left = @(zfs mount | ForEach-Object { ($_ -split '\\s+')[0] } | "
                "Where-Object { $_ -eq $ds -or $_.StartsWith($ds + '/') }); "
                "if ($left.Count -gt 0) { throw ('cannot unmount dataset tree: ' + $ds + ' (' + $left[0] + ')') }"
            )
            return self._run_ps(script)
        try:
            return self._run_ps(f"zfs umount {dataset}")
        except Exception:
            return self._run_ps(f"zfs unmount {dataset}")

    def list_pool_properties(self, pool: str) -> List[Dict[str, str]]:
        output = self._run_ps(f"zpool get -H all {pool}")
        rows: List[Dict[str, str]] = []
        for raw in output.splitlines():
            line = raw.strip()
            if not line:
                continue
            parts = line.split(None, 3)
            if len(parts) < 4:
                continue
            rows.append({"property": parts[1], "value": parts[2], "source": parts[3]})
        return rows

    def pool_status_verbose(self, pool: str) -> str:
        return self._run_ps(f"zpool status -v {pool}")

    def list_mounted_datasets(self) -> List[Dict[str, str]]:
        out = self._run_ps("zfs mount")
        return parse_zfs_mount_rows(out)

    def create_dataset(self, dataset_path: str, options: Dict[str, Any]) -> str:
        cmd = build_zfs_create_cmd(dataset_path, options)
        return self._run_ps(f"zfs {cmd}")

    def list_dataset_properties(self, dataset: str) -> List[Dict[str, str]]:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"

        try:
            out = self._run_ps(f"zfs get -H -o property,value,source,readonly all {_ps_quote(dataset)}")
        except Exception:
            out = self._run_ps(f"zfs get -H -o property,value,source all {_ps_quote(dataset)}")
        return parse_zfs_get_properties(out)

    def list_all_dataset_properties(self, pools: List[str]) -> Dict[str, List[Dict[str, str]]]:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"

        uniq: List[str] = []
        seen: set[str] = set()
        for pool in pools:
            p = (pool or "").strip()
            if not p or p in seen:
                continue
            seen.add(p)
            uniq.append(p)
        if not uniq:
            return {}
        datasets_arg = " ".join(_ps_quote(p) for p in uniq)
        try:
            out = self._run_ps(f"zfs get -H -o name,property,value,source,readonly all -r {datasets_arg}")
        except Exception:
            out = self._run_ps(f"zfs get -H -o name,property,value,source all -r {datasets_arg}")
        return parse_zfs_get_properties_grouped(out)

    def set_dataset_properties(self, dataset: str, properties: Dict[str, str]) -> str:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"

        logs: List[str] = []
        for prop, value in properties.items():
            assignment = f"{prop}={value}"
            self._run_ps(f"zfs set {_ps_quote(assignment)} {_ps_quote(dataset)}")
            logs.append(assignment)
        return "\n".join(logs)

    def inherit_dataset_properties(self, dataset: str, properties: List[str]) -> str:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"

        logs: List[str] = []
        for prop in properties:
            p = (prop or "").strip()
            if not p:
                continue
            self._run_ps(f"zfs inherit {_ps_quote(p)} {_ps_quote(dataset)}")
            logs.append(f"inherit {p}")
        return "\n".join(logs)

    def rename_dataset(self, dataset: str, new_name: str) -> str:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"

        return self._run_ps(f"zfs rename {_ps_quote(dataset)} {_ps_quote(new_name)}")

    def destroy_dataset(self, dataset: str, recursive: bool = False) -> str:
        def _ps_quote(s: str) -> str:
            return "'" + s.replace("'", "''") + "'"
        ds_q = _ps_quote(dataset)
        if not recursive:
            # Conservador: deja que la UI pregunte confirmacion recursiva si aplica.
            try:
                return self._run_ps(f"zfs destroy {ds_q}", timeout_seconds=None)
            except Exception:
                return self._run_ps(f"zfs destroy -f {ds_q}", timeout_seconds=None)

        # En Windows/PSRP puede fallar por dataset montado/bloqueado.
        # Probamos variantes crecientes para datasets con hijos/dependencias.
        commands = [
            f"zfs destroy -r -f {ds_q}",
            f"zfs destroy -R -f {ds_q}",
            f"zfs destroy -r {ds_q}",
            f"zfs destroy -R {ds_q}",
        ]
        last_err: Optional[Exception] = None
        for cmd in commands:
            try:
                return self._run_ps(cmd, timeout_seconds=None)
            except Exception as exc:
                last_err = exc
                # Reintento tras forzar desmontado del dataset raiz.
                try:
                    self._run_ps(f"zfs unmount -f {ds_q}", timeout_seconds=None)
                except Exception:
                    pass
        if last_err is not None:
            raise last_err
        raise RuntimeError("destroy failed")


def make_executor(profile: ConnectionProfile) -> BaseExecutor:
    if profile.conn_type == "LOCAL":
        return LocalExecutor(profile)
    if profile.conn_type == "SSH":
        return SSHExecutor(profile)
    if profile.conn_type == "PSRP":
        return PSRPExecutor(profile)
    raise ValueError(f"Tipo no soportado: {profile.conn_type}")


def parse_zpool_import_output(text: str) -> List[Dict[str, str]]:
    pools: List[Dict[str, str]] = []
    current: Optional[Dict[str, str]] = None
    collecting_status = False

    for raw in text.splitlines():
        line = raw.rstrip("\n")
        pool_match = re.match(r"\s*pool:\s*(.+)$", line)
        if pool_match:
            if current:
                pools.append(current)
            current = {"pool": pool_match.group(1).strip(), "id": "", "state": "", "status": ""}
            collecting_status = False
            continue

        if not current:
            continue

        id_match = re.match(r"\s*id:\s*(.+)$", line)
        if id_match:
            current["id"] = id_match.group(1).strip()
            continue

        state_match = re.match(r"\s*state:\s*(.+)$", line)
        if state_match:
            current["state"] = state_match.group(1).strip()
            collecting_status = False
            continue

        status_match = re.match(r"\s*status:\s*(.+)$", line)
        if status_match:
            current["status"] = status_match.group(1).strip()
            collecting_status = True
            continue

        if collecting_status:
            if re.match(r"\s*(action|see|config):", line):
                collecting_status = False
            elif line.strip():
                current["status"] = (current["status"] + " " + line.strip()).strip()

    if current:
        pools.append(current)

    return pools


def parse_importable_name_lines(text: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for raw in text.splitlines():
        name = raw.strip()
        if not name:
            continue
        rows.append({"pool": name, "id": "", "state": "", "status": ""})
    return rows


def parse_zfs_get_properties(text: str) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        # zfs -H separa por tab; fallback a split por espacios.
        parts = line.split("\t")
        if len(parts) >= 4:
            # Con -o property,value,source,readonly
            prop, value, source, readonly = parts[0], parts[1], parts[2], parts[3]
        elif len(parts) >= 3:
            # Con -o property,value,source
            prop, value, source = parts[0], parts[1], parts[2]
            readonly = ""
        else:
            sp = line.split(None, 3)
            if len(sp) < 3:
                continue
            prop, value, source = sp[0], sp[1], sp[2]
            readonly = sp[3] if len(sp) > 3 else ""
        rows.append(
            {
                "property": prop.strip(),
                "value": value.strip(),
                "source": source.strip(),
                "readonly": readonly.strip().lower(),
            }
        )
    return rows


def parse_zfs_get_properties_grouped(text: str) -> Dict[str, List[Dict[str, str]]]:
    grouped: Dict[str, List[Dict[str, str]]] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) >= 5:
            ds, prop, value, source, readonly = parts[0], parts[1], parts[2], parts[3], parts[4]
        elif len(parts) >= 4:
            ds, prop, value, source = parts[0], parts[1], parts[2], parts[3]
            readonly = ""
        else:
            sp = line.split(None, 4)
            if len(sp) < 4:
                continue
            ds, prop, value, source = sp[0], sp[1], sp[2], sp[3]
            readonly = sp[4] if len(sp) > 4 else ""
        ds = ds.strip()
        if not ds:
            continue
        grouped.setdefault(ds, []).append(
            {
                "property": (prop or "").strip(),
                "value": (value or "").strip(),
                "source": (source or "").strip(),
                "readonly": (readonly or "").strip().lower(),
            }
        )
    return grouped


def _is_user_property(prop_name: str) -> bool:
    # User properties siguen el formato namespace:property.
    return ":" in prop_name


def is_dataset_property_editable(prop_name: str, dataset_type: str, source: str, readonly: str) -> bool:
    prop = (prop_name or "").strip().lower()
    ds_type = (dataset_type or "").strip().lower()
    src = (source or "").strip()
    ro = (readonly or "").strip().lower()
    if not prop:
        return False
    if ro in {"true", "on", "yes", "1"}:
        return False
    # source "-" suele corresponder a metricas/valores no configurables.
    if src == "-":
        return False
    if _is_user_property(prop):
        return True
    if ds_type == "filesystem":
        return prop in EDITABLE_DATASET_PROPS_FILESYSTEM
    if ds_type == "volume":
        return prop in EDITABLE_DATASET_PROPS_VOLUME
    if ds_type == "snapshot":
        return prop in EDITABLE_DATASET_PROPS_SNAPSHOT
    # Tipo desconocido: fallback conservador.
    return prop in (EDITABLE_DATASET_PROPS_FILESYSTEM | EDITABLE_DATASET_PROPS_VOLUME)


def parse_zpool_status_devices_tree(text: str, pool_name: str) -> List[Dict[str, Any]]:
    nodes: List[Dict[str, Any]] = []
    in_config = False
    base_indent: Optional[int] = None
    for raw in text.splitlines():
        line = raw.rstrip("\n")
        stripped = line.strip()
        if stripped.lower().startswith("config:"):
            in_config = True
            continue
        if not in_config:
            continue
        if stripped.lower().startswith("errors:"):
            break
        if not stripped:
            continue
        if stripped.startswith("NAME") or stripped.startswith("----"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        parts = stripped.split()
        token = parts[0]
        state = ""
        if len(parts) > 1 and re.fullmatch(r"[A-Z_]+", parts[1] or ""):
            state = parts[1]
        if base_indent is None:
            base_indent = indent
        rel = max(0, indent - (base_indent or 0))
        level = rel // 2
        # La raiz del arbol es el nombre del pool; se representa en la fila padre.
        if token == pool_name and level == 0:
            continue
        nodes.append({"name": token, "level": level, "state": state})
    return nodes


def build_zpool_import_cmd(pool: str, options: Dict[str, Any]) -> str:
    parts = ["import"]

    flag_map = {
        "force": "-f",
        "missing_log": "-m",
        "do_not_mount": "-N",
        "rewind": "-F",
        "rewind_dry_run": "-n",
        "destroyed": "-D",
        "extreme_rewind": "-X",
        "load_keys": "-l",
    }
    for key, flag in flag_map.items():
        if options.get(key):
            parts.append(flag)

    if options.get("cachefile"):
        parts.extend(["-c", shlex.quote(options["cachefile"])])

    altroot = options.get("altroot")
    if altroot:
        parts.extend(["-R", shlex.quote(altroot)])

    for d in options.get("directories", []):
        if d.strip():
            parts.extend(["-d", shlex.quote(d.strip())])

    mntopts = options.get("mntopts")
    if mntopts:
        parts.extend(["-o", shlex.quote(mntopts)])

    for prop in options.get("properties", []):
        if prop.strip():
            parts.extend(["-o", shlex.quote(prop.strip())])

    txg = options.get("txg")
    if txg:
        parts.extend(["-T", shlex.quote(str(txg))])

    parts.append(shlex.quote(pool))

    new_name = options.get("new_name")
    if new_name:
        parts.append(shlex.quote(new_name))

    extra = options.get("extra_args")
    if extra:
        parts.extend(shlex.split(extra))

    return " ".join(parts)


def build_zfs_create_cmd(dataset_path: str, options: Dict[str, Any]) -> str:
    ds_type = (options.get("ds_type") or "filesystem").strip().lower()
    if ds_type == "snapshot":
        parts = ["snapshot"]
        if options.get("snapshot_recursive"):
            parts.append("-r")
        for prop in options.get("properties", []):
            p = str(prop).strip()
            if p:
                parts.extend(["-o", shlex.quote(p)])
        extra = (options.get("extra_args") or "").strip()
        if extra:
            parts.extend(shlex.split(extra))
        parts.append(shlex.quote(dataset_path))
        return " ".join(parts)

    parts = ["create"]
    if options.get("parents"):
        parts.append("-p")
    if options.get("sparse"):
        parts.append("-s")
    if options.get("nomount"):
        parts.append("-u")
    blocksize = (options.get("blocksize") or "").strip()
    if blocksize:
        parts.extend(["-b", shlex.quote(blocksize)])
    volsize = (options.get("volsize") or "").strip()
    if ds_type == "volume" and volsize:
        parts.extend(["-V", shlex.quote(volsize)])
    for prop in options.get("properties", []):
        p = str(prop).strip()
        if p:
            parts.extend(["-o", shlex.quote(p)])
    extra = (options.get("extra_args") or "").strip()
    if extra:
        parts.extend(shlex.split(extra))
    parts.append(shlex.quote(dataset_path))
    return " ".join(parts)


class ConnectionDialog(tk.Toplevel):
    def __init__(self, master: tk.Misc, profile: Optional[ConnectionProfile] = None) -> None:
        super().__init__(master)
        self.title(tr("conn_dialog_title"))
        self.resizable(True, True)
        self.transient(master)
        self.attributes("-topmost", True)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.profile = profile
        self.result: Optional[ConnectionProfile] = None

        self.var_name = tk.StringVar(value=profile.name if profile else "")
        default_os = profile.os_type if profile else "Linux"
        default_transport = profile.transport if profile else ("PSRP" if default_os == "Windows" else "SSH")
        self.var_os = tk.StringVar(value=default_os)
        self.var_win_transport = tk.StringVar(value=default_transport if default_os == "Windows" else "PSRP")
        self.var_host = tk.StringVar(value=profile.host if profile else "")
        default_port = profile.port if profile else (5985 if default_os == "Windows" and default_transport == "PSRP" else 22)
        self.var_port = tk.StringVar(value=str(default_port))
        self.var_user = tk.StringVar(value=profile.username if profile else "")
        self.var_pass = tk.StringVar(value=profile.password if profile else "")
        self.var_key = tk.StringVar(value=profile.key_path if profile else "")
        self.var_ssl = tk.BooleanVar(value=profile.use_ssl if profile else False)
        self.var_auth = tk.StringVar(value=profile.auth if profile else "ntlm")
        self.var_sudo = tk.BooleanVar(value=profile.use_sudo if profile else True)
        self.test_running = False

        frm = ttk.Frame(self, padding=12)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(1, weight=1)

        rows = [
            ("conn_name", ttk.Entry(frm, textvariable=self.var_name, width=45)),
            (
                "conn_system",
                ttk.Combobox(
                    frm,
                    textvariable=self.var_os,
                    values=["Windows", "Linux", "MacOS"],
                    width=42,
                    state="readonly",
                ),
            ),
            (
                "conn_windows_method",
                ttk.Combobox(frm, textvariable=self.var_win_transport, values=["PSRP", "SSH"], width=42, state="readonly"),
            ),
            ("conn_host", ttk.Entry(frm, textvariable=self.var_host, width=45)),
            ("conn_port", ttk.Entry(frm, textvariable=self.var_port, width=45)),
            ("conn_user", ttk.Entry(frm, textvariable=self.var_user, width=45)),
            ("conn_password", ttk.Entry(frm, textvariable=self.var_pass, show="*", width=45)),
            ("conn_auth_psrp", ttk.Combobox(frm, textvariable=self.var_auth, values=["ntlm", "kerberos", "basic", "credssp"], width=42)),
        ]

        self.field_labels: Dict[str, ttk.Label] = {}
        self.field_widgets: Dict[str, tk.Widget] = {}
        for i, (label_key, widget) in enumerate(rows):
            lbl = ttk.Label(frm, text=tr(label_key))
            lbl.grid(row=i, column=0, sticky="w", padx=(0, 8), pady=4)
            widget.grid(row=i, column=1, sticky="ew", pady=4)
            self.field_labels[label_key] = lbl
            self.field_widgets[label_key] = widget

        key_row = len(rows)
        self.key_label = ttk.Label(frm, text=tr("conn_ssh_key"))
        self.key_label.grid(row=key_row, column=0, sticky="w", padx=(0, 8), pady=4)
        key_frame = ttk.Frame(frm)
        key_frame.grid(row=key_row, column=1, sticky="ew", pady=4)
        key_frame.columnconfigure(0, weight=1)
        self.key_entry = ttk.Entry(key_frame, textvariable=self.var_key)
        self.key_entry.grid(row=0, column=0, sticky="ew")
        self.pick_key_btn = ttk.Button(key_frame, text=tr("conn_pick_ssh_key"), command=self._pick_ssh_key)
        self.pick_key_btn.grid(row=0, column=1, padx=(6, 0))

        self.psrp_ssl_check = ttk.Checkbutton(frm, text=tr("conn_psrp_ssl"), variable=self.var_ssl)
        self.psrp_ssl_check.grid(row=key_row + 1, column=1, sticky="w", pady=4)
        self.sudo_check = ttk.Checkbutton(frm, text=tr("conn_sudo_ssh"), variable=self.var_sudo)
        self.sudo_check.grid(row=key_row + 2, column=1, sticky="w", pady=4)

        test_frame = ttk.LabelFrame(frm, text=tr("conn_test_log"))
        test_frame.grid(row=key_row + 3, column=0, columnspan=2, sticky="nsew", pady=(8, 0))
        frm.rowconfigure(key_row + 3, weight=1)
        test_frame.columnconfigure(0, weight=1)
        test_frame.rowconfigure(0, weight=1)
        self.log_text = tk.Text(
            test_frame,
            height=10,
            width=78,
            state="disabled",
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            insertbackground=UI_TEXT,
            highlightthickness=1,
            highlightbackground=UI_BORDER,
            highlightcolor=UI_ACCENT,
        )
        self.log_text.grid(row=0, column=0, sticky="nsew")
        log_scroll = ttk.Scrollbar(test_frame, orient="vertical", command=self.log_text.yview)
        log_scroll.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=log_scroll.set)

        actions = ttk.Frame(frm)
        actions.grid(row=key_row + 4, column=0, columnspan=2, sticky="e", pady=(10, 0))
        self.test_btn = ttk.Button(actions, text=tr("conn_test_btn"), command=self._test_connection)
        self.test_btn.grid(row=0, column=0, padx=4)
        ttk.Button(actions, text=tr("conn_copy_log_btn"), command=self._copy_log).grid(row=0, column=1, padx=4)
        ttk.Button(actions, text=tr("conn_save_btn"), command=self._save).grid(row=0, column=2, padx=4)
        ttk.Button(actions, text=tr("cancel"), command=self.destroy).grid(row=0, column=3, padx=4)

        self.var_os.trace_add("write", self._toggle_fields)
        self.var_win_transport.trace_add("write", self._toggle_fields)
        self.var_ssl.trace_add("write", self._toggle_fields)
        self._toggle_fields()

        self.grab_set()
        self.wait_visibility()
        self.focus_set()

    def _log(self, message: str) -> None:
        def _append() -> None:
            self.log_text.configure(state="normal")
            self.log_text.insert("end", message.rstrip() + "\n")
            self.log_text.see("end")
            self.log_text.configure(state="disabled")

        self.after(0, _append)

    def _pick_ssh_key(self) -> None:
        # Tk permite controlar visibilidad de archivos ocultos con estas variables.
        # Si la plataforma/dialogo no las soporta, simplemente se ignoran.
        try:
            self.tk.call("set", "::tk::dialog::file::showHiddenBtn", "1")
            self.tk.call("set", "::tk::dialog::file::showHiddenVar", "1")
        except Exception:
            pass

        self.lift()
        self.focus_force()
        path = filedialog.askopenfilename(
            parent=self,
            title=tr("conn_pick_ssh_key_title"),
            filetypes=[(tr("filetypes_all"), "*"), (tr("filetypes_all"), "*.*")],
        )
        self.lift()
        self.focus_force()
        if path:
            self.var_key.set(path)

    def _toggle_fields(self, *_args: Any) -> None:
        os_type = self.var_os.get()
        transport = self.var_win_transport.get() if os_type == "Windows" else "SSH"
        is_ssh = transport == "SSH"
        is_psrp = os_type == "Windows" and transport == "PSRP"

        # Linux/MacOS usan SSH siempre.
        win_method_label = self.field_labels["conn_windows_method"]
        win_method_widget = self.field_widgets["conn_windows_method"]
        if os_type == "Windows":
            win_method_label.grid()
            win_method_widget.grid()
        else:
            win_method_label.grid_remove()
            win_method_widget.grid_remove()

        # Campos de PSRP visibles solo cuando aplica.
        auth_label = self.field_labels["conn_auth_psrp"]
        auth_widget = self.field_widgets["conn_auth_psrp"]
        if is_psrp:
            auth_label.grid()
            auth_widget.grid()
            self.psrp_ssl_check.grid()
            if self.var_port.get() in {"", "22", "5985", "5986"}:
                self.var_port.set("5986" if self.var_ssl.get() else "5985")
        else:
            auth_label.grid_remove()
            auth_widget.grid_remove()
            self.psrp_ssl_check.grid_remove()

        # Campos exclusivos de SSH.
        if is_ssh:
            self.key_label.grid()
            self.pick_key_btn.configure(state="normal")
            self.key_entry.configure(state="normal")
            if os_type in {"Linux", "MacOS"}:
                self.sudo_check.grid()
            else:
                self.sudo_check.grid_remove()
                self.var_sudo.set(False)
            if not self.var_port.get() or self.var_port.get() in {"5985", "5986"}:
                self.var_port.set("22")
        else:
            self.key_label.grid_remove()
            self.pick_key_btn.configure(state="disabled")
            self.key_entry.configure(state="disabled")
            self.sudo_check.grid_remove()
            self.var_sudo.set(False)
            self.var_key.set("")

    def _build_profile_from_fields(self, show_errors: bool) -> Optional[ConnectionProfile]:
        name = self.var_name.get().strip()
        os_type = self.var_os.get().strip()
        if os_type == "Windows":
            if show_errors:
                messagebox.showerror(tr("validation_title"), WINDOWS_CONN_BLOCK_MSG)
            return None
        transport = self.var_win_transport.get().strip() if os_type == "Windows" else "SSH"
        ctype = "PSRP" if (os_type == "Windows" and transport == "PSRP") else "SSH"
        if not name:
            if show_errors:
                messagebox.showerror(tr("validation_title"), tr("validation_name_required"))
            return None

        if not self.var_host.get().strip():
            if show_errors:
                messagebox.showerror(tr("validation_title"), tr("validation_host_required"))
            return None

        try:
            port = int(self.var_port.get() or "0")
        except ValueError:
            if show_errors:
                messagebox.showerror(tr("validation_title"), tr("validation_port_invalid"))
            return None

        if ctype == "PSRP" and port == 0:
            port = 5986 if self.var_ssl.get() else 5985
        elif ctype == "SSH" and port == 0:
            port = 22
        # Autocorreccion PSRP: 5985<->HTTP, 5986<->HTTPS.
        if ctype == "PSRP":
            if self.var_ssl.get() and port == 5985:
                port = 5986
                self.var_port.set(str(port))
            elif (not self.var_ssl.get()) and port == 5986:
                port = 5985
                self.var_port.set(str(port))

        conn_id = self.profile.id if self.profile else str(uuid.uuid4())
        return ConnectionProfile(
            id=conn_id,
            name=name,
            conn_type=ctype,
            os_type=os_type,
            transport=transport,
            host=self.var_host.get().strip(),
            port=port,
            username=self.var_user.get().strip(),
            password=self.var_pass.get(),
            key_path=self.var_key.get().strip() if ctype == "SSH" else "",
            use_ssl=self.var_ssl.get(),
            auth=self.var_auth.get().strip() or "ntlm",
            use_sudo=self.var_sudo.get() if os_type in {"Linux", "MacOS"} else False,
        )

    def _test_connection(self) -> None:
        if self.test_running:
            return

        profile = self._build_profile_from_fields(show_errors=True)
        if profile is None:
            return

        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

        self.test_running = True
        self.test_btn.configure(state="disabled")
        self._log(f"[INFO] {trf('log_test_start', name=profile.name, ctype=profile.conn_type)}")

        def worker() -> None:
            ok = False
            try:
                if profile.conn_type == "LOCAL":
                    ok = self._test_local()
                elif profile.conn_type == "SSH":
                    ok = self._test_ssh(profile)
                elif profile.conn_type == "PSRP":
                    ok = self._test_psrp(profile)
                else:
                    self._log(f"[ERROR] {trf('log_test_unsupported_type', ctype=profile.conn_type)}")
            except Exception as exc:
                self._log(f"[ERROR] {trf('log_test_unhandled_error', error=exc)}")
            finally:
                final_msg = (
                    f"[OK] {tr('log_test_done_ok')}" if ok else f"[ERROR] {tr('log_test_done_error')}"
                )
                self._log(final_msg)
                self.after(0, self._finish_test)

        threading.Thread(target=worker, daemon=True).start()

    def _finish_test(self) -> None:
        self.test_running = False
        self.test_btn.configure(state="normal")

    def _copy_log(self) -> None:
        text = self.log_text.get("1.0", "end-1c")
        self.clipboard_clear()
        self.clipboard_append(text)
        self.update_idletasks()
        self._log(f"[INFO] {tr('log_test_copied')}")

    def _test_local(self) -> bool:
        import subprocess

        self._log(f"[INFO] {tr('log_local_check_env')}")
        if libzfs is not None:
            self._log(f"[INFO] {tr('log_local_libzfs_found')}")
            with libzfs.ZFS() as zfs:
                pools = [p.name for p in zfs.pools]
            self._log(f"[INFO] {trf('log_local_pools', pools=', '.join(pools) if pools else tr('label_none'))}")
        else:
            self._log(f"[WARN] {tr('log_local_libzfs_missing')}")

        self._log(f"[INFO] {tr('log_local_check_zpool')}")
        try:
            proc = subprocess.run(
                build_zfs_binary_cmd("zpool", "list -H -o name"),
                shell=True,
                capture_output=True,
                text=True,
                timeout=COMMAND_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            self._log(f"[ERROR] {tr('log_local_zpool_timeout')}")
            return False
        if proc.returncode != 0:
            self._log(f"[ERROR] {trf('log_local_zpool_failed', error=proc.stderr.strip() or proc.stdout.strip())}")
            return False
        self._log(f"[INFO] {tr('log_local_zpool_ok')}")

        try:
            sudo_proc = subprocess.run("sudo -n true", shell=True, capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            self._log(f"[WARN] {tr('log_local_sudo_timeout')}")
            return True
        if sudo_proc.returncode == 0:
            self._log(f"[OK] {tr('log_local_sudo_ok')}")
        else:
            self._log(f"[WARN] {tr('log_local_sudo_not_allowed')}")
        return True

    def _test_ssh(self, profile: ConnectionProfile) -> bool:
        self._log(f"[INFO] {trf('log_ssh_prepare', host=profile.host, port=profile.port)}")
        if paramiko is None:
            self._log(f"[ERROR] {tr('log_ssh_paramiko_missing')}")
            return False

        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kwargs: Dict[str, Any] = {
            "hostname": profile.host,
            "port": profile.port or 22,
            "username": profile.username,
            "timeout": 10,
            "look_for_keys": False,
        }
        resolved_key = _resolve_ssh_private_key_path(profile.key_path)
        if resolved_key:
            kwargs["key_filename"] = resolved_key
            self._log(f"[INFO] {trf('log_ssh_using_key', path=resolved_key)}")
        if profile.password:
            kwargs["password"] = profile.password
            self._log(f"[INFO] {tr('log_ssh_password_set')}")
        kwargs["banner_timeout"] = 10
        kwargs["auth_timeout"] = 10

        def _exec(client: Any, command: str, sudo: bool = False) -> Tuple[int, str, str]:
            def _wrap_remote_shell(cmd: str) -> str:
                if profile.os_type == "Windows":
                    escaped = cmd.replace('"', '\\"')
                    return f'powershell -NoProfile -NonInteractive -Command "{escaped}"'
                return f"sh -lc {shlex.quote(cmd)}"

            target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or 22}"
            if sudo and profile.use_sudo:
                if profile.password:
                    ssh_cmd = f"sudo -S -p '' -k sh -lc {shlex.quote(command)}"
                    ssh_trace(f"{target} $ {ssh_cmd}")
                    stdin, stdout, stderr = client.exec_command(ssh_cmd, timeout=COMMAND_TIMEOUT_SECONDS)
                    stdin.write(profile.password + "\n")
                    stdin.flush()
                    try:
                        stdin.close()
                    except Exception:
                        pass
                else:
                    ssh_cmd = f"sudo -n sh -lc {shlex.quote(command)}"
                    ssh_trace(f"{target} $ {ssh_cmd}")
                    _stdin, stdout, stderr = client.exec_command(
                        ssh_cmd,
                        timeout=COMMAND_TIMEOUT_SECONDS,
                    )
            else:
                ssh_cmd = _wrap_remote_shell(command)
                ssh_trace(f"{target} $ {ssh_cmd}")
                _stdin, stdout, stderr = client.exec_command(
                    ssh_cmd,
                    timeout=COMMAND_TIMEOUT_SECONDS,
                )

            start = time.monotonic()
            while not stdout.channel.exit_status_ready():
                if time.monotonic() - start > COMMAND_TIMEOUT_SECONDS:
                    try:
                        stdout.channel.close()
                    except Exception:
                        pass
                    return 124, "", trf("error_timeout_remote", cmd=command)
                time.sleep(0.1)
            code = stdout.channel.recv_exit_status()
            out = stdout.read().decode("utf-8", errors="replace").strip()
            err = stderr.read().decode("utf-8", errors="replace").strip()
            if sudo and profile.use_sudo and profile.password:
                if out == profile.password:
                    out = ""
                if err == profile.password:
                    err = ""
            return code, out, err

        try:
            client.connect(**kwargs)
            self._log(f"[OK] {tr('log_ssh_handshake_ok')}")

            probe_cmd = "uname -a" if profile.os_type != "Windows" else "[System.Environment]::OSVersion.VersionString"
            code, out, err = _exec(client, probe_cmd)
            if code != 0:
                self._log(f"[ERROR] {trf('log_ssh_test_cmd_failed', error=err or out)}")
                return False
            self._log(f"[INFO] {trf('log_ssh_remote_host', output=out or tr('label_no_output'))}")
            zfs_ver_code, zfs_ver_out, _zfs_ver_err = _exec(client, build_remote_zfs_cmd(profile.os_type, "zfs", "version"))
            if zfs_ver_code == 0:
                parsed_ver = parse_openzfs_version(zfs_ver_out)
                if parsed_ver:
                    self._log(f"[INFO] OpenZFS: {format_openzfs_version(parsed_ver)}")

            use_sudo_for_checks = profile.use_sudo and bool(profile.password)
            if profile.use_sudo and not profile.password:
                self._log(f"[WARN] {tr('log_ssh_sudo_no_password')}")

            if use_sudo_for_checks:
                self._log(f"[INFO] {tr('log_ssh_check_sudo')}")
                sudo_code, _sudo_out, sudo_err = _exec(client, "true", sudo=True)
                if sudo_code == 0:
                    self._log(f"[OK] {tr('log_ssh_sudo_ok')}")
                else:
                    self._log(f"[WARN] {trf('log_ssh_sudo_not_available', error=sudo_err or tr('label_exit_code_nonzero'))}")

            self._log(f"[INFO] {tr('log_ssh_check_zpool')}")
            zpool_code, zpool_out, zpool_err = _exec(
                client,
                build_remote_zfs_cmd(profile.os_type, "zpool", "list -H -o name"),
                sudo=use_sudo_for_checks,
            )
            if zpool_code != 0:
                self._log(f"[WARN] {trf('log_ssh_zpool_not_accessible', error=zpool_err or zpool_out)}")
                return False
            self._log(f"[OK] {trf('log_ssh_zpool_ok', pools=zpool_out or tr('label_none'))}")
            return True
        except Exception as exc:
            self._log(f"[ERROR] {trf('log_ssh_error', error=exc)}")
            return False
        finally:
            client.close()

    def _test_psrp(self, profile: ConnectionProfile) -> bool:
        self._log(f"[INFO] {trf('log_psrp_prepare', host=profile.host, port=profile.port)}")
        if PSRPClient is None:
            self._log(f"[ERROR] {tr('log_psrp_missing')}")
            return False
        if not profile.password:
            self._log(f"[ERROR] {tr('log_psrp_requires_password')}")
            return False

        try:
            client = PSRPClient(
                server=profile.host,
                username=profile.username,
                password=profile.password,
                ssl=profile.use_ssl,
                port=profile.port or (5986 if profile.use_ssl else 5985),
                auth=profile.auth or "ntlm",
                cert_validation=False,
            )
            self._log(f"[INFO] {tr('log_psrp_session_created')}")
            output, streams, had_errors = run_with_timeout(
                lambda: client.execute_ps("$PSVersionTable.PSVersion.ToString()"),
                PSRP_TIMEOUT_SECONDS,
                tr("error_timeout_ps_check"),
            )
            if had_errors:
                errs_raw = getattr(streams, "error", []) or []
                errs: List[str] = []
                for item in errs_raw:
                    try:
                        msg = getattr(item, "message", None)
                        txt = str(msg if msg is not None else item).strip()
                    except Exception:
                        txt = ""
                    if txt:
                        errs.append(txt)
                self._log(f"[ERROR] {trf('log_psrp_failure', error='; '.join(errs) if errs else tr('label_remote_error'))}")
                return False
            self._log(f"[OK] {trf('log_psrp_connected', version=output.strip() or tr('label_unknown'))}")

            self._log(f"[INFO] {tr('log_psrp_check_admin')}")
            admin_script = (
                "$id=[Security.Principal.WindowsIdentity]::GetCurrent();"
                "$p=New-Object Security.Principal.WindowsPrincipal($id);"
                "$p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)"
            )
            admin_out, _, admin_errors = run_with_timeout(
                lambda: client.execute_ps(admin_script),
                PSRP_TIMEOUT_SECONDS,
                tr("error_timeout_ps_admin"),
            )
            if admin_errors:
                self._log(f"[WARN] {tr('log_psrp_admin_unknown')}")
            else:
                is_admin = admin_out.strip().lower() == "true"
                if is_admin:
                    self._log(f"[OK] {tr('log_psrp_admin_ok')}")
                else:
                    self._log(f"[WARN] {tr('log_psrp_admin_no')}")

            self._log(f"[INFO] {tr('log_psrp_check_zpool')}")
            zpool_out, zpool_streams, zpool_errors = run_with_timeout(
                lambda: client.execute_ps("zpool list -H -o name"),
                PSRP_TIMEOUT_SECONDS,
                tr("error_timeout_ps_zpool"),
            )
            if zpool_errors:
                errs_raw = getattr(zpool_streams, "error", []) or []
                errs: List[str] = []
                for item in errs_raw:
                    try:
                        msg = getattr(item, "message", None)
                        txt = str(msg if msg is not None else item).strip()
                    except Exception:
                        txt = ""
                    if txt:
                        errs.append(txt)
                self._log(f"[WARN] {trf('log_psrp_zpool_not_accessible', error='; '.join(errs) if errs else tr('label_remote_error'))}")
            else:
                self._log(f"[OK] {trf('log_psrp_zpool_ok', pools=zpool_out.strip() or tr('label_none'))}")
            return True
        except Exception as exc:
            self._log(f"[ERROR] {trf('log_psrp_error', error=exc)}")
            return False

    def _save(self) -> None:
        profile = self._build_profile_from_fields(show_errors=True)
        if profile is None:
            return
        self.result = profile
        self.destroy()


class ImportDialog(tk.Toplevel):
    def __init__(self, master: tk.Misc, pool_name: str) -> None:
        super().__init__(master)
        self.title(trf("import_dialog_title", pool=pool_name))
        self.resizable(False, False)
        self.transient(master)
        self.pool_name = pool_name
        self.result: Optional[Dict[str, Any]] = None

        self.vars = {
            "force": tk.BooleanVar(value=False),
            "missing_log": tk.BooleanVar(value=False),
            "do_not_mount": tk.BooleanVar(value=False),
            "rewind": tk.BooleanVar(value=False),
            "rewind_dry_run": tk.BooleanVar(value=False),
            "destroyed": tk.BooleanVar(value=False),
            "extreme_rewind": tk.BooleanVar(value=False),
            "load_keys": tk.BooleanVar(value=False),
            "cachefile": tk.StringVar(),
            "altroot": tk.StringVar(),
            "directories": tk.StringVar(),
            "mntopts": tk.StringVar(),
            "properties": tk.StringVar(),
            "txg": tk.StringVar(),
            "new_name": tk.StringVar(),
            "extra_args": tk.StringVar(),
        }

        frm = ttk.Frame(self, padding=12)
        frm.grid(row=0, column=0, sticky="nsew")

        flags = ttk.LabelFrame(frm, text=tr("import_flags"))
        flags.grid(row=0, column=0, sticky="ew")

        flag_labels = [
            ("force", tr("import_flag_force")),
            ("missing_log", tr("import_flag_missing_log")),
            ("do_not_mount", tr("import_flag_do_not_mount")),
            ("rewind", tr("import_flag_rewind")),
            ("rewind_dry_run", tr("import_flag_rewind_dry_run")),
            ("destroyed", tr("import_flag_destroyed")),
            ("extreme_rewind", tr("import_flag_extreme_rewind")),
            ("load_keys", tr("import_flag_load_keys")),
        ]

        for i, (key, label) in enumerate(flag_labels):
            ttk.Checkbutton(flags, text=label, variable=self.vars[key]).grid(
                row=i // 2, column=i % 2, sticky="w", padx=6, pady=3
            )

        fields = ttk.LabelFrame(frm, text=tr("import_values"))
        fields.grid(row=1, column=0, sticky="ew", pady=(8, 0))

        entry_rows = [
            ("cachefile", tr("import_field_cachefile")),
            ("altroot", tr("import_field_altroot")),
            ("directories", tr("import_field_directories")),
            ("mntopts", tr("import_field_mntopts")),
            ("properties", tr("import_field_properties")),
            ("txg", tr("import_field_txg")),
            ("new_name", tr("import_field_new_name")),
            ("extra_args", tr("import_field_extra_args")),
        ]

        for i, (key, label) in enumerate(entry_rows):
            ttk.Label(fields, text=label).grid(row=i, column=0, sticky="w", padx=(6, 6), pady=4)
            ttk.Entry(fields, textvariable=self.vars[key], width=45).grid(row=i, column=1, sticky="ew", pady=4)

        actions = ttk.Frame(frm)
        actions.grid(row=2, column=0, sticky="e", pady=(10, 0))
        ttk.Button(actions, text=tr("cancel"), command=self.destroy).grid(row=0, column=0, padx=4)
        ttk.Button(actions, text=tr("import_btn"), command=self._accept).grid(row=0, column=1, padx=4)

        self.wait_visibility()
        try:
            self.grab_set()
        except tk.TclError:
            # Evita "grab failed: window not viewable" en algunos WM.
            self.after(10, lambda: self.grab_set() if self.winfo_exists() else None)
        self.focus_set()

    def _accept(self) -> None:
        directories = [x.strip() for x in self.vars["directories"].get().split(",") if x.strip()]
        properties = [x.strip() for x in self.vars["properties"].get().split(",") if x.strip()]

        self.result = {
            "force": self.vars["force"].get(),
            "missing_log": self.vars["missing_log"].get(),
            "do_not_mount": self.vars["do_not_mount"].get(),
            "rewind": self.vars["rewind"].get(),
            "rewind_dry_run": self.vars["rewind_dry_run"].get(),
            "destroyed": self.vars["destroyed"].get(),
            "extreme_rewind": self.vars["extreme_rewind"].get(),
            "load_keys": self.vars["load_keys"].get(),
            "cachefile": self.vars["cachefile"].get().strip(),
            "altroot": self.vars["altroot"].get().strip(),
            "directories": directories,
            "mntopts": self.vars["mntopts"].get().strip(),
            "properties": properties,
            "txg": self.vars["txg"].get().strip(),
            "new_name": self.vars["new_name"].get().strip(),
            "extra_args": self.vars["extra_args"].get().strip(),
        }
        self.destroy()


class CreateDatasetDialog(tk.Toplevel):
    def __init__(self, master: tk.Misc, initial_path: str, base_dataset: str = "") -> None:
        super().__init__(master)
        self.title(tr("create_dataset_title"))
        self.resizable(False, False)
        self.transient(master)
        self.result: Optional[Dict[str, Any]] = None

        self.var_path = tk.StringVar(value=initial_path)
        self.base_dataset = base_dataset.strip()
        self.var_type = tk.StringVar(value="filesystem")
        self.var_volsize = tk.StringVar()
        self.var_blocksize = tk.StringVar()
        self.var_extra = tk.StringVar()
        self.var_parents = tk.BooleanVar(value=True)
        self.var_sparse = tk.BooleanVar(value=False)
        self.var_nomount = tk.BooleanVar(value=False)
        self.var_snapshot_recursive = tk.BooleanVar(value=False)
        self.prop_vars: Dict[str, tk.StringVar] = {}

        frm = ttk.Frame(self, padding=12)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, text=tr("create_dataset_path")).grid(row=0, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(frm, textvariable=self.var_path, width=56).grid(row=0, column=1, sticky="ew", pady=4)

        ttk.Label(frm, text=tr("create_dataset_type")).grid(row=1, column=0, sticky="w", padx=(0, 8), pady=4)
        type_combo = ttk.Combobox(frm, textvariable=self.var_type, values=["filesystem", "volume", "snapshot"], state="readonly", width=18)
        type_combo.grid(row=1, column=1, sticky="w", pady=4)

        self.volsize_label = ttk.Label(frm, text=tr("create_dataset_volsize"))
        self.volsize_label.grid(row=2, column=0, sticky="w", padx=(0, 8), pady=4)
        self.volsize_entry = ttk.Entry(frm, textvariable=self.var_volsize, width=24)
        self.volsize_entry.grid(row=2, column=1, sticky="w", pady=4)

        self.blocksize_label = ttk.Label(frm, text=tr("create_dataset_blocksize"))
        self.blocksize_label.grid(row=3, column=0, sticky="w", padx=(0, 8), pady=4)
        self.blocksize_entry = ttk.Entry(frm, textvariable=self.var_blocksize, width=24)
        self.blocksize_entry.grid(row=3, column=1, sticky="w", pady=4)

        opts = ttk.Frame(frm)
        opts.grid(row=4, column=0, columnspan=2, sticky="w", pady=(4, 2))
        self.parents_chk = ttk.Checkbutton(opts, text=tr("create_dataset_opt_parents"), variable=self.var_parents)
        self.parents_chk.grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.sparse_chk = ttk.Checkbutton(opts, text=tr("create_dataset_opt_sparse"), variable=self.var_sparse)
        self.sparse_chk.grid(row=0, column=1, sticky="w", padx=(0, 8))
        self.nomount_chk = ttk.Checkbutton(opts, text=tr("create_dataset_opt_nomount"), variable=self.var_nomount)
        self.nomount_chk.grid(row=0, column=2, sticky="w")
        self.snapshot_recursive_chk = ttk.Checkbutton(
            opts,
            text=tr("create_dataset_snapshot_recursive"),
            variable=self.var_snapshot_recursive,
        )
        self.snapshot_recursive_chk.grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 0))

        props_frame = ttk.LabelFrame(frm, text=tr("create_dataset_properties"))
        props_frame.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(6, 2))
        props_frame.columnconfigure(1, weight=1)
        props_frame.columnconfigure(3, weight=1)

        prop_specs: List[Tuple[str, str, List[str]]] = [
            ("mountpoint", "entry", []),
            ("canmount", "combo", ["on", "off", "noauto"]),
            ("compression", "combo", ["off", "on", "lz4", "gzip", "zstd", "zle"]),
            ("atime", "combo", ["on", "off"]),
            ("relatime", "combo", ["on", "off"]),
            ("xattr", "combo", ["on", "off", "sa"]),
            ("acltype", "combo", ["off", "posix", "nfsv4"]),
            ("aclinherit", "combo", ["discard", "noallow", "restricted", "passthrough", "passthrough-x"]),
            ("recordsize", "entry", []),
            ("volblocksize", "entry", []),
            ("quota", "entry", []),
            ("reservation", "entry", []),
            ("refquota", "entry", []),
            ("refreservation", "entry", []),
            ("copies", "combo", ["1", "2", "3"]),
            ("checksum", "combo", ["on", "off", "fletcher2", "fletcher4", "sha256", "sha512", "skein", "edonr"]),
            ("sync", "combo", ["standard", "always", "disabled"]),
            ("logbias", "combo", ["latency", "throughput"]),
            ("primarycache", "combo", ["all", "none", "metadata"]),
            ("secondarycache", "combo", ["all", "none", "metadata"]),
            ("dedup", "combo", ["off", "on", "verify", "sha256", "sha512", "skein"]),
            ("encryption", "combo", ["off", "on", "aes-128-ccm", "aes-192-ccm", "aes-256-ccm", "aes-128-gcm", "aes-192-gcm", "aes-256-gcm"]),
            ("keyformat", "combo", ["passphrase", "raw", "hex"]),
            ("keylocation", "entry", []),
            ("normalization", "combo", ["none", "formC", "formD", "formKC", "formKD"]),
            ("casesensitivity", "combo", ["sensitive", "insensitive", "mixed"]),
            ("utf8only", "combo", ["on", "off"]),
        ]

        for idx, (prop_name, kind, values) in enumerate(prop_specs):
            var = tk.StringVar()
            self.prop_vars[prop_name] = var
            r = idx // 2
            c = (idx % 2) * 2
            ttk.Label(props_frame, text=prop_name).grid(row=r, column=c, sticky="w", padx=(6, 6), pady=3)
            if kind == "combo":
                w = ttk.Combobox(props_frame, textvariable=var, values=values, state="readonly", width=18)
            else:
                w = ttk.Entry(props_frame, textvariable=var, width=20)
            w.grid(row=r, column=c + 1, sticky="ew", padx=(0, 8), pady=3)

        ttk.Label(frm, text=tr("create_dataset_extra_args")).grid(row=6, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(frm, textvariable=self.var_extra, width=56).grid(row=6, column=1, sticky="ew", pady=4)

        actions = ttk.Frame(frm)
        actions.grid(row=7, column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(actions, text=tr("cancel"), command=self.destroy).grid(row=0, column=0, padx=4)
        ttk.Button(actions, text=tr("create_dataset_btn"), command=self._accept).grid(row=0, column=1, padx=4)

        def _toggle_type(*_args: Any) -> None:
            sel_type = self.var_type.get().strip().lower()
            is_volume = sel_type == "volume"
            is_snapshot = sel_type == "snapshot"
            if is_volume:
                self.volsize_label.grid()
                self.volsize_entry.grid()
                self.volsize_entry.configure(state="normal")
            else:
                self.volsize_label.grid_remove()
                self.volsize_entry.grid_remove()
                self.var_volsize.set("")
            if is_snapshot:
                self.blocksize_label.grid_remove()
                self.blocksize_entry.grid_remove()
                self.var_blocksize.set("")
                self.parents_chk.grid_remove()
                self.sparse_chk.grid_remove()
                self.nomount_chk.grid_remove()
                self.var_parents.set(False)
                self.var_sparse.set(False)
                self.var_nomount.set(False)
                self.snapshot_recursive_chk.grid()
                self.snapshot_recursive_chk.configure(state="normal")
            else:
                self.blocksize_label.grid()
                self.blocksize_entry.grid()
                self.parents_chk.grid()
                self.sparse_chk.grid()
                self.nomount_chk.grid()
                self.snapshot_recursive_chk.grid_remove()
                self.snapshot_recursive_chk.configure(state="disabled")
            if is_snapshot:
                cur = self.var_path.get().strip()
                if "@" not in cur:
                    base = self.base_dataset or cur
                    base = base.split("@", 1)[0]
                    if base:
                        self.var_path.set(f"{base}@snap")
            else:
                cur = self.var_path.get().strip()
                if "@" in cur:
                    self.var_path.set(cur.split("@", 1)[0])
                self.var_snapshot_recursive.set(False)

        self.var_type.trace_add("write", _toggle_type)
        _toggle_type()

        self.wait_visibility()
        try:
            self.grab_set()
        except tk.TclError:
            self.after(10, lambda: self.grab_set() if self.winfo_exists() else None)
        self.focus_set()

    def _accept(self) -> None:
        path = self.var_path.get().strip()
        ds_type = self.var_type.get().strip().lower()
        volsize = self.var_volsize.get().strip()
        if not path:
            messagebox.showerror(tr("validation_title"), tr("create_dataset_path_required"), parent=self)
            return
        if ds_type == "snapshot" and "@" not in path:
            messagebox.showerror(tr("validation_title"), tr("create_dataset_snapshot_required"), parent=self)
            return
        if ds_type == "volume" and not volsize:
            messagebox.showerror(tr("validation_title"), tr("create_dataset_volsize_required"), parent=self)
            return
        props: List[str] = []
        for prop_name, var in self.prop_vars.items():
            val = var.get().strip()
            if val:
                props.append(f"{prop_name}={val}")
        self.result = {
            "dataset_path": path,
            "ds_type": ds_type,
            "volsize": volsize,
            "blocksize": self.var_blocksize.get().strip(),
            "parents": self.var_parents.get(),
            "sparse": self.var_sparse.get(),
            "nomount": self.var_nomount.get(),
            "snapshot_recursive": self.var_snapshot_recursive.get(),
            "properties": props,
            "extra_args": self.var_extra.get().strip(),
        }
        self.destroy()


class ModifyDatasetDialog(tk.Toplevel):
    def __init__(self, master: tk.Misc, dataset_path: str, properties: List[Dict[str, str]]) -> None:
        super().__init__(master)
        self.dataset_path = dataset_path
        self.title(tr("modify_dataset_title"))
        self.resizable(True, True)
        self.transient(master)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)
        self.result: Optional[Dict[str, Any]] = None

        ttk.Label(self, text=trf("modify_dataset_target", dataset=dataset_path), padding=(10, 8)).grid(row=0, column=0, sticky="w")
        dataset_type = ""
        for prop in properties:
            if (prop.get("property", "") or "").strip().lower() == "type":
                dataset_type = (prop.get("value", "") or "").strip().lower()
                break
        type_label = tr("dataset_type_unknown")
        if dataset_type == "filesystem":
            type_label = tr("dataset_type_filesystem")
        elif dataset_type == "volume":
            type_label = tr("dataset_type_volume")
        elif dataset_type == "snapshot":
            type_label = tr("dataset_type_snapshot")
        ttk.Label(self, text=trf("modify_dataset_detected_type", dtype=type_label), padding=(10, 0, 10, 8), foreground=UI_MUTED).grid(
            row=1, column=0, sticky="w"
        )

        rename_row = ttk.Frame(self, padding=(10, 0, 10, 6))
        rename_row.grid(row=2, column=0, sticky="ew")
        rename_row.columnconfigure(1, weight=1)
        ttk.Label(rename_row, text=tr("modify_dataset_rename_label")).grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.rename_var = tk.StringVar(value=dataset_path)
        ttk.Entry(rename_row, textvariable=self.rename_var).grid(row=0, column=1, sticky="ew")

        table = ttk.Frame(self, padding=(10, 0, 10, 0))
        table.grid(row=3, column=0, sticky="nsew")
        table.columnconfigure(0, weight=1)
        table.rowconfigure(1, weight=1)

        columns = [
            (tr("col_property"), 220, "w"),
            (tr("col_value"), 240, "w"),
            (tr("modify_dataset_new_value"), 240, "w"),
            (tr("col_source"), 120, "w"),
        ]

        header = tk.Frame(table, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER)
        header.grid(row=0, column=0, sticky="ew")
        for idx, (title, width, anchor) in enumerate(columns):
            lbl = tk.Label(header, text=title, bg=UI_PANEL_BG, fg=UI_TEXT, anchor=anchor, padx=6, pady=4, font=("TkDefaultFont", 10, "bold"))
            lbl.grid(row=0, column=idx, sticky="nsew")
            header.grid_columnconfigure(idx, minsize=width, weight=0)

        canvas = tk.Canvas(table, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER)
        canvas.grid(row=1, column=0, sticky="nsew")
        ybar = ttk.Scrollbar(table, orient="vertical", command=canvas.yview)
        ybar.grid(row=1, column=1, sticky="ns")
        canvas.configure(yscrollcommand=ybar.set)
        rows = ttk.Frame(canvas)
        canvas.create_window((0, 0), window=rows, anchor="nw")

        def _on_mousewheel(event: Any) -> str:
            step = 0
            if getattr(event, "delta", 0):
                step = -1 if event.delta > 0 else 1
            else:
                num = int(getattr(event, "num", 0) or 0)
                if num == 4:
                    step = -1
                elif num == 5:
                    step = 1
            if step:
                canvas.yview_scroll(step, "units")
            return "break"

        def _bind_mousewheel(widget: tk.Widget) -> None:
            widget.bind("<MouseWheel>", _on_mousewheel)
            widget.bind("<Button-4>", _on_mousewheel)
            widget.bind("<Button-5>", _on_mousewheel)

        _bind_mousewheel(canvas)
        _bind_mousewheel(rows)
        _bind_mousewheel(header)

        def _sync_scroll(_event: Any = None) -> None:
            canvas.configure(scrollregion=canvas.bbox("all"))

        rows.bind("<Configure>", _sync_scroll)

        self.entry_vars: Dict[str, tk.StringVar] = {}
        self.original_values: Dict[str, str] = {}
        row_idx = 0
        for prop in sorted(properties, key=lambda p: p.get("property", "")):
            name = (prop.get("property") or "").strip()
            if not name:
                continue
            value = prop.get("value", "")
            source = prop.get("source", "")
            readonly = (prop.get("readonly") or "").strip().lower()
            editable = is_dataset_property_editable(name, dataset_type, source, readonly)
            if not editable:
                continue

            bg = UI_PANEL_BG if row_idx % 2 == 0 else "#f8fbfd"
            row = tk.Frame(rows, bg=bg)
            row.grid(row=row_idx * 2, column=0, sticky="ew")
            _bind_mousewheel(row)

            tk.Label(row, text=name, bg=bg, fg=UI_TEXT, anchor="w", padx=6, pady=3).grid(row=0, column=0, sticky="nsew")
            tk.Label(row, text=value, bg=bg, fg=UI_TEXT, anchor="w", padx=6, pady=3).grid(row=0, column=1, sticky="nsew")
            var = tk.StringVar(value=value)
            self.entry_vars[name] = var
            self.original_values[name] = value
            entry = ttk.Entry(row, textvariable=var, width=28)
            entry.grid(row=0, column=2, sticky="nsew", padx=(4, 4), pady=2)
            _bind_mousewheel(entry)

            tk.Label(row, text=source, bg=bg, fg=UI_TEXT, anchor="w", padx=6, pady=3).grid(row=0, column=3, sticky="nsew")

            for col_idx, (_, width, _anchor) in enumerate(columns):
                row.grid_columnconfigure(col_idx, minsize=width, weight=0)
            sep = tk.Frame(rows, bg=UI_BORDER, height=1)
            sep.grid(row=row_idx * 2 + 1, column=0, sticky="ew")
            row_idx += 1

        actions = ttk.Frame(self, padding=(10, 10))
        actions.grid(row=4, column=0, sticky="e")
        ttk.Button(actions, text=tr("cancel"), command=self.destroy).grid(row=0, column=0, padx=4)
        ttk.Button(actions, text=tr("modify_dataset_apply"), command=self._accept).grid(row=0, column=1, padx=4)

        self.wait_visibility()
        try:
            self.grab_set()
        except tk.TclError:
            self.after(10, lambda: self.grab_set() if self.winfo_exists() else None)
        self.focus_set()

    def _accept(self) -> None:
        changes: Dict[str, str] = {}
        for prop, var in self.entry_vars.items():
            old = self.original_values.get(prop, "")
            new = var.get()
            if new != old:
                changes[prop] = new
        rename_to = (self.rename_var.get() or "").strip()
        if rename_to == self.dataset_path:
            rename_to = ""
        self.result = {"changes": changes, "rename_to": rename_to}
        self.destroy()


class MultiSelectDialog(tk.Toplevel):
    def __init__(
        self,
        master: tk.Misc,
        title: str,
        prompt: str,
        items: List[str],
        preselect_all: bool = True,
    ) -> None:
        super().__init__(master)
        self.title(title)
        self.transient(master)
        self.resizable(True, True)
        self.minsize(520, 380)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)
        self.result: Optional[List[str]] = None

        ttk.Label(self, text=prompt, padding=(10, 10, 10, 6), justify="left", wraplength=640).grid(
            row=0, column=0, sticky="ew"
        )
        body = ttk.Frame(self, padding=(10, 0, 10, 8))
        body.grid(row=1, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.rowconfigure(0, weight=1)

        self.listbox = tk.Listbox(body, selectmode="extended", exportselection=False)
        self.listbox.grid(row=0, column=0, sticky="nsew")
        ybar = ttk.Scrollbar(body, orient="vertical", command=self.listbox.yview)
        ybar.grid(row=0, column=1, sticky="ns")
        self.listbox.configure(yscrollcommand=ybar.set)
        for item in items:
            self.listbox.insert(tk.END, item)
        if preselect_all:
            self.listbox.selection_set(0, tk.END)

        actions = ttk.Frame(self, padding=(10, 0, 10, 10))
        actions.grid(row=2, column=0, sticky="ew")
        actions.columnconfigure(0, weight=1)
        left = ttk.Frame(actions)
        left.grid(row=0, column=0, sticky="w")
        ttk.Button(left, text="Seleccionar todo", command=self._select_all).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(left, text="Seleccionar ninguno", command=self._select_none).grid(row=0, column=1)
        right = ttk.Frame(actions)
        right.grid(row=0, column=1, sticky="e")
        ttk.Button(right, text=tr("cancel"), command=self.destroy).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(right, text="Aceptar", command=self._accept).grid(row=0, column=1)

        self.bind("<Escape>", lambda _e: self.destroy())
        self.bind("<Return>", lambda _e: self._accept())
        self.wait_visibility()
        try:
            self.grab_set()
        except tk.TclError:
            self.after(10, lambda: self.grab_set() if self.winfo_exists() else None)
        self.focus_set()

    def _select_all(self) -> None:
        self.listbox.selection_set(0, tk.END)

    def _select_none(self) -> None:
        self.listbox.selection_clear(0, tk.END)

    def _accept(self) -> None:
        picked = [self.listbox.get(i) for i in self.listbox.curselection()]
        self.result = picked
        self.destroy()


class App(tk.Tk):
    def __init__(self, store: ConnectionStore, startup_sudo_ok: Optional[bool] = None) -> None:
        super().__init__()
        global SSH_LOG_HOOK, SSH_BUSY_HOOK
        self.title(tr("app_title"))
        self.geometry("1200x700")
        _apply_window_icon(self)
        self.startup_sudo_ok = startup_sudo_ok

        self.store = store
        self.states: Dict[str, ConnectionState] = {}
        self.selected_conn_id: Optional[str] = None
        self.level_running = False
        self.last_selected_dataset_side = "origin"
        self.ssh_busy_count = 0
        self.ssh_actions_locked = False
        self._dataset_proc_lock = threading.Lock()
        self._active_dataset_proc: Optional[subprocess.Popen[str]] = None
        self._active_dataset_action: str = ""
        self._dataset_cancel_requested = False
        self._active_dataset_cancel_event: Optional[threading.Event] = None
        self.pool_props_loading_count = 0
        self._busy_cursor = "wait" if os.name == "nt" else "watch"
        self.selected_imported_pool: Optional[Tuple[str, str]] = None
        self._active_context_menu: Optional[tk.Menu] = None
        self._context_menu_global_bindings: List[Tuple[str, str]] = []
        self._context_menu_unmap_bind_id: Optional[str] = None
        self._persistent_log_lock = threading.Lock()
        self._is_closing = False
        self._app_log_level_cached = "normal"
        self._ssh_last_line_full: str = ""
        self._app_log_tipwindow: Optional[tk.Toplevel] = None
        self._app_log_tip_text: str = ""
        self._pool_status_tipwindow: Optional[tk.Toplevel] = None
        self._pool_status_tip_text: str = ""
        self.pool_properties_cache: Dict[str, List[Dict[str, str]]] = {}
        self.dataset_properties_cache: Dict[str, Dict[str, List[Dict[str, str]]]] = {}
        self.pool_status_cache: Dict[str, str] = {}
        self.pool_status_loading: set[str] = set()
        self.pool_props_loading_keys: set[str] = set()
        self.pool_status_lock = threading.Lock()
        self.dataset_snapshot_max_cols = 3
        self.dataset_snapshot_col_ids = [f"snap{i}" for i in range(1, self.dataset_snapshot_max_cols + 1)]
        self.dataset_snapshot_more_col_id = "snap_more"
        self.dataset_snapshots_by_side: Dict[str, Dict[str, List[str]]] = {"origin": {}, "dest": {}}
        self.dataset_selected_snapshot_by_side: Dict[str, Dict[str, str]] = {"origin": {}, "dest": {}}
        self._snapshot_cell_editor: Optional[ttk.Combobox] = None
        self._snapshot_cell_editor_side: str = ""
        self._snapshot_cell_editor_dataset: str = ""
        self._snapshot_cell_popup: Optional[tk.Toplevel] = None
        self._suspend_dataset_tree_select = False
        base_size = int(tkfont.nametofont("TkDefaultFont").actual("size") or 9)
        self.snapshot_font_normal = ("TkDefaultFont", base_size)
        self._last_dataset_props_sig: str = ""
        self._dataset_props_ctx: Optional[Tuple[str, str, str, str]] = None  # (side, conn_id, pool, dataset)
        self._dataset_props_edit_vars: Dict[str, tk.StringVar] = {}
        self._dataset_props_inherit_vars: Dict[str, tk.BooleanVar] = {}
        self._dataset_props_original_values: Dict[str, str] = {}
        self._dataset_props_load_token: int = 0

        self._apply_theme()
        self._build_ui()
        SSH_LOG_HOOK = self._ssh_log
        SSH_BUSY_HOOK = self._on_ssh_busy_delta
        self.protocol("WM_DELETE_WINDOW", self._on_main_close)
        self._load_connections_list()
        self.after(30, self._ensure_main_window_visible)
        self.after(100, self.refresh_all_connections)

    def _ensure_main_window_visible(self) -> None:
        try:
            self.deiconify()
            self.lift()
            self.focus_force()
        except Exception:
            pass

    def _dataset_action_running(self) -> bool:
        if self.level_running:
            return True
        with self._dataset_proc_lock:
            proc = self._active_dataset_proc
            if proc is None:
                return False
            return proc.poll() is None

    def _on_main_close(self) -> None:
        if self._dataset_action_running():
            self._app_log("normal", tr("log_close_blocked_dataset_action"))
            messagebox.showwarning(
                tr("app_title"),
                tr("close_blocked_dataset_action"),
                parent=self,
            )
            return
        self._is_closing = True
        global SSH_LOG_HOOK, SSH_BUSY_HOOK
        SSH_LOG_HOOK = None
        SSH_BUSY_HOOK = None
        self.destroy()

    def _apply_theme(self) -> None:
        self.configure(bg=UI_BG)
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except Exception:
            pass

        style.configure(".", background=UI_BG, foreground=UI_TEXT)
        style.configure("TFrame", background=UI_BG)
        style.configure("TLabel", background=UI_BG, foreground=UI_TEXT)
        style.configure("TLabelframe", background=UI_BG, bordercolor=UI_BORDER)
        style.configure("TLabelframe.Label", background=UI_BG, foreground=UI_ACCENT)
        style.configure(
            "TButton",
            background=UI_PANEL_BG,
            foreground=UI_TEXT,
            bordercolor=UI_BORDER,
            focusthickness=1,
            focuscolor=UI_SELECTION,
            padding=(8, 4),
        )
        style.map("TButton", background=[("active", "#eaf2f7"), ("disabled", "#edf1f4")], foreground=[("disabled", UI_MUTED)])
        style.configure("TMenubutton", background=UI_PANEL_BG, foreground=UI_TEXT, bordercolor=UI_BORDER, padding=(8, 4))
        style.map("TMenubutton", background=[("active", "#eaf2f7"), ("disabled", "#edf1f4")], foreground=[("disabled", UI_MUTED)])
        style.configure("TNotebook", background=UI_BG, borderwidth=0)
        base_font = ("TkDefaultFont", 9)
        selected_font = ("TkDefaultFont", 10, "bold")
        style.configure("TNotebook.Tab", background="#e9eef2", foreground=UI_TEXT, padding=(8, 3), font=base_font)
        style.map(
            "TNotebook.Tab",
            background=[("selected", UI_PANEL_BG)],
            foreground=[("selected", UI_ACCENT)],
            padding=[("selected", (10, 5)), ("!selected", (8, 3))],
            font=[("selected", selected_font), ("!selected", base_font)],
        )
        # Notebook compacto para el bloque de logs (Aplicacion/Ejecucion SSH).
        log_tab_font = ("TkDefaultFont", 8)
        style.configure("Log.TNotebook", background=UI_BG, borderwidth=0, tabmargins=(0, 0, 0, 0))
        style.configure("Log.TNotebook.Tab", background="#e9eef2", foreground=UI_TEXT, padding=(4, 1), font=log_tab_font)
        style.map(
            "Log.TNotebook.Tab",
            background=[("selected", UI_PANEL_BG)],
            foreground=[("selected", UI_ACCENT)],
            padding=[("selected", (4, 1)), ("!selected", (4, 1))],
            font=[("selected", log_tab_font), ("!selected", log_tab_font)],
        )
        tree_base = tkfont.nametofont("TkDefaultFont")
        tree_family = str(tree_base.actual("family") or "TkDefaultFont")
        tree_size = int(tree_base.actual("size") or 9)
        tree_font_normal = (tree_family, tree_size)
        tree_font_bold = (tree_family, tree_size, "bold")
        style.configure(
            "Treeview",
            background=UI_PANEL_BG,
            fieldbackground=UI_PANEL_BG,
            foreground=UI_TEXT,
            bordercolor=UI_BORDER,
            font=tree_font_normal,
        )
        style.configure("Treeview.Heading", background="#e5edf3", foreground=UI_TEXT)
        style.map(
            "Treeview",
            background=[("selected", UI_SELECTION)],
            foreground=[("selected", UI_TEXT)],
            font=[("selected", tree_font_bold), ("!selected", tree_font_normal)],
        )
        style.configure("TCombobox", fieldbackground=UI_PANEL_BG, background=UI_PANEL_BG, foreground=UI_TEXT)
        style.configure("TEntry", fieldbackground=UI_PANEL_BG, foreground=UI_TEXT)
        style.configure("TCheckbutton", background=UI_BG, foreground=UI_TEXT)
        style.configure("TPanedwindow", background=UI_BG)
        style.configure("Sash", background=UI_BORDER)

    def _build_ui(self) -> None:
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        if self.startup_sudo_ok is True:
            initial_status = tr("status_sudo_ok")
        elif self.startup_sudo_ok is False:
            initial_status = tr("status_no_sudo")
        else:
            initial_status = tr("status_ready")
        self.status_var = tk.StringVar(value=initial_status)
        self.ssh_last_line_var = tk.StringVar(value="")
        self.dataset_action_target_var = tk.StringVar(value=trf("datasets_selected_target", dataset=tr("label_none")))
        self.transfer_origin_target_var = tk.StringVar(value=f"{tr('datasets_origin')}: {tr('label_none')}")
        self.transfer_dest_target_var = tk.StringVar(value=f"{tr('datasets_dest')}: {tr('label_none')}")

        self.priv_var = tk.StringVar(value=tr("priv_unknown"))

        main_layout = ttk.Frame(self)
        main_layout.grid(row=0, column=0, sticky="nsew")
        main_layout.columnconfigure(0, weight=1)
        # El log queda con altura natural (sin crecer al redimensionar verticalmente).
        main_layout.rowconfigure(0, weight=1)
        main_layout.rowconfigure(1, weight=0)

        top_container = ttk.Frame(main_layout)
        top_container.grid(row=0, column=0, sticky="nsew")
        top_container.columnconfigure(0, weight=0)
        top_container.columnconfigure(1, weight=1)
        top_container.rowconfigure(0, weight=1)

        left = ttk.Frame(top_container, padding=(10, 6, 6, 10))
        left.grid(row=0, column=0, sticky="nsew")
        left.columnconfigure(0, weight=1)
        left.rowconfigure(0, weight=1)

        self.left_tabs = ttk.Notebook(left)
        self.left_tabs.grid(row=0, column=0, sticky="nsew")
        self.left_tabs.bind("<<NotebookTabChanged>>", self._on_left_tab_changed)

        self.tab_connections = ttk.Frame(self.left_tabs, padding=8)
        self.tab_connections.columnconfigure(0, weight=1)
        self.tab_connections.rowconfigure(1, weight=1)
        self.left_tabs.add(self.tab_connections, text=tr("tab_connections"))

        ttk.Label(self.tab_connections, text=tr("tab_connections")).grid(row=0, column=0, sticky="w")

        conn_frame = ttk.Frame(self.tab_connections)
        conn_frame.grid(row=1, column=0, sticky="nsew", pady=(4, 8))
        conn_frame.columnconfigure(0, weight=1)
        conn_frame.rowconfigure(0, weight=1)
        self.conn_list = tk.Listbox(
            conn_frame,
            height=25,
            width=35,
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            selectbackground=UI_SELECTION,
            selectforeground=UI_TEXT,
            highlightthickness=1,
            highlightbackground=UI_BORDER,
            highlightcolor=UI_ACCENT,
        )
        self.conn_list.grid(row=0, column=0, sticky="nsew")
        conn_y = ttk.Scrollbar(conn_frame, orient="vertical", command=self.conn_list.yview)
        conn_y.grid(row=0, column=1, sticky="ns")
        conn_x = ttk.Scrollbar(conn_frame, orient="horizontal", command=self.conn_list.xview)
        conn_x.grid(row=1, column=0, sticky="ew")

        def _auto_yset(first: str, last: str) -> None:
            conn_y.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                conn_y.grid()
            else:
                conn_y.grid_remove()

        def _auto_xset(first: str, last: str) -> None:
            conn_x.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                conn_x.grid()
            else:
                conn_x.grid_remove()

        self.conn_list.configure(yscrollcommand=_auto_yset, xscrollcommand=_auto_xset)
        conn_y.grid_remove()
        conn_x.grid_remove()
        self.conn_list.bind("<<ListboxSelect>>", self.on_select_connection)
        self.conn_list.bind("<Double-Button-1>", self.on_double_click_connection)
        self.conn_list.bind("<Button-3>", self._on_connection_context)
        self.conn_list.bind("<Button-2>", self._on_connection_context)
        self.conn_list.bind("<Control-Button-1>", self._on_connection_context)

        conn_buttons = ttk.Frame(self.tab_connections)
        conn_buttons.grid(row=2, column=0, sticky="ew")
        conn_buttons.columnconfigure(0, weight=1)
        self.new_conn_btn = ttk.Button(conn_buttons, text=tr("action_new"), command=self.add_connection)
        self.new_conn_btn.grid(row=0, column=0, sticky="ew")
        self.refresh_all_btn = ttk.Button(conn_buttons, text=tr("action_refresh_all"), command=self.refresh_all_connections)
        self.refresh_all_btn.grid(row=1, column=0, sticky="ew", pady=(4, 0))

        self.conn_context_menu = tk.Menu(
            self,
            tearoff=False,
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            activebackground=UI_SELECTION,
            activeforeground=UI_TEXT,
        )
        self.conn_context_menu.add_command(label=tr("action_edit"), command=self.edit_connection)
        self.conn_context_menu.add_command(label=tr("action_delete"), command=self.delete_connection)
        self.conn_context_menu.add_separator()
        self.conn_context_menu.add_command(label=tr("action_refresh"), command=self.refresh_selected)

        self.tab_datasets = ttk.Frame(self.left_tabs, padding=8)
        self.tab_datasets.columnconfigure(0, weight=1)
        self.left_tabs.add(self.tab_datasets, text=tr("tab_datasets"))
        # Acciones de gestion solo via menu contextual (sin caja/botones visibles).
        hidden_actions = ttk.Frame(self.tab_datasets)
        self.create_btn = ttk.Button(hidden_actions, text=tr("create_dataset_btn"), command=self._create_dataset)
        self.create_btn.configure(state="disabled")
        ToolTip(self.create_btn, tr("create_dataset_tooltip"))

        self.modify_btn = ttk.Button(hidden_actions, text=tr("modify_dataset_btn"), command=self._modify_dataset)
        self.modify_btn.configure(state="disabled")
        ToolTip(self.modify_btn, tr("modify_dataset_tooltip"))

        self.delete_dataset_btn = ttk.Button(hidden_actions, text=tr("delete_dataset_btn"), command=self._delete_dataset)
        self.delete_dataset_btn.configure(state="disabled")
        ToolTip(self.delete_dataset_btn, tr("delete_dataset_tooltip"))
        self.datasets_context_menu = tk.Menu(
            self,
            tearoff=False,
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            activebackground=UI_SELECTION,
            activeforeground=UI_TEXT,
        )
        self._dataset_context_side = "origin"
        self.datasets_context_menu.add_command(label=tr("create_dataset_btn"), command=self._create_dataset)
        self.datasets_context_menu.add_command(label=tr("modify_dataset_btn"), command=self._modify_dataset)
        self.datasets_context_menu.add_command(label=tr("delete_dataset_btn"), command=self._delete_dataset)
        self.datasets_context_menu.add_separator()
        self.datasets_context_menu.add_command(label=tr("action_mount"), command=lambda: self._run_dataset_mount_action(self._dataset_context_side, True))
        self.datasets_context_menu.add_command(label=tr("action_umount"), command=lambda: self._run_dataset_mount_action(self._dataset_context_side, False))

        transfer_box = ttk.LabelFrame(self.tab_datasets, text=tr("datasets_box_transfer"), padding=(8, 6))
        transfer_box.grid(row=0, column=0, sticky="ew", pady=(0, 0))
        transfer_box.columnconfigure(0, weight=1)

        self.copy_btn = ttk.Button(transfer_box, text=tr("copy_snapshot_btn"), command=self._copy_snapshot_to_dataset)
        self.copy_btn.grid(row=0, column=0, sticky="ew")
        self.copy_btn.configure(state="disabled")
        ToolTip(self.copy_btn, tr("copy_snapshot_tooltip"))

        self.level_btn = ttk.Button(transfer_box, text=tr("datasets_level_btn"), command=self._level_datasets)
        self.level_btn.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        self.level_btn.configure(state="disabled")
        ToolTip(self.level_btn, tr("datasets_level_tooltip"))

        self.sync_btn = ttk.Button(transfer_box, text=tr("datasets_sync_btn"), command=self._sync_datasets)
        self.sync_btn.grid(row=2, column=0, sticky="ew", pady=(4, 0))
        self.sync_btn.configure(state="disabled")
        ToolTip(self.sync_btn, tr("datasets_sync_tooltip"))

        breakdown_box = ttk.LabelFrame(self.tab_datasets, text="Avanzado", padding=(8, 6))
        breakdown_box.grid(row=1, column=0, sticky="ew", pady=(6, 0))
        breakdown_box.columnconfigure(0, weight=1)
        self.breakdown_selected_label = ttk.Label(
            breakdown_box,
            textvariable=self.dataset_action_target_var,
            foreground=UI_ACCENT,
            justify="left",
            wraplength=220,
            font=("TkDefaultFont", 9, "bold"),
        )
        self.breakdown_btn = ttk.Button(breakdown_box, text=tr("datasets_breakdown_btn"), command=self._breakdown_dataset_plan)
        self.breakdown_btn.grid(row=0, column=0, sticky="ew")
        self.breakdown_btn.configure(state="disabled")
        ToolTip(self.breakdown_btn, tr("datasets_breakdown_tooltip"))
        self.assemble_btn = ttk.Button(breakdown_box, text=tr("datasets_assemble_btn"), command=self._assemble_dataset_plan)
        self.assemble_btn.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        self.assemble_btn.configure(state="disabled")
        ToolTip(self.assemble_btn, tr("datasets_assemble_tooltip"))
        self.breakdown_selected_label.grid(row=2, column=0, sticky="w", pady=(6, 0))

        right = ttk.Frame(top_container, padding=(6, 6, 10, 10))
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(0, weight=1)
        # Sin barra de redimensionado: panel izquierdo de ancho fijo.
        def _init_fixed_left_width() -> None:
            base_width = max(top_container.winfo_width(), self.winfo_width(), 1200)
            current_height = max(self.winfo_height(), top_container.winfo_height(), 700)
            target_min_height = int(current_height * 1.15)
            width = max(120, int(base_width * 0.20608))
            top_container.grid_columnconfigure(0, minsize=width)
            # Mantener visible el detalle derecho (Origen/Destino/Propiedades).
            top_container.grid_columnconfigure(1, minsize=520)
            self._left_tabs_fixed_width = width
            left.configure(width=width)
            left.grid_propagate(False)
            self.left_tabs.configure(width=width)
            cur_min_w, cur_min_h = self.minsize()
            min_h = max(cur_min_h, target_min_height) if cur_min_h > 0 else target_min_height
            self.minsize(max(cur_min_w, width * 4), min_h)

        self.after_idle(_init_fixed_left_width)

        self.right_conn_detail = ttk.Frame(right)
        self.right_conn_detail.grid(row=0, column=0, sticky="nsew")
        self.right_conn_detail.columnconfigure(0, weight=1)
        self.right_conn_detail.rowconfigure(0, weight=1)

        pools_tabs = ttk.Notebook(self.right_conn_detail)
        pools_tabs.grid(row=0, column=0, sticky="nsew")

        imp_frame = ttk.Frame(pools_tabs, padding=6)
        imp_frame.columnconfigure(0, weight=1)
        imp_frame.rowconfigure(0, weight=1)
        imp_layout = ttk.Frame(imp_frame)
        imp_layout.grid(row=0, column=0, sticky="nsew")
        imp_layout.columnconfigure(0, weight=1)
        imp_layout.rowconfigure(0, weight=2)
        imp_layout.rowconfigure(1, weight=1)

        imp_top = ttk.Frame(imp_layout)
        imp_top.grid(row=0, column=0, sticky="nsew", pady=(0, 6))
        imp_top.columnconfigure(0, weight=1)
        imp_top.rowconfigure(0, weight=1)

        self.imported_table_columns: List[Tuple[str, int, str]] = [
            (tr("col_connection"), 180, "w"),
            (tr("col_pool"), 260, "w"),
            ("Accion", 120, "w"),
        ]
        self.imported_table_rows = self._build_plain_table(imp_top, self.imported_table_columns, enable_xscroll=True)

        imp_detail_tabs = ttk.Notebook(imp_layout)
        imp_detail_tabs.grid(row=1, column=0, sticky="nsew")

        imp_props = ttk.Frame(imp_detail_tabs, padding=4)
        imp_props.columnconfigure(0, weight=1)
        imp_props.rowconfigure(0, weight=1)
        self.pool_props_columns: List[Tuple[str, int, str]] = [
            (tr("col_property"), 220, "w"),
            (tr("col_value"), 320, "w"),
            (tr("col_source"), 140, "w"),
        ]
        self.pool_props_rows = self._build_plain_table(
            imp_props,
            self.pool_props_columns,
        )
        imp_detail_tabs.add(imp_props, text=tr("pool_properties_title"))

        imp_status = ttk.Frame(imp_detail_tabs, padding=4)
        imp_status.columnconfigure(0, weight=1)
        imp_status.rowconfigure(0, weight=1)
        self.pool_status_text = tk.Text(
            imp_status,
            height=8,
            state="disabled",
            wrap="none",
            font=("TkFixedFont", 9),
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            insertbackground=UI_TEXT,
            highlightthickness=1,
            highlightbackground=UI_BORDER,
            highlightcolor=UI_ACCENT,
        )
        self.pool_status_text.grid(row=0, column=0, sticky="nsew")
        pool_status_y = ttk.Scrollbar(imp_status, orient="vertical", command=self.pool_status_text.yview)
        pool_status_y.grid(row=0, column=1, sticky="ns")
        pool_status_x = ttk.Scrollbar(imp_status, orient="horizontal", command=self.pool_status_text.xview)
        pool_status_x.grid(row=1, column=0, sticky="ew")
        self.pool_status_text.configure(yscrollcommand=pool_status_y.set, xscrollcommand=pool_status_x.set)
        imp_detail_tabs.add(imp_status, text="Estado")

        avail_frame = ttk.Frame(pools_tabs, padding=6)
        avail_frame.columnconfigure(0, weight=1)
        avail_frame.rowconfigure(0, weight=1)
        self.importable_table_columns: List[Tuple[str, int, str]] = [
            (tr("col_connection"), 180, "w"),
            (tr("col_pool"), 260, "w"),
            ("Accion", 120, "w"),
        ]
        self.importable_table_rows = self._build_plain_table(avail_frame, self.importable_table_columns, enable_xscroll=True)
        pools_tabs.add(imp_frame, text=tr("pools_imported"))
        pools_tabs.add(avail_frame, text=tr("pools_importable"))

        self.right_datasets_detail = ttk.LabelFrame(right, text=tr("datasets_title"))
        self.right_datasets_detail.grid(row=0, column=0, sticky="nsew")
        self.right_datasets_detail.columnconfigure(0, weight=1)
        self.right_datasets_detail.rowconfigure(0, weight=1)

        self.origin_pool_var = tk.StringVar()
        self.origin_dataset_var = tk.StringVar()
        self.dest_pool_var = tk.StringVar()
        self.dest_dataset_var = tk.StringVar()

        self.datasets_cache: Dict[str, List[Dict[str, str]]] = {}
        self.dataset_pool_options: Dict[str, Tuple[str, str]] = {}

        datasets_main = ttk.Frame(self.right_datasets_detail)
        datasets_main.grid(row=0, column=0, sticky="nsew")
        # Propiedades a ancho fijo (sin crecer al redimensionar horizontalmente).
        datasets_main.columnconfigure(0, weight=1)
        datasets_main.columnconfigure(1, weight=0, minsize=414)
        datasets_main.rowconfigure(0, weight=1)

        datasets_row = ttk.Frame(datasets_main)
        datasets_row.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        datasets_row.columnconfigure(0, weight=1)
        datasets_row.rowconfigure(0, weight=1)
        datasets_row.rowconfigure(1, weight=1)

        origin_tree_wrap = ttk.LabelFrame(datasets_row, text=tr("datasets_origin"))
        origin_tree_wrap.grid(row=0, column=0, sticky="nsew", pady=(0, 3))
        origin_tree_wrap.columnconfigure(0, weight=1)
        origin_tree_wrap.rowconfigure(1, weight=1)
        origin_top = ttk.Frame(origin_tree_wrap)
        origin_top.grid(row=0, column=0, sticky="w", padx=(6, 6), pady=(4, 4))
        origin_top.columnconfigure(1, weight=1)
        self.origin_pool_combo = ttk.Combobox(origin_top, textvariable=self.origin_pool_var, state="readonly", width=21)
        self.origin_pool_combo.grid(row=0, column=0, sticky="w", padx=(0, 2))
        self.origin_pool_combo.bind("<<ComboboxSelected>>", self._on_origin_pool_selected)
        self.transfer_origin_label = ttk.Label(
            origin_top,
            textvariable=self.transfer_origin_target_var,
            foreground=UI_ACCENT,
            justify="left",
            wraplength=420,
            font=("TkDefaultFont", 9, "bold"),
        )
        self.transfer_origin_label.grid(row=0, column=1, sticky="w")
        self.datasets_tree_origin = ttk.Treeview(
            origin_tree_wrap,
            columns=("snapshot",),
            show="tree headings",
        )
        self.datasets_tree_origin.heading("#0", text=tr("datasets_dataset"))
        self.datasets_tree_origin.column("#0", width=320, minwidth=140, anchor="w", stretch=True)
        self.datasets_tree_origin.heading("snapshot", text="Snapshot")
        self.datasets_tree_origin.column("snapshot", width=90, minwidth=65, anchor="w", stretch=False)
        self.datasets_tree_origin.grid(row=1, column=0, sticky="nsew")
        ds_oy = ttk.Scrollbar(origin_tree_wrap, orient="vertical", command=self.datasets_tree_origin.yview)
        ds_oy.grid(row=1, column=1, sticky="ns")
        ds_ox = ttk.Scrollbar(origin_tree_wrap, orient="horizontal", command=self.datasets_tree_origin.xview)
        ds_ox.grid(row=2, column=0, sticky="ew")
        def _origin_auto_yset(first: str, last: str) -> None:
            ds_oy.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                ds_oy.grid()
            else:
                ds_oy.grid_remove()

        def _origin_auto_xset(first: str, last: str) -> None:
            ds_ox.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                ds_ox.grid()
            else:
                ds_ox.grid_remove()

        self.datasets_tree_origin.configure(yscrollcommand=_origin_auto_yset, xscrollcommand=_origin_auto_xset)
        ds_oy.grid_remove()
        ds_ox.grid_remove()
        self.datasets_tree_origin.bind("<<TreeviewSelect>>", self._on_origin_tree_selected)
        self.datasets_tree_origin.bind("<ButtonRelease-1>", lambda e: self._on_dataset_tree_click("origin", e), add="+")
        self.datasets_tree_origin.bind("<MouseWheel>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_origin.bind("<Button-4>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_origin.bind("<Button-5>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_origin.bind("<Button-3>", lambda e: self._on_dataset_tree_context("origin", e))
        self.datasets_tree_origin.bind("<Button-2>", lambda e: self._on_dataset_tree_context("origin", e))
        self.datasets_tree_origin.bind("<Control-Button-1>", lambda e: self._on_dataset_tree_context("origin", e))

        dest_tree_wrap = ttk.LabelFrame(datasets_row, text=tr("datasets_dest"))
        dest_tree_wrap.grid(row=1, column=0, sticky="nsew", pady=(3, 0))
        dest_tree_wrap.columnconfigure(0, weight=1)
        dest_tree_wrap.rowconfigure(1, weight=1)
        dest_top = ttk.Frame(dest_tree_wrap)
        dest_top.grid(row=0, column=0, sticky="w", padx=(6, 6), pady=(4, 4))
        dest_top.columnconfigure(1, weight=1)
        self.dest_pool_combo = ttk.Combobox(dest_top, textvariable=self.dest_pool_var, state="readonly", width=21)
        self.dest_pool_combo.grid(row=0, column=0, sticky="w", padx=(0, 2))
        self.dest_pool_combo.bind("<<ComboboxSelected>>", self._on_dest_pool_selected)
        self.transfer_dest_label = ttk.Label(
            dest_top,
            textvariable=self.transfer_dest_target_var,
            foreground=UI_ACCENT,
            justify="left",
            wraplength=420,
            font=("TkDefaultFont", 9, "bold"),
        )
        self.transfer_dest_label.grid(row=0, column=1, sticky="w")
        self.datasets_tree_dest = ttk.Treeview(
            dest_tree_wrap,
            columns=("snapshot",),
            show="tree headings",
        )
        self.datasets_tree_dest.heading("#0", text=tr("datasets_dataset"))
        self.datasets_tree_dest.column("#0", width=320, minwidth=140, anchor="w", stretch=True)
        self.datasets_tree_dest.heading("snapshot", text="Snapshot")
        self.datasets_tree_dest.column("snapshot", width=90, minwidth=65, anchor="w", stretch=False)
        self.datasets_tree_dest.grid(row=1, column=0, sticky="nsew")
        ds_dy = ttk.Scrollbar(dest_tree_wrap, orient="vertical", command=self.datasets_tree_dest.yview)
        ds_dy.grid(row=1, column=1, sticky="ns")
        ds_dx = ttk.Scrollbar(dest_tree_wrap, orient="horizontal", command=self.datasets_tree_dest.xview)
        ds_dx.grid(row=2, column=0, sticky="ew")
        def _dest_auto_yset(first: str, last: str) -> None:
            ds_dy.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                ds_dy.grid()
            else:
                ds_dy.grid_remove()

        def _dest_auto_xset(first: str, last: str) -> None:
            ds_dx.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                ds_dx.grid()
            else:
                ds_dx.grid_remove()

        self.datasets_tree_dest.configure(yscrollcommand=_dest_auto_yset, xscrollcommand=_dest_auto_xset)
        ds_dy.grid_remove()
        ds_dx.grid_remove()
        self.datasets_tree_dest.bind("<<TreeviewSelect>>", self._on_dest_tree_selected)
        self.datasets_tree_dest.bind("<ButtonRelease-1>", lambda e: self._on_dataset_tree_click("dest", e), add="+")
        self.datasets_tree_dest.bind("<MouseWheel>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_dest.bind("<Button-4>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_dest.bind("<Button-5>", lambda _e: self._hide_snapshot_dropdown(), add="+")
        self.datasets_tree_dest.bind("<Button-3>", lambda e: self._on_dataset_tree_context("dest", e))
        self.datasets_tree_dest.bind("<Button-2>", lambda e: self._on_dataset_tree_context("dest", e))
        self.datasets_tree_dest.bind("<Control-Button-1>", lambda e: self._on_dataset_tree_context("dest", e))

        props_row = ttk.Frame(datasets_main)
        props_row.grid(row=0, column=1, sticky="nsew")
        props_row.columnconfigure(0, weight=1)
        props_row.rowconfigure(0, weight=1)
        props_row.configure(width=414)
        props_row.grid_propagate(False)

        dataset_props = ttk.LabelFrame(props_row, text=tr("dataset_properties"))
        dataset_props.grid(row=0, column=0, sticky="nsew")
        dataset_props.columnconfigure(0, weight=1)
        dataset_props.rowconfigure(1, weight=1)
        self.dataset_props_selected_var = tk.StringVar(value=trf("datasets_selected_target", dataset=tr("label_none")))
        ttk.Label(
            dataset_props,
            textvariable=self.dataset_props_selected_var,
            anchor="w",
            justify="left",
        ).grid(row=0, column=0, sticky="ew", padx=(6, 6), pady=(4, 4))
        dataset_props_table_wrap = ttk.Frame(dataset_props)
        dataset_props_table_wrap.grid(row=1, column=0, sticky="nsew")
        dataset_props_table_wrap.columnconfigure(0, weight=1)
        dataset_props_table_wrap.rowconfigure(0, weight=1)
        dataset_props_table_wrap.grid_propagate(False)
        self.dataset_props_columns: List[Tuple[str, int, str]] = [
            (tr("col_property"), 108, "w"),
            (tr("col_value"), 280, "w"),
        ]
        self.dataset_props_rows = self._build_plain_table(
            dataset_props_table_wrap,
            self.dataset_props_columns,
            enable_xscroll=True,
        )
        dataset_props_actions = ttk.Frame(dataset_props)
        dataset_props_actions.grid(row=2, column=0, sticky="e", padx=(6, 6), pady=(4, 6))
        self.dataset_props_apply_btn = ttk.Button(
            dataset_props_actions,
            text=tr("modify_dataset_apply"),
            command=self._apply_right_panel_dataset_properties,
            state="disabled",
        )
        self.dataset_props_apply_btn.grid(row=0, column=0, sticky="e")

        log_container = ttk.Frame(main_layout)
        log_container.grid(row=1, column=0, sticky="nsew")
        log_container.columnconfigure(0, weight=1)
        log_container.rowconfigure(0, weight=1)
        log_frame = ttk.LabelFrame(log_container, text=tr("log_section"), padding=(8, 6))
        log_frame.grid(row=0, column=0, sticky="nsew", padx=10, pady=(0, 10))
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)

        log_body = ttk.Frame(log_frame)
        log_body.grid(row=0, column=0, sticky="nsew")
        log_body.columnconfigure(0, weight=1, uniform="logcols")
        log_body.columnconfigure(1, weight=2, uniform="logcols")
        log_body.rowconfigure(0, weight=1)
        def _sync_log_columns(_event: Any = None) -> None:
            try:
                width = max(3, log_body.winfo_width())
                left_w = max(160, width // 3)
                right_w = max(260, width - left_w)
                log_body.grid_columnconfigure(0, minsize=left_w)
                log_body.grid_columnconfigure(1, minsize=right_w)
            except Exception:
                pass
        log_body.bind("<Configure>", _sync_log_columns)

        # Panel izquierdo: estado (max 3 lineas) y ultima linea SSH.
        left_info = ttk.Frame(log_body)
        left_info.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        left_info.columnconfigure(0, weight=1)
        left_info.rowconfigure(1, weight=1)

        status_title = re.split(r"[:：]", tr("status_ready"), maxsplit=1)[0].strip() or tr("status_ready")
        detail_title = "Detalle"

        status_panel = ttk.LabelFrame(left_info, text=status_title, padding=(6, 6))
        status_panel.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        status_panel.columnconfigure(0, weight=1)
        self.status_label = tk.Label(
            status_panel,
            textvariable=self.status_var,
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            justify="left",
            anchor="nw",
            height=3,
            wraplength=320,
        )
        self.status_label.grid(row=0, column=0, sticky="nsew")
        self.status_label.bind("<Configure>", lambda e: self.status_label.configure(wraplength=max(80, e.width - 8)))

        ssh_last_panel = ttk.LabelFrame(left_info, text=detail_title, padding=(6, 6))
        ssh_last_panel.grid(row=1, column=0, sticky="nsew")
        ssh_last_panel.columnconfigure(0, weight=1)
        ssh_last_panel.rowconfigure(0, weight=1)
        self.ssh_last_line_text = tk.Text(
            ssh_last_panel,
            height=4,
            state="disabled",
            wrap="word",
            font=("TkDefaultFont", 9),
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            relief="flat",
            highlightthickness=1,
            highlightbackground=UI_BORDER,
            highlightcolor=UI_ACCENT,
        )
        self.ssh_last_line_text.grid(row=0, column=0, sticky="nsew")
        self.ssh_last_line_scroll = ttk.Scrollbar(ssh_last_panel, orient="vertical", command=self.ssh_last_line_text.yview)
        self.ssh_last_line_scroll.grid(row=0, column=1, sticky="ns")
        def _ssh_last_auto_yset(first: str, last: str) -> None:
            self.ssh_last_line_scroll.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                self.ssh_last_line_scroll.grid()
            else:
                self.ssh_last_line_scroll.grid_remove()
        self.ssh_last_line_text.configure(yscrollcommand=_ssh_last_auto_yset)
        self.ssh_last_line_scroll.grid_remove()
        self.ssh_last_line_text.bind("<Configure>", lambda _e: self._refresh_ssh_last_line_summary(), add="+")

        # Panel derecho: tabs de logs arriba y controles debajo.
        right_logs = ttk.Frame(log_body)
        right_logs.grid(row=0, column=1, sticky="nsew")
        right_logs.columnconfigure(0, weight=1)
        right_logs.rowconfigure(0, weight=1)

        self.logs_tabs = ttk.Notebook(right_logs, style="Log.TNotebook")
        self.logs_tabs.grid(row=0, column=0, sticky="nsew")
        self.connection_log_tabs: Dict[str, Dict[str, Any]] = {}

        app_tab = ttk.Frame(self.logs_tabs, padding=(6, 6))
        app_tab.columnconfigure(0, weight=1)
        app_tab.rowconfigure(0, weight=1)

        self.app_log_text = tk.Text(
            app_tab,
            height=8,
            state="disabled",
            wrap="none",
            font=("TkDefaultFont", 9),
            bg=UI_PANEL_BG,
            fg=UI_TEXT,
            insertbackground=UI_TEXT,
            highlightthickness=1,
            highlightbackground=UI_BORDER,
            highlightcolor=UI_ACCENT,
        )
        self.app_log_text.grid(row=0, column=0, sticky="nsew")
        app_log_y = ttk.Scrollbar(app_tab, orient="vertical", command=self.app_log_text.yview)
        app_log_y.grid(row=0, column=1, sticky="ns")
        app_log_x = ttk.Scrollbar(app_tab, orient="horizontal", command=self.app_log_text.xview)
        app_log_x.grid(row=1, column=0, sticky="ew")
        self.app_log_text.configure(yscrollcommand=app_log_y.set, xscrollcommand=app_log_x.set)
        self.app_log_text.tag_configure("warn", foreground=UI_WARNING)
        self.app_log_text.bind("<Motion>", self._on_app_log_hover)
        self.app_log_text.bind("<Leave>", self._hide_app_log_tooltip)
        self.logs_tabs.add(app_tab, text=tr("log_tab_app"))

        log_controls = ttk.Frame(right_logs)
        log_controls.grid(row=1, column=0, sticky="ew", pady=(6, 0))
        log_controls.columnconfigure(1, weight=1)
        ttk.Label(log_controls, text=tr("log_level")).grid(row=0, column=0, sticky="w")
        self.app_log_level_var = tk.StringVar(value="normal")
        self.app_log_level_combo = ttk.Combobox(
            log_controls,
            textvariable=self.app_log_level_var,
            values=["normal", "info", "debug"],
            state="readonly",
            width=10,
        )
        self.app_log_level_combo.grid(row=0, column=1, sticky="w", padx=(6, 0))
        self.app_log_level_combo.bind("<<ComboboxSelected>>", self._on_log_level_changed)
        self.log_max_lines_var = tk.StringVar(value="500")
        self.log_max_lines_combo = ttk.Combobox(
            log_controls,
            textvariable=self.log_max_lines_var,
            values=["100", "200", "500", "1000"],
            state="readonly",
            width=6,
        )
        self.log_max_lines_combo.grid(row=0, column=2, sticky="w", padx=(8, 0))
        self.log_max_lines_combo.bind("<<ComboboxSelected>>", self._on_log_max_lines_changed)
        self.log_clear_btn = ttk.Button(log_controls, text=tr("log_clear"), command=self._clear_app_log)
        self.log_clear_btn.grid(row=0, column=3, sticky="w", padx=(8, 0))
        self.log_copy_btn = ttk.Button(log_controls, text=tr("log_copy"), command=self._copy_app_log)
        self.log_copy_btn.grid(row=0, column=4, sticky="w", padx=(6, 0))
        self.cancel_dataset_btn = ttk.Button(
            log_controls,
            text=tr("cancel_operation_btn"),
            width=8,
            command=self._cancel_dataset_operation,
        )
        self.cancel_dataset_btn.grid(row=0, column=5, sticky="e", padx=(12, 0))
        self.cancel_dataset_btn.configure(state="disabled")
        self.cancel_dataset_btn.grid_remove()

        def _enforce_dataset_tab_visibility() -> None:
            try:
                self.update_idletasks()
                min_top = max(420, int(self.tab_datasets.winfo_reqheight() + 24))
                main_layout.grid_rowconfigure(0, minsize=min_top)
            except Exception:
                pass

        self.after_idle(_enforce_dataset_tab_visibility)

        self._app_log("normal", tr("log_app_started"))
        self.origin_dataset_var.trace_add("write", lambda *_a: self._update_level_button_state())
        self.dest_dataset_var.trace_add("write", lambda *_a: self._update_level_button_state())
        self._on_left_tab_changed()

    def _build_plain_table(
        self,
        parent: ttk.Frame,
        columns: List[Tuple[str, int, str]],
        enable_xscroll: bool = False,
        always_show_yscroll: bool = False,
    ) -> ttk.Frame:
        wrap = ttk.Frame(parent)
        wrap.grid(row=0, column=0, sticky="nsew")
        wrap.columnconfigure(0, weight=1)
        wrap.rowconfigure(1, weight=1)

        total_width = sum(max(0, width) for _title, width, _anchor in columns)

        if not enable_xscroll:
            header = tk.Frame(wrap, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER)
            header.grid(row=0, column=0, sticky="ew")
            for idx, (title, width, anchor) in enumerate(columns):
                lbl = tk.Label(
                    header,
                    text=title,
                    bg=UI_PANEL_BG,
                    fg=UI_TEXT,
                    anchor=anchor,
                    padx=6,
                    pady=4,
                    font=("TkDefaultFont", 10, "bold"),
                )
                lbl.grid(row=0, column=idx, sticky="nsew")
                header.grid_columnconfigure(idx, minsize=width, weight=0)

            rows_canvas = tk.Canvas(wrap, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER)
            rows_canvas.grid(row=1, column=0, sticky="nsew")
            ybar = ttk.Scrollbar(wrap, orient="vertical", command=rows_canvas.yview)
            ybar.grid(row=1, column=1, sticky="ns")
            def _auto_yset(first: str, last: str) -> None:
                ybar.set(first, last)
                if always_show_yscroll:
                    ybar.grid()
                    return
                try:
                    need = not (float(first) <= 0.0 and float(last) >= 1.0)
                except Exception:
                    need = True
                if need:
                    ybar.grid()
                else:
                    ybar.grid_remove()

            rows_canvas.configure(yscrollcommand=_auto_yset)
            if always_show_yscroll:
                ybar.grid()
            else:
                ybar.grid_remove()

            rows = ttk.Frame(rows_canvas)
            rows_canvas.create_window((0, 0), window=rows, anchor="nw")
            rows._scroll_canvas = rows_canvas  # type: ignore[attr-defined]

            def _sync_scroll(_event: Any = None) -> None:
                rows_canvas.configure(scrollregion=rows_canvas.bbox("all"))

            rows.bind("<Configure>", _sync_scroll)
            return rows

        header_canvas = tk.Canvas(wrap, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER, height=30)
        header_canvas.grid(row=0, column=0, sticky="ew")
        header = tk.Frame(header_canvas, bg=UI_PANEL_BG)
        header_window = header_canvas.create_window((0, 0), window=header, anchor="nw", width=total_width)
        for idx, (title, width, anchor) in enumerate(columns):
            lbl = tk.Label(
                header,
                text=title,
                bg=UI_PANEL_BG,
                fg=UI_TEXT,
                anchor=anchor,
                padx=6,
                pady=4,
                font=("TkDefaultFont", 10, "bold"),
            )
            lbl.grid(row=0, column=idx, sticky="nsew")
            header.grid_columnconfigure(idx, minsize=width, weight=0)

        rows_canvas = tk.Canvas(wrap, bg=UI_PANEL_BG, highlightthickness=1, highlightbackground=UI_BORDER)
        rows_canvas.grid(row=1, column=0, sticky="nsew")
        ybar = ttk.Scrollbar(wrap, orient="vertical", command=rows_canvas.yview)
        ybar.grid(row=1, column=1, sticky="ns")
        def _auto_yset(first: str, last: str) -> None:
            ybar.set(first, last)
            if always_show_yscroll:
                ybar.grid()
                return
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                ybar.grid()
            else:
                ybar.grid_remove()

        rows_canvas.configure(yscrollcommand=_auto_yset)
        if always_show_yscroll:
            ybar.grid()
        else:
            ybar.grid_remove()

        rows = ttk.Frame(rows_canvas)
        rows_window = rows_canvas.create_window((0, 0), window=rows, anchor="nw", width=total_width)
        rows._scroll_canvas = rows_canvas  # type: ignore[attr-defined]

        xbar = ttk.Scrollbar(wrap, orient="horizontal")
        xbar.grid(row=2, column=0, sticky="ew")

        def _xview(*args: Any) -> None:
            rows_canvas.xview(*args)
            header_canvas.xview(*args)

        xbar.configure(command=_xview)
        def _auto_xset(first: str, last: str) -> None:
            xbar.set(first, last)
            try:
                need = not (float(first) <= 0.0 and float(last) >= 1.0)
            except Exception:
                need = True
            if need:
                xbar.grid()
            else:
                xbar.grid_remove()

        rows_canvas.configure(xscrollcommand=_auto_xset)
        header_canvas.configure(xscrollcommand=_auto_xset)
        xbar.grid_remove()

        def _sync_scroll(_event: Any = None) -> None:
            rows_canvas.itemconfigure(rows_window, width=total_width)
            header_canvas.itemconfigure(header_window, width=total_width)
            rows_canvas.configure(scrollregion=(0, 0, total_width, max(rows.winfo_reqheight(), rows_canvas.winfo_height())))
            header_canvas.configure(scrollregion=(0, 0, total_width, max(header.winfo_reqheight(), header_canvas.winfo_height())))

        rows.bind("<Configure>", _sync_scroll)
        header.bind("<Configure>", _sync_scroll)
        _sync_scroll()
        return rows

    def _on_table_mousewheel(self, event: Any, canvas: tk.Canvas) -> None:
        # Windows/macOS: MouseWheel con delta. Linux/X11: Button-4/5.
        if getattr(event, "num", None) == 4:
            canvas.yview_scroll(-1, "units")
            return
        if getattr(event, "num", None) == 5:
            canvas.yview_scroll(1, "units")
            return
        delta = getattr(event, "delta", 0)
        if delta:
            step = -1 if delta > 0 else 1
            canvas.yview_scroll(step, "units")

    def _clear_plain_table(self, rows_frame: ttk.Frame) -> None:
        for child in rows_frame.winfo_children():
            child.destroy()

    def _add_plain_row(
        self,
        rows_frame: ttk.Frame,
        row_index: int,
        columns: List[Tuple[str, int, str]],
        values: List[str],
        action_col: Optional[int] = None,
        action_callback: Optional[Callable[[], None]] = None,
        action_color: Optional[str] = None,
        on_row_click: Optional[Callable[[], None]] = None,
        on_row_context: Optional[Callable[[Any], None]] = None,
        on_cell_hover: Optional[Callable[[int, str, Any], None]] = None,
        on_cell_leave: Optional[Callable[[], None]] = None,
        selected: bool = False,
        text_colors: Optional[Dict[int, str]] = None,
    ) -> None:
        if selected:
            bg = UI_SELECTION
        else:
            bg = UI_PANEL_BG if row_index % 2 == 0 else "#f8fbfd"
        row = tk.Frame(rows_frame, bg=bg, highlightthickness=0)
        grid_row = row_index * 2
        row.grid(row=grid_row, column=0, sticky="ew")
        scroll_canvas = getattr(rows_frame, "_scroll_canvas", None)
        if isinstance(scroll_canvas, tk.Canvas):
            row.bind("<MouseWheel>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
            row.bind("<Button-4>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
            row.bind("<Button-5>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
        if on_row_click is not None:
            row.bind("<Button-1>", lambda _e, cb=on_row_click: cb())
        if on_row_context is not None:
            row.bind("<Button-3>", lambda e, cb=on_row_context: cb(e))
            row.bind("<Button-2>", lambda e, cb=on_row_context: cb(e))
            row.bind("<Control-Button-1>", lambda e, cb=on_row_context: cb(e))
        for idx, (_title, width, anchor) in enumerate(columns):
            text = values[idx] if idx < len(values) else ""
            if action_col is not None and idx == action_col and text and action_callback is not None:
                base_action_color = action_color or UI_ACCENT
                hover_action_color = "#174e63"
                if base_action_color == UI_ACTION_MOUNT:
                    hover_action_color = "#166431"
                elif base_action_color == UI_ACTION_UMOUNT:
                    hover_action_color = "#8f2f2f"
                lbl = tk.Label(row, text=text, bg=bg, fg=base_action_color, anchor=anchor, padx=6, pady=3)
                def _on_action_click(_e: Any, cb: Callable[[], None] = action_callback, row_cb: Optional[Callable[[], None]] = on_row_click) -> None:
                    if row_cb is not None:
                        row_cb()
                    cb()
                lbl.bind("<Button-1>", _on_action_click)
                lbl.bind(
                    "<Enter>",
                    lambda _e, w=lbl, c=hover_action_color: w.configure(
                        cursor=("hand2" if self.ssh_busy_count == 0 else ""),
                        fg=c,
                    ),
                )
                lbl.bind("<Leave>", lambda _e, w=lbl, c=base_action_color: w.configure(cursor="", fg=c))
                if on_cell_hover is not None:
                    lbl.bind("<Enter>", lambda e, i=idx, t=text, cb=on_cell_hover: cb(i, t, e), add="+")
                    lbl.bind("<Motion>", lambda e, i=idx, t=text, cb=on_cell_hover: cb(i, t, e), add="+")
                if on_cell_leave is not None:
                    lbl.bind("<Leave>", lambda _e, cb=on_cell_leave: cb(), add="+")
            else:
                fg_color = (text_colors or {}).get(idx, UI_TEXT)
                lbl = tk.Label(row, text=text, bg=bg, fg=fg_color, anchor=anchor, padx=6, pady=3)
                if on_row_click is not None:
                    lbl.bind("<Button-1>", lambda _e, cb=on_row_click: cb())
                if on_row_context is not None:
                    lbl.bind("<Button-3>", lambda e, cb=on_row_context: cb(e))
                    lbl.bind("<Button-2>", lambda e, cb=on_row_context: cb(e))
                    lbl.bind("<Control-Button-1>", lambda e, cb=on_row_context: cb(e))
                if on_cell_hover is not None:
                    lbl.bind("<Enter>", lambda e, i=idx, t=text, cb=on_cell_hover: cb(i, t, e))
                    lbl.bind("<Motion>", lambda e, i=idx, t=text, cb=on_cell_hover: cb(i, t, e))
                if on_cell_leave is not None:
                    lbl.bind("<Leave>", lambda _e, cb=on_cell_leave: cb())
            if isinstance(scroll_canvas, tk.Canvas):
                lbl.bind("<MouseWheel>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                lbl.bind("<Button-4>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                lbl.bind("<Button-5>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
            lbl.grid(row=0, column=idx, sticky="nsew")
            row.grid_columnconfigure(idx, minsize=width, weight=0)
        sep = tk.Frame(rows_frame, bg=UI_BORDER, height=1)
        sep.grid(row=grid_row + 1, column=0, sticky="ew")

    def _load_connections_list(self) -> None:
        selected_id = self.selected_conn_id
        self.conn_list.delete(0, tk.END)
        selected_index: Optional[int] = None
        for idx, c in enumerate(self.store.connections):
            method = c.transport if c.os_type == "Windows" else "SSH"
            st = self.states.get(c.id)
            mark = "[x]" if st and st.ok else "[ ]"
            zfs_ver = "-"
            if st and st.zfs_version:
                zfs_ver = st.zfs_version
            zfs_txt = zfs_ver if zfs_ver != "-" else tr("label_unknown")
            self.conn_list.insert(tk.END, f"{mark} {c.name}/{method}")
            self.conn_list.insert(tk.END, f"   {c.os_type} | ZFS v{zfs_txt}")
            if c.id == selected_id:
                selected_index = idx * 2
        try:
            self.conn_list.xview_moveto(0.0)
        except Exception:
            pass
        if selected_index is not None:
            self.conn_list.selection_clear(0, tk.END)
            self.conn_list.selection_set(selected_index)
            self.conn_list.activate(selected_index)
        self._sync_connection_log_tabs()

    def _conn_profile_index_from_list_index(self, list_idx: int) -> Optional[int]:
        if list_idx < 0:
            return None
        pidx = list_idx // 2
        if 0 <= pidx < len(self.store.connections):
            return pidx
        return None

    def _conn_list_primary_index(self, list_idx: int) -> int:
        if list_idx < 0:
            return -1
        return (list_idx // 2) * 2

    def _sync_connection_log_tabs(self) -> None:
        wanted = {c.id: c.name for c in self.store.connections}
        existing = set(self.connection_log_tabs.keys())
        # Eliminar tabs de conexiones borradas.
        for conn_id in sorted(existing - set(wanted.keys())):
            entry = self.connection_log_tabs.pop(conn_id, None)
            if not entry:
                continue
            frame = entry.get("frame")
            try:
                if frame is not None:
                    self.logs_tabs.forget(frame)
            except Exception:
                pass
        # Crear/actualizar tabs de conexiones actuales.
        for conn in self.store.connections:
            if conn.id not in self.connection_log_tabs:
                frame = ttk.Frame(self.logs_tabs, padding=(6, 6))
                frame.columnconfigure(0, weight=1)
                frame.rowconfigure(0, weight=1)
                txt = tk.Text(
                    frame,
                    height=8,
                    state="disabled",
                    wrap="none",
                    font=("TkDefaultFont", 9),
                    bg=UI_PANEL_BG,
                    fg=UI_TEXT,
                    insertbackground=UI_TEXT,
                    highlightthickness=1,
                    highlightbackground=UI_BORDER,
                    highlightcolor=UI_ACCENT,
                )
                txt.grid(row=0, column=0, sticky="nsew")
                ybar = ttk.Scrollbar(frame, orient="vertical", command=txt.yview)
                ybar.grid(row=0, column=1, sticky="ns")
                xbar = ttk.Scrollbar(frame, orient="horizontal", command=txt.xview)
                xbar.grid(row=1, column=0, sticky="ew")
                txt.configure(yscrollcommand=ybar.set, xscrollcommand=xbar.set)
                self.logs_tabs.add(frame, text=conn.name)
                self.connection_log_tabs[conn.id] = {"frame": frame, "text": txt}
            else:
                frame = self.connection_log_tabs[conn.id]["frame"]
                try:
                    self.logs_tabs.tab(frame, text=conn.name)
                except Exception:
                    pass

    def _append_line_to_log_widget(self, widget: tk.Text, line: str) -> None:
        widget.configure(state="normal")
        if "[WARNING]" in line:
            widget.insert("end", line + "\n", ("warn",))
        else:
            widget.insert("end", line + "\n")
        self._trim_log_widget_lines(widget)
        widget.see("end")
        widget.configure(state="disabled")

    def _max_log_lines(self) -> int:
        try:
            return max(1, int((self.log_max_lines_var.get() or "500").strip()))
        except Exception:
            return 500

    def _trim_log_widget_lines(self, widget: tk.Text) -> None:
        try:
            limit = self._max_log_lines()
            # "end-1c" evita contar la linea vacia final de Tk Text.
            total_lines = int(widget.index("end-1c").split(".", 1)[0])
            if total_lines <= limit:
                return
            remove = total_lines - limit
            widget.delete("1.0", f"{remove + 1}.0")
        except Exception:
            pass

    def _trim_all_log_widgets(self) -> None:
        widgets: List[tk.Text] = [self.app_log_text]
        for entry in self.connection_log_tabs.values():
            txt = entry.get("text")
            if isinstance(txt, tk.Text):
                widgets.append(txt)
        for w in widgets:
            try:
                w.configure(state="normal")
                self._trim_log_widget_lines(w)
                w.configure(state="disabled")
            except Exception:
                pass

    def _resolve_conn_id_from_ssh_line(self, line: str) -> Optional[str]:
        try:
            m = re.search(r"([A-Za-z0-9._-]+):(\d+)\s+\$", line)
            if not m:
                return None
            host = m.group(1).strip().lower()
            port = int(m.group(2))
        except Exception:
            return None
        # Primero por host+port exacto.
        for conn in self.store.connections:
            if conn.host.strip().lower() == host and int(conn.port or 22) == port:
                return conn.id
        # Fallback por host.
        for conn in self.store.connections:
            if conn.host.strip().lower() == host:
                return conn.id
        return None

    def _strip_conn_endpoint_from_log_line(self, line: str) -> str:
        """Oculta prefijo 'usuario@host:port' en tabs por conexion."""
        try:
            m = re.match(
                r"^(\[[^\]]+\]\s+)(?:\S+@)?([A-Za-z0-9._-]+):(\d+)\s+(.*)$",
                line,
            )
            if m:
                return f"{m.group(1)}{m.group(4)}".rstrip()
        except Exception:
            pass
        return line

    def _clear_app_log(self) -> None:
        self._hide_app_log_tooltip()
        self._ssh_last_line_full = ""
        self.ssh_last_line_var.set("")
        try:
            self.ssh_last_line_text.configure(state="normal")
            self.ssh_last_line_text.delete("1.0", "end")
            self.ssh_last_line_text.configure(state="disabled")
        except Exception:
            pass
        widgets: List[tk.Text] = [self.app_log_text]
        for entry in self.connection_log_tabs.values():
            txt = entry.get("text")
            if isinstance(txt, tk.Text):
                widgets.append(txt)
        for widget in widgets:
            widget.configure(state="normal")
            widget.delete("1.0", "end")
            widget.configure(state="disabled")

    def _copy_app_log(self) -> None:
        app_text = self.app_log_text.get("1.0", "end-1c")
        chunks = [f"[{tr('log_tab_app')}]\n{app_text}"]
        for conn in self.store.connections:
            entry = self.connection_log_tabs.get(conn.id)
            if not entry:
                continue
            txt = entry.get("text")
            if not isinstance(txt, tk.Text):
                continue
            body = txt.get("1.0", "end-1c")
            chunks.append(f"[{conn.name}]\n{body}")
        text = "\n\n".join(chunks)
        self.clipboard_clear()
        self.clipboard_append(text)
        self.update_idletasks()
        self._app_log("info", tr("log_copied"))

    def _on_log_level_changed(self, _event: Any = None) -> None:
        try:
            level = (self.app_log_level_var.get() or "normal").lower()
        except Exception:
            level = "normal"
        if level not in {"normal", "info", "debug"}:
            level = "normal"
        self._app_log_level_cached = level
        self._app_log("info", tr("log_level_updated"))

    def _on_log_max_lines_changed(self, _event: Any = None) -> None:
        self._trim_all_log_widgets()
        self._app_log("info", f"Max log lines: {self._max_log_lines()}")

    def _should_log(self, level: str) -> bool:
        order = {"normal": 0, "info": 1, "debug": 2}
        current = order.get((self._app_log_level_cached or "normal").lower(), 0)
        wanted = order.get(level.lower(), 0)
        return wanted <= current

    def _log_timestamp(self) -> str:
        return time.strftime("%Y-%m-%d %H:%M:%S")

    def _rotate_log_file_if_needed(self, path: Path, incoming_len: int) -> None:
        try:
            if not path.exists():
                return
            if path.stat().st_size + incoming_len <= LOG_ROTATE_MAX_BYTES:
                return
            oldest = path.with_name(f"{path.name}.{LOG_ROTATE_BACKUP_COUNT}")
            if oldest.exists():
                try:
                    oldest.unlink()
                except Exception:
                    pass
            for idx in range(LOG_ROTATE_BACKUP_COUNT - 1, 0, -1):
                src = path.with_name(f"{path.name}.{idx}")
                dst = path.with_name(f"{path.name}.{idx + 1}")
                if src.exists():
                    try:
                        src.rename(dst)
                    except Exception:
                        pass
            path.rename(path.with_name(f"{path.name}.1"))
        except Exception:
            pass

    def _append_persistent_log(self, path: Path, line: str) -> None:
        text = (line or "").rstrip() + "\n"
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with self._persistent_log_lock:
                self._rotate_log_file_if_needed(path, len(text.encode("utf-8")))
                with path.open("a", encoding="utf-8") as fh:
                    fh.write(text)
        except Exception:
            pass

    def _app_log(self, level: str, message: str) -> None:
        if not self._should_log(level):
            return
        safe_message = self._mask_sensitive_text(message)
        ts = self._log_timestamp()
        one_line = " | ".join(part.strip() for part in (safe_message or "").splitlines() if part.strip())
        if not one_line:
            return
        line = f"[{ts}] [{level.upper()}] {one_line}"
        self._append_persistent_log(APP_LOG_FILE, line)

        if self._is_closing:
            return

        def _append() -> None:
            if self._is_closing:
                return
            self._append_line_to_log_widget(self.app_log_text, line)
            self._ssh_last_line_full = line
            self._refresh_ssh_last_line_summary()
        try:
            self.after(0, _append)
        except Exception:
            pass

    def _on_app_log_hover(self, event: Any) -> None:
        try:
            index = self.app_log_text.index(f"@{event.x},{event.y}")
            line_no = index.split(".", 1)[0]
            line_text = self.app_log_text.get(f"{line_no}.0", f"{line_no}.end").strip()
        except Exception:
            line_text = ""
        if not line_text:
            self._hide_app_log_tooltip()
            return
        if self._app_log_tipwindow is not None and line_text == self._app_log_tip_text:
            try:
                self._app_log_tipwindow.wm_geometry(f"+{event.x_root + 12}+{event.y_root + 12}")
            except Exception:
                pass
            return
        self._show_app_log_tooltip(line_text, event.x_root + 12, event.y_root + 12)

    def _show_app_log_tooltip(self, text: str, x: int, y: int) -> None:
        self._hide_app_log_tooltip()
        try:
            tw = tk.Toplevel(self)
            tw.wm_overrideredirect(True)
            tw.wm_geometry(f"+{x}+{y}")
            lbl = tk.Label(
                tw,
                text=text,
                bg=UI_PANEL_BG,
                fg=UI_TEXT,
                relief="solid",
                borderwidth=1,
                padx=6,
                pady=3,
                justify="left",
                wraplength=900,
                font=("TkFixedFont", 9),
            )
            lbl.pack()
            self._app_log_tipwindow = tw
            self._app_log_tip_text = text
        except Exception:
            self._app_log_tipwindow = None
            self._app_log_tip_text = ""

    def _hide_app_log_tooltip(self, _event: Any = None) -> None:
        if self._app_log_tipwindow is not None:
            try:
                self._app_log_tipwindow.destroy()
            except Exception:
                pass
        self._app_log_tipwindow = None
        self._app_log_tip_text = ""

    def _refresh_ssh_last_line_summary(self) -> None:
        full = (self._ssh_last_line_full or "").strip()
        if not full:
            self.ssh_last_line_var.set("")
            try:
                self.ssh_last_line_text.configure(state="normal")
                self.ssh_last_line_text.delete("1.0", "end")
                self.ssh_last_line_text.configure(state="disabled")
            except Exception:
                pass
            return
        prefix = "Detalle: "
        text = f"{prefix}{full}"
        self.ssh_last_line_var.set(text)
        try:
            self.ssh_last_line_text.configure(state="normal")
            self.ssh_last_line_text.delete("1.0", "end")
            self.ssh_last_line_text.insert("1.0", text)
            self.ssh_last_line_text.configure(state="disabled")
            self.ssh_last_line_text.yview_moveto(0.0)
        except Exception:
            pass

    def _ssh_log(self, message: str) -> None:
        safe_message = self._mask_sensitive_text(message)
        one_line = " | ".join(part.strip() for part in (safe_message or "").splitlines() if part.strip())
        if not one_line:
            return
        ts = self._log_timestamp()
        line = f"[{ts}] {one_line}"
        self._append_persistent_log(SSH_EXEC_LOG_FILE, line)

        if self._is_closing:
            return

        def _append() -> None:
            if self._is_closing:
                return
            self._append_line_to_log_widget(self.app_log_text, line)
            conn_id = self._resolve_conn_id_from_ssh_line(line)
            if conn_id and conn_id in self.connection_log_tabs:
                txt = self.connection_log_tabs[conn_id].get("text")
                if isinstance(txt, tk.Text):
                    conn_line = self._strip_conn_endpoint_from_log_line(line)
                    self._append_line_to_log_widget(txt, conn_line)
            self._ssh_last_line_full = line
            self._refresh_ssh_last_line_summary()
        try:
            self.after(0, _append)
        except Exception:
            pass

    def _split_top_level_pipes(self, cmd: str) -> List[str]:
        parts: List[str] = []
        if not cmd:
            return parts
        cur: List[str] = []
        in_single = False
        in_double = False
        escape = False
        for ch in cmd:
            if escape:
                cur.append(ch)
                escape = False
                continue
            if ch == "\\" and not in_single:
                cur.append(ch)
                escape = True
                continue
            if ch == "'" and not in_double:
                in_single = not in_single
                cur.append(ch)
                continue
            if ch == '"' and not in_single:
                in_double = not in_double
                cur.append(ch)
                continue
            if ch == "|" and not in_single and not in_double:
                part = "".join(cur).strip()
                if part:
                    parts.append(part)
                cur = []
                continue
            cur.append(ch)
        tail = "".join(cur).strip()
        if tail:
            parts.append(tail)
        return parts

    def _log_action_subcommands(self, action_label: str, cmd: str) -> None:
        parts = self._split_top_level_pipes(cmd)
        if not parts:
            return
        total = len(parts)
        for idx, part in enumerate(parts, start=1):
            self._app_log("info", f"{action_label} subcmd [{idx}/{total}]: {part}")

    def _pool_status_cache_key(self, conn_id: str, pool_name: str) -> str:
        return f"{conn_id}:{pool_name}"

    def _prefetch_imported_pool_status(self, conn_id: str, pool_name: str) -> None:
        key = self._pool_status_cache_key(conn_id, pool_name)
        with self.pool_status_lock:
            if key in self.pool_status_cache or key in self.pool_status_loading:
                return
            self.pool_status_loading.add(key)

        profile = self.store.get(conn_id)
        if not profile:
            with self.pool_status_lock:
                self.pool_status_loading.discard(key)
            return

        def worker() -> None:
            text = ""
            try:
                execu = make_executor(profile)
                text = (execu.pool_status_verbose(pool_name) or "").strip()
            except Exception as exc:
                text = str(exc).strip()
            if not text:
                text = "(sin salida)"
            with self.pool_status_lock:
                self.pool_status_cache[key] = text
                self.pool_status_loading.discard(key)

        threading.Thread(target=worker, daemon=True).start()

    def _show_pool_status_tooltip(self, text: str, x: int, y: int) -> None:
        if self._pool_status_tipwindow is not None and text == self._pool_status_tip_text:
            try:
                self._pool_status_tipwindow.wm_geometry(f"+{x}+{y}")
            except Exception:
                pass
            return
        self._hide_pool_status_tooltip()
        try:
            tw = tk.Toplevel(self)
            tw.wm_overrideredirect(True)
            tw.wm_geometry(f"+{x}+{y}")
            lbl = tk.Label(
                tw,
                text=text,
                bg=UI_PANEL_BG,
                fg=UI_TEXT,
                relief="solid",
                borderwidth=1,
                padx=6,
                pady=3,
                justify="left",
                wraplength=1100,
                font=("TkFixedFont", 9),
            )
            lbl.pack()
            self._pool_status_tipwindow = tw
            self._pool_status_tip_text = text
        except Exception:
            self._pool_status_tipwindow = None
            self._pool_status_tip_text = ""

    def _hide_pool_status_tooltip(self, _event: Any = None) -> None:
        if self._pool_status_tipwindow is not None:
            try:
                self._pool_status_tipwindow.destroy()
            except Exception:
                pass
        self._pool_status_tipwindow = None
        self._pool_status_tip_text = ""

    def _on_imported_pool_cell_hover(self, conn_id: str, pool_name: str, col_idx: int, _cell_text: str, event: Any) -> None:
        # Solo tooltip sobre la columna "Pool".
        if col_idx != 1:
            self._hide_pool_status_tooltip()
            return
        key = self._pool_status_cache_key(conn_id, pool_name)
        text = ""
        with self.pool_status_lock:
            text = self.pool_status_cache.get(key, "")
        if not text:
            self._prefetch_imported_pool_status(conn_id, pool_name)
            text = f"zpool status -v {pool_name}\n..."
        self._show_pool_status_tooltip(text, event.x_root + 12, event.y_root + 12)

    def _mask_sensitive_text(self, text: str) -> str:
        out = text or ""
        # Enmascara passwords configurados (tal cual y escapados en shell/log).
        for conn in self.store.connections:
            pwd = (conn.password or "").strip()
            if not pwd:
                continue
            variants = {
                pwd,
                shlex.quote(pwd),
                pwd.replace("'", "\\'"),
            }
            for v in variants:
                if v:
                    out = out.replace(v, "*")
        # Fallback generico: printf/sudo con password inline.
        out = re.sub(r"(printf\s+'%s\\n'\s+)(\S+)", r"\1*", out)
        return out

    def _on_ssh_busy_delta(self, delta: int) -> None:
        def _apply() -> None:
            self.ssh_busy_count = max(0, self.ssh_busy_count + delta)
            locked = self.ssh_busy_count > 0
            if locked != self.ssh_actions_locked:
                self.ssh_actions_locked = locked
                self._set_actions_locked(locked)
        self.after(0, _apply)

    def _set_actions_locked(self, locked: bool) -> None:
        state = "disabled" if locked else "normal"
        combo_state = "disabled" if locked else "readonly"
        self.new_conn_btn.configure(state=state)
        self.refresh_all_btn.configure(state=state)
        self.conn_list.configure(state=state)
        self.origin_pool_combo.configure(state=combo_state)
        self.dest_pool_combo.configure(state=combo_state)
        self.app_log_level_combo.configure(state=combo_state)
        self.log_max_lines_combo.configure(state=combo_state)
        # Importar/Exportar se bloquea por validacion en click handlers.
        self._update_level_button_state()
        self._refresh_busy_cursor()
        if locked:
            self.status_var.set(tr("status_ssh_busy"))
        elif self.status_var.get() == tr("status_ssh_busy"):
            self.status_var.set(tr("status_ready"))

    def _refresh_busy_cursor(self) -> None:
        busy = bool(self.ssh_busy_count > 0 or self.pool_props_loading_count > 0)
        self._set_cursor_busy(busy)

    def _set_cursor_busy(self, busy: bool) -> None:
        cursor = self._busy_cursor if busy else ""

        def apply_widget(widget: tk.Misc) -> None:
            try:
                widget.configure(cursor=cursor)
            except Exception:
                pass
            for child in widget.winfo_children():
                apply_widget(child)

        apply_widget(self)
        self.update_idletasks()

    def _dismiss_active_context_menu(self, unpost: bool = True) -> None:
        menu = self._active_context_menu
        if menu and unpost:
            try:
                menu.unpost()
            except Exception:
                pass
        if menu and self._context_menu_unmap_bind_id:
            try:
                menu.unbind("<Unmap>", self._context_menu_unmap_bind_id)
            except Exception:
                pass
        self._context_menu_unmap_bind_id = None
        for sequence, bind_id in self._context_menu_global_bindings:
            try:
                self.unbind(sequence, bind_id)
            except Exception:
                pass
        self._context_menu_global_bindings.clear()
        self._active_context_menu = None

    def _on_context_menu_global_click(self, event: Any) -> None:
        menu = self._active_context_menu
        if not menu:
            return
        widget_name = str(getattr(event, "widget", ""))
        if widget_name and widget_name.startswith(str(menu)):
            return
        self._dismiss_active_context_menu(unpost=True)

    def _on_context_menu_unmap(self, _event: Any) -> None:
        self._dismiss_active_context_menu(unpost=False)

    def _show_context_menu(self, menu: tk.Menu, event: Any) -> None:
        self._dismiss_active_context_menu(unpost=True)
        self._active_context_menu = menu
        self._context_menu_unmap_bind_id = menu.bind("<Unmap>", self._on_context_menu_unmap, add="+")
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            try:
                menu.grab_release()
            except Exception:
                pass
        # El mismo click derecho que abre el menu puede disparar los bind_all
        # de cierre inmediato si se registran antes del popup.
        self.after_idle(self._arm_context_menu_dismiss_bindings)

    def _arm_context_menu_dismiss_bindings(self) -> None:
        if not self._active_context_menu:
            return
        for sequence in ("<Button-1>", "<Button-2>", "<Button-3>", "<Escape>"):
            bind_id = self.bind(sequence, self._on_context_menu_global_click, add="+")
            if bind_id:
                self._context_menu_global_bindings.append((sequence, bind_id))

    def _reject_if_ssh_busy(self) -> bool:
        if self.ssh_busy_count > 0:
            self._app_log("info", tr("status_ssh_busy"))
            return True
        return False

    def _selected_profile(self) -> Optional[ConnectionProfile]:
        selected = self.conn_list.curselection()
        if selected:
            idx = selected[0]
            pidx = self._conn_profile_index_from_list_index(idx)
            if pidx is not None:
                profile = self.store.connections[pidx]
                self.selected_conn_id = profile.id
                return profile
        if self.selected_conn_id:
            return self.store.get(self.selected_conn_id)
        return None

    def on_select_connection(self, _event: Any = None) -> None:
        profile = self._selected_profile()
        if not profile:
            return
        selected = self.conn_list.curselection()
        if selected:
            primary = self._conn_list_primary_index(selected[0])
            if primary >= 0 and primary != selected[0]:
                self.conn_list.selection_clear(0, tk.END)
                self.conn_list.selection_set(primary)
                self.conn_list.activate(primary)
        self.selected_conn_id = profile.id
        self._app_log("info", trf("log_connection_selected", name=profile.name))
        self._render_connection_state(profile, refresh_tables=False)

    def on_double_click_connection(self, event: Any) -> None:
        if self._reject_if_ssh_busy():
            return
        idx = self.conn_list.nearest(event.y)
        if idx < 0:
            return
        idx = self._conn_list_primary_index(idx)
        self.conn_list.selection_clear(0, tk.END)
        self.conn_list.selection_set(idx)
        self.conn_list.activate(idx)
        self.on_select_connection()
        self.refresh_selected()

    def _on_connection_context(self, event: Any) -> str:
        if self._reject_if_ssh_busy():
            return "break"
        idx = self.conn_list.nearest(event.y)
        if idx < 0 or idx >= self.conn_list.size():
            return "break"
        idx = self._conn_list_primary_index(idx)
        bbox = self.conn_list.bbox(idx)
        if not bbox:
            return "break"
        x, y, w, h = bbox
        if not (x <= event.x <= x + w and y <= event.y <= y + h):
            return "break"
        self.conn_list.selection_clear(0, tk.END)
        self.conn_list.selection_set(idx)
        self.conn_list.activate(idx)
        self.on_select_connection()
        has_sel = self._selected_profile() is not None
        self.conn_context_menu.entryconfigure(0, state=("normal" if has_sel else "disabled"))
        self.conn_context_menu.entryconfigure(1, state=("normal" if has_sel else "disabled"))
        self.conn_context_menu.entryconfigure(3, state=("normal" if has_sel else "disabled"))
        self._show_context_menu(self.conn_context_menu, event)
        return "break"

    def _on_left_tab_changed(self, _event: Any = None) -> None:
        current = self.left_tabs.select()
        tab_id = str(current) if current else str(self.tab_connections)
        self._app_log("debug", trf("log_tab_changed", tab=tab_id))
        if tab_id == str(self.tab_datasets):
            self.right_conn_detail.grid_remove()
            self.right_datasets_detail.grid()
            self._load_datasets_for_active_connection()
        else:
            self.right_datasets_detail.grid_remove()
            self.right_conn_detail.grid()

    def _conn_openzfs_version(self, conn_id: str) -> Optional[Tuple[int, int, int]]:
        state = self.states.get(conn_id)
        if state and state.zfs_version and state.zfs_version != "-":
            parsed = parse_openzfs_version(state.zfs_version)
            if parsed:
                return parsed
        return None

    def _build_send_cmd_candidates(
        self,
        src_conn_id: str,
        src_dataset: str,
        target_snapshot: str,
        recursive: bool,
        incremental_base: Optional[str] = None,
    ) -> List[str]:
        version = self._conn_openzfs_version(src_conn_id)
        flags = build_send_flag_candidates(version, recursive=recursive)
        commands: List[str] = []
        for flag in flags:
            if incremental_base:
                if flag:
                    cmd = (
                        f"zfs send -{flag} -I "
                        f"{shlex.quote(incremental_base)} {shlex.quote(target_snapshot)}"
                    )
                else:
                    cmd = (
                        f"zfs send -I "
                        f"{shlex.quote(incremental_base)} {shlex.quote(target_snapshot)}"
                    )
            else:
                cmd = f"zfs send -{flag} {shlex.quote(target_snapshot)}" if flag else f"zfs send {shlex.quote(target_snapshot)}"
            commands.append(cmd)
        self._app_log(
            "debug",
            f"OpenZFS src={src_dataset} version={format_openzfs_version(version)} send_flags={', '.join(flags)}",
        )
        return commands

    def _level_datasets(self) -> None:
        if self._reject_if_ssh_busy():
            return
        src_selection = self.origin_pool_var.get().strip()
        dst_selection = self.dest_pool_var.get().strip()
        src_dataset = self.origin_dataset_var.get().strip()
        dst_dataset = self.dest_dataset_var.get().strip()
        if not src_dataset or not dst_dataset:
            self._app_log("normal", tr("log_level_missing_selection"))
            return
        if "@" in src_dataset or "@" in dst_dataset:
            self._app_log("normal", tr("log_level_snapshot_selected"))
            return
        if src_selection not in self.dataset_pool_options or dst_selection not in self.dataset_pool_options:
            self._app_log("normal", tr("log_level_invalid_pool"))
            return
        src_conn_id, src_pool = self.dataset_pool_options[src_selection]
        dst_conn_id, dst_pool = self.dataset_pool_options[dst_selection]
        src_profile = self.store.get(src_conn_id)
        dst_profile = self.store.get(dst_conn_id)
        if not src_profile or not dst_profile:
            self._app_log("normal", tr("log_level_invalid_pool"))
            return
        src_key = f"{src_conn_id}:{src_pool}"
        dst_key = f"{dst_conn_id}:{dst_pool}"
        src_rows = self.datasets_cache.get(src_key, [])
        dst_rows = self.datasets_cache.get(dst_key, [])
        if not src_rows or not dst_rows:
            self._app_log("normal", tr("log_level_missing_cache"))
            return

        def wrap_outer_exec(profile: ConnectionProfile, command: str) -> Optional[str]:
            return _ssh_outer_exec_command(
                profile,
                command,
                include_key=True,
                allow_password_auth=True,
            )

        def wrap_inner_dest_exec(profile: ConnectionProfile, command: str, include_key: bool = True) -> Optional[str]:
            return _ssh_outer_exec_command(
                profile,
                command,
                include_key=include_key,
                allow_password_auth=True,
            )

        def sudo_wrap(profile: ConnectionProfile, base_cmd: str, preserve_stdin_stream: bool = False) -> str:
            if profile.conn_type != "SSH" or not profile.use_sudo:
                return base_cmd
            if profile.password:
                if preserve_stdin_stream:
                    askpass_line = "printf '%s\\n' " + shlex.quote(profile.password)
                    return (
                        "ask=$(mktemp); "
                        "trap 'rm -f \"$ask\"' EXIT; "
                        "{ printf '%s\\n' '#!/bin/sh'; "
                        f"printf '%s\\n' {shlex.quote(askpass_line)}; "
                        "} >\"$ask\"; "
                        "chmod 700 \"$ask\"; "
                        "SUDO_ASKPASS=\"$ask\" sudo -A -p '' sh -lc "
                        f"{shlex.quote(base_cmd)}"
                    )
                return (
                    f"printf '%s\\n' {shlex.quote(profile.password)} | "
                    f"sudo -S -p '' sh -lc {shlex.quote(base_cmd)}"
                )
            return f"sudo -n sh -lc {shlex.quote(base_cmd)}"

        def _source_can_reach_dest(src: ConnectionProfile, dst: ConnectionProfile) -> bool:
            # LOCAL -> LOCAL: siempre alcanzable.
            if src.conn_type == "LOCAL" and dst.conn_type == "LOCAL":
                return True
            # Cualquier caso no-SSH no soportado para este chequeo.
            if dst.conn_type != "SSH":
                return False

            target = dst.host
            if dst.username:
                target = f"{dst.username}@{dst.host}"
            check_parts = _ssh_common_parts(dst, include_key=True)
            check_parts.extend(["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"])
            check_parts.extend([shlex.quote(target), "true"])
            check_cmd = " ".join(check_parts)

            if src.conn_type == "LOCAL":
                try:
                    proc = subprocess.run(
                        check_cmd,
                        shell=True,
                        capture_output=True,
                        text=True,
                        timeout=8,
                    )
                    return proc.returncode == 0
                except Exception:
                    return False

            if src.conn_type == "SSH":
                if paramiko is None:
                    return False
                client = paramiko.SSHClient()
                client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                kwargs: Dict[str, Any] = {
                    "hostname": src.host,
                    "port": src.port or 22,
                    "username": src.username,
                    "timeout": 8,
                    "look_for_keys": False,
                    "banner_timeout": 8,
                    "auth_timeout": 8,
                }
                resolved_key = _resolve_ssh_private_key_path(src.key_path)
                if resolved_key:
                    kwargs["key_filename"] = resolved_key
                if src.password:
                    kwargs["password"] = src.password
                try:
                    client.connect(**kwargs)
                    _stdin, stdout, _stderr = client.exec_command(f"sh -lc {shlex.quote(check_cmd)}", timeout=8)
                    code = stdout.channel.recv_exit_status()
                    return code == 0
                except Exception:
                    return False
                finally:
                    try:
                        client.close()
                    except Exception:
                        pass

            return False

        def snapshot_suffixes(rows: List[Dict[str, str]], dataset_name: str) -> List[str]:
            snaps: List[str] = []
            prefix = dataset_name + "@"
            for row in rows:
                name = row.get("name", "").strip()
                if name.startswith(prefix):
                    snaps.append(name.split("@", 1)[1])
            return snaps

        src_snaps = snapshot_suffixes(src_rows, src_dataset)
        dst_snaps = set(snapshot_suffixes(dst_rows, dst_dataset))
        if not src_snaps:
            self._app_log("normal", trf("log_level_no_source_snapshots", dataset=src_dataset))
            return

        missing = [snap for snap in src_snaps if snap not in dst_snaps]
        if not missing:
            self._app_log("info", trf("log_level_already_aligned", src=src_dataset, dst=dst_dataset))
            return

        common = [snap for snap in src_snaps if snap in dst_snaps]
        target_snap = missing[-1]
        target_full = f"{src_dataset}@{target_snap}"
        if common:
            base_snap = common[-1]
            base_full = f"{src_dataset}@{base_snap}"
            send_candidates = self._build_send_cmd_candidates(
                src_conn_id=src_conn_id,
                src_dataset=src_dataset,
                target_snapshot=target_full,
                recursive=True,
                incremental_base=base_full,
            )
            send_raw = send_candidates[0]
            recv_raw = f"zfs recv -F {shlex.quote(dst_dataset)}"
            send_cmd = sudo_wrap(src_profile, send_raw, preserve_stdin_stream=False)
            recv_cmd = sudo_wrap(dst_profile, recv_raw, preserve_stdin_stream=True)
            self._app_log(
                "normal",
                trf("log_level_incremental_plan", src=src_dataset, dst=dst_dataset, base=base_snap, target=target_snap),
            )
        else:
            send_candidates = self._build_send_cmd_candidates(
                src_conn_id=src_conn_id,
                src_dataset=src_dataset,
                target_snapshot=target_full,
                recursive=True,
                incremental_base=None,
            )
            send_raw = send_candidates[0]
            recv_raw = f"zfs recv -F {shlex.quote(dst_dataset)}"
            send_cmd = sudo_wrap(src_profile, send_raw, preserve_stdin_stream=False)
            recv_cmd = sudo_wrap(dst_profile, recv_raw, preserve_stdin_stream=True)
            self._app_log("normal", trf("log_level_no_common_base", src=src_dataset, dst=dst_dataset, target=target_snap))

        if src_profile.conn_type == "PSRP" or dst_profile.conn_type == "PSRP":
            if not (src_profile.conn_type == "PSRP" and dst_profile.conn_type == "PSRP" and src_conn_id == dst_conn_id):
                self._app_log(
                    "normal",
                    trf(
                        "log_level_transport_unsupported",
                        src=src_profile.conn_type,
                        dst=dst_profile.conn_type,
                    ),
                )
                return
            ps_cmd = f"$ErrorActionPreference='Stop'; {send_raw} | {recv_raw}"
            self._app_log("info", trf("log_level_missing_snapshots", count=len(missing), snaps=', '.join(missing)))
            self._app_log("normal", ps_cmd)
            self._run_level_psrp_command(ps_cmd, src_profile, affected_conn_ids=[src_conn_id, dst_conn_id])
            return

        if src_profile.conn_type not in {"LOCAL", "SSH"} or dst_profile.conn_type not in {"LOCAL", "SSH"}:
            self._app_log(
                "normal",
                trf(
                    "log_level_transport_unsupported",
                    src=src_profile.conn_type,
                    dst=dst_profile.conn_type,
                ),
            )
            return

        self._app_log("info", tr("log_level_forced_local_execution"))
        send_side = wrap_outer_exec(src_profile, send_cmd)
        recv_side = wrap_outer_exec(dst_profile, recv_cmd)
        if not send_side or not recv_side:
            self._app_log("normal", trf("log_level_transport_unsupported", src=src_profile.conn_type, dst=dst_profile.conn_type))
            return
        progress_stage = ""
        if shutil.which("pv"):
            progress_stage = "pv -f -trab"
            self._app_log("info", tr("log_level_progress_pv"))
        elif shutil.which("dd"):
            progress_stage = "dd bs=4M status=progress"
            self._app_log("info", tr("log_level_progress_dd"))
        else:
            self._app_log("normal", tr("log_level_progress_none"))
        if progress_stage:
            cmd = f"{send_side} | {progress_stage} | {recv_side}"
        else:
            cmd = f"{send_side} | {recv_side}"

        self._app_log("info", trf("log_level_missing_snapshots", count=len(missing), snaps=', '.join(missing)))
        self._app_log("normal", cmd)
        extra_cmds: List[str] = []
        if len(send_candidates) > 1:
            for alt_send_raw in send_candidates[1:]:
                alt_send_cmd = sudo_wrap(src_profile, alt_send_raw, preserve_stdin_stream=False)
                alt_send_side = wrap_outer_exec(src_profile, alt_send_cmd)
                if not alt_send_side:
                    continue
                if progress_stage:
                    extra_cmds.append(f"{alt_send_side} | {progress_stage} | {recv_side}")
                else:
                    extra_cmds.append(f"{alt_send_side} | {recv_side}")
        self._run_level_command(cmd, fallback_cmds=extra_cmds, affected_conn_ids=[src_conn_id, dst_conn_id])

    def _copy_snapshot_to_dataset(self) -> None:
        if self._reject_if_ssh_busy():
            return
        src_selection = self.origin_pool_var.get().strip()
        dst_selection = self.dest_pool_var.get().strip()
        src_snapshot = self.origin_dataset_var.get().strip()
        dst_dataset = self.dest_dataset_var.get().strip()
        if not src_snapshot or not dst_dataset:
            self._app_log("normal", tr("log_copy_missing_selection"))
            return
        if "@" not in src_snapshot:
            self._app_log("normal", tr("log_copy_requires_origin_snapshot"))
            return
        if "@" in dst_dataset:
            self._app_log("normal", tr("log_copy_requires_dest_dataset"))
            return
        if src_selection not in self.dataset_pool_options or dst_selection not in self.dataset_pool_options:
            self._app_log("normal", tr("log_copy_invalid_pool"))
            return
        src_conn_id, src_pool = self.dataset_pool_options[src_selection]
        dst_conn_id, dst_pool = self.dataset_pool_options[dst_selection]
        src_profile = self.store.get(src_conn_id)
        dst_profile = self.store.get(dst_conn_id)
        if not src_profile or not dst_profile:
            self._app_log("normal", tr("log_copy_invalid_pool"))
            return
        src_key = f"{src_conn_id}:{src_pool}"
        dst_key = f"{dst_conn_id}:{dst_pool}"
        src_rows = self.datasets_cache.get(src_key, [])
        dst_rows = self.datasets_cache.get(dst_key, [])
        if not src_rows or not dst_rows:
            self._app_log("normal", tr("log_copy_missing_cache"))
            return
        if not any((row.get("name", "").strip() == src_snapshot) for row in src_rows):
            self._app_log("normal", trf("log_copy_snapshot_not_found", snap=src_snapshot))
            return
        if not any((row.get("name", "").strip() == dst_dataset and "@" not in row.get("name", "").strip()) for row in dst_rows):
            self._app_log("normal", trf("log_copy_dest_not_found", dataset=dst_dataset))
            return

        def wrap_outer_exec(profile: ConnectionProfile, command: str) -> Optional[str]:
            return _ssh_outer_exec_command(
                profile,
                command,
                include_key=True,
                allow_password_auth=True,
            )

        def sudo_wrap(profile: ConnectionProfile, base_cmd: str, preserve_stdin_stream: bool = False) -> str:
            if profile.conn_type != "SSH" or not profile.use_sudo:
                return base_cmd
            if profile.password:
                if preserve_stdin_stream:
                    askpass_line = "printf '%s\\n' " + shlex.quote(profile.password)
                    return (
                        "ask=$(mktemp); "
                        "trap 'rm -f \"$ask\"' EXIT; "
                        "{ printf '%s\\n' '#!/bin/sh'; "
                        f"printf '%s\\n' {shlex.quote(askpass_line)}; "
                        "} >\"$ask\"; "
                        "chmod 700 \"$ask\"; "
                        "SUDO_ASKPASS=\"$ask\" sudo -A -p '' sh -lc "
                        f"{shlex.quote(base_cmd)}"
                    )
                return (
                    f"printf '%s\\n' {shlex.quote(profile.password)} | "
                    f"sudo -S -p '' sh -lc {shlex.quote(base_cmd)}"
                )
            return f"sudo -n sh -lc {shlex.quote(base_cmd)}"

        if src_profile.conn_type == "PSRP" or dst_profile.conn_type == "PSRP":
            if not (src_profile.conn_type == "PSRP" and dst_profile.conn_type == "PSRP" and src_conn_id == dst_conn_id):
                self._app_log(
                    "normal",
                    trf("log_copy_transport_unsupported", src=src_profile.conn_type, dst=dst_profile.conn_type),
                )
                return
        elif src_profile.conn_type not in {"LOCAL", "SSH"} or dst_profile.conn_type not in {"LOCAL", "SSH"}:
            self._app_log(
                "normal",
                trf("log_copy_transport_unsupported", src=src_profile.conn_type, dst=dst_profile.conn_type),
            )
            return

        recursive_choice = messagebox.askyesnocancel(
            tr("confirm"),
            trf("copy_recursive_question", src=src_snapshot, dst=dst_dataset),
        )
        if recursive_choice is None:
            self._app_log("info", tr("log_copy_recursive_cancelled"))
            return
        recursive_send = bool(recursive_choice)
        if recursive_send:
            self._app_log("info", tr("log_copy_recursive_yes"))
        else:
            self._app_log("info", tr("log_copy_recursive_no"))
        send_candidates = self._build_send_cmd_candidates(
            src_conn_id=src_conn_id,
            src_dataset=src_snapshot.split("@", 1)[0],
            target_snapshot=src_snapshot,
            recursive=recursive_send,
            incremental_base=None,
        )
        src_dataset_name = src_snapshot.split("@", 1)[0]
        src_leaf = src_dataset_name.rsplit("/", 1)[-1]
        recv_candidates: List[str] = [f"zfs recv -F -e {shlex.quote(dst_dataset)}"]
        if recursive_send:
            recv_candidates.append(f"zfs recv -F -d {shlex.quote(dst_dataset)}")
        recv_candidates.append(f"zfs recv -F {shlex.quote(dst_dataset)}")
        recv_candidates.append(f"zfs recv -F {shlex.quote(dst_dataset + '/' + src_leaf)}")
        # Deduplicar conservando orden.
        recv_candidates = list(dict.fromkeys(recv_candidates))
        send_raw = send_candidates[0]
        recv_raw = recv_candidates[0]
        if src_profile.conn_type == "PSRP" and dst_profile.conn_type == "PSRP" and src_conn_id == dst_conn_id:
            ps_cmd = f"$ErrorActionPreference='Stop'; {send_raw} | {recv_raw}"
            self._app_log("normal", trf("log_copy_plan", src=src_snapshot, dst=dst_dataset))
            self._app_log("normal", tr("log_copy_command_header"))
            self._app_log("normal", ps_cmd)
            self._run_copy_psrp_command(ps_cmd, src_profile, dst_conn_id=dst_conn_id)
            return
        if src_profile.conn_type == "SSH" and dst_profile.conn_type == "SSH" and paramiko is not None:
            self._app_log("normal", trf("log_copy_plan", src=src_snapshot, dst=dst_dataset))
            self._app_log("normal", tr("log_copy_command_header"))
            self._app_log("normal", "[paramiko-stream] zfs send | zfs recv")
            self._run_copy_paramiko_command(
                src_profile=src_profile,
                dst_profile=dst_profile,
                send_candidates=send_candidates,
                recv_candidates=recv_candidates,
                dst_conn_id=dst_conn_id,
            )
            return
        send_cmd = sudo_wrap(src_profile, send_raw, preserve_stdin_stream=False)
        recv_cmd = sudo_wrap(dst_profile, recv_raw, preserve_stdin_stream=True)
        self._app_log("info", tr("log_copy_forced_local_execution"))
        send_side = wrap_outer_exec(src_profile, send_cmd)
        recv_side = wrap_outer_exec(dst_profile, recv_cmd)
        if not send_side or not recv_side:
            self._app_log("normal", trf("log_copy_transport_unsupported", src=src_profile.conn_type, dst=dst_profile.conn_type))
            return
        progress_stage = ""
        if shutil.which("pv"):
            progress_stage = "pv -f -trab"
            self._app_log("info", tr("log_level_progress_pv"))
        elif shutil.which("dd"):
            progress_stage = "dd bs=4M status=progress"
            self._app_log("info", tr("log_level_progress_dd"))
        else:
            self._app_log("normal", tr("log_level_progress_none"))
        if progress_stage:
            cmd = f"{send_side} | {progress_stage} | {recv_side}"
        else:
            cmd = f"{send_side} | {recv_side}"
        self._app_log("normal", trf("log_copy_plan", src=src_snapshot, dst=dst_dataset))
        self._app_log("normal", tr("log_copy_command_header"))
        self._app_log("normal", cmd)
        extra_cmds: List[str] = []
        # Primero probar variantes de send con recv base.
        if len(send_candidates) > 1:
            for alt_send_raw in send_candidates[1:]:
                alt_send_cmd = sudo_wrap(src_profile, alt_send_raw, preserve_stdin_stream=False)
                alt_send_side = wrap_outer_exec(src_profile, alt_send_cmd)
                if not alt_send_side:
                    continue
                if progress_stage:
                    extra_cmds.append(f"{alt_send_side} | {progress_stage} | {recv_side}")
                else:
                    extra_cmds.append(f"{alt_send_side} | {recv_side}")
        # Si sigue fallando, variar también el recv (clave para destino Windows).
        if len(recv_candidates) > 1:
            for alt_recv_raw in recv_candidates[1:]:
                alt_recv_cmd = sudo_wrap(dst_profile, alt_recv_raw, preserve_stdin_stream=True)
                alt_recv_side = wrap_outer_exec(dst_profile, alt_recv_cmd)
                if not alt_recv_side:
                    continue
                for alt_send_raw in send_candidates:
                    alt_send_cmd = sudo_wrap(src_profile, alt_send_raw, preserve_stdin_stream=False)
                    alt_send_side = wrap_outer_exec(src_profile, alt_send_cmd)
                    if not alt_send_side:
                        continue
                    if progress_stage:
                        extra_cmds.append(f"{alt_send_side} | {progress_stage} | {alt_recv_side}")
                    else:
                        extra_cmds.append(f"{alt_send_side} | {alt_recv_side}")
        # Evitar reintentos duplicados.
        if extra_cmds:
            extra_cmds = list(dict.fromkeys(extra_cmds))
        self._run_copy_command(cmd, dst_conn_id=dst_conn_id, fallback_cmds=extra_cmds)

    def _sync_datasets(self) -> None:
        if self._reject_if_ssh_busy():
            return
        src_selection = self.origin_pool_var.get().strip()
        dst_selection = self.dest_pool_var.get().strip()
        src_dataset = self.origin_dataset_var.get().strip()
        dst_dataset = self.dest_dataset_var.get().strip()
        if not src_dataset or not dst_dataset:
            self._app_log("normal", tr("sync_missing_selection"))
            return
        if "@" in src_dataset or "@" in dst_dataset:
            self._app_log("normal", tr("sync_snapshot_selected"))
            return
        if src_selection not in self.dataset_pool_options or dst_selection not in self.dataset_pool_options:
            self._app_log("normal", tr("sync_invalid_pool"))
            return

        src_conn_id, _src_pool = self.dataset_pool_options[src_selection]
        dst_conn_id, _dst_pool = self.dataset_pool_options[dst_selection]
        src_profile = self.store.get(src_conn_id)
        dst_profile = self.store.get(dst_conn_id)
        if not src_profile or not dst_profile:
            self._app_log("normal", tr("sync_invalid_pool"))
            return

        def ssh_target(profile: ConnectionProfile) -> str:
            return f"{profile.username}@{profile.host}" if profile.username else profile.host

        def ssh_prefix(profile: ConnectionProfile) -> str:
            parts: List[str] = _ssh_common_parts(profile, include_key=True)
            parts.append(shlex.quote(ssh_target(profile)))
            return " ".join(parts)

        def sudo_wrap(profile: ConnectionProfile, cmd: str, keep_stdin: bool = False) -> str:
            if profile.conn_type != "SSH" or not profile.use_sudo:
                return cmd
            if profile.password:
                if keep_stdin:
                    askpass_line = "printf '%s\\n' " + shlex.quote(profile.password)
                    return (
                        "ask=$(mktemp); "
                        "trap 'rm -f \"$ask\"' EXIT; "
                        "{ printf '%s\\n' '#!/bin/sh'; "
                        f"printf '%s\\n' {shlex.quote(askpass_line)}; "
                        "} >\"$ask\"; "
                        "chmod 700 \"$ask\"; "
                        "SUDO_ASKPASS=\"$ask\" sudo -A -p '' sh -lc "
                        f"{shlex.quote(cmd)}"
                    )
                return (
                    f"printf '%s\\n' {shlex.quote(profile.password)} | "
                    f"sudo -S -p '' sh -lc {shlex.quote(cmd)}"
                )
            return f"sudo -n sh -lc {shlex.quote(cmd)}"

        def run_on(profile: ConnectionProfile, cmd: str) -> str:
            if profile.conn_type == "LOCAL":
                return cmd
            if profile.conn_type == "SSH":
                return f"{ssh_prefix(profile)} {shlex.quote(cmd)}"
            if profile.conn_type == "PSRP":
                target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or (5986 if profile.use_ssl else 5985)}"
                return f"# PSRP {target} :: {cmd}"
            return f"# {tr('sync_unsupported_conn')} {profile.name} ({profile.conn_type})"

        tag = uuid.uuid4().hex[:8]
        src_tmp = f"/tmp/zfsmgr-sync-src-{tag}"
        dst_tmp = f"/tmp/zfsmgr-sync-dst-{tag}"
        src_q = shlex.quote(src_dataset)
        dst_q = shlex.quote(dst_dataset)
        src_row = self._find_selected_dataset_row("origin", src_dataset)
        dst_row = self._find_selected_dataset_row("dest", dst_dataset)

        def can_use_current_mountpoint(row: Optional[Dict[str, str]]) -> bool:
            if not row:
                return False
            mounted = (row.get("mounted", "") or "").strip().lower()
            mountpoint = (row.get("mountpoint", "") or "").strip()
            return mounted in {"yes", "on", "true"} and bool(mountpoint) and mountpoint.lower() != "none"

        src_use_current = can_use_current_mountpoint(src_row)
        dst_use_current = can_use_current_mountpoint(dst_row)
        src_sync_base = (src_row or {}).get("mountpoint", "").strip() if src_use_current else src_tmp
        dst_sync_base = (dst_row or {}).get("mountpoint", "").strip() if dst_use_current else dst_tmp

        pre_steps: List[str] = []
        post_steps: List[str] = []

        if not src_use_current:
            pre_steps.extend(
                [
                    run_on(src_profile, sudo_wrap(src_profile, f"mkdir -p {shlex.quote(src_tmp)}")),
                    run_on(src_profile, sudo_wrap(src_profile, f"mount -t zfs {src_q} {shlex.quote(src_tmp)} || mount -t zfs -o zfsutil {src_q} {shlex.quote(src_tmp)}")),
                ]
            )
            post_steps.extend(
                [
                    run_on(src_profile, sudo_wrap(src_profile, f"umount {shlex.quote(src_tmp)} || true")),
                    run_on(src_profile, sudo_wrap(src_profile, f"rmdir {shlex.quote(src_tmp)} || true")),
                ]
            )

        if not dst_use_current:
            pre_steps.extend(
                [
                    run_on(dst_profile, sudo_wrap(dst_profile, f"mkdir -p {shlex.quote(dst_tmp)}")),
                    run_on(dst_profile, sudo_wrap(dst_profile, f"mount -t zfs {dst_q} {shlex.quote(dst_tmp)} || mount -t zfs -o zfsutil {dst_q} {shlex.quote(dst_tmp)}")),
                ]
            )
            post_steps.extend(
                [
                    run_on(dst_profile, sudo_wrap(dst_profile, f"umount {shlex.quote(dst_tmp)} || true")),
                    run_on(dst_profile, sudo_wrap(dst_profile, f"rmdir {shlex.quote(dst_tmp)} || true")),
                ]
            )

        use_windows_fallback = src_profile.os_type == "Windows" or dst_profile.os_type == "Windows"
        if use_windows_fallback:
            # Si origen y destino son el mismo host Windows, usamos robocopy.
            # En otros casos con Windows mantenemos fallback por stream tar.
            same_windows_host = (
                src_conn_id == dst_conn_id
                and src_profile.os_type == "Windows"
                and dst_profile.os_type == "Windows"
            )
            if same_windows_host:
                self._app_log("info", tr("sync_windows_fallback_robocopy"))
                robocopy_raw = (
                    f"robocopy {shlex.quote(src_sync_base)} {shlex.quote(dst_sync_base)} "
                    "/MIR /COPYALL /DCOPY:DAT /R:1 /W:1 /XJ /NP /NFL /NDL"
                )
                rsync_cmd = run_on(src_profile, sudo_wrap(src_profile, robocopy_raw, keep_stdin=False))
            else:
                self._app_log("info", tr("sync_windows_fallback_tar"))
                send_tar_raw = f"tar -cf - -C {shlex.quote(src_sync_base)} ."
                recv_tar_raw = f"tar -xf - -C {shlex.quote(dst_sync_base)}"
                send_tar = run_on(src_profile, sudo_wrap(src_profile, send_tar_raw, keep_stdin=False))
                recv_tar = run_on(dst_profile, sudo_wrap(dst_profile, recv_tar_raw, keep_stdin=True))
                rsync_cmd = f"{send_tar} | {recv_tar}"
        else:
            if src_profile.conn_type == "SSH" and dst_profile.conn_type == "SSH":
                # Se ejecuta en origen, empujando al destino por ssh.
                remote_rsync = (
                    f"rsync -aHAWXS --numeric-ids -e {shlex.quote(ssh_prefix(dst_profile))} "
                    f"{shlex.quote(src_sync_base)}/ {shlex.quote(ssh_target(dst_profile) + ':' + dst_sync_base + '/')}"
                )
                rsync_cmd = run_on(src_profile, sudo_wrap(src_profile, remote_rsync, keep_stdin=True))
            elif src_profile.conn_type == "SSH" and dst_profile.conn_type == "LOCAL":
                rsync_cmd = (
                    f"rsync -aHAWXS --numeric-ids -e {shlex.quote(ssh_prefix(src_profile))} "
                    f"{shlex.quote(ssh_target(src_profile) + ':' + src_sync_base + '/')} {shlex.quote(dst_sync_base + '/')}"
                )
            elif src_profile.conn_type == "LOCAL" and dst_profile.conn_type == "SSH":
                rsync_cmd = (
                    f"rsync -aHAWXS --numeric-ids -e {shlex.quote(ssh_prefix(dst_profile))} "
                    f"{shlex.quote(src_sync_base + '/')} {shlex.quote(ssh_target(dst_profile) + ':' + dst_sync_base + '/')}"
                )
            else:
                rsync_cmd = f"rsync -aHAWXS --numeric-ids {shlex.quote(src_sync_base + '/')} {shlex.quote(dst_sync_base + '/')}"

        self._app_log("normal", trf("sync_plan_generated", src=src_dataset, dst=dst_dataset))
        self._app_log("normal", tr("sync_plan_header"))
        sync_lines = [*pre_steps, rsync_cmd, *post_steps]
        for line in sync_lines:
            self._app_log("normal", line)
        for line in sync_lines:
            self._log_action_subcommands("Sincronizar", line)

    def _ask_multi_select(self, title: str, prompt: str, items: List[str]) -> Optional[List[str]]:
        if not items:
            return []
        dlg = MultiSelectDialog(self, title=title, prompt=prompt, items=items, preselect_all=True)
        self.wait_window(dlg)
        return dlg.result

    def _list_breakdown_dirs(self, profile: ConnectionProfile, dataset_name: str, mountpoint_hint: str) -> List[str]:
        execu = make_executor(profile)
        if isinstance(execu, PSRPExecutor):
            ds_q = "'" + dataset_name.replace("'", "''") + "'"
            mp_q = "'" + mountpoint_hint.replace("'", "''") + "'"
            script = (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $mpHint={mp_q}; "
                "$mp=''; "
                "try { "
                "  $mountedRows = zfs mount; "
                "  foreach ($line in $mountedRows) { "
                "    $parts = ($line -split '\\s+'); "
                "    if ($parts.Length -ge 2 -and $parts[0] -eq $dataset) { $mp = $parts[1]; break } "
                "  } "
                "} catch { $mp='' }; "
                "if ([string]::IsNullOrWhiteSpace($mp)) { "
                "  try { $mp=(zfs get -H -o value mountpoint $dataset).Trim() } catch { $mp='' } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or $mp -eq 'none') { $mp = $mpHint }; "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { return }; "
                "Get-ChildItem -LiteralPath $mp -Directory -Force -ErrorAction SilentlyContinue | "
                "  Sort-Object Name | ForEach-Object { $_.Name }"
            )
            out = execu._run_ps(script, timeout_seconds=REFRESH_TIMEOUT_SECONDS)
        elif isinstance(execu, (SSHExecutor, LocalExecutor)):
            cmd = (
                "set -e; "
                f"DATASET={shlex.quote(dataset_name)}; MP_HINT={shlex.quote(mountpoint_hint)}; "
                "TMP_MP=''; TEMP_MOUNTED=0; "
                "cleanup(){ "
                "if [ \"$TEMP_MOUNTED\" = '1' ] && [ -n \"$TMP_MP\" ]; then "
                "if umount \"$TMP_MP\" >/dev/null 2>&1; then "
                "echo '__ZFSMGR_EVT__|UMOUNT|ok|'\"$TMP_MP\"; "
                "else "
                "echo '__ZFSMGR_EVT__|UMOUNT|error|'\"$TMP_MP\"; "
                "fi; "
                "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
                "fi; "
                "}; "
                "trap cleanup EXIT INT TERM; "
                "ACTIVE_MP=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds {print $2; exit}')\"; "
                "ORIG_MP=\"$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true)\"; "
                "[ -n \"$ORIG_MP\" ] && [ \"$ORIG_MP\" != \"none\" ] || ORIG_MP=\"$MP_HINT\"; "
                "MP=''; [ -n \"$ACTIVE_MP\" ] && MP=\"$ACTIVE_MP\"; "
                "if [ -z \"$MP\" ] && [ \"$(uname -s 2>/dev/null || true)\" = 'Linux' ]; then "
                "TMP_MP=\"$(mktemp -d /tmp/zfsmgr-breakdown-ls-XXXXXX 2>/dev/null || true)\"; "
                "if [ -n \"$TMP_MP\" ]; then "
                "if mount -t zfs -o ro,zfsutil \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1 || mount -t zfs -o ro \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1; then "
                "MP=\"$TMP_MP\"; TEMP_MOUNTED=1; "
                "echo '__ZFSMGR_EVT__|MOUNT|ok|'\"$DATASET\"'|'\"$TMP_MP\"; "
                "fi; "
                "fi; "
                "fi; "
                "if [ -z \"$MP\" ]; then MP=\"$ORIG_MP\"; fi; "
                "[ -n \"$MP\" ] || exit 0; "
                "[ -d \"$MP\" ] || exit 0; "
                "find \"$MP\" -mindepth 1 -maxdepth 1 -type d -printf '%f\\n' | sort -u"
            )
            out = execu._run(cmd, sudo=bool(profile.use_sudo), timeout_seconds=REFRESH_TIMEOUT_SECONDS)
        else:
            return []
        result: List[str] = []
        for raw in (out or "").splitlines():
            line = (raw or "").strip()
            if not line:
                continue
            if line.startswith("__ZFSMGR_EVT__|"):
                parts = line.split("|")
                evt = parts[1] if len(parts) > 1 else ""
                status = parts[2] if len(parts) > 2 else ""
                if evt == "MOUNT":
                    ds = parts[3] if len(parts) > 3 else dataset_name
                    mp = parts[4] if len(parts) > 4 else ""
                    self._app_log("normal", f"mount temporal {status}: {ds} -> {mp}")
                    continue
                if evt == "UMOUNT":
                    mp = parts[3] if len(parts) > 3 else ""
                    self._app_log("normal", f"umount temporal {status}: {mp}")
                    continue
            result.append(line)
        return list(dict.fromkeys(result))

    def _list_assemble_children(self, conn_id: str, pool_name: str, dataset_name: str, profile: ConnectionProfile) -> List[str]:
        cache_key = f"{conn_id}:{pool_name}"
        rows = self.datasets_cache.get(cache_key, [])
        if not rows:
            execu = make_executor(profile)
            rows = execu.list_datasets(pool_name)
            self.datasets_cache[cache_key] = rows
        base_depth = dataset_name.count("/")
        children: List[str] = []
        pref = dataset_name + "/"
        for row in rows:
            name = (row.get("name", "") or "").strip()
            if not name or "@" in name:
                continue
            if not name.startswith(pref):
                continue
            if name.count("/") != base_depth + 1:
                continue
            children.append(name)
        children.sort()
        return children

    def _list_breakdown_existing_child_dataset_names(self, profile: ConnectionProfile, dataset_name: str) -> List[str]:
        pool_name = dataset_name.split("/", 1)[0].strip()
        if not pool_name:
            return []
        try:
            execu = make_executor(profile)
            rows = execu.list_datasets(pool_name)
        except Exception:
            return []
        pref = dataset_name + "/"
        base_depth = dataset_name.count("/")
        children: List[str] = []
        for row in rows:
            name = (row.get("name", "") or "").strip()
            if not name or "@" in name:
                continue
            if not name.startswith(pref):
                continue
            if name.count("/") != base_depth + 1:
                continue
            children.append(name.rsplit("/", 1)[1])
        return list(dict.fromkeys(children))

    def _breakdown_dataset_plan(self) -> None:
        if self._reject_if_ssh_busy():
            return
        target = self._get_dataset_for_create()
        if not target:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        side, selection_label, dataset_name, conn_id = target
        if "@" in dataset_name:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        profile = self.store.get(conn_id)
        if not profile:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        row = self._find_selected_dataset_row(side, dataset_name)
        mountpoint = ((row or {}).get("mountpoint", "") or "").strip()
        if profile.conn_type not in {"LOCAL", "SSH", "PSRP"}:
            self._app_log("normal", trf("log_breakdown_transport_unsupported", ctype=profile.conn_type))
            return
        try:
            dir_candidates = self._list_breakdown_dirs(profile, dataset_name, mountpoint)
        except Exception as exc:
            self._app_log("normal", trf("log_breakdown_exec_runtime_error", error=exc))
            self.after(0, lambda e=exc: messagebox.showerror(tr("datasets_breakdown_btn"), str(e)))
            return
        # Excluir directorios que ya son datasets hijos directos; no deben mostrarse
        # como candidatos de desglosar.
        existing_children = set(self._list_breakdown_existing_child_dataset_names(profile, dataset_name))
        if existing_children:
            dir_candidates = [d for d in dir_candidates if d not in existing_children]
        if not dir_candidates:
            msg = f"No hay directorios para desglosar en {profile.name}::{dataset_name}"
            self._app_log("normal", msg)
            self.after(0, lambda m=msg: messagebox.showinfo(tr("datasets_breakdown_btn"), m))
            return
        selected_dirs = self._ask_multi_select(
            title=tr("datasets_breakdown_btn"),
            prompt=f"Selecciona directorios a desglosar en {profile.name}::{dataset_name}",
            items=dir_candidates,
        )
        if selected_dirs is None:
            self._app_log("info", "Desglosar cancelado por usuario")
            return
        selected_dirs = [d.strip() for d in selected_dirs if d.strip()]
        if not selected_dirs:
            self._app_log("info", "Desglosar cancelado: no hay directorios seleccionados")
            return

        def _wrap_for_profile(raw_cmd: str) -> str:
            if profile.conn_type == "LOCAL":
                base = f"sh -lc {shlex.quote(raw_cmd)}"
                if profile.use_sudo:
                    return f"sudo -n {base}"
                return base
            ssh_parts: List[str] = _ssh_common_parts(profile, include_key=True)
            target_host = profile.host
            if profile.username:
                target_host = f"{profile.username}@{profile.host}"
            remote = f"sh -lc {shlex.quote(raw_cmd)}"
            if profile.use_sudo:
                if profile.password:
                    remote = f"printf '%s\\n' {shlex.quote(profile.password)} | sudo -S -p '' {remote}"
                else:
                    remote = f"sudo -n {remote}"
            ssh_parts.append(shlex.quote(target_host))
            ssh_parts.append(shlex.quote(remote))
            return " ".join(ssh_parts)

        def _ps_quote(value: str) -> str:
            return "'" + (value or "").replace("'", "''") + "'"

        def _build_psrp_breakdown_script(dataset: str, mp_hint: str, selected_names: List[str]) -> str:
            ds_q = _ps_quote(dataset)
            mp_q = _ps_quote(mp_hint)
            sel_ps = ", ".join(_ps_quote(x) for x in selected_names)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $mpHint={mp_q}; "
                f"$selected=@({sel_ps}); "
                "$tmpSuffix = ($dataset -replace '[^A-Za-z0-9_\\-]', '_'); "
                "$tmpRoot = Join-Path $env:TEMP ('zfsmgr-breakdown-' + $tmpSuffix); "
                "New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null; "
                "try { $origMp = (zfs get -H -o value mountpoint $dataset).Trim() } catch { $origMp = '' }; "
                "if ([string]::IsNullOrWhiteSpace($origMp) -or $origMp -eq 'none') { $origMp = $mpHint }; "
                "if ([string]::IsNullOrWhiteSpace($origMp) -or $origMp -eq 'none') { throw \"[BREAKDOWN][ERROR] mountpoint invalid for $dataset\" }; "
                "$activeMp=''; "
                "try { "
                "  $mountedRows = zfs mount; "
                "  foreach ($line in $mountedRows) { "
                "    $parts = ($line -split '\\s+'); "
                "    if ($parts.Length -ge 2 -and $parts[0] -eq $dataset) { $activeMp = $parts[1]; break } "
                "  } "
                "} catch { $activeMp='' }; "
                "$tempMounted=$false; "
                "$mp = if ($activeMp -and (Test-Path -LiteralPath $activeMp)) { $activeMp } else { $origMp }; "
                "if (-not (Test-Path -LiteralPath $mp)) { "
                "  $tmpMp = Join-Path $tmpRoot '__dataset_root'; "
                "  New-Item -ItemType Directory -Path $tmpMp -Force | Out-Null; "
                "  try { zfs unmount $dataset *> $null } catch {}; "
                "  zfs set (\"mountpoint=\" + $tmpMp) $dataset | Out-Null; "
                "  zfs mount $dataset | Out-Null; "
                "  $mp = $tmpMp; "
                "  $tempMounted = $true; "
                "} "
                "$dirs = @(Get-ChildItem -LiteralPath $mp -Directory -Force -ErrorAction SilentlyContinue); "
                "if ($dirs.Count -eq 0) { Write-Output (\"[BREAKDOWN] no subdirectories found in {0}\" -f $mp) } "
                "foreach ($d in $dirs) { "
                "  if ($selected.Count -gt 0 -and -not ($selected -contains $d.Name)) { continue } "
                "  $n = $d.Name; "
                "  $safe = ($n -replace '[^A-Za-z0-9_.:-]', '_'); "
                "  if ([string]::IsNullOrWhiteSpace($safe)) { $safe = 'dir' }; "
                "  $child = \"$dataset/$safe\"; "
                "  $idx = 1; "
                "  while ($true) { "
                "    try { zfs list -H -o name $child *> $null; $child = \"$dataset/$safe-$idx\"; $idx++ } "
                "    catch { break } "
                "  } "
                "  $tmp = Join-Path $tmpRoot ($child.Substring($dataset.Length + 1) -replace '[\\\\/:*?\"\"<>|]', '_'); "
                "  zfs create -o (\"mountpoint=\" + $tmp) $child | Out-Null; "
                "  New-Item -ItemType Directory -Path $tmp -Force | Out-Null; "
                "  if (Get-Command robocopy -ErrorAction SilentlyContinue) { "
                "    & robocopy $d.FullName $tmp /E /MOVE /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "  } else { "
                "    Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue | "
                "      ForEach-Object { Move-Item -LiteralPath $_.FullName -Destination $tmp -Force -ErrorAction Stop }; "
                "  } "
                "  $childFinalMp = Join-Path $origMp $n; "
                "  zfs set (\"mountpoint=\" + $childFinalMp) $child | Out-Null; "
                "  try { zfs unmount $child *> $null } catch {}; "
                "  try { zfs mount $child *> $null } catch {}; "
                "  Write-Output (\"[BREAKDOWN] {0} -> {1} (mp={2})\" -f $d.FullName, $child, $childFinalMp); "
                "} "
                "if ($tempMounted) { "
                "  try { zfs unmount $dataset *> $null } catch {}; "
                "  try { zfs set (\"mountpoint=\" + $origMp) $dataset | Out-Null } catch {}; "
                "} "
            )

        dataset_q = shlex.quote(dataset_name)
        mount_q = shlex.quote(mountpoint)
        selected_dirs_blob = shlex.quote("\n".join(selected_dirs))
        breakdown_cmd = (
            "set -e; "
            f"DATASET={dataset_q}; MP_HINT={mount_q}; "
            f"SELECTED_DIRS_NL={selected_dirs_blob}; "
            "is_selected_dir() { printf '%s\\n' \"$SELECTED_DIRS_NL\" | grep -F -x -q -- \"$1\"; }; "
            "TMP_SUFFIX=\"$(printf '%s' \"$DATASET\" | tr '/' '_')\"; "
            "TMP_ROOT=\"/tmp/zfsmgr-breakdown-$TMP_SUFFIX\"; "
            "ORIG_MP=\"$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true)\"; "
            "[ -n \"$ORIG_MP\" ] && [ \"$ORIG_MP\" != \"none\" ] || ORIG_MP=\"$MP_HINT\"; "
            "mkdir -p \"$TMP_ROOT\"; "
            "MOUNTED=\"$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null || true)\"; "
            "ACTIVE_MP=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds {print $2; exit}')\"; "
            "WAS_MOUNTED=0; "
            "if [ \"$MOUNTED\" = \"yes\" ] || [ -n \"$ACTIVE_MP\" ]; then WAS_MOUNTED=1; fi; "
            "TMP_MP=\"$TMP_ROOT/__dataset_root\"; "
            "mkdir -p \"$TMP_MP\"; "
            "TEMP_KIND=''; "
            "if mount -t zfs \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1 || mount -t zfs -o zfsutil \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1; then "
            "MP=\"$TMP_MP\"; TEMP_KIND='manual'; "
            "else "
            "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
            "ACTIVE_RETRY=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds {print $2; exit}')\"; "
            "[ -n \"$ACTIVE_RETRY\" ] && [ -d \"$ACTIVE_RETRY\" ] || { echo \"[BREAKDOWN][ERROR] cannot mount dataset: $DATASET\"; exit 33; }; "
            "MP=\"$ACTIVE_RETRY\"; TEMP_KIND='zfs'; "
            "fi; "
            "TEMP_MOUNTED=1; "
            "cleanup() { "
            "rc=$?; "
            "if [ \"$TEMP_MOUNTED\" = \"1\" ]; then "
            "if [ \"$TEMP_KIND\" = \"manual\" ]; then "
            "umount \"$TMP_MP\" >/dev/null 2>&1 || true; "
            "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
            "elif [ \"$TEMP_KIND\" = \"zfs\" ] && [ \"$WAS_MOUNTED\" != \"1\" ]; then "
            "zfs unmount \"$DATASET\" >/dev/null 2>&1 || true; "
            "fi; "
            "fi; "
            "exit \"$rc\"; "
            "}; "
            "trap cleanup EXIT INT TERM; "
            "[ -d \"$MP\" ] || { echo \"[BREAKDOWN][ERROR] active mountpoint does not exist: $MP\"; exit 23; }; "
            "printf '[BREAKDOWN] dataset=%s mounted=%s original_mp=%s active_mp=%s\\n' \"$DATASET\" \"$MOUNTED\" \"$ORIG_MP\" \"$MP\"; "
            "found=0; "
            "for d in \"$MP\"/*; do "
            "[ -d \"$d\" ] || continue; "
            "{ "
            "n=\"$(basename \"$d\")\"; "
            "is_selected_dir \"$n\" || continue; "
            "found=1; "
            "safe=\"$(printf '%s' \"$n\" | tr ' ' '_' | tr -cd 'A-Za-z0-9_.:-')\"; "
            "[ -n \"$safe\" ] || safe=\"dir\"; "
            "child=\"$DATASET/$safe\"; "
            "tmp=\"$TMP_ROOT/$safe\"; "
            "i=1; "
            "while zfs list -H -o name \"$child\" >/dev/null 2>&1; do "
            "child=\"$DATASET/${safe}-$i\"; "
            "tmp=\"$TMP_ROOT/${safe}-$i\"; "
            "i=$((i+1)); "
            "done; "
            "zfs create -u \"$child\"; "
            "CHILD_MP=''; CHILD_TEMP=0; "
            "if mount -t zfs \"$child\" \"$tmp\" >/dev/null 2>&1 || mount -t zfs -o zfsutil \"$child\" \"$tmp\" >/dev/null 2>&1; then "
            "CHILD_MP=\"$tmp\"; CHILD_TEMP=1; "
            "else "
            "zfs mount \"$child\" >/dev/null 2>&1 || true; "
            "CHILD_MP=\"$(zfs mount 2>/dev/null | awk -v ds=\"$child\" '$1==ds {print $2; exit}')\"; "
            "[ -n \"$CHILD_MP\" ] && [ -d \"$CHILD_MP\" ] || { echo \"[BREAKDOWN][ERROR] cannot mount child dataset: $child\"; continue; }; "
            "fi; "
            "rsync -aHAWXS --remove-source-files \"$d\"/ \"$CHILD_MP\"/; "
            "find \"$d\" -mindepth 1 -type d -empty -delete; "
            "if [ \"$CHILD_TEMP\" = \"1\" ]; then umount \"$tmp\" >/dev/null 2>&1 || true; rmdir \"$tmp\" >/dev/null 2>&1 || true; fi; "
            "child_mp_final=\"$(zfs get -H -o value mountpoint \"$child\" 2>/dev/null || true)\"; "
            "zfs mount \"$child\" >/dev/null 2>&1 || true; "
            "printf '[BREAKDOWN] %s -> %s (mp=%s)\\n' \"$d\" \"$child\" \"$child_mp_final\"; "
            "} || { "
            "printf '[BREAKDOWN][ERROR] failed processing %s\\n' \"$d\"; "
            "continue; "
            "}; "
            "done; "
            "if [ \"$found\" = \"0\" ]; then "
            "echo \"[BREAKDOWN] no subdirectories found in $MP\"; "
            "fi"
        )
        if profile.conn_type == "PSRP":
            ps_cmd = _build_psrp_breakdown_script(dataset_name, mountpoint, selected_dirs)
            self._app_log(
                "normal",
                trf("log_breakdown_plan_generated", dataset=dataset_name, name=profile.name, selection=selection_label),
            )
            self._app_log("normal", tr("log_breakdown_plan_header"))
            self._app_log("normal", ps_cmd)
            self._run_breakdown_psrp_command(ps_cmd, profile, conn_id=conn_id)
            return

        wrapped_cmd = _wrap_for_profile(breakdown_cmd)
        self._app_log(
            "normal",
            trf("log_breakdown_plan_generated", dataset=dataset_name, name=profile.name, selection=selection_label),
        )
        self._app_log("normal", tr("log_breakdown_plan_header"))
        self._app_log("normal", wrapped_cmd)
        self._run_breakdown_command(wrapped_cmd, conn_id=conn_id)

    def _assemble_dataset_plan(self) -> None:
        if self._reject_if_ssh_busy():
            return
        target = self._get_dataset_for_create()
        if not target:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        side, selection_label, dataset_name, conn_id = target
        if "@" in dataset_name:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        profile = self.store.get(conn_id)
        if not profile:
            self._app_log("normal", tr("create_dataset_select_required"))
            return
        row = self._find_selected_dataset_row(side, dataset_name)
        mountpoint = ((row or {}).get("mountpoint", "") or "").strip()
        if not mountpoint or mountpoint.lower() == "none":
            self._app_log("normal", trf("log_assemble_invalid_mountpoint", dataset=dataset_name, name=profile.name))
            return
        if profile.conn_type not in {"LOCAL", "SSH", "PSRP"}:
            self._app_log("normal", trf("log_assemble_transport_unsupported", ctype=profile.conn_type))
            return
        pool_name = dataset_name.split("/", 1)[0]
        try:
            child_candidates = self._list_assemble_children(conn_id, pool_name, dataset_name, profile)
        except Exception as exc:
            self._app_log("normal", trf("log_assemble_exec_runtime_error", error=exc))
            self.after(0, lambda e=exc: messagebox.showerror(tr("datasets_assemble_btn"), str(e)))
            return
        if not child_candidates:
            msg = f"No hay subdatasets para ensamblar en {profile.name}::{dataset_name}"
            self._app_log("normal", msg)
            self.after(0, lambda m=msg: messagebox.showinfo(tr("datasets_assemble_btn"), m))
            return
        selected_children = self._ask_multi_select(
            title=tr("datasets_assemble_btn"),
            prompt=f"Selecciona datasets a ensamblar en {profile.name}::{dataset_name}",
            items=child_candidates,
        )
        if selected_children is None:
            self._app_log("info", "Ensamblar cancelado por usuario")
            return
        selected_children = [d.strip() for d in selected_children if d.strip()]
        if not selected_children:
            self._app_log("info", "Ensamblar cancelado: no hay datasets seleccionados")
            return

        def _wrap_for_profile(raw_cmd: str) -> str:
            if profile.conn_type == "LOCAL":
                base = f"sh -lc {shlex.quote(raw_cmd)}"
                if profile.use_sudo:
                    return f"sudo -n {base}"
                return base
            ssh_parts: List[str] = _ssh_common_parts(profile, include_key=True)
            target_host = profile.host
            if profile.username:
                target_host = f"{profile.username}@{profile.host}"
            remote = f"sh -lc {shlex.quote(raw_cmd)}"
            if profile.use_sudo:
                if profile.password:
                    remote = f"printf '%s\\n' {shlex.quote(profile.password)} | sudo -S -p '' {remote}"
                else:
                    remote = f"sudo -n {remote}"
            ssh_parts.append(shlex.quote(target_host))
            ssh_parts.append(shlex.quote(remote))
            return " ".join(ssh_parts)

        def _ps_quote(value: str) -> str:
            return "'" + (value or "").replace("'", "''") + "'"

        def _build_psrp_assemble_script(dataset: str, mp_hint: str, selected_ds: List[str]) -> str:
            ds_q = _ps_quote(dataset)
            mp_q = _ps_quote(mp_hint)
            sel_ps = ", ".join(_ps_quote(x) for x in selected_ds)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $mpHint={mp_q}; "
                f"$selected=@({sel_ps}); "
                "$tmpSuffix = ($dataset -replace '[^A-Za-z0-9_\\-]', '_'); "
                "$tmpRoot = ('/tmp/zfsmgr-assemble-' + $tmpSuffix); "
                "$tmpRootWin = ($env:SystemDrive + ($tmpRoot -replace '/', '\\')); "
                "New-Item -ItemType Directory -Path $tmpRootWin -Force | Out-Null; "
                "try { $origMp = (zfs get -H -o value mountpoint $dataset).Trim() } catch { $origMp = '' }; "
                "if ([string]::IsNullOrWhiteSpace($origMp) -or $origMp -eq 'none') { $origMp = $mpHint }; "
                "if ([string]::IsNullOrWhiteSpace($origMp) -or $origMp -eq 'none') { throw \"[ASSEMBLE][ERROR] mountpoint invalid for $dataset\" }; "
                "$activeMp=''; "
                "try { "
                "  $mountedRows = zfs mount; "
                "  foreach ($line in $mountedRows) { "
                "    $parts = ($line -split '\\s+'); "
                "    if ($parts.Length -ge 2 -and $parts[0] -eq $dataset) { $activeMp = $parts[1]; break } "
                "  } "
                "} catch { $activeMp='' }; "
                "$tempMounted=$false; "
                "$mp = if ($activeMp -and (Test-Path -LiteralPath $activeMp)) { $activeMp } else { $origMp }; "
                "if (-not (Test-Path -LiteralPath $mp)) { "
                "  $tmpMp = Join-Path $tmpRoot '__dataset_root'; "
                "  New-Item -ItemType Directory -Path $tmpMp -Force | Out-Null; "
                "  try { zfs unmount $dataset *> $null } catch {}; "
                "  zfs set (\"mountpoint=\" + $tmpMp) $dataset | Out-Null; "
                "  zfs mount $dataset | Out-Null; "
                "  $mp = $tmpMp; "
                "  $tempMounted = $true; "
                "} "
                "Write-Output (\"[ASSEMBLE] dataset={0} original_mp={1} active_mp={2}\" -f $dataset, $origMp, $mp); "
                "$baseDepth = ($dataset -split '/').Count; "
                "$children = @(zfs list -H -o name -r $dataset | Where-Object { $_ -like ($dataset + '/*') -and $_ -notmatch '@' -and (($_ -split '/').Count -eq ($baseDepth + 1)) }); "
                "if ($selected.Count -gt 0) { $children = @($children | Where-Object { $selected -contains $_ }) }; "
                "if ($children.Count -eq 0) { Write-Output (\"[ASSEMBLE] no child datasets found in {0}\" -f $dataset); return }; "
                "$children = $children | Sort-Object { ($_ -split '/').Count } -Descending; "
                "foreach ($child in $children) { "
                "  $rel = $child.Substring($dataset.Length + 1); "
                "  $dest = Join-Path $mp $rel; "
                "  New-Item -ItemType Directory -Path $dest -Force | Out-Null; "
                "  $safeRel = ($rel -replace '[\\\\/:*?\"\"<>|]', '_'); "
                "  $childTmp = Join-Path $tmpRoot $safeRel; "
                "  $suffix=1; "
                "  while (Test-Path -LiteralPath $childTmp) { $childTmp = Join-Path $tmpRoot ($safeRel + '-' + $suffix); $suffix++ }; "
                "  New-Item -ItemType Directory -Path $childTmp -Force | Out-Null; "
                "  $childMpOrig=''; "
                "  try { $childMpOrig = (zfs get -H -o value mountpoint $child).Trim() } catch { $childMpOrig='' }; "
                "  try { zfs unmount $child *> $null } catch {}; "
                "  zfs set (\"mountpoint=\" + $childTmp) $child | Out-Null; "
                "  zfs mount $child | Out-Null; "
                "  $copied = $false; "
                "  if (Get-Command robocopy -ErrorAction SilentlyContinue) { "
                "    & robocopy $childTmp $dest /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "    if ($LASTEXITCODE -le 7) { $copied = $true } "
                "  } "
                "  if (-not $copied) { "
                "    if (Test-Path -LiteralPath $childTmp) { "
                "      Get-ChildItem -LiteralPath $childTmp -Force -ErrorAction SilentlyContinue | "
                "        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dest -Recurse -Force -ErrorAction Stop } "
                "    } "
                "  } "
                "  try { zfs unmount $child *> $null } catch {}; "
                "  if (-not [string]::IsNullOrWhiteSpace($childMpOrig) -and $childMpOrig -ne 'none') { "
                "    try { zfs set (\"mountpoint=\" + $childMpOrig) $child | Out-Null } catch {} "
                "  } "
                "  try { Remove-Item -LiteralPath $childTmp -Force -Recurse -ErrorAction SilentlyContinue } catch {}; "
                "  zfs destroy -r $child | Out-Null; "
                "  Write-Output (\"[ASSEMBLE] {0} -> {1} and removed dataset\" -f $child, $dest); "
                "} "
                "if ($tempMounted) { "
                "  try { zfs unmount $dataset *> $null } catch {}; "
                "  try { zfs set (\"mountpoint=\" + $origMp) $dataset | Out-Null } catch {}; "
                "} "
            )

        dataset_q = shlex.quote(dataset_name)
        mount_q = shlex.quote(mountpoint)
        selected_children_blob = shlex.quote("\n".join(selected_children))
        if profile.conn_type == "PSRP":
            ps_cmd = _build_psrp_assemble_script(dataset_name, mountpoint, selected_children)
            self._app_log(
                "normal",
                trf("log_assemble_plan_generated", dataset=dataset_name, name=profile.name, selection=selection_label),
            )
            self._app_log("normal", tr("log_assemble_plan_header"))
            self._app_log("normal", ps_cmd)
            self._run_assemble_psrp_command(ps_cmd, profile, conn_id=conn_id, dataset_name=dataset_name, mountpoint_hint=mountpoint)
            return

        assemble_cmd = (
            "set -e; "
            f"DATASET={dataset_q}; MP_HINT={mount_q}; "
            f"SELECTED_CHILDREN_NL={selected_children_blob}; "
            "is_selected_child() { printf '%s\\n' \"$SELECTED_CHILDREN_NL\" | grep -F -x -q -- \"$1\"; }; "
            "TMP_SUFFIX=\"$(printf '%s' \"$DATASET\" | tr '/' '_')\"; "
            "TMP_ROOT=\"/tmp/zfsmgr-assemble-$TMP_SUFFIX\"; "
            "ORIG_MP=\"$(zfs get -H -o value mountpoint \"$DATASET\" 2>/dev/null || true)\"; "
            "[ -n \"$ORIG_MP\" ] && [ \"$ORIG_MP\" != \"none\" ] || ORIG_MP=\"$MP_HINT\"; "
            "[ -n \"$ORIG_MP\" ] && [ \"$ORIG_MP\" != \"none\" ] || { echo \"[ASSEMBLE][ERROR] mountpoint invalid for $DATASET\"; exit 31; }; "
            "case \"$ORIG_MP\" in /*) ;; *) echo \"[ASSEMBLE][ERROR] mountpoint is not a path: $ORIG_MP\"; exit 32 ;; esac; "
            "mkdir -p \"$TMP_ROOT\"; "
            "MOUNTED=\"$(zfs get -H -o value mounted \"$DATASET\" 2>/dev/null || true)\"; "
            "ACTIVE_MP=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds {print $2; exit}')\"; "
            "TEMP_MOUNTED=0; "
            "MP=\"$ORIG_MP\"; "
            "if [ -n \"$ACTIVE_MP\" ] && [ -d \"$ACTIVE_MP\" ]; then "
            "MP=\"$ACTIVE_MP\"; "
            "elif [ -d \"$ORIG_MP\" ]; then "
            "MP=\"$ORIG_MP\"; "
            "else "
            "TMP_MP=\"$TMP_ROOT/__dataset_root\"; "
            "mkdir -p \"$TMP_MP\"; "
            "if mount -t zfs \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1 || mount -t zfs -o zfsutil \"$DATASET\" \"$TMP_MP\" >/dev/null 2>&1; then "
            "MP=\"$TMP_MP\"; TEMP_KIND='manual'; "
            "else "
            "zfs mount \"$DATASET\" >/dev/null 2>&1 || true; "
            "ACTIVE_RETRY=\"$(zfs mount 2>/dev/null | awk -v ds=\"$DATASET\" '$1==ds {print $2; exit}')\"; "
            "[ -n \"$ACTIVE_RETRY\" ] && [ -d \"$ACTIVE_RETRY\" ] || { echo \"[ASSEMBLE][ERROR] cannot mount dataset: $DATASET\"; exit 33; }; "
            "MP=\"$ACTIVE_RETRY\"; TEMP_KIND='zfs'; "
            "fi; "
            "TEMP_MOUNTED=1; "
            "fi; "
            "cleanup() { "
            "rc=$?; "
            "if [ \"$TEMP_MOUNTED\" = \"1\" ]; then "
            "if [ \"$TEMP_KIND\" = \"manual\" ]; then "
            "umount \"$TMP_MP\" >/dev/null 2>&1 || true; "
            "rmdir \"$TMP_MP\" >/dev/null 2>&1 || true; "
            "elif [ \"$TEMP_KIND\" = \"zfs\" ] && [ \"$MOUNTED\" != \"yes\" ]; then "
            "zfs unmount \"$DATASET\" >/dev/null 2>&1 || true; "
            "fi; "
            "fi; "
            "exit \"$rc\"; "
            "}; "
            "trap cleanup EXIT INT TERM; "
            "[ -d \"$MP\" ] || { echo \"[ASSEMBLE][ERROR] active mountpoint does not exist: $MP\"; exit 33; }; "
            "printf '[ASSEMBLE] dataset=%s mounted=%s original_mp=%s active_mp=%s\\n' \"$DATASET\" \"$MOUNTED\" \"$ORIG_MP\" \"$MP\"; "
            "HAS_CHILD=\"$(zfs list -H -o name -t filesystem,volume -r \"$DATASET\" | awk -v root=\"$DATASET\" 'index($0, root\"/\")==1 {print 1; exit}')\"; "
            "if [ \"$HAS_CHILD\" != \"1\" ]; then "
            "echo \"[ASSEMBLE] no child datasets found in $DATASET\"; "
            "exit 0; "
            "fi; "
            "zfs list -H -o name -t filesystem,volume -r \"$DATASET\" "
            "| awk -v root=\"$DATASET\" 'index($0, root\"/\")==1 { depth=gsub(/\\//, \"/\", $0); printf \"%08d\\t%s\\n\", depth, $0 }' "
            "| sort -r "
            "| cut -f2- "
            "| while IFS= read -r child; do "
            "[ -n \"$child\" ] || continue; "
            "[ \"$child\" = \"$DATASET\" ] && continue; "
            "is_selected_child \"$child\" || continue; "
            "{ "
            "rel=\"${child#\"$DATASET\"/}\"; "
            "dest=\"$MP/$rel\"; "
            "mkdir -p \"$dest\"; "
            "safe_rel=\"$(printf '%s' \"$rel\" | tr '/' '_')\"; "
            "child_tmp=\"$TMP_ROOT/$safe_rel\"; "
            "j=1; "
            "while [ -e \"$child_tmp\" ]; do "
            "child_tmp=\"$TMP_ROOT/${safe_rel}-$j\"; "
            "j=$((j+1)); "
            "done; "
            "CHILD_TEMP=0; "
            "CHILD_PATH=''; "
            "mkdir -p \"$child_tmp\"; "
            "if mount -t zfs \"$child\" \"$child_tmp\" >/dev/null 2>&1 || mount -t zfs -o zfsutil \"$child\" \"$child_tmp\" >/dev/null 2>&1; then "
            "CHILD_PATH=\"$child_tmp\"; CHILD_TEMP=1; "
            "else "
            "zfs mount \"$child\" >/dev/null 2>&1 || true; "
            "child_active_retry=\"$(zfs mount 2>/dev/null | awk -v ds=\"$child\" '$1==ds {print $2; exit}')\"; "
            "[ -n \"$child_active_retry\" ] && [ -d \"$child_active_retry\" ] || { echo \"[ASSEMBLE][ERROR] cannot mount child dataset: $child\"; exit 34; }; "
            "CHILD_PATH=\"$child_active_retry\"; CHILD_TEMP=0; "
            "fi; "
            "[ -d \"$CHILD_PATH\" ] || { echo \"[ASSEMBLE][ERROR] child mount not available: $child\"; exit 34; }; "
            "SRC_REAL=\"$(readlink -f \"$CHILD_PATH\" 2>/dev/null || printf '%s' \"$CHILD_PATH\")\"; "
            "DST_REAL=\"$(readlink -f \"$dest\" 2>/dev/null || printf '%s' \"$dest\")\"; "
            "[ \"$SRC_REAL\" != \"$DST_REAL\" ] || { "
            "echo \"[ASSEMBLE][ERROR] source and destination resolve to same path for $child ($SRC_REAL)\"; "
            "if [ \"$CHILD_TEMP\" = \"1\" ]; then "
            "umount \"$child_tmp\" >/dev/null 2>&1 || true; "
            "rmdir \"$child_tmp\" >/dev/null 2>&1 || true; "
            "fi; "
            "exit 36; "
            "}; "
            "rsync -aHAWXS \"$CHILD_PATH\"/ \"$dest\"/ || { "
            "echo \"[ASSEMBLE][ERROR] rsync failed for $child -> $dest\"; "
            "if [ \"$CHILD_TEMP\" = \"1\" ]; then "
            "umount \"$child_tmp\" >/dev/null 2>&1 || true; "
            "rmdir \"$child_tmp\" >/dev/null 2>&1 || true; "
            "fi; "
            "exit 35; "
            "}; "
            "if [ \"$CHILD_TEMP\" = \"1\" ]; then "
            "umount \"$child_tmp\" >/dev/null 2>&1 || true; "
            "rmdir \"$child_tmp\" >/dev/null 2>&1 || true; "
            "fi; "
            "zfs destroy -r \"$child\"; "
            "printf '[ASSEMBLE] %s -> %s and removed dataset\\n' \"$child\" \"$dest\"; "
            "}; "
            "done"
        )
        wrapped_cmd = _wrap_for_profile(assemble_cmd)
        self._app_log(
            "normal",
            trf("log_assemble_plan_generated", dataset=dataset_name, name=profile.name, selection=selection_label),
        )
        self._app_log("normal", tr("log_assemble_plan_header"))
        self._app_log("normal", wrapped_cmd)
        self._run_assemble_command(wrapped_cmd, conn_id=conn_id)

    def _run_assemble_psrp_command(
        self,
        ps_script: str,
        profile: ConnectionProfile,
        conn_id: Optional[str] = None,
        dataset_name: str = "",
        mountpoint_hint: str = "",
    ) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_assemble_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Ensamblar", ps_script)
        target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or (5986 if profile.use_ssl else 5985)}"
        self._ssh_log(f"{target} $ powershell -NoProfile -NonInteractive -Command <assemble-script>")
        cancel_event = threading.Event()
        self._set_active_dataset_coop_action(tr("datasets_assemble_btn"), cancel_event)

        def _ps_quote(value: str) -> str:
            return "'" + (value or "").replace("'", "''") + "'"

        def _build_children_list_script(dataset_name: str) -> str:
            ds_q = _ps_quote(dataset_name)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; "
                "$baseDepth = ($dataset -split '/').Count; "
                "$children = @(zfs list -H -o name -r $dataset | "
                "Where-Object { $_ -like ($dataset + '/*') -and $_ -notmatch '@' -and (($_ -split '/').Count -eq ($baseDepth + 1)) }); "
                "$children = $children | Sort-Object { ($_ -split '/').Count } -Descending; "
                "$children -join \"`n\""
            )

        def _build_root_prepare_script(dataset_name: str, mountpoint_hint: str) -> str:
            ds_q = _ps_quote(dataset_name)
            mp_q = _ps_quote(mountpoint_hint)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $mpHint={mp_q}; "
                "$tmpSuffix = ($dataset -replace '[^A-Za-z0-9_\\-]', '_'); "
                "$tmpRoot = Join-Path $env:TEMP ('zfsmgr-assemble-' + $tmpSuffix); "
                "New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null; "
                "$datasetMpOrig=''; "
                "try { $datasetMpOrig = (zfs get -H -o value mountpoint $dataset).Trim() } catch { $datasetMpOrig='' }; "
                "$datasetDriveOrig=''; "
                "try { $datasetDriveOrig = (zfs get -H -o value driveletter $dataset).Trim() } catch { $datasetDriveOrig='' }; "
                "$datasetTempMp=''; "
                "$datasetTempDrive=''; "
                "$mp=''; "
                "if (-not [string]::IsNullOrWhiteSpace($mpHint)) { "
                "  if (Test-Path -LiteralPath $mpHint) { $mp = $mpHint } "
                "}; "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  try { "
                "    $mountedRows = zfs mount; "
                "    foreach ($line in $mountedRows) { "
                "      $parts = ($line -split '\\s+'); "
                "      if ($parts.Length -ge 2 -and $parts[0] -eq $dataset) { "
                "        $cand = $parts[1]; "
                "        if ($cand.StartsWith('\\??\\')) { $cand = $cand.Substring(4) }; "
                "        if ($cand -match '^[A-Za-z]:$') { $cand = ($cand + '\\') }; "
                "        if (Test-Path -LiteralPath $cand) { $mp = $cand; break } "
                "      } "
                "    } "
                "  } catch {} "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  if (-not [string]::IsNullOrWhiteSpace($datasetDriveOrig) -and $datasetDriveOrig -notmatch '^(off|none|-)$') { "
                "    $cand = $datasetDriveOrig; "
                "    if ($cand.StartsWith('\\??\\')) { $cand = $cand.Substring(4) }; "
                "    if ($cand -match '^[A-Za-z]:$') { $cand = ($cand + '\\') }; "
                "    if (-not (Test-Path -LiteralPath $cand)) { "
                "      try { zfs mount $dataset | Out-Null } catch {} "
                "    } "
                "    if (Test-Path -LiteralPath $cand) { $mp = $cand } "
                "  } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  $datasetTempMp = ($tmpRoot + '/__dataset_root'); "
                "  $datasetTempMpWin = ($env:SystemDrive + ($datasetTempMp -replace '/', '\\')); "
                "  New-Item -ItemType Directory -Path $datasetTempMpWin -Force | Out-Null; "
                "  try { zfs unmount $dataset *> $null } catch {}; "
                "  try { zfs set driveletter=- $dataset | Out-Null } catch {}; "
                "  try { zfs set (\"mountpoint=\" + $datasetTempMp) $dataset | Out-Null } catch {}; "
                "  try { zfs mount $dataset | Out-Null } catch {}; "
                "  if (Test-Path -LiteralPath $datasetTempMpWin) { $mp = $datasetTempMpWin } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  $used = @{}; "
                "  Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object { $used[$_.Name.ToUpper()] = $true }; "
                "  $letters = @('Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G','F','E','D'); "
                "  $free = $null; "
                "  foreach ($l in $letters) { if (-not $used.ContainsKey($l)) { $free = $l; break } }; "
                "  if ($free) { "
                "    try { zfs set (\"driveletter=\" + $free) $dataset | Out-Null } catch {}; "
                "    try { zfs mount $dataset | Out-Null } catch {}; "
                "    $cand = ($free + ':\\'); "
                "    if (Test-Path -LiteralPath $cand) { $mp = $cand; $datasetTempDrive = $free } "
                "  } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  $used = @{}; "
                "  Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object { $used[$_.Name.ToUpper()] = $true }; "
                "  $letters = @('Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G','F','E','D'); "
                "  $free = $null; "
                "  foreach ($l in $letters) { if (-not $used.ContainsKey($l)) { $free = $l; break } }; "
                "  if ($free) { "
                "    try { zfs set (\"driveletter=\" + $free) $dataset | Out-Null } catch { throw (\"[ASSEMBLE][ROOT] set driveletter failed: \" + $_.Exception.Message) }; "
                "    try { zfs mount $dataset | Out-Null } catch { throw (\"[ASSEMBLE][ROOT] mount failed after set driveletter: \" + $_.Exception.Message) }; "
                "    $cand = ($free + ':\\'); "
                "    if (Test-Path -LiteralPath $cand) { $mp = $cand; $datasetTempDrive = $free } "
                "  } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($mp) -or -not (Test-Path -LiteralPath $mp)) { "
                "  throw \"[ASSEMBLE][ERROR] cannot resolve dataset mount path for $dataset\" "
                "}; "
                "Write-Output ('ROOT_MP=' + $mp); "
                "Write-Output ('TMP_ROOT=' + $tmpRoot); "
                "Write-Output ('DATASET_TEMP_MP=' + $datasetTempMp); "
                "Write-Output ('DATASET_MP_ORIG=' + $datasetMpOrig); "
                "Write-Output ('DATASET_TEMP_DRIVE=' + $datasetTempDrive); "
                "Write-Output ('DATASET_DRIVE_ORIG=' + $datasetDriveOrig); "
            )

        def _build_child_stage_script(dataset_name: str, child_name: str, root_mp: str, tmp_root: str) -> str:
            ds_q = _ps_quote(dataset_name)
            child_q = _ps_quote(child_name)
            root_q = _ps_quote(root_mp)
            tmp_q = _ps_quote(tmp_root)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $child={child_q}; $rootMp={root_q}; $tmpRoot={tmp_q}; "
                "$tmpRootWin = ($env:SystemDrive + ($tmpRoot -replace '/', '\\')); "
                "$rel = $child.Substring($dataset.Length + 1); "
                "$dest = Join-Path $rootMp $rel; "
                "$safeRel = ($rel -replace '[\\\\/:*?\"\"<>|]', '_'); "
                "$stageTmp = Join-Path $tmpRootWin ('stage-' + $safeRel); "
                "New-Item -ItemType Directory -Path $stageTmp -Force | Out-Null; "
                "$childTmp = Join-Path $tmpRootWin ('child-' + $safeRel); "
                "$childTmpMp = ($tmpRoot + '/child-' + $safeRel); "
                "New-Item -ItemType Directory -Path $childTmp -Force | Out-Null; "
                "$childSrc=''; "
                "$childErr=''; "
                "$candInRoot = Join-Path $rootMp $rel; "
                "if (Test-Path -LiteralPath $candInRoot) { $childSrc = $candInRoot } "
                "if ([string]::IsNullOrWhiteSpace($childSrc)) { "
                "  try { zfs mount $child | Out-Null } catch { $childErr = $_.Exception.Message } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($childSrc)) { "
                "  try { "
                "    $mountedRows = zfs mount; "
                "    foreach ($line in $mountedRows) { "
                "      $parts = ($line -split '\\s+'); "
                "      if ($parts.Length -ge 2 -and $parts[0] -eq $child) { "
                "        $cand = $parts[1]; "
                "        if ($cand.StartsWith('\\??\\')) { $cand = $cand.Substring(4) }; "
                "        $cand = ($cand -replace '/', '\\'); "
                "        if ($cand -match '^[A-Za-z]:(?!\\\\)') { $cand = ($cand.Substring(0,2) + '\\' + $cand.Substring(2)) }; "
                "        if ($cand -match '^[A-Za-z]:$') { $cand = ($cand + '\\') }; "
                "        if (Test-Path -LiteralPath $cand) { $childSrc = $cand; break } "
                "      } "
                "    } "
                "  } catch {} "
                "} "
                "if ([string]::IsNullOrWhiteSpace($childSrc)) { "
                "  try { "
                "    $mpProp = (zfs get -H -o value mountpoint $child).Trim(); "
                "    if ($mpProp -and $mpProp -ne 'none' -and $mpProp -ne 'legacy') { "
                "      if ($mpProp -match '^/') { "
                "        $tail = ($mpProp -replace '^/', '' -replace '/', '\\'); "
                "        $drives = Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue; "
                "        foreach ($d in $drives) { "
                "          $cand = ($d.Name + ':\\' + $tail); "
                "          if (Test-Path -LiteralPath $cand) { $childSrc = $cand; break } "
                "        } "
                "      } else { "
                "        $cand = $mpProp; "
                "        if ($cand.StartsWith('\\??\\')) { $cand = $cand.Substring(4) }; "
                "        $cand = ($cand -replace '/', '\\'); "
                "        if ($cand -match '^[A-Za-z]:(?!\\\\)') { $cand = ($cand.Substring(0,2) + '\\' + $cand.Substring(2)) }; "
                "        if ($cand -match '^[A-Za-z]:$') { $cand = ($cand + '\\') }; "
                "        if (Test-Path -LiteralPath $cand) { $childSrc = $cand } "
                "      } "
                "    } "
                "  } catch {} "
                "} "
                "if ([string]::IsNullOrWhiteSpace($childSrc)) { "
                "  try { "
                "    $uok = $false; "
                "    for ($ui=0; $ui -lt 5 -and -not $uok; $ui++) { "
                "      try { zfs unmount -f $child | Out-Null; $uok = $true } catch { Start-Sleep -Milliseconds 300 } "
                "    }; "
                "    try { zfs set driveletter=- $child | Out-Null } catch {}; "
                "    zfs set (\"mountpoint=\" + $childTmpMp) $child | Out-Null; "
                "    zfs mount $child | Out-Null; "
                "    if (Test-Path -LiteralPath $childTmp) { $childSrc = $childTmp } "
                "  } catch { $childErr = $_.Exception.Message } "
                "} "
                "if ([string]::IsNullOrWhiteSpace($childSrc)) { Write-Output ('STAGE_ERROR=' + $childErr); return }; "
                "$copiedStage = $false; "
                "if (Get-Command robocopy -ErrorAction SilentlyContinue) { "
                "  & robocopy $childSrc $stageTmp /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "  if ($LASTEXITCODE -le 7) { $copiedStage = $true } "
                "} "
                "if (-not $copiedStage) { "
                "  Get-ChildItem -LiteralPath $childSrc -Force -ErrorAction SilentlyContinue | "
                "    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stageTmp -Recurse -Force -ErrorAction Stop } "
                "} "
                "try { zfs unmount $child *> $null } catch {}; "
                "$destroyed=$false; "
                "for ($k=0; $k -lt 5 -and -not $destroyed; $k++) { "
                "  try { zfs destroy -r -f $child | Out-Null; $destroyed=$true } catch { "
                "    try { zfs unmount $child *> $null } catch {}; "
                "    Start-Sleep -Milliseconds 400; "
                "  } "
                "} "
                "if (-not $destroyed) { throw \"[ASSEMBLE][ERROR] cannot destroy child dataset after retries: $child\" }; "
                "Write-Output ('STAGE_TMP=' + $stageTmp); "
                "Write-Output ('CHILD_TMP=' + $childTmp); "
                "Write-Output ('DEST_PATH=' + $dest); "
            )

        def _build_child_finalize_script(stage_tmp: str, child_tmp: str, dest_path: str, child_name: str) -> str:
            stage_q = _ps_quote(stage_tmp)
            child_tmp_q = _ps_quote(child_tmp)
            dest_q = _ps_quote(dest_path)
            child_q = _ps_quote(child_name)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$stageTmp={stage_q}; $childTmp={child_tmp_q}; $dest={dest_q}; $child={child_q}; "
                "New-Item -ItemType Directory -Path $dest -Force | Out-Null; "
                "$copiedDest = $false; "
                "if (Get-Command robocopy -ErrorAction SilentlyContinue) { "
                "  & robocopy $stageTmp $dest /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "  if ($LASTEXITCODE -le 7) { $copiedDest = $true } "
                "} "
                "if (-not $copiedDest) { "
                "  if (Test-Path -LiteralPath $stageTmp) { "
                    "    Get-ChildItem -LiteralPath $stageTmp -Force -ErrorAction SilentlyContinue | "
                    "      ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dest -Recurse -Force -ErrorAction Stop } "
                "  } "
                "} "
                "try { Remove-Item -LiteralPath $stageTmp -Force -Recurse -ErrorAction SilentlyContinue } catch {}; "
                "try { Remove-Item -LiteralPath $childTmp -Force -Recurse -ErrorAction SilentlyContinue } catch {}; "
                "Write-Output (\"[ASSEMBLE] {0} -> {1} and removed dataset\" -f $child, $dest); "
            )

        def _build_root_restore_script(
            dataset_name: str,
            dataset_temp_mp: str,
            dataset_mp_orig: str,
            dataset_temp_drive: str,
            dataset_drive_orig: str,
        ) -> str:
            ds_q = _ps_quote(dataset_name)
            dtmp_q = _ps_quote(dataset_temp_mp)
            dorig_q = _ps_quote(dataset_mp_orig)
            dtd_q = _ps_quote(dataset_temp_drive)
            ddo_q = _ps_quote(dataset_drive_orig)
            return (
                "$ErrorActionPreference='Stop'; "
                f"$dataset={ds_q}; $datasetTempMp={dtmp_q}; $datasetMpOrig={dorig_q}; $datasetTempDrive={dtd_q}; $datasetDriveOrig={ddo_q}; "
                "if ($datasetTempMp) { "
                "  try { zfs unmount $dataset | Out-Null } catch {}; "
                "  if ($datasetDriveOrig -and $datasetDriveOrig -notmatch '^(off|none|-)$') { "
                "    try { zfs set (\"driveletter=\" + $datasetDriveOrig) $dataset | Out-Null } catch {} "
                "  } "
                "  if ($datasetMpOrig -and $datasetMpOrig -ne 'none') { "
                "    try { zfs set (\"mountpoint=\" + $datasetMpOrig) $dataset | Out-Null } catch {} "
                "  } else { "
                "    try { zfs inherit mountpoint $dataset | Out-Null } catch {} "
                "  } "
                "}; "
                "if ($datasetTempDrive) { "
                "  try { zfs unmount $dataset | Out-Null } catch {}; "
                "  if ($datasetDriveOrig -and $datasetDriveOrig -notmatch '^(off|none|-)$') { "
                "    try { zfs set (\"driveletter=\" + $datasetDriveOrig) $dataset | Out-Null } catch {} "
                "  } "
                "}; "
            )

        def worker() -> None:
            rc = 1
            cancelled = False
            try:
                execu = make_executor(profile)
                if not isinstance(execu, PSRPExecutor):
                    raise ExecutorError(trf("log_assemble_transport_unsupported", ctype=profile.conn_type))
                target_dataset = (dataset_name or "").strip()
                hint_mountpoint = (mountpoint_hint or "").strip()
                if not target_dataset:
                    for part in (ps_script or "").split(";"):
                        seg = part.strip()
                        if seg.startswith("$dataset="):
                            target_dataset = seg[len("$dataset="):].strip().strip("'").replace("''", "'")
                        if seg.startswith("$mpHint="):
                            hint_mountpoint = seg[len("$mpHint="):].strip().strip("'").replace("''", "'")

                list_script = _build_children_list_script(target_dataset)
                children_out = execu._run_ps(list_script, timeout_seconds=None)
                children = [line.strip() for line in (children_out or "").splitlines() if line.strip()]
                if not children:
                    self._ssh_log(f"[ASSEMBLE] no child datasets found in {target_dataset}")
                    rc = 0
                else:
                    prep_script = _build_root_prepare_script(target_dataset, hint_mountpoint)
                    prep_out = execu._run_ps(prep_script, timeout_seconds=None)
                    prep_vals: Dict[str, str] = {}
                    for line in (prep_out or "").splitlines():
                        if "=" in line:
                            k, v = line.split("=", 1)
                            prep_vals[k.strip()] = v.strip()
                    root_mp = prep_vals.get("ROOT_MP", "")
                    tmp_root = prep_vals.get("TMP_ROOT", "")
                    dataset_temp_mp = prep_vals.get("DATASET_TEMP_MP", "")
                    dataset_mp_orig = prep_vals.get("DATASET_MP_ORIG", "")
                    dataset_temp_drive = prep_vals.get("DATASET_TEMP_DRIVE", "")
                    dataset_drive_orig = prep_vals.get("DATASET_DRIVE_ORIG", "")
                    if not root_mp or not tmp_root:
                        raise ExecutorError("[ASSEMBLE][ERROR] root prepare failed")

                    for child in children:
                        if cancel_event.is_set():
                            cancelled = True
                            break
                        self._ssh_log(f"[ASSEMBLE] processing {child}")
                        stage_script = _build_child_stage_script(target_dataset, child, root_mp, tmp_root)
                        stage_out = execu._run_ps(stage_script, timeout_seconds=None)
                        stage_vals: Dict[str, str] = {}
                        for line in (stage_out or "").splitlines():
                            if "=" in line:
                                k, v = line.split("=", 1)
                                stage_vals[k.strip()] = v.strip()
                        stage_tmp = stage_vals.get("STAGE_TMP", "")
                        child_tmp = stage_vals.get("CHILD_TMP", "")
                        dest_path = stage_vals.get("DEST_PATH", "")
                        if not stage_tmp or not dest_path:
                            stage_err = stage_vals.get("STAGE_ERROR", "").strip()
                            raise ExecutorError(
                                f"[ASSEMBLE][ERROR] stage failed for {child}: {stage_err or (stage_out or '').strip()}"
                            )
                        finalize_script = _build_child_finalize_script(stage_tmp, child_tmp, dest_path, child)
                        out = execu._run_ps(finalize_script, timeout_seconds=None)
                        for line in (out or "").splitlines():
                            txt = (line or "").strip()
                            if txt:
                                self._ssh_log(txt)

                    restore_script = _build_root_restore_script(
                        target_dataset,
                        dataset_temp_mp,
                        dataset_mp_orig,
                        dataset_temp_drive,
                        dataset_drive_orig,
                    )
                    execu._run_ps(restore_script, timeout_seconds=None)
                    rc = 0 if not cancelled else 130
                if cancelled:
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                else:
                    self._app_log("normal", tr("log_assemble_exec_ok"))
            except Exception as exc:
                self._ssh_log(f"[ASSEMBLE][ERROR] {exc}")
                self._app_log("normal", trf("log_assemble_exec_runtime_error", error=exc))
            finally:
                self._clear_active_dataset_coop_action(cancel_event)
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if conn_id and rc == 0 and not cancelled:
                    self.after(0, lambda cid=conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _get_dataset_for_create(self) -> Optional[Tuple[str, str, str, str]]:
        # Devuelve (side, selection_label, dataset, conn_id)
        origin_ds = self.origin_dataset_var.get().strip()
        dest_ds = self.dest_dataset_var.get().strip()
        origin_sel = self.origin_pool_var.get().strip()
        dest_sel = self.dest_pool_var.get().strip()
        candidates: List[Tuple[str, str, str, str]] = []
        if origin_ds and "@" not in origin_ds and origin_sel in self.dataset_pool_options:
            cid, _pool = self.dataset_pool_options[origin_sel]
            candidates.append(("origin", origin_sel, origin_ds, cid))
        if dest_ds and "@" not in dest_ds and dest_sel in self.dataset_pool_options:
            cid, _pool = self.dataset_pool_options[dest_sel]
            candidates.append(("dest", dest_sel, dest_ds, cid))
        if not candidates:
            return None
        if len(candidates) == 1:
            return candidates[0]
        for cand in candidates:
            if cand[0] == self.last_selected_dataset_side:
                return cand
        return candidates[0]

    def _get_target_for_delete(self) -> Optional[Tuple[str, str, str, str]]:
        # Devuelve (side, selection_label, dataset_or_snapshot, conn_id)
        origin_ds = self.origin_dataset_var.get().strip()
        dest_ds = self.dest_dataset_var.get().strip()
        origin_sel = self.origin_pool_var.get().strip()
        dest_sel = self.dest_pool_var.get().strip()
        candidates: List[Tuple[str, str, str, str]] = []
        if origin_ds and origin_sel in self.dataset_pool_options:
            cid, _pool = self.dataset_pool_options[origin_sel]
            candidates.append(("origin", origin_sel, origin_ds, cid))
        if dest_ds and dest_sel in self.dataset_pool_options:
            cid, _pool = self.dataset_pool_options[dest_sel]
            candidates.append(("dest", dest_sel, dest_ds, cid))
        if not candidates:
            return None
        if len(candidates) == 1:
            return candidates[0]
        for cand in candidates:
            if cand[0] == self.last_selected_dataset_side:
                return cand
        return candidates[0]

    def _create_dataset(self) -> None:
        if self._reject_if_ssh_busy():
            return
        selected = self._get_dataset_for_create()
        if not selected:
            messagebox.showwarning(tr("create_dataset_btn"), tr("create_dataset_select_required"))
            return
        side, selection_label, base_dataset, conn_id = selected
        profile = self.store.get(conn_id)
        if not profile:
            return

        initial_path = f"{base_dataset}/{tr('create_dataset_default_name')}"
        dlg = CreateDatasetDialog(self, initial_path=initial_path, base_dataset=base_dataset)
        self.wait_window(dlg)
        if not dlg.result:
            return
        dataset_path = str(dlg.result.get("dataset_path", "")).strip()
        if not dataset_path:
            return

        self._app_log("normal", trf("log_create_dataset_start", name=profile.name, dataset=dataset_path))
        self._ssh_log(f"[ACTION] {trf('log_create_dataset_start', name=profile.name, dataset=dataset_path)}")

        def worker() -> None:
            try:
                execu = make_executor(profile)
                out = execu.create_dataset(dataset_path, dlg.result or {})
                self._app_log("info", trf("log_create_dataset_done", name=profile.name, dataset=dataset_path))
                if (out or "").strip():
                    self._app_log("debug", out.strip())
            except Exception as exc:
                self._app_log("normal", trf("log_create_dataset_error", name=profile.name, dataset=dataset_path, error=exc))
                self.after(0, lambda e=exc: messagebox.showerror(tr("create_dataset_title"), str(e)))
            finally:
                self.datasets_cache = {k: v for k, v in self.datasets_cache.items() if not k.startswith(f"{conn_id}:")}
                self.after(0, lambda: self._refresh_connection_by_id(conn_id))
                self.after(0, lambda: self._load_side_datasets(side))

        threading.Thread(target=worker, daemon=True).start()

    def _modify_dataset(self) -> None:
        if self._reject_if_ssh_busy():
            return
        selected = self._get_dataset_for_create()
        if not selected:
            messagebox.showwarning(tr("modify_dataset_btn"), tr("create_dataset_select_required"))
            return
        side, _selection_label, dataset_path, conn_id = selected
        profile = self.store.get(conn_id)
        if not profile:
            return

        self._app_log("info", trf("log_modify_dataset_load_start", name=profile.name, dataset=dataset_path))

        def worker_load() -> None:
            try:
                execu = make_executor(profile)
                props = execu.list_dataset_properties(dataset_path)
            except Exception as exc:
                self._app_log("normal", trf("log_modify_dataset_load_error", name=profile.name, dataset=dataset_path, error=exc))
                self.after(0, lambda e=exc: messagebox.showerror(tr("modify_dataset_title"), str(e)))
                return

            def _open_dialog() -> None:
                dlg = ModifyDatasetDialog(self, dataset_path=dataset_path, properties=props)
                self.wait_window(dlg)
                payload = dlg.result or {}
                changes = payload.get("changes", {}) if isinstance(payload, dict) else {}
                rename_to = str(payload.get("rename_to", "")).strip() if isinstance(payload, dict) else ""
                if not changes and not rename_to:
                    self._app_log("info", tr("log_modify_dataset_no_changes"))
                    return
                if changes:
                    self._app_log(
                        "normal",
                        trf("log_modify_dataset_apply_start", name=profile.name, dataset=dataset_path, count=len(changes)),
                    )
                if rename_to:
                    self._app_log(
                        "normal",
                        trf("log_modify_dataset_rename_start", name=profile.name, dataset=dataset_path, new_name=rename_to),
                    )

                def worker_apply() -> None:
                    try:
                        execu = make_executor(profile)
                        current_name = dataset_path
                        if changes:
                            out = execu.set_dataset_properties(current_name, changes)
                            self._app_log("info", trf("log_modify_dataset_apply_done", name=profile.name, dataset=current_name))
                            if (out or "").strip():
                                self._app_log("debug", out.strip())
                        if rename_to:
                            out_rename = execu.rename_dataset(current_name, rename_to)
                            self._app_log(
                                "info",
                                trf("log_modify_dataset_rename_done", name=profile.name, dataset=current_name, new_name=rename_to),
                            )
                            if (out_rename or "").strip():
                                self._app_log("debug", out_rename.strip())
                    except Exception as exc:
                        self._app_log(
                            "normal",
                            trf("log_modify_dataset_apply_error", name=profile.name, dataset=dataset_path, error=exc),
                        )
                        self.after(0, lambda e=exc: messagebox.showerror(tr("modify_dataset_title"), str(e)))
                    finally:
                        self.datasets_cache = {k: v for k, v in self.datasets_cache.items() if not k.startswith(f"{conn_id}:")}
                        self.after(0, lambda: self._refresh_connection_by_id(conn_id))
                        self.after(0, lambda: self._load_side_datasets(side))

                threading.Thread(target=worker_apply, daemon=True).start()

            self.after(0, _open_dialog)

        threading.Thread(target=worker_load, daemon=True).start()

    def _delete_dataset(self) -> None:
        if self._reject_if_ssh_busy():
            return
        selected = self._get_target_for_delete()
        if not selected:
            messagebox.showwarning(tr("delete_dataset_btn"), tr("delete_dataset_select_required"))
            return
        side, _selection_label, dataset_path, conn_id = selected
        profile = self.store.get(conn_id)
        if not profile:
            return

        msg1 = trf("delete_dataset_confirm_1", dataset=dataset_path, name=profile.name)
        if not messagebox.askyesno(tr("confirm"), msg1):
            return
        msg2 = trf("delete_dataset_confirm_2", dataset=dataset_path, name=profile.name)
        if not messagebox.askyesno(tr("confirm"), msg2):
            return
        recursive_first = messagebox.askyesno(
            tr("confirm"),
            trf("delete_dataset_recursive_confirm", dataset=dataset_path, name=profile.name),
        )
        if recursive_first:
            self._app_log("normal", trf("log_delete_dataset_recursive_start", name=profile.name, dataset=dataset_path))

        self._app_log("normal", trf("log_delete_dataset_start", name=profile.name, dataset=dataset_path))
        self._ssh_log(f"[ACTION] {trf('log_delete_dataset_start', name=profile.name, dataset=dataset_path)}")

        def worker(recursive: bool = False, prompted_recursive: bool = False) -> None:
            try:
                execu = make_executor(profile)
                out = execu.destroy_dataset(dataset_path, recursive=recursive)

                def _exists_after_delete() -> bool:
                    pool_name = dataset_path.split("/", 1)[0].split("@", 1)[0]
                    rows = execu.list_datasets(pool_name)
                    return any((r.get("name", "").strip() == dataset_path) for r in rows)

                # Verificacion explicita: evitar falsos OK cuando el backend no elimina realmente.
                still_exists = False
                try:
                    still_exists = _exists_after_delete()
                except Exception:
                    # Si no se puede verificar, mantenemos el resultado del comando.
                    still_exists = False

                if still_exists:
                    raise RuntimeError(f"verification failed: dataset still exists ({dataset_path})")

                if recursive:
                    self._app_log("normal", trf("log_delete_dataset_done_recursive", name=profile.name, dataset=dataset_path))
                else:
                    self._app_log("normal", trf("log_delete_dataset_done", name=profile.name, dataset=dataset_path))
                if (out or "").strip():
                    self._app_log("debug", out.strip())
            except Exception as exc:
                err_txt = str(exc)
                lower_err = err_txt.lower()
                needs_recursive = (not recursive) and (
                    "has children" in lower_err
                    or "tiene hijos" in lower_err
                    or "use '-r'" in lower_err
                    or "use '-r' to destroy" in lower_err
                    or "still exists" in lower_err
                )
                if needs_recursive and not prompted_recursive:
                    self._app_log("normal", trf("log_delete_dataset_needs_recursive", name=profile.name, dataset=dataset_path))

                    def _ask_recursive() -> None:
                        ask = messagebox.askyesno(
                            tr("confirm"),
                            trf("delete_dataset_recursive_confirm", dataset=dataset_path, name=profile.name),
                        )
                        if ask:
                            self._app_log("normal", trf("log_delete_dataset_recursive_start", name=profile.name, dataset=dataset_path))
                            threading.Thread(target=lambda: worker(True, True), daemon=True).start()
                        else:
                            self._app_log("normal", trf("log_delete_dataset_error", name=profile.name, dataset=dataset_path, error=err_txt))

                    self.after(0, _ask_recursive)
                    return
                if needs_recursive and prompted_recursive:
                    self._app_log("normal", trf("log_delete_dataset_needs_recursive", name=profile.name, dataset=dataset_path))
                self._app_log("normal", trf("log_delete_dataset_error", name=profile.name, dataset=dataset_path, error=err_txt))
                self.after(0, lambda: messagebox.showerror(tr("delete_dataset_btn"), err_txt))
            finally:
                self.datasets_cache = {k: v for k, v in self.datasets_cache.items() if not k.startswith(f"{conn_id}:")}
                self.after(0, lambda: self._refresh_connection_by_id(conn_id))
                self.after(0, lambda: self._load_side_datasets(side))

        threading.Thread(target=lambda: worker(recursive_first, True), daemon=True).start()

    def _run_level_psrp_command(
        self,
        ps_script: str,
        profile: ConnectionProfile,
        affected_conn_ids: Optional[List[str]] = None,
    ) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_level_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_level_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Nivelar", ps_script)
        target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or (5986 if profile.use_ssl else 5985)}"
        self._ssh_log(f"{target} $ powershell -NoProfile -NonInteractive -Command <level-script>")

        def worker() -> None:
            rc = 1
            try:
                execu = make_executor(profile)
                if not isinstance(execu, PSRPExecutor):
                    raise ExecutorError(trf("log_level_transport_unsupported", src=profile.conn_type, dst=profile.conn_type))
                out = execu._run_ps(ps_script, timeout_seconds=None)
                if (out or "").strip():
                    for line in out.splitlines():
                        txt = (line or "").strip()
                        if txt:
                            self._ssh_log(txt)
                rc = 0
                self._app_log("normal", tr("log_level_exec_ok"))
            except Exception as exc:
                self._app_log("normal", trf("log_level_exec_runtime_error", error=exc))
            finally:
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if rc == 0:
                    conn_ids = [c for c in (affected_conn_ids or []) if c]
                    if conn_ids:
                        self.after(0, lambda ids=conn_ids: self._update_caches_after_mutation(ids))

        threading.Thread(target=worker, daemon=True).start()

    def _run_copy_psrp_command(self, ps_script: str, profile: ConnectionProfile, dst_conn_id: Optional[str] = None) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_copy_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Copiar", ps_script)
        target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or (5986 if profile.use_ssl else 5985)}"
        self._ssh_log(f"{target} $ powershell -NoProfile -NonInteractive -Command <copy-script>")

        def worker() -> None:
            rc = 1
            try:
                execu = make_executor(profile)
                if not isinstance(execu, PSRPExecutor):
                    raise ExecutorError(trf("log_copy_transport_unsupported", src=profile.conn_type, dst=profile.conn_type))
                out = execu._run_ps(ps_script, timeout_seconds=None)
                if (out or "").strip():
                    for line in out.splitlines():
                        txt = (line or "").strip()
                        if txt:
                            self._ssh_log(txt)
                rc = 0
                self._app_log("normal", tr("log_copy_exec_ok"))
            except Exception as exc:
                self._app_log("normal", trf("log_copy_exec_runtime_error", error=exc))
            finally:
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if rc == 0 and dst_conn_id:
                    self.after(0, lambda cid=dst_conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _run_breakdown_psrp_command(self, ps_script: str, profile: ConnectionProfile, conn_id: Optional[str] = None) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_breakdown_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Desglosar", ps_script)
        target = f"{profile.username + '@' if profile.username else ''}{profile.host}:{profile.port or (5986 if profile.use_ssl else 5985)}"
        self._ssh_log(f"{target} $ powershell -NoProfile -NonInteractive -Command <breakdown-script>")

        def worker() -> None:
            rc = 1
            try:
                execu = make_executor(profile)
                if not isinstance(execu, PSRPExecutor):
                    raise ExecutorError(trf("log_breakdown_transport_unsupported", ctype=profile.conn_type))
                out = execu._run_ps(ps_script, timeout_seconds=None)
                if (out or "").strip():
                    for line in out.splitlines():
                        txt = (line or "").strip()
                        if txt:
                            self._ssh_log(txt)
                rc = 0
                self._app_log("normal", tr("log_breakdown_exec_ok"))
            except Exception as exc:
                self._app_log("normal", trf("log_breakdown_exec_runtime_error", error=exc))
            finally:
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if conn_id and rc == 0:
                    self.after(0, lambda cid=conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _run_level_command(
        self,
        cmd: str,
        fallback_cmds: Optional[List[str]] = None,
        affected_conn_ids: Optional[List[str]] = None,
    ) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_level_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_level_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Nivelar", cmd)
        self._ssh_log(f"$ {cmd}")

        def worker() -> None:
            proc: Optional[subprocess.Popen[str]] = None
            commands = [cmd] + list(fallback_cmds or [])
            last_rc = 0
            executed_any = False
            cancelled = False
            try:
                for idx, current_cmd in enumerate(commands):
                    if idx > 0:
                        self._app_log("info", f"Retry send flags fallback {idx}/{len(commands) - 1}")
                        self._log_action_subcommands("Nivelar", current_cmd)
                        self._ssh_log(f"$ {current_cmd}")
                    proc = subprocess.Popen(
                        current_cmd,
                        shell=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        bufsize=1,
                        executable="/bin/bash",
                        start_new_session=True,
                    )
                    executed_any = True
                    self._set_active_dataset_process(proc, tr("datasets_level_btn"))
                    if proc.stdout is not None:
                        self._stream_process_output(proc.stdout)
                    last_rc = proc.wait()
                    if last_rc == 0:
                        break
                    if idx < len(commands) - 1:
                        self._app_log("info", f"Fallo comando (rc={last_rc}), probando fallback")
                        self._clear_active_dataset_process(proc)
                        proc = None
            except Exception as exc:
                self._app_log("normal", trf("log_level_exec_spawn_error", error=exc))
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                return

            try:
                if not executed_any:
                    self._app_log("normal", trf("log_level_exec_fail_code", code=1))
                    return
                with self._dataset_proc_lock:
                    cancelled = self._dataset_cancel_requested
                if cancelled:
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                elif last_rc == 0:
                    self._app_log("normal", tr("log_level_exec_ok"))
                else:
                    self._app_log("normal", trf("log_level_exec_fail_code", code=last_rc))
            except Exception as exc:
                self._app_log("normal", trf("log_level_exec_runtime_error", error=exc))
            finally:
                self._clear_active_dataset_process(proc)
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if executed_any and last_rc == 0 and not cancelled:
                    conn_ids = [c for c in (affected_conn_ids or []) if c]
                    if conn_ids:
                        self.after(0, lambda ids=conn_ids: self._update_caches_after_mutation(ids))

        threading.Thread(target=worker, daemon=True).start()

    def _run_copy_command(
        self,
        cmd: str,
        dst_conn_id: Optional[str] = None,
        fallback_cmds: Optional[List[str]] = None,
    ) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_copy_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Copiar", cmd)
        self._ssh_log(f"$ {cmd}")

        def worker() -> None:
            proc: Optional[subprocess.Popen[str]] = None
            commands = [cmd] + list(fallback_cmds or [])
            last_rc = 0
            executed_any = False
            cancelled = False
            try:
                for idx, current_cmd in enumerate(commands):
                    if idx > 0:
                        self._app_log("info", f"Retry send flags fallback {idx}/{len(commands) - 1}")
                        self._log_action_subcommands("Copiar", current_cmd)
                        self._ssh_log(f"$ {current_cmd}")
                    proc = subprocess.Popen(
                        current_cmd,
                        shell=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        bufsize=1,
                        executable="/bin/bash",
                        start_new_session=True,
                    )
                    executed_any = True
                    self._set_active_dataset_process(proc, tr("copy_snapshot_btn"))
                    if proc.stdout is not None:
                        self._stream_process_output(proc.stdout)
                    last_rc = proc.wait()
                    if last_rc == 0:
                        break
                    if idx < len(commands) - 1:
                        self._app_log("info", f"Fallo comando (rc={last_rc}), probando fallback")
                        self._clear_active_dataset_process(proc)
                        proc = None
            except Exception as exc:
                self._app_log("normal", trf("log_copy_exec_spawn_error", error=exc))
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                return

            try:
                if not executed_any:
                    self._app_log("normal", trf("log_copy_exec_fail_code", code=1))
                    return
                with self._dataset_proc_lock:
                    cancelled = self._dataset_cancel_requested
                if cancelled:
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                elif last_rc == 0:
                    self._app_log("normal", tr("log_copy_exec_ok"))
                else:
                    self._app_log("normal", trf("log_copy_exec_fail_code", code=last_rc))
            except Exception as exc:
                self._app_log("normal", trf("log_copy_exec_runtime_error", error=exc))
            finally:
                self._clear_active_dataset_process(proc)
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if executed_any and last_rc == 0 and not cancelled and dst_conn_id:
                    self.after(0, lambda cid=dst_conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _run_copy_paramiko_command(
        self,
        src_profile: ConnectionProfile,
        dst_profile: ConnectionProfile,
        send_candidates: List[str],
        recv_candidates: List[str],
        dst_conn_id: Optional[str] = None,
    ) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        if paramiko is None:
            self._app_log("normal", tr("log_ssh_paramiko_missing"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_copy_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        cancel_event = threading.Event()
        self._set_active_dataset_coop_action(tr("copy_snapshot_btn"), cancel_event)

        def _remote_exec_cmd(profile: ConnectionProfile, raw_cmd: str, keep_stdin: bool) -> Tuple[str, bool]:
            cmd = raw_cmd
            needs_password = False
            if profile.use_sudo and profile.conn_type == "SSH":
                if profile.password:
                    cmd = f"sudo -S -p '' -k sh -lc {shlex.quote(raw_cmd)}"
                    needs_password = True
                else:
                    cmd = f"sudo -n sh -lc {shlex.quote(raw_cmd)}"
            wrap = SSHExecutor(profile)._wrap_remote_shell  # type: ignore[attr-defined]
            return wrap(cmd), needs_password and keep_stdin

        def _run_once(send_raw: str, recv_raw: str) -> Tuple[int, str]:
            src_exec = make_executor(src_profile)
            dst_exec = make_executor(dst_profile)
            if not isinstance(src_exec, SSHExecutor) or not isinstance(dst_exec, SSHExecutor):
                return 1, "copy paramiko supports only SSH->SSH"

            src_client = src_exec._connect()  # type: ignore[attr-defined]
            dst_client = dst_exec._connect()  # type: ignore[attr-defined]
            send_cmd, send_keep_pwd = _remote_exec_cmd(src_profile, send_raw, keep_stdin=False)
            recv_cmd, recv_keep_pwd = _remote_exec_cmd(dst_profile, recv_raw, keep_stdin=True)
            src_target = f"{src_profile.username + '@' if src_profile.username else ''}{src_profile.host}:{src_profile.port or 22}"
            dst_target = f"{dst_profile.username + '@' if dst_profile.username else ''}{dst_profile.host}:{dst_profile.port or 22}"
            self._ssh_log(f"{src_target} $ {send_cmd}")
            self._ssh_log(f"{dst_target} $ {recv_cmd}")
            src_transport = src_client.get_transport()
            dst_transport = dst_client.get_transport()
            if src_transport is None or not src_transport.is_active() or dst_transport is None or not dst_transport.is_active():
                return 1, "SSH transport inactive"
            src_ch = src_transport.open_session()
            dst_ch = dst_transport.open_session()
            src_ch.exec_command(send_cmd)
            dst_ch.exec_command(recv_cmd)
            if src_profile.use_sudo and src_profile.password:
                try:
                    src_ch.send((src_profile.password or "") + "\n")
                except Exception:
                    pass
            if dst_profile.use_sudo and dst_profile.password:
                try:
                    dst_ch.send((dst_profile.password or "") + "\n")
                except Exception:
                    pass
            if send_keep_pwd:
                try:
                    src_ch.shutdown_write()
                except Exception:
                    pass

            stderr_buf = ""
            src_done = False
            last_progress_at = 0.0
            transferred = 0
            while True:
                if cancel_event.is_set():
                    try:
                        src_ch.close()
                    except Exception:
                        pass
                    try:
                        dst_ch.close()
                    except Exception:
                        pass
                    return 130, "cancelled"
                moved = False
                if src_ch.recv_ready():
                    data = src_ch.recv(131072)
                    moved = True
                    if data:
                        transferred += len(data)
                        dst_ch.sendall(data)
                if src_ch.recv_stderr_ready():
                    err = src_ch.recv_stderr(131072).decode("utf-8", errors="replace")
                    if err:
                        moved = True
                        stderr_buf += err
                        for ln in err.splitlines():
                            if ln.strip():
                                self._ssh_log(ln.strip())
                if dst_ch.recv_stderr_ready():
                    err = dst_ch.recv_stderr(131072).decode("utf-8", errors="replace")
                    if err:
                        moved = True
                        stderr_buf += err
                        for ln in err.splitlines():
                            if ln.strip():
                                self._ssh_log(ln.strip())
                if src_ch.exit_status_ready() and not src_ch.recv_ready() and not src_done:
                    src_done = True
                    try:
                        dst_ch.shutdown_write()
                    except Exception:
                        pass
                now = time.monotonic()
                if transferred > 0 and (now - last_progress_at) > 1.0:
                    self._ssh_log(f"[copy] {format_bytes_compact(str(transferred))} transferred")
                    last_progress_at = now
                if src_done and dst_ch.exit_status_ready() and not dst_ch.recv_ready() and not dst_ch.recv_stderr_ready():
                    break
                if not moved:
                    time.sleep(0.02)

            src_rc = src_ch.recv_exit_status() if src_ch.exit_status_ready() else 1
            dst_rc = dst_ch.recv_exit_status() if dst_ch.exit_status_ready() else 1
            if src_rc != 0 or dst_rc != 0:
                return src_rc or dst_rc or 1, (stderr_buf.strip() or f"send_rc={src_rc} recv_rc={dst_rc}")
            return 0, stderr_buf.strip()

        def worker() -> None:
            rc = 1
            tried = 0
            last_err = ""
            combos: List[Tuple[str, str]] = []
            for i, recv_raw in enumerate(recv_candidates):
                for j, send_raw in enumerate(send_candidates):
                    if i == 0 and j == 0:
                        combos.insert(0, (send_raw, recv_raw))
                    else:
                        combos.append((send_raw, recv_raw))
            combos = list(dict.fromkeys(combos))
            try:
                for idx, (send_raw, recv_raw) in enumerate(combos):
                    if idx > 0:
                        self._app_log("info", f"Retry send/recv fallback {idx}/{len(combos)-1}")
                    tried += 1
                    attempt_rc, attempt_err = _run_once(send_raw, recv_raw)
                    if attempt_rc == 0:
                        rc = 0
                        break
                    last_err = attempt_err
                if cancel_event.is_set():
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                elif rc == 0:
                    self._app_log("normal", tr("log_copy_exec_ok"))
                else:
                    if last_err:
                        self._ssh_log(last_err)
                    self._app_log("normal", trf("log_copy_exec_fail_code", code=255))
            except Exception as exc:
                self._app_log("normal", trf("log_copy_exec_runtime_error", error=exc))
            finally:
                ssh_busy(-1)
                self._clear_active_dataset_coop_action(cancel_event)
                self.after(0, self._finish_level_command)
                if rc == 0 and dst_conn_id:
                    self.after(0, lambda cid=dst_conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _run_breakdown_command(self, cmd: str, conn_id: Optional[str] = None) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_breakdown_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Desglosar", cmd)
        self._ssh_log(f"$ {cmd}")

        def worker() -> None:
            proc: Optional[subprocess.Popen[str]] = None
            rc = 1
            cancelled = False
            try:
                proc = subprocess.Popen(
                    cmd,
                    shell=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    executable="/bin/bash",
                    start_new_session=True,
                )
                self._set_active_dataset_process(proc, tr("datasets_breakdown_btn"))
            except Exception as exc:
                self._app_log("normal", trf("log_breakdown_exec_spawn_error", error=exc))
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                return

            assert proc is not None
            try:
                if proc.stdout is not None:
                    self._stream_process_output(proc.stdout)
                rc = proc.wait()
                with self._dataset_proc_lock:
                    cancelled = self._dataset_cancel_requested
                if cancelled:
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                elif rc == 0:
                    self._app_log("normal", tr("log_breakdown_exec_ok"))
                else:
                    self._app_log("normal", trf("log_breakdown_exec_fail_code", code=rc))
            except Exception as exc:
                self._app_log("normal", trf("log_breakdown_exec_runtime_error", error=exc))
            finally:
                self._clear_active_dataset_process(proc)
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if conn_id and rc == 0 and not cancelled:
                    self.after(0, lambda cid=conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _run_assemble_command(self, cmd: str, conn_id: Optional[str] = None) -> None:
        if self.level_running:
            self._app_log("normal", tr("log_copy_already_running"))
            return
        self.level_running = True
        ssh_busy(1)
        self._update_level_button_state()
        self._app_log("normal", tr("log_assemble_exec_start"))
        self._app_log("info", tr("log_dataset_progress_tab"))
        self._log_action_subcommands("Ensamblar", cmd)
        self._ssh_log(f"$ {cmd}")

        def worker() -> None:
            proc: Optional[subprocess.Popen[str]] = None
            rc = 1
            cancelled = False
            try:
                proc = subprocess.Popen(
                    cmd,
                    shell=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    executable="/bin/bash",
                    start_new_session=True,
                )
                self._set_active_dataset_process(proc, tr("datasets_assemble_btn"))
            except Exception as exc:
                self._app_log("normal", trf("log_assemble_exec_spawn_error", error=exc))
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                return

            assert proc is not None
            try:
                if proc.stdout is not None:
                    self._stream_process_output(proc.stdout)
                rc = proc.wait()
                with self._dataset_proc_lock:
                    cancelled = self._dataset_cancel_requested
                if cancelled:
                    self._app_log("normal", tr("log_dataset_cancel_done"))
                elif rc == 0:
                    self._app_log("normal", tr("log_assemble_exec_ok"))
                else:
                    self._app_log("normal", trf("log_assemble_exec_fail_code", code=rc))
            except Exception as exc:
                self._app_log("normal", trf("log_assemble_exec_runtime_error", error=exc))
            finally:
                self._clear_active_dataset_process(proc)
                ssh_busy(-1)
                self.after(0, self._finish_level_command)
                if conn_id and rc == 0 and not cancelled:
                    self.after(0, lambda cid=conn_id: self._update_caches_after_mutation([cid]))

        threading.Thread(target=worker, daemon=True).start()

    def _stream_process_output(self, stream: Any) -> None:
        # Captura progreso tipo pv/dd que usa '\r' (sin '\n') y salida normal por lineas.
        buf = ""
        while True:
            ch = stream.read(1)
            if ch == "":
                break
            if ch in ("\n", "\r"):
                txt = buf.strip()
                if txt:
                    self._ssh_log(txt)
                buf = ""
            else:
                buf += ch
        tail = buf.strip()
        if tail:
            self._ssh_log(tail)

    def _set_active_dataset_process(self, proc: subprocess.Popen[str], action: str) -> None:
        with self._dataset_proc_lock:
            self._active_dataset_proc = proc
            self._active_dataset_action = action
            self._dataset_cancel_requested = False
            self._active_dataset_cancel_event = None
        self.after(0, lambda: self._set_cancel_button_visibility(True, enabled=True))

    def _set_active_dataset_coop_action(self, action: str, cancel_event: threading.Event) -> None:
        with self._dataset_proc_lock:
            self._active_dataset_proc = None
            self._active_dataset_action = action
            self._dataset_cancel_requested = False
            self._active_dataset_cancel_event = cancel_event
        self.after(0, lambda: self._set_cancel_button_visibility(True, enabled=True))

    def _clear_active_dataset_process(self, proc: Optional[subprocess.Popen[str]] = None) -> None:
        with self._dataset_proc_lock:
            if proc is None or self._active_dataset_proc is proc:
                self._active_dataset_proc = None
                self._active_dataset_action = ""
                self._dataset_cancel_requested = False
                self._active_dataset_cancel_event = None
        self.after(0, lambda: self._set_cancel_button_visibility(False, enabled=False))

    def _clear_active_dataset_coop_action(self, cancel_event: Optional[threading.Event] = None) -> None:
        with self._dataset_proc_lock:
            if cancel_event is None or self._active_dataset_cancel_event is cancel_event:
                self._active_dataset_proc = None
                self._active_dataset_action = ""
                self._dataset_cancel_requested = False
                self._active_dataset_cancel_event = None
        self.after(0, lambda: self._set_cancel_button_visibility(False, enabled=False))

    def _set_cancel_button_visibility(self, visible: bool, enabled: bool) -> None:
        try:
            self.cancel_dataset_btn.configure(state=("normal" if enabled else "disabled"))
            if visible:
                self.cancel_dataset_btn.grid()
            else:
                self.cancel_dataset_btn.grid_remove()
        except Exception:
            pass

    def _cancel_dataset_operation(self) -> None:
        with self._dataset_proc_lock:
            proc = self._active_dataset_proc
            action = self._active_dataset_action
            cancel_event = self._active_dataset_cancel_event
            if proc is None or proc.poll() is not None:
                proc = None
            else:
                self._dataset_cancel_requested = True
        if proc is None and cancel_event is None:
            self._app_log("info", tr("log_dataset_cancel_noop"))
            return
        self._app_log("normal", trf("log_dataset_cancel_requested", action=action or "-"))
        if cancel_event is not None:
            self._dataset_cancel_requested = True
            cancel_event.set()
            return
        try:
            if hasattr(os, "killpg"):
                os.killpg(proc.pid, signal.SIGTERM)
            else:
                proc.terminate()
        except Exception:
            pass

        def _kill_later(p: subprocess.Popen[str]) -> None:
            time.sleep(1.5)
            if p.poll() is None:
                try:
                    if hasattr(os, "killpg"):
                        os.killpg(p.pid, signal.SIGKILL)
                    else:
                        p.kill()
                except Exception:
                    pass

        threading.Thread(target=lambda: _kill_later(proc), daemon=True).start()

    def _finish_level_command(self) -> None:
        self.level_running = False
        self._update_level_button_state()

    def _on_origin_pool_selected(self, _event: Any = None) -> None:
        if self._reject_if_ssh_busy():
            return
        self._hide_snapshot_dropdown()
        self.dataset_selected_snapshot_by_side["origin"].clear()
        try:
            self.datasets_tree_origin.selection_remove(self.datasets_tree_origin.selection())
        except Exception:
            pass
        self.origin_dataset_var.set("")
        self._render_dataset_properties("origin", None)
        self._update_snapshot_highlight("origin")
        self._load_side_datasets("origin")
        self._update_level_button_state()

    def _on_dest_pool_selected(self, _event: Any = None) -> None:
        if self._reject_if_ssh_busy():
            return
        self._hide_snapshot_dropdown()
        self.dataset_selected_snapshot_by_side["dest"].clear()
        try:
            self.datasets_tree_dest.selection_remove(self.datasets_tree_dest.selection())
        except Exception:
            pass
        self.dest_dataset_var.set("")
        self._render_dataset_properties("dest", None)
        self._update_snapshot_highlight("dest")
        self._load_side_datasets("dest")
        self._update_level_button_state()

    def _current_selected_pool_name(self, side: str) -> str:
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        if selection not in self.dataset_pool_options:
            return ""
        _cid, pool = self.dataset_pool_options[selection]
        return pool

    def _on_origin_tree_selected(self, _event: Any = None) -> None:
        if self._reject_if_ssh_busy():
            return
        if self._suspend_dataset_tree_select:
            return
        selected = self.datasets_tree_origin.selection()
        if not selected:
            self._set_dataset_selection("origin", "", None)
            return
        iid = str(selected[0]).strip()
        self._set_dataset_selection("origin", iid, None)

    def _on_dest_tree_selected(self, _event: Any = None) -> None:
        if self._reject_if_ssh_busy():
            return
        if self._suspend_dataset_tree_select:
            return
        selected = self.datasets_tree_dest.selection()
        if not selected:
            self._set_dataset_selection("dest", "", None)
            return
        iid = str(selected[0]).strip()
        self._set_dataset_selection("dest", iid, None)

    def _set_dataset_selection(self, side: str, dataset_iid: str, snapshot_name: Optional[str]) -> None:
        self._hide_snapshot_dropdown()
        dataset_iid = (dataset_iid or "").strip()
        none_label = "(seleccione)"
        snap_map = self.dataset_selected_snapshot_by_side.setdefault(side, {})
        tree = self.datasets_tree_origin if side == "origin" else self.datasets_tree_dest
        self._suspend_dataset_tree_select = True
        try:
            if dataset_iid:
                current = tuple(tree.selection())
                if current != (dataset_iid,):
                    tree.selection_set(dataset_iid)
                tree.focus(dataset_iid)
            else:
                tree.selection_remove(tree.selection())
        except Exception:
            pass
        finally:
            self._suspend_dataset_tree_select = False

        # Solo puede haber un snapshot seleccionado por panel.
        prev_selected = list(snap_map.keys())
        snap_map.clear()
        for prev_dataset in prev_selected:
            prev_snaps = self.dataset_snapshots_by_side.get(side, {}).get(prev_dataset, [])
            prev_cell = none_label if prev_snaps else ""
            try:
                tree.item(prev_dataset, values=(prev_cell,))
            except Exception:
                pass

        if dataset_iid:
            if snapshot_name:
                snap_map[dataset_iid] = snapshot_name
            snaps = self.dataset_snapshots_by_side.get(side, {}).get(dataset_iid, [])
            selected_snap = snap_map.get(dataset_iid, "")
            cell_value = f"@{selected_snap}" if selected_snap else (none_label if snaps else "")
            try:
                tree.item(dataset_iid, values=(cell_value,))
            except Exception:
                pass
        selected_name = f"{dataset_iid}@{snapshot_name}" if dataset_iid and snapshot_name else dataset_iid
        self.last_selected_dataset_side = side
        if side == "origin":
            self.origin_dataset_var.set(selected_name)
        else:
            self.dest_dataset_var.set(selected_name)
        row = self._find_selected_dataset_row(side, selected_name) if selected_name else None
        self._render_dataset_properties(side, row)
        self._update_level_button_state()

    def _update_snapshot_highlight(self, side: str) -> None:
        # Resaltado especial de snapshots deshabilitado por preferencia de UI.
        return

    def _hide_snapshot_dropdown(self) -> None:
        if self._snapshot_cell_editor is not None:
            try:
                self._snapshot_cell_editor.destroy()
            except Exception:
                pass
        self._snapshot_cell_editor = None
        self._snapshot_cell_editor_side = ""
        self._snapshot_cell_editor_dataset = ""
        if self._snapshot_cell_popup is not None:
            try:
                self._snapshot_cell_popup.destroy()
            except Exception:
                pass
            self._snapshot_cell_popup = None

    def _open_snapshot_dropdown(self, side: str, tree: ttk.Treeview, dataset_iid: str) -> None:
        dataset_iid = (dataset_iid or "").strip()
        if not dataset_iid:
            return
        bbox = tree.bbox(dataset_iid, "#1")
        if not bbox:
            return
        x, y, w, h = bbox
        snaps = self.dataset_snapshots_by_side.get(side, {}).get(dataset_iid, [])
        if not snaps:
            return
        self._hide_snapshot_dropdown()

        none_label = "(seleccione)"
        options = [none_label] + [f"@{snap}" for snap in snaps]
        current_snap = self.dataset_selected_snapshot_by_side.get(side, {}).get(dataset_iid, "")
        selected_value = f"@{current_snap}" if current_snap else none_label

        popup = tk.Toplevel(self)
        popup.overrideredirect(True)
        popup.transient(self)
        popup.lift()
        try:
            popup.attributes("-topmost", True)
        except Exception:
            pass
        popup.geometry(f"{max(100, w)}x{max(24, h)}+{tree.winfo_rootx() + x}+{tree.winfo_rooty() + y}")
        self._snapshot_cell_popup = popup

        value_var = tk.StringVar(value=selected_value)
        combo = ttk.Combobox(
            popup,
            textvariable=value_var,
            state="readonly",
            values=options,
        )
        combo.pack(fill="both", expand=True)
        self._snapshot_cell_editor = combo
        self._snapshot_cell_editor_side = side
        self._snapshot_cell_editor_dataset = dataset_iid

        def _apply_selection(_event: Any = None) -> None:
            raw = (value_var.get() or "").strip()
            snap = raw[1:] if raw.startswith("@") else ""
            self._set_dataset_selection(side, dataset_iid, snap or None)

        def _on_escape(_event: Any = None) -> str:
            self._hide_snapshot_dropdown()
            return "break"

        combo.bind("<<ComboboxSelected>>", _apply_selection)
        combo.bind("<Return>", _apply_selection)
        combo.bind("<Escape>", _on_escape)

        try:
            combo.focus_set()
            combo.after(20, lambda: combo.event_generate("<Down>"))
        except Exception:
            pass

    def _on_dataset_tree_click(self, side: str, event: Any) -> None:
        if self._reject_if_ssh_busy():
            return
        tree = self.datasets_tree_origin if side == "origin" else self.datasets_tree_dest
        row_iid = str(tree.identify_row(event.y) or "").strip()
        if not row_iid:
            self._hide_snapshot_dropdown()
            return
        col_token = str(tree.identify_column(event.x) or "")
        if col_token in {"#0", ""}:
            self._set_dataset_selection(side, row_iid, None)
            return
        if col_token == "#1":
            # Primero asegura la seleccion de fila; abre el dropdown despues
            # para evitar que <<TreeviewSelect>> lo cierre inmediatamente.
            try:
                tree.selection_set(row_iid)
                tree.focus(row_iid)
            except Exception:
                pass
            self.after_idle(lambda s=side, t=tree, iid=row_iid: self._open_snapshot_dropdown(s, t, iid))
            return
        self._set_dataset_selection(side, row_iid, None)

    def _on_dataset_tree_context(self, side: str, event: Any) -> str:
        if self._reject_if_ssh_busy():
            return "break"
        tree = self.datasets_tree_origin if side == "origin" else self.datasets_tree_dest
        iid = str(tree.identify_row(event.y) or "").strip()
        if not iid:
            return "break"
        tree.selection_set(iid)
        tree.focus(iid)
        if side == "origin":
            self._on_origin_tree_selected()
        else:
            self._on_dest_tree_selected()
        self._dataset_context_side = side
        self.datasets_context_menu.entryconfigure(0, state=self.create_btn.cget("state"))
        self.datasets_context_menu.entryconfigure(1, state=self.modify_btn.cget("state"))
        self.datasets_context_menu.entryconfigure(2, state=self.delete_dataset_btn.cget("state"))
        can_mount, can_umount = self._dataset_mount_context_states(side)
        self.datasets_context_menu.entryconfigure(4, state=("normal" if can_mount else "disabled"))
        self.datasets_context_menu.entryconfigure(5, state=("normal" if can_umount else "disabled"))
        self._show_context_menu(self.datasets_context_menu, event)
        return "break"

    def _dataset_mount_context_states(self, side: str) -> Tuple[bool, bool]:
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        dataset = self.origin_dataset_var.get().strip() if side == "origin" else self.dest_dataset_var.get().strip()
        if not selection or not dataset or selection not in self.dataset_pool_options:
            return False, False
        if "@" in dataset:
            return False, False
        row = self._find_selected_dataset_row(side, dataset)
        if not row:
            return False, False
        mounted_val = (row.get("mounted", "") or "").strip().lower()
        mountpoint_val = (row.get("mountpoint", "") or "").strip().lower()
        canmount_val = (row.get("canmount", "") or "").strip().lower()
        can_mount = mounted_val in {"no", "off", "false"} and bool(mountpoint_val) and mountpoint_val != "none" and canmount_val != "off"
        can_umount = mounted_val in {"yes", "on", "true"}
        return can_mount, can_umount

    def _load_datasets_for_active_connection(self) -> None:
        self._refresh_dataset_pool_options_global()
        if not self.dataset_pool_options:
            self._app_log("info", tr("log_no_pools_for_datasets"))
            self._render_datasets_tree("origin", self.datasets_tree_origin, [], None)
            self._render_datasets_tree("dest", self.datasets_tree_dest, [], None)
            self._render_dataset_properties("origin", None)
            self._render_dataset_properties("dest", None)
            self._update_level_button_state()
            return
        self._app_log("info", tr("log_loading_datasets_both"))
        self._load_side_datasets("origin")
        self._load_side_datasets("dest")

    def _refresh_dataset_pool_options_global(self) -> None:
        options: Dict[str, Tuple[str, str]] = {}
        labels: List[str] = []
        for conn in self.store.connections:
            state = self.states.get(conn.id)
            if not state or not state.imported:
                continue
            for row in state.imported:
                pool = row.get("pool", "").strip()
                if not pool:
                    continue
                label = f"{conn.name} :: {pool}"
                options[label] = (conn.id, pool)
                labels.append(label)
        labels.sort()
        self.dataset_pool_options = options
        self.origin_pool_combo["values"] = labels
        self.dest_pool_combo["values"] = labels
        if not labels:
            self.origin_pool_var.set("")
            self.dest_pool_var.set("")
            self.origin_dataset_var.set("")
            self.dest_dataset_var.set("")
            self._update_level_button_state()
            return
        if self.origin_pool_var.get() not in options:
            self.origin_pool_var.set(labels[0])
        if self.dest_pool_var.get() not in options:
            self.dest_pool_var.set(labels[0])

    def _load_side_datasets(self, side: str) -> None:
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        if not selection:
            return
        if selection not in self.dataset_pool_options:
            return
        conn_id, pool = self.dataset_pool_options[selection]
        profile = self.store.get(conn_id)
        if not profile:
            return
        cache_key = f"{conn_id}:{pool}"
        if cache_key in self.datasets_cache:
            datasets = self.datasets_cache[cache_key]
            self._app_log("debug", trf("log_datasets_from_cache", side=side, selection=selection, count=len(datasets)))
            if side == "origin":
                self._render_datasets_tree("origin", self.datasets_tree_origin, datasets, pool)
                self._normalize_selected_dataset_var("origin", datasets)
            else:
                self._render_datasets_tree("dest", self.datasets_tree_dest, datasets, pool)
                self._normalize_selected_dataset_var("dest", datasets)
            return
        self.status_var.set(trf("status_loading_datasets", pool=pool, name=profile.name))
        self._app_log("info", trf("log_loading_datasets_side", side=side, name=profile.name, pool=pool))

        def worker() -> None:
            try:
                execu = make_executor(profile)
                datasets = run_with_timeout(
                    lambda: execu.list_datasets(pool),
                    REFRESH_TIMEOUT_SECONDS,
                    trf("error_timeout_loading_datasets", pool=pool),
                )
                self.datasets_cache[cache_key] = datasets
                self._app_log("debug", trf("log_datasets_loaded_side", side=side, name=profile.name, pool=pool, count=len(datasets)))
                if side == "origin":
                    self.after(0, lambda p=pool: self._render_datasets_tree("origin", self.datasets_tree_origin, datasets, p))
                    self.after(0, lambda: self._normalize_selected_dataset_var("origin", datasets))
                else:
                    self.after(0, lambda p=pool: self._render_datasets_tree("dest", self.datasets_tree_dest, datasets, p))
                    self.after(0, lambda: self._normalize_selected_dataset_var("dest", datasets))
                self.after(0, lambda: self.status_var.set(trf("status_datasets_loaded", name=profile.name, pool=pool)))
            except Exception as exc:
                self._app_log("normal", trf("log_datasets_load_error", side=side, error=exc))
                self.after(0, lambda e=exc: messagebox.showerror(tr("datasets_title"), str(e)))
                self.after(0, lambda: self.status_var.set(trf("status_datasets_error", name=profile.name)))

        threading.Thread(target=worker, daemon=True).start()

    def _normalize_selected_dataset_var(self, side: str, datasets: List[Dict[str, str]]) -> None:
        names_all = {row.get("name", "").strip() for row in datasets if row.get("name", "").strip()}
        dataset_names = {n.split("@", 1)[0] for n in names_all}
        current = self.origin_dataset_var.get().strip() if side == "origin" else self.dest_dataset_var.get().strip()

        target_dataset = ""
        target_snap: Optional[str] = None

        if current:
            if "@" in current:
                ds, snap = current.split("@", 1)
                ds = ds.strip()
                snap = snap.strip()
                if ds in dataset_names:
                    if current in names_all and snap:
                        target_dataset = ds
                        target_snap = snap
                    else:
                        target_dataset = ds
                else:
                    target_dataset = ""
            else:
                if current in dataset_names:
                    target_dataset = current
                else:
                    target_dataset = ""

        # Mantiene sincronizado StringVar <-> Treeview tras recargas.
        self._set_dataset_selection(side, target_dataset, target_snap)
        if not target_dataset:
            self._render_dataset_properties(side, None)
        self._update_level_button_state()

    def _find_selected_dataset_row(self, side: str, dataset_name: str) -> Optional[Dict[str, str]]:
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        if selection not in self.dataset_pool_options:
            return None
        conn_id, pool = self.dataset_pool_options[selection]
        rows = self.datasets_cache.get(f"{conn_id}:{pool}", [])
        for row in rows:
            if row.get("name", "").strip() == dataset_name:
                return row
        return None

    def _render_dataset_properties(self, side: str, row: Optional[Dict[str, str]]) -> None:
        rows_frame = self.dataset_props_rows
        columns = self.dataset_props_columns
        if not row:
            self._last_dataset_props_sig = ""
            self._dataset_props_ctx = None
            self._dataset_props_load_token += 1
            self._dataset_props_edit_vars = {}
            self._dataset_props_inherit_vars = {}
            self._dataset_props_original_values = {}
            self._clear_plain_table(rows_frame)
            self.dataset_props_selected_var.set(trf("datasets_selected_target", dataset=tr("label_none")))
            try:
                self.dataset_props_apply_btn.configure(state="disabled")
            except Exception:
                pass
            return
        sig = json.dumps(row, sort_keys=True, ensure_ascii=False)
        if sig == self._last_dataset_props_sig:
            return
        self._last_dataset_props_sig = sig
        dataset_name = str(row.get("name", "") or "").strip()
        self.dataset_props_selected_var.set(trf("datasets_selected_target", dataset=dataset_name or tr("label_none")))
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        if not dataset_name or selection not in self.dataset_pool_options:
            self._clear_plain_table(rows_frame)
            self._add_plain_row(rows_frame, 0, columns, [tr("datasets_dataset"), dataset_name or tr("label_none")])
            try:
                self.dataset_props_apply_btn.configure(state="disabled")
            except Exception:
                pass
            return
        conn_id, pool = self.dataset_pool_options[selection]
        ctx = (side, conn_id, pool, dataset_name)
        self._dataset_props_ctx = ctx
        self._dataset_props_edit_vars = {}
        self._dataset_props_inherit_vars = {}
        self._dataset_props_original_values = {}
        try:
            self.dataset_props_apply_btn.configure(state="disabled")
        except Exception:
            pass

        self._clear_plain_table(rows_frame)
        self._add_plain_row(rows_frame, 0, columns, [tr("datasets_dataset"), dataset_name])
        self._add_plain_row(rows_frame, 1, columns, [tr("status"), "..."])
        token = self._dataset_props_load_token = self._dataset_props_load_token + 1
        profile = self.store.get(conn_id)
        if not profile:
            return

        def _render_loaded(props: List[Dict[str, str]]) -> None:
            if token != self._dataset_props_load_token or self._dataset_props_ctx != ctx:
                return
            self._clear_plain_table(rows_frame)
            ordered_props: List[Dict[str, str]] = []
            by_name = {str((p.get("property") or "")).strip(): p for p in props}
            first_keys = ["mountpoint", "mounted", "used", "compressratio", "encryption"]
            for fk in first_keys:
                if fk in by_name:
                    ordered_props.append(by_name[fk])
            consumed = set(first_keys)
            rest = [p for p in props if str((p.get("property") or "")).strip() not in consumed]
            rest.sort(key=lambda p: str((p.get("property") or "")).strip().lower())
            ordered_props.extend(rest)

            dataset_type = ""
            for prop in props:
                if (prop.get("property", "") or "").strip().lower() == "type":
                    dataset_type = (prop.get("value", "") or "").strip().lower()
                    break

            row_idx = 0
            self._add_plain_row(rows_frame, row_idx, columns, [tr("datasets_dataset"), dataset_name])
            row_idx += 1
            editable_count = 0
            for prop in ordered_props:
                name = str((prop.get("property") or "")).strip()
                value = str((prop.get("value") or ""))
                source = str((prop.get("source") or ""))
                readonly = str((prop.get("readonly") or ""))
                editable = is_dataset_property_editable(name, dataset_type, source, readonly)
                if not editable:
                    continue
                editable_count += 1
                bg = UI_PANEL_BG if row_idx % 2 == 0 else "#f8fbfd"
                line = tk.Frame(rows_frame, bg=bg, highlightthickness=0)
                grid_row = row_idx * 2
                line.grid(row=grid_row, column=0, sticky="ew")
                scroll_canvas = getattr(rows_frame, "_scroll_canvas", None)
                if isinstance(scroll_canvas, tk.Canvas):
                    line.bind("<MouseWheel>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    line.bind("<Button-4>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    line.bind("<Button-5>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                tk.Label(line, text=name, bg=bg, fg=UI_TEXT, anchor="w", padx=6, pady=3).grid(row=0, column=0, sticky="nsew")
                var = tk.StringVar(value=value)
                self._dataset_props_edit_vars[name] = var
                inherit_var = tk.BooleanVar(value=False)
                self._dataset_props_inherit_vars[name] = inherit_var
                self._dataset_props_original_values[name] = value
                var.trace_add("write", lambda *_a: self._update_dataset_props_apply_btn_state())
                inherit_var.trace_add("write", lambda *_a: self._update_dataset_props_apply_btn_state())
                value_wrap = tk.Frame(line, bg=bg, highlightthickness=0)
                value_wrap.grid(row=0, column=1, sticky="nsew", padx=(2, 2), pady=2)
                value_wrap.grid_columnconfigure(0, weight=1)
                ent = ttk.Entry(value_wrap, textvariable=var)
                ent.grid(row=0, column=0, sticky="nsew", padx=(0, 4))
                chk = ttk.Checkbutton(value_wrap, text="inherit", variable=inherit_var)
                chk.grid(row=0, column=1, sticky="e")
                if isinstance(scroll_canvas, tk.Canvas):
                    ent.bind("<MouseWheel>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    ent.bind("<Button-4>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    ent.bind("<Button-5>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    chk.bind("<MouseWheel>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    chk.bind("<Button-4>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                    chk.bind("<Button-5>", lambda e, c=scroll_canvas: self._on_table_mousewheel(e, c))
                line.grid_columnconfigure(0, minsize=columns[0][1], weight=0)
                line.grid_columnconfigure(1, minsize=columns[1][1], weight=0)
                sep = tk.Frame(rows_frame, bg=UI_BORDER, height=1)
                sep.grid(row=grid_row + 1, column=0, sticky="ew")
                row_idx += 1
            if editable_count == 0:
                self._add_plain_row(rows_frame, row_idx, columns, [tr("status"), "Sin propiedades editables"])
            try:
                self._update_dataset_props_apply_btn_state()
            except Exception:
                pass

        def worker() -> None:
            try:
                execu = make_executor(profile)
                props = execu.list_dataset_properties(dataset_name)
                # Actualiza cache por conexion para reusarla en siguientes selecciones.
                conn_cache = self.dataset_properties_cache.setdefault(conn_id, {})
                conn_cache[dataset_name] = props
                self.after(0, lambda p=props: _render_loaded(p))
            except Exception as exc:
                self._app_log("normal", trf("log_modify_dataset_load_error", name=profile.name, dataset=dataset_name, error=exc))
                self.after(0, lambda: self._clear_plain_table(rows_frame))
                self.after(0, lambda: self._add_plain_row(rows_frame, 0, columns, [tr("datasets_dataset"), dataset_name]))
                self.after(0, lambda e=exc: self._add_plain_row(rows_frame, 1, columns, [tr("label_error"), str(e)]))

        # Preferir propiedades precargadas durante refresh para evitar consultas una a una.
        preloaded = self.dataset_properties_cache.get(conn_id, {}).get(dataset_name)
        if preloaded:
            _render_loaded(preloaded)
        else:
            threading.Thread(target=worker, daemon=True).start()

    def _dataset_props_has_changes(self) -> bool:
        for prop, var in self._dataset_props_edit_vars.items():
            inh = self._dataset_props_inherit_vars.get(prop)
            if inh is not None and bool(inh.get()):
                return True
            old = self._dataset_props_original_values.get(prop, "")
            if var.get() != old:
                return True
        return False

    def _update_dataset_props_apply_btn_state(self) -> None:
        try:
            enabled = bool(self._dataset_props_ctx and self._dataset_props_edit_vars and self._dataset_props_has_changes())
            self.dataset_props_apply_btn.configure(state=("normal" if enabled else "disabled"))
        except Exception:
            pass

    def _apply_right_panel_dataset_properties(self) -> None:
        if self._reject_if_ssh_busy():
            return
        ctx = self._dataset_props_ctx
        if not ctx:
            return
        side, conn_id, _pool, dataset_name = ctx
        origin_pool_sel = self.origin_pool_var.get().strip()
        origin_ds_sel = self.origin_dataset_var.get().strip()
        dest_pool_sel = self.dest_pool_var.get().strip()
        dest_ds_sel = self.dest_dataset_var.get().strip()
        profile = self.store.get(conn_id)
        if not profile:
            return
        changes: Dict[str, str] = {}
        inherit_props: List[str] = []
        for prop, var in self._dataset_props_edit_vars.items():
            inh = self._dataset_props_inherit_vars.get(prop)
            if inh is not None and bool(inh.get()):
                inherit_props.append(prop)
                continue
            old = self._dataset_props_original_values.get(prop, "")
            new = var.get()
            if new != old:
                changes[prop] = new
        if not changes and not inherit_props:
            self._app_log("info", tr("log_modify_dataset_no_changes"))
            self._update_dataset_props_apply_btn_state()
            return
        self._app_log(
            "normal",
            trf("log_modify_dataset_apply_start", name=profile.name, dataset=dataset_name, count=len(changes)),
        )

        def worker() -> None:
            try:
                execu = make_executor(profile)
                out_parts: List[str] = []
                if changes:
                    out_set = execu.set_dataset_properties(dataset_name, changes)
                    if (out_set or "").strip():
                        out_parts.append(out_set.strip())
                if inherit_props:
                    out_inherit = execu.inherit_dataset_properties(dataset_name, inherit_props)
                    if (out_inherit or "").strip():
                        out_parts.append(out_inherit.strip())
                self._app_log("info", trf("log_modify_dataset_apply_done", name=profile.name, dataset=dataset_name))
                out = "\n".join(out_parts)
                if out.strip():
                    self._app_log("debug", out.strip())
            except Exception as exc:
                self._app_log(
                    "normal",
                    trf("log_modify_dataset_apply_error", name=profile.name, dataset=dataset_name, error=exc),
                )
                self.after(0, lambda e=exc: messagebox.showerror(tr("modify_dataset_title"), str(e)))
            finally:
                def _post_apply_refresh() -> None:
                    # Preserva seleccion actual en ambos lados y refresca solo datasets/propiedades.
                    if origin_pool_sel:
                        self.origin_pool_var.set(origin_pool_sel)
                    if origin_ds_sel:
                        self.origin_dataset_var.set(origin_ds_sel)
                    if dest_pool_sel:
                        self.dest_pool_var.set(dest_pool_sel)
                    if dest_ds_sel:
                        self.dest_dataset_var.set(dest_ds_sel)

                    # Invalida cache solo de los pools de este conn que esten visibles en origen/destino.
                    for sel in (origin_pool_sel, dest_pool_sel):
                        if sel and sel in self.dataset_pool_options:
                            cid, pool_name = self.dataset_pool_options[sel]
                            if cid == conn_id:
                                self.datasets_cache.pop(f"{cid}:{pool_name}", None)

                    self._load_side_datasets("origin")
                    self._load_side_datasets("dest")
                    # Fuerza recarga de propiedades del dataset aplicado sin perder seleccion.
                    self._render_dataset_properties(side, {"name": dataset_name})

                self.after(0, _post_apply_refresh)

        threading.Thread(target=worker, daemon=True).start()

    def _run_dataset_mount_action(self, side: str, do_mount: bool) -> None:
        selection = self.origin_pool_var.get().strip() if side == "origin" else self.dest_pool_var.get().strip()
        dataset = self.origin_dataset_var.get().strip() if side == "origin" else self.dest_dataset_var.get().strip()
        if not selection or not dataset or selection not in self.dataset_pool_options:
            return
        conn_id, pool = self.dataset_pool_options[selection]
        row = self._find_selected_dataset_row(side, dataset)
        if not row:
            return
        recursive_unmount = False
        if do_mount:
            mountpoint_raw = (row.get("mountpoint", "") or "").strip()
            mountpoint_val = mountpoint_raw.lower()
            canmount_val = (row.get("canmount", "") or "").strip().lower()
            if not mountpoint_val or mountpoint_val == "none" or canmount_val == "off":
                self._app_log("normal", trf("log_dataset_mount_not_allowed", dataset=dataset))
                return
        else:
            rows = self.datasets_cache.get(f"{conn_id}:{pool}", [])
            pref = dataset + "/"
            mounted_children = [
                r.get("name", "").strip()
                for r in rows
                if (r.get("name", "").strip().startswith(pref))
                and ((r.get("mounted", "") or "").strip().lower() in {"yes", "on", "true"})
            ]
            if mounted_children:
                confirm_recursive = messagebox.askyesno(
                    tr("action_umount"),
                    f"Hay {len(mounted_children)} subdatasets montados bajo {dataset}. ¿Desmontar recursivamente (hojas a raiz)?",
                    parent=self,
                )
                if not confirm_recursive:
                    self._app_log("info", f"Desmontar cancelado por usuario: {dataset}")
                    return
                recursive_unmount = True
        profile = self.store.get(conn_id)
        if not profile:
            return
        action_name = tr("action_mount") if do_mount else tr("action_umount")
        self._app_log("info", trf("log_dataset_mount_start", action=action_name, dataset=dataset, name=profile.name))
        self._ssh_log(f"[ACTION] {action_name} {dataset} @ {profile.name}")

        def worker() -> None:
            try:
                execu = make_executor(profile)
                if do_mount:
                    # Requisito de seguridad operativa:
                    # no montar un dataset si su padre no esta montado.
                    if "/" in dataset and "@" not in dataset:
                        parent_ds = dataset.rsplit("/", 1)[0].strip()
                        if parent_ds:
                            parent_row = next(
                                (
                                    r
                                    for r in self.datasets_cache.get(f"{conn_id}:{pool}", [])
                                    if (r.get("name", "").strip() == parent_ds)
                                ),
                                None,
                            )
                            parent_mounted = (
                                (parent_row.get("mounted", "") if parent_row else "").strip().lower()
                                in {"yes", "on", "true"}
                            )
                            parent_mountpoint = (parent_row.get("mountpoint", "") if parent_row else "").strip().lower()
                            # Excepcion: si el padre tiene mountpoint=none, se permite montar
                            # el hijo siempre que no haya conflicto de mountpoint (validado despues).
                            if not parent_mounted and parent_mountpoint != "none":
                                msg = f"El dataset padre {parent_ds} no está montado, móntelo antes por favor"
                                self._app_log("warning", msg)
                                self.after(0, lambda m=msg: messagebox.showwarning(tr("action_mount"), m))
                                return
                    mounted_rows = execu.list_mounted_datasets()
                    target_mp = (row.get("mountpoint", "") or "").strip()
                    conflicts = sorted(
                        {
                            (r.get("dataset", "") or "").strip()
                            for r in mounted_rows
                            if (r.get("mountpoint", "") or "").strip() == target_mp
                            and (r.get("dataset", "") or "").strip()
                            and (r.get("dataset", "") or "").strip() != dataset
                        }
                    )
                    if conflicts:
                        conflict_text = ", ".join(conflicts)
                        msg = (
                            f"No se permite montar {dataset} en {target_mp}: "
                            f"ya esta montado {conflict_text}. "
                            "Por favor cambie el mountpoint."
                        )
                        self._app_log("warning", msg)
                        self.after(0, lambda m=msg: messagebox.showwarning(tr("action_mount"), m))
                        return
                    out = execu.mount_dataset(dataset)
                else:
                    out = execu.unmount_dataset(dataset, recursive=recursive_unmount)
                self._app_log(
                    "info",
                    trf("log_dataset_mount_done", action=action_name, dataset=dataset, name=profile.name),
                )
                if (out or "").strip():
                    self._app_log("debug", out.strip())
            except Exception as exc:
                self._app_log(
                    "normal",
                    trf("log_dataset_mount_error", action=action_name, dataset=dataset, name=profile.name, error=exc),
                )
                self.after(0, lambda e=exc: messagebox.showerror(tr("datasets_title"), str(e)))
            finally:
                cache_key = f"{conn_id}:{pool}"
                self.datasets_cache.pop(cache_key, None)
                self.after(0, lambda: self._load_side_datasets(side))

        threading.Thread(target=worker, daemon=True).start()

    def _update_level_button_state(self) -> None:
        src = self.origin_dataset_var.get().strip()
        dst = self.dest_dataset_var.get().strip()
        self._update_transfer_selection_labels(src, dst)
        origin_sel = self.datasets_tree_origin.selection()
        dest_sel = self.datasets_tree_dest.selection()
        src_dataset_only = src.split("@", 1)[0] if "@" in src else src
        origin_pool_name = self._current_selected_pool_name("origin")
        dest_pool_name = self._current_selected_pool_name("dest")
        origin_selected_ok = bool(
            "@" not in src
            and (
                (origin_sel and str(origin_sel[0]).strip() == src_dataset_only)
                or ((not origin_sel) and src_dataset_only == origin_pool_name)
            )
        )
        dest_selected_ok = bool(
            "@" not in dst
            and (
                (dest_sel and str(dest_sel[0]).strip() == dst)
                or ((not dest_sel) and dst == dest_pool_name)
            )
        )
        enabled = bool((not self.level_running) and (self.ssh_busy_count == 0) and origin_selected_ok and dest_selected_ok)
        origin_snapshot_ok = bool(
            "@" in src
            and (
                (origin_sel and str(origin_sel[0]).strip() == src_dataset_only)
                or ((not origin_sel) and src_dataset_only == origin_pool_name)
            )
        )
        copy_enabled = bool((not self.level_running) and (self.ssh_busy_count == 0) and origin_snapshot_ok and dest_selected_ok)
        has_dataset_target = self._get_dataset_for_create() is not None
        has_delete_target = self._get_target_for_delete() is not None
        create_enabled = bool((self.ssh_busy_count == 0) and has_dataset_target)
        modify_enabled = bool((self.ssh_busy_count == 0) and has_dataset_target)
        delete_enabled = bool((self.ssh_busy_count == 0) and has_delete_target)
        breakdown_enabled = bool((self.ssh_busy_count == 0) and has_dataset_target)
        assemble_enabled = bool((self.ssh_busy_count == 0) and has_dataset_target)
        self._app_log(
            "debug",
            trf(
                "log_level_button_state",
                src=src or "-",
                dst=dst or "-",
                src_ok=origin_selected_ok,
                dst_ok=dest_selected_ok,
                enabled=enabled,
            ),
        )
        self.level_btn.configure(state="normal" if enabled else "disabled")
        self.copy_btn.configure(state="normal" if copy_enabled else "disabled")
        self.sync_btn.configure(state="normal" if enabled else "disabled")
        self.create_btn.configure(state="normal" if create_enabled else "disabled")
        self.modify_btn.configure(state="normal" if modify_enabled else "disabled")
        self.delete_dataset_btn.configure(state="normal" if delete_enabled else "disabled")
        self.breakdown_btn.configure(state="normal" if breakdown_enabled else "disabled")
        self.assemble_btn.configure(state="normal" if assemble_enabled else "disabled")
        delete_target = self._get_target_for_delete()
        if delete_target:
            _side, _sel, target, target_conn_id = delete_target
            profile = self.store.get(target_conn_id)
            conn_label = (profile.name if profile else target_conn_id).strip() or target_conn_id
            self.dataset_action_target_var.set(trf("datasets_selected_target", dataset=f"{conn_label}::{target}"))
        else:
            self.dataset_action_target_var.set(trf("datasets_selected_target", dataset=tr("label_none")))

    def _update_transfer_selection_labels(self, src_dataset: str, dst_dataset: str) -> None:
        def _fmt(value: str) -> str:
            clean = (value or "").strip()
            if not clean:
                return f"Dataset: {tr('label_none')}"
            kind = "Snapshot" if "@" in clean else "Dataset"
            return f"{kind}: {clean}"

        self.transfer_origin_target_var.set(_fmt(src_dataset))
        self.transfer_dest_target_var.set(_fmt(dst_dataset))

    def _render_datasets_tree(
        self,
        side: str,
        tree: ttk.Treeview,
        datasets: List[Dict[str, str]],
        root_dataset: Optional[str],
    ) -> None:
        for iid in tree.get_children():
            tree.delete(iid)

        self.dataset_snapshots_by_side[side] = {}
        inserted: set[str] = set()
        dataset_names: set[str] = set()
        snapshots: Dict[str, List[Tuple[str, str]]] = {}
        root = (root_dataset or "").strip()

        for item in sorted(datasets, key=lambda d: d.get("name", "")):
            full = (item.get("name", "") or "").strip()
            if not full:
                continue
            if "@" in full:
                ds, snap = full.split("@", 1)
                creation = str(item.get("creation", "") or "").strip()
                snapshots.setdefault(ds, []).append((snap, creation))
            else:
                dataset_names.add(full)

        if root:
            dataset_names.add(root)

        def _ensure_dataset_path(dataset_full: str) -> str:
            full = (dataset_full or "").strip()
            if not full:
                return ""
            if root and full == root:
                parts = [root]
            elif root and full.startswith(f"{root}/"):
                parts = [root] + full[len(root) + 1 :].split("/")
            else:
                parts = full.split("/")
            iid_accum = ""
            parent_iid = ""
            for part in parts:
                iid_accum = part if not iid_accum else f"{iid_accum}/{part}"
                if iid_accum not in inserted:
                    tree.insert(
                        parent_iid,
                        "end",
                        iid=iid_accum,
                        text=part,
                        values=("",),
                        open=bool(root and iid_accum == root),
                    )
                    inserted.add(iid_accum)
                parent_iid = iid_accum
            return parent_iid

        for dataset_full in sorted(dataset_names):
            _ensure_dataset_path(dataset_full)

        def _snap_sort_key(entry: Tuple[str, str]) -> Tuple[int, str]:
            snap_name, creation = entry
            try:
                ts = int(creation)
            except Exception:
                ts = -1
            return (ts, snap_name)

        for dataset_full in sorted(dataset_names):
            snaps = snapshots.get(dataset_full, [])
            snaps_sorted = [snap for snap, _creation in sorted(snaps, key=_snap_sort_key, reverse=True)]
            self.dataset_snapshots_by_side[side][dataset_full] = snaps_sorted
            selected_snap = self.dataset_selected_snapshot_by_side.get(side, {}).get(dataset_full, "")
            if selected_snap and selected_snap not in snaps_sorted:
                self.dataset_selected_snapshot_by_side.get(side, {}).pop(dataset_full, None)
                selected_snap = ""
            none_label = "(seleccione)"
            values: List[str] = [f"@{selected_snap}" if selected_snap else (none_label if snaps_sorted else "")]
            if dataset_full in inserted:
                try:
                    tree.item(dataset_full, values=tuple(values))
                except Exception:
                    pass
        self._update_snapshot_highlight(side)

    def _render_connection_state(self, profile: ConnectionProfile, refresh_tables: bool = True) -> None:
        state = self.states.get(profile.id, ConnectionState(message=tr("status_no_data")))

        self.status_var.set(trf("status_connection_fmt", name=profile.name, msg=state.message))
        if profile.conn_type in {"SSH", "LOCAL"}:
            priv_text = tr("priv_ok") if state.sudo_ok else tr("priv_no")
            if state.sudo_ok is None:
                priv_text = tr("priv_not_checked")
            self.priv_var.set(trf("priv_sudo_fmt", name=profile.name, value=priv_text))
        else:
            admin_text = tr("priv_ok") if state.sudo_ok else tr("priv_no")
            if state.sudo_ok is None:
                admin_text = tr("priv_not_checked")
            self.priv_var.set(trf("priv_admin_fmt", name=profile.name, value=admin_text))

        if refresh_tables:
            self._render_all_imported_pools()
            self._render_all_importable_pools()
        current = self.left_tabs.select()
        if current and str(current) == str(self.tab_datasets):
            self._load_datasets_for_active_connection()

    def _render_all_imported_pools(self) -> None:
        self._clear_plain_table(self.imported_table_rows)
        row_idx = 0
        visible_keys: set[str] = set()
        for conn in self.store.connections:
            state = self.states.get(conn.id)
            if not state or not state.imported:
                continue
            for pool in state.imported:
                pool_name = pool.get("pool", "")
                key = f"{conn.id}:{pool_name}"
                visible_keys.add(key)
                self._add_plain_row(
                    self.imported_table_rows,
                    row_idx,
                    self.imported_table_columns,
                    [
                        conn.name,
                        pool_name,
                        tr("export_btn"),
                    ],
                    action_col=2,
                    action_callback=(lambda p=pool_name, cid=conn.id: self.export_pool_by_name(p, conn_id=cid)),
                    on_row_click=lambda cid=conn.id, p=pool_name: self._on_select_imported_pool(cid, p),
                    selected=bool(self.selected_imported_pool == (conn.id, pool_name)),
                )
                row_idx += 1

        if row_idx == 0:
            self.selected_imported_pool = None
            self._add_plain_row(
                self.imported_table_rows,
                0,
                self.imported_table_columns,
                [tr("label_no_pools"), "", ""],
            )
            self._render_pool_properties_rows([])
            self._render_pool_status_text("")
        else:
            if self.selected_imported_pool:
                sel_key = f"{self.selected_imported_pool[0]}:{self.selected_imported_pool[1]}"
                if sel_key not in visible_keys:
                    self.selected_imported_pool = None
            if self.selected_imported_pool:
                self._render_selected_pool_properties_from_cache()
            else:
                self._render_pool_properties_rows([])
                self._render_pool_status_text("")

    def _render_selected_pool_properties_from_cache(self) -> None:
        if not self.selected_imported_pool:
            self._render_pool_properties_rows([])
            self._render_pool_status_text("")
            return
        conn_id, pool_name = self.selected_imported_pool
        props_cache_key = f"{conn_id}:{pool_name}"
        status_cache_key = self._pool_status_cache_key(conn_id, pool_name)
        if props_cache_key in self.pool_properties_cache:
            self._render_pool_properties_rows(self.pool_properties_cache.get(props_cache_key, []))
        else:
            # Fallback local sin ejecutar comandos remotos.
            basic_rows: List[Dict[str, str]] = []
            profile = self.store.get(conn_id)
            state = self.states.get(conn_id)
            pool_row: Optional[Dict[str, str]] = None
            if state and state.imported:
                for row in state.imported:
                    if (row.get("pool", "") or "").strip() == pool_name:
                        pool_row = row
                        break
            if profile:
                basic_rows.append({"property": "connection", "value": profile.name, "source": "cache"})
            basic_rows.append({"property": "pool", "value": pool_name, "source": "cache"})
            if pool_row:
                for key in ("size", "used", "free", "compressratio", "dedup"):
                    val = (pool_row.get(key, "") or "").strip()
                    if val:
                        basic_rows.append({"property": key, "value": val, "source": "cache"})
            self._render_pool_properties_rows(basic_rows if basic_rows else [])
        if status_cache_key in self.pool_status_cache:
            self._render_pool_status_text(self.pool_status_cache.get(status_cache_key, ""))
        else:
            self._render_pool_status_text("(sin estado en cache)")

    def _render_all_importable_pools(self) -> None:
        try:
            self._clear_plain_table(self.importable_table_rows)
            row_idx = 0
            for conn in self.store.connections:
                state = self.states.get(conn.id)
                if not state or not state.importable:
                    continue
                for pool in state.importable:
                    pool_name = pool.get("pool", "")
                    pool_state = (pool.get("state", "") or "").strip()
                    is_online = pool_state.upper() == "ONLINE"
                    action_text = tr("import_btn") if is_online else ""
                    self._add_plain_row(
                        self.importable_table_rows,
                        row_idx,
                        self.importable_table_columns,
                        [
                            conn.name,
                            pool_name,
                            action_text,
                        ],
                        action_col=2,
                        action_callback=((lambda p=pool_name, cid=conn.id: self.import_pool_by_name(p, conn_id=cid)) if is_online else None),
                    )
                    row_idx += 1

            if row_idx == 0:
                self._add_plain_row(
                    self.importable_table_rows,
                    0,
                    self.importable_table_columns,
                    [tr("label_no_importable_pools"), "", ""],
                )
        except Exception as exc:
            self._app_log("normal", f"Error renderizando pools importables: {exc}")

    def _on_select_imported_pool(self, conn_id: str, pool_name: str) -> None:
        if self._reject_if_ssh_busy():
            return
        if self.selected_imported_pool == (conn_id, pool_name):
            return
        self.selected_imported_pool = (conn_id, pool_name)
        self._render_selected_pool_properties_from_cache()

    def _render_pool_properties_rows(self, rows: List[Dict[str, str]]) -> None:
        self._clear_plain_table(self.pool_props_rows)
        if not rows:
            self._add_plain_row(self.pool_props_rows, 0, self.pool_props_columns, [tr("label_select_pool"), "", ""])
            return
        for idx, row in enumerate(rows):
            self._add_plain_row(
                self.pool_props_rows,
                idx,
                self.pool_props_columns,
                [row.get("property", ""), row.get("value", ""), row.get("source", "")],
            )

    def _render_pool_status_text(self, text: str) -> None:
        body = (text or "").strip()
        if not body:
            body = tr("label_select_pool")
        self.pool_status_text.configure(state="normal")
        self.pool_status_text.delete("1.0", "end")
        self.pool_status_text.insert("1.0", body)
        self.pool_status_text.configure(state="disabled")

    def _load_selected_pool_properties(self) -> None:
        if not self.selected_imported_pool:
            self._render_pool_properties_rows([])
            self._render_pool_status_text("")
            return
        conn_id, pool_name = self.selected_imported_pool
        profile = self.store.get(conn_id)
        if not profile:
            self._render_pool_properties_rows([])
            self._render_pool_status_text("")
            return
        props_cache_key = f"{conn_id}:{pool_name}"
        status_cache_key = self._pool_status_cache_key(conn_id, pool_name)
        if props_cache_key in self.pool_properties_cache and status_cache_key in self.pool_status_cache:
            self._render_pool_properties_rows(self.pool_properties_cache[props_cache_key])
            self._render_pool_status_text(self.pool_status_cache.get(status_cache_key, ""))
            return
        if props_cache_key in self.pool_props_loading_keys:
            return
        self.status_var.set(trf("status_loading_pool_props", pool=pool_name, name=profile.name))
        self._app_log("info", trf("log_loading_pool_props", pool=pool_name, name=profile.name))
        self.pool_props_loading_count += 1
        self.pool_props_loading_keys.add(props_cache_key)
        self._refresh_busy_cursor()

        def worker() -> None:
            try:
                execu = make_executor(profile)
                rows = run_with_timeout(
                    lambda: execu.list_pool_properties(pool_name),
                    REFRESH_TIMEOUT_SECONDS,
                    trf("error_timeout_refresh_connection", name=profile.name),
                )
                status_text = run_with_timeout(
                    lambda: execu.pool_status_verbose(pool_name),
                    REFRESH_TIMEOUT_SECONDS,
                    trf("error_timeout_refresh_connection", name=profile.name),
                )
                self.pool_properties_cache[props_cache_key] = rows
                with self.pool_status_lock:
                    self.pool_status_cache[status_cache_key] = (status_text or "").strip()
                self.after(0, lambda: self._render_pool_properties_rows(rows))
                self.after(0, lambda: self._render_pool_status_text(status_text))
                self.after(0, lambda: self.status_var.set(trf("status_pool_props_loaded", pool=pool_name, name=profile.name)))
                self._app_log("debug", trf("log_pool_props_loaded", pool=pool_name, name=profile.name, count=len(rows)))
            except Exception as exc:
                self._app_log("normal", trf("log_pool_props_error", pool=pool_name, name=profile.name, error=exc))
                # Cachea el error para evitar relanzar comandos al volver a seleccionar.
                self.pool_properties_cache[props_cache_key] = []
                with self.pool_status_lock:
                    self.pool_status_cache[status_cache_key] = str(exc)
                self.after(0, lambda: self.status_var.set(trf("status_pool_props_error", name=profile.name)))
                self.after(0, lambda: self._render_pool_properties_rows([]))
                self.after(0, lambda e=exc: self._render_pool_status_text(str(e)))
            finally:
                def _finish_pool_props_loading() -> None:
                    self.pool_props_loading_count = max(0, self.pool_props_loading_count - 1)
                    self.pool_props_loading_keys.discard(props_cache_key)
                    self._refresh_busy_cursor()
                self.after(0, _finish_pool_props_loading)

        threading.Thread(target=worker, daemon=True).start()

    def add_connection(self) -> None:
        if self._reject_if_ssh_busy():
            return
        dlg = ConnectionDialog(self)
        self.wait_window(dlg)
        if dlg.result:
            self._app_log("normal", trf("log_connection_saved", name=dlg.result.name))
            self.store.upsert(dlg.result)
            self._load_connections_list()
            self.refresh_all_connections()

    def edit_connection(self) -> None:
        if self._reject_if_ssh_busy():
            return
        profile = self._selected_profile()
        if not profile:
            messagebox.showwarning(tr("action_edit"), tr("warning_select_connection"))
            return
        dlg = ConnectionDialog(self, profile=profile)
        self.wait_window(dlg)
        if dlg.result:
            self._app_log("normal", trf("log_connection_edited", name=dlg.result.name))
            self.store.upsert(dlg.result)
            self._load_connections_list()
            self.refresh_all_connections()

    def delete_connection(self) -> None:
        if self._reject_if_ssh_busy():
            return
        profile = self._selected_profile()
        if not profile:
            messagebox.showwarning(tr("action_delete"), tr("warning_select_connection"))
            return
        if not messagebox.askyesno(tr("confirm"), trf("confirm_delete_connection", name=profile.name)):
            return
        self._app_log("normal", trf("log_connection_deleted", name=profile.name))
        self.store.delete(profile.id)
        self.states.pop(profile.id, None)
        self.selected_conn_id = None
        self._load_connections_list()
        self.status_var.set(tr("status_ready"))
        current = self.left_tabs.select()
        if current and str(current) == str(self.tab_datasets):
            self._load_datasets_for_active_connection()

    def refresh_selected(self) -> None:
        if self._reject_if_ssh_busy():
            return
        profile = self._selected_profile()
        if not profile:
            messagebox.showwarning(tr("action_refresh"), tr("warning_select_connection"))
            return
        self._app_log("normal", trf("log_refresh_connection", name=profile.name))
        self._run_background_refresh([profile])

    def _refresh_connection_by_id(self, conn_id: Optional[str]) -> None:
        if not conn_id:
            self.refresh_selected()
            return
        profile = self.store.get(conn_id)
        if profile:
            self._run_background_refresh([profile])

    def refresh_all_connections(self) -> None:
        if self._reject_if_ssh_busy():
            return
        self._app_log("normal", tr("log_refresh_all"))
        self._run_background_refresh(self.store.connections)

    def connect_all(self) -> None:
        if self._reject_if_ssh_busy():
            return
        self.status_var.set(tr("status_connecting_all"))
        self._app_log("normal", tr("log_connect_all_start"))
        self.refresh_all_connections()

    def _run_background_refresh(self, profiles: List[ConnectionProfile]) -> None:
        if not profiles:
            return

        def worker() -> None:
            def find_duplicate_mountpoints(rows: List[Dict[str, str]]) -> Dict[str, List[str]]:
                grouped: Dict[str, List[str]] = {}
                for row in rows or []:
                    mp = (row.get("mountpoint", "") or "").strip()
                    ds = (row.get("dataset", "") or "").strip()
                    if not mp or not ds:
                        continue
                    grouped.setdefault(mp, []).append(ds)
                return {mp: sorted(set(dsets)) for mp, dsets in grouped.items() if len(set(dsets)) > 1}

            def emit_duplicate_mountpoints_warning(profile: ConnectionProfile, state: ConnectionState) -> None:
                duplicates = find_duplicate_mountpoints(state.mounted_datasets or [])
                if not duplicates:
                    return
                for mp, datasets in sorted(duplicates.items()):
                    ds_txt = ", ".join(datasets)
                    self._app_log("warning", f"Mountpoint duplicado en {profile.name}: {mp} -> {ds_txt}")

            def psrp_connectivity_hint(message: str) -> Optional[str]:
                msg = (message or "").lower()
                tokens = (
                    "no route to host",
                    "connection timed out",
                    "timed out",
                    "failed to establish a new connection",
                    "connection refused",
                    "max retries exceeded",
                    "network is unreachable",
                )
                if not any(t in msg for t in tokens):
                    return None
                return (
                    "Sugerencia PSRP: no hay conectividad WinRM. "
                    "Verifica listener/puerto 5986 (o 5985), firewall en Windows "
                    "y conectividad desde Linux (ej. `nc -vz host 5986`)."
                )

            def refresh_one(profile: ConnectionProfile) -> Tuple[str, ConnectionState]:
                state = ConnectionState()
                conn_label = profile.transport if profile.os_type == "Windows" else profile.conn_type
                self._app_log("normal", f"Inicio refresh: {profile.name} [{conn_label}]")
                if profile.os_type == "Windows":
                    state.ok = False
                    state.message = WINDOWS_CONN_BLOCK_MSG
                    self._app_log(
                        "normal",
                        f"Fin refresh: {profile.name} [{conn_label}] -> ERROR ({state.message})",
                    )
                    return profile.id, state
                try:
                    def refresh_profile() -> ConnectionState:
                        local_state = ConnectionState()
                        execu = make_executor(profile)
                        ok, msg = execu.check_connection()
                        local_state.ok = ok
                        local_state.message = msg
                        if ok:
                            local_state.zfs_version = format_openzfs_version(execu.get_zfs_version())
                            local_state.sudo_ok = execu.check_sudo()
                            local_state.imported = execu.list_imported_pools()
                            # No precargar dispositivos por pool durante refresh:
                            # en algunos destinos (especialmente PSRP/Windows) esta
                            # consulta puede bloquearse y provocar desconexiones falsas.
                            local_state.imported_devices = {}
                            # Precargar estado verbose por pool para el tab Estado.
                            status_map: Dict[str, str] = {}
                            for pool_row in local_state.imported:
                                pool_name = (pool_row.get("pool", "") or "").strip()
                                if not pool_name:
                                    continue
                                try:
                                    txt = (execu.pool_status_verbose(pool_name) or "").strip()
                                    status_map[pool_name] = txt or "(sin salida)"
                                except Exception as exc:
                                    status_map[pool_name] = str(exc)
                            local_state.imported_status = status_map
                            local_state.importable = execu.list_importable_pools()
                            pool_names = [
                                (r.get("pool", "") or "").strip()
                                for r in local_state.imported
                                if (r.get("pool", "") or "").strip()
                            ]
                            try:
                                local_state.dataset_properties = execu.list_all_dataset_properties(pool_names)
                            except Exception:
                                local_state.dataset_properties = {}
                            try:
                                local_state.mounted_datasets = execu.list_mounted_datasets()
                            except Exception:
                                local_state.mounted_datasets = []
                        return local_state

                    # En PSRP cada comando remoto ya tiene su propio timeout.
                    # Evitar un timeout global corto de refresh que genera falsos ERROR
                    # en conexiones lentas tras importar/exportar pools.
                    if profile.conn_type == "PSRP":
                        state = refresh_profile()
                    else:
                        state = run_with_timeout(
                            refresh_profile,
                            REFRESH_TIMEOUT_SECONDS,
                            trf("error_timeout_refresh_connection", name=profile.name),
                        )
                except Exception as exc:
                    state.ok = False
                    state.message = str(exc)
                self._app_log(
                    "normal",
                    f"Fin refresh: {profile.name} [{conn_label}] -> {'OK' if state.ok else 'ERROR'} ({state.message})",
                )
                if not state.ok and profile.conn_type == "PSRP":
                    hint = psrp_connectivity_hint(state.message)
                    if hint:
                        self._app_log("normal", hint)
                return profile.id, state

            max_workers = max(1, min(8, len(profiles)))
            batch_mode = len(profiles) > 1
            refreshed_ids: List[str] = []
            self._app_log("info", trf("log_parallel_refresh_start", count=len(profiles), workers=max_workers))
            work_q: queue.Queue[ConnectionProfile] = queue.Queue()
            done_q: queue.Queue[Tuple[ConnectionProfile, str, ConnectionState]] = queue.Queue()
            for p in profiles:
                work_q.put(p)

            def refresh_worker() -> None:
                while True:
                    try:
                        p = work_q.get_nowait()
                    except queue.Empty:
                        break
                    try:
                        profile_id, state = refresh_one(p)
                    except Exception as exc:
                        profile_id = p.id
                        state = ConnectionState(ok=False, message=str(exc))
                    finally:
                        work_q.task_done()
                    done_q.put((p, profile_id, state))

            for _ in range(max_workers):
                threading.Thread(target=refresh_worker, daemon=True).start()

            remaining = len(profiles)
            while remaining > 0:
                profile, profile_id, state = done_q.get()
                remaining -= 1
                self.states[profile_id] = state
                refreshed_ids.append(profile_id)
                self._app_log(
                    "info",
                    trf(
                        "log_refresh_done",
                        name=profile.name,
                        result=tr("priv_ok") if state.ok else tr("label_error"),
                        msg=state.message,
                    ),
                )
                self._app_log(
                    "debug",
                    trf(
                        "log_refresh_detail",
                        name=profile.name,
                        imported=len(state.imported),
                        importable=len(state.importable),
                        sudo=state.sudo_ok,
                    ),
                )
                if state.zfs_version:
                    self._app_log("debug", f"OpenZFS {profile.name}: {state.zfs_version}")
                emit_duplicate_mountpoints_warning(profile, state)
                # Mantener cache de propiedades/estado de pools entre refrescos
                # para evitar re-ejecutar zpool get/status al hacer click en pools importados.
                status_prefix = f"{profile_id}:"
                with self.pool_status_lock:
                    self.pool_status_cache = {
                        k: v for k, v in self.pool_status_cache.items() if not k.startswith(status_prefix)
                    }
                    for pool_name, status_text in (state.imported_status or {}).items():
                        self.pool_status_cache[f"{profile_id}:{pool_name}"] = (status_text or "").strip() or "(sin salida)"
                    self.pool_status_loading = {
                        k for k in self.pool_status_loading if not k.startswith(status_prefix)
                    }
                self.dataset_properties_cache[profile_id] = dict(state.dataset_properties or {})

                if not batch_mode:
                    self.after(0, self._load_connections_list)
                    self.after(0, self._render_all_imported_pools)
                    self.after(0, self._render_all_importable_pools)
                    self.after(0, self._update_if_selected, profile_id)
                    self.after(0, self._refresh_datasets_if_tab_visible)

            if batch_mode:
                def apply_batch_refresh() -> None:
                    self._load_connections_list()
                    self._render_all_imported_pools()
                    self._render_all_importable_pools()
                    sel = self.selected_conn_id
                    if sel and sel in set(refreshed_ids):
                        profile = self.store.get(sel)
                        if profile:
                            self._render_connection_state(profile)
                    self._refresh_datasets_if_tab_visible()
                self.after(0, apply_batch_refresh)
            self._app_log("normal", tr("log_parallel_refresh_end"))

        threading.Thread(target=worker, daemon=True).start()

    def _update_if_selected(self, conn_id: str) -> None:
        if self.selected_conn_id == conn_id:
            profile = self.store.get(conn_id)
            if profile:
                self._render_connection_state(profile)

    def _refresh_datasets_if_tab_visible(self) -> None:
        current = self.left_tabs.select()
        if current and str(current) == str(self.tab_datasets):
            self._load_datasets_for_active_connection()

    def _update_caches_after_mutation(self, conn_ids: List[str]) -> None:
        seen: set[str] = set()
        for conn_id in conn_ids:
            cid = (conn_id or "").strip()
            if not cid or cid in seen:
                continue
            seen.add(cid)
            self.datasets_cache = {k: v for k, v in self.datasets_cache.items() if not k.startswith(f"{cid}:")}
            self.pool_properties_cache = {
                k: v for k, v in self.pool_properties_cache.items() if not k.startswith(f"{cid}:")
            }
            self.dataset_properties_cache.pop(cid, None)
            self.pool_props_loading_keys = {k for k in self.pool_props_loading_keys if not k.startswith(f"{cid}:")}
            with self.pool_status_lock:
                self.pool_status_cache = {k: v for k, v in self.pool_status_cache.items() if not k.startswith(f"{cid}:")}
                self.pool_status_loading = {k for k in self.pool_status_loading if not k.startswith(f"{cid}:")}
            self._refresh_connection_by_id(cid)

    def import_pool_by_name(self, pool_name: str, conn_id: Optional[str] = None) -> None:
        if self._reject_if_ssh_busy():
            return
        profile = self.store.get(conn_id) if conn_id else self._selected_profile()
        if not profile:
            messagebox.showwarning(tr("import_btn"), tr("warning_select_connection"))
            return
        if not pool_name:
            messagebox.showerror(tr("import_btn"), tr("import_invalid_pool"))
            return

        dlg = ImportDialog(self, pool_name)
        self.wait_window(dlg)
        if not dlg.result:
            return

        self.status_var.set(trf("status_importing_pool", pool=pool_name, name=profile.name))
        self._app_log("normal", trf("log_import_requested", name=profile.name, pool=pool_name))
        self._ssh_log(f"[ACTION] {trf('log_import_requested', name=profile.name, pool=pool_name)}")

        def worker() -> None:
            try:
                execu = make_executor(profile)
                out = execu.import_pool(pool_name, dlg.result or {})
                self._app_log("info", trf("log_import_done", name=profile.name, pool=pool_name))
                self._app_log("debug", trf("log_import_output", name=profile.name, pool=pool_name, output=out.strip() or tr("label_no_output")))
                self._ssh_log(f"[ACTION-END] {trf('log_import_done', name=profile.name, pool=pool_name)}")
                self.after(0, lambda cid=profile.id: self._update_caches_after_mutation([cid]))
                self.after(
                    0,
                    lambda: messagebox.showinfo(
                        tr("import_done_title"),
                        trf("import_done_msg", pool=pool_name, output=out.strip() or tr("label_no_output")),
                    ),
                )
            except Exception as exc:
                self._app_log("normal", trf("log_import_error", name=profile.name, pool=pool_name, error=exc))
                self._ssh_log(f"[ACTION-END] {trf('log_import_error', name=profile.name, pool=pool_name, error=exc)}")
                detail = f"{exc}\n\n{traceback.format_exc()}"
                self.after(0, lambda: messagebox.showerror(tr("import_error_title"), detail))

        threading.Thread(target=worker, daemon=True).start()

    def export_pool_by_name(self, pool_name: str, conn_id: Optional[str] = None) -> None:
        if self._reject_if_ssh_busy():
            return
        profile = self.store.get(conn_id) if conn_id else self._selected_profile()
        if not profile:
            messagebox.showwarning(tr("export_btn"), tr("warning_select_connection"))
            return
        if not pool_name:
            messagebox.showerror(tr("export_btn"), tr("warning_select_pool"))
            return
        if not messagebox.askyesno(tr("confirm"), trf("confirm_export_pool", pool=pool_name, name=profile.name)):
            return

        self.status_var.set(trf("status_exporting_pool", pool=pool_name, name=profile.name))
        self._app_log("normal", trf("log_export_requested", name=profile.name, pool=pool_name))
        self._ssh_log(f"[ACTION] {trf('log_export_requested', name=profile.name, pool=pool_name)}")

        def worker() -> None:
            try:
                execu = make_executor(profile)
                out = execu.export_pool(pool_name)
                self._app_log("info", trf("log_export_done", name=profile.name, pool=pool_name))
                self._app_log("debug", trf("log_export_output", name=profile.name, pool=pool_name, output=out.strip() or tr("label_no_output")))
                self._ssh_log(f"[ACTION-END] {trf('log_export_done', name=profile.name, pool=pool_name)}")
                self.after(0, lambda cid=profile.id: self._update_caches_after_mutation([cid]))
                self.after(
                    0,
                    lambda: messagebox.showinfo(
                        tr("export_done_title"),
                        trf("export_done_msg", pool=pool_name, output=out.strip() or tr("label_no_output")),
                    ),
                )
            except Exception as exc:
                self._app_log("normal", trf("log_export_error", name=profile.name, pool=pool_name, error=exc))
                self._ssh_log(f"[ACTION-END] {trf('log_export_error', name=profile.name, pool=pool_name, error=exc)}")
                detail = f"{exc}\n\n{traceback.format_exc()}"
                self.after(0, lambda: messagebox.showerror(tr("export_error_title"), detail))

        threading.Thread(target=worker, daemon=True).start()


def _ask_master_password() -> Optional[str]:
    global CURRENT_LANG
    has_gui = os.name == "nt" or bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    if not has_gui:
        try:
            return getpass.getpass(tr("master_prompt_terminal"))
        except (EOFError, KeyboardInterrupt):
            return None

    try:
        root = tk.Tk()
        root.title(tr("master_title"))
        root.resizable(False, False)
        root.attributes("-topmost", True)
        _apply_window_icon(root)

        result: Dict[str, Optional[str]] = {"password": None}

        lang_var = tk.StringVar(value=LANG_OPTIONS.get(CURRENT_LANG, "Español"))
        prompt_var = tk.StringVar(value=tr("master_prompt"))
        lang_label_var = tk.StringVar(value=tr("language"))
        pass_var = tk.StringVar()

        def apply_language() -> None:
            root.title(tr("master_title"))
            prompt_var.set(tr("master_prompt"))
            lang_label_var.set(tr("language"))
            ok_btn.configure(text=tr("accept"))
            cancel_btn.configure(text=tr("cancel"))
            try:
                change_btn.configure(text=tr("master_change_btn"))
            except Exception:
                pass

        def on_lang_change(_event: Any = None) -> None:
            global CURRENT_LANG
            selected_label = lang_var.get()
            CURRENT_LANG = LANG_LABEL_TO_CODE.get(selected_label, "es")
            save_preferred_language_to_ini(CONNECTIONS_FILE, CURRENT_LANG)
            apply_language()

        def on_ok() -> None:
            result["password"] = pass_var.get()
            root.destroy()

        def on_cancel() -> None:
            result["password"] = None
            root.destroy()

        def on_change_password() -> None:
            dlg = tk.Toplevel(root)
            dlg.title(tr("master_change_title"))
            dlg.resizable(False, False)
            dlg.transient(root)
            dlg.attributes("-topmost", True)
            dlg.columnconfigure(0, weight=1)
            dlg.rowconfigure(0, weight=1)

            current_var = tk.StringVar()
            new_var = tk.StringVar()
            confirm_var = tk.StringVar()

            body = ttk.Frame(dlg, padding=12)
            body.grid(row=0, column=0, sticky="nsew")
            body.columnconfigure(1, weight=1)

            ttk.Label(body, text=tr("master_current_password")).grid(row=0, column=0, sticky="w", padx=(0, 8), pady=(0, 6))
            cur_entry = ttk.Entry(body, textvariable=current_var, show="*", width=34)
            cur_entry.grid(row=0, column=1, sticky="ew", pady=(0, 6))

            ttk.Label(body, text=tr("master_new_password")).grid(row=1, column=0, sticky="w", padx=(0, 8), pady=(0, 6))
            new_entry = ttk.Entry(body, textvariable=new_var, show="*", width=34)
            new_entry.grid(row=1, column=1, sticky="ew", pady=(0, 6))

            ttk.Label(body, text=tr("master_confirm_password")).grid(row=2, column=0, sticky="w", padx=(0, 8), pady=(0, 8))
            confirm_entry = ttk.Entry(body, textvariable=confirm_var, show="*", width=34)
            confirm_entry.grid(row=2, column=1, sticky="ew", pady=(0, 8))

            def close_dlg() -> None:
                try:
                    dlg.grab_release()
                except Exception:
                    pass
                dlg.destroy()

            def apply_change() -> None:
                current_password = current_var.get()
                new_password = new_var.get()
                confirm_password = confirm_var.get()
                if new_password != confirm_password:
                    messagebox.showerror(tr("master_change_title"), tr("master_change_mismatch"), parent=dlg)
                    return
                try:
                    _change_master_password(current_password, new_password)
                except Exception as exc:
                    messagebox.showerror(tr("master_change_title"), str(exc), parent=dlg)
                    return
                messagebox.showinfo(tr("master_change_title"), tr("master_change_success"), parent=dlg)
                close_dlg()

            actions = ttk.Frame(body)
            actions.grid(row=3, column=0, columnspan=2, sticky="e")
            ttk.Button(actions, text=tr("accept"), command=apply_change).grid(row=0, column=0, padx=(0, 6))
            ttk.Button(actions, text=tr("cancel"), command=close_dlg).grid(row=0, column=1)

            dlg.protocol("WM_DELETE_WINDOW", close_dlg)
            dlg.bind("<Return>", lambda _e: apply_change())
            dlg.bind("<Escape>", lambda _e: close_dlg())
            dlg.update_idletasks()
            dlg.deiconify()
            dlg.lift()
            dlg.focus_force()
            cur_entry.focus_set()
            dlg.grab_set()
            dlg.wait_window()

        frm = ttk.Frame(root, padding=12)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, textvariable=lang_label_var).grid(row=0, column=0, sticky="w", padx=(0, 8), pady=(0, 8))
        lang_combo = ttk.Combobox(frm, textvariable=lang_var, values=list(LANG_OPTIONS.values()), state="readonly", width=16)
        lang_combo.grid(row=0, column=1, sticky="ew", pady=(0, 8))
        lang_combo.bind("<<ComboboxSelected>>", on_lang_change)

        ttk.Label(frm, textvariable=prompt_var, wraplength=420, justify="left").grid(
            row=1,
            column=0,
            columnspan=2,
            sticky="w",
            pady=(0, 8),
        )

        ttk.Label(
            frm,
            text="Autor: Eladio Linares | Licencia: GNU",
            justify="left",
            foreground=UI_MUTED,
        ).grid(row=2, column=0, columnspan=2, sticky="w", pady=(0, 8))

        entry = ttk.Entry(frm, textvariable=pass_var, show="*", width=38)
        entry.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(0, 10))

        actions = ttk.Frame(frm)
        actions.grid(row=4, column=0, columnspan=2, sticky="e")
        change_btn = ttk.Button(actions, command=on_change_password)
        change_btn.grid(row=0, column=0, padx=(0, 6))
        ok_btn = ttk.Button(actions, command=on_ok)
        ok_btn.grid(row=0, column=1, padx=(0, 6))
        cancel_btn = ttk.Button(actions, command=on_cancel)
        cancel_btn.grid(row=0, column=2)

        apply_language()
        root.update_idletasks()
        root.deiconify()
        root.lift()
        root.focus_force()
        entry.focus_set()
        root.protocol("WM_DELETE_WINDOW", on_cancel)
        root.bind("<Escape>", lambda _e: on_cancel())
        root.bind("<Return>", lambda _e: on_ok())
        root.bind("<Control-c>", lambda _e: on_cancel())
        root.mainloop()
        return result["password"]
    except tk.TclError:
        try:
            return getpass.getpass(tr("master_prompt_terminal"))
        except (EOFError, KeyboardInterrupt):
            return None


def _show_startup_error(message: str) -> None:
    try:
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        messagebox.showerror(tr("app_title"), message, parent=root)
        root.destroy()
    except tk.TclError:
        if sys.platform == "darwin":
            try:
                safe = message.replace("\\", "\\\\").replace('"', '\\"')
                subprocess.run(
                    ["osascript", "-e", f'display alert "{tr("app_title")}" message "{safe}"'],
                    check=False,
                )
            except Exception:
                pass
        print(message, file=sys.stderr)


def _write_startup_error_log(message: str) -> None:
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        path = CONFIG_DIR / "startup_error.log"
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        with path.open("a", encoding="utf-8", errors="replace") as fh:
            fh.write(f"[{ts}] {message}\n")
    except Exception:
        pass


def _change_master_password(current_password: str, new_password: str) -> None:
    if not new_password:
        raise ValueError(tr("master_empty_error"))
    store = ConnectionStore(CONNECTIONS_FILE, current_password)
    store.cipher = SecretCipher(new_password)
    store.save()


def prompt_master_password_and_load_store() -> ConnectionStore:
    while True:
        password = _ask_master_password()
        if password is None:
            raise SystemExit(1)
        if not password:
            _show_startup_error(tr("master_empty_error"))
            continue
        try:
            store = ConnectionStore(CONNECTIONS_FILE, password)
            # Reescribe para migrar passwords en claro a formato cifrado.
            store.save()
            return store
        except ValueError as exc:
            _show_startup_error(str(exc))


def main() -> None:
    global CURRENT_LANG
    saved_lang = load_preferred_language_from_ini(CONNECTIONS_FILE)
    if saved_lang:
        CURRENT_LANG = saved_lang

    lock = SingleInstanceLock(INSTANCE_LOCK_FILE)
    if not lock.acquire():
        _show_startup_error("ZFSMgr ya esta en ejecucion. Cierra la instancia activa antes de abrir otra.")
        raise SystemExit(1)

    try:
        store = prompt_master_password_and_load_store()
    except KeyboardInterrupt:
        lock.release()
        raise SystemExit(130)

    if os.name == "nt":
        # Evita problemas de escalado en ciertas configuraciones de Windows
        try:
            from ctypes import windll

            windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

    try:
        app = App(store=store, startup_sudo_ok=None)
    except Exception as exc:
        _show_startup_error(f"{exc}\n\n{traceback.format_exc()}")
        lock.release()
        raise SystemExit(1)
    try:
        app.mainloop()
    except KeyboardInterrupt:
        _close_all_remote_sessions()
        lock.release()
        raise SystemExit(130)
    finally:
        _close_all_remote_sessions()
        lock.release()


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:
        detail = f"{exc}\n\n{traceback.format_exc()}"
        _write_startup_error_log(detail)
        _show_startup_error(
            f"{tr('app_title')} failed to start.\n\n{detail}\n\n"
            f"Log: {CONFIG_DIR / 'startup_error.log'}"
        )
        raise
