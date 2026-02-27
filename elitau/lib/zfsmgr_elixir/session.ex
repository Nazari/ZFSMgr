defmodule ZfsmgrElixir.Session do
  @moduledoc "Session orchestration for per-connection workers."

  alias ZfsmgrElixir.Session.ConnectionSession

  def ensure_connection_session(connection_id) do
    case Registry.lookup(ZfsmgrElixir.SessionRegistry, connection_id) do
      [{pid, _value}] ->
        {:ok, pid}

      [] ->
        case DynamicSupervisor.start_child(
               ZfsmgrElixir.SessionSupervisor,
               {ConnectionSession, connection_id}
             ) do
          {:ok, pid} -> {:ok, pid}
          {:error, {:already_started, pid}} -> {:ok, pid}
          other -> other
        end
    end
  end
end
