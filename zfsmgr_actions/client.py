from __future__ import annotations

import shlex
import subprocess
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from app import (
    CONNECTIONS_FILE,
    ConnectionProfile,
    ConnectionStore,
    LocalExecutor,
    PSRPExecutor,
    SSHExecutor,
    make_executor,
)


@dataclass(frozen=True)
class ConnectionInfo:
    id: str
    name: str
    conn_type: str
    host: str
    port: int
    username: str
    transport: str
    os_type: str


@dataclass(frozen=True)
class ActionResult:
    connection_id: str
    action: str
    target: str
    output: str


class ZFSMgrActions:
    """API programatica de acciones ZFS reutilizable por otras aplicaciones."""

    def __init__(self, master_password: str, ini_path: Optional[str] = None) -> None:
        path = Path(ini_path).expanduser() if ini_path else CONNECTIONS_FILE
        self.store = ConnectionStore(path, master_password)

    def list_connections(self) -> List[ConnectionInfo]:
        return [
            ConnectionInfo(
                id=p.id,
                name=p.name,
                conn_type=p.conn_type,
                host=p.host,
                port=p.port,
                username=p.username,
                transport=p.transport,
                os_type=p.os_type,
            )
            for p in self.store.connections
        ]

    def refresh_connection(self, connection: str) -> Dict[str, Any]:
        profile = self._profile(connection)
        return make_executor(profile).refresh_state()

    def list_importable_pools(self, connection: str) -> List[Dict[str, str]]:
        return make_executor(self._profile(connection)).list_importable_pools()

    def list_datasets(self, connection: str, pool: str) -> List[Dict[str, str]]:
        return make_executor(self._profile(connection)).list_datasets(pool)

    def import_pool(self, connection: str, pool: str, options: Optional[Dict[str, Any]] = None) -> ActionResult:
        profile = self._profile(connection)
        out = make_executor(profile).import_pool(pool, options or {})
        return ActionResult(profile.id, "import_pool", pool, out or "")

    def export_pool(self, connection: str, pool: str) -> ActionResult:
        profile = self._profile(connection)
        out = make_executor(profile).export_pool(pool)
        return ActionResult(profile.id, "export_pool", pool, out or "")

    def create_dataset(
        self, connection: str, dataset_path: str, options: Optional[Dict[str, Any]] = None
    ) -> ActionResult:
        profile = self._profile(connection)
        out = make_executor(profile).create_dataset(dataset_path, options or {})
        return ActionResult(profile.id, "create_dataset", dataset_path, out or "")

    def modify_dataset(
        self,
        connection: str,
        dataset_path: str,
        properties: Optional[Dict[str, str]] = None,
        rename_to: Optional[str] = None,
    ) -> ActionResult:
        profile = self._profile(connection)
        execu = make_executor(profile)
        out_parts: List[str] = []
        if properties:
            out_parts.append(execu.set_dataset_properties(dataset_path, properties))
        if rename_to:
            out_parts.append(execu.rename_dataset(dataset_path, rename_to))
        return ActionResult(profile.id, "modify_dataset", dataset_path, "\n".join([x for x in out_parts if x]))

    def mount_dataset(self, connection: str, dataset_path: str) -> ActionResult:
        profile = self._profile(connection)
        out = make_executor(profile).mount_dataset(dataset_path)
        return ActionResult(profile.id, "mount_dataset", dataset_path, out or "")

    def unmount_dataset(self, connection: str, dataset_path: str) -> ActionResult:
        profile = self._profile(connection)
        out = make_executor(profile).unmount_dataset(dataset_path)
        return ActionResult(profile.id, "unmount_dataset", dataset_path, out or "")

    def delete_dataset(self, connection: str, dataset_path: str, recursive: bool = False) -> ActionResult:
        profile = self._profile(connection)
        execu = make_executor(profile)
        out = execu.destroy_dataset(dataset_path, recursive=recursive)
        if self._dataset_exists(execu, dataset_path):
            raise RuntimeError(f"dataset still exists after delete: {dataset_path}")
        return ActionResult(profile.id, "delete_dataset", dataset_path, out or "")

    def copy_snapshot(
        self,
        source_connection: str,
        source_snapshot: str,
        dest_connection: str,
        dest_dataset: str,
        recursive: bool = False,
    ) -> ActionResult:
        if source_connection != dest_connection:
            raise NotImplementedError("copy_snapshot entre conexiones distintas no soportado en esta API inicial")
        if "@" not in source_snapshot:
            raise ValueError("source_snapshot debe incluir @snapshot")
        if "@" in dest_dataset:
            raise ValueError("dest_dataset no puede ser snapshot")
        profile, execu = self._profile_executor(source_connection)
        flag = "wLecR" if recursive else "wLec"
        cmd = f"zfs send -{flag} {shlex.quote(source_snapshot)} | zfs recv -F -e {shlex.quote(dest_dataset)}"
        out = self._run_raw(profile, execu, cmd, sudo=True)
        return ActionResult(profile.id, "copy_snapshot", f"{source_snapshot} -> {dest_dataset}", out or "")

    def level_datasets(
        self,
        connection: str,
        source_dataset: str,
        dest_dataset: str,
    ) -> ActionResult:
        profile, execu = self._profile_executor(connection)
        src_snaps = self._snapshot_names(execu, source_dataset)
        dst_snaps = self._snapshot_names(execu, dest_dataset)
        missing = [s for s in src_snaps if s not in dst_snaps]
        if not missing:
            return ActionResult(profile.id, "level_datasets", f"{source_dataset} -> {dest_dataset}", "already leveled")
        common = [s for s in src_snaps if s in dst_snaps]
        target = f"{source_dataset}@{missing[-1]}"
        if common:
            base = f"{source_dataset}@{common[-1]}"
            send = f"zfs send -wLecR -I {shlex.quote(base)} {shlex.quote(target)}"
        else:
            send = f"zfs send -wLecR {shlex.quote(target)}"
        recv = f"zfs recv -F {shlex.quote(dest_dataset)}"
        cmd = f"{send} | {recv}"
        out = self._run_raw(profile, execu, cmd, sudo=True)
        return ActionResult(profile.id, "level_datasets", f"{source_dataset} -> {dest_dataset}", out or "")

    def sync_datasets(self, connection: str, source_dataset: str, dest_dataset: str) -> ActionResult:
        profile, execu = self._profile_executor(connection)
        src_mp = self._resolve_mountpoint(execu, source_dataset)
        dst_mp = self._resolve_mountpoint(execu, dest_dataset)
        if profile.os_type == "Windows":
            cmd = (
                f"robocopy {shlex.quote(src_mp)} {shlex.quote(dst_mp)} "
                "/MIR /COPYALL /DCOPY:DAT /R:1 /W:1 /XJ /NP /NFL /NDL"
            )
        else:
            cmd = f"rsync -aHAWXS --numeric-ids {shlex.quote(src_mp)}/ {shlex.quote(dst_mp)}/"
        out = self._run_raw(profile, execu, cmd, sudo=(profile.conn_type != "PSRP"))
        return ActionResult(profile.id, "sync_datasets", f"{source_dataset} -> {dest_dataset}", out or "")

    def breakdown_dataset(self, connection: str, dataset_path: str) -> ActionResult:
        profile, execu = self._profile_executor(connection)
        mp = self._resolve_mountpoint(execu, dataset_path)
        if profile.os_type == "Windows":
            cmd = (
                "$ErrorActionPreference='Stop'; "
                f"$ds='{dataset_path}'; $mp='{mp}'; "
                "$dirs = @(Get-ChildItem -LiteralPath $mp -Directory -Force -ErrorAction SilentlyContinue); "
                "foreach ($d in $dirs) { "
                "  $safe = ($d.Name -replace '[^A-Za-z0-9_.:-]', '_'); "
                "  if ([string]::IsNullOrWhiteSpace($safe)) { $safe = 'dir' }; "
                "  $child = \"$ds/$safe\"; $i=1; "
                "  while ($true) { try { zfs list -H -o name $child *> $null; $child = \"$ds/$safe-$i\"; $i++ } catch { break } } "
                "  zfs create $child | Out-Null; "
                "  $cmp=(zfs get -H -o value mountpoint $child).Trim(); "
                "  if ($cmp -match '^/') { $cmp = ($env:SystemDrive + ($cmp -replace '/', '\\')) } "
                "  New-Item -ItemType Directory -Path $cmp -Force | Out-Null; "
                "  & robocopy $d.FullName $cmp /E /MOVE /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "}"
            )
            out = self._run_raw(profile, execu, cmd, sudo=False)
        else:
            cmd = (
                "set -e; "
                f"DS={shlex.quote(dataset_path)}; MP={shlex.quote(mp)}; "
                "for d in \"$MP\"/*; do "
                "[ -d \"$d\" ] || continue; "
                "n=\"$(basename \"$d\")\"; safe=\"$(printf '%s' \"$n\" | tr -cd 'A-Za-z0-9_.:-')\"; "
                "[ -n \"$safe\" ] || safe='dir'; child=\"$DS/$safe\"; i=1; "
                "while zfs list -H -o name \"$child\" >/dev/null 2>&1; do child=\"$DS/$safe-$i\"; i=$((i+1)); done; "
                "zfs create \"$child\"; "
                "cmp=\"$(zfs get -H -o value mountpoint \"$child\")\"; "
                "mkdir -p \"$cmp\"; "
                "rsync -aHAWXS --remove-source-files \"$d\"/ \"$cmp\"/; "
                "find \"$d\" -mindepth 1 -type d -empty -delete; "
                "done"
            )
            out = self._run_raw(profile, execu, cmd, sudo=True)
        return ActionResult(profile.id, "breakdown_dataset", dataset_path, out or "")

    def assemble_dataset(self, connection: str, dataset_path: str) -> ActionResult:
        profile, execu = self._profile_executor(connection)
        children = self._child_datasets(execu, dataset_path)
        if not children:
            return ActionResult(profile.id, "assemble_dataset", dataset_path, "no children")
        if profile.os_type == "Windows":
            cmd = (
                "$ErrorActionPreference='Stop'; "
                f"$ds='{dataset_path}'; "
                "$children = @(zfs list -H -o name -r $ds | Where-Object { $_ -like ($ds + '/*') -and $_ -notmatch '@' }); "
                "$children = $children | Sort-Object { ($_ -split '/').Count } -Descending; "
                "foreach ($child in $children) { "
                "  $rel = $child.Substring($ds.Length + 1); "
                "  $cmp=(zfs get -H -o value mountpoint $child).Trim(); "
                "  if ($cmp -match '^/') { $cmp = ($env:SystemDrive + ($cmp -replace '/', '\\')) } "
                "  $dest=(Join-Path ((zfs get -H -o value mountpoint $ds).Trim() -replace '^/', ($env:SystemDrive + '\\')) $rel); "
                "  New-Item -ItemType Directory -Path $dest -Force | Out-Null; "
                "  & robocopy $cmp $dest /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null; "
                "  zfs destroy -r -f $child | Out-Null; "
                "}"
            )
            out = self._run_raw(profile, execu, cmd, sudo=False)
        else:
            cmd = (
                "set -e; "
                f"DS={shlex.quote(dataset_path)}; "
                "children=\"$(zfs list -H -o name -r \"$DS\" | awk -v root=\"$DS\" 'index($0, root\"/\")==1 && index($0,\"@\")<1 {print $0}' | awk -F'/' '{print NF\" \"$0}' | sort -nr | cut -d' ' -f2-)\"; "
                "while IFS= read -r child; do "
                "[ -n \"$child\" ] || continue; rel=\"${child#$DS/}\"; "
                "cmp=\"$(zfs get -H -o value mountpoint \"$child\")\"; dmp=\"$(zfs get -H -o value mountpoint \"$DS\")/$rel\"; "
                "mkdir -p \"$dmp\"; rsync -aHAWXS \"$cmp\"/ \"$dmp\"/; zfs destroy -r -f \"$child\"; "
                "done <<EOF\n$children\nEOF"
            )
            out = self._run_raw(profile, execu, cmd, sudo=True)
        return ActionResult(profile.id, "assemble_dataset", dataset_path, out or "")

    def _dataset_exists(self, execu: Any, dataset_path: str) -> bool:
        if "/" in dataset_path:
            pool = dataset_path.split("/", 1)[0]
        else:
            pool = dataset_path.split("@", 1)[0]
        try:
            rows = execu.list_datasets(pool)
        except Exception:
            return False
        return any((r.get("name", "").strip() == dataset_path) for r in rows)

    def _profile(self, connection: str) -> ConnectionProfile:
        # Permite usar id o nombre de conexión.
        for p in self.store.connections:
            if p.id == connection or p.name == connection:
                return p
        raise KeyError(f"Conexion no encontrada: {connection}")

    def _profile_executor(self, connection: str) -> Tuple[ConnectionProfile, Any]:
        profile = self._profile(connection)
        return profile, make_executor(profile)

    def _run_raw(self, profile: ConnectionProfile, execu: Any, command: str, sudo: bool = False) -> str:
        if isinstance(execu, LocalExecutor):
            cmd = command
            if sudo:
                cmd = f"sudo -n sh -lc {shlex.quote(command)}"
            proc = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError((proc.stderr or proc.stdout or cmd).strip())
            return proc.stdout or ""
        if isinstance(execu, SSHExecutor):
            return execu._run(command, sudo=sudo, timeout_seconds=None)  # type: ignore[attr-defined]
        if isinstance(execu, PSRPExecutor):
            return execu._run_ps(command, timeout_seconds=None)  # type: ignore[attr-defined]
        raise RuntimeError(f"Executor no soportado: {type(execu).__name__}")

    def _snapshot_names(self, execu: Any, dataset: str) -> List[str]:
        pool = dataset.split("/", 1)[0]
        rows = execu.list_datasets(pool)
        pref = dataset + "@"
        names = [r.get("name", "").strip() for r in rows if r.get("name", "").strip().startswith(pref)]
        return [n.split("@", 1)[1] for n in names]

    def _resolve_mountpoint(self, execu: Any, dataset: str) -> str:
        props = {r.get("property"): r.get("value", "") for r in execu.list_dataset_properties(dataset)}
        mp = (props.get("mountpoint") or "").strip()
        mounted = (props.get("mounted") or "").strip().lower()
        if mounted not in {"yes", "on", "true"}:
            execu.mount_dataset(dataset)
            props = {r.get("property"): r.get("value", "") for r in execu.list_dataset_properties(dataset)}
            mp = (props.get("mountpoint") or "").strip()
        if not mp or mp.lower() in {"none", "legacy"}:
            raise RuntimeError(f"dataset sin mountpoint usable: {dataset}")
        return mp

    def _child_datasets(self, execu: Any, dataset: str) -> List[str]:
        pool = dataset.split("/", 1)[0]
        rows = execu.list_datasets(pool)
        base = dataset.count("/") + 1
        out: List[str] = []
        pref = dataset + "/"
        for r in rows:
            n = (r.get("name") or "").strip()
            if not n.startswith(pref) or "@" in n:
                continue
            if n.count("/") == base:
                out.append(n)
        return out
