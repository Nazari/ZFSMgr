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

namespace {
constexpr int kPropKeyRole = Qt::UserRole + 777;

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

bool isDatasetPropertyEditable(const QString& propName, const QString& datasetType, const QString& source, const QString& readonly) {
    const QString prop = propName.trimmed().toLower();
    const QString dsType = datasetType.trimmed().toLower();
    const QString src = source.trimmed();
    const QString ro = readonly.trimmed().toLower();
    if (prop.isEmpty()) {
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
            QStringLiteral("driveletter")
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

void MainWindow::refreshDatasetProperties(const QString& side) {
    beginUiBusy();
    auto saveCurrentDraft = [this]() {
        if (!m_propsDirty || m_propsSide.isEmpty() || m_propsDataset.isEmpty()) {
            return;
        }
        QString currToken;
        if (m_propsSide == QStringLiteral("origin")) {
            currToken = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
        } else if (m_propsSide == QStringLiteral("dest")) {
            currToken = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
        } else if (m_propsSide == QStringLiteral("conncontent")) {
            currToken = m_connContentToken;
        } else {
            return;
        }
        if (currToken.isEmpty()) {
            return;
        }
        QTableWidget* currTable = (m_propsSide == QStringLiteral("conncontent")) ? m_connContentPropsTable : m_datasetPropsTable;
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
        dataset = m_originSelectedDataset;
        snapshot = m_originSelectedSnapshot;
    } else if (side == QStringLiteral("dest")) {
        dataset = m_destSelectedDataset;
        snapshot = m_destSelectedSnapshot;
    } else if (side == QStringLiteral("advanced")) {
        const auto selected = m_advTree ? m_advTree->selectedItems() : QList<QTreeWidgetItem*>{};
        if (!selected.isEmpty()) {
            dataset = selected.first()->data(0, Qt::UserRole).toString();
            snapshot = selected.first()->data(1, Qt::UserRole).toString();
        }
    } else if (side == QStringLiteral("conncontent")) {
        const auto selected = m_connContentTree ? m_connContentTree->selectedItems() : QList<QTreeWidgetItem*>{};
        if (!selected.isEmpty()) {
            dataset = selected.first()->data(0, Qt::UserRole).toString();
            snapshot = selected.first()->data(1, Qt::UserRole).toString();
        }
    }
    QTableWidget* table = m_datasetPropsTable;
    if (side == QStringLiteral("advanced")) {
        table = m_advPropsTable;
    } else if (side == QStringLiteral("conncontent")) {
        table = m_connContentPropsTable;
    }
    if (!table) {
        endUiBusy();
        return;
    }
    if (dataset.isEmpty()) {
        setTablePopulationMode(table, true);
        table->setRowCount(0);
        setTablePopulationMode(table, false);
        if (side == QStringLiteral("advanced")) {
            m_advPropsDataset.clear();
            m_advPropsOriginalValues.clear();
            m_advPropsOriginalInherit.clear();
            m_advPropsDirty = false;
        } else {
            m_propsDataset.clear();
            m_propsSide = side;
            m_propsOriginalValues.clear();
            m_propsOriginalInherit.clear();
            m_propsDirty = false;
        }
        updateApplyPropsButtonState();
        endUiBusy();
        return;
    }

    QString token;
    if (side == QStringLiteral("origin")) {
        token = m_originPoolCombo->currentData().toString();
    } else if (side == QStringLiteral("dest")) {
        token = m_destPoolCombo->currentData().toString();
    } else if (side == QStringLiteral("advanced")) {
        token = m_advPoolCombo->currentData().toString();
    } else if (side == QStringLiteral("conncontent")) {
        token = m_connContentToken;
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        endUiBusy();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const QString key = datasetCacheKey(connIdx, poolName);
    const auto it = m_poolDatasetCache.constFind(key);
    if (it == m_poolDatasetCache.constEnd()) {
        endUiBusy();
        return;
    }
    const PoolDatasetCache& cache = it.value();
    const auto recIt = cache.recordByName.constFind(dataset);
    if (recIt == cache.recordByName.constEnd()) {
        endUiBusy();
        return;
    }
    const DatasetRecord& rec = recIt.value();
    const QString objectName = snapshot.isEmpty() ? dataset : QStringLiteral("%1@%2").arg(dataset, snapshot);
    const QString draftKey = propsDraftKey(side, token, objectName);
    const ConnectionProfile& p = m_profiles[connIdx];

    QString datasetType = objectName.contains('@') ? QStringLiteral("snapshot") : QStringLiteral("filesystem");
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

    struct PropRow {
        QString prop;
        QString value;
        QString source;
        QString readonly;
    };
    QVector<PropRow> rawRows;
    rawRows.push_back({QStringLiteral("dataset"), objectName, QString(), QStringLiteral("true")});

    QString out;
    QString err;
    int rc = -1;
    bool propsLoadedFromLibzfs = false;
    if (isLocalConnection(connIdx) && detectLocalLibzfs()) {
        const QStringList localWantedProps = {
            QStringLiteral("mountpoint"), QStringLiteral("canmount"), QStringLiteral("recordsize"),
            QStringLiteral("quota"), QStringLiteral("reservation"), QStringLiteral("refquota"),
            QStringLiteral("refreservation"), QStringLiteral("snapdir"), QStringLiteral("exec"),
            QStringLiteral("setuid"), QStringLiteral("devices"), QStringLiteral("driveletter"),
            QStringLiteral("volsize"), QStringLiteral("volblocksize"), QStringLiteral("volmode"),
            QStringLiteral("snapdev"), QStringLiteral("atime"), QStringLiteral("relatime"),
            QStringLiteral("readonly"), QStringLiteral("compression"), QStringLiteral("checksum"),
            QStringLiteral("sync"), QStringLiteral("logbias"), QStringLiteral("primarycache"),
            QStringLiteral("secondarycache"), QStringLiteral("dedup"), QStringLiteral("copies"),
            QStringLiteral("acltype"), QStringLiteral("aclinherit"), QStringLiteral("xattr"),
            QStringLiteral("normalization"), QStringLiteral("casesensitivity"), QStringLiteral("utf8only"),
            QStringLiteral("keylocation"), QStringLiteral("comment")
        };
        QMap<QString, QString> propValues;
        QString libDetail;
        if (getLocalDatasetPropsLibzfs(objectName, localWantedProps, propValues, &libDetail)) {
            propsLoadedFromLibzfs = true;
            appLog(QStringLiteral("INFO"),
                   QStringLiteral("Dataset props via libzfs %1::%2 (%3)")
                       .arg(p.name, objectName, libDetail));
            for (auto itp = propValues.constBegin(); itp != propValues.constEnd(); ++itp) {
                const QString prop = itp.key().trimmed();
                const QString val = itp.value().trimmed();
                if (!isDatasetPropertyEditable(prop, datasetType, QStringLiteral("local"), QStringLiteral("false"))) {
                    continue;
                }
                rawRows.push_back({prop, val, QStringLiteral("local"), QStringLiteral("false")});
            }
        } else {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Dataset props libzfs fallback to CLI %1::%2 (%3)")
                       .arg(p.name, objectName, libDetail));
        }
    }

    if (!propsLoadedFromLibzfs) {
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
    }
    if (propsLoadedFromLibzfs || rc == 0) {
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        if (!propsLoadedFromLibzfs) {
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
                if (!isDatasetPropertyEditable(prop, datasetType, source, ro)) {
                    continue;
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

    m_loadingPropsTable = true;
    setTablePopulationMode(table, true);
    table->setRowCount(0);
    if (side == QStringLiteral("advanced")) {
        m_advPropsOriginalValues.clear();
        m_advPropsOriginalInherit.clear();
        m_advPropsDataset = objectName;
    } else {
        m_propsOriginalValues.clear();
        m_propsOriginalInherit.clear();
        m_propsSide = side;
        m_propsDataset = objectName;
    }
    const QSet<QString> inheritableProps = {QStringLiteral("mountpoint"), QStringLiteral("canmount")};
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
                    if (table == m_advPropsTable) {
                        onAdvancedPropsCellChanged(rr, 1);
                    } else {
                        onDatasetPropsCellChanged(rr, 1);
                    }
                    break;
                }
            });
        }
        auto* inh = new PinnedSortItem();
        inh->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        inh->setData(kPropKeyRole, row.prop);
        if (inheritableProps.contains(row.prop)) {
            inh->setFlags((inh->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) & ~Qt::ItemIsEditable);
            inh->setCheckState(Qt::Unchecked);
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
        if (side == QStringLiteral("advanced")) {
            m_advPropsOriginalValues[row.prop] = row.value;
            m_advPropsOriginalInherit[row.prop] = false;
        } else {
            m_propsOriginalValues[row.prop] = row.value;
            m_propsOriginalInherit[row.prop] = false;
        }
    }
    if (side != QStringLiteral("advanced")) {
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
    }
    if (side == QStringLiteral("advanced")) {
        m_advPropsDirty = false;
    }
    setTablePopulationMode(table, false);
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
    endUiBusy();
}

void MainWindow::onDatasetPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2)) {
        return;
    }
    QTableWidget* table = qobject_cast<QTableWidget*>(sender());
    if (!table) {
        table = (m_propsSide == QStringLiteral("conncontent")) ? m_connContentPropsTable : m_datasetPropsTable;
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
    Q_UNUSED(pk);
    Q_UNUSED(pv);
    Q_UNUSED(pi);
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

void MainWindow::onAdvancedPropsCellChanged(int row, int col) {
    if (m_loadingPropsTable || (col != 1 && col != 2) || !m_advPropsTable) {
        return;
    }
    Q_UNUSED(row);
    m_advPropsDirty = false;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* rk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* rv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* ri = m_advPropsTable->item(r, 2);
        if (!rk || !rv || !ri) {
            continue;
        }
        const QString key = propKeyFromItem(rk);
        const bool inh = (ri->flags() & Qt::ItemIsUserCheckable) && ri->checkState() == Qt::Checked;
        if (inh != m_advPropsOriginalInherit.value(key, false)
            || rv->text() != m_advPropsOriginalValues.value(key)) {
            m_advPropsDirty = true;
            break;
        }
    }
    updateApplyPropsButtonState();
}

