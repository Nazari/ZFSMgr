#include "mainwindow.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
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

QString connPersistKeyFromProfiles(const QVector<ConnectionProfile>& profiles, int connIdx) {
    if (connIdx < 0 || connIdx >= profiles.size()) {
        return QString();
    }
    const QString id = profiles[connIdx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return profiles[connIdx].name.trimmed().toLower();
}

void removeLegacyColumnWidthPersistence(QSettings& ini) {
    const QStringList keys = ini.allKeys();
    for (const QString& keyRaw : keys) {
        const QString key = keyRaw.trimmed().toLower();
        // Purga de persistencia legacy de anchos/estado de columnas.
        if (!key.contains(QStringLiteral("conncontent"))
            && !key.contains(QStringLiteral("dataset_tree"))
            && !key.contains(QStringLiteral("tree_columns"))
            && !key.contains(QStringLiteral("table_columns"))) {
            continue;
        }
        if (key.contains(QStringLiteral("column"))
            || key.contains(QStringLiteral("colwidth"))
            || key.contains(QStringLiteral("width"))
            || key.contains(QStringLiteral("header_state"))
            || key.contains(QStringLiteral("headerstate"))) {
            ini.remove(keyRaw);
        }
    }
}

QStringList normalizePropsOrderList(QStringList in) {
    QStringList out;
    QSet<QString> seen;
    for (const QString& raw : in) {
        const QString t = raw.trimmed();
        const QString k = t.toLower();
        if (t.isEmpty() || seen.contains(k)) {
            continue;
        }
        seen.insert(k);
        out.push_back(t);
    }
    return out;
}

QVector<MainWindow::InlinePropGroupConfig> decodeInlinePropGroups(const QString& raw) {
    QVector<MainWindow::InlinePropGroupConfig> out;
    if (raw.trimmed().isEmpty()) {
        return out;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;
    }
    QSet<QString> seenNames;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject obj = v.toObject();
        MainWindow::InlinePropGroupConfig cfg;
        cfg.name = obj.value(QStringLiteral("name")).toString().trimmed();
        const QString key = cfg.name.toLower();
        if (cfg.name.isEmpty() || key == QStringLiteral("__all__") || seenNames.contains(key)) {
            continue;
        }
        seenNames.insert(key);
        for (const QJsonValue& pv : obj.value(QStringLiteral("props")).toArray()) {
            cfg.props.push_back(pv.toString());
        }
        cfg.props = normalizePropsOrderList(cfg.props);
        out.push_back(cfg);
    }
    return out;
}

QString encodeInlinePropGroups(const QVector<MainWindow::InlinePropGroupConfig>& groups) {
    QJsonArray arr;
    for (const MainWindow::InlinePropGroupConfig& cfg : groups) {
        const QString name = cfg.name.trimmed();
        if (name.isEmpty()) {
            continue;
        }
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), name);
        QJsonArray props;
        for (const QString& p : normalizePropsOrderList(cfg.props)) {
            props.push_back(p);
        }
        obj.insert(QStringLiteral("props"), props);
        arr.push_back(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

}

QString MainWindow::trk(const QString& key, const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}

