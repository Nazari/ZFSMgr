#pragma once

#include <QMap>
#include <QVector>

#include "connectionmodel.h"
#include "connectionstore.h"

// Las conexiones que la aplicación tiene abiertas: lo que se sabe de cada máquina.
//
// Existe porque `m_profiles` lo tocaban 167 métodos de `MainWindow` y `m_states` otros
// 70 —el 41% y el 17%—, y mientras fueran campos sueltos de la ventana nada de eso podía
// probarse sin levantar Qt, ni reutilizarse desde un CLI. Ver la medición en
// docs/diseno_tecnico_capa_base_sin_qt.md.
//
// **EL INVARIANTE:** `profiles` y `states` son vectores PARALELOS, indexados los dos por
// el mismo `connIdx`. Tenerlos separados hacía que mantenerlo fuera cosa de acordarse; el
// único sitio que puede cambiar el tamaño es `setProfiles()`, que ajusta los dos a la vez.
//
// **LO QUE NO RESUELVE, y conviene no olvidar:** guardar una REFERENCIA a un elemento
// sigue siendo peligroso. Un diálogo modal bombea el bucle de eventos, por ahí puede
// colarse una recarga de conexiones, y la referencia queda colgando —lo cual importa de
// verdad porque alguno de esos valores acaba dentro de una orden que se ejecuta con
// sudo—. La regla sigue siendo copiar por valor antes de abrir nada modal.
struct ConnectionRegistry {
    // De dónde salen las conexiones y adónde vuelven. Vive aquí porque `loadConnections()`
    // era ya una danza entre el almacén y estos dos vectores repartida en dos campos
    // distintos de la ventana.
    //
    // Sin constructor por defecto, así que el registro tiene el suyo: es lo que obliga a
    // decir de qué aplicación se están cargando las conexiones, en vez de que se dé por
    // sabido.
    ConnectionStore store;

    explicit ConnectionRegistry(const QString& appName) : store(appName) {}

    QVector<ConnectionProfile> profiles;
    QVector<ConnectionRuntimeState> states;

    // Cachés de lo leído a cada máquina.
    //
    // Se indexaban por POSICIÓN, y ahí había un fallo: `loadConnections()` reindexa los
    // perfiles, así que al borrar una conexión la siguiente heredaba su índice —y con él
    // sus datasets cacheados—. Ya no: la parte de conexión de toda clave sale de
    // `MainWindow::connToken()`, que devuelve el IDENTIFICADOR.
    //
    // El vaciado de abajo se conserva de todos modos, y no por desconfianza: con claves
    // estables, las entradas de una conexión BORRADA no las reclamaría nadie nunca.
    QMap<QString, PoolDatasetCache> poolDatasetCache;
    QMap<QString, PoolDetailsCacheEntry> poolDetailsCache;
    QMap<QString, DatasetPermissionsCacheEntry> datasetPermissionsCache;
    QMap<QString, ConnInfo> connInfoById;
    QVector<PoolListEntry> poolListEntries;

    // `connInfoById` no entra: lo reconstruye `rebuildConnInfoModel()`. Ni
    // `poolListEntries`, que no es una caché sino una lista que rehace entera
    // `populateAllPoolsTables()`, a la que `loadConnections()` ni llama: vaciarla aquí
    // sería una regresión metida por el propio arreglo.
    void clearIndexedCaches() {
        poolDatasetCache.clear();
        poolDetailsCache.clear();
        datasetPermissionsCache.clear();
    }

    int size() const { return profiles.size(); }
    bool indexOk(int i) const { return i >= 0 && i < profiles.size(); }

    // El ÚNICO camino para cambiar el número de conexiones. Los estados se ajustan al
    // nuevo tamaño; los que sobreviven conserva quien llame, que es quien sabe emparejar
    // por identificador o por nombre.
    void setProfiles(QVector<ConnectionProfile> nuevos) {
        profiles = std::move(nuevos);
        states.clear();
        states.resize(profiles.size());
        // Las claves ya son estables, así que esto no es lo que impide el fallo. Se
        // mantiene para no acumular las entradas de conexiones que ya no existen.
        clearIndexedCaches();
    }

    void clear() {
        profiles.clear();
        states.clear();
        clearIndexedCaches();
        connInfoById.clear();
        poolListEntries.clear();
    }

    // Añade una conexión con su estado, manteniendo los dos vectores a la par.
    void append(const ConnectionProfile& p, const ConnectionRuntimeState& s) {
        profiles.push_back(p);
        states.push_back(s);
    }
};
