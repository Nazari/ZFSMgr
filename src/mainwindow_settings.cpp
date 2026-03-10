#include "mainwindow.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTableWidgetItem>
#include <QTextEdit>

namespace {
int connectionIndexFromTable(const QTableWidget* table) {
    if (!table) {
        return -1;
    }
    const int row = table->currentRow();
    if (row < 0 || row >= table->rowCount()) {
        return -1;
    }
    const QTableWidgetItem* it = table->item(row, 0);
    if (!it) {
        return -1;
    }
    bool ok = false;
    const int idx = it->data(Qt::UserRole).toInt(&ok);
    return ok ? idx : -1;
}

int rowForConnectionId(const QTableWidget* table, const QVector<ConnectionProfile>& profiles, const QString& id) {
    if (!table || id.isEmpty()) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem* it = table->item(row, 0);
        if (!it) {
            continue;
        }
        bool ok = false;
        const int connIdx = it->data(Qt::UserRole).toInt(&ok);
        if (!ok || connIdx < 0 || connIdx >= profiles.size()) {
            continue;
        }
        if (profiles[connIdx].id == id) {
            return row;
        }
    }
    return -1;
}
}

QString MainWindow::trk(const QString& key, const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}

void MainWindow::loadUiSettings() {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    // Legacy support: [ui]language (migrated to [app]language)
    ini.beginGroup(QStringLiteral("ui"));
    const QString langUi = ini.value(QStringLiteral("language"), QString()).toString().trimmed().toLower();
    ini.endGroup();
    ini.beginGroup(QStringLiteral("app"));
    QString lang = ini.value(QStringLiteral("language"), QString()).toString().trimmed().toLower();
    if (lang.isEmpty() && !langUi.isEmpty()) {
        // One-time migration path from legacy key.
        lang = langUi;
        ini.setValue(QStringLiteral("language"), lang);
    }
    if (!lang.isEmpty()) {
        m_language = lang;
    }
    m_actionConfirmEnabled = ini.value(QStringLiteral("confirm_actions"), true).toBool();
    m_logMaxSizeMb = ini.value(QStringLiteral("log_max_mb"), 10).toInt();
    m_logLevelSetting = ini.value(QStringLiteral("log_level"), QStringLiteral("normal")).toString().trimmed().toLower();
    if (m_logLevelSetting != QStringLiteral("normal")
        && m_logLevelSetting != QStringLiteral("info")
        && m_logLevelSetting != QStringLiteral("debug")) {
        m_logLevelSetting = QStringLiteral("normal");
    }
    m_logMaxLinesSetting = ini.value(QStringLiteral("log_max_lines"), 500).toInt();
    if (m_logMaxLinesSetting != 100 && m_logMaxLinesSetting != 200
        && m_logMaxLinesSetting != 500 && m_logMaxLinesSetting != 1000) {
        m_logMaxLinesSetting = 500;
    }
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
    const QString level = m_logLevelSetting.trimmed().toLower();
    ini.setValue(QStringLiteral("log_level"), level.isEmpty() ? QStringLiteral("normal") : level);
    int lines = m_logMaxLinesSetting;
    if (lines != 100 && lines != 200 && lines != 500 && lines != 1000) {
        lines = 500;
    }
    ini.setValue(QStringLiteral("log_max_lines"), lines);
    ini.endGroup();
    // Remove legacy duplicated key.
    ini.beginGroup(QStringLiteral("ui"));
    ini.remove(QStringLiteral("language"));
    ini.endGroup();
    ini.sync();
}

void MainWindow::applyLanguageLive() {
    const int leftTabIndex = m_leftTabs ? m_leftTabs->currentIndex() : 0;
    const QString originPool = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    const QString destPool = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();

    QString selectedConnId;
    if (m_connectionsTable) {
        const int idx = connectionIndexFromTable(m_connectionsTable);
        if (idx >= 0 && idx < m_profiles.size()) {
            selectedConnId = m_profiles[idx].id;
        }
    }

    const QString appLogText = m_logView ? m_logView->toPlainText() : QString();
    QMap<QString, QString> connectionLogs;
    for (auto it = m_connectionLogViews.constBegin(); it != m_connectionLogViews.constEnd(); ++it) {
        if (it.value()) {
            connectionLogs.insert(it.key(), it.value()->toPlainText());
        }
    }
    const QString statusText = m_statusText ? m_statusText->toPlainText() : QString();
    const QString detailText = m_lastDetailText ? m_lastDetailText->toPlainText() : QString();

    m_connectionLogViews.clear();
    if (QWidget* old = takeCentralWidget()) {
        old->deleteLater();
    }
    if (menuBar()) {
        menuBar()->clear();
    }

    buildUi();
    loadConnections();

    if (m_logView) {
        m_logView->setPlainText(appLogText);
        trimLogWidget(m_logView);
    }
    for (auto it = connectionLogs.constBegin(); it != connectionLogs.constEnd(); ++it) {
        QPlainTextEdit* view = m_connectionLogViews.value(it.key(), nullptr);
        if (!view) {
            continue;
        }
        view->setPlainText(it.value());
        trimLogWidget(view);
    }
    if (m_statusText) {
        m_statusText->setPlainText(statusText);
    }
    if (m_lastDetailText) {
        m_lastDetailText->setPlainText(detailText);
    }

    auto restoreCombo = [](QComboBox* combo, const QString& token) {
        if (!combo || token.isEmpty()) {
            return;
        }
        const int idx = combo->findData(token);
        if (idx >= 0) {
            combo->setCurrentIndex(idx);
        }
    };
    restoreCombo(m_originPoolCombo, originPool);
    restoreCombo(m_destPoolCombo, destPool);

    if (!selectedConnId.isEmpty() && m_connectionsTable) {
        const int row = rowForConnectionId(m_connectionsTable, m_profiles, selectedConnId);
        if (row >= 0) {
            m_connectionsTable->setCurrentCell(row, 0);
        }
    }
    if (m_leftTabs) {
        const int idx = qBound(0, leftTabIndex, m_leftTabs->count() - 1);
        m_leftTabs->setCurrentIndex(idx);
    }

    refreshTransferSelectionLabels();
}
