defmodule ZfsmgrElixir.Crypto do
  @moduledoc """
  Minimal symmetric encryption helper for secrets (passwords/API keys).
  """

  @algo :aes_256_gcm
  @aad "zfsmgr:connections:v1"

  def encrypt(plaintext, master_password)
      when is_binary(plaintext) and is_binary(master_password) do
    key = derive_key(master_password)
    iv = :crypto.strong_rand_bytes(12)
    {cipher, tag} = :crypto.crypto_one_time_aead(@algo, key, iv, plaintext, @aad, true)
    :erlang.term_to_binary({iv, cipher, tag})
  end

  def decrypt(cipher_blob, master_password)
      when is_binary(cipher_blob) and is_binary(master_password) do
    key = derive_key(master_password)
    {iv, cipher, tag} = :erlang.binary_to_term(cipher_blob)
    :crypto.crypto_one_time_aead(@algo, key, iv, cipher, @aad, tag, false)
  end

  defp derive_key(master_password) do
    :crypto.hash(:sha256, master_password)
  end
end
