#pragma once

#include "base/transportsession.h"
#include "connectionstore.h"

#include <QByteArray>
#include <QString>

// Adaptador de la sesión del transporte para el lado que todavía habla `QString`.
//
// La sesión de verdad vive en `base/transportsession.h` y no depende de Qt: es la que
// usan las funciones ya mudadas. Esto solo añade las mismas operaciones tomando tipos de
// Qt, para no tener que reescribir en un único cambio los ochenta sitios que registran
// mensajes mientras el resto del transporte se muda.
//
// **Es temporal.** Cuando la última función del transporte hable `std::string`, este
// fichero desaparece y `TransportSession` pasa a ser directamente el de la base.
struct TransportSession : zfsmgr::base::TransportSession {
    using Base = zfsmgr::base::TransportSession;

    // Las de la base siguen visibles: las funciones ya mudadas llaman a estas.
    using Base::askCredentials;
    using Base::log;
    using Base::logConn;
    using Base::persistTls;
    using Base::resolveLocalSudo;

    void log(Nivel n, const QString& msg) const { Base::log(n, msg.toStdString()); }
    void logConn(Nivel n, const QString& connId, const QString& msg) const {
        Base::logConn(n, connId.toStdString(), msg.toStdString());
    }
    bool askCredentials(const QString& motivo, QString& usuario, QString& clave) const {
        std::string u = usuario.toStdString();
        std::string c = clave.toStdString();
        const bool ok = Base::askCredentials(motivo.toStdString(), u, c);
        usuario = QString::fromStdString(u);
        clave = QString::fromStdString(c);
        return ok;
    }
    bool resolveLocalSudo(ConnectionProfile& perfil) const {
        zfsmgr::base::ConnectionProfile b = toBaseProfile(perfil);
        const bool ok = Base::resolveLocalSudo(b);
        perfil = fromBaseProfile(b);
        return ok;
    }
    bool persistTls(const ConnectionProfile& p, const QByteArray& serverCertPem,
                    const QByteArray& clientCertPem, const QByteArray& clientKeyPem,
                    quint16 daemonPort, QString* errorOut) const {
        std::string e;
        const bool ok = Base::persistTls(toBaseProfile(p), serverCertPem.toStdString(),
                                         clientCertPem.toStdString(), clientKeyPem.toStdString(),
                                         daemonPort, &e);
        if (errorOut) {
            *errorOut = QString::fromStdString(e);
        }
        return ok;
    }
};
