#include "compresion.h"

#include <algorithm>
#include <vector>

#include <zlib.h>
#include <lz4frame.h>
#include <zstd.h>

namespace zfsmgr::compresion {
namespace {

// El trozo de salida con el que se trabaja. Las tres bibliotecas escriben en un búfer que se
// les da y dicen cuánto han llenado; con uno fijo se vuelca a `salida` y se repite.
constexpr std::size_t kTrozo = 256 * 1024;

// ── gzip (zlib) ─────────────────────────────────────────────────────────────────────

// `windowBits` 15 + 16 es el formato gzip, con su cabecera y su CRC, y no el zlib crudo: lo
// que `gzip -d` y `zcat` entienden.
class CompresorGzip : public Compresor {
public:
    bool inicia(int nivel, std::string& error) {
        if (deflateInit2(&z_, nivel, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            error = "could not start gzip";
            return false;
        }
        vivo_ = true;
        return true;
    }
    ~CompresorGzip() override {
        if (vivo_) {
            deflateEnd(&z_);
        }
    }
    bool comprime(const char* datos, std::size_t n, std::string& salida,
                  std::string& error) override {
        return pasa(datos, n, Z_NO_FLUSH, salida, error);
    }
    bool termina(std::string& salida, std::string& error) override {
        return pasa(nullptr, 0, Z_FINISH, salida, error);
    }

private:
    bool pasa(const char* datos, std::size_t n, int modo, std::string& salida,
              std::string& error) {
        // `avail_in` es de 32 bits: un trozo mayor se da en varias veces.
        std::size_t hecho = 0;
        do {
            const std::size_t ahora = std::min<std::size_t>(n - hecho, 1u << 30);
            z_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(datos ? datos + hecho : nullptr));
            z_.avail_in = static_cast<uInt>(ahora);
            hecho += ahora;
            const int modoAhora = hecho < n ? Z_NO_FLUSH : modo;
            int rc = Z_OK;
            do {
                z_.next_out = reinterpret_cast<Bytef*>(buf_);
                z_.avail_out = sizeof(buf_);
                rc = deflate(&z_, modoAhora);
                if (rc == Z_STREAM_ERROR) {
                    error = "gzip compression failed";
                    return false;
                }
                salida.append(buf_, sizeof(buf_) - z_.avail_out);
            } while (z_.avail_out == 0 || (modoAhora == Z_FINISH && rc != Z_STREAM_END));
        } while (hecho < n);
        return true;
    }

