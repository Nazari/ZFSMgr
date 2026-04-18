#pragma once

#include <QString>

namespace agentversion {

QString currentVersion();
QString expectedApiVersion();
int compareVersions(const QString& a, const QString& b);

} // namespace agentversion

