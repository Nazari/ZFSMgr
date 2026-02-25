#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shlex
import sys
import time
from pathlib import Path
from typing import List, Tuple

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app import COMMAND_TIMEOUT_SECONDS, ConnectionStore, SSHExecutor, build_send_flag_candidates


def _find_profile(store: ConnectionStore, name_or_id: str):
    for p in store.connections:
        if p.name == name_or_id or p.id == name_or_id:
            return p
    raise RuntimeError(f"Conexion no encontrada: {name_or_id}")


def _build_send_candidates(src_exec: SSHExecutor, snapshot: str, recursive: bool) -> List[str]:
    version = src_exec.get_zfs_version()
    flags = build_send_flag_candidates(version, recursive=recursive)
    return [f"zfs send -{f} {shlex.quote(snapshot)}" if f else f"zfs send {shlex.quote(snapshot)}" for f in flags]


def _build_recv_candidates(dest_dataset: str, src_snapshot: str, recursive: bool) -> List[str]:
    src_dataset = src_snapshot.split("@", 1)[0]
    src_leaf = src_dataset.rsplit("/", 1)[-1]
    candidates = [f"zfs recv -F -e {shlex.quote(dest_dataset)}"]
    if recursive:
        candidates.append(f"zfs recv -F -d {shlex.quote(dest_dataset)}")
    candidates.append(f"zfs recv -F {shlex.quote(dest_dataset)}")
    candidates.append(f"zfs recv -F {shlex.quote(dest_dataset + '/' + src_leaf)}")
    return list(dict.fromkeys(candidates))


def _remote_cmd(execu: SSHExecutor, raw_cmd: str) -> str:
    p = execu.profile
    cmd = raw_cmd
    if p.use_sudo:
        if p.password:
            cmd = f"sudo -S -p '' -k sh -lc {shlex.quote(raw_cmd)}"
        else:
            cmd = f"sudo -n sh -lc {shlex.quote(raw_cmd)}"
    return execu._wrap_remote_shell(cmd)  # noqa: SLF001 - test helper


def _stream_copy(
    src_exec: SSHExecutor,
    dst_exec: SSHExecutor,
    send_raw: str,
    recv_raw: str,
    timeout_s: int,
) -> Tuple[int, str]:
    src_client = src_exec._connect()  # noqa: SLF001 - test helper
    dst_client = dst_exec._connect()  # noqa: SLF001 - test helper
    src_transport = src_client.get_transport()
    dst_transport = dst_client.get_transport()
    if src_transport is None or dst_transport is None:
        return 1, "transport inactive"
    if not src_transport.is_active() or not dst_transport.is_active():
        return 1, "transport inactive"

    src_cmd = _remote_cmd(src_exec, send_raw)
    dst_cmd = _remote_cmd(dst_exec, recv_raw)
    print(f"SRC$ {src_cmd}")
    print(f"DST$ {dst_cmd}")

    src_ch = src_transport.open_session()
    dst_ch = dst_transport.open_session()
    src_ch.exec_command(src_cmd)
    dst_ch.exec_command(dst_cmd)

    if src_exec.profile.use_sudo and src_exec.profile.password:
        src_ch.send((src_exec.profile.password or "") + "\n")
    if dst_exec.profile.use_sudo and dst_exec.profile.password:
        dst_ch.send((dst_exec.profile.password or "") + "\n")

    transferred = 0
    last_emit = 0.0
    start = time.monotonic()
    stderr_buf = ""
    src_done = False

    while True:
        if timeout_s and (time.monotonic() - start) > timeout_s:
            try:
                src_ch.close()
            except Exception:
                pass
            try:
                dst_ch.close()
            except Exception:
                pass
            return 124, "timeout"

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
                for line in err.splitlines():
                    if line.strip():
                        print(f"SRC! {line.strip()}")
        if dst_ch.recv_stderr_ready():
            err = dst_ch.recv_stderr(131072).decode("utf-8", errors="replace")
            if err:
                moved = True
                stderr_buf += err
                for line in err.splitlines():
                    if line.strip():
                        print(f"DST! {line.strip()}")

        if src_ch.exit_status_ready() and not src_ch.recv_ready() and not src_done:
            src_done = True
            try:
                dst_ch.shutdown_write()
            except Exception:
                pass

        now = time.monotonic()
        if transferred > 0 and now - last_emit >= 1.0:
            print(f"PROGRESS {transferred} bytes")
            last_emit = now

        if src_done and dst_ch.exit_status_ready() and not dst_ch.recv_ready() and not dst_ch.recv_stderr_ready():
            break
        if not moved:
            time.sleep(0.02)

    src_rc = src_ch.recv_exit_status() if src_ch.exit_status_ready() else 1
    dst_rc = dst_ch.recv_exit_status() if dst_ch.exit_status_ready() else 1
    if src_rc != 0 or dst_rc != 0:
        return src_rc or dst_rc or 1, (stderr_buf.strip() or f"send_rc={src_rc} recv_rc={dst_rc}")
    return 0, stderr_buf.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description="Prueba automatizada de copia snapshot send/recv entre conexiones SSH.")
    ap.add_argument("--master-password", required=True, help="Password maestra para desencriptar connections.ini")
    ap.add_argument("--src", default="fc16", help="Conexion origen (nombre o id)")
    ap.add_argument("--snapshot", default="fc16/tmp/Applications@desg", help="Snapshot origen completo")
    ap.add_argument("--dst", default="surface", help="Conexion destino (nombre o id)")
    ap.add_argument("--target", default="games/Juegos", help="Dataset destino")
    ap.add_argument("--recursive", action="store_true", help="Usar send recursivo (flags ...R)")
    ap.add_argument("--timeout", type=int, default=COMMAND_TIMEOUT_SECONDS * 6, help="Timeout por intento")
    ap.add_argument("--src-password", default="", help="Override password origen (opcional)")
    ap.add_argument("--dst-password", default="", help="Override password destino (opcional)")
    args = ap.parse_args()

    store = ConnectionStore(master_password=args.master_password)
    src = _find_profile(store, args.src)
    dst = _find_profile(store, args.dst)
    if src.conn_type != "SSH" or dst.conn_type != "SSH":
        raise RuntimeError("Esta prueba automatizada requiere origen y destino SSH")
    if args.src_password:
        src.password = args.src_password
    if args.dst_password:
        dst.password = args.dst_password

    src_exec = SSHExecutor(src)
    dst_exec = SSHExecutor(dst)
    send_candidates = _build_send_candidates(src_exec, args.snapshot, recursive=args.recursive)
    recv_candidates = _build_recv_candidates(args.target, args.snapshot, recursive=args.recursive)

    print(f"Testing copy: {args.src}::{args.snapshot} -> {args.dst}::{args.target}")
    print(f"Attempts: {len(send_candidates) * len(recv_candidates)}")
    last_err = ""
    for recv in recv_candidates:
        for send in send_candidates:
            print("-" * 80)
            print(f"TRY send={send} | recv={recv}")
            rc, err = _stream_copy(src_exec, dst_exec, send, recv, args.timeout)
            if rc == 0:
                print("RESULT: OK")
                return 0
            last_err = err
            print(f"RESULT: FAIL rc={rc}")
            if err:
                print(err.strip())

    print("RESULT: FAIL")
    if last_err:
        print(last_err)
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Interrupted.", file=sys.stderr)
        raise SystemExit(130)
