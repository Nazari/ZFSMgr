defmodule ZfsmgrElixir.Application do
  @moduledoc false

  use Application
  alias ZfsmgrElixir.Core

  @impl true
  def start(_type, _args) do
    children = [
      ZfsmgrElixir.Repo,
      {Registry, keys: :unique, name: ZfsmgrElixir.SessionRegistry},
      {DynamicSupervisor, strategy: :one_for_one, name: ZfsmgrElixir.SessionSupervisor},
      {ZfsmgrElixirWeb.Endpoint, []}
    ]

    opts = [strategy: :one_for_one, name: ZfsmgrElixir.Supervisor]

    with {:ok, pid} <- Supervisor.start_link(children, opts) do
      Task.start(fn -> Core.bootstrap_connections_from_legacy_ini() end)
      {:ok, pid}
    end
  end
end
