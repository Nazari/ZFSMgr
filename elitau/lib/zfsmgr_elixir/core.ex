defmodule ZfsmgrElixir.Core do
  @moduledoc "Core persistence layer for connections and logs."

  import Ecto.Query, warn: false
  alias ZfsmgrElixir.{Repo, Crypto}
  alias ZfsmgrElixir.Core.{Connection, ActionLog}

  def list_connections do
    Repo.all(from(c in Connection, order_by: [asc: c.name]))
  end

  def count_connections do
    Repo.aggregate(Connection, :count, :id)
  end

  def get_connection!(id), do: Repo.get!(Connection, id)

  def create_connection(attrs) do
    %Connection{}
    |> Connection.changeset(attrs)
    |> Repo.insert()
  end

  def update_connection(%Connection{} = conn, attrs) do
    conn
    |> Connection.changeset(attrs)
    |> Repo.update()
  end

  def delete_connection(%Connection{} = conn), do: Repo.delete(conn)

  def change_connection(%Connection{} = conn, attrs \\ %{}) do
    Connection.changeset(conn, attrs)
  end

  def log(level, message, source \\ "application", connection_name \\ nil) do
    %ActionLog{}
    |> ActionLog.changeset(%{
      level: level,
      message: message,
      source: source,
      connection_name: connection_name
    })
    |> Repo.insert()
  end

  def list_logs(limit \\ 500) do
    Repo.all(
      from(l in ActionLog,
        order_by: [desc: l.inserted_at],
        limit: ^limit
      )
    )
    |> Enum.reverse()
  end

  def bootstrap_connections_from_legacy_ini do
    if count_connections() > 0 do
      {:skip, :already_seeded}
    else
      ini_path =
        System.get_env("ZFSMGR_LEGACY_INI") ||
          Path.expand("~/.config/ZFSMgr/connections.ini")

      if File.exists?(ini_path) do
        import_legacy_ini(ini_path)
      else
        {:skip, :legacy_ini_not_found}
      end
    end
  end

  defp import_legacy_ini(path) do
    with {:ok, text} <- File.read(path) do
      sections = parse_legacy_ini_sections(text)

      {inserted, skipped_pw} =
        sections
        |> Enum.map(&legacy_section_to_attrs/1)
        |> Enum.reduce({0, 0}, fn
          {:ok, {attrs, skipped_pw?}}, {ok_count, skip_count} ->
            case create_connection(attrs) do
              {:ok, _} -> {ok_count + 1, skip_count + if(skipped_pw?, do: 1, else: 0)}
              {:error, _} -> {ok_count, skip_count}
            end

          _err, acc ->
            acc
        end)

      _ =
        log(
          "normal",
          "Legacy import from connections.ini: inserted=#{inserted} skipped_password=#{skipped_pw}"
        )

      {:ok, %{inserted: inserted, skipped_password: skipped_pw}}
    end
  end

  defp parse_legacy_ini_sections(text) do
    lines = String.split(text, ~r/\r?\n/, trim: false)

    {sections, current_name, current_map} =
      Enum.reduce(lines, {[], nil, %{}}, fn line, {acc, sec_name, sec_map} ->
        trimmed = String.trim(line)

        cond do
          trimmed == "" or String.starts_with?(trimmed, ";") or String.starts_with?(trimmed, "#") ->
            {acc, sec_name, sec_map}

          String.starts_with?(trimmed, "[") and String.ends_with?(trimmed, "]") ->
            next_name =
              trimmed
              |> String.trim_leading("[")
              |> String.trim_trailing("]")
              |> String.trim()

            acc2 =
              if is_binary(sec_name) and String.starts_with?(sec_name, "connection:") do
                [{sec_name, sec_map} | acc]
              else
                acc
              end

            {acc2, next_name, %{}}

          String.contains?(trimmed, "=") ->
            [k, v] = String.split(trimmed, "=", parts: 2)
            {acc, sec_name, Map.put(sec_map, String.trim(k), String.trim(v))}

          true ->
            {acc, sec_name, sec_map}
        end
      end)

    sections2 =
      if is_binary(current_name) and String.starts_with?(current_name, "connection:") do
        [{current_name, current_map} | sections]
      else
        sections
      end

    Enum.reverse(sections2)
  end

  defp legacy_section_to_attrs({_section_name, raw}) do
    conn_type = (raw["conn_type"] || "SSH") |> String.upcase()
    os_type = raw["os_type"] || "Linux"
    transport = (raw["transport"] || conn_type) |> String.upcase()
    name = (raw["name"] || "") |> String.trim()
    host = (raw["host"] || "") |> String.trim()

    if name == "" do
      {:error, :name_required}
    else
      master = System.get_env("ZFSMGR_MASTER_PASSWORD", "change-me")
      raw_password = (raw["password"] || "") |> String.trim()

      {password_enc, skipped_pw?} =
        cond do
          raw_password == "" ->
            {nil, false}

          String.starts_with?(raw_password, "gAAAA") ->
            {nil, true}

          true ->
            {Crypto.encrypt(raw_password, master), false}
        end

      attrs = %{
        "name" => name,
        "conn_type" => if(conn_type in ["LOCAL", "SSH", "PSRP"], do: conn_type, else: "SSH"),
        "os_type" => if(os_type in ["Linux", "MacOS", "Windows"], do: os_type, else: "Linux"),
        "transport" => if(transport in ["SSH", "PSRP"], do: transport, else: "SSH"),
        "host" => host,
        "port" => parse_int(raw["port"], 22),
        "username" => raw["username"] || "",
        "password_enc" => password_enc,
        "key_path" => raw["key_path"] || "",
        "use_ssl" => parse_bool(raw["use_ssl"], false),
        "auth" => raw["auth"] || "ntlm",
        "use_sudo" => parse_bool(raw["use_sudo"], true),
        "is_active" => true
      }

      {:ok, {attrs, skipped_pw?}}
    end
  end

  defp parse_bool(nil, default), do: default

  defp parse_bool(v, _default) when is_boolean(v), do: v

  defp parse_bool(v, default) do
    case String.downcase(String.trim(to_string(v))) do
      "1" -> true
      "true" -> true
      "yes" -> true
      "on" -> true
      "0" -> false
      "false" -> false
      "no" -> false
      "off" -> false
      _ -> default
    end
  end

  defp parse_int(nil, default), do: default
  defp parse_int(v, _default) when is_integer(v), do: v

  defp parse_int(v, default) do
    case Integer.parse(String.trim(to_string(v))) do
      {n, _} when n > 0 -> n
      _ -> default
    end
  end
end
