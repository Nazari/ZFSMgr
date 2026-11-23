// `master-key`: the FIDO2 keys (a YubiKey) that open ZFSMgr besides the master password, and
// opening with one on the terminal. See store::addKeySlot and fidokey.h.

#include "fidokey.h"
#include "shell_internal.h"

#include <atomic>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace zfsmgr::cli::detail {

namespace F = zfsmgr::cli::fido;

namespace {

std::string textoDe(F::Result r, const std::string& detalle) {
    switch (r) {
        case F::Result::Ok:
            return {};
        case F::Result::NoDevice:
            return T("t_mk_sin_llave", "no hay ninguna llave conectada");
        case F::Result::NotTouched:
            return T("t_mk_no_tocada", "la llave no se tocó a tiempo");
        case F::Result::PinRequired:
            return T("t_mk_pin", "la llave pide su PIN");
        case F::Result::PinInvalid:
            return T("t_mk_pin_malo", "ese no es el PIN de la llave");
        case F::Result::PinBlocked:
            return T("t_mk_pin_bloq",
                     "el PIN de la llave está bloqueado: hay que desbloquearlo con la "
                     "herramienta del fabricante");
        case F::Result::NotRegistered:
            return T("t_mk_no_registrada", "esta llave no está registrada aquí");
        case F::Result::Unsupported:
            return T("t_mk_no_sirve", "esta llave no sabe dar un secreto (hmac-secret)");
        case F::Result::Cancelled:
            return T("t_mk_cancelada", "cancelado");
        case F::Result::Failed:
            return B::format(T("t_mk_fallo", "la llave falló: %1"), {detalle});
    }
    return {};
}

void borra(std::string& s) {
    for (char& c : s) {
        c = '\0';
    }
    s.clear();
}

}  // namespace

