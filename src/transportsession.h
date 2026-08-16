#pragma once

#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>

// Lo que la aplicación mantiene ABIERTO mientras habla con las máquinas remotas: los
// túneles del RPC y la memoria de los intentos que fallaron.
//
// Existe por dos motivos. Uno: es lo que un CLI necesitaría para hablar con el agente, y
// mientras fueran campos sueltos de la ventana no se podía usar desde otro sitio. Dos, y
// más importante hoy: **el cerrojo y lo que protege estaban separados**, y solo un
// comentario decía cuáles iban juntos. Ahora viven en la misma estructura.
//
// Las claves NO son índices de conexión: salen de las coordenadas (usuario, host, puerto,
// ruta de clave), así que sobreviven a que se reordene la lista. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md, sección de las cachés por posición, para lo
// que pasa cuando no es así.
struct RemoteRpcTunnelState {
    QPointer<QProcess> process;
    quint16 localPort{0};
    quint16 remotePort{0};
    QDateTime startedAtUtc;
    QDateTime lastUsedUtc;
};

struct TransportSession {
    // TODO lo de abajo va bajo este cerrojo. El refresco de conexiones corre en hilos
    // (QtConcurrent) y estos mapas se tocan desde varios a la vez.
    mutable QMutex mutex;

    // Túneles `ssh -L` vivos, por clave de conexión.
    QMap<QString, RemoteRpcTunnelState> tunnelsByConnKey;

    // Claves cuyo túnel se está montando AHORA MISMO. Protege de la reentrancia que
    // provoca el `processEvents` de la espera: sin esto se montaban túneles duplicados
    // que quedaban huérfanos fuera del mapa.
    QSet<QString> tunnelsBeingCreated;

    // Hasta cuándo no se reintenta el RPC de una conexión, y por qué. Sin esto, una
    // conexión con el daemon caído se lleva una ida y vuelta por SSH en cada operación.
    QMap<QString, QDateTime> retryAfterByConnKey;
    QMap<QString, QString> retryReasonByConnKey;

    // Conexiones a las que se ha renunciado al multiplexado de SSH, y aquellas cuya
    // resolución de nombre ya se anotó en el registro: las dos existen para no repetir
    // el mismo mensaje en cada operación.
    QSet<QString> disableMultiplexKeys;
    QSet<QString> loggedResolutionKeys;
};
