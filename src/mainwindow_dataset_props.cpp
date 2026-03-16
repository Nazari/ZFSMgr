#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QAbstractScrollArea>
#include <QComboBox>
#include <QFont>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWheelEvent>

#include <cmath>
#include <functional>

namespace {
constexpr int kPropKeyRole = Qt::UserRole + 777;
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kPoolNameRole = Qt::UserRole + 11;

class PinnedSortItem final : public QTableWidgetItem {
public:
    explicit PinnedSortItem(const QString& text = QString()) : QTableWidgetItem(text) {}
    bool operator<(const QTableWidgetItem& other) const override {
        constexpr int kPinRole = Qt::UserRole + 501;
        const int a = data(kPinRole).toInt();
        const int b = other.data(kPinRole).toInt();
        Qt::SortOrder order = Qt::AscendingOrder;
        if (const QTableWidget* t = tableWidget()) {
            order = static_cast<Qt::SortOrder>(t->property("sort_order").toInt());
        }
        const bool aPinned = (a >= 0);
        const bool bPinned = (b >= 0);
        if (aPinned || bPinned) {
            if (aPinned && bPinned) {
                if (a != b) {
                    return (order == Qt::DescendingOrder) ? (a > b) : (a < b);
                }
            } else {
                return (order == Qt::DescendingOrder) ? (!aPinned) : aPinned;
            }
        }
        return QTableWidgetItem::operator<(other);
    }
};

class NoWheelComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent* event) override {
        QWidget* p = parentWidget();
        while (p) {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(p)) {
                QWheelEvent forwarded(
                    event->position(),
                    event->globalPosition(),
                    event->pixelDelta(),
                    event->angleDelta(),
                    event->buttons(),
                    event->modifiers(),
                    event->phase(),
                    event->inverted(),
                    event->source());
                QApplication::sendEvent(area->viewport(), &forwarded);
                event->accept();
                return;
            }
            p = p->parentWidget();
        }
        event->ignore();
    }
};

bool isUserProperty(const QString& prop) {
    return prop.contains(':');
}

enum class DatasetPlatformFamily {
    Linux,
    MacOS,
    FreeBSD,
    Windows,
    Other,
};

DatasetPlatformFamily datasetPlatformFamilyFromStrings(const QString& osType, const QString& osLine) {
    const QString merged = (osType + QStringLiteral(" ") + osLine).trimmed().toLower();
    if (merged.contains(QStringLiteral("windows"))) {
        return DatasetPlatformFamily::Windows;
    }
    if (merged.contains(QStringLiteral("darwin")) || merged.contains(QStringLiteral("mac"))) {
        return DatasetPlatformFamily::MacOS;
    }
    if (merged.contains(QStringLiteral("freebsd"))) {
        return DatasetPlatformFamily::FreeBSD;
    }
    if (merged.contains(QStringLiteral("linux"))) {
        return DatasetPlatformFamily::Linux;
    }
    return DatasetPlatformFamily::Other;
}

bool isDatasetPropertySupportedOnPlatform(const QString& propName, DatasetPlatformFamily platform) {
    const QString prop = propName.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (prop == QStringLiteral("vscan")) {
        return false;
    }
    if (prop == QStringLiteral("jailed")) {
        return platform == DatasetPlatformFamily::FreeBSD;
    }
    if (prop == QStringLiteral("zoned")) {
        return platform == DatasetPlatformFamily::Linux;
    }
    if (prop == QStringLiteral("sharesmb")) {
        return platform != DatasetPlatformFamily::MacOS;
    }
    if (prop == QStringLiteral("nbmand")) {
        return platform == DatasetPlatformFamily::Linux;
    }
    return true;
}

bool isDatasetPropertyEditable(const QString& propName,
                               const QString& datasetType,
                               const QString& source,
                               const QString& readonly,
                               DatasetPlatformFamily platform) {
    const QString prop = propName.trimmed().toLower();
    const QString dsType = datasetType.trimmed().toLower();
    const QString src = source.trimmed();
    const QString ro = readonly.trimmed().toLower();
    if (prop.isEmpty()) {
        return false;
    }
    if (!isDatasetPropertySupportedOnPlatform(propName, platform)) {
        return false;
    }
    if (ro == QStringLiteral("true") || ro == QStringLiteral("on") || ro == QStringLiteral("yes") || ro == QStringLiteral("1")) {
        return false;
    }
    if (src == QStringLiteral("-")) {
        return false;
    }
    if (isUserProperty(prop)) {
        return true;
    }

    static const QSet<QString> common = {
        QStringLiteral("atime"), QStringLiteral("relatime"), QStringLiteral("readonly"), QStringLiteral("compression"),
        QStringLiteral("checksum"), QStringLiteral("sync"), QStringLiteral("logbias"), QStringLiteral("primarycache"),
        QStringLiteral("secondarycache"), QStringLiteral("dedup"), QStringLiteral("copies"), QStringLiteral("acltype"),
        QStringLiteral("aclinherit"), QStringLiteral("xattr"), QStringLiteral("normalization"),
        QStringLiteral("casesensitivity"), QStringLiteral("utf8only"), QStringLiteral("keylocation"), QStringLiteral("comment")
    };
    static const QSet<QString> fs = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("mountpoint"), QStringLiteral("canmount"), QStringLiteral("recordsize"), QStringLiteral("quota"),
            QStringLiteral("reservation"), QStringLiteral("refquota"), QStringLiteral("refreservation"),
            QStringLiteral("snapdir"), QStringLiteral("exec"), QStringLiteral("setuid"), QStringLiteral("devices"),
            QStringLiteral("driveletter"), QStringLiteral("sharesmb"), QStringLiteral("sharenfs"),
            QStringLiteral("nbmand"), QStringLiteral("overlay"), QStringLiteral("jailed"),
            QStringLiteral("zoned")
        });
        return s;
    }();
    static const QSet<QString> vol = []() {
        QSet<QString> s = common;
        s.unite(QSet<QString>{
            QStringLiteral("volsize"), QStringLiteral("volblocksize"), QStringLiteral("reservation"),
            QStringLiteral("refreservation"), QStringLiteral("snapdev"), QStringLiteral("volmode")
        });
        return s;
    }();

    if (dsType == QStringLiteral("filesystem")) {
        return fs.contains(prop);
    }
    if (dsType == QStringLiteral("volume")) {
        return vol.contains(prop);
    }
    if (dsType == QStringLiteral("snapshot")) {
        return false;
    }
    return fs.contains(prop) || vol.contains(prop);
}

bool isDatasetPropertyInheritable(const QString& propName,
                                  const QString& datasetType,
                                  const QString& source,
                                  const QString& readonly,
                                  DatasetPlatformFamily platform) {
    const QString prop = propName.trimmed().toLower();
    if (prop.isEmpty()
        || prop == QStringLiteral("dataset")
        || prop == QStringLiteral("tamaño")
        || prop == QStringLiteral("estado")
        || prop == QStringLiteral("snapshot")) {
        return false;
    }
    return isDatasetPropertyEditable(propName, datasetType, source, readonly, platform);
}

bool isDatasetPropertyCurrentlyInherited(const QString& source) {
    const QString src = source.trimmed().toLower();
    if (src.isEmpty() || src == QStringLiteral("-")) {
        return false;
    }
    if (src == QStringLiteral("local")
        || src == QStringLiteral("default")
        || src == QStringLiteral("received")
        || src == QStringLiteral("temporary")) {
        return false;
    }
    return src.startsWith(QStringLiteral("inherited"));
}

using mwhelpers::shSingleQuote;

