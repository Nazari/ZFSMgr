#include "mainwindow.h"
#include "i18nmanager.h"

#include <QSettings>

QString MainWindow::tr3(const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translate(m_language, es, en, zh);
}

void MainWindow::loadUiSettings() {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("ui"));
    const QString langUi = ini.value(QStringLiteral("language"), QString()).toString().trimmed().toLower();
    ini.endGroup();
    ini.beginGroup(QStringLiteral("app"));
    const QString lang = ini.value(QStringLiteral("language"), m_language).toString().trimmed().toLower();
    if (!lang.isEmpty()) {
        m_language = lang;
    } else if (!langUi.isEmpty()) {
        m_language = langUi;
    }
    m_actionConfirmEnabled = ini.value(QStringLiteral("confirm_actions"), true).toBool();
    m_logMaxSizeMb = ini.value(QStringLiteral("log_max_mb"), 10).toInt();
    if (m_logMaxSizeMb < 1) {
        m_logMaxSizeMb = 1;
    } else if (m_logMaxSizeMb > 1024) {
        m_logMaxSizeMb = 1024;
    }
    ini.endGroup();
}

void MainWindow::saveUiSettings() const {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("app"));
    ini.setValue(QStringLiteral("language"), m_language);
    ini.setValue(QStringLiteral("confirm_actions"), m_actionConfirmEnabled);
    ini.setValue(QStringLiteral("log_max_mb"), m_logMaxSizeMb);
    ini.endGroup();
    ini.beginGroup(QStringLiteral("ui"));
    ini.setValue(QStringLiteral("language"), m_language);
    ini.endGroup();
    ini.sync();
}
