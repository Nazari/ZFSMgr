#include "mainwindow.h"
#include "i18nmanager.h"

#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QPlainTextEdit>
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

QStringList jsonStringList(const QJsonObject& obj, const QString& key) {
    QStringList out;
    const QJsonValue v = obj.value(key);
    if (v.isArray()) {
        for (const QJsonValue& item : v.toArray()) {
            out.push_back(item.toString());
        }
    }
    return out;
}

QJsonArray toJsonArray(const QStringList& list) {
    QJsonArray out;
    for (const QString& s : list) {
        out.push_back(s);
    }
    return out;
}

QByteArray decodeBase64State(const QJsonObject& obj, const QString& key) {
    return QByteArray::fromBase64(obj.value(key).toString().toUtf8());
}

}

QString MainWindow::trk(const QString& key, const QString& es, const QString& en, const QString& zh) const {
    return I18nManager::instance().translateKey(m_language, key, es, en, zh);
}

void MainWindow::loadUiSettings() {
    const QJsonObject root = m_store.loadConfigJson();
    const QJsonObject uiObj = root.value(QStringLiteral("ui")).toObject();
    QJsonObject appObj = root.value(QStringLiteral("app")).toObject();
    const QString langUi = uiObj.value(QStringLiteral("language")).toString().trimmed().toLower();
    QString lang = appObj.value(QStringLiteral("language")).toString().trimmed().toLower();
    if (lang.isEmpty() && !langUi.isEmpty()) {
        lang = langUi;
        appObj.insert(QStringLiteral("language"), lang);
    }
    if (!lang.isEmpty()) {
        m_language = lang;
    }
    m_actionConfirmEnabled = appObj.value(QStringLiteral("confirm_actions")).toBool(true);
    m_logMaxSizeMb = appObj.value(QStringLiteral("log_max_mb")).toInt(10);
    m_logLevelSetting = appObj.value(QStringLiteral("log_level")).toString(QStringLiteral("normal")).trimmed().toLower();
    if (m_logLevelSetting != QStringLiteral("normal")
        && m_logLevelSetting != QStringLiteral("info")
        && m_logLevelSetting != QStringLiteral("debug")) {
        m_logLevelSetting = QStringLiteral("normal");
    }
    m_logMaxLinesSetting = appObj.value(QStringLiteral("log_max_lines")).toInt(500);
    m_showInlineDatasetProps = appObj.value(QStringLiteral("show_inline_dataset_props")).toBool(true);
    const bool legacyShowInlinePropertyNodes = appObj.value(QStringLiteral("show_inline_property_nodes")).toBool(true);
    const bool legacyShowInlinePermissionsNodes = appObj.value(QStringLiteral("show_inline_permissions_nodes")).toBool(true);
    const bool legacyShowInlineGsaNode = appObj.value(QStringLiteral("show_inline_gsa_node")).toBool(true);
    m_showInlinePropertyNodesTop =
        appObj.value(QStringLiteral("show_inline_property_nodes_top")).toBool(legacyShowInlinePropertyNodes);
    m_showInlinePropertyNodesBottom =
        appObj.value(QStringLiteral("show_inline_property_nodes_bottom")).toBool(legacyShowInlinePropertyNodes);
    m_showInlinePermissionsNodesTop =
        appObj.value(QStringLiteral("show_inline_permissions_nodes_top")).toBool(legacyShowInlinePermissionsNodes);
    m_showInlinePermissionsNodesBottom =
        appObj.value(QStringLiteral("show_inline_permissions_nodes_bottom")).toBool(legacyShowInlinePermissionsNodes);
    m_showInlineGsaNodeTop =
        appObj.value(QStringLiteral("show_inline_gsa_node_top")).toBool(legacyShowInlineGsaNode);
    m_showInlineGsaNodeBottom =
        appObj.value(QStringLiteral("show_inline_gsa_node_bottom")).toBool(legacyShowInlineGsaNode);
    m_showPoolInfoNodeTop = true;
    m_showPoolInfoNodeBottom = true;
    m_connPropColumnsSetting = appObj.value(QStringLiteral("conn_prop_columns")).toInt(4);
    m_persistedTopDetailConnectionKey =
        appObj.value(QStringLiteral("top_detail_connection")).toString().trimmed().toLower();
    m_persistedBottomDetailConnectionKey =
        appObj.value(QStringLiteral("bottom_detail_connection")).toString().trimmed().toLower();
    m_datasetInlinePropsOrder = jsonStringList(appObj, QStringLiteral("dataset_inline_props_order"));
    m_datasetInlinePropGroups =
        decodeInlinePropGroups(appObj.value(QStringLiteral("dataset_inline_prop_groups")).toString());
    m_poolInlinePropsOrder = jsonStringList(appObj, QStringLiteral("pool_inline_props_order"));
    m_poolInlinePropGroups =
        decodeInlinePropGroups(appObj.value(QStringLiteral("pool_inline_prop_groups")).toString());
    m_snapshotInlineVisibleProps = jsonStringList(appObj, QStringLiteral("snapshot_inline_visible_props"));
    m_snapshotInlinePropGroups =
        decodeInlinePropGroups(appObj.value(QStringLiteral("snapshot_inline_prop_groups")).toString());
    m_datasetInlinePropsOrder = normalizePropsOrderList(m_datasetInlinePropsOrder);
    m_poolInlinePropsOrder = normalizePropsOrderList(m_poolInlinePropsOrder);
    m_snapshotInlineVisibleProps = normalizePropsOrderList(m_snapshotInlineVisibleProps);
    if (m_snapshotInlineVisibleProps.isEmpty()) {
        // Migración legacy desde snapshot_inline_props_order (retirado).
        QStringList legacy = jsonStringList(appObj, QStringLiteral("snapshot_inline_props_order"));
        legacy = normalizePropsOrderList(legacy);
        for (int i = legacy.size() - 1; i >= 0; --i) {
            if (legacy.at(i).trimmed().compare(QStringLiteral("snapshot"), Qt::CaseInsensitive) == 0) {
                legacy.removeAt(i);
            }
        }
        m_snapshotInlineVisibleProps = legacy;
    }
    m_disconnectedConnectionKeys.clear();
    {
        const QStringList raw = jsonStringList(appObj, QStringLiteral("disconnected_connections"));
        for (const QString& k : raw) {
            const QString norm = k.trimmed().toLower();
            if (!norm.isEmpty()) {
                m_disconnectedConnectionKeys.insert(norm);
            }
        }
    }
    m_mainWindowGeometryState = decodeBase64State(appObj, QStringLiteral("main_window_geometry"));
    m_topMainSplitState = decodeBase64State(appObj, QStringLiteral("top_main_splitter"));
    m_rightMainSplitState = decodeBase64State(appObj, QStringLiteral("right_main_splitter"));
    m_verticalMainSplitState = decodeBase64State(appObj, QStringLiteral("vertical_main_splitter"));
    m_bottomInfoSplitState = decodeBase64State(appObj, QStringLiteral("bottom_info_splitter"));
    m_splitTreeLayoutState = appObj.value(QStringLiteral("split_tree_layout")).toString();
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
}

