#include "transportreason.h"

namespace zfsmgr::base::transport {

bool sugiereRevivirDaemon(Failure f) {
    switch (f) {
        // El daemon no contesta o corta a media conversación: puede estar caído, y
        // levantarlo es barato comparado con dar la conexión por perdida.
        case Failure::ConexionRechazada:
        case Failure::HandshakeFallido:
        case Failure::RespuestaNoValida:
        case Failure::TunelCortadoEnEspera:
            return true;

        // El material TLS está mal. Revivir el servicio no lo arregla —hace falta volver a
        // aprovisionar— y gastaría una conexión SSH para nada.
        case Failure::CertificadosInvalidos:
        case Failure::ClaveClienteInvalida:
        case Failure::CertificadoNoCoincide:
        case Failure::MaterialNoSeLee:
        case Failure::MaterialIncompleto:
        case Failure::ClaveClienteNoDisponible:
            return false;

        // Ni siquiera se llegó a hablar con el daemon.
        case Failure::TunelOcupado:
        case Failure::FueraDelHiloDeTuneles:
        case Failure::ArgumentosVacios:
        case Failure::ConexionNoSsh:
        case Failure::EnEspera:
        case Failure::TunelNoSeMonta:
        // El envío pudo llegar. Reintentar ya lo decide quien sabe si la orden mutaba:
        // aquí levantar el servicio no aporta y podría solaparse con lo que ya corre.
        case Failure::EnvioFallido:
        case Failure::NoEspecificado:
        case Failure::None_:
            return false;
    }
    return false;
}

bool esDeTls(Failure f) {
    switch (f) {
        case Failure::MaterialNoSeLee:
        case Failure::MaterialIncompleto:
        case Failure::ClaveClienteNoDisponible:
        case Failure::CertificadosInvalidos:
        case Failure::ClaveClienteInvalida:
        case Failure::CertificadoNoCoincide:
        case Failure::HandshakeFallido:
            return true;

        case Failure::TunelOcupado:
        case Failure::FueraDelHiloDeTuneles:
        case Failure::ArgumentosVacios:
        case Failure::ConexionNoSsh:
        case Failure::EnEspera:
        case Failure::TunelNoSeMonta:
        case Failure::ConexionRechazada:
        case Failure::EnvioFallido:
        case Failure::TunelCortadoEnEspera:
        case Failure::RespuestaNoValida:
        case Failure::NoEspecificado:
        case Failure::None_:
            return false;
    }
    return false;
}

bool mereceCastigo(Failure f) {
    switch (f) {
        // Ocupado no dice nada sobre si el daemon está vivo: castigarlo dejaba sin daemon
        // al refresco que venía detrás por haber caído en el hueco equivocado.
        case Failure::TunelOcupado:
        case Failure::FueraDelHiloDeTuneles:
        // Ya se está castigando; volver a castigar alargaría la espera sin motivo.
        case Failure::EnEspera:
        // No son de la conexión, sino de quien llama.
        case Failure::ArgumentosVacios:
        case Failure::ConexionNoSsh:
        case Failure::None_:
            return false;

        case Failure::MaterialNoSeLee:
        case Failure::MaterialIncompleto:
        case Failure::ClaveClienteNoDisponible:
        case Failure::CertificadosInvalidos:
        case Failure::ClaveClienteInvalida:
        case Failure::TunelNoSeMonta:
        case Failure::ConexionRechazada:
        case Failure::CertificadoNoCoincide:
        case Failure::EnvioFallido:
        case Failure::TunelCortadoEnEspera:
        case Failure::HandshakeFallido:
        case Failure::RespuestaNoValida:
        case Failure::NoEspecificado:
            return true;
    }
    return true;
}

const char* labelOf(Failure f) {
    switch (f) {
        case Failure::None_: return "";
        case Failure::TunelOcupado: return "tunel-ocupado";
        case Failure::FueraDelHiloDeTuneles: return "fuera-del-hilo";
        case Failure::ArgumentosVacios: return "argumentos-vacios";
        case Failure::ConexionNoSsh: return "conexion-no-ssh";
        case Failure::EnEspera: return "en-espera";
        case Failure::MaterialNoSeLee: return "tls-no-se-lee";
        case Failure::MaterialIncompleto: return "tls-incompleto";
        case Failure::ClaveClienteNoDisponible: return "tls-sin-clave-cliente";
        case Failure::CertificadosInvalidos: return "tls-certificados-invalidos";
        case Failure::ClaveClienteInvalida: return "tls-clave-cliente-invalida";
        case Failure::TunelNoSeMonta: return "tunel-no-se-monta";
        case Failure::ConexionRechazada: return "conexion-rechazada";
        case Failure::CertificadoNoCoincide: return "tls-certificado-no-coincide";
        case Failure::EnvioFallido: return "envio-fallido";
        case Failure::TunelCortadoEnEspera: return "tunel-cortado";
        case Failure::HandshakeFallido: return "tls-handshake";
        case Failure::RespuestaNoValida: return "respuesta-no-valida";
        case Failure::NoEspecificado: return "sin-motivo";
    }
    return "sin-motivo";
}


const char* labelOf(Aviso a) {
    switch (a) {
        case Aviso::None_: return "";
        case Aviso::TlsLocalNoLegible: return "tls-local-no-legible";
        case Aviso::TlsLocalSinSudo: return "tls-local-sin-sudo";
        case Aviso::TlsLocalNoSeLee: return "tls-local-no-se-lee";
        case Aviso::TlsLocalIncompleto: return "tls-local-incompleto";
        case Aviso::HostSshNoVerificado: return "host-ssh-no-verificado";
        case Aviso::SinSshpass: return "sin-sshpass";
        case Aviso::MultiplexadoFallo: return "multiplexado-fallo";
        case Aviso::MultiplexadoDesactivado: return "multiplexado-desactivado";
        case Aviso::TunelNoAceptaSshMurio: return "tunel-ssh-murio";
        case Aviso::TunelNoAceptaEsperaAgotada: return "tunel-espera-agotada";
    }
    return "";
}

}  // namespace zfsmgr::base::transport
