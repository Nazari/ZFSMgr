#include "transportreason.h"

namespace zfsmgr::base::transport {

bool sugiereRevivirDaemon(Fallo f) {
    switch (f) {
        // El daemon no contesta o corta a media conversación: puede estar caído, y
        // levantarlo es barato comparado con dar la conexión por perdida.
        case Fallo::ConexionRechazada:
        case Fallo::HandshakeFallido:
        case Fallo::RespuestaNoValida:
        case Fallo::TunelCortadoEnEspera:
            return true;

        // El material TLS está mal. Revivir el servicio no lo arregla —hace falta volver a
        // aprovisionar— y gastaría una conexión SSH para nada.
        case Fallo::CertificadosInvalidos:
        case Fallo::ClaveClienteInvalida:
        case Fallo::CertificadoNoCoincide:
        case Fallo::MaterialNoSeLee:
        case Fallo::MaterialIncompleto:
        case Fallo::ClaveClienteNoDisponible:
            return false;

        // Ni siquiera se llegó a hablar con el daemon.
        case Fallo::TunelOcupado:
        case Fallo::FueraDelHiloDeTuneles:
        case Fallo::ArgumentosVacios:
        case Fallo::ConexionNoSsh:
        case Fallo::EnEspera:
        case Fallo::TunelNoSeMonta:
        // El envío pudo llegar. Reintentar ya lo decide quien sabe si la orden mutaba:
        // aquí levantar el servicio no aporta y podría solaparse con lo que ya corre.
        case Fallo::EnvioFallido:
        case Fallo::NoEspecificado:
        case Fallo::Ninguno:
            return false;
    }
    return false;
}

bool esDeTls(Fallo f) {
    switch (f) {
        case Fallo::MaterialNoSeLee:
        case Fallo::MaterialIncompleto:
        case Fallo::ClaveClienteNoDisponible:
        case Fallo::CertificadosInvalidos:
        case Fallo::ClaveClienteInvalida:
        case Fallo::CertificadoNoCoincide:
        case Fallo::HandshakeFallido:
            return true;

        case Fallo::TunelOcupado:
        case Fallo::FueraDelHiloDeTuneles:
        case Fallo::ArgumentosVacios:
        case Fallo::ConexionNoSsh:
        case Fallo::EnEspera:
        case Fallo::TunelNoSeMonta:
        case Fallo::ConexionRechazada:
        case Fallo::EnvioFallido:
        case Fallo::TunelCortadoEnEspera:
        case Fallo::RespuestaNoValida:
        case Fallo::NoEspecificado:
        case Fallo::Ninguno:
            return false;
    }
    return false;
}

bool mereceCastigo(Fallo f) {
    switch (f) {
        // Ocupado no dice nada sobre si el daemon está vivo: castigarlo dejaba sin daemon
        // al refresco que venía detrás por haber caído en el hueco equivocado.
        case Fallo::TunelOcupado:
        case Fallo::FueraDelHiloDeTuneles:
        // Ya se está castigando; volver a castigar alargaría la espera sin motivo.
        case Fallo::EnEspera:
        // No son de la conexión, sino de quien llama.
        case Fallo::ArgumentosVacios:
        case Fallo::ConexionNoSsh:
        case Fallo::Ninguno:
            return false;

        case Fallo::MaterialNoSeLee:
        case Fallo::MaterialIncompleto:
        case Fallo::ClaveClienteNoDisponible:
        case Fallo::CertificadosInvalidos:
        case Fallo::ClaveClienteInvalida:
        case Fallo::TunelNoSeMonta:
        case Fallo::ConexionRechazada:
        case Fallo::CertificadoNoCoincide:
        case Fallo::EnvioFallido:
        case Fallo::TunelCortadoEnEspera:
        case Fallo::HandshakeFallido:
        case Fallo::RespuestaNoValida:
        case Fallo::NoEspecificado:
            return true;
    }
    return true;
}

const char* etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno: return "";
        case Fallo::TunelOcupado: return "tunel-ocupado";
        case Fallo::FueraDelHiloDeTuneles: return "fuera-del-hilo";
        case Fallo::ArgumentosVacios: return "argumentos-vacios";
        case Fallo::ConexionNoSsh: return "conexion-no-ssh";
        case Fallo::EnEspera: return "en-espera";
        case Fallo::MaterialNoSeLee: return "tls-no-se-lee";
        case Fallo::MaterialIncompleto: return "tls-incompleto";
        case Fallo::ClaveClienteNoDisponible: return "tls-sin-clave-cliente";
        case Fallo::CertificadosInvalidos: return "tls-certificados-invalidos";
        case Fallo::ClaveClienteInvalida: return "tls-clave-cliente-invalida";
        case Fallo::TunelNoSeMonta: return "tunel-no-se-monta";
        case Fallo::ConexionRechazada: return "conexion-rechazada";
        case Fallo::CertificadoNoCoincide: return "tls-certificado-no-coincide";
        case Fallo::EnvioFallido: return "envio-fallido";
        case Fallo::TunelCortadoEnEspera: return "tunel-cortado";
        case Fallo::HandshakeFallido: return "tls-handshake";
        case Fallo::RespuestaNoValida: return "respuesta-no-valida";
        case Fallo::NoEspecificado: return "sin-motivo";
    }
    return "sin-motivo";
}

}  // namespace zfsmgr::base::transport
