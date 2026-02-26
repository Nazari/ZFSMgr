import Config

config :zfsmgr_elixir,
  ecto_repos: [ZfsmgrElixir.Repo]

config :zfsmgr_elixir, ZfsmgrElixir.Repo,
  database: Path.expand("../zfsmgr_elixir_dev.db", __DIR__),
  pool_size: 10,
  stacktrace: true,
  show_sensitive_data_on_connection_error: true

if config_env() == :test do
  config :zfsmgr_elixir, ZfsmgrElixir.Repo,
    database: Path.expand("../zfsmgr_elixir_test.db", __DIR__),
    pool: Ecto.Adapters.SQL.Sandbox,
    pool_size: 5
end