void MainWindow::loadUiSettings() {
    QSettings ini(m_store.iniPath(), QSettings::IniFormat);
    removeLegacyColumnWidthPersistence(ini);
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
    m_showInlineDatasetProps = ini.value(QStringLiteral("show_inline_dataset_props"), true).toBool();
    const bool legacyShowInlinePropertyNodes =
        ini.value(QStringLiteral("show_inline_property_nodes"), true).toBool();
    const bool legacyShowInlinePermissionsNodes =
        ini.value(QStringLiteral("show_inline_permissions_nodes"), true).toBool();
    const bool legacyShowInlineGsaNode =
        ini.value(QStringLiteral("show_inline_gsa_node"), true).toBool();
    m_showInlinePropertyNodesTop =
        ini.value(QStringLiteral("show_inline_property_nodes_top"), legacyShowInlinePropertyNodes).toBool();
    m_showInlinePropertyNodesBottom =
        ini.value(QStringLiteral("show_inline_property_nodes_bottom"), legacyShowInlinePropertyNodes).toBool();
    m_showInlinePermissionsNodesTop =
        ini.value(QStringLiteral("show_inline_permissions_nodes_top"), legacyShowInlinePermissionsNodes).toBool();
    m_showInlinePermissionsNodesBottom =
        ini.value(QStringLiteral("show_inline_permissions_nodes_bottom"), legacyShowInlinePermissionsNodes).toBool();
    m_showInlineGsaNodeTop =
        ini.value(QStringLiteral("show_inline_gsa_node_top"), legacyShowInlineGsaNode).toBool();
    m_showInlineGsaNodeBottom =
        ini.value(QStringLiteral("show_inline_gsa_node_bottom"), legacyShowInlineGsaNode).toBool();
    const bool legacyShowPoolInfoNode =
        ini.value(QStringLiteral("show_pool_info_node"), true).toBool();
    m_showPoolInfoNodeTop =
        ini.value(QStringLiteral("show_pool_info_node_top"), legacyShowPoolInfoNode).toBool();
    m_showPoolInfoNodeBottom =
        ini.value(QStringLiteral("show_pool_info_node_bottom"), legacyShowPoolInfoNode).toBool();
    m_connPropColumnsSetting = ini.value(QStringLiteral("conn_prop_columns"), 4).toInt();
    m_persistedTopDetailConnectionKey =
        ini.value(QStringLiteral("top_detail_connection")).toString().trimmed().toLower();
    m_persistedBottomDetailConnectionKey =
        ini.value(QStringLiteral("bottom_detail_connection")).toString().trimmed().toLower();
    m_datasetInlinePropsOrder = ini.value(QStringLiteral("dataset_inline_props_order")).toStringList();
    m_datasetInlinePropGroups =
        decodeInlinePropGroups(ini.value(QStringLiteral("dataset_inline_prop_groups")).toString());
    m_poolInlinePropsOrder = ini.value(QStringLiteral("pool_inline_props_order")).toStringList();
    m_poolInlinePropGroups =
        decodeInlinePropGroups(ini.value(QStringLiteral("pool_inline_prop_groups")).toString());
    m_snapshotInlinePropsOrder = ini.value(QStringLiteral("snapshot_inline_props_order")).toStringList();
    m_snapshotInlinePropGroups =
        decodeInlinePropGroups(ini.value(QStringLiteral("snapshot_inline_prop_groups")).toString());
    m_datasetInlinePropsOrder = normalizePropsOrderList(m_datasetInlinePropsOrder);
    m_poolInlinePropsOrder = normalizePropsOrderList(m_poolInlinePropsOrder);
    m_snapshotInlinePropsOrder = normalizePropsOrderList(m_snapshotInlinePropsOrder);
    m_disconnectedConnectionKeys.clear();
    {
        const QStringList raw = ini.value(QStringLiteral("disconnected_connections")).toStringList();
        for (const QString& k : raw) {
            const QString norm = k.trimmed().toLower();
            if (!norm.isEmpty()) {
                m_disconnectedConnectionKeys.insert(norm);
            }
        }
    }
    m_mainWindowGeometryState = ini.value(QStringLiteral("main_window_geometry")).toByteArray();
    m_topMainSplitState = ini.value(QStringLiteral("top_main_splitter")).toByteArray();
    m_rightMainSplitState = ini.value(QStringLiteral("right_main_splitter")).toByteArray();
    m_verticalMainSplitState = ini.value(QStringLiteral("vertical_main_splitter")).toByteArray();
    m_bottomInfoSplitState = ini.value(QStringLiteral("bottom_info_splitter")).toByteArray();
    if (m_logMaxLinesSetting != 100 && m_logMaxLinesSetting != 200
        && m_logMaxLinesSetting != 500 && m_logMaxLinesSetting != 1000) {
        m_logMaxLinesSetting = 500;
    }
    if (m_logMaxSizeMb < 1) {
        m_logMaxSizeMb = 1;
    } else if (m_logMaxSizeMb > 1024) {
        m_logMaxSizeMb = 1024;
    }
    m_connPropColumnsSetting = qBound(4, m_connPropColumnsSetting, 16);
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
    ini.setValue(QStringLiteral("show_inline_dataset_props"), m_showInlineDatasetProps);
    const bool showInlinePropertyNodesTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlineProperties
                         : m_showInlinePropertyNodesTop;
    const bool showInlinePermissionsNodesTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlinePermissions
                         : m_showInlinePermissionsNodesTop;
    const bool showInlineGsaNodeTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlineGsa
                         : m_showInlineGsaNodeTop;
    const bool showPoolInfoNodeTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showPoolInfo
                         : m_showPoolInfoNodeTop;
    ini.setValue(QStringLiteral("show_inline_property_nodes_top"), showInlinePropertyNodesTop);
    ini.setValue(QStringLiteral("show_inline_permissions_nodes_top"), showInlinePermissionsNodesTop);
    ini.setValue(QStringLiteral("show_inline_gsa_node_top"), showInlineGsaNodeTop);
    ini.setValue(QStringLiteral("show_pool_info_node_top"), showPoolInfoNodeTop);
    ini.setValue(QStringLiteral("conn_prop_columns"), qBound(4, m_connPropColumnsSetting, 16));
    ini.setValue(QStringLiteral("top_detail_connection"),
                 connPersistKeyFromProfiles(m_profiles, m_topDetailConnIdx));
    ini.setValue(QStringLiteral("dataset_inline_props_order"), m_datasetInlinePropsOrder);
    ini.setValue(QStringLiteral("dataset_inline_prop_groups"), encodeInlinePropGroups(m_datasetInlinePropGroups));
    ini.setValue(QStringLiteral("pool_inline_props_order"), m_poolInlinePropsOrder);
    ini.setValue(QStringLiteral("pool_inline_prop_groups"), encodeInlinePropGroups(m_poolInlinePropGroups));
    ini.setValue(QStringLiteral("snapshot_inline_props_order"), m_snapshotInlinePropsOrder);
    ini.setValue(QStringLiteral("snapshot_inline_prop_groups"), encodeInlinePropGroups(m_snapshotInlinePropGroups));
    QStringList disconnected = QStringList(m_disconnectedConnectionKeys.begin(), m_disconnectedConnectionKeys.end());
    disconnected.sort(Qt::CaseInsensitive);
    ini.setValue(QStringLiteral("disconnected_connections"), disconnected);
    ini.setValue(QStringLiteral("main_window_geometry"), saveGeometry());
    ini.setValue(QStringLiteral("top_main_splitter"),
                 m_topMainSplit ? m_topMainSplit->saveState() : QByteArray());
    ini.setValue(QStringLiteral("right_main_splitter"),
                 m_rightMainSplit ? m_rightMainSplit->saveState() : QByteArray());
    ini.setValue(QStringLiteral("vertical_main_splitter"),
                 m_verticalMainSplit ? m_verticalMainSplit->saveState() : QByteArray());
    ini.setValue(QStringLiteral("bottom_info_splitter"),
                 m_bottomInfoSplit ? m_bottomInfoSplit->saveState() : QByteArray());
    ini.endGroup();
    // Remove legacy duplicated key.
    ini.beginGroup(QStringLiteral("ui"));
    ini.remove(QStringLiteral("language"));
    ini.endGroup();
    removeLegacyColumnWidthPersistence(ini);
    ini.sync();
}

void MainWindow::applyLanguageLive() {
    QString selectedConnId;
    const int selectedConnIdx = currentConnectionIndexFromUi();
    if (selectedConnIdx >= 0 && selectedConnIdx < m_profiles.size()) {
        selectedConnId = m_profiles[selectedConnIdx].id;
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
    m_connectionGsaLogViews.clear();
    m_connectionLogTabs.clear();
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

    if (!selectedConnId.isEmpty()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id.trimmed().compare(selectedConnId, Qt::CaseInsensitive) == 0) {
                setCurrentConnectionInUi(i);
                break;
            }
        }
    }
    updateConnectionActionsState();
}