bool parseSizeToBytes(const QString& input, double& bytesOut) {
    const QString s = input.trimmed();
    if (s.isEmpty()) {
        return false;
    }
    bool ok = false;
    const qint64 rawBytes = s.toLongLong(&ok);
    if (ok) {
        bytesOut = static_cast<double>(rawBytes);
        return true;
    }

    const QRegularExpression rx(QStringLiteral("^\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?)(?:i?B)?\\s*$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(s);
    if (!m.hasMatch()) {
        return false;
    }
    const double value = m.captured(1).toDouble(&ok);
    if (!ok) {
        return false;
    }
    const QString unit = m.captured(2).toUpper();
    int power = 0;
    if (unit == QStringLiteral("K")) {
        power = 1;
    } else if (unit == QStringLiteral("M")) {
        power = 2;
    } else if (unit == QStringLiteral("G")) {
        power = 3;
    } else if (unit == QStringLiteral("T")) {
        power = 4;
    } else if (unit == QStringLiteral("P")) {
        power = 5;
    } else if (unit == QStringLiteral("E")) {
        power = 6;
    }
    bytesOut = value * std::pow(1024.0, power);
    return std::isfinite(bytesOut);
}

QString formatDatasetSize(const QString& rawUsed) {
    double bytes = 0.0;
    if (!parseSizeToBytes(rawUsed, bytes)) {
        return rawUsed.trimmed();
    }
    if (bytes < 0.0) {
        bytes = 0.0;
    }

    static const QStringList units = {
        QStringLiteral("B"),
        QStringLiteral("KB"),
        QStringLiteral("MB"),
        QStringLiteral("GB"),
        QStringLiteral("TB"),
    };
    int unitIndex = 0;
    double value = bytes;
    while (value >= 1024.0 && unitIndex < units.size() - 1) {
        value /= 1024.0;
        ++unitIndex;
    }

    int integerDigits = 1;
    if (value >= 1.0) {
        integerDigits = static_cast<int>(std::floor(std::log10(value))) + 1;
    }
    int decimals = qMax(0, 4 - integerDigits);
    decimals = qMin(decimals, 3);
    if (unitIndex == 0) {
        decimals = 0;
    }

    const QLocale locale = QLocale::system();
    QString number = locale.toString(value, 'f', decimals);
    if (decimals > 0) {
        const QString decimalPoint = locale.decimalPoint();
        while (number.endsWith(QLatin1Char('0'))) {
            number.chop(1);
        }
        if (number.endsWith(decimalPoint)) {
            number.chop(decimalPoint.size());
        }
    }
    return QStringLiteral("%1 %2").arg(number, units[unitIndex]);
}

QString propKeyFromItem(const QTableWidgetItem* item) {
    if (!item) {
        return QString();
    }
    const QString fromRole = item->data(kPropKeyRole).toString().trimmed();
    if (!fromRole.isEmpty()) {
        return fromRole;
    }
    return item->text().trimmed();
}

QString propsDraftKey(const QString& side, const QString& token, const QString& objectName) {
    return QStringLiteral("%1|%2|%3")
        .arg(side.trimmed().toLower(),
             token.trimmed().toLower(),
             objectName.trimmed().toLower());
}

} // namespace

QString MainWindow::datasetPropsCachePrefix(int connIdx, const QString& poolName) const {
    const QString poolKey = datasetCacheKey(connIdx, poolName);
    if (poolKey.isEmpty()) {
        return QString();
    }
    return poolKey + QStringLiteral("::");
}

QString MainWindow::datasetPropsCacheKey(int connIdx, const QString& poolName, const QString& objectName) const {
    const QString prefix = datasetPropsCachePrefix(connIdx, poolName);
    if (prefix.isEmpty()) {
        return QString();
    }
    return prefix + objectName.trimmed().toLower();
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    beginTransientUiBusy(QStringLiteral("Leyendo propiedades..."));
    auto saveCurrentDraft = [this]() {
        if (!m_propsDirty || m_propsSide.isEmpty() || m_propsDataset.isEmpty()) {
            return;
        }
        QString currToken;
        if (m_propsSide == QStringLiteral("origin")) {
            if (m_connActionOrigin.valid) {
                currToken = QStringLiteral("%1::%2")
                                .arg(m_connActionOrigin.connIdx)
                                .arg(m_connActionOrigin.poolName);
            }
        } else if (m_propsSide == QStringLiteral("dest")) {
            if (m_connActionDest.valid) {
                currToken = QStringLiteral("%1::%2")
                                .arg(m_connActionDest.connIdx)
                                .arg(m_connActionDest.poolName);
            }
        } else if (m_propsSide == QStringLiteral("conncontent")) {
            currToken = m_propsToken;
        } else {
            return;
        }
        if (currToken.isEmpty()) {
            return;
        }
        QTableWidget* currTable = m_connContentPropsTable;
        if (!currTable) {
            return;
        }
        DatasetPropsDraft draft;
        draft.dirty = true;
        for (int r = 0; r < currTable->rowCount(); ++r) {
            QTableWidgetItem* rk = currTable->item(r, 0);
            QTableWidgetItem* rv = currTable->item(r, 1);
            QTableWidgetItem* ri = currTable->item(r, 2);
            if (!rk || !rv || !ri) {
                continue;
            }
            const QString key = propKeyFromItem(rk);
            if (key.isEmpty()) {
                continue;
            }
            draft.valuesByProp[key] = rv->text();
            const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
            draft.inheritByProp[key] = inh;
        }
        m_propsDraftByKey[propsDraftKey(m_propsSide, currToken, m_propsDataset)] = draft;
    };
    saveCurrentDraft();

    QString dataset;
    QString snapshot;
    if (side == QStringLiteral("origin")) {
        dataset = m_connActionOrigin.datasetName;
        snapshot = m_connActionOrigin.snapshotName;
    } else if (side == QStringLiteral("dest")) {
        dataset = m_connActionDest.datasetName;
        snapshot = m_connActionDest.snapshotName;
    } else if (side == QStringLiteral("conncontent")) {
        const auto selected = m_connContentTree ? m_connContentTree->selectedItems() : QList<QTreeWidgetItem*>{};
        if (!selected.isEmpty()) {
            QTreeWidgetItem* sel = selected.first();
            while (sel && sel->data(0, Qt::UserRole).toString().isEmpty() && sel->parent()) {
                sel = sel->parent();
            }
            if (sel) {
                dataset = sel->data(0, Qt::UserRole).toString();
                snapshot = sel->data(1, Qt::UserRole).toString();
            }
        }
    }
    QTableWidget* table = m_connContentPropsTable;
    if (!table) {
        endTransientUiBusy();
        return;
    }
    if (dataset.isEmpty()) {
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        setTablePopulationMode(table, false);
        m_propsDataset.clear();
        m_propsToken.clear();
        m_propsSide = side;
        m_propsOriginalValues.clear();
        m_propsOriginalInherit.clear();
        m_propsDirty = false;
        updateApplyPropsButtonState();
        endTransientUiBusy();
        return;
    }
    updateStatus(QStringLiteral("Leyendo propiedades de %1").arg(snapshot.isEmpty() ? dataset : QStringLiteral("%1@%2").arg(dataset, snapshot)));

    QString token;
    if (side == QStringLiteral("origin")) {
        if (m_connActionOrigin.valid) {
            token = QStringLiteral("%1::%2")
                        .arg(m_connActionOrigin.connIdx)
                        .arg(m_connActionOrigin.poolName);
        }
    } else if (side == QStringLiteral("dest")) {
        if (m_connActionDest.valid) {
            token = QStringLiteral("%1::%2")
                        .arg(m_connActionDest.connIdx)
                        .arg(m_connActionDest.poolName);
        }
    } else if (side == QStringLiteral("conncontent")) {
        token = m_connContentToken;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        m_propsToken.clear();
        endTransientUiBusy();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        endTransientUiBusy();
        return;
    }
    const PoolDatasetCache& cache = it.value();
    const auto recIt = cache.recordByName.constFind(dataset);
    if (recIt == cache.recordByName.constEnd()) {
        endTransientUiBusy();
        return;
    }
    const DatasetRecord& rec = recIt.value();
    const QString objectName = snapshot.isEmpty() ? dataset : QStringLiteral("%1@%2").arg(dataset, snapshot);
    const QString draftKey = propsDraftKey(side, token, objectName);
    const ConnectionProfile& p = m_profiles[connIdx];
    const QString propsCacheKey = datasetPropsCacheKey(connIdx, poolName, objectName);
    const DatasetPlatformFamily platform =
        datasetPlatformFamilyFromStrings(p.osType, (connIdx >= 0 && connIdx < m_states.size()) ? m_states[connIdx].osLine : QString());

    struct PropRow {
        QString prop;
        QString value;
        QString source;
        QString readonly;
    };
    QVector<PropRow> rawRows;
    QString datasetType = objectName.contains('@') ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
    bool propsFromCache = false;
    if (!propsCacheKey.isEmpty()) {
        const auto propsIt = m_datasetPropsCache.constFind(propsCacheKey);
        if (propsIt != m_datasetPropsCache.constEnd() && propsIt->loaded) {
            const DatasetPropsCacheEntry& cached = propsIt.value();
            datasetType = cached.datasetType.trimmed().isEmpty() ? datasetType : cached.datasetType;
            rawRows.reserve(cached.rows.size());
            for (const DatasetPropCacheRow& row : cached.rows) {
                rawRows.push_back({row.prop, row.value, row.source, row.readonly});
            }
            propsFromCache = !rawRows.isEmpty();
            if (propsFromCache) {
                appLog(QStringLiteral("DEBUG"),
                       QStringLiteral("Dataset props cache hit %1::%2")
                           .arg(p.name, objectName));
            }
        }
    }

    if (!propsFromCache) {
        rawRows.push_back({QStringLiteral("dataset"), objectName, QString(), QStringLiteral("true")});
        {
            QString tOut, tErr;
            int tRc = -1;
            const QString typeCmd = withSudo(
                p,
                QStringLiteral("zfs get -H -o value type %1").arg(shSingleQuote(objectName)));
            if (runSsh(p, typeCmd, 12000, tOut, tErr, tRc) && tRc == 0) {
                const QString t = tOut.trimmed().toLower();
                if (!t.isEmpty()) {
                    datasetType = t;
                }
            }
        }

        QString out;
        QString err;
        int rc = -1;
        QString propsCmd = withSudo(
            p,
            QStringLiteral("zfs get -H -o property,value,source,readonly all %1").arg(shSingleQuote(objectName)));
        if (!runSsh(p, propsCmd, 20000, out, err, rc) || rc != 0) {
            propsCmd = withSudo(
                p,
                QStringLiteral("zfs get -H -o property,value,source all %1").arg(shSingleQuote(objectName)));
            out.clear();
            err.clear();
            rc = -1;
            runSsh(p, propsCmd, 20000, out, err, rc);
        }
        if (rc == 0) {
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& raw : lines) {
                QString prop, val, source, ro;
                const QStringList parts = raw.split('\t');
                if (parts.size() >= 4) {
                    prop = parts[0].trimmed();
                    val = parts[1].trimmed();
                    source = parts[2].trimmed();
                    ro = parts[3].trimmed().toLower();
                } else if (parts.size() >= 3) {
                    prop = parts[0].trimmed();
                    val = parts[1].trimmed();
                    source = parts[2].trimmed();
                    ro.clear();
                } else {
                    const QStringList sp = raw.simplified().split(' ');
                    if (sp.size() < 3) {
                        continue;
                    }
                    prop = sp[0].trimmed();
                    val = sp[1].trimmed();
                    source = sp[2].trimmed();
                    ro = (sp.size() > 3) ? sp[3].trimmed().toLower() : QString();
                }
                rawRows.push_back({prop, val, source, ro});
            }
        }
    }

    QMap<QString, PropRow> byProp;
    for (const PropRow& row : rawRows) {
        byProp[row.prop] = row;
    }
    QVector<PropRow> rows;
    rows.reserve(byProp.size() + 2);
    const bool windowsConn = isWindowsConnection(connIdx);
    if (byProp.contains(QStringLiteral("dataset"))) {
        rows.push_back(byProp.take(QStringLiteral("dataset")));
    }
    if (snapshot.isEmpty()) {
        if (byProp.contains(QStringLiteral("mountpoint"))) {
            rows.push_back(byProp.take(QStringLiteral("mountpoint")));
        } else {
            rows.push_back({QStringLiteral("mountpoint"), rec.mountpoint.trimmed(), QString(), QStringLiteral("true")});
        }
        if (byProp.contains(QStringLiteral("canmount"))) {
            rows.push_back(byProp.take(QStringLiteral("canmount")));
        } else {
            rows.push_back({QStringLiteral("canmount"), rec.canmount.trimmed(), QString(), QStringLiteral("true")});
        }
        const QString mountedRaw = rec.mounted.trimmed().toLower();
        const bool mountedYes = (mountedRaw == QStringLiteral("yes")
                                 || mountedRaw == QStringLiteral("on")
                                 || mountedRaw == QStringLiteral("true")
                                 || mountedRaw == QStringLiteral("1"));
        rows.push_back({QStringLiteral("estado"),
                        mountedYes ? trk(QStringLiteral("t_montado_a97484"),
                                         QStringLiteral("Montado"),
                                         QStringLiteral("Mounted"),
                                         QStringLiteral("已挂载"))
                                   : trk(QStringLiteral("t_desmontado_bbceae"),
                                         QStringLiteral("Desmontado"),
                                         QStringLiteral("Unmounted"),
                                         QStringLiteral("未挂载")),
                        QString(),
                        QStringLiteral("true")});
        rows.push_back({QStringLiteral("Tamaño"), formatDatasetSize(rec.used.trimmed()), QString(), QStringLiteral("true")});
        if (windowsConn) {
            if (byProp.contains(QStringLiteral("driveletter"))) {
                rows.push_back(byProp.take(QStringLiteral("driveletter")));
            } else {
                rows.push_back({QStringLiteral("driveletter"), QString(), QString(), QStringLiteral("true")});
            }
        }
    } else {
        rows.push_back({QStringLiteral("estado"), QStringLiteral("Snapshot"), QString(), QStringLiteral("true")});
    }
    const QStringList remainingProps = byProp.keys();
    for (const QString& prop : remainingProps) {
        rows.push_back(byProp.value(prop));
    }

    if (!propsFromCache && !propsCacheKey.isEmpty()) {
        DatasetPropsCacheEntry entry;
        entry.loaded = true;
        entry.objectName = objectName;
        entry.datasetType = datasetType;
        entry.rows.reserve(rows.size());
        for (const PropRow& row : rows) {
            entry.rows.push_back(DatasetPropCacheRow{row.prop, row.value, row.source, row.readonly});
        }
        m_datasetPropsCache.insert(propsCacheKey, entry);
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Dataset props cache store %1::%2 (%3 rows)")
                   .arg(p.name, objectName)
                   .arg(entry.rows.size()));
    }

    if (side == QStringLiteral("conncontent")) {
        QMap<QString, QString> valuesByProp;
        valuesByProp[QStringLiteral("snapshot")] =
            snapshot.trimmed().isEmpty()
                ? trk(QStringLiteral("t_none_paren_001"),
                      QStringLiteral("(ninguno)"),
                      QStringLiteral("(none)"),
                      QStringLiteral("（无）"))
                : snapshot.trimmed();
        for (const PropRow& row : rows) {
            const QString prop = row.prop.trimmed();
            if (prop.isEmpty()) {
                continue;
            }
            if (prop.compare(QStringLiteral("dataset"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            if (!snapshot.trimmed().isEmpty()
                && prop.compare(QStringLiteral("estado"), Qt::CaseInsensitive) == 0) {
                // Para snapshots no mostramos "Montado" en propiedades inline del treeview.
                continue;
            }
            valuesByProp[prop] = row.value;
        }
        updateConnContentPropertyValues(token, objectName, valuesByProp);
        syncConnContentPropertyColumns();
    }

    m_loadingPropsTable = true;
    setTablePopulationMode(table, true);
    table->setRowCount(0);
    m_propsOriginalValues.clear();
    m_propsOriginalInherit.clear();
    m_propsSide = side;
    m_propsDataset = objectName;
    m_propsToken = token;
    const QMap<QString, QStringList> enumValues = {
        {QStringLiteral("atime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("relatime"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("readonly"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("compression"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("lz4"), QStringLiteral("zstd"), QStringLiteral("gzip"), QStringLiteral("zle"), QStringLiteral("lzjb")}},
        {QStringLiteral("checksum"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("fletcher2"), QStringLiteral("fletcher4"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("sync"), {QStringLiteral("standard"), QStringLiteral("always"), QStringLiteral("disabled")}},
        {QStringLiteral("logbias"), {QStringLiteral("latency"), QStringLiteral("throughput")}},
        {QStringLiteral("primarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("secondarycache"), {QStringLiteral("all"), QStringLiteral("none"), QStringLiteral("metadata")}},
        {QStringLiteral("dedup"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("verify"), QStringLiteral("sha256"), QStringLiteral("sha512"), QStringLiteral("skein"), QStringLiteral("edonr"), QStringLiteral("blake3")}},
        {QStringLiteral("copies"), {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}},
        {QStringLiteral("acltype"), {QStringLiteral("off"), QStringLiteral("posix"), QStringLiteral("nfsv4")}},
        {QStringLiteral("aclinherit"), {QStringLiteral("discard"), QStringLiteral("noallow"), QStringLiteral("restricted"), QStringLiteral("passthrough"), QStringLiteral("passthrough-x")}},
        {QStringLiteral("xattr"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("sa"), QStringLiteral("dir")}},
        {QStringLiteral("normalization"), {QStringLiteral("none"), QStringLiteral("formC"), QStringLiteral("formD"), QStringLiteral("formKC"), QStringLiteral("formKD")}},
        {QStringLiteral("casesensitivity"), {QStringLiteral("sensitive"), QStringLiteral("insensitive"), QStringLiteral("mixed")}},
        {QStringLiteral("utf8only"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("canmount"), {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("noauto")}},
        {QStringLiteral("snapdir"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("exec"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("setuid"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("devices"), {QStringLiteral("on"), QStringLiteral("off")}},
        {QStringLiteral("snapdev"), {QStringLiteral("hidden"), QStringLiteral("visible")}},
        {QStringLiteral("volmode"), {QStringLiteral("default"), QStringLiteral("full"), QStringLiteral("dev"), QStringLiteral("none"), QStringLiteral("geom")}},
    };
    const int pinnedCount = (!snapshot.isEmpty() ? 2 : (windowsConn ? 6 : 5));
    table->setProperty("pinned_rows", pinnedCount);
    for (const PropRow& row : rows) {
        const int r = table->rowCount();
        table->insertRow(r);
        auto* k = new PinnedSortItem(row.prop);
        k->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        k->setData(kPropKeyRole, row.prop);
        if (row.prop == QStringLiteral("dataset")) {
            k->setText(trk(QStringLiteral("t_prop_name_001"),
                           QStringLiteral("Nombre"),
                           QStringLiteral("Name"),
                           QStringLiteral("名称")));
        }
        table->setItem(r, 0, k);
        auto* v = new PinnedSortItem(row.value);
        v->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        const bool editable =
            isDatasetPropertyEditable(row.prop, datasetType, row.source, row.readonly, platform);
        if (!editable) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
            const QColor disabledColor = table->palette().color(QPalette::Disabled, QPalette::Text);
            k->setForeground(disabledColor);
            v->setForeground(disabledColor);
            const QString reason =
                !isDatasetPropertySupportedOnPlatform(row.prop, platform)
                    ? trk(QStringLiteral("t_prop_unsupported_platform_001"),
                          QStringLiteral("Propiedad no soportada en este sistema operativo."))
                    : QString();
            if (!reason.isEmpty()) {
                k->setToolTip(reason);
                v->setToolTip(reason);
            }
        }
        if (row.prop == QStringLiteral("estado") || row.prop == QStringLiteral("Tamaño")) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        }
        table->setItem(r, 1, v);
        const QString propLower = row.prop.trimmed().toLower();
        const auto enumIt = enumValues.constFind(propLower);
        if ((v->flags() & Qt::ItemIsEditable) && enumIt != enumValues.constEnd()) {
            auto* combo = new NoWheelComboBox(table);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            QStringList options = enumIt.value();
            const QString current = row.value.trimmed();
            if (!current.isEmpty() && !options.contains(current)) {
                options.prepend(current);
            }
            combo->addItems(options);
            if (!current.isEmpty()) {
                combo->setCurrentText(current);
            }
            table->setCellWidget(r, 1, combo);
            QObject::connect(combo, &QComboBox::currentTextChanged, table, [this, table, combo](const QString& txt) {
                for (int rr = 0; rr < table->rowCount(); ++rr) {
                    if (table->cellWidget(rr, 1) != combo) {
                        continue;
                    }
                    if (QTableWidgetItem* item = table->item(rr, 1)) {
                        item->setText(txt);
                    }
                    onDatasetPropsCellChanged(rr, 1);
                    break;
                }
            });
        }
        auto* inh = new PinnedSortItem();
        inh->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        inh->setData(kPropKeyRole, row.prop);
        const bool inheritable = isDatasetPropertyInheritable(row.prop, datasetType, row.source, row.readonly, platform);
        const bool currentlyInherited = isDatasetPropertyCurrentlyInherited(row.source);
        if (inheritable) {
            inh->setFlags((inh->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) & ~Qt::ItemIsEditable);
            inh->setCheckState(currentlyInherited ? Qt::Checked : Qt::Unchecked);
        } else {
            inh->setFlags(Qt::ItemIsEnabled);
            inh->setText(QStringLiteral("-"));
        }
        table->setItem(r, 2, inh);
        if (r < pinnedCount) {
            QFont f0 = k->font();
            f0.setBold(true);
            k->setFont(f0);
            QFont f1 = v->font();
            f1.setBold(true);
            v->setFont(f1);
            QFont f2 = inh->font();
            f2.setBold(true);
            inh->setFont(f2);
            if (QComboBox* cb = qobject_cast<QComboBox*>(table->cellWidget(r, 1))) {
                QFont cf = cb->font();
                cf.setBold(true);
                cb->setFont(cf);
            }
        }
        m_propsOriginalValues[row.prop] = row.value;
        m_propsOriginalInherit[row.prop] = currentlyInherited;
    }
    const auto draftIt = m_propsDraftByKey.constFind(draftKey);
    if (draftIt != m_propsDraftByKey.constEnd()) {
        const DatasetPropsDraft& draft = draftIt.value();
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* rk = table->item(r, 0);
            QTableWidgetItem* rv = table->item(r, 1);
            QTableWidgetItem* ri = table->item(r, 2);
            if (!rk || !rv || !ri) {
                continue;
            }
            const QString key = propKeyFromItem(rk);
            if (key.isEmpty()) {
                continue;
            }
            const auto vIt = draft.valuesByProp.constFind(key);
            if (vIt != draft.valuesByProp.constEnd()) {
                rv->setText(vIt.value());
                if (QComboBox* cb = qobject_cast<QComboBox*>(table->cellWidget(r, 1))) {
                    cb->setCurrentText(vIt.value());
                }
            }
            const auto iIt = draft.inheritByProp.constFind(key);
            if (iIt != draft.inheritByProp.constEnd()
                && (ri->flags() & Qt::ItemIsUserCheckable)) {
                ri->setCheckState(iIt.value() ? Qt::Checked : Qt::Unchecked);
            }
        }
        m_propsDirty = draft.dirty;
    } else {
        m_propsDirty = false;
    }
    setTablePopulationMode(table, false);
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
    endTransientUiBusy();
}

void MainWindow::onDatasetPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2)) {
        return;
    }
    QTableWidget* table = qobject_cast<QTableWidget*>(sender());
    if (!table) {
        table = m_connContentPropsTable;
    }
    if (!table) {
        return;
    }
    QTableWidgetItem* pk = table->item(row, 0);
    QTableWidgetItem* pv = table->item(row, 1);
    QTableWidgetItem* pi = table->item(row, 2);
    if (!pk || !pv || !pi) {
        return;
    }
    m_propsDirty = false;
    for (int r = 0; r < table->rowCount(); ++r) {
        QTableWidgetItem* rk = table->item(r, 0);
        QTableWidgetItem* rv = table->item(r, 1);
        QTableWidgetItem* ri = table->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = propKeyFromItem(rk);
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_propsOriginalInherit.value(key, false)
            || rv->text() != m_propsOriginalValues.value(key)) {
            m_propsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::applyDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    bool hasPendingPermissionDrafts = false;
    for (auto it = m_datasetPermissionsCache.cbegin(); it != m_datasetPermissionsCache.cend(); ++it) {
        if (it.value().loaded && it.value().dirty) {
            hasPendingPermissionDrafts = true;
            break;
        }
    }
    if (m_propsSide == QStringLiteral("conncontent") || hasPendingPermissionDrafts) {
        auto saveCurrentConnContentDraft = [this]() {
            if (m_propsDataset.isEmpty() || m_propsToken.isEmpty() || !m_connContentPropsTable) {
                return;
            }
            DatasetPropsDraft draft;
            for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
                QTableWidgetItem* rk = m_connContentPropsTable->item(r, 0);
                QTableWidgetItem* rv = m_connContentPropsTable->item(r, 1);
                QTableWidgetItem* ri = m_connContentPropsTable->item(r, 2);
                if (!rk || !rv || !ri) {
                    continue;
                }
                const QString key = propKeyFromItem(rk);
                if (key.isEmpty()) {
                    continue;
                }
                draft.valuesByProp[key] = rv->text();
                draft.inheritByProp[key] =
                    (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
            }
            draft.dirty = m_propsDirty;
            const QString key = propsDraftKey(QStringLiteral("conncontent"), m_propsToken, m_propsDataset);
            if (draft.dirty) {
                m_propsDraftByKey[key] = draft;
            }
        };
        saveCurrentConnContentDraft();

        struct PendingDraft {
            QString draftKey;
            QString token;
            QString objectName;
            DatasetSelectionContext ctx;
            DatasetPropsDraft draft;
        };
        QVector<PendingDraft> pending;
        for (auto it = m_propsDraftByKey.cbegin(); it != m_propsDraftByKey.cend(); ++it) {
            if (!it.value().dirty || !it.key().startsWith(QStringLiteral("conncontent|"))) {
                continue;
            }
            const QStringList parts = it.key().split(QLatin1Char('|'));
            if (parts.size() < 3) {
                continue;
            }
            const QString token = parts.value(1).trimmed();
            const QString objectName = parts.mid(2).join(QStringLiteral("|")).trimmed();
            if (token.isEmpty() || objectName.isEmpty() || objectName.contains(QLatin1Char('@'))) {
                continue;
            }
            const int sep = token.indexOf(QStringLiteral("::"));
            if (sep <= 0) {
                continue;
            }
            bool okConn = false;
            const int connIdx = token.left(sep).toInt(&okConn);
            const QString poolName = token.mid(sep + 2);
            if (!okConn || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
                continue;
            }
            DatasetSelectionContext ctx;
            ctx.valid = true;
            ctx.connIdx = connIdx;
            ctx.poolName = poolName;
            ctx.datasetName = objectName;
            pending.push_back(PendingDraft{it.key(), token, objectName, ctx, it.value()});
        }
        auto isMountedText = [](const QString& v) -> bool {
            const QString s = v.trimmed().toLower();
            return s == QStringLiteral("montado")
                   || s == QStringLiteral("mounted")
                   || s == QStringLiteral("已挂载")
                   || s == QStringLiteral("on")
                   || s == QStringLiteral("yes")
                   || s == QStringLiteral("true")
                   || s == QStringLiteral("1");
        };

        for (const PendingDraft& item : pending) {
            const QString cacheKey = datasetPropsCacheKey(item.ctx.connIdx, item.ctx.poolName, item.objectName);
            const auto cacheIt = m_datasetPropsCache.constFind(cacheKey);
            if (cacheIt == m_datasetPropsCache.cend() || !cacheIt->loaded) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_seleccione_615ce3"),
                                         QStringLiteral("No hay caché de propiedades para aplicar cambios a %1.")
                                             .arg(item.objectName),
                                         QStringLiteral("No property cache is available to apply changes to %1.")
                                             .arg(item.objectName),
                                         QStringLiteral("没有可用于将更改应用到 %1 的属性缓存。")
                                             .arg(item.objectName)));
                updateApplyPropsButtonState();
                return;
            }

            QMap<QString, QString> originalValues;
            QMap<QString, bool> originalInherit;
            for (const DatasetPropCacheRow& row : cacheIt->rows) {
                originalValues[row.prop] = row.value;
                originalInherit[row.prop] = isDatasetPropertyCurrentlyInherited(row.source);
            }

            QSet<QString> touched;
            for (auto it = item.draft.valuesByProp.cbegin(); it != item.draft.valuesByProp.cend(); ++it) {
                touched.insert(it.key());
            }
            for (auto it = item.draft.inheritByProp.cbegin(); it != item.draft.inheritByProp.cend(); ++it) {
                touched.insert(it.key());
            }

            QStringList subcmds;
            for (const QString& prop : touched) {
                if (prop.isEmpty() || prop == QStringLiteral("dataset")
                    || prop == QStringLiteral("Tamaño")
                    || prop == QStringLiteral("snapshot")) {
                    continue;
                }
                const bool originalInh = originalInherit.value(prop, false);
                const bool finalInh = item.draft.inheritByProp.contains(prop)
                                          ? item.draft.inheritByProp.value(prop)
                                          : originalInh;
                const QString originalValue = originalValues.value(prop);
                const QString finalValue = item.draft.valuesByProp.contains(prop)
                                               ? item.draft.valuesByProp.value(prop)
                                               : originalValue;

                if (prop == QStringLiteral("estado")) {
                    if (isMountedText(finalValue) != isMountedText(originalValue)) {
                        subcmds << QStringLiteral("zfs %1 %2")
                                       .arg((isWindowsConnection(item.ctx.connIdx)
                                                 ? (isMountedText(finalValue) ? QStringLiteral("mount")
                                                                              : QStringLiteral("unmount"))
                                                 : (isMountedText(finalValue) ? QStringLiteral("mount")
                                                                              : QStringLiteral("umount"))),
                                            shSingleQuote(item.ctx.datasetName));
                    }
                    continue;
                }
                if (finalInh != originalInh) {
                    if (finalInh) {
                        subcmds << QStringLiteral("zfs inherit %1 %2")
                                       .arg(shSingleQuote(prop), shSingleQuote(item.ctx.datasetName));
                    } else {
                        subcmds << QStringLiteral("zfs set %1 %2")
                                       .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                            shSingleQuote(item.ctx.datasetName));
                    }
                    continue;
                }
                if (!finalInh && finalValue != originalValue) {
                    subcmds << QStringLiteral("zfs set %1 %2")
                                   .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                        shSingleQuote(item.ctx.datasetName));
                }
            }

            if (subcmds.isEmpty()) {
                m_propsDraftByKey.remove(item.draftKey);
                continue;
            }

            const bool isWin = isWindowsConnection(item.ctx.connIdx);
            const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                                      : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
            if (!executeDatasetAction(QStringLiteral("conncontent"),
                                      QStringLiteral("Aplicar propiedades"),
                                      item.ctx,
                                      cmd,
                                      60000,
                                      isWin)) {
                updateApplyPropsButtonState();
                return;
            }
            m_propsDraftByKey.remove(item.draftKey);
        }

        auto normalizeTokens = [](QStringList tokens) {
            QSet<QString> seen;
            QStringList normalized;
            for (const QString& token : tokens) {
                const QString t = token.trimmed();
                const QString key = t.toLower();
                if (t.isEmpty() || seen.contains(key)) {
                    continue;
                }
                seen.insert(key);
                normalized.push_back(t);
            }
            normalized.sort(Qt::CaseInsensitive);
            return normalized;
        };
        auto grantKey = [](const DatasetPermissionGrant& grant) {
            return QStringLiteral("%1|%2|%3")
                .arg(grant.scope.trimmed().toLower(),
                     grant.targetType.trimmed().toLower(),
                     grant.targetName.trimmed().toLower());
        };
        auto setKey = [](const DatasetPermissionSet& set) {
            return set.name.trimmed().toLower();
        };
        auto scopeFlagsForPermission = [](const QString& scope) {
            const QString s = scope.trimmed().toLower();
            if (s == QStringLiteral("local")) {
                return QStringLiteral("-l ");
            }
            if (s == QStringLiteral("descendant")) {
                return QStringLiteral("-d ");
            }
            return QString();
        };
        auto targetFlagsForPermission = [](const QString& targetType, const QString& targetName) {
            const QString tt = targetType.trimmed().toLower();
            if (tt == QStringLiteral("user")) {
                return QStringLiteral("-u %1 ").arg(shSingleQuote(targetName));
            }
            if (tt == QStringLiteral("group")) {
                return QStringLiteral("-g %1 ").arg(shSingleQuote(targetName));
            }
            return QStringLiteral("-e ");
        };
        auto findDatasetItemByIdentityLocal = [](QTreeWidget* tree,
                                                 int connIdx,
                                                 const QString& poolName,
                                                 const QString& datasetName) -> QTreeWidgetItem* {
            if (!tree) {
                return nullptr;
            }
            std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) -> QTreeWidgetItem* {
                if (!node) {
                    return nullptr;
                }
                if (node->data(0, Qt::UserRole).toString().trimmed() == datasetName
                    && node->data(0, kConnIdxRole).toInt() == connIdx
                    && node->data(0, kPoolNameRole).toString().trimmed() == poolName) {
                    return node;
                }
                for (int i = 0; i < node->childCount(); ++i) {
                    if (QTreeWidgetItem* found = rec(node->child(i))) {
                        return found;
                    }
                }
                return nullptr;
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                if (QTreeWidgetItem* found = rec(tree->topLevelItem(i))) {
                    return found;
                }
            }
            return nullptr;
        };

        QStringList permissionKeysToRefresh;
        for (auto it = m_datasetPermissionsCache.cbegin(); it != m_datasetPermissionsCache.cend(); ++it) {
            if (!it.value().loaded || !it.value().dirty) {
                continue;
            }
            permissionKeysToRefresh << it.key();
        }
        for (const QString& cacheKey : permissionKeysToRefresh) {
            auto it = m_datasetPermissionsCache.find(cacheKey);
            if (it == m_datasetPermissionsCache.end()) {
                continue;
            }
            const QStringList parts = cacheKey.split(QStringLiteral("::"));
            if (parts.size() < 3) {
                continue;
            }
            bool okConn = false;
            const int connIdx = parts.at(0).toInt(&okConn);
            const QString poolName = parts.at(1).trimmed();
            const QString datasetName = parts.mid(2).join(QStringLiteral("::")).trimmed();
            if (!okConn || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty() || datasetName.isEmpty()) {
                continue;
            }
            QStringList subcmds;
            auto appendGrantCommands = [&](const QVector<DatasetPermissionGrant>& original,
                                           const QVector<DatasetPermissionGrant>& current) {
                QMap<QString, DatasetPermissionGrant> origMap;
                QMap<QString, DatasetPermissionGrant> currMap;
                for (const DatasetPermissionGrant& g : original) {
                    origMap.insert(grantKey(g), g);
                }
                for (const DatasetPermissionGrant& g : current) {
                    currMap.insert(grantKey(g), g);
                }
                QSet<QString> keys;
                for (auto k = origMap.cbegin(); k != origMap.cend(); ++k) keys.insert(k.key());
                for (auto k = currMap.cbegin(); k != currMap.cend(); ++k) keys.insert(k.key());
                for (const QString& key : keys) {
                    const bool had = origMap.contains(key);
                    const bool has = currMap.contains(key);
                    const DatasetPermissionGrant before = origMap.value(key);
                    const DatasetPermissionGrant after = currMap.value(key);
                    const QString beforeFlags = scopeFlagsForPermission(before.scope) + targetFlagsForPermission(before.targetType, before.targetName);
                    const QString afterFlags = scopeFlagsForPermission(after.scope) + targetFlagsForPermission(after.targetType, after.targetName);
                    const QStringList beforeTokens = normalizeTokens(before.permissions);
                    const QStringList afterTokens = normalizeTokens(after.permissions);
                    if (had && (!has || afterTokens.isEmpty())) {
                        subcmds << QStringLiteral("zfs unallow %1%2")
                                       .arg(beforeFlags, shSingleQuote(datasetName));
                        continue;
                    }
                    if (!had && has) {
                        if (!afterTokens.isEmpty()) {
                            subcmds << QStringLiteral("zfs allow %1%2 %3")
                                           .arg(afterFlags,
                                                afterTokens.join(','),
                                                shSingleQuote(datasetName));
                        }
                        continue;
                    }
                    if (had && has && beforeTokens != afterTokens) {
                        QString cmd = QStringLiteral("zfs unallow %1%2")
                                          .arg(beforeFlags, shSingleQuote(datasetName));
                        if (!afterTokens.isEmpty()) {
                            cmd += QStringLiteral(" && zfs allow %1%2 %3")
                                       .arg(afterFlags,
                                            afterTokens.join(','),
                                            shSingleQuote(datasetName));
                        }
                        subcmds << cmd;
                    }
                }
            };
            appendGrantCommands(it->originalLocalGrants, it->localGrants);
            appendGrantCommands(it->originalDescendantGrants, it->descendantGrants);
            appendGrantCommands(it->originalLocalDescendantGrants, it->localDescendantGrants);

            const QStringList originalCreate = normalizeTokens(it->originalCreatePermissions);
            const QStringList currentCreate = normalizeTokens(it->createPermissions);
            if (originalCreate != currentCreate) {
                QString cmd = QStringLiteral("zfs unallow -c %1").arg(shSingleQuote(datasetName));
                if (!currentCreate.isEmpty()) {
                    cmd += QStringLiteral(" && zfs allow -c %1 %2")
                               .arg(currentCreate.join(','),
                                    shSingleQuote(datasetName));
                }
                subcmds << cmd;
            }

            QMap<QString, DatasetPermissionSet> origSets;
            QMap<QString, DatasetPermissionSet> currSets;
            for (const DatasetPermissionSet& s : it->originalPermissionSets) {
                origSets.insert(setKey(s), s);
            }
            for (const DatasetPermissionSet& s : it->permissionSets) {
                currSets.insert(setKey(s), s);
            }
            QSet<QString> setKeys;
            for (auto k = origSets.cbegin(); k != origSets.cend(); ++k) setKeys.insert(k.key());
            for (auto k = currSets.cbegin(); k != currSets.cend(); ++k) setKeys.insert(k.key());
            for (const QString& key : setKeys) {
                const bool had = origSets.contains(key);
                const bool has = currSets.contains(key);
                const DatasetPermissionSet before = origSets.value(key);
                const DatasetPermissionSet after = currSets.value(key);
                const QStringList beforeTokens = normalizeTokens(before.permissions);
                const QStringList afterTokens = normalizeTokens(after.permissions);
                if (had && !has) {
                    subcmds << QStringLiteral("zfs unallow -s %1 %2")
                                   .arg(before.name, shSingleQuote(datasetName));
                    continue;
                }
                if (!had && has) {
                    if (!afterTokens.isEmpty()) {
                        subcmds << QStringLiteral("zfs allow -s %1 %2 %3")
                                       .arg(after.name,
                                            afterTokens.join(','),
                                            shSingleQuote(datasetName));
                    }
                    continue;
                }
                if (had && has && (before.name != after.name || beforeTokens != afterTokens)) {
                    QString cmd = QStringLiteral("zfs unallow -s %1 %2")
                                      .arg(before.name, shSingleQuote(datasetName));
                    if (!afterTokens.isEmpty()) {
                        cmd += QStringLiteral(" && zfs allow -s %1 %2 %3")
                                   .arg(after.name,
                                        afterTokens.join(','),
                                        shSingleQuote(datasetName));
                    }
                    subcmds << cmd;
                }
            }

            if (subcmds.isEmpty()) {
                it->dirty = false;
                continue;
            }

            DatasetSelectionContext ctx;
            ctx.valid = true;
            ctx.connIdx = connIdx;
            ctx.poolName = poolName;
            ctx.datasetName = datasetName;
            const QString tokenForTree = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            QTreeWidget* prevTree = m_connContentTree;
            const QString prevToken = m_connContentToken;
            if (QTreeWidgetItem* ownerNode = findDatasetItemByIdentityLocal(m_connContentTree, connIdx, poolName, datasetName)) {
                m_connContentToken = tokenForTree;
                saveConnContentTreeState(tokenForTree);
                Q_UNUSED(ownerNode);
            } else if (QTreeWidgetItem* ownerNode = findDatasetItemByIdentityLocal(m_bottomConnContentTree, connIdx, poolName, datasetName)) {
                m_connContentTree = m_bottomConnContentTree;
                m_connContentToken = tokenForTree;
                saveConnContentTreeState(tokenForTree);
                Q_UNUSED(ownerNode);
            }
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
            const QString cmd = QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
            if (!executeDatasetAction(QStringLiteral("conncontent"),
                                      QStringLiteral("Aplicar permisos"),
                                      ctx,
                                      cmd,
                                      60000,
                                      false)) {
                updateApplyPropsButtonState();
                return;
            }
            m_datasetPermissionsCache.remove(cacheKey);
            if (QTreeWidgetItem* ownerNode = findDatasetItemByIdentityLocal(m_connContentTree, connIdx, poolName, datasetName)) {
                prevTree = m_connContentTree;
                const QString prevApplyToken = m_connContentToken;
                m_connContentToken = tokenForTree;
                populateDatasetPermissionsNode(m_connContentTree, ownerNode, true);
                restoreConnContentTreeState(tokenForTree);
                m_connContentToken = prevApplyToken;
                m_connContentTree = prevTree;
            }
            if (QTreeWidgetItem* ownerNode = findDatasetItemByIdentityLocal(m_bottomConnContentTree, connIdx, poolName, datasetName)) {
                prevTree = m_connContentTree;
                const QString prevApplyToken = m_connContentToken;
                m_connContentTree = m_bottomConnContentTree;
                m_connContentToken = tokenForTree;
                populateDatasetPermissionsNode(m_bottomConnContentTree, ownerNode, true);
                restoreConnContentTreeState(tokenForTree);
                m_connContentToken = prevApplyToken;
                m_connContentTree = prevTree;
            }
        }

        bool hasPropertyDrafts = false;
        for (auto it = m_propsDraftByKey.cbegin(); it != m_propsDraftByKey.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("conncontent|")) && it.value().dirty) {
                hasPropertyDrafts = true;
                break;
            }
        }
        bool hasPermissionDrafts = false;
        for (auto it = m_datasetPermissionsCache.cbegin(); it != m_datasetPermissionsCache.cend(); ++it) {
            if (it.value().loaded && it.value().dirty) {
                hasPermissionDrafts = true;
                break;
            }
        }
        m_propsDirty = hasPropertyDrafts || hasPermissionDrafts;
        if (m_connContentPropsTable && !m_propsDataset.isEmpty()) {
            m_propsOriginalValues.clear();
            m_propsOriginalInherit.clear();
            for (int r = 0; r < m_connContentPropsTable->rowCount(); ++r) {
                QTableWidgetItem* rk = m_connContentPropsTable->item(r, 0);
                QTableWidgetItem* rv = m_connContentPropsTable->item(r, 1);
                QTableWidgetItem* ri = m_connContentPropsTable->item(r, 2);
                if (!rk || !rv || !ri) {
                    continue;
                }
                const QString key = propKeyFromItem(rk);
                if (key.isEmpty()) {
                    continue;
                }
                m_propsOriginalValues[key] = rv->text();
                m_propsOriginalInherit[key] =
                    (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
            }
        }
        updateApplyPropsButtonState();
        return;
    }
    if (!m_propsDirty || m_propsDataset.isEmpty() || m_propsSide.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    if (m_propsSide == QStringLiteral("conncontent")) {
        const QString tokenCtx = m_propsToken.trimmed();
        const int sepCtx = tokenCtx.indexOf(QStringLiteral("::"));
        if (sepCtx > 0) {
            bool okConn = false;
            const int connIdx = tokenCtx.left(sepCtx).toInt(&okConn);
            const QString poolName = tokenCtx.mid(sepCtx + 2);
            if (okConn && connIdx >= 0 && !poolName.isEmpty()) {
                ctx.valid = true;
                ctx.connIdx = connIdx;
                ctx.poolName = poolName;
                const int at = m_propsDataset.indexOf('@');
                if (at > 0) {
                    ctx.datasetName = m_propsDataset.left(at);
                    ctx.snapshotName = m_propsDataset.mid(at + 1);
                } else {
                    ctx.datasetName = m_propsDataset;
                    ctx.snapshotName.clear();
                }
            }
        }
    }
    if (!ctx.valid || (ctx.snapshotName.isEmpty() ? ctx.datasetName : QStringLiteral("%1@%2").arg(ctx.datasetName, ctx.snapshotName)) != m_propsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_seleccione_615ce3"),
                                 QStringLiteral("Seleccione un dataset activo para aplicar cambios."),
                                 QStringLiteral("Select an active dataset to apply changes."),
                                 QStringLiteral("请选择一个活动数据集以应用更改。")));
        return;
    }

    QTableWidget* propsTable = m_connContentPropsTable;
    if (!propsTable) {
        return;
    }
    QString currentToken;
    if (m_propsSide == QStringLiteral("origin")) {
        if (m_connActionOrigin.valid) {
            currentToken = QStringLiteral("%1::%2")
                               .arg(m_connActionOrigin.connIdx)
                               .arg(m_connActionOrigin.poolName);
        }
    } else if (m_propsSide == QStringLiteral("dest")) {
        if (m_connActionDest.valid) {
            currentToken = QStringLiteral("%1::%2")
                               .arg(m_connActionDest.connIdx)
                               .arg(m_connActionDest.poolName);
        }
    } else if (m_propsSide == QStringLiteral("conncontent")) {
        currentToken = m_propsToken;
    }
    const QString currentDraftKey = currentToken.isEmpty() ? QString()
                                                           : propsDraftKey(m_propsSide, currentToken, m_propsDataset);
    auto refreshConncontentTarget = [this](const QString& token, const QString& datasetToSelect) {
        const int sep2 = token.indexOf(QStringLiteral("::"));
        if (sep2 <= 0) {
            return;
        }
        bool okConn = false;
        const int connIdx = token.left(sep2).toInt(&okConn);
        const QString poolName = token.mid(sep2 + 2);
        if (!okConn || connIdx < 0 || poolName.isEmpty()) {
            return;
        }
        auto refreshOneTree = [this, connIdx, &poolName, &datasetToSelect](QTreeWidget* tree, const QString& tokenForTree) {
            if (!tree || tokenForTree != QStringLiteral("%1::%2").arg(connIdx).arg(poolName)) {
                return false;
            }
            const QString prevToken = m_connContentToken;
            QTreeWidget* prevTree = m_connContentTree;
            m_connContentTree = tree;
            m_connContentToken = tokenForTree;
            saveConnContentTreeState(tokenForTree);
            populateDatasetTree(tree, connIdx, poolName, QStringLiteral("conncontent"), true);
            if (!datasetToSelect.trimmed().isEmpty()) {
                auto findInTree = [](QTreeWidget* tw, const QString& ds) -> QTreeWidgetItem* {
                    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                        if (!n) {
                            return nullptr;
                        }
                        if (n->data(0, Qt::UserRole).toString() == ds) {
                            return n;
                        }
                        for (int i = 0; i < n->childCount(); ++i) {
                            if (QTreeWidgetItem* f = rec(n->child(i))) {
                                return f;
                            }
                        }
                        return nullptr;
                    };
                    if (!tw) {
                        return nullptr;
                    }
                    for (int i = 0; i < tw->topLevelItemCount(); ++i) {
                        if (QTreeWidgetItem* f = rec(tw->topLevelItem(i))) {
                            return f;
                        }
                    }
                    return nullptr;
                };
                if (QTreeWidgetItem* item = findInTree(tree, datasetToSelect.trimmed())) {
                    tree->setCurrentItem(item);
                }
            }
            refreshDatasetProperties(QStringLiteral("conncontent"));
            m_connContentTree = prevTree;
            m_connContentToken = prevToken;
            return true;
        };
        const QString topToken = m_connContentToken;
        const int bIdx = m_bottomConnectionEntityTabs ? m_bottomConnectionEntityTabs->currentIndex() : -1;
        const QString bottomToken =
            (bIdx >= 0 && m_bottomConnectionEntityTabs && bIdx < m_bottomConnectionEntityTabs->count())
                ? ([this, bIdx]() {
                      const QString key = m_bottomConnectionEntityTabs->tabData(bIdx).toString();
                      const QStringList parts = key.split(':');
                      if (parts.size() < 3 || parts.first() != QStringLiteral("pool")) {
                          return QString();
                      }
                      return QStringLiteral("%1::%2").arg(parts.value(1)).arg(parts.value(2).trimmed());
                  })()
                : QString();
        bool refreshed = refreshOneTree(m_connContentTree, topToken);
        refreshed = refreshOneTree(m_bottomConnContentTree, bottomToken) || refreshed;
        if (!refreshed) {
            reloadDatasetSide(QStringLiteral("conncontent"));
        }
    };

    QStringList subcmds;
    struct PropChange {
        bool inherit{false};
        QString prop;
        QString value;
    };
    QVector<PropChange> propChanges;
    bool mountStateChangeRequested = false;
    bool mountStateTargetOn = false;
    auto isMountedText = [](const QString& v) -> bool {
        const QString s = v.trimmed().toLower();
        return s == QStringLiteral("montado")
               || s == QStringLiteral("mounted")
               || s == QStringLiteral("已挂载")
               || s == QStringLiteral("on")
               || s == QStringLiteral("yes")
               || s == QStringLiteral("true")
               || s == QStringLiteral("1");
    };
    bool renameRequested = false;
    QString renameOld = ctx.datasetName;
    QString renameNew = ctx.datasetName;
    QString targetDataset = ctx.datasetName;
    for (int r = 0; r < propsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = propsTable->item(r, 0);
        QTableWidgetItem* pv = propsTable->item(r, 1);
        if (!pk || !pv) {
            continue;
        }
        const QString prop = propKeyFromItem(pk);
        if (prop != QStringLiteral("dataset")) {
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_propsOriginalValues.value(prop).trimmed();
        if (!now.isEmpty() && now != old) {
            renameRequested = true;
            renameNew = now;
            targetDataset = now;
        }
        break;
    }
    for (int r = 0; r < propsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = propsTable->item(r, 0);
        QTableWidgetItem* pv = propsTable->item(r, 1);
        QTableWidgetItem* pi = propsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = propKeyFromItem(pk);
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("Tamaño")) {
            continue;
        }
        if (prop == QStringLiteral("estado")) {
            const QString now = pv->text().trimmed();
            const QString old = m_propsOriginalValues.value(prop).trimmed();
            const bool nowMounted = isMountedText(now);
            const bool oldMounted = isMountedText(old);
            if (nowMounted != oldMounted) {
                mountStateChangeRequested = true;
                mountStateTargetOn = nowMounted;
            }
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(targetDataset));
            propChanges.push_back(PropChange{true, prop, QString()});
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_propsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(targetDataset));
        propChanges.push_back(PropChange{false, prop, now});
    }
    if (mountStateChangeRequested) {
        subcmds << (isWindowsConnection(ctx.connIdx)
                        ? QStringLiteral("zfs %1 %2")
                              .arg(mountStateTargetOn ? QStringLiteral("mount")
                                                      : QStringLiteral("unmount"),
                                   shSingleQuote(targetDataset))
                        : QStringLiteral("zfs %1 %2")
                              .arg(mountStateTargetOn ? QStringLiteral("mount")
                                                      : QStringLiteral("umount"),
                                   shSingleQuote(targetDataset)));
    }
    const bool localRenameEligible =
        renameRequested && isLocalConnection(ctx.connIdx) && !isWindowsConnection(ctx.connIdx) && detectLocalLibzfs();
    bool localRenameDone = false;
    if (localRenameEligible) {
        const QString preview =
            QStringLiteral("[local/libzfs]\nzfs rename %1 %2")
                .arg(shSingleQuote(renameOld), shSingleQuote(renameNew));
        if (!confirmActionExecution(QStringLiteral("Aplicar propiedades"), {preview})) {
            return;
        }
        setActionsLocked(true);
        const ConnectionProfile& p = m_profiles[ctx.connIdx];
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Aplicar propiedades %1::%2 (rename backend=LOCAL/libzfs)")
                   .arg(p.name, renameOld));
        QString detail;
        const bool ok = localLibzfsRenameDataset(renameOld, renameNew, &detail);
        if (!ok) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Rename LOCAL/libzfs falló: %1; fallback CLI")
                       .arg(mwhelpers::oneLine(detail)));
            setActionsLocked(false);
            QStringList fallbackSubcmds = subcmds;
            fallbackSubcmds.prepend(QStringLiteral("zfs rename %1 %2")
                                        .arg(shSingleQuote(renameOld), shSingleQuote(renameNew)));
            const bool isWinFallback = isWindowsConnection(ctx.connIdx);
            const QString fallbackCmd = isWinFallback ? fallbackSubcmds.join(QStringLiteral("; "))
                                                      : QStringLiteral("set -e; %1").arg(fallbackSubcmds.join(QStringLiteral("; ")));
            if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, fallbackCmd, 60000, isWinFallback)) {
                setSelectedDataset(m_propsSide, renameNew, QString());
                m_propsDirty = false;
                if (!currentDraftKey.isEmpty()) {
                    m_propsDraftByKey.remove(currentDraftKey);
                }
                updateApplyPropsButtonState();
            }
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Aplicar propiedades rename finalizado (%1)").arg(mwhelpers::oneLine(detail)));
        invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
        setSelectedDataset(m_propsSide, renameNew, QString());
        ctx.datasetName = renameNew;
        localRenameDone = true;
        setActionsLocked(false);
    }

    if (subcmds.isEmpty()) {
        if (localRenameDone) {
            m_propsDirty = false;
            if (!currentDraftKey.isEmpty()) {
                m_propsDraftByKey.remove(currentDraftKey);
            }
            updateApplyPropsButtonState();
            if (m_propsSide == QStringLiteral("conncontent")) {
                refreshConncontentTarget(currentToken, renameNew);
            } else {
                reloadDatasetSide(m_propsSide);
            }
            return;
        }
        if (renameRequested) {
            subcmds << QStringLiteral("zfs rename %1 %2").arg(shSingleQuote(renameOld), shSingleQuote(renameNew));
        }
    } else if (renameRequested && !localRenameDone) {
        subcmds.prepend(QStringLiteral("zfs rename %1 %2").arg(shSingleQuote(renameOld), shSingleQuote(renameNew)));
    }
    if (subcmds.isEmpty()) {
        m_propsDirty = false;
        if (!currentDraftKey.isEmpty()) {
            m_propsDraftByKey.remove(currentDraftKey);
        }
        updateApplyPropsButtonState();
        return;
    }
    const bool localPropsEligible =
        isLocalConnection(ctx.connIdx) && !isWindowsConnection(ctx.connIdx) && detectLocalLibzfs();
    if (localPropsEligible && !propChanges.isEmpty()) {
        QStringList previewLines;
        for (const PropChange& c : propChanges) {
            if (c.inherit) {
                previewLines << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(c.prop), shSingleQuote(targetDataset));
            } else {
                previewLines << QStringLiteral("zfs set %1 %2")
                                    .arg(shSingleQuote(c.prop + QStringLiteral("=") + c.value), shSingleQuote(targetDataset));
            }
        }
        const QString preview = QStringLiteral("[local/libzfs]\n%1").arg(previewLines.join(QStringLiteral("\n")));
        if (!confirmActionExecution(QStringLiteral("Aplicar propiedades"), {preview})) {
            return;
        }
        setActionsLocked(true);
        const ConnectionProfile& p = m_profiles[ctx.connIdx];
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Aplicar propiedades %1::%2 (backend=LOCAL/libzfs)")
                   .arg(p.name, targetDataset));
        bool allOk = true;
        QString failDetail;
        for (const PropChange& c : propChanges) {
            QString detail;
            const bool ok = c.inherit
                                ? localLibzfsInheritProperty(targetDataset, c.prop, &detail)
                                : localLibzfsSetProperty(targetDataset, c.prop, c.value, &detail);
            if (!ok) {
                allOk = false;
                failDetail = detail;
                break;
            }
            appLog(QStringLiteral("INFO"), mwhelpers::oneLine(detail));
        }
        if (!allOk) {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Aplicar propiedades LOCAL/libzfs falló: %1; fallback CLI")
                       .arg(mwhelpers::oneLine(failDetail)));
            setActionsLocked(false);
            const bool isWinFallback = isWindowsConnection(ctx.connIdx);
            const QString cmdFallback = isWinFallback ? subcmds.join(QStringLiteral("; "))
                                                      : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
            if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, cmdFallback, 60000, isWinFallback)) {
                if (targetDataset != ctx.datasetName) {
                    setSelectedDataset(m_propsSide, targetDataset, QString());
                }
                m_propsDirty = false;
                if (!currentDraftKey.isEmpty()) {
                    m_propsDraftByKey.remove(currentDraftKey);
                }
                const QString targetDraftKey = currentToken.isEmpty() ? QString()
                                                                       : propsDraftKey(m_propsSide, currentToken, targetDataset);
                if (!targetDraftKey.isEmpty() && targetDraftKey != currentDraftKey) {
                    m_propsDraftByKey.remove(targetDraftKey);
                }
                updateApplyPropsButtonState();
            }
            return;
        }
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Aplicar propiedades finalizado"));
        invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
        if (m_propsSide == QStringLiteral("conncontent")) {
            refreshConncontentTarget(currentToken, targetDataset);
        } else {
            reloadDatasetSide(m_propsSide);
        }
        if (targetDataset != ctx.datasetName) {
            setSelectedDataset(m_propsSide, targetDataset, QString());
        }
        m_propsDirty = false;
        if (!currentDraftKey.isEmpty()) {
            m_propsDraftByKey.remove(currentDraftKey);
        }
        const QString targetDraftKey = currentToken.isEmpty() ? QString()
                                                               : propsDraftKey(m_propsSide, currentToken, targetDataset);
        if (!targetDraftKey.isEmpty() && targetDraftKey != currentDraftKey) {
            m_propsDraftByKey.remove(targetDraftKey);
        }
        updateApplyPropsButtonState();
        setActionsLocked(false);
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                              : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(m_propsSide, QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000, isWin)) {
        if (targetDataset != ctx.datasetName) {
            setSelectedDataset(m_propsSide, targetDataset, QString());
        }
        if (m_propsSide == QStringLiteral("conncontent")) {
            refreshConncontentTarget(currentToken, targetDataset);
        }
        m_propsDirty = false;
        if (!currentDraftKey.isEmpty()) {
            m_propsDraftByKey.remove(currentDraftKey);
        }
        const QString targetDraftKey = currentToken.isEmpty() ? QString()
                                                               : propsDraftKey(m_propsSide, currentToken, targetDataset);
        if (!targetDraftKey.isEmpty() && targetDraftKey != currentDraftKey) {
            m_propsDraftByKey.remove(targetDraftKey);
        }
        updateApplyPropsButtonState();
    }
}

