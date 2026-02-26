defmodule ZfsmgrElixir.Core do
  @moduledoc "Core persistence layer for connections and logs."

  import Ecto.Query, warn: false
  alias ZfsmgrElixir.Repo
  alias ZfsmgrElixir.Core.{Connection, ActionLog}

  def list_connections do
    Repo.all(from(c in Connection, order_by: [asc: c.name]))
  end

  def get_connection!(id), do: Repo.get!(Connection, id)

  def create_connection(attrs) do
    %Connection{}
    |> Connection.changeset(attrs)
    |> Repo.insert()
  end

  def update_connection(%Connection{} = conn, attrs) do
    conn
    |> Connection.changeset(attrs)
    |> Repo.update()
  end

  def delete_connection(%Connection{} = conn), do: Repo.delete(conn)

  def change_connection(%Connection{} = conn, attrs \\ %{}) do
    Connection.changeset(conn, attrs)
  end

  def log(level, message, source \\ "application", connection_name \\ nil) do
    %ActionLog{}
    |> ActionLog.changeset(%{
      level: level,
      message: message,
      source: source,
      connection_name: connection_name
    })
    |> Repo.insert()
  end

  def list_logs(limit \\ 500) do
    Repo.all(
      from(l in ActionLog,
        order_by: [desc: l.inserted_at],
        limit: ^limit
      )
    )
    |> Enum.reverse()
  end
end
