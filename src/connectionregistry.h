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
    QVector<ConnectionProfile> profiles;
    QVector<ConnectionRuntimeState> states;

    // Cachés de lo leído a cada máquina. Las TRES PRIMERAS se indexan por «connIdx::…»,
    // y AHÍ ESTABA UN FALLO: `loadConnections()` reindexa los perfiles y no las tocaba,
    // así que al borrar una conexión la siguiente heredaba su índice —y con él sus
    // datasets cacheados—. Solo se salvaba si esa conexión se refrescaba antes, porque
    // `refreshConnectionByIndex` sí invalida por índice; pero recargar no refresca.
    //
    // Por eso viven aquí: su ciclo de vida es el de la lista de conexiones, y
    // `setProfiles()` se las lleva. La cura de fondo sería indexar por IDENTIFICADOR en
    // vez de por posición, como ya hace `connInfoById`; eso queda pendiente.
    QMap<QString, PoolDatasetCache> poolDatasetCache;
    QMap<QString, PoolDetailsCacheEntry> poolDetailsCache;
    QMap<QString, DatasetPermissionsCacheEntry> datasetPermissionsCache;
    QMap<QString, ConnInfo> connInfoById;
    QVector<PoolListEntry> poolListEntries;

    // Solo las indexadas POR POSICIÓN. `connInfoById` va por identificador, así que no
    // sufre el reindexado; y `poolListEntries` no es una caché sino una lista que
    // reconstruye entera `populateAllPoolsTables()`. Vaciarlas aquí no arreglaría nada y
    // sí metería un vaciado donde nadie lo espera.
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
        // Las cachés van por índice: si la lista cambia, lo cacheado deja de
        // corresponder con quien creía. Tirarlas cuesta una relectura; no tirarlas
        // costaba enseñar los datos de una máquina bajo el nombre de otra.
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
