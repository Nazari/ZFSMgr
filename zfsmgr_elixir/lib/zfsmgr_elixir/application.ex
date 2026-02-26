defmodule ZfsmgrElixir.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = [
      ZfsmgrElixir.Repo,
      {Registry, keys: :unique, name: ZfsmgrElixir.SessionRegistry},
      {DynamicSupervisor, strategy: :one_for_one, name: ZfsmgrElixir.SessionSupervisor},
      {ZfsmgrElixirWeb.Endpoint, []}
    ]

    opts = [strategy: :one_for_one, name: ZfsmgrElixir.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
