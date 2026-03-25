#pragma once

#include "connectionstore.h"

#include <QString>
#include <QStringList>

namespace helperinstall {

struct PlatformInfo {
    QString platformId;
    QString platformLabel;
    QString packageManagerId;
    QString packageManagerLabel;
    bool supportedByDesign{false};
    bool windowsUsesMsys2{false};
    QString reason;
};

struct InstallPlan {
    bool supported{false};
    QString platformId;
    QString platformLabel;
    QString packageManagerId;
    QString packageManagerLabel;
    QStringList requestedCommands;
    QStringList supportedCommands;
    QStringList unsupportedCommands;
    QStringList packages;
    QString commandPreview;
    QStringList warnings;
};

QStringList trackedInstallableCommands();
PlatformInfo detectPlatform(const ConnectionProfile& profile, const QString& osLine);
InstallPlan buildInstallPlan(const PlatformInfo& platform, const QStringList& missingCommands, bool useSudo);

} // namespace helperinstall