    z_stream z_{};
    bool vivo_{false};
    char buf_[kTrozo];
};

class DescompresorGzip : public Descompresor {
public:
    bool inicia(std::string& error) {
        if (inflateInit2(&z_, 15 + 16) != Z_OK) {
            error = "could not start gzip";
            return false;
        }
        vivo_ = true;
        return true;
    }
    ~DescompresorGzip() override {
        if (vivo_) {
            inflateEnd(&z_);
        }
    }
    bool descomprime(const char* datos, std::size_t n, std::string& salida,
                     std::string& error) override {
        std::size_t hecho = 0;
        while (hecho < n) {
            const std::size_t ahora = std::min<std::size_t>(n - hecho, 1u << 30);
            z_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(datos + hecho));
            z_.avail_in = static_cast<uInt>(ahora);
            while (z_.avail_in > 0) {
                // Un miembro que acaba con datos detrás: es otro miembro, como los que deja
                // `cat a.gz b.gz`. Se reinicia y se sigue.
                if (fin_) {
                    if (inflateReset(&z_) != Z_OK) {
                        error = "gzip: could not start the next member";
                        return false;
                    }
                    fin_ = false;
                }
                z_.next_out = reinterpret_cast<Bytef*>(buf_);
                z_.avail_out = sizeof(buf_);
                const int rc = inflate(&z_, Z_NO_FLUSH);
                salida.append(buf_, sizeof(buf_) - z_.avail_out);
                if (rc == Z_STREAM_END) {
                    fin_ = true;
                } else if (rc == Z_BUF_ERROR) {
                    if (z_.avail_out != 0) {
                        break;   // necesita más entrada; ya no queda en este trozo
                    }
                } else if (rc != Z_OK) {
                    error = std::string("the gzip data is damaged")
                            + (z_.msg ? std::string(": ") + z_.msg : std::string());
                    return false;
                }
            }
            hecho += ahora;
        }
        return true;
    }
    bool completo() const override { return fin_; }

private:
    z_stream z_{};
    bool vivo_{false};
    bool fin_{false};
    char buf_[kTrozo];
};

// ── lz4 (formato frame) ────────────────────────────────────────────────────────────

class CompresorLz4 : public Compresor {
public:
    bool inicia(int nivel, std::string& error) {
        if (LZ4F_isError(LZ4F_createCompressionContext(&ctx_, LZ4F_VERSION))) {
            error = "could not start lz4";
            return false;
        }
        prefs_ = LZ4F_preferences_t{};
        prefs_.compressionLevel = nivel;
        // La suma de comprobación del contenido: sin ella, un fichero dañado se
        // descomprimiría sin error y el fallo lo encontraría `zfs recv`, más tarde y peor.
        prefs_.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        return true;
    }
    ~CompresorLz4() override {
        if (ctx_) {
            LZ4F_freeCompressionContext(ctx_);
        }
    }
    bool comprime(const char* datos, std::size_t n, std::string& salida,
                  std::string& error) override {
        if (!empezado_) {
            std::vector<char> cab(LZ4F_HEADER_SIZE_MAX);
            const std::size_t r = LZ4F_compressBegin(ctx_, cab.data(), cab.size(), &prefs_);
            if (LZ4F_isError(r)) {
                error = std::string("lz4: ") + LZ4F_getErrorName(r);
                return false;
            }
            salida.append(cab.data(), r);
            empezado_ = true;
        }
        std::size_t hecho = 0;
        while (hecho < n) {
            const std::size_t ahora = std::min<std::size_t>(n - hecho, kTrozo);
            std::vector<char> out(LZ4F_compressBound(ahora, &prefs_));
            const std::size_t r = LZ4F_compressUpdate(ctx_, out.data(), out.size(),
                                                      datos + hecho, ahora, nullptr);
            if (LZ4F_isError(r)) {
                error = std::string("lz4: ") + LZ4F_getErrorName(r);
                return false;
            }
            salida.append(out.data(), r);
            hecho += ahora;
        }
        return true;
    }
    bool termina(std::string& salida, std::string& error) override {
        if (!empezado_ && !comprime(nullptr, 0, salida, error)) {
            return false;
        }
        std::vector<char> out(LZ4F_compressBound(0, &prefs_));
        const std::size_t r = LZ4F_compressEnd(ctx_, out.data(), out.size(), nullptr);
        if (LZ4F_isError(r)) {
            error = std::string("lz4: ") + LZ4F_getErrorName(r);
            return false;
        }
        salida.append(out.data(), r);
        return true;
    }

private:
    LZ4F_cctx* ctx_{nullptr};
    LZ4F_preferences_t prefs_{};
    bool empezado_{false};
};

class DescompresorLz4 : public Descompresor {
public:
    bool inicia(std::string& error) {
        if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx_, LZ4F_VERSION))) {
            error = "could not start lz4";
            return false;
        }
        return true;
    }
    ~DescompresorLz4() override {
        if (ctx_) {
            LZ4F_freeDecompressionContext(ctx_);
        }
    }
    bool descomprime(const char* datos, std::size_t n, std::string& salida,
                     std::string& error) override {
        std::size_t hecho = 0;
        while (hecho < n) {
            std::size_t entra = n - hecho;
            std::size_t sale = sizeof(buf_);
            // Devuelve cuánto espera todavía para acabar el frame: 0 es «frame completo».
            const std::size_t r = LZ4F_decompress(ctx_, buf_, &sale, datos + hecho, &entra,
                                                  nullptr);
            if (LZ4F_isError(r)) {
                error = std::string("the lz4 data is damaged: ") + LZ4F_getErrorName(r);
                return false;
            }
            salida.append(buf_, sale);
            hecho += entra;
            fin_ = r == 0;
            if (entra == 0 && sale == 0) {
                break;
            }
        }
        return true;
    }
    bool completo() const override { return fin_; }

private:
    LZ4F_dctx* ctx_{nullptr};
    bool fin_{false};
    char buf_[kTrozo];
};

// ── zstd ────────────────────────────────────────────────────────────────────────────

