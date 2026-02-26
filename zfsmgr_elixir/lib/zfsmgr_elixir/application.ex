defmodule ZfsmgrElixir.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = [
      ZfsmgrElixir.Repo,
      {DynamicSupervisor, strategy: :one_for_one, name: ZfsmgrElixir.SessionSupervisor}
    ]

    opts = [strategy: :one_for_one, name: ZfsmgrElixir.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
