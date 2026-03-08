#include "mainwindow.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTreeWidgetItem>

QString MainWindow::trk(const QString& key, const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
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
    ini.beginGroup(QStringLiteral("ui"));
    ini.setValue(QStringLiteral("language"), m_language);
    ini.endGroup();
    ini.sync();
}

void MainWindow::applyLanguageLive() {
    const int leftTabIndex = m_leftTabs ? m_leftTabs->currentIndex() : 0;
    const QString originPool = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    const QString destPool = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
    const QString advPool = m_advPoolCombo ? m_advPoolCombo->currentData().toString() : QString();

    QString selectedConnId;
    if (m_connectionsList) {
        const auto selected = m_connectionsList->selectedItems();
        if (!selected.isEmpty()) {
            QTreeWidgetItem* item = selected.first();
            while (item && item->parent()) {
                item = item->parent();
            }
            const int idx = item ? item->data(0, Qt::UserRole).toInt() : -1;
            if (idx >= 0 && idx < m_profiles.size()) {
                selectedConnId = m_profiles[idx].id;
            }
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
    restoreCombo(m_advPoolCombo, advPool);

    if (!selectedConnId.isEmpty() && m_connectionsList) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id != selectedConnId) {
                continue;
            }
            if (QTreeWidgetItem* top = m_connectionsList->topLevelItem(i)) {
                m_connectionsList->setCurrentItem(top);
            }
            break;
        }
    }
    if (m_leftTabs) {
        const int idx = qBound(0, leftTabIndex, m_leftTabs->count() - 1);
        m_leftTabs->setCurrentIndex(idx);
    }

    refreshTransferSelectionLabels();
    updateAdvancedSelectionUi(m_advPropsDataset, QString());
    updateStatus(trk(QStringLiteral("t_lang_applied_001"),
                     QStringLiteral("Estado: idioma aplicado"),
                     QStringLiteral("Status: language applied"),
                     QStringLiteral("状态：语言已应用")));
}
