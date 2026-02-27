defmodule ZfsmgrElixir.Repo do
  use Ecto.Repo,
    otp_app: :zfsmgr_elixir,
    adapter: Ecto.Adapters.SQLite3
end
