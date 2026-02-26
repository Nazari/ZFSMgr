defmodule ZfsmgrElixirWeb.RouterHelpers do
  @moduledoc false

  alias ZfsmgrElixir.{Core, Crypto, Session}
  alias ZfsmgrElixir.Core.Connection
  alias ZfsmgrElixir.Session.ConnectionSession

  def list_connections_payload do
    %{connections: Enum.map(Core.list_connections(), &serialize_connection/1)}
  end

  def list_logs_payload(limit) do
    %{logs: Core.list_logs(limit)}
  end

  def create_connection(attrs) do
    with {:ok, attrs2} <- normalize_connection_attrs(attrs),
         {:ok, conn} <- Core.create_connection(attrs2) do
      _ = Core.log("normal", "Connection created: #{conn.name}")
      {:ok, %{connection: serialize_connection(conn)}}
    else
      {:error, %Ecto.Changeset{} = cs} -> {:error, %{errors: changeset_errors(cs)}}
      {:error, reason} -> {:error, %{error: inspect(reason)}}
    end
  end

  def update_connection(id, attrs) do
    with %Connection{} = conn <- get_connection(id),
         {:ok, attrs2} <- normalize_connection_attrs(attrs),
         {:ok, conn2} <- Core.update_connection(conn, attrs2) do
      _ = Core.log("normal", "Connection updated: #{conn2.name}")
      {:ok, %{connection: serialize_connection(conn2)}}
    else
      nil -> {:error, %{error: "connection_not_found"}}
      {:error, %Ecto.Changeset{} = cs} -> {:error, %{errors: changeset_errors(cs)}}
      {:error, reason} -> {:error, %{error: inspect(reason)}}
    end
  end

  def delete_connection(id) do
    case get_connection(id) do
      %Connection{} = conn ->
        case Core.delete_connection(conn) do
          {:ok, _} ->
            _ = Core.log("normal", "Connection deleted: #{conn.name}")
            :ok

          {:error, _} = err ->
            {:error, %{error: inspect(err)}}
        end

      nil ->
        {:error, %{error: "connection_not_found"}}
    end
  end

  def refresh_connection(id) do
    case get_connection(id) do
      %Connection{} = conn ->
        _ = Core.log("normal", "Refresh started: #{conn.name}")

        with {:ok, _pid} <- Session.ensure_connection_session(conn.id),
             {:ok, result} <- ConnectionSession.refresh(conn.id) do
          _ = Core.log("normal", "Refresh finished: #{conn.name}")
          {:ok, %{result: result, connection: serialize_connection(conn)}}
        else
          other ->
            _ = Core.log("normal", "Refresh failed: #{conn.name} -> #{inspect(other)}")
            {:error, %{error: inspect(other)}}
        end

      nil ->
        {:error, %{error: "connection_not_found"}}
    end
  end

  defp get_connection(id) do
    case Integer.parse(to_string(id)) do
      {int_id, _} ->
        Core.get_connection!(int_id)

      :error ->
        nil
    end
  rescue
    Ecto.NoResultsError -> nil
  end

  defp serialize_connection(conn) do
    %{
      id: conn.id,
      name: conn.name,
      conn_type: conn.conn_type,
      os_type: conn.os_type,
      transport: conn.transport,
      host: conn.host,
      port: conn.port,
      username: conn.username,
      key_path: conn.key_path,
      use_ssl: conn.use_ssl,
      auth: conn.auth,
      use_sudo: conn.use_sudo,
      is_active: conn.is_active,
      has_password: is_binary(conn.password_enc) and byte_size(conn.password_enc) > 0,
      inserted_at: conn.inserted_at,
      updated_at: conn.updated_at
    }
  end

  defp normalize_connection_attrs(attrs) when is_map(attrs) do
    attrs = for {k, v} <- attrs, into: %{}, do: {to_string(k), v}
    master = System.get_env("ZFSMGR_MASTER_PASSWORD", "change-me")

    attrs =
      case Map.get(attrs, "password") do
        nil ->
          attrs

        "" ->
          Map.put(attrs, "password_enc", nil)

        pwd when is_binary(pwd) ->
          Map.put(attrs, "password_enc", Crypto.encrypt(pwd, master))
      end

    attrs =
      attrs
      |> Map.delete("password")
      |> normalize_bool("use_ssl")
      |> normalize_bool("use_sudo")
      |> normalize_bool("is_active")
      |> normalize_int("port")

    {:ok, attrs}
  end

  defp normalize_bool(attrs, key) do
    case Map.get(attrs, key) do
      nil -> attrs
      v when is_boolean(v) -> attrs
      v when is_binary(v) -> Map.put(attrs, key, String.downcase(v) in ["1", "true", "yes", "on"])
      _ -> attrs
    end
  end

  defp normalize_int(attrs, key) do
    case Map.get(attrs, key) do
      nil ->
        attrs

      v when is_integer(v) ->
        attrs

      v when is_binary(v) ->
        case Integer.parse(v) do
          {n, _} -> Map.put(attrs, key, n)
          :error -> attrs
        end

      _ ->
        attrs
    end
  end

  defp changeset_errors(cs) do
    Ecto.Changeset.traverse_errors(cs, fn {msg, opts} ->
      Regex.replace(~r"%{(\w+)}", msg, fn _, key ->
        opts |> Keyword.get(String.to_atom(key), key) |> to_string()
      end)
    end)
  end
end
