# Third-party code

Compression of `send` stream files (`send … --comp gzip|lz4|zstd`). Only the agent links it;
the CLI only validates the option.

The libraries are built from source with the project's own toolchains (`resources/CMakeLists.txt`,
target `zfsmgr_compresion`), statically. No sysroot used by the cross-builds had all three,
and the agent must keep having no runtime dependencies on any of the five platforms.

| Library | Version | Source tarball | SHA-256 of the tarball | License |
|---|---|---|---|---|
| zlib | 1.3.1 | https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` | zlib (`zlib/LICENSE`) |
| lz4 | 1.10.0 | https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz | `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b` | BSD-2-Clause (`lz4/LICENSE`) |
| zstd | 1.5.6 | https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz | `8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1` | BSD-3-Clause (`zstd/LICENSE`) |

The lz4 and zstd checksums were checked against the `.sha256` files published with each
release.

## What was copied

Only the library sources and their license, unmodified. No tests, programs, build files or
documentation.

- **zlib**: the `.c`/`.h` of the library root (deflate, inflate, crc32, adler32, trees, zutil
  and their headers). The `gz*` file API is not built.
- **lz4**: `lib/` — `lz4`, `lz4frame`, `lz4hc`, `xxhash`.
- **zstd**: `lib/common`, `lib/compress`, `lib/decompress` and the public headers. Not the
  legacy formats, the dictionary builder or the x86-64 assembly decoder
  (`huf_decompress_amd64.S`); it is built with `ZSTD_DISABLE_ASM`.

lz4 and zstd each bring their own `xxhash`; the lz4 sources are built with
`XXH_NAMESPACE=LZ4_` so the symbols do not clash.

## Updating

Download the new release, check its checksum, replace the same files and update this table.
`tests/base_test.cpp` round-trips the three formats, including cut and damaged streams.


# FIDO2 security keys

Opening ZFSMgr with a YubiKey (`master-key`, src/cli/fidokey.cpp). Only the CLI links them,
never the agent: on Linux libfido2 needs libudev, and the agent must keep having no
dependencies. Built from source inside `resources/CMakeLists.txt` (target `zfsmgr_fido`), with
the defines libfido2's own CMake found on each platform.

| Library | Version | Source tarball | SHA-256 of the tarball | License |
|---|---|---|---|---|
| libfido2 | 1.16.0 | https://developers.yubico.com/libfido2/Releases/libfido2-1.16.0.tar.gz | `8c2b6fb279b5b42e9ac92ade71832e485852647b53607c43baaafbbcecea04e4` | BSD-2-Clause (`libfido2/LICENSE`) |
| libcbor | 0.12.0 | https://github.com/PJK/libcbor/archive/refs/tags/v0.12.0.tar.gz | `5368add109db559f546d7ed10f440f39a273b073daa8da4abffc83815069fa7f` | MIT (`libcbor/LICENSE.md`) |

The libfido2 tarball's signature (`.sig` next to it) was verified: a good signature by Ludvig
Michaelsson, key `B37003FF15A068A413E50B1C3D6A49E4C4E0B673`, a libfido2 maintainer at Yubico.

## What was copied

- **libfido2**: `src/` — the library sources its CMake builds on every platform, the four HID
  backends (`hid_linux`, `hid_unix`, `hid_osx`, `hid_freebsd`, `hid_win`) and `winhello.c`;
  its headers and `src/fido/`; and the ten `openbsd-compat/` files it builds, with their
  headers. Not NFC, PC/SC, the tools, the examples or the tests. `openbsd-compat/` must NOT be
  on the include path: its `time.h` includes itself through the system one.
- **libcbor**: `src/` — the library sources and headers. `cbor/configuration.h` is the one
  its CMake generates (the same on the five targets); `cbor/cbor_export.h` is written by hand
  for a static library.

zlib comes from `zlib/` above (target `zfsmgr_zlib`, shared with the agent's compression).