bool cmdMasterKey(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const std::vector<std::string> args = pet.lista("texto");
    const std::string sub = args.empty() ? std::string("list") : args[0];
    const std::string& dir = e.ses->dirConfig;

    if (sub == "list") {
        Tabla t;
        t.nombreJson = "keys";
        t.cabecerasTexto = {T("t_cab_n", "N"), T("t_cab_tipo", "TIPO"),
                            T("t_cab_nombre", "NOMBRE")};
        t.campos = {"slot", "type", "label"};
        t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
        if (B::store::hasMasterSlots(dir)) {
            t.filas.push_back({"-", "password", T("t_mk_la_maestra", "la contraseña maestra")});
        }
        for (const B::store::KeySlot& k : B::store::keySlots(dir)) {
            t.filas.push_back({std::to_string(k.index), "fido2", k.label});
        }
        t.imprime(e.formato);
        return true;
    }

    if (e.ses->maestra.empty()) {
        std::fputs(TC("t_mk_sin_maestra",
                      "primero hace falta una contraseña maestra («master-password»): la llave "
                      "abre lo mismo que ella, no la sustituye\n"),
                   stderr);
        return false;
    }

    if (sub == "add") {
        B::store::Warning aviso;
        std::string sal;
        if (!B::store::keySalt(dir, sal, aviso)) {
            std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
            return false;
        }
        if (F::devices().empty()) {
            std::fprintf(stderr, "%s\n", textoDe(F::Result::NoDevice, {}).c_str());
            return false;
        }
        std::fputs(TC("t_mk_toca_dos",
                      "Toca la llave DOS veces cuando parpadee: una para registrarla y otra "
                      "para leer su secreto.\n"),
                   stderr);
        std::fflush(stderr);
        std::string pin;
        std::string id;
        bool residente = false;
        std::string secreto;
        std::string producto;
        std::string detalle;
        F::Result r = F::registerKey(sal, pin, id, residente, secreto, producto, detalle);
        if (r == F::Result::PinRequired) {
            std::string err;
            if (!pideSecreto(e, T("t_mk_p_pin", "PIN de la llave: "), pin, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            std::fputs(TC("t_mk_toca_dos_otra", "Toca la llave otra vez, dos veces.\n"), stderr);
            r = F::registerKey(sal, pin, id, residente, secreto, producto, detalle);
            borra(pin);
        }
        if (r != F::Result::Ok) {
            std::fprintf(stderr, "%s\n", textoDe(r, detalle).c_str());
            return false;
        }
        const std::string etiqueta = !pet.valor("label").empty()
                                         ? pet.valor("label")
                                         : (producto.empty() ? "FIDO2" : producto);
        const bool ok =
            B::store::addKeySlot(dir, e.ses->maestra, etiqueta, id, residente, secreto, aviso);
        borra(secreto);
        if (!ok) {
            std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
            return false;
        }
        std::fprintf(stderr, TC("t_mk_anadida", "llave añadida: «%s»; desde ahora abre ZFSMgr\n"),
                     etiqueta.c_str());
        return true;
    }

    if (sub == "remove") {
        if (args.size() < 2 || args[1].find_first_not_of("0123456789") != std::string::npos) {
            std::fputs(TC("t_mk_uso_remove", "uso: master-key remove <N> (el N de «master-key»)\n"),
                       stderr);
            return false;
        }
        const std::size_t n = static_cast<std::size_t>(std::stoul(args[1]));
        std::string etiqueta;
        for (const B::store::KeySlot& k : B::store::keySlots(dir)) {
            if (k.index == n) {
                etiqueta = k.label;
            }
        }
        if (etiqueta.empty()) {
            std::fprintf(stderr, TC("t_mk_no_hay", "no hay ninguna llave con el número %s\n"),
                         args[1].c_str());
            return false;
        }
        Pregunta q;
        q.titulo = B::format(T("t_pq_mk_quitar", "Quitar la llave «%1»"), {etiqueta});
        q.notas.push_back(T("t_pq_mk_quitar_nota",
                            "Dejará de abrir ZFSMgr. La contraseña maestra y las demás llaves "
                            "siguen sirviendo; en la llave no se borra nada."));
        if (!confirma(e, q)) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        B::store::Warning aviso;
        if (!B::store::removeKeySlot(dir, n, aviso)) {
            std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
            return false;
        }
        std::fprintf(stderr, TC("t_mk_quitada", "llave quitada: «%s»\n"), etiqueta.c_str());
        return true;
    }

    std::fprintf(stderr,
                 TC("t_mk_uso", "uso: master-key [list|add [--label <nombre>]|remove <N>]\n"));
    return false;
}

bool abreConLlave(const std::string& dir, std::string& dek, std::string& tecleada) {
    dek.clear();
    tecleada.clear();
    const std::vector<B::store::KeySlot> llaves = B::store::keySlots(dir);
    if (llaves.empty() || F::devices().empty()) {
        return false;
    }
    std::string sal;
    B::store::Warning aviso;
    if (!B::store::keySalt(dir, sal, aviso)) {
        return false;
    }
    std::vector<std::string> ids;
    for (const B::store::KeySlot& k : llaves) {
        ids.push_back(k.credentialId);
    }
    std::fputs(TC("t_mk_toca_entrar",
                  "Toca la llave para entrar (o escribe la contraseña maestra y pulsa Intro): "),
               stderr);
    std::fflush(stderr);

    std::atomic<bool> hecho{false};
    F::Result r = F::Result::Failed;
    std::string usada;
    std::string secreto;
    std::string detalle;
    std::thread espera([&] {
        r = F::secret(ids, sal, {}, usada, secreto, detalle);
        hecho = true;
    });
    bool escrita = false;
#ifndef _WIN32
    // Whatever is typed meanwhile is the master password: without echo, and it cancels the
    // wait for the touch when Enter is pressed.
    termios antes{};
    const bool terminal = isatty(0) && tcgetattr(0, &antes) == 0;
    if (terminal) {
        termios sinEco = antes;
        sinEco.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(0, TCSANOW, &sinEco);
    }
    while (!hecho) {
        pollfd p{0, POLLIN, 0};
        if (terminal && poll(&p, 1, 100) > 0) {
            std::getline(std::cin, tecleada);
            escrita = true;
            F::cancel();
            break;
        }
        if (!terminal) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    espera.join();
    if (terminal) {
        tcsetattr(0, TCSANOW, &antes);
    }
#else
    // Windows shows its own Windows Hello dialog, which has its own «cancel».
    espera.join();
#endif
    std::fputs("\n", stderr);
    if (escrita) {
        borra(secreto);
        return false;
    }
    if (r != F::Result::Ok) {
        std::fprintf(stderr, "%s\n", textoDe(r, detalle).c_str());
        borra(secreto);
        return false;
    }
    const bool ok = B::store::unlockWithKeySecret(dir, usada, secreto, dek, aviso);
    borra(secreto);
    if (!ok) {
        std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
    }
    return ok;
}

}  // namespace zfsmgr::cli::detail

namespace zfsmgr::cli {

bool abrirConLlave(const std::string& dirConfig, std::string& dataKey, std::string& typed) {
    return detail::abreConLlave(dirConfig, dataKey, typed);
}

}  // namespace zfsmgr::cli
