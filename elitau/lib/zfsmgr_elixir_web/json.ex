defmodule ZfsmgrElixirWeb.Json do
  @moduledoc false

  import Plug.Conn

  def send_json(conn, status, payload) do
    body = Jason.encode!(normalize(payload))

    conn
    |> put_resp_content_type("application/json")
    |> send_resp(status, body)
  end

  defp normalize(%DateTime{} = dt), do: DateTime.to_iso8601(dt)
  defp normalize(%NaiveDateTime{} = ndt), do: NaiveDateTime.to_iso8601(ndt)
  defp normalize(%{} = map), do: Map.new(map, fn {k, v} -> {k, normalize(v)} end)
  defp normalize(list) when is_list(list), do: Enum.map(list, &normalize/1)
  defp normalize(other), do: other
end
