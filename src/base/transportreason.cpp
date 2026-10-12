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
        // La clave SSH cambiada tampoco: ni siquiera se llegó al daemon.
        case Failure::HostKeyChanged:
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
        // Es de SSH, no de TLS: presentarlo como «TLS en espera» mandaría a mirar los
        // certificados.
        case Failure::HostKeyChanged:
            return false;

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
        // La clave no va a arreglarse sola en 30 s, y reintentar al momento repetiría el
        // mismo error en cada refresco.
        case Failure::HostKeyChanged:
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
        case Failure::TunnelBusy:
            return "tunnel-busy";
        case Failure::OffTheTunnelThread:
            return "off-the-tunnel-thread";
        case Failure::EmptyArguments:
            return "empty-arguments";
        case Failure::ConnectionNotSsh:
            return "connection-not-ssh";
        case Failure::Cooling:
            return "cooling";
        case Failure::MaterialCannotBeRead:
            return "tls-unreadable";
        case Failure::HostKeyChanged:
            return "ssh-host-key-changed";
        case Failure::MaterialIncomplete:
            return "tls-incomplete";
        case Failure::ClientKeyUnavailable:
            return "tls-no-client-key";
        case Failure::InvalidCertificates:
            return "tls-invalid-certificates";
        case Failure::InvalidClientKey:
            return "tls-invalid-client-key";
        case Failure::TunnelCannotBeBuilt:
            return "tunnel-cannot-be-built";
        case Failure::ConnectionRefused:
            return "connection-refused";
        case Failure::CertificateMismatch:
            return "tls-certificate-mismatch";
        case Failure::SendFailed:
            return "send-failed";
        case Failure::TunnelCutWhileWaiting:
            return "tunnel-cut";
        case Failure::HandshakeFailed: return "tls-handshake";
        case Failure::InvalidAnswer:
            return "invalid-answer";
        case Failure::Unspecified:
            return "unspecified";
    }
    return "unspecified";
}


const char* labelOf(Warning a) {
    switch (a) {
        case Warning::None_: return "";
        case Warning::LocalTlsUnreadable:
            return "local-tls-unreadable";
        case Warning::LocalTlsNeedsSudo:
            return "local-tls-needs-sudo";
        case Warning::LocalTlsCannotBeRead:
            return "local-tls-cannot-be-read";
        case Warning::LocalTlsIncomplete:
            return "local-tls-incomplete";
        case Warning::SshHostUnverified:
            return "ssh-host-unverified";
        case Warning::NoSshpass:
            return "no-sshpass";
        case Warning::MultiplexingFailed:
            return "multiplexing-failed";
        case Warning::MultiplexingDisabled:
            return "multiplexing-disabled";
        case Warning::TunnelNotAcceptingSshDied:
            return "tunnel-ssh-died";
        case Warning::TunnelNotAcceptingTimedOut: return "tunel-espera-agotada";
    }
    return "";
}

}  // namespace zfsmgr::base::transport