QStringList MainWindow::pendingConnContentApplyCommands() const {
    QStringList commands;
    auto normalizeTokens = [](QStringList tokens) {
        QSet<QString> seen;
        QStringList normalized;
        for (const QString& token : tokens) {
            const QString t = token.trimmed();
            const QString key = t.toLower();
            if (t.isEmpty() || seen.contains(key)) {
                continue;
            }
            seen.insert(key);
            normalized.push_back(t);
        }
        normalized.sort(Qt::CaseInsensitive);
        return normalized;
    };
    auto grantKey = [](const DatasetPermissionGrant& grant) {
        return QStringLiteral("%1|%2|%3")
            .arg(grant.scope.trimmed().toLower(),
                 grant.targetType.trimmed().toLower(),
                 grant.targetName.trimmed().toLower());
    };
    auto setKey = [](const DatasetPermissionSet& set) {
        return set.name.trimmed().toLower();
    };
    auto scopeFlagsForPermission = [](const QString& scope) {
        const QString s = scope.trimmed().toLower();
        if (s == QStringLiteral("local")) {
            return QStringLiteral("-l ");
        }
        if (s == QStringLiteral("descendant")) {
            return QStringLiteral("-d ");
        }
        return QString();
    };
    auto targetFlagsForPermission = [](const QString& targetType, const QString& targetName) {
        const QString tt = targetType.trimmed().toLower();
        if (tt == QStringLiteral("user")) {
            return QStringLiteral("-u %1 ").arg(shSingleQuote(targetName));
        }
        if (tt == QStringLiteral("group")) {
            return QStringLiteral("-g %1 ").arg(shSingleQuote(targetName));
        }
        return QStringLiteral("-e ");
    };
    auto isMountedText = [](const QString& v) -> bool {
        const QString s = v.trimmed().toLower();
        return s == QStringLiteral("montado")
               || s == QStringLiteral("mounted")
               || s == QStringLiteral("已挂载")
               || s == QStringLiteral("on")
               || s == QStringLiteral("yes")
               || s == QStringLiteral("true")
               || s == QStringLiteral("1");
    };
    for (auto it = m_propsDraftByKey.cbegin(); it != m_propsDraftByKey.cend(); ++it) {
        if (!it.value().dirty || !it.key().startsWith(QStringLiteral("conncontent|"))) {
            continue;
        }
        const QStringList parts = it.key().split(QLatin1Char('|'));
        if (parts.size() < 3) {
            continue;
        }
        const QString token = parts.value(1).trimmed();
        const QString objectName = parts.mid(2).join(QStringLiteral("|")).trimmed();
        if (token.isEmpty() || objectName.isEmpty() || objectName.contains(QLatin1Char('@'))) {
            continue;
        }
        const int sep = token.indexOf(QStringLiteral("::"));
        if (sep <= 0) {
            continue;
        }
        bool okConn = false;
        const int connIdx = token.left(sep).toInt(&okConn);
        const QString poolName = token.mid(sep + 2);
        if (!okConn || connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty()) {
            continue;
        }
        const QString cacheKey = datasetPropsCacheKey(connIdx, poolName, objectName);
        const auto cacheIt = m_datasetPropsCache.constFind(cacheKey);
        if (cacheIt == m_datasetPropsCache.cend() || !cacheIt->loaded) {
            continue;
        }

        QMap<QString, QString> originalValues;
        QMap<QString, bool> originalInherit;
        for (const DatasetPropCacheRow& row : cacheIt->rows) {
            originalValues[row.prop] = row.value;
            originalInherit[row.prop] = isDatasetPropertyCurrentlyInherited(row.source);
        }

        QSet<QString> touched;
        for (auto vit = it.value().valuesByProp.cbegin(); vit != it.value().valuesByProp.cend(); ++vit) {
            touched.insert(vit.key());
        }
        for (auto iit = it.value().inheritByProp.cbegin(); iit != it.value().inheritByProp.cend(); ++iit) {
            touched.insert(iit.key());
        }

        for (const QString& prop : touched) {
            if (prop.isEmpty() || prop == QStringLiteral("dataset")
                || prop == QStringLiteral("Tamaño")
                || prop == QStringLiteral("snapshot")) {
                continue;
            }
            const bool originalInh = originalInherit.value(prop, false);
            const bool finalInh = it.value().inheritByProp.contains(prop)
                                      ? it.value().inheritByProp.value(prop)
                                      : originalInh;
            const QString originalValue = originalValues.value(prop);
            const QString finalValue = it.value().valuesByProp.contains(prop)
                                           ? it.value().valuesByProp.value(prop)
                                           : originalValue;

            if (prop == QStringLiteral("estado")) {
                if (isMountedText(finalValue) != isMountedText(originalValue)) {
                    commands << QStringLiteral("zfs %1 %2")
                                    .arg((isWindowsConnection(connIdx)
                                              ? (isMountedText(finalValue) ? QStringLiteral("mount")
                                                                           : QStringLiteral("unmount"))
                                              : (isMountedText(finalValue) ? QStringLiteral("mount")
                                                                           : QStringLiteral("umount"))),
                                         shSingleQuote(objectName));
                }
                continue;
            }
            if (finalInh != originalInh) {
                if (finalInh) {
                    commands << QStringLiteral("zfs inherit %1 %2")
                                    .arg(shSingleQuote(prop), shSingleQuote(objectName));
                } else {
                    commands << QStringLiteral("zfs set %1 %2")
                                    .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                         shSingleQuote(objectName));
                }
                continue;
            }
            if (!finalInh && finalValue != originalValue) {
                commands << QStringLiteral("zfs set %1 %2")
                                .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                     shSingleQuote(objectName));
            }
        }
    }
    for (auto it = m_datasetPermissionsCache.cbegin(); it != m_datasetPermissionsCache.cend(); ++it) {
        const DatasetPermissionsCacheEntry& entry = it.value();
        if (!entry.loaded || !entry.dirty) {
            continue;
        }
        const QStringList parts = it.key().split(QStringLiteral("::"));
        if (parts.size() < 3) {
            continue;
        }
        bool okConn = false;
        const int connIdx = parts.at(0).toInt(&okConn);
        const QString datasetName = parts.mid(2).join(QStringLiteral("::")).trimmed();
        if (!okConn || connIdx < 0 || connIdx >= m_profiles.size() || datasetName.isEmpty()) {
            continue;
        }
        auto appendGrantCommands = [&](const QVector<DatasetPermissionGrant>& original,
                                       const QVector<DatasetPermissionGrant>& current) {
            QMap<QString, DatasetPermissionGrant> origMap;
            QMap<QString, DatasetPermissionGrant> currMap;
            for (const DatasetPermissionGrant& g : original) {
                origMap.insert(grantKey(g), g);
            }
            for (const DatasetPermissionGrant& g : current) {
                currMap.insert(grantKey(g), g);
            }
            QSet<QString> keys;
            for (auto k = origMap.cbegin(); k != origMap.cend(); ++k) keys.insert(k.key());
            for (auto k = currMap.cbegin(); k != currMap.cend(); ++k) keys.insert(k.key());
            for (const QString& key : keys) {
                const bool had = origMap.contains(key);
                const bool has = currMap.contains(key);
                const DatasetPermissionGrant before = origMap.value(key);
                const DatasetPermissionGrant after = currMap.value(key);
                const QString beforeFlags = scopeFlagsForPermission(before.scope) + targetFlagsForPermission(before.targetType, before.targetName);
                const QString afterFlags = scopeFlagsForPermission(after.scope) + targetFlagsForPermission(after.targetType, after.targetName);
                const QStringList beforeTokens = normalizeTokens(before.permissions);
                const QStringList afterTokens = normalizeTokens(after.permissions);
                if (had && (!has || afterTokens.isEmpty())) {
                    commands << QStringLiteral("zfs unallow %1%2")
                                    .arg(beforeFlags, shSingleQuote(datasetName));
                    continue;
                }
                if (!had && has) {
                    if (!afterTokens.isEmpty()) {
                        commands << QStringLiteral("zfs allow %1%2%3 %4")
                                        .arg(afterFlags,
                                             afterTokens.join(','),
                                             QString(),
                                             shSingleQuote(datasetName));
                    }
                    continue;
                }
                if (had && has && beforeTokens != afterTokens) {
                    QString cmd = QStringLiteral("zfs unallow %1%2")
                                      .arg(beforeFlags, shSingleQuote(datasetName));
                    if (!afterTokens.isEmpty()) {
                        cmd += QStringLiteral(" && zfs allow %1%2%3 %4")
                                   .arg(afterFlags,
                                        afterTokens.join(','),
                                        QString(),
                                        shSingleQuote(datasetName));
                    }
                    commands << cmd;
                }
            }
        };
        appendGrantCommands(entry.originalLocalGrants, entry.localGrants);
        appendGrantCommands(entry.originalDescendantGrants, entry.descendantGrants);
        appendGrantCommands(entry.originalLocalDescendantGrants, entry.localDescendantGrants);

        const QStringList originalCreate = normalizeTokens(entry.originalCreatePermissions);
        const QStringList currentCreate = normalizeTokens(entry.createPermissions);
        if (originalCreate != currentCreate) {
            QString cmd = QStringLiteral("zfs unallow -c %1").arg(shSingleQuote(datasetName));
            if (!currentCreate.isEmpty()) {
                cmd += QStringLiteral(" && zfs allow -c %1 %2")
                           .arg(currentCreate.join(','),
                                shSingleQuote(datasetName));
            }
            commands << cmd;
        }

        QMap<QString, DatasetPermissionSet> origSets;
        QMap<QString, DatasetPermissionSet> currSets;
        for (const DatasetPermissionSet& s : entry.originalPermissionSets) {
            origSets.insert(setKey(s), s);
        }
        for (const DatasetPermissionSet& s : entry.permissionSets) {
            currSets.insert(setKey(s), s);
        }
        QSet<QString> setKeys;
        for (auto k = origSets.cbegin(); k != origSets.cend(); ++k) setKeys.insert(k.key());
        for (auto k = currSets.cbegin(); k != currSets.cend(); ++k) setKeys.insert(k.key());
        for (const QString& key : setKeys) {
            const bool had = origSets.contains(key);
            const bool has = currSets.contains(key);
            const DatasetPermissionSet before = origSets.value(key);
            const DatasetPermissionSet after = currSets.value(key);
            const QStringList beforeTokens = normalizeTokens(before.permissions);
            const QStringList afterTokens = normalizeTokens(after.permissions);
            if (had && !has) {
                commands << QStringLiteral("zfs unallow -s %1 %2")
                                .arg(before.name, shSingleQuote(datasetName));
                continue;
            }
            if (!had && has) {
                if (!afterTokens.isEmpty()) {
                    commands << QStringLiteral("zfs allow -s %1 %2 %3")
                                    .arg(after.name,
                                         afterTokens.join(','),
                                         shSingleQuote(datasetName));
                }
                continue;
            }
            if (had && has && (before.name != after.name || beforeTokens != afterTokens)) {
                QString cmd = QStringLiteral("zfs unallow -s %1 %2")
                                  .arg(before.name, shSingleQuote(datasetName));
                if (!afterTokens.isEmpty()) {
                    cmd += QStringLiteral(" && zfs allow -s %1 %2 %3")
                               .arg(after.name,
                                    afterTokens.join(','),
                                    shSingleQuote(datasetName));
                }
                commands << cmd;
            }
        }
    }
    return commands;
}

