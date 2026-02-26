defmodule ZfsmgrElixir.Session do
  @moduledoc "Session orchestration for per-connection workers."

  alias ZfsmgrElixir.Session.ConnectionSession

  def ensure_connection_session(connection_id) do
    spec = {ConnectionSession, connection_id}

    case DynamicSupervisor.start_child(ZfsmgrElixir.SessionSupervisor, spec) do
      {:ok, pid} ->
        {:ok, pid}

      {:error, {:already_started, pid}} ->
        {:ok, pid}

      other ->
        other
    end
  end
end
