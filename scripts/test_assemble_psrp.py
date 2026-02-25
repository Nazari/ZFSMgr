#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app import CONNECTIONS_FILE, LEGACY_INI_FALLBACK, ConnectionStore, PSRPExecutor, ExecutorError


def ps_quote(value: str) -> str:
    return "'" + (value or "").replace("'", "''") + "'"


def build_children_list_script(dataset_name: str) -> str:
    ds_q = ps_quote(dataset_name)
    return (
        "$ErrorActionPreference='Stop'; "
        f"$dataset={ds_q}; "
        "$baseDepth = ($dataset -split '/').Count; "
        "$children = @(zfs list -H -o name -r $dataset | "
        "Where-Object { $_ -like ($dataset + '/*') -and $_ -notmatch '@' -and (($_ -split '/').Count -eq ($baseDepth + 1)) }); "
        "$children = $children | Sort-Object { ($_ -split '/').Count } -Descending; "
        "$children -join \"`n\""
    )


def build_root_prepare_script(dataset_name: str, mountpoint_hint: str) -> str:
    ds_q = ps_quote(dataset_name)
    mp_q = ps_quote(mountpoint_hint)
    return (
        "$ErrorActionPreference='Stop'; "
        f"$dataset={ds_q}; $mpHint={mp_q}; "
        "$tmpSuffix = ($dataset -replace '[^A-Za-z0-9_\\-]', '_'); "
        "$tmpRoot = ('/tmp/zfsmgr-assemble-' + $tmpSuffix); "
        "$tmpRootWin = ($env:SystemDrive + ($tmpRoot -replace '/', '\\')); "
        "New-Item -ItemType Directory -Path $tmpRootWin -Force | Out-Null; "
        "$datasetMpOrig=''; "
        "try { $datasetMpOrig = (zfs get -H -o value mountpoint $dataset).Trim() } catch { $datasetMpOrig='' }; "
        "$datasetDriveOrig=''; "
        "try { $datasetDriveOrig = (zfs get -H -o value driveletter $dataset).Trim() } catch { $datasetDriveOrig='' }; "
        "$datasetTempMp=''; "
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
        "    try { zfs set (\"driveletter=\" + $free) $dataset | Out-Null } catch { throw (\"[ASSEMBLE][ROOT] set driveletter failed: \" + $_.Exception.Message) }; "
        "    try { zfs mount $dataset | Out-Null } catch { throw (\"[ASSEMBLE][ROOT] mount failed after set driveletter: \" + $_.Exception.Message) }; "
        "    $cand = ($free + ':\\\\'); "
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
        "Write-Output ('DATASET_DRIVE_ORIG=' + $datasetDriveOrig); "
    )


def build_child_stage_script(dataset_name: str, child_name: str, root_mp: str, tmp_root: str) -> str:
    ds_q = ps_quote(dataset_name)
    child_q = ps_quote(child_name)
    root_q = ps_quote(root_mp)
    tmp_q = ps_quote(tmp_root)
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


def build_child_finalize_script(stage_tmp: str, child_tmp: str, dest_path: str, child_name: str) -> str:
    stage_q = ps_quote(stage_tmp)
    child_tmp_q = ps_quote(child_tmp)
    dest_q = ps_quote(dest_path)
    child_q = ps_quote(child_name)
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


def build_root_restore_script(dataset_name: str, dataset_temp_mp: str, dataset_mp_orig: str, dataset_drive_orig: str) -> str:
    ds_q = ps_quote(dataset_name)
    dtmp_q = ps_quote(dataset_temp_mp)
    dorig_q = ps_quote(dataset_mp_orig)
    ddo_q = ps_quote(dataset_drive_orig)
    return (
        "$ErrorActionPreference='Stop'; "
        f"$dataset={ds_q}; $datasetTempMp={dtmp_q}; $datasetMpOrig={dorig_q}; $datasetDriveOrig={ddo_q}; "
        "if ($datasetTempMp) { "
        "  try { zfs unmount $dataset | Out-Null } catch {}; "
        "  if ($datasetDriveOrig) { "
        "    try { zfs set (\"driveletter=\" + $datasetDriveOrig) $dataset | Out-Null } catch {} "
        "  } "
        "  if ($datasetMpOrig -and $datasetMpOrig -ne 'none') { "
        "    try { zfs set (\"mountpoint=\" + $datasetMpOrig) $dataset | Out-Null } catch {} "
        "  } else { "
        "    try { zfs inherit mountpoint $dataset | Out-Null } catch {} "
        "  } "
        "}; "
    )


