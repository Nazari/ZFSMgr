defmodule ZfsmgrElixirWeb.Endpoint do
  @moduledoc false

  use Plug.Router

  alias ZfsmgrElixirWeb.Json
  alias ZfsmgrElixirWeb.RouterHelpers, as: RH

  plug(Plug.Logger)

  plug(Plug.Parsers,
    parsers: [:json],
    pass: ["application/json"],
    json_decoder: Jason
  )

  plug(:match)
  plug(:dispatch)

  def child_spec(_opts) do
    cfg = Application.fetch_env!(:zfsmgr_elixir, __MODULE__)
    http_opts = Keyword.get(cfg, :http, ip: {127, 0, 0, 1}, port: 4001)

    Plug.Cowboy.child_spec(
      scheme: :http,
      plug: __MODULE__,
      options: http_opts
    )
  end

  get "/api/health" do
    Json.send_json(conn, 200, %{ok: true, service: "zfsmgr_elixir", ts: DateTime.utc_now()})
  end

  get "/api/connections" do
    Json.send_json(conn, 200, RH.list_connections_payload())
  end

  post "/api/connections" do
    case RH.create_connection(conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 201, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  put "/api/connections/:id" do
    case RH.update_connection(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  delete "/api/connections/:id" do
    case RH.delete_connection(id) do
      :ok -> Json.send_json(conn, 204, %{})
      {:error, payload} -> Json.send_json(conn, 404, payload)
    end
  end

  post "/api/connections/:id/refresh" do
    case RH.refresh_connection(id) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 404, payload)
    end
  end

  post "/api/connections/:id/actions/import_pool" do
    case RH.import_pool(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/export_pool" do
    case RH.export_pool(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/create_dataset" do
    case RH.create_dataset(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/delete_dataset" do
    case RH.delete_dataset(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/rename_dataset" do
    case RH.rename_dataset(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/set_property" do
    case RH.set_dataset_property(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  post "/api/connections/:id/actions/inherit_property" do
    case RH.inherit_dataset_property(id, conn.body_params) do
      {:ok, payload} -> Json.send_json(conn, 200, payload)
      {:error, payload} -> Json.send_json(conn, 422, payload)
    end
  end

  get "/api/logs" do
    limit = parse_limit(conn.params["limit"])

    Json.send_json(conn, 200, RH.list_logs_payload(limit))
  end

  match _ do
    Json.send_json(conn, 404, %{error: "not_found"})
  end

  defp parse_limit(nil), do: 500

  defp parse_limit(raw) when is_binary(raw) do
    case Integer.parse(raw) do
      {n, _} when n > 0 and n <= 10_000 -> n
      _ -> 500
    end
  end

  defp parse_limit(_), do: 500
end
