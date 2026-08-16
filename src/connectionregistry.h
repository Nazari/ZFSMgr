#pragma once

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

    int size() const { return profiles.size(); }
    bool indexOk(int i) const { return i >= 0 && i < profiles.size(); }

    // El ÚNICO camino para cambiar el número de conexiones. Los estados se ajustan al
    // nuevo tamaño; los que sobreviven conserva quien llame, que es quien sabe emparejar
    // por identificador o por nombre.
    void setProfiles(QVector<ConnectionProfile> nuevos) {
        profiles = std::move(nuevos);
        states.clear();
        states.resize(profiles.size());
    }

    void clear() {
        profiles.clear();
        states.clear();
    }

    // Añade una conexión con su estado, manteniendo los dos vectores a la par.
    void append(const ConnectionProfile& p, const ConnectionRuntimeState& s) {
        profiles.push_back(p);
        states.push_back(s);
    }
};