def parse_kv_lines(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in (text or "").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def load_profile(master_password: str, conn_id_or_name: str):
    path = CONNECTIONS_FILE if CONNECTIONS_FILE.exists() else LEGACY_INI_FALLBACK
    store = ConnectionStore(path, master_password)
    for p in store.connections:
        if p.id == conn_id_or_name or p.name == conn_id_or_name:
            return p, path
    raise RuntimeError(f"No se encontro conexion '{conn_id_or_name}' en {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Prueba automatica Ensamblar PSRP")
    ap.add_argument("--master-password", required=True)
    ap.add_argument("--connection", default="surface-psrp")
    ap.add_argument("--dataset", default="games/Juegos/Applications/Windows")
    ap.add_argument("--mountpoint-hint", default="/mnt/games/Juegos/Applications/Windows")
    ap.add_argument("--conn-password", default="")
    ap.add_argument("--host", default="", help="Sobrescribe host de la conexion")
    ap.add_argument("--port", type=int, default=0, help="Sobrescribe puerto de la conexion")
    ap.add_argument("--username", default="", help="Sobrescribe usuario de la conexion")
    ap.add_argument("--auth", default="", help="Sobrescribe auth PSRP (ntlm/basic/kerberos)")
    ap.add_argument("--debug-root", action="store_true", help="Depura la fase root en pasos cortos")
    args = ap.parse_args()

    profile, ini_path = load_profile(args.master_password, args.connection)
    if args.conn_password:
        profile.password = args.conn_password
    if args.host:
        profile.host = args.host
    if args.port:
        profile.port = int(args.port)
    if args.username:
        profile.username = args.username
    if args.auth:
        profile.auth = args.auth

    if profile.conn_type != "PSRP":
        raise RuntimeError(f"La conexion {profile.name} no es PSRP (es {profile.conn_type})")

    execu = PSRPExecutor(profile)
    ok, msg = execu.check_connection()
    print(f"[CHECK] {ok} :: {msg}")
    if not ok:
        return 2

    dataset = args.dataset.strip()
    hint = args.mountpoint_hint.strip()
    print(
        f"[RUN] connection={profile.name} ini={ini_path} host={profile.host}:{profile.port} "
        f"user={profile.username} auth={profile.auth}"
    )
    print(f"[RUN] dataset={dataset}")

    def run_step(label: str, cmd: str) -> None:
        print(f"[STEP] {label}: {cmd}")
        try:
            out = execu._run_ps(cmd, timeout_seconds=30)
            print(f"[STEP-OK] {label}: {(out or '').strip() or '<ok>'}")
        except Exception as exc:
            print(f"[STEP-ERR] {label}: {exc}")
            raise

    if args.debug_root:
        try:
            run_step("list_dataset", f"zfs list -H -o name {dataset}")
            run_step("list_recursive", f"zfs list -H -o name -r {dataset}")
            run_step("get_mountpoint", f"zfs get -H -o value mountpoint {dataset}")
            run_step("get_driveletter", f"zfs get -H -o value driveletter {dataset}")
            run_step("zfs_mount_line", f'zfs mount | findstr /I "{dataset}"')
            run_step("set_driveletter_y", f"zfs set driveletter=Y {dataset}")
            run_step("mount_dataset", f"zfs mount {dataset}")
            run_step("mounted_prop", f"zfs get -H -o value mounted {dataset}")
            run_step("test_y_drive", "$p='Y:\\\\'; if (Test-Path -LiteralPath $p) { 'Y_OK' } else { 'Y_NO' }")
            run_step("test_z_drive", "$p='Z:\\\\'; if (Test-Path -LiteralPath $p) { 'Z_OK' } else { 'Z_NO' }")
        except Exception:
            return 3

    try:
        children_out = execu._run_ps(build_children_list_script(dataset), timeout_seconds=None)
        children = [x.strip() for x in children_out.splitlines() if x.strip()]
        print(f"[INFO] children={len(children)}")
        if not children:
            print("[OK] no child datasets")
            return 0

        prep_out = execu._run_ps(build_root_prepare_script(dataset, hint), timeout_seconds=None)
        prep = parse_kv_lines(prep_out)
        root_mp = prep.get("ROOT_MP", "")
        tmp_root = prep.get("TMP_ROOT", "")
        dataset_temp_mp = prep.get("DATASET_TEMP_MP", "")
        dataset_mp_orig = prep.get("DATASET_MP_ORIG", "")
        dataset_drive_orig = prep.get("DATASET_DRIVE_ORIG", "")
        print(f"[INFO] root_mp={root_mp}")

        for child in children:
            print(f"[CHILD] {child}")
            stage_out = execu._run_ps(build_child_stage_script(dataset, child, root_mp, tmp_root), timeout_seconds=None)
            stage = parse_kv_lines(stage_out)
            stage_tmp = stage.get("STAGE_TMP", "")
            child_tmp = stage.get("CHILD_TMP", "")
            dest_path = stage.get("DEST_PATH", "")
            if not stage_tmp or not dest_path:
                raise RuntimeError(f"Stage invalido para {child}: {stage_out}")
            fin_out = execu._run_ps(build_child_finalize_script(stage_tmp, child_tmp, dest_path, child), timeout_seconds=None)
            if fin_out.strip():
                print(fin_out.strip())

        execu._run_ps(build_root_restore_script(dataset, dataset_temp_mp, dataset_mp_orig, dataset_drive_orig), timeout_seconds=None)
        print("[OK] Ensamblar completado")
        return 0
    except ExecutorError as exc:
        print(f"[ERROR] {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
