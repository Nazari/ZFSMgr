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

  def child_spec(connection_id) do
    %{
      id: {__MODULE__, connection_id},
      start: {__MODULE__, :start_link, [connection_id]},
      restart: :transient
    }
  end

  def start_link(connection_id) do
    GenServer.start_link(__MODULE__, connection_id, name: via(connection_id))
  end

  def refresh(connection_id_or_pid) do
    case connection_id_or_pid do
      pid when is_pid(pid) ->
        GenServer.call(pid, :refresh, 120_000)

      connection_id ->
        GenServer.call(via(connection_id), :refresh, 120_000)
    end
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

  defp via(connection_id) do
    {:via, Registry, {ZfsmgrElixir.SessionRegistry, connection_id}}
  end
end