void MainWindow::applyDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_propsDirty || m_propsDataset.isEmpty() || m_propsSide.isEmpty()) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    if (!ctx.valid || ctx.datasetName != m_propsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_seleccione_615ce3"),
                                 QStringLiteral("Seleccione un dataset activo para aplicar cambios."),
                                 QStringLiteral("Select an active dataset to apply changes."),
                                 QStringLiteral("请选择一个活动数据集以应用更改。")));
        return;
    }

    QTableWidget* propsTable = (m_propsSide == QStringLiteral("conncontent")) ? m_connContentPropsTable : m_datasetPropsTable;
    if (!propsTable) {
        return;
    }
    QString currentToken;
    if (m_propsSide == QStringLiteral("origin")) {
        currentToken = m_originPoolCombo ? m_originPoolCombo->currentData().toString() : QString();
    } else if (m_propsSide == QStringLiteral("dest")) {
        currentToken = m_destPoolCombo ? m_destPoolCombo->currentData().toString() : QString();
    } else if (m_propsSide == QStringLiteral("conncontent")) {
        currentToken = m_connContentToken;
    }
    const QString currentDraftKey = currentToken.isEmpty() ? QString()
                                                           : propsDraftKey(m_propsSide, currentToken, m_propsDataset);

    QStringList subcmds;
    struct PropChange {
        bool inherit{false};
        QString prop;
        QString value;
    };
    QVector<PropChange> propChanges;
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
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado") || prop == QStringLiteral("Tamaño")) {
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
            reloadDatasetSide(m_propsSide);
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
        reloadDatasetSide(m_propsSide);
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

