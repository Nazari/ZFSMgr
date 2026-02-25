from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from app import CONNECTIONS_FILE, ConnectionProfile, ConnectionStore, make_executor


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
