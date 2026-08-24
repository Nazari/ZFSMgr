#include "transportreason.h"

namespace zfsmgr::base::transport {

bool suggestsDaemonRevival(Failure f) {
    switch (f) {
        // El daemon no contesta o corta a media conversación: puede estar caído, y
        // levantarlo es barato comparado con dar la conexión por perdida.
        case Failure::ConnectionRefused:
        case Failure::HandshakeFailed:
        case Failure::InvalidAnswer:
        case Failure::TunnelCutWhileWaiting:
            return true;

        // El material TLS está mal. Revivir el servicio no lo arregla —hace falta volver a
        // aprovisionar— y gastaría una conexión SSH para nada.
        case Failure::InvalidCertificates:
        case Failure::InvalidClientKey:
        case Failure::CertificateMismatch:
        case Failure::MaterialCannotBeRead:
        case Failure::MaterialIncomplete:
        case Failure::ClientKeyUnavailable:
            return false;

        // Ni siquiera se llegó a hablar con el daemon.
        case Failure::TunnelBusy:
        case Failure::OffTheTunnelThread:
        case Failure::EmptyArguments:
        case Failure::ConnectionNotSsh:
        case Failure::Cooling:
        case Failure::TunnelCannotBeBuilt:
        // El envío pudo llegar. Reintentar ya lo decide quien sabe si la orden mutaba:
        // aquí levantar el servicio no aporta y podría solaparse con lo que ya corre.
        case Failure::SendFailed:
        case Failure::Unspecified:
        case Failure::None_:
            return false;
    }
    return false;
}

bool looksLikeTls(Failure f) {
    switch (f) {
        case Failure::MaterialCannotBeRead:
        case Failure::MaterialIncomplete:
        case Failure::ClientKeyUnavailable:
        case Failure::InvalidCertificates:
        case Failure::InvalidClientKey:
        case Failure::CertificateMismatch:
        case Failure::HandshakeFailed:
            return true;

        case Failure::TunnelBusy:
        case Failure::OffTheTunnelThread:
        case Failure::EmptyArguments:
        case Failure::ConnectionNotSsh:
        case Failure::Cooling:
        case Failure::TunnelCannotBeBuilt:
        case Failure::ConnectionRefused:
        case Failure::SendFailed:
        case Failure::TunnelCutWhileWaiting:
        case Failure::InvalidAnswer:
        case Failure::Unspecified:
        case Failure::None_:
            return false;
    }
    return false;
}

bool deservesPenalty(Failure f) {
    switch (f) {
        // Ocupado no dice nada sobre si el daemon está vivo: castigarlo dejaba sin daemon
        // al refresco que venía detrás por haber caído en el hueco equivocado.
        case Failure::TunnelBusy:
        case Failure::OffTheTunnelThread:
        // Ya se está castigando; volver a castigar alargaría la espera sin motivo.
        case Failure::Cooling:
        // No son de la conexión, sino de quien llama.
        case Failure::EmptyArguments:
        case Failure::ConnectionNotSsh:
        case Failure::None_:
            return false;

        case Failure::MaterialCannotBeRead:
        case Failure::MaterialIncomplete:
        case Failure::ClientKeyUnavailable:
        case Failure::InvalidCertificates:
        case Failure::InvalidClientKey:
        case Failure::TunnelCannotBeBuilt:
        case Failure::ConnectionRefused:
        case Failure::CertificateMismatch:
        case Failure::SendFailed:
        case Failure::TunnelCutWhileWaiting:
        case Failure::HandshakeFailed:
        case Failure::InvalidAnswer:
        case Failure::Unspecified:
            return true;
    }
    return true;
}

const char* labelOf(Failure f) {
    switch (f) {
        case Failure::None_: return "";
        case Failure::TunnelBusy: return "tunel-ocupado";
        case Failure::OffTheTunnelThread: return "fuera-del-hilo";
        case Failure::EmptyArguments: return "argumentos-vacios";
        case Failure::ConnectionNotSsh: return "conexion-no-ssh";
        case Failure::Cooling: return "en-espera";
        case Failure::MaterialCannotBeRead: return "tls-no-se-lee";
        case Failure::MaterialIncomplete: return "tls-incompleto";
        case Failure::ClientKeyUnavailable: return "tls-sin-clave-cliente";
        case Failure::InvalidCertificates: return "tls-certificados-invalidos";
        case Failure::InvalidClientKey: return "tls-clave-cliente-invalida";
        case Failure::TunnelCannotBeBuilt: return "tunel-no-se-monta";
        case Failure::ConnectionRefused: return "conexion-rechazada";
        case Failure::CertificateMismatch: return "tls-certificado-no-coincide";
        case Failure::SendFailed: return "envio-fallido";
        case Failure::TunnelCutWhileWaiting: return "tunel-cortado";
        case Failure::HandshakeFailed: return "tls-handshake";
        case Failure::InvalidAnswer: return "respuesta-no-valida";
        case Failure::Unspecified: return "sin-motivo";
    }
    return "sin-motivo";
}


const char* labelOf(Warning a) {
    switch (a) {
        case Warning::None_: return "";
        case Warning::LocalTlsUnreadable: return "tls-local-no-legible";
        case Warning::LocalTlsNeedsSudo: return "tls-local-sin-sudo";
        case Warning::LocalTlsCannotBeRead: return "tls-local-no-se-lee";
        case Warning::LocalTlsIncomplete: return "tls-local-incompleto";
        case Warning::SshHostUnverified: return "host-ssh-no-verificado";
        case Warning::NoSshpass: return "sin-sshpass";
        case Warning::MultiplexingFailed: return "multiplexado-fallo";
        case Warning::MultiplexingDisabled: return "multiplexado-desactivado";
        case Warning::TunnelNotAcceptingSshDied: return "tunel-ssh-murio";
        case Warning::TunnelNotAcceptingTimedOut: return "tunel-espera-agotada";
    }
    return "";
}

}  // namespace zfsmgr::base::transport
