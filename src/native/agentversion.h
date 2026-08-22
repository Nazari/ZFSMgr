#pragma once

#include <QString>

namespace agentversion {

QString currentVersion();
// Versión que declara un binario de agente, leída del FICHERO. Hace falta leerla así
// porque el agente empaquetado suele ser de otra plataforma y no se puede ejecutar aquí.
QString versionFromBinary(const QString& path);
QString expectedApiVersion();
int compareVersions(const QString& a, const QString& b);

} // namespace agentversion

