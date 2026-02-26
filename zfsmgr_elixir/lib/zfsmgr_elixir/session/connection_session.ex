defmodule ZfsmgrElixir.Session.ConnectionSession do
  @moduledoc """
  Stateful process per remote connection.

  This module is the base to migrate SSH/PSRP command execution from Python
  to native Elixir workers while keeping one long-lived session per connection.
  """

  use GenServer

  defstruct [
    :connection_id,
    :status,
    :last_refresh_at
  ]

  def start_link(connection_id) do
    GenServer.start_link(__MODULE__, connection_id)
  end

  def refresh(pid) do
    GenServer.call(pid, :refresh, 120_000)
  end

  @impl true
  def init(connection_id) do
    state = %__MODULE__{
      connection_id: connection_id,
      status: :idle,
      last_refresh_at: nil
    }

    {:ok, state}
  end

  @impl true
  def handle_call(:refresh, _from, state) do
    # Placeholder for next phase: run pooled SSH/PSRP refresh here.
    now = DateTime.utc_now()

    {:reply, {:ok, %{connection_id: state.connection_id, refreshed_at: now}},
     %{state | status: :ok, last_refresh_at: now}}
  end
end
