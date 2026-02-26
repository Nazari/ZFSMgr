defmodule ZfsmgrElixir.Session.ConnectionSession do
  @moduledoc """
  Stateful process per remote connection.

  This module is the base to migrate SSH/PSRP command execution from Python
  to native Elixir workers while keeping one long-lived session per connection.
  """

  use GenServer
  alias ZfsmgrElixir.{Core, Zfs}

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
    now = DateTime.utc_now()

    result =
      with conn <- Core.get_connection!(state.connection_id),
           {:ok, payload} <- Zfs.refresh_connection(conn) do
        {:ok, Map.put(payload, :connection_id, state.connection_id)}
      end

    new_status =
      case result do
        {:ok, _} -> :ok
        _ -> :error
      end

    {:reply, result, %{state | status: new_status, last_refresh_at: now}}
  rescue
    exc ->
      now = DateTime.utc_now()

      {:reply, {:error, %{error: Exception.message(exc)}},
       %{state | status: :error, last_refresh_at: now}}
  end

  defp via(connection_id) do
    {:via, Registry, {ZfsmgrElixir.SessionRegistry, connection_id}}
  end
end
