defmodule ZfsmgrElixir.Zfs do
  @moduledoc """
  ZFS actions service, callable from API or other Elixir apps.

  This module centralizes remote execution and exposes reusable operations:
  - connection refresh
  - pool import/export
  - dataset create/delete/rename
  - dataset property set/inherit
  """

  alias ZfsmgrElixir.{Core, Crypto}
  alias ZfsmgrElixir.Core.Connection

  @default_timeout 90_000

  def refresh_connection(%Connection{} = conn) do
    with {:ok, uname} <- run(conn, base_os_cmd(conn), sudo: false),
         {:ok, zfs_version} <- run(conn, zfs_cmd("zfs version"), sudo: conn.use_sudo),
         {:ok, pools} <-
           run(conn, zpool_cmd("zpool list -H -p -o name,size,alloc,free,cap,dedupratio"),
             sudo: conn.use_sudo
           ) do
      {:ok,
       %{
         os: trim_output(uname.stdout),
         zfs_version: trim_output(zfs_version.stdout),
         pools: parse_lines(pools.stdout),
         checked_at: DateTime.utc_now()
       }}
    end
  end

  def import_pool(%Connection{} = conn, pool_name) when is_binary(pool_name) do
    cmd = zpool_cmd("zpool import #{shell_quote(pool_name)}")
    run(conn, cmd, sudo: conn.use_sudo)
  end

  def export_pool(%Connection{} = conn, pool_name) when is_binary(pool_name) do
    cmd = zpool_cmd("zpool export #{shell_quote(pool_name)}")
    run(conn, cmd, sudo: conn.use_sudo)
  end

  def create_dataset(%Connection{} = conn, attrs) when is_map(attrs) do
    dataset = Map.get(attrs, "dataset") || Map.get(attrs, :dataset)
    kind = Map.get(attrs, "kind") || Map.get(attrs, :kind) || "filesystem"
    opts = Map.get(attrs, "options") || Map.get(attrs, :options) || %{}

    with true <- is_binary(dataset) and dataset != "",
         {:ok, opt_str} <- build_options(opts),
         {:ok, cmd} <- build_create_cmd(kind, dataset, opt_str, attrs) do
      run(conn, zfs_cmd(cmd), sudo: conn.use_sudo)
    else
      false -> {:error, :dataset_required}
      {:error, _} = err -> err
      _ -> {:error, :invalid_request}
    end
  end

  def delete_dataset(%Connection{} = conn, target, recursive \\ false)
      when is_binary(target) and is_boolean(recursive) do
    flags = if recursive, do: "-r ", else: ""
    cmd = zfs_cmd("zfs destroy #{flags}#{shell_quote(target)}")
    run(conn, cmd, sudo: conn.use_sudo)
  end

  def rename_dataset(%Connection{} = conn, source, target)
      when is_binary(source) and is_binary(target) do
    cmd = zfs_cmd("zfs rename #{shell_quote(source)} #{shell_quote(target)}")
    run(conn, cmd, sudo: conn.use_sudo)
  end

  def set_property(%Connection{} = conn, dataset, property, value)
      when is_binary(dataset) and is_binary(property) and is_binary(value) do
    cmd =
      zfs_cmd("zfs set #{shell_quote(property)}=#{shell_quote(value)} #{shell_quote(dataset)}")

    run(conn, cmd, sudo: conn.use_sudo)
  end

  def inherit_property(%Connection{} = conn, dataset, property)
      when is_binary(dataset) and is_binary(property) do
    cmd = zfs_cmd("zfs inherit #{shell_quote(property)} #{shell_quote(dataset)}")
    run(conn, cmd, sudo: conn.use_sudo)
  end

  def list_pools(%Connection{} = conn) do
    with {:ok, result} <- run(conn, zpool_cmd("zpool list -H -o name"), sudo: conn.use_sudo) do
      {:ok, parse_lines(result.stdout)}
    end
  end

  def list_datasets(%Connection{} = conn, pool_name) when is_binary(pool_name) do
    with {:ok, result} <-
           run(
             conn,
             zfs_cmd(
               "zfs list -H -o name -t filesystem,volume,snapshot -r #{shell_quote(pool_name)}"
             ),
             sudo: conn.use_sudo
           ) do
      {:ok, parse_lines(result.stdout)}
    end
  end

  def editable_properties do
    [
      "mountpoint",
      "canmount",
      "compression",
      "atime",
      "readonly",
      "recordsize",
      "quota",
      "reservation",
      "refquota",
      "refreservation",
      "snapdir",
      "sync",
      "primarycache",
      "secondarycache",
      "logbias",
      "relatime"
    ]
  end

  def run(%Connection{} = conn, command, opts \\ []) when is_binary(command) do
    timeout = Keyword.get(opts, :timeout, @default_timeout)
    sudo? = Keyword.get(opts, :sudo, false)

    effective_command =
      if sudo? do
        wrap_sudo(command, conn)
      else
        command
      end

    case String.upcase(to_string(conn.transport || "SSH")) do
      "PSRP" ->
        {:error, :psrp_not_implemented}

      _ ->
        run_over_ssh_or_local(conn, effective_command, timeout)
    end
  end

  defp run_over_ssh_or_local(%Connection{conn_type: "LOCAL"} = conn, command, timeout) do
    masked = mask_password(command, conn)
    _ = Core.log("info", "$ sh -lc #{shell_quote(masked)}", "ssh_execution", conn.name)

    {out, code} = System.cmd("sh", ["-lc", command], stderr_to_stdout: true, timeout: timeout)
    result = %{stdout: out, stderr: "", exit_code: code, command: masked}

    if code == 0, do: {:ok, result}, else: {:error, result}
  end

  defp run_over_ssh_or_local(%Connection{} = conn, command, timeout) do
    target = build_ssh_target(conn)
    args = ssh_args(conn) ++ [target, remote_exec(conn, command)]
    masked_cmd = mask_password(command, conn)
    masked_args = (ssh_args(conn) ++ [target, remote_exec(conn, masked_cmd)]) |> Enum.join(" ")

    _ = Core.log("info", "$ ssh #{masked_args}", "ssh_execution", conn.name)

    {out, code} = System.cmd("ssh", args, stderr_to_stdout: true, timeout: timeout)
    result = %{stdout: out, stderr: "", exit_code: code, command: "ssh #{masked_args}"}

    if code == 0, do: {:ok, result}, else: {:error, result}
  end

  defp build_ssh_target(conn) do
    user = (conn.username || "") |> String.trim()
    host = (conn.host || "") |> String.trim()

    if user == "" do
      host
    else
      "#{user}@#{host}"
    end
  end

  defp ssh_args(conn) do
    base = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"]
    base = base ++ ["-p", Integer.to_string(conn.port || 22)]

    case conn.key_path do
      path when is_binary(path) and path != "" ->
        base ++ ["-i", path]

      _ ->
        base
    end
  end

  defp wrap_sudo(command, conn) do
    case connection_password(conn) do
      nil ->
        "sudo -n sh -lc #{shell_quote(command)}"

      password ->
        "printf '%s\\n' #{shell_quote(password)} | sudo -S -p '' sh -lc #{shell_quote(command)}"
    end
  end

  defp connection_password(%Connection{password_enc: nil}), do: nil

  defp connection_password(%Connection{password_enc: blob}) when is_binary(blob) do
    master = System.get_env("ZFSMGR_MASTER_PASSWORD", "change-me")

    try do
      Crypto.decrypt(blob, master)
    rescue
      _ -> nil
    end
  end

  defp base_os_cmd(%Connection{os_type: "Windows"}) do
    "powershell -NoProfile -NonInteractive -Command \"[System.Environment]::OSVersion.VersionString\""
  end

  defp base_os_cmd(_), do: "uname -a"

  defp zfs_cmd(inner) do
    "(command -v zfs >/dev/null 2>&1 && #{inner}) || ([ -x /usr/local/zfs/bin/zfs ] && #{String.replace(inner, "zfs ", "/usr/local/zfs/bin/zfs ")}) || ([ -x /sbin/zfs ] && #{String.replace(inner, "zfs ", "/sbin/zfs ")})"
  end

  defp zpool_cmd(inner) do
    "(command -v zpool >/dev/null 2>&1 && #{inner}) || ([ -x /usr/local/zfs/bin/zpool ] && #{String.replace(inner, "zpool ", "/usr/local/zfs/bin/zpool ")}) || ([ -x /sbin/zpool ] && #{String.replace(inner, "zpool ", "/sbin/zpool ")})"
  end

  defp remote_exec(%Connection{os_type: "Windows"}, command) do
    "powershell -NoProfile -NonInteractive -Command #{shell_quote(command)}"
  end

  defp remote_exec(_conn, command), do: "sh -lc #{shell_quote(command)}"

  defp build_options(opts) when map_size(opts) == 0, do: {:ok, ""}

  defp build_options(opts) when is_map(opts) do
    parts =
      opts
      |> Enum.reject(fn {_k, v} -> is_nil(v) or to_string(v) == "" end)
      |> Enum.map(fn {k, v} -> "-o #{shell_quote(to_string(k) <> "=" <> to_string(v))}" end)

    {:ok, Enum.join(parts, " ")}
  end

  defp build_create_cmd("snapshot", dataset, _opt_str, attrs) do
    recursive = Map.get(attrs, "recursive") || Map.get(attrs, :recursive) || false
    flag = if recursive, do: "-r ", else: ""
    {:ok, "zfs snapshot #{flag}#{shell_quote(dataset)}"}
  end

  defp build_create_cmd("volume", dataset, opt_str, attrs) do
    volsize = Map.get(attrs, "volsize") || Map.get(attrs, :volsize)

    if is_binary(volsize) and volsize != "" do
      {:ok,
       "zfs create -V #{shell_quote(volsize)} #{opt_str} #{shell_quote(dataset)}" |> String.trim()}
    else
      {:error, :volsize_required}
    end
  end

  defp build_create_cmd(_kind, dataset, opt_str, _attrs) do
    {:ok, "zfs create #{opt_str} #{shell_quote(dataset)}" |> String.trim()}
  end

  defp parse_lines(text) when is_binary(text) do
    text
    |> String.split("\n", trim: true)
  end

  defp trim_output(text) when is_binary(text) do
    String.trim(text)
  end

  defp mask_password(command, conn) do
    case connection_password(conn) do
      nil -> command
      "" -> command
      pwd -> String.replace(command, pwd, "*")
    end
  end

  defp shell_quote(value) when is_binary(value) do
    escaped = String.replace(value, "'", "'\\''")
    "'" <> escaped <> "'"
  end
end
