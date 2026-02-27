defmodule ZfsmgrElixir.Core.Connection do
  use Ecto.Schema
  import Ecto.Changeset

  @conn_types ~w(LOCAL SSH PSRP)
  @os_types ~w(Linux MacOS Windows)
  @transport_types ~w(SSH PSRP)

  schema "connections" do
    field(:name, :string)
    field(:conn_type, :string)
    field(:os_type, :string, default: "Linux")
    field(:transport, :string, default: "SSH")
    field(:host, :string)
    field(:port, :integer, default: 22)
    field(:username, :string)
    field(:password_enc, :binary)
    field(:key_path, :string)
    field(:use_ssl, :boolean, default: false)
    field(:auth, :string, default: "ntlm")
    field(:use_sudo, :boolean, default: true)
    field(:is_active, :boolean, default: true)

    timestamps(type: :utc_datetime)
  end

  def changeset(connection, attrs) do
    connection
    |> cast(attrs, [
      :name,
      :conn_type,
      :os_type,
      :transport,
      :host,
      :port,
      :username,
      :password_enc,
      :key_path,
      :use_ssl,
      :auth,
      :use_sudo,
      :is_active
    ])
    |> validate_required([:name, :conn_type])
    |> validate_number(:port, greater_than: 0, less_than: 65_536)
    |> validate_inclusion(:conn_type, @conn_types)
    |> validate_inclusion(:os_type, @os_types)
    |> validate_inclusion(:transport, @transport_types)
    |> unique_constraint(:name)
  end
end
