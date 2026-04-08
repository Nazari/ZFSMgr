Inventario de comandos SSH remotos de ZFSMgr

Este directorio contiene un archivo por plantilla de comando remoto usada por la app.
Cada archivo incluye:
- PATH remoto usado
- Argumentos
- Evento detonador
- Referencias de codigo fuente

Archivos:
- 00_ssh_wrapper_base.txt
- 01_unix_path_wrapper.txt
- 02_sudo_wrappers.txt
- 03_windows_wrap_remote_command.txt
- 10_refresh_os_probe.txt
- 11_refresh_command_detection.txt
- 12_refresh_zfs_version_probe.txt
- 13_refresh_pool_inventory.txt
- 14_refresh_importable_pools_probe.txt
- 15_refresh_mount_inventory.txt
- 16_dataset_inventory_and_guid.txt
- 17_dataset_properties_queries.txt
- 18_permissions_queries.txt
- 19_pool_actions_commands.txt
- 20_pool_create_command.txt
- 21_dataset_manage_commands.txt
- 22_snapshot_commands.txt
- 23_transfer_copy_clone_diff.txt
- 24_transfer_sync_rsync.txt
- 25_transfer_sync_tar_fallback.txt
- 26_advanced_breakdown_assemble.txt
- 27_gsa_probe_and_props.txt
- 28_gsa_install_uninstall.txt
- 29_windows_msys2_install.txt
- 30_mount_prechecks_and_umount.txt
- 31_gsa_log_fetch.txt
- 32_helper_package_manager_probe.txt
- 33_remote_scripts_manager.txt

Nota para siguiente fase (scripts desplegados en remoto):
- Candidatos a extraer primero a $HOME/.config/ZFSMgr/bin:
  - transfer/sync (23,24,25)
  - advanced scripts (26)
  - permisos batch (18)
  - GSA install/probe (27,28)

Instalacion/actualizacion automatica actual:
- Implementada con versionado por tag en refresh remoto Unix y en Sync.
- Detalle en 33_remote_scripts_manager.txt.