void MainWindow::saveUiSettings() const {
    QString jsonErr;
    QJsonObject root = m_store.loadConfigJson(&jsonErr);
    QJsonObject appObj = root.value(QStringLiteral("app")).toObject();
    appObj.insert(QStringLiteral("language"), m_language);
    appObj.insert(QStringLiteral("confirm_actions"), m_actionConfirmEnabled);
    appObj.insert(QStringLiteral("log_max_mb"), m_logMaxSizeMb);
    const QString level = m_logLevelSetting.trimmed().toLower();
    appObj.insert(QStringLiteral("log_level"), level.isEmpty() ? QStringLiteral("normal") : level);
    int lines = m_logMaxLinesSetting;
    if (lines != 100 && lines != 200 && lines != 500 && lines != 1000) {
        lines = 500;
    }
    appObj.insert(QStringLiteral("log_max_lines"), lines);
    appObj.insert(QStringLiteral("show_inline_dataset_props"), m_showInlineDatasetProps);
    const bool showInlinePropertyNodesTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlineProperties
                         : m_showInlinePropertyNodesTop;
    const bool showInlinePermissionsNodesTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlinePermissions
                         : m_showInlinePermissionsNodesTop;
    const bool showInlineGsaNodeTop =
        m_topDatasetPane ? m_topDatasetPane->visualOptions().showInlineGsa
                         : m_showInlineGsaNodeTop;
    appObj.insert(QStringLiteral("show_inline_property_nodes_top"), showInlinePropertyNodesTop);
    appObj.insert(QStringLiteral("show_inline_permissions_nodes_top"), showInlinePermissionsNodesTop);
    appObj.insert(QStringLiteral("show_inline_gsa_node_top"), showInlineGsaNodeTop);
    appObj.insert(QStringLiteral("conn_prop_columns"), qBound(4, m_connPropColumnsSetting, 16));
    appObj.insert(QStringLiteral("top_detail_connection"),
                  connPersistKeyFromProfiles(m_profiles, m_topDetailConnIdx));
    appObj.insert(QStringLiteral("dataset_inline_props_order"), toJsonArray(m_datasetInlinePropsOrder));
    appObj.insert(QStringLiteral("dataset_inline_prop_groups"), encodeInlinePropGroups(m_datasetInlinePropGroups));
    appObj.insert(QStringLiteral("pool_inline_props_order"), toJsonArray(m_poolInlinePropsOrder));
    appObj.insert(QStringLiteral("pool_inline_prop_groups"), encodeInlinePropGroups(m_poolInlinePropGroups));
    appObj.insert(QStringLiteral("snapshot_inline_visible_props"), toJsonArray(m_snapshotInlineVisibleProps));
    appObj.insert(QStringLiteral("snapshot_inline_prop_groups"), encodeInlinePropGroups(m_snapshotInlinePropGroups));
    appObj.remove(QStringLiteral("snapshot_inline_props_order"));
    QStringList disconnected = QStringList(m_disconnectedConnectionKeys.begin(), m_disconnectedConnectionKeys.end());
    disconnected.sort(Qt::CaseInsensitive);
    appObj.insert(QStringLiteral("disconnected_connections"), toJsonArray(disconnected));
    appObj.insert(QStringLiteral("main_window_geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    appObj.insert(QStringLiteral("top_main_splitter"),
                  QString::fromLatin1((m_topMainSplit ? m_topMainSplit->saveState() : QByteArray()).toBase64()));
    appObj.insert(QStringLiteral("right_main_splitter"),
                  QString::fromLatin1((m_rightMainSplit ? m_rightMainSplit->saveState() : QByteArray()).toBase64()));
    appObj.insert(QStringLiteral("vertical_main_splitter"),
                  QString::fromLatin1((m_verticalMainSplit ? m_verticalMainSplit->saveState() : QByteArray()).toBase64()));
    appObj.insert(QStringLiteral("bottom_info_splitter"),
                  QString::fromLatin1((m_bottomInfoSplit ? m_bottomInfoSplit->saveState() : QByteArray()).toBase64()));
    appObj.insert(QStringLiteral("split_tree_layout"), serializeSplitTreeLayoutState());
    root.insert(QStringLiteral("app"), appObj);
    QJsonObject uiObj = root.value(QStringLiteral("ui")).toObject();
    uiObj.remove(QStringLiteral("language"));
    root.insert(QStringLiteral("ui"), uiObj);
    m_store.saveConfigJson(root, &jsonErr);
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