class CompresorZstd : public Compresor {
public:
    bool inicia(int nivel, std::string& error) {
        ctx_ = ZSTD_createCCtx();
        if (!ctx_) {
            error = "could not start zstd";
            return false;
        }
        ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, nivel);
        // La suma del contenido, por lo mismo que en lz4.
        ZSTD_CCtx_setParameter(ctx_, ZSTD_c_checksumFlag, 1);
        return true;
    }
    ~CompresorZstd() override {
        if (ctx_) {
            ZSTD_freeCCtx(ctx_);
        }
    }
    bool comprime(const char* datos, std::size_t n, std::string& salida,
                  std::string& error) override {
        return pasa(datos, n, ZSTD_e_continue, salida, error);
    }
    bool termina(std::string& salida, std::string& error) override {
        return pasa(nullptr, 0, ZSTD_e_end, salida, error);
    }

private:
    bool pasa(const char* datos, std::size_t n, ZSTD_EndDirective modo, std::string& salida,
              std::string& error) {
        ZSTD_inBuffer in{datos, n, 0};
        for (;;) {
            ZSTD_outBuffer out{buf_, sizeof(buf_), 0};
            const std::size_t r = ZSTD_compressStream2(ctx_, &out, &in, modo);
            if (ZSTD_isError(r)) {
                error = std::string("zstd: ") + ZSTD_getErrorName(r);
                return false;
            }
            salida.append(buf_, out.pos);
            // Al terminar, `r` es lo que queda por volcar; sin terminar, basta con haber
            // consumido toda la entrada.
            const bool acabado = modo == ZSTD_e_end ? r == 0 : in.pos == in.size;
            if (acabado) {
                return true;
            }
        }
    }

    ZSTD_CCtx* ctx_{nullptr};
    char buf_[kTrozo];
};

class DescompresorZstd : public Descompresor {
public:
    bool inicia(std::string& error) {
        ctx_ = ZSTD_createDCtx();
        if (!ctx_) {
            error = "could not start zstd";
            return false;
        }
        return true;
    }
    ~DescompresorZstd() override {
        if (ctx_) {
            ZSTD_freeDCtx(ctx_);
        }
    }
    bool descomprime(const char* datos, std::size_t n, std::string& salida,
                     std::string& error) override {
        ZSTD_inBuffer in{datos, n, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{buf_, sizeof(buf_), 0};
            // Devuelve 0 justo al acabar un frame; con varios frames seguidos, sigue solo.
            const std::size_t r = ZSTD_decompressStream(ctx_, &out, &in);
            if (ZSTD_isError(r)) {
                error = std::string("the zstd data is damaged: ") + ZSTD_getErrorName(r);
                return false;
            }
            salida.append(buf_, out.pos);
            fin_ = r == 0;
        }
        return true;
    }
    bool completo() const override { return fin_; }

private:
    ZSTD_DCtx* ctx_{nullptr};
    bool fin_{false};
    char buf_[kTrozo];
};

}  // namespace

std::unique_ptr<Compresor> creaCompresor(const Eleccion& e, std::string& error) {
    switch (e.formato) {
        case Formato::Gzip: {
            auto c = std::make_unique<CompresorGzip>();
            return c->inicia(e.nivel, error) ? std::move(c) : nullptr;
        }
        case Formato::Lz4: {
            auto c = std::make_unique<CompresorLz4>();
            return c->inicia(e.nivel, error) ? std::move(c) : nullptr;
        }
        case Formato::Zstd: {
            auto c = std::make_unique<CompresorZstd>();
            return c->inicia(e.nivel, error) ? std::move(c) : nullptr;
        }
        case Formato::Ninguno:
            break;
    }
    return nullptr;
}

std::unique_ptr<Descompresor> creaDescompresor(Formato f, std::string& error) {
    switch (f) {
        case Formato::Gzip: {
            auto d = std::make_unique<DescompresorGzip>();
            return d->inicia(error) ? std::move(d) : nullptr;
        }
        case Formato::Lz4: {
            auto d = std::make_unique<DescompresorLz4>();
            return d->inicia(error) ? std::move(d) : nullptr;
        }
        case Formato::Zstd: {
            auto d = std::make_unique<DescompresorZstd>();
            return d->inicia(error) ? std::move(d) : nullptr;
        }
        case Formato::Ninguno:
            break;
    }
    return nullptr;
}

}  // namespace zfsmgr::compresion
