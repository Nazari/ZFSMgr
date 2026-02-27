defmodule ZfsmgrElixir.Core.ActionLog do
  use Ecto.Schema
  import Ecto.Changeset

  schema "action_logs" do
    field(:connection_name, :string)
    field(:level, :string)
    field(:message, :string)
    field(:source, :string, default: "application")

    timestamps(type: :utc_datetime, updated_at: false)
  end

  def changeset(log, attrs) do
    log
    |> cast(attrs, [:connection_name, :level, :message, :source])
    |> validate_required([:level, :message, :source])
  end
end
