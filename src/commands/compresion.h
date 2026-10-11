#pragma once

#include <cstddef>
#include <memory>
#include <string>

// Compression of `send` stream files: gzip, lz4 and zstd.
//
// The stream is compressed where the FILE is, never where ZFS is: the agent that writes the
// file compresses before writing, and the agent that reads it decompresses before relaying.
// The ZFS end keeps receiving or sending a raw `zfs send` stream and does not know a file is
// involved, let alone a compressed one.
//
// Reading does not ask which format: it is recognised by the first bytes, never by the
// extension, which can lie. Writing is chosen with `send … --comp <format>[:<level>]`.
//
// The three libraries are built from `third_party/`, statically, so the agent keeps having no
// runtime dependencies on any of the five platforms.
//
// Two halves, in two libraries: `leeEleccion`, `textoDe`, `nombreDe` and `detecta` need no
// library and live in `compresionformato.cpp` (in `zfsmgr_commands`, so the CLI can validate
// `--comp`); the compressors live in `compresion.cpp` (in `zfsmgr_compresion`, which only the
// agent and the tests link).
namespace zfsmgr::compresion {

enum class Formato { Ninguno, Gzip, Lz4, Zstd };

// What `--comp` asked for. `nivel` is already resolved: the format's default when none was
// given.
struct Eleccion {
    Formato formato{Formato::Ninguno};
    int nivel{0};
};

// «gzip», «lz4», «zstd», optionally «:<level>», and «none». Upper or lower case. False, with
// the reason in `error`, for an unknown format or a level out of range:
//   gzip 1–9 (default 6), lz4 0–12 (default 0, the fast one; 3 or more is HC),
//   zstd 1–19 (default 3).
bool leeEleccion(const std::string& texto, Eleccion& out, std::string& error);

// The canonical text of a choice, the inverse of `leeEleccion`: «zstd:3». «none» for no
// compression.
std::string textoDe(const Eleccion& e);

const char* nombreDe(Formato f);

// The format of a stream by its first bytes. With fewer than 4 it can only say `Ninguno`.
//   gzip  1f 8b
//   lz4   04 22 4d 18   (frame format; the legacy one is not supported)
//   zstd  28 b5 2f fd
// Anything else is taken as a raw stream: a `zfs send` stream starts with a DRR_BEGIN record,
// whose first bytes are none of these.
Formato detecta(const unsigned char* datos, std::size_t n);

// Compresses a stream in pieces. `comprime` appends to `salida` what it can give so far;
// `termina` appends the rest and closes the frame. After `termina`, it is not used again.
class Compresor {
public:
    virtual ~Compresor() = default;
    virtual bool comprime(const char* datos, std::size_t n, std::string& salida,
                          std::string& error) = 0;
    virtual bool termina(std::string& salida, std::string& error) = 0;
};

// Decompresses a stream in pieces. Concatenated frames or members are accepted, as the
// command-line tools do.
//
// `completo()` says whether what arrived so far ends exactly at the end of a frame. A file
// that was cut leaves it false, and THAT is what tells a truncated compressed stream from a
// whole one: the decompressor usually notices before `zfs recv` does, and with a clearer
// reason.
class Descompresor {
public:
    virtual ~Descompresor() = default;
    virtual bool descomprime(const char* datos, std::size_t n, std::string& salida,
                             std::string& error) = 0;
    virtual bool completo() const = 0;
};

// Null, with the reason in `error`, if it could not be created. `Ninguno` gives null too: there
// is nothing to do.
std::unique_ptr<Compresor> creaCompresor(const Eleccion& e, std::string& error);
std::unique_ptr<Descompresor> creaDescompresor(Formato f, std::string& error);

}  // namespace zfsmgr::compresion