void MainWindow::applyAdvancedDatasetPropertyChanges() {
    if (actionsLocked()) {
        return;
    }
    if (!m_advPropsDirty || m_advPropsDataset.isEmpty() || !m_advPropsTable) {
        return;
    }
    DatasetSelectionContext ctx = currentDatasetSelection(QStringLiteral("advanced"));
    if (!ctx.valid || ctx.datasetName != m_advPropsDataset || !ctx.snapshotName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                             trk(QStringLiteral("t_seleccione_615ce3"),
                                 QStringLiteral("Seleccione un dataset activo para aplicar cambios."),
                                 QStringLiteral("Select an active dataset to apply changes."),
                                 QStringLiteral("请选择一个活动数据集以应用更改。")));
        return;
    }

    QStringList subcmds;
    struct PropChange {
        bool inherit{false};
        QString prop;
        QString value;
    };
    QVector<PropChange> propChanges;
    bool renameRequested = false;
    QString renameOld = ctx.datasetName;
    QString renameNew = ctx.datasetName;
    QString targetDataset = ctx.datasetName;
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_advPropsTable->item(r, 1);
        if (!pk || !pv) {
            continue;
        }
        const QString prop = propKeyFromItem(pk);
        if (prop != QStringLiteral("dataset")) {
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_advPropsOriginalValues.value(prop).trimmed();
        if (!now.isEmpty() && now != old) {
            renameRequested = true;
            renameNew = now;
            targetDataset = now;
        }
        break;
    }
    for (int r = 0; r < m_advPropsTable->rowCount(); ++r) {
        QTableWidgetItem* pk = m_advPropsTable->item(r, 0);
        QTableWidgetItem* pv = m_advPropsTable->item(r, 1);
        QTableWidgetItem* pi = m_advPropsTable->item(r, 2);
        if (!pk || !pv || !pi) {
            continue;
        }
        const QString prop = propKeyFromItem(pk);
        if (prop.isEmpty() || prop == QStringLiteral("dataset") || prop == QStringLiteral("estado") || prop == QStringLiteral("Tamaño")) {
            continue;
        }
        const bool inheritChecked = (pi->flags() & Qt::ItemIsUserCheckable) && (pi->checkState() == Qt::Checked);
        if (inheritChecked) {
            subcmds << QStringLiteral("zfs inherit %1 %2").arg(shSingleQuote(prop), shSingleQuote(targetDataset));
            propChanges.push_back(PropChange{true, prop, QString()});
            continue;
        }
        const QString now = pv->text().trimmed();
        const QString old = m_advPropsOriginalValues.value(prop).trimmed();
        if (now == old) {
            continue;
        }
        const QString assign = prop + QStringLiteral("=") + now;
        subcmds << QStringLiteral("zfs set %1 %2").arg(shSingleQuote(assign), shSingleQuote(targetDataset));
        propChanges.push_back(PropChange{false, prop, now});
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
            if (executeDatasetAction(QStringLiteral("advanced"), QStringLiteral("Aplicar propiedades"), ctx, fallbackCmd, 60000, isWinFallback)) {
                updateAdvancedSelectionUi(renameNew, QString());
                m_advPropsDirty = false;
                updateApplyPropsButtonState();
            }
            return;
        }
        appLog(QStringLiteral("NORMAL"),
               QStringLiteral("Aplicar propiedades rename finalizado (%1)").arg(mwhelpers::oneLine(detail)));
        invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
        updateAdvancedSelectionUi(renameNew, QString());
        ctx.datasetName = renameNew;
        localRenameDone = true;
        setActionsLocked(false);
    }

    if (subcmds.isEmpty()) {
        if (localRenameDone) {
            m_advPropsDirty = false;
            updateApplyPropsButtonState();
            reloadDatasetSide(QStringLiteral("advanced"));
            return;
        }
        if (renameRequested) {
            subcmds << QStringLiteral("zfs rename %1 %2").arg(shSingleQuote(renameOld), shSingleQuote(renameNew));
        }
    } else if (renameRequested && !localRenameDone) {
        subcmds.prepend(QStringLiteral("zfs rename %1 %2").arg(shSingleQuote(renameOld), shSingleQuote(renameNew)));
    }
    if (subcmds.isEmpty()) {
        m_advPropsDirty = false;
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
            if (executeDatasetAction(QStringLiteral("advanced"), QStringLiteral("Aplicar propiedades"), ctx, cmdFallback, 60000, isWinFallback)) {
                if (targetDataset != ctx.datasetName) {
                    updateAdvancedSelectionUi(targetDataset, QString());
                }
                m_advPropsDirty = false;
                updateApplyPropsButtonState();
            }
            return;
        }
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Aplicar propiedades finalizado"));
        invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
        reloadDatasetSide(QStringLiteral("advanced"));
        if (targetDataset != ctx.datasetName) {
            updateAdvancedSelectionUi(targetDataset, QString());
        }
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
        setActionsLocked(false);
        return;
    }

    const bool isWin = isWindowsConnection(ctx.connIdx);
    const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                              : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
    if (executeDatasetAction(QStringLiteral("advanced"), QStringLiteral("Aplicar propiedades"), ctx, cmd, 60000, isWin)) {
        if (targetDataset != ctx.datasetName) {
            updateAdvancedSelectionUi(targetDataset, QString());
        }
        m_advPropsDirty = false;
        updateApplyPropsButtonState();
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const DatasetSelectionContext ctx = currentDatasetSelection(m_propsSide);
    const bool eligible = ctx.valid && ctx.snapshotName.isEmpty() && (ctx.datasetName == m_propsDataset);
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
            if (prop.isEmpty() || prop == QStringLiteral("estado")) {
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
    QTableWidget* activePropsTable = (m_propsSide == QStringLiteral("conncontent")) ? m_connContentPropsTable : m_datasetPropsTable;
    const bool hasChanges = hasEffectiveChanges(activePropsTable, m_propsOriginalValues, m_propsOriginalInherit);
    const bool baseEnable = m_propsDirty && eligible && hasChanges;
    if (m_btnApplyDatasetProps) {
        m_btnApplyDatasetProps->setEnabled(baseEnable && m_propsSide != QStringLiteral("conncontent"));
    }
    if (m_btnApplyConnContentProps) {
        m_btnApplyConnContentProps->setEnabled(baseEnable && m_propsSide == QStringLiteral("conncontent"));
    }
    const DatasetSelectionContext actx = currentDatasetSelection(QStringLiteral("advanced"));
    const bool aok = actx.valid && actx.snapshotName.isEmpty() && (actx.datasetName == m_advPropsDataset);
    if (m_btnApplyAdvancedProps) {
        const bool advHasChanges =
            hasEffectiveChanges(m_advPropsTable, m_advPropsOriginalValues, m_advPropsOriginalInherit);
        m_btnApplyAdvancedProps->setEnabled(m_advPropsDirty && aok && advHasChanges);
    }
}
