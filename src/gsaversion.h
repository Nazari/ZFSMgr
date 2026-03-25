#pragma once

#include <QString>

namespace gsaversion {

QString unixTemplate();
QString windowsTemplate();
QString currentVersion();
int compareVersions(const QString& a, const QString& b);

} // namespace gsaversion
