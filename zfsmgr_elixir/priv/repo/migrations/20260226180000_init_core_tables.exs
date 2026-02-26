defmodule ZfsmgrElixir.Repo.Migrations.InitCoreTables do
  use Ecto.Migration

  def change do
    create table(:connections) do
      add :name, :string, null: false
      add :conn_type, :string, null: false
      add :os_type, :string, null: false, default: "Linux"
      add :transport, :string, null: false, default: "SSH"
      add :host, :string
      add :port, :integer, null: false, default: 22
      add :username, :string
      add :password_enc, :binary
      add :key_path, :string
      add :use_ssl, :boolean, null: false, default: false
      add :auth, :string, null: false, default: "ntlm"
      add :use_sudo, :boolean, null: false, default: true
      add :is_active, :boolean, null: false, default: true

      timestamps(type: :utc_datetime)
    end

    create unique_index(:connections, [:name])

    create table(:action_logs) do
      add :connection_name, :string
      add :level, :string, null: false
      add :message, :text, null: false
      add :source, :string, null: false, default: "application"

      timestamps(type: :utc_datetime, updated_at: false)
    end

    create index(:action_logs, [:inserted_at])
    create index(:action_logs, [:source])
    create index(:action_logs, [:connection_name])
  end
end
