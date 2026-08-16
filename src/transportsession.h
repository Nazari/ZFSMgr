#pragma once

#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

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
    // --- A dónde va lo que el transporte cuenta mientras trabaja.
    //
    // Se consideró que cada llamada DEVOLVIERA la lista de lo ocurrido y que quien llama
    // decidiera qué hacer con ella. Es más limpio sobre el papel, pero **habría sido una
    // regresión**: `appLog()` escribe en la interfaz al momento y `runSsh()` bombea el
    // bucle de eventos seis veces, así que hoy el registro se llena MIENTRAS la operación
    // ocurre. Acumular y devolver al final dejaría treinta segundos de silencio y luego
    // un volcado de golpe.
    //
    // Así que se emite sobre la marcha, pero **a algo que se recibe**, no a algo que el
    // transporte busca: aquí dentro no se nombra `appLog` ni `MainWindow`. La interfaz
    // pone un destino que escribe en su pestaña; un CLI pondría uno que escriba por la
    // salida de error.
    enum class Nivel { Normal, Info, Warn, Error, Debug };

    // `connId` vacío significa «al registro general»; con valor, además al de esa
    // conexión. Sin destino puesto, no se pierde nada importante: solo no se cuenta.
    std::function<void(Nivel, const QString& connId, const QString& msg)> sink;

    void log(Nivel n, const QString& msg) const {
        if (sink) {
            sink(n, QString(), msg);
        }
    }
    // Al registro general Y al de la conexión, que es la pareja que se repetía a mano en
    // treinta sitios.
    void logConn(Nivel n, const QString& connId, const QString& msg) const {
        if (sink) {
            sink(n, connId, msg);
        }
    }

    // --- Transporte de mentira, para los tests.
    //
    // Vive aquí y no en la ventana porque es una propiedad DEL TRANSPORTE: mientras está
    // puesto no se abre ninguna conexión, las órdenes por argv van a esa función, y las
    // que salgan como cadena de shell se anotan y fracasan —para que un test pueda
    // afirmar que algo NO se fue por ese camino—.
    struct AgentCallForTest {
        QStringList argv;      // vacío si la orden salió como cadena de shell
        QString shellCommand;  // no vacío solo en ese caso
        QByteArray stdinPayload;
    };
    using AgentTransportForTest =
        std::function<bool(const QStringList& argv, QString& out, QString& err, int& rc)>;

    AgentTransportForTest transportForTest;
    QVector<AgentCallForTest> callsForTest;

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