void MainWindow::updateApplyPropsButtonState() {
    if (m_btnApplyConnContentProps) {
        const QStringList pendingCommands = pendingConnContentApplyCommands();
        if (m_propsSide == QStringLiteral("conncontent")) {
            m_btnApplyConnContentProps->setEnabled(!pendingCommands.isEmpty());
            m_btnApplyConnContentProps->setToolTip(
                pendingCommands.isEmpty()
                    ? trk(QStringLiteral("t_apply_changes_tt_001"),
                          QStringLiteral("Sin cambios pendientes."),
                          QStringLiteral("No pending changes."),
                          QStringLiteral("没有待应用的更改。"))
                    : pendingCommands.join(QLatin1Char('\n')));
            return;
        }
        if (!pendingCommands.isEmpty()) {
            m_btnApplyConnContentProps->setEnabled(true);
            m_btnApplyConnContentProps->setToolTip(pendingCommands.join(QLatin1Char('\n')));
            return;
        }
        m_btnApplyConnContentProps->setToolTip(
            trk(QStringLiteral("t_apply_changes_tt_001"),
                QStringLiteral("Sin cambios pendientes."),
                QStringLiteral("No pending changes."),
                QStringLiteral("没有待应用的更改。")));
    }
    const DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    bool eligible = ctx.valid && ctx.snapshotName.isEmpty() && (ctx.datasetName == m_propsDataset);
    if (m_propsSide == QStringLiteral("conncontent")) {
        // En vista de Conexiones hay dos treeviews (origen/destino) y la referencia
        // activa puede no coincidir temporalmente con el que originó la edición.
        // Para habilitar "Aplicar cambios" usamos el dataset actualmente cargado.
        eligible = !m_propsDataset.trimmed().isEmpty() && !m_propsDataset.contains('@');
    }
    auto hasEffectiveChanges = [](QTableWidget* table,
                                  const QMap<QString, QString>& originals,
                                  const QMap<QString, bool>& originalInherit) -> bool {
        if (!table) {
            return false;
        }
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* pk = table->item(r, 0);
            QTableWidgetItem* pv = table->item(r, 1);
            QTableWidgetItem* pi = table->item(r, 2);
            if (!pk || !pv || !pi) {
                continue;
            }
            const QString prop = propKeyFromItem(pk);
            if (prop.isEmpty()) {
                continue;
            }
            const bool inh = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
            const QString now = pv->text();
            if (inh != originalInherit.value(prop, false) || now != originals.value(prop)) {
                return true;
            }
        }
        return false;
    };
    QTableWidget* activePropsTable = m_connContentPropsTable;
    const bool hasChanges = hasEffectiveChanges(activePropsTable, m_propsOriginalValues, m_propsOriginalInherit);
    const bool baseEnable = m_propsDirty && eligible && hasChanges;
    if (m_btnApplyConnContentProps) {
        m_btnApplyConnContentProps->setEnabled(baseEnable && m_propsSide == QStringLiteral("conncontent"));
    }
}
