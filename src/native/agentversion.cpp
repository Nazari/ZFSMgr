#include "agentversion.h"

#include "base/agentversion.h"

// Adaptador. La lógica está en `src/base/agentversion.cpp`, que no depende de Qt; aquí
// solo se convierte en la frontera.
//
// Estaba solo aquí, y el intérprete tenía su propia manera —comparar con `!=` y escribir
// la versión con la que se compiló— porque no podía llamar a esto. Ahora los dos usan la
// misma. Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace BAV = zfsmgr::base::agentversion;

namespace agentversion {

QString currentVersion() { return QString::fromStdString(BAV::laEsperada()); }

QString expectedApiVersion() { return QString::fromStdString(BAV::apiEsperada()); }

int compareVersions(const QString& a, const QString& b) {
    return BAV::compara(a.toStdString(), b.toStdString());
}

QString versionFromBinary(const QString& path) {
    return QString::fromStdString(BAV::versionEnBinario(path.toStdString()));
}

} // namespace agentversion
