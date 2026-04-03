#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QAbstractScrollArea>
#include <QComboBox>
#include <QFont>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
constexpr int kPropKeyRole = Qt::UserRole + 777;
constexpr int kPropValueEditableRole = Qt::UserRole + 778;
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
        || prop == QStringLiteral("snapshot")
        || prop == QStringLiteral("canmount")) {
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

template <typename Rows>
bool encryptionDisabledForRows(const Rows& rows) {
    for (const auto& row : rows) {
        if (row.prop.compare(QStringLiteral("encryption"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        return row.value.trimmed().compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0;
    }
    return false;
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

void applyInheritedStateToPropsRow(QTableWidget* table, int row) {
    if (!table || row < 0 || row >= table->rowCount()) {
        return;
    }
    QTableWidgetItem* valueItem = table->item(row, 1);
    QTableWidgetItem* inheritItem = table->item(row, 2);
    if (!valueItem || !inheritItem) {
        return;
    }
    const bool baseEditable = valueItem->data(kPropValueEditableRole).toBool();
    const bool inheritable = (inheritItem->flags() & Qt::ItemIsUserCheckable);
    const bool inheritOn = inheritable && inheritItem->checkState() == Qt::Checked;
    if (QWidget* editor = table->cellWidget(row, 1)) {
        editor->setEnabled(baseEditable && !inheritOn);
    }
    Qt::ItemFlags flags = valueItem->flags();
    if (baseEditable && !inheritOn) {
        flags |= Qt::ItemIsEditable;
    } else {
        flags &= ~Qt::ItemIsEditable;
    }
    valueItem->setFlags(flags);
}

QString propsDraftKey(const QString& side, const QString& token, const QString& objectName) {
    return QStringLiteral("%1|%2|%3")
        .arg(side.trimmed().toLower(),
             token.trimmed(),
             objectName.trimmed());
}

QString findMapValueCaseInsensitive(const QMap<QString, QString>& map, const QString& wantedKey) {
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().compare(wantedKey, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return QString();
}

bool gsaBoolOn(const QString& value) {
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("on")
           || v == QStringLiteral("yes")
           || v == QStringLiteral("true")
           || v == QStringLiteral("1");
}

bool gsaParseNonNegativeInt(const QString& value, int& out) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        out = 0;
        return true;
    }
    bool ok = false;
    const int n = trimmed.toInt(&ok);
    if (!ok || n < 0) {
        return false;
    }
    out = n;
    return true;
}

bool datasetIsSameOrDescendantOf(const QString& dataset, const QString& ancestor) {
    const QString d = dataset.trimmed();
    const QString a = ancestor.trimmed();
    return d == a || d.startsWith(a + QLatin1Char('/'));
}

bool mountedStateFromText(const QString& value, bool* mountedOut) {
    const QString s = value.trimmed().toLower();
    if (s == QStringLiteral("montado")
        || s == QStringLiteral("mounted")
        || s == QStringLiteral("已挂载")
        || s == QStringLiteral("on")
        || s == QStringLiteral("yes")
        || s == QStringLiteral("true")
        || s == QStringLiteral("1")) {
        if (mountedOut) {
            *mountedOut = true;
        }
        return true;
    }
    if (s == QStringLiteral("desmontado")
        || s == QStringLiteral("unmounted")
        || s == QStringLiteral("未挂载")
        || s == QStringLiteral("off")
        || s == QStringLiteral("no")
        || s == QStringLiteral("false")
        || s == QStringLiteral("0")) {
        if (mountedOut) {
            *mountedOut = false;
        }
        return true;
    }
    return false;
}

QString gsaComparableValue(const QString& propName, const QString& rawValue) {
    const QString prop = propName.trimmed();
    const QString value = rawValue.trimmed();
    if (!prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
        return rawValue;
    }
    if (prop.compare(QStringLiteral("org.fc16.gsa:destino"), Qt::CaseInsensitive) == 0) {
        return (value == QStringLiteral("-")) ? QString() : rawValue;
    }
    if (value.isEmpty() || value == QStringLiteral("-")) {
        if (prop.compare(QStringLiteral("org.fc16.gsa:horario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:diario"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:semanal"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:mensual"), Qt::CaseInsensitive) == 0
            || prop.compare(QStringLiteral("org.fc16.gsa:anual"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("0");
        }
        return QStringLiteral("off");
    }
    return rawValue;
}

} // namespace

bool MainWindow::validatePendingGsaDrafts(QString* errorOut) {
    struct GsaState {
        int connIdx{-1};
        QString poolName;
        QString datasetName;
        bool enabled{false};
        bool recursive{false};
        bool level{false};
        int hourly{0};
        int daily{0};
        int weekly{0};
        int monthly{0};
        int yearly{0};
        QString destination;
        bool hasExplicitConfig{false};
    };

    auto fail = [errorOut](const QString& msg) {
        if (errorOut) {
            *errorOut = msg;
        }
        return false;
    };

    QMap<QString, GsaState> statesByKey;
    for (auto itConn = m_connInfoById.cbegin(); itConn != m_connInfoById.cend(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend(); ++itPool) {
            const int connIdx = itConn->connIdx;
            const QString poolName = itPool->key.poolName.trimmed();
            if (connIdx < 0 || poolName.isEmpty()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                const QString datasetName = itDs.key().trimmed();
                if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@'))
                    || itDs->runtime.datasetType.trimmed().compare(QStringLiteral("filesystem"), Qt::CaseInsensitive) != 0) {
                    continue;
                }

                QMap<QString, QString> propValues;
                QMap<QString, QString> propSources;
                for (const DatasetPropCacheRow& row : itDs->runtime.propertyRows) {
                    propValues.insert(row.prop, row.value);
                    propSources.insert(row.prop, row.source);
                }

                const QString token = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
                const QString liveKey = QStringLiteral("%1|%2").arg(token, datasetName);
                const auto liveIt = m_connContentPropValuesByObject.constFind(liveKey);
                if (liveIt != m_connContentPropValuesByObject.cend()) {
                    for (auto vit = liveIt->cbegin(); vit != liveIt->cend(); ++vit) {
                        propValues[vit.key()] = vit.value();
                    }
                }

                const DatasetPropsDraft draft =
                    propertyDraftForObject(QStringLiteral("conncontent"), token, datasetName);
                bool hasExplicitDraftGsaEdit = false;
                if (draft.dirty) {
                    for (auto vit = draft.valuesByProp.cbegin(); vit != draft.valuesByProp.cend(); ++vit) {
                        propValues[vit.key()] = vit.value();
                        if (vit.key().startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                            hasExplicitDraftGsaEdit = true;
                        }
                    }
                    for (auto iit = draft.inheritByProp.cbegin(); iit != draft.inheritByProp.cend(); ++iit) {
                        if (iit.value()) {
                            propValues.remove(iit.key());
                            propSources.remove(iit.key());
                        }
                        if (iit.key().startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                            hasExplicitDraftGsaEdit = true;
                        }
                    }
                }

                bool hasExplicitRuntimeGsaConfig = false;
                for (auto sit = propSources.cbegin(); sit != propSources.cend(); ++sit) {
                    if (!sit.key().startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    const QString source = sit.value().trimmed().toLower();
                    if (!source.startsWith(QStringLiteral("inherited"))
                        && source != QStringLiteral("-")
                        && !source.isEmpty()) {
                        hasExplicitRuntimeGsaConfig = true;
                        break;
                    }
                }

                GsaState state;
                state.connIdx = connIdx;
                state.poolName = poolName;
                state.datasetName = datasetName;
                state.hasExplicitConfig = hasExplicitRuntimeGsaConfig || hasExplicitDraftGsaEdit;
                state.enabled = gsaBoolOn(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:activado")));
                state.recursive = gsaBoolOn(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:recursivo")));
                state.level = gsaBoolOn(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:nivelar")));
                state.destination = findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:destino")).trimmed();

                if (!state.hasExplicitConfig) {
                    continue;
                }

                if (!gsaParseNonNegativeInt(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:horario")), state.hourly)) {
                    return fail(trk(QStringLiteral("t_gsa_invalid_hourly_001"),
                                    QStringLiteral("La retención horaria de %1 no es válida. Debe ser un entero mayor o igual que 0."),
                                    QStringLiteral("The hourly retention for %1 is invalid. It must be an integer greater than or equal to 0."),
                                    QStringLiteral("%1 的每小时保留值无效。它必须是大于或等于 0 的整数。")).arg(datasetName));
                }
                if (!gsaParseNonNegativeInt(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:diario")), state.daily)) {
                    return fail(trk(QStringLiteral("t_gsa_invalid_daily_001"),
                                    QStringLiteral("La retención diaria de %1 no es válida. Debe ser un entero mayor o igual que 0."),
                                    QStringLiteral("The daily retention for %1 is invalid. It must be an integer greater than or equal to 0."),
                                    QStringLiteral("%1 的每日保留值无效。它必须是大于或等于 0 的整数。")).arg(datasetName));
                }
                if (!gsaParseNonNegativeInt(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:semanal")), state.weekly)) {
                    return fail(trk(QStringLiteral("t_gsa_invalid_weekly_001"),
                                    QStringLiteral("La retención semanal de %1 no es válida. Debe ser un entero mayor o igual que 0."),
                                    QStringLiteral("The weekly retention for %1 is invalid. It must be an integer greater than or equal to 0."),
                                    QStringLiteral("%1 的每周保留值无效。它必须是大于或等于 0 的整数。")).arg(datasetName));
                }
                if (!gsaParseNonNegativeInt(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:mensual")), state.monthly)) {
                    return fail(trk(QStringLiteral("t_gsa_invalid_monthly_001"),
                                    QStringLiteral("La retención mensual de %1 no es válida. Debe ser un entero mayor o igual que 0."),
                                    QStringLiteral("The monthly retention for %1 is invalid. It must be an integer greater than or equal to 0."),
                                    QStringLiteral("%1 的每月保留值无效。它必须是大于或等于 0 的整数。")).arg(datasetName));
                }
                if (!gsaParseNonNegativeInt(findMapValueCaseInsensitive(propValues, QStringLiteral("org.fc16.gsa:anual")), state.yearly)) {
                    return fail(trk(QStringLiteral("t_gsa_invalid_yearly_001"),
                                    QStringLiteral("La retención anual de %1 no es válida. Debe ser un entero mayor o igual que 0."),
                                    QStringLiteral("The yearly retention for %1 is invalid. It must be an integer greater than or equal to 0."),
                                    QStringLiteral("%1 的每年保留值无效。它必须是大于或等于 0 的整数。")).arg(datasetName));
                }

                if (state.level && state.destination.isEmpty()) {
                    return fail(trk(QStringLiteral("t_gsa_level_dest_required_001"),
                                    QStringLiteral("La programación GSA de %1 tiene Nivelar=on pero no tiene Destino."),
                                    QStringLiteral("GSA scheduling for %1 has Level=on but no Destination."),
                                    QStringLiteral("%1 的 GSA 计划启用了层级同步，但未指定目标。")).arg(datasetName));
                }
                if (state.level && !state.destination.contains(QStringLiteral("::"))) {
                    return fail(trk(QStringLiteral("t_gsa_dest_format_001"),
                                    QStringLiteral("El destino GSA de %1 debe tener formato Con::Pool/Dataset."),
                                    QStringLiteral("The GSA destination for %1 must use the Con::Pool/Dataset format."),
                                    QStringLiteral("%1 的 GSA 目标必须使用 Con::Pool/Dataset 格式。")).arg(datasetName));
                }
                if (state.level) {
                    const QString destConnName = state.destination.section(QStringLiteral("::"), 0, 0).trimmed();
                    if (connectionIndexByNameOrId(destConnName) < 0) {
                        return fail(trk(QStringLiteral("t_gsa_dest_conn_missing_001"),
                                        QStringLiteral("El destino GSA de %1 referencia una conexión inexistente: %2."),
                                        QStringLiteral("The GSA destination for %1 references a missing connection: %2."),
                                        QStringLiteral("%1 的 GSA 目标引用了不存在的连接：%2。")).arg(datasetName, destConnName));
                    }
                }

                if (state.enabled) {
                    if (state.hourly <= 0 && state.daily <= 0 && state.weekly <= 0 && state.monthly <= 0 && state.yearly <= 0) {
                        return fail(trk(QStringLiteral("t_gsa_requires_retention_001"),
                                        QStringLiteral("La programación GSA de %1 está activada pero no tiene ninguna retención mayor que 0."),
                                        QStringLiteral("GSA scheduling for %1 is enabled but it does not have any retention greater than 0."),
                                        QStringLiteral("%1 的 GSA 计划已启用，但没有任何大于 0 的保留值。")).arg(datasetName));
                    }
                    if (!state.destination.isEmpty() && !state.destination.contains(QStringLiteral("::"))) {
                        return fail(trk(QStringLiteral("t_gsa_dest_format_001"),
                                        QStringLiteral("El destino GSA de %1 debe tener formato Con::Pool/Dataset."),
                                        QStringLiteral("The GSA destination for %1 must use the Con::Pool/Dataset format."),
                                        QStringLiteral("%1 的 GSA 目标必须使用 Con::Pool/Dataset 格式。")).arg(datasetName));
                    }
                    if (!state.destination.isEmpty()) {
                        const QString destConnName = state.destination.section(QStringLiteral("::"), 0, 0).trimmed();
                        if (connectionIndexByNameOrId(destConnName) < 0) {
                            return fail(trk(QStringLiteral("t_gsa_dest_conn_missing_001"),
                                            QStringLiteral("El destino GSA de %1 referencia una conexión inexistente: %2."),
                                            QStringLiteral("The GSA destination for %1 references a missing connection: %2."),
                                            QStringLiteral("%1 的 GSA 目标引用了不存在的连接：%2。")).arg(datasetName, destConnName));
                        }
                    }
                }

                statesByKey.insert(QStringLiteral("%1::%2::%3").arg(connIdx).arg(poolName, datasetName), state);
            }
        }
    }

    QVector<GsaState> enabledStates;
    enabledStates.reserve(statesByKey.size());
    for (auto it = statesByKey.cbegin(); it != statesByKey.cend(); ++it) {
        if (it.value().enabled && it.value().hasExplicitConfig) {
            enabledStates.push_back(it.value());
        }
    }
    std::sort(enabledStates.begin(), enabledStates.end(), [](const GsaState& a, const GsaState& b) {
        if (a.connIdx != b.connIdx) return a.connIdx < b.connIdx;
        const int poolCmp = QString::compare(a.poolName, b.poolName, Qt::CaseInsensitive);
        if (poolCmp != 0) return poolCmp < 0;
        return QString::compare(a.datasetName, b.datasetName, Qt::CaseInsensitive) < 0;
    });

    for (int i = 0; i < enabledStates.size(); ++i) {
        const GsaState& a = enabledStates.at(i);
        for (int j = i + 1; j < enabledStates.size(); ++j) {
            const GsaState& b = enabledStates.at(j);
            if (a.connIdx != b.connIdx || a.poolName.compare(b.poolName, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (a.recursive && datasetIsSameOrDescendantOf(b.datasetName, a.datasetName) && a.datasetName.compare(b.datasetName, Qt::CaseInsensitive) != 0) {
                return fail(trk(QStringLiteral("t_gsa_recursive_child_conflict_001"),
                                QStringLiteral("No se puede programar %1 porque %2 ya tiene una programación GSA recursiva."),
                                QStringLiteral("%1 cannot be scheduled because %2 already has a recursive GSA schedule."),
                                QStringLiteral("无法为 %1 设置计划，因为 %2 已经有递归 GSA 计划。")).arg(b.datasetName, a.datasetName));
            }
            if (b.recursive && datasetIsSameOrDescendantOf(a.datasetName, b.datasetName) && a.datasetName.compare(b.datasetName, Qt::CaseInsensitive) != 0) {
                return fail(trk(QStringLiteral("t_gsa_recursive_parent_conflict_001"),
                                QStringLiteral("No se puede programar %1 porque %2 ya tiene una programación GSA recursiva."),
                                QStringLiteral("%1 cannot be scheduled because %2 already has a recursive GSA schedule."),
                                QStringLiteral("无法为 %1 设置计划，因为 %2 已经有递归 GSA 计划。")).arg(a.datasetName, b.datasetName));
            }
        }
    }

    if (errorOut) {
        errorOut->clear();
    }
    return true;
}

QString MainWindow::pendingDatasetRenameCommand(const PendingDatasetRenameDraft& draft) const {
    return QStringLiteral("zfs rename %1 %2")
        .arg(shSingleQuote(draft.sourceName.trimmed()),
             shSingleQuote(draft.targetName.trimmed()));
}

bool MainWindow::queuePendingDatasetRename(const PendingDatasetRenameDraft& draft, QString* errorOut) {
    auto fail = [errorOut](const QString& text) {
        if (errorOut) {
            *errorOut = text;
        }
        return false;
    };
    if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()) {
        return fail(QStringLiteral("Conexión inválida para renombrado pendiente."));
    }
    const QString poolName = draft.poolName.trimmed();
    const QString sourceName = draft.sourceName.trimmed();
    const QString targetName = draft.targetName.trimmed();
    if (poolName.isEmpty() || sourceName.isEmpty() || targetName.isEmpty()) {
        return fail(QStringLiteral("Faltan datos para crear el renombrado pendiente."));
    }
    if (sourceName == targetName) {
        return fail(QStringLiteral("El origen y el destino del renombrado son iguales."));
    }

    for (int i = m_pendingChangesModel.size() - 1; i >= 0; --i) {
        const PendingChange& existingChange = m_pendingChangesModel.at(i);
        if (existingChange.kind != PendingChange::Kind::Rename) {
            continue;
        }
        const PendingDatasetRenameDraft& existing = existingChange.renameDraft;
        if (existing.connIdx != draft.connIdx || existing.poolName.trimmed() != poolName) {
            continue;
        }
        if (existing.targetName.trimmed() == targetName
            && existing.sourceName.trimmed() != sourceName) {
            return fail(QStringLiteral("Ya existe un renombrado pendiente hacia %1.").arg(targetName));
        }
        if (existing.sourceName.trimmed() == sourceName) {
            m_pendingChangesModel.removeAt(i);
        }
    }

    PendingChange change;
    change.kind = PendingChange::Kind::Rename;
    change.renameDraft = PendingDatasetRenameDraft{draft.connIdx, poolName, sourceName, targetName};
    change.removableIndividually = true;
    change.executableIndividually = true;
    change.stableId = QStringLiteral("rename|%1|%2|%3|%4")
                          .arg(draft.connIdx)
                          .arg(poolName, sourceName, targetName);
    m_pendingChangesModel.push_back(change);
    return true;
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    refreshDatasetProperties(side, m_connContentTree);
}

void MainWindow::refreshDatasetProperties(const QString& side, QTreeWidget* connContentTree) {
    beginTransientUiBusy(QStringLiteral("Leyendo propiedades..."));
    auto gsaPropsForView = []() {
        return QStringList{
            QStringLiteral("org.fc16.gsa:activado"),
            QStringLiteral("org.fc16.gsa:recursivo"),
            QStringLiteral("org.fc16.gsa:horario"),
            QStringLiteral("org.fc16.gsa:diario"),
            QStringLiteral("org.fc16.gsa:semanal"),
            QStringLiteral("org.fc16.gsa:mensual"),
            QStringLiteral("org.fc16.gsa:anual"),
            QStringLiteral("org.fc16.gsa:nivelar"),
            QStringLiteral("org.fc16.gsa:destino"),
        };
    };
    auto saveCurrentDraft = [this]() {
        if (m_pendingChangeActivationInProgress || !m_propsDirty || m_propsSide.isEmpty() || m_propsDataset.isEmpty()) {
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
            const bool inheritable = (ri->flags() & Qt::ItemIsUserCheckable);
            const bool inh = inheritable && ri->checkState() == Qt::Checked;
            const QString currentValue = rv->text();
            if (m_propsOriginalValues.value(key) != currentValue) {
                draft.valuesByProp[key] = currentValue;
            }
            if (inheritable && m_propsOriginalInherit.value(key, false) != inh) {
                draft.inheritByProp[key] = inh;
            }
        }
        draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
        storePropertyDraftForObject(m_propsSide, currToken, m_propsDataset, draft);
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
        const DatasetSelectionContext ctx = currentConnContentSelection(connContentTree);
        dataset = ctx.datasetName;
        snapshot = ctx.snapshotName;
    }
    QTableWidget* table = m_connContentPropsTable;
    const bool hasPropsTable = (table != nullptr);
    if (dataset.isEmpty()) {
        if (hasPropsTable) {
            setTablePopulationMode(table, true);
            table->setRowCount(0);
            setTablePopulationMode(table, false);
        }
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
        token = connContentTokenForTree(connContentTree);
    }
    const int sep = token.indexOf(QStringLiteral("::"));
    if (sep <= 0) {
        m_propsToken.clear();
        endTransientUiBusy();
        return;
    }
    const int connIdx = token.left(sep).toInt();
    const QString poolName = token.mid(sep + 2);
    const DSInfo* dsInfo = findDsInfo(connIdx, poolName, dataset);
    if (!dsInfo) {
        endTransientUiBusy();
        return;
    }
    bool selectedInsideGsaNode = false;
    if (side == QStringLiteral("conncontent") && connContentTree) {
        QTreeWidgetItem* selectedItem = connContentTree->currentItem();
        if (!selectedItem) {
            const auto selected = connContentTree->selectedItems();
            if (!selected.isEmpty()) {
                selectedItem = selected.first();
            }
        }
        if (selectedItem) {
            for (QTreeWidgetItem* p = selectedItem; p; p = p->parent()) {
                if (p->text(0).trimmed() == QStringLiteral("Programar snapshots")) {
                    selectedInsideGsaNode = true;
                    break;
                }
            }
        }
    }
    const QString objectName = snapshot.isEmpty() ? dataset : QStringLiteral("%1@%2").arg(dataset, snapshot);
    const ConnectionProfile& p = m_profiles[connIdx];
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
    const DSInfo* objectInfo = findDsInfo(connIdx, poolName, objectName);
    if (objectInfo && !objectInfo->runtime.datasetType.trimmed().isEmpty()) {
        datasetType = objectInfo->runtime.datasetType;
    }
    const bool propsLoaded = selectedInsideGsaNode
                                 ? ensureDatasetPropertySubsetLoaded(connIdx, poolName, objectName, gsaPropsForView())
                                 : ensureDatasetAllPropertiesLoaded(connIdx, poolName, objectName);
    const QVector<DatasetPropCacheRow> loadedRows = selectedInsideGsaNode
                                                        ? datasetPropertyRowsForNames(connIdx, poolName, objectName, gsaPropsForView())
                                                        : datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
    if (propsLoaded && !loadedRows.isEmpty()) {
        rawRows.reserve(loadedRows.size());
        for (const DatasetPropCacheRow& row : loadedRows) {
            rawRows.push_back({row.prop, row.value, row.source, row.readonly});
        }
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Dataset props model hit %1::%2")
                   .arg(p.name, objectName));
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
            rows.push_back({QStringLiteral("mountpoint"),
                            dsInfo->runtime.properties.value(QStringLiteral("mountpoint")).trimmed(),
                            QString(),
                            QStringLiteral("true")});
        }
        if (byProp.contains(QStringLiteral("canmount"))) {
            rows.push_back(byProp.take(QStringLiteral("canmount")));
        } else {
            rows.push_back({QStringLiteral("canmount"),
                            dsInfo->runtime.properties.value(QStringLiteral("canmount")).trimmed(),
                            QString(),
                            QStringLiteral("true")});
        }
        rows.push_back({QStringLiteral("Tamaño"),
                        formatDatasetSize(dsInfo->runtime.properties.value(QStringLiteral("used")).trimmed()),
                        QString(),
                        QStringLiteral("true")});
        if (windowsConn) {
            if (byProp.contains(QStringLiteral("driveletter"))) {
                rows.push_back(byProp.take(QStringLiteral("driveletter")));
            } else {
                rows.push_back({QStringLiteral("driveletter"), QString(), QString(), QStringLiteral("true")});
            }
        }
    }
    const QStringList remainingProps = byProp.keys();
    for (const QString& prop : remainingProps) {
        rows.push_back(byProp.value(prop));
    }

    if (selectedInsideGsaNode) {
        QVector<PropRow> gsaRows;
        const QStringList wanted = gsaPropsForView();
        gsaRows.reserve(wanted.size());
        for (const QString& prop : wanted) {
            if (byProp.contains(prop)) {
                gsaRows.push_back(byProp.value(prop));
                continue;
            }
            gsaRows.push_back({prop, gsaComparableValue(prop, QString()), QString(), QStringLiteral("false")});
        }
        rows = gsaRows;
    }

    if (side == QStringLiteral("conncontent")) {
        QMap<QString, QString> valuesByProp;
        const QString inlineCacheKey = QStringLiteral("%1|%2").arg(token, objectName);
        const auto existingInlineIt = m_connContentPropValuesByObject.constFind(inlineCacheKey);
        if (existingInlineIt != m_connContentPropValuesByObject.cend()) {
            valuesByProp = existingInlineIt.value();
        } else {
            const QVector<DatasetPropCacheRow> fullRows =
                datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
            for (const DatasetPropCacheRow& row : fullRows) {
                const QString prop = row.prop.trimmed();
                if (!prop.isEmpty()) {
                    valuesByProp[prop] = row.value;
                }
            }
            const auto runtimeProps = dsInfo->runtime.properties;
            for (auto it = runtimeProps.cbegin(); it != runtimeProps.cend(); ++it) {
                if (!it.key().trimmed().isEmpty() && !valuesByProp.contains(it.key())) {
                    valuesByProp[it.key()] = it.value();
                }
            }
        }
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

    m_propsSide = side;
    m_propsDataset = objectName;
    m_propsToken = token;
    m_propsOriginalValues.clear();
    m_propsOriginalInherit.clear();
    for (const PropRow& row : rows) {
        const QString key = row.prop.trimmed();
        if (key.isEmpty()) {
            continue;
        }
        m_propsOriginalValues[key] = gsaComparableValue(row.prop, row.value);
        m_propsOriginalInherit[key] = isDatasetPropertyCurrentlyInherited(row.source);
    }
    m_propsDirty =
        propertyDraftForObject(m_propsSide, m_propsToken, m_propsDataset).dirty
        || !m_pendingChangesModel.isEmpty();
    updateApplyPropsButtonState();

    if (!hasPropsTable) {
        endTransientUiBusy();
        return;
    }

    m_loadingPropsTable = true;
    setTablePopulationMode(table, true);
    table->setRowCount(0);
    m_propsOriginalValues.clear();
    m_propsOriginalInherit.clear();
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
    const int pinnedCount = (!snapshot.isEmpty() ? 1 : (windowsConn ? 5 : 4));
    table->setProperty("pinned_rows", pinnedCount);
    const bool encryptionOff = encryptionDisabledForRows(rows);
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
        const QString displayValue = gsaComparableValue(row.prop, row.value);
        auto* v = new PinnedSortItem(displayValue);
        v->setData(Qt::UserRole + 501, (r < pinnedCount) ? r : -1);
        const bool editable =
            isDatasetPropertyEditable(row.prop, datasetType, row.source, row.readonly, platform)
            && !(row.prop.compare(QStringLiteral("keylocation"), Qt::CaseInsensitive) == 0 && encryptionOff);
        v->setData(kPropValueEditableRole, editable);
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
        if (row.prop == QStringLiteral("Tamaño")) {
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
        }
        table->setItem(r, 1, v);
        const QString propLower = row.prop.trimmed().toLower();
        const auto enumIt = enumValues.constFind(propLower);
        if ((v->flags() & Qt::ItemIsEditable) && enumIt != enumValues.constEnd()) {
            auto* combo = new NoWheelComboBox(table);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
            QStringList options = enumIt.value();
            const QString current = displayValue.trimmed();
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
        m_propsOriginalValues[row.prop] = displayValue;
        m_propsOriginalInherit[row.prop] = currentlyInherited;
    }
    const DatasetPropsDraft draft = propertyDraftForObject(m_propsSide, token, m_propsDataset);
        if (draft.dirty) {
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
            applyInheritedStateToPropsRow(table, r);
        }
        m_propsDirty = draft.dirty;
    } else {
        for (int r = 0; r < table->rowCount(); ++r) {
            applyInheritedStateToPropsRow(table, r);
        }
        m_propsDirty = false;
    }
    setTablePopulationMode(table, false);
    m_loadingPropsTable = false;
    updateApplyPropsButtonState();
    endTransientUiBusy();
}

void MainWindow::refreshConnContentPropertiesFor(QTreeWidget* tree) {
    if (!tree) {
        return;
    }
    refreshDatasetProperties(QStringLiteral("conncontent"), tree);
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
    if (col == 2) {
        applyInheritedStateToPropsRow(table, row);
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

    if (!currentToken.isEmpty() && !m_propsDataset.isEmpty()) {
        DatasetPropsDraft draft;
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
            const bool inheritable = (ri->flags() & Qt::ItemIsUserCheckable);
            const bool inh = inheritable && ri->checkState() == Qt::Checked;
            const QString nowValue = rv->text();
            if (m_propsOriginalValues.value(key) != nowValue) {
                draft.valuesByProp[key] = nowValue;
            }
            if (inheritable && m_propsOriginalInherit.value(key, false) != inh) {
                draft.inheritByProp[key] = inh;
            }
        }
        draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
        storePropertyDraftForObject(m_propsSide, currentToken, m_propsDataset, draft);
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
    struct PendingDraft {
        QString draftKey;
        QString token;
        QString objectName;
        DatasetSelectionContext ctx;
        DatasetPropsDraft draft;
    };
    QVector<PendingDraft> pendingPropertyDrafts;
    for (const PendingPropertyDraftEntry& item : pendingConnContentPropertyDraftsFromModel()) {
        if (item.objectName.contains(QLatin1Char('@'))) {
            continue;
        }
        DatasetSelectionContext ctx;
        ctx.valid = true;
        ctx.connIdx = item.connIdx;
        ctx.poolName = item.poolName;
        ctx.datasetName = item.objectName;
        pendingPropertyDrafts.push_back(PendingDraft{
            propsDraftKey(QStringLiteral("conncontent"), item.token, item.objectName),
            item.token,
            item.objectName,
            ctx,
            item.draft
        });
    }
    const bool hasPendingConnContentDrafts = !pendingPropertyDrafts.isEmpty();
    const bool hasPendingPermissionDrafts = !dirtyDatasetPermissionsEntriesFromModel().isEmpty();
    if (m_propsSide == QStringLiteral("conncontent")
        || hasPendingConnContentDrafts
        || hasPendingPermissionDrafts
        || !m_pendingChangesModel.isEmpty()) {
        const QStringList gsaProps = {
            QStringLiteral("org.fc16.gsa:activado"),
            QStringLiteral("org.fc16.gsa:recursivo"),
            QStringLiteral("org.fc16.gsa:horario"),
            QStringLiteral("org.fc16.gsa:diario"),
            QStringLiteral("org.fc16.gsa:semanal"),
            QStringLiteral("org.fc16.gsa:mensual"),
            QStringLiteral("org.fc16.gsa:anual"),
            QStringLiteral("org.fc16.gsa:nivelar"),
            QStringLiteral("org.fc16.gsa:destino"),
        };
        auto saveCurrentConnContentDraft = [this]() {
            if (m_propsDataset.isEmpty() || m_propsToken.isEmpty() || !m_connContentPropsTable) {
                return;
            }
            DatasetPropsDraft draft =
                propertyDraftForObject(QStringLiteral("conncontent"), m_propsToken, m_propsDataset);
            QSet<QString> visibleKeys;
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
                visibleKeys.insert(key);
                const QString nowValue = rv->text();
                const bool inheritable = (ri->flags() & Qt::ItemIsUserCheckable);
                const bool nowInherit = inheritable && ri->checkState() == Qt::Checked;
                const QString originalValue = m_propsOriginalValues.value(key);
                const bool originalInherit = m_propsOriginalInherit.value(key, false);
                if (nowValue != originalValue) {
                    draft.valuesByProp[key] = nowValue;
                } else {
                    draft.valuesByProp.remove(key);
                }
                if (inheritable && nowInherit != originalInherit) {
                    draft.inheritByProp[key] = nowInherit;
                } else {
                    draft.inheritByProp.remove(key);
                }
            }
            draft.dirty = !draft.valuesByProp.isEmpty() || !draft.inheritByProp.isEmpty();
            storePropertyDraftForObject(QStringLiteral("conncontent"), m_propsToken, m_propsDataset, draft);
        };
        saveCurrentConnContentDraft();

        if (hasPendingConnContentDrafts) {
            QString gsaValidationError;
            if (!validatePendingGsaDrafts(&gsaValidationError)) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"), gsaValidationError);
                updateApplyPropsButtonState();
                return;
            }
        }

        struct TransientBusyGuard {
            MainWindow* self{nullptr};
            bool active{false};
            ~TransientBusyGuard() {
                if (active && self) {
                    self->endTransientUiBusy();
                }
            }
        };
        beginTransientUiBusy(QStringLiteral("Aplicando cambios y refrescando conexiones..."));
        TransientBusyGuard busyGuard{this, true};
        QSet<int> connectionsToRefresh;
        for (const PendingDraft& item : pendingPropertyDrafts) {
            QMap<QString, QString> originalValues;
            QMap<QString, bool> originalInherit;
            const QVector<DatasetPropCacheRow> propertyRows =
                datasetPropertyRowsFromModelOrCache(item.ctx.connIdx, item.ctx.poolName, item.objectName);
            const bool hasLoadedCache = !propertyRows.isEmpty();
            for (const DatasetPropCacheRow& row : propertyRows) {
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
            bool touchedAnyProperty = false;
            bool touchedOnlyGsaProperties = true;
            for (const QString& prop : touched) {
                if (prop.isEmpty() || prop == QStringLiteral("dataset")
                    || prop == QStringLiteral("Tamaño")
                    || prop == QStringLiteral("snapshot")) {
                    continue;
                }
                touchedAnyProperty = true;
                if (!prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                    touchedOnlyGsaProperties = false;
                }
                if (!hasLoadedCache
                    && prop.compare(QStringLiteral("mounted"), Qt::CaseInsensitive) != 0
                    && !prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
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

                if (prop == QStringLiteral("mounted")) {
                    bool finalMounted = false;
                    bool originalMounted = false;
                    const bool finalKnown = mountedStateFromText(finalValue, &finalMounted);
                    const bool originalKnown = mountedStateFromText(originalValue, &originalMounted);
                    if (finalKnown && originalKnown && finalMounted != originalMounted) {
                        subcmds << QStringLiteral("zfs %1 %2")
                                       .arg((isWindowsConnection(item.ctx.connIdx)
                                                 ? (finalMounted ? QStringLiteral("mount")
                                                                 : QStringLiteral("unmount"))
                                                 : (finalMounted ? QStringLiteral("mount")
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
                storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
                continue;
            }

            const bool isWin = isWindowsConnection(item.ctx.connIdx);
            const QString cmd = isWin ? subcmds.join(QStringLiteral("; "))
                                      : QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
            const bool useGranularDatasetRefresh = touchedAnyProperty;
            const bool useGranularGsaRefresh = useGranularDatasetRefresh && touchedOnlyGsaProperties;
            if (!executeDatasetAction(QStringLiteral("conncontent"),
                                      QStringLiteral("Aplicar propiedades"),
                                      item.ctx,
                                      cmd,
                                      60000,
                                      isWin,
                                      {},
                                      !useGranularDatasetRefresh,
                                      useGranularDatasetRefresh
                                          ? [this, item, gsaProps, useGranularGsaRefresh]() {
                                                invalidateDatasetCacheEntry(item.ctx.connIdx,
                                                                            item.ctx.poolName,
                                                                            item.objectName,
                                                                            false);
                                                if (useGranularGsaRefresh) {
                                                    ensureDatasetPropertySubsetLoaded(item.ctx.connIdx,
                                                                                     item.ctx.poolName,
                                                                                     item.objectName,
                                                                                     gsaProps);
                                                    if (PoolInfo* poolInfo = findPoolInfo(item.ctx.connIdx, item.ctx.poolName)) {
                                                        poolInfo->runtime.schedulesState = LoadState::NotLoaded;
                                                        poolInfo->runtime.autoSnapshotPropsByDataset.remove(item.objectName);
                                                    }
                                                } else {
                                                    ensureDatasetAllPropertiesLoaded(item.ctx.connIdx,
                                                                                     item.ctx.poolName,
                                                                                     item.objectName);
                                                }
                                                const QString token = item.token.trimmed();
                                                const QList<QTreeWidget*> trees{m_connContentTree};
                                                for (QTreeWidget* tree : trees) {
                                                    if (!tree || connContentTokenForTree(tree).trimmed() != token) {
                                                        continue;
                                                    }
                                                    syncConnContentPropertyColumnsFor(tree, token);
                                                    syncConnContentPoolColumnsFor(tree, token);
                                                    const DatasetSelectionContext selected = currentConnContentSelection(tree);
                                                    const QString selectedObjectName =
                                                        selected.snapshotName.trimmed().isEmpty()
                                                            ? selected.datasetName.trimmed()
                                                            : QStringLiteral("%1@%2")
                                                                  .arg(selected.datasetName.trimmed(),
                                                                       selected.snapshotName.trimmed());
                                                    if (selected.valid
                                                        && selected.connIdx == item.ctx.connIdx
                                                        && selected.poolName.trimmed() == item.ctx.poolName.trimmed()
                                                        && selectedObjectName == item.objectName.trimmed()) {
                                                        refreshConnContentPropertiesFor(tree);
                                                    }
                                                }
                                            }
                                          : std::function<void()>{})) {
                updateApplyPropsButtonState();
                return;
            }
            if (!useGranularDatasetRefresh) {
                connectionsToRefresh.insert(item.ctx.connIdx);
            }
            storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
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
        auto refreshVisiblePermissionsNodes = [this, &findDatasetItemByIdentityLocal](int connIdx,
                                                                                      const QString& poolName,
                                                                                      const QString& datasetName) {
            const QList<QTreeWidget*> trees{m_connContentTree};
            for (QTreeWidget* tree : trees) {
                if (!tree) {
                    continue;
                }
                QTreeWidgetItem* ownerNode =
                    findDatasetItemByIdentityLocal(tree, connIdx, poolName, datasetName);
                if (!ownerNode) {
                    continue;
                }
                const QString token = connContentTokenForTree(tree).trimmed();
                if (!token.isEmpty()) {
                    saveConnContentTreeStateFor(tree, token);
                }
                populateDatasetPermissionsNode(tree, ownerNode, false);
                if (!token.isEmpty()) {
                    syncConnContentPropertyColumnsFor(tree, token);
                    restoreConnContentTreeStateFor(tree, token);
                }
                const DatasetSelectionContext selected = currentConnContentSelection(tree);
                if (selected.valid
                    && selected.connIdx == connIdx
                    && selected.poolName.trimmed() == poolName.trimmed()
                    && selected.datasetName.trimmed() == datasetName.trimmed()
                    && selected.snapshotName.trimmed().isEmpty()) {
                    refreshConnContentPropertiesFor(tree);
                }
            }
        };
        auto objectDatasetName = [](const QString& objectName) {
            const int at = objectName.indexOf(QLatin1Char('@'));
            return (at > 0) ? objectName.left(at).trimmed() : objectName.trimmed();
        };

        struct PendingPermissionEntry {
            int connIdx{-1};
            QString poolName;
            QString datasetName;
            DatasetPermissionsCacheEntry entry;
        };
        QVector<PendingPermissionEntry> pendingPermissions;
        for (auto itConn = m_connInfoById.cbegin(); itConn != m_connInfoById.cend(); ++itConn) {
            const ConnInfo& connInfo = itConn.value();
            for (auto itPool = connInfo.poolsByStableId.cbegin(); itPool != connInfo.poolsByStableId.cend(); ++itPool) {
                const PoolInfo& poolInfo = itPool.value();
                for (auto itDs = poolInfo.objectsByFullName.cbegin(); itDs != poolInfo.objectsByFullName.cend(); ++itDs) {
                    const DSInfo& dsInfo = itDs.value();
                    if (!dsInfo.permissionsCache.loaded || !dsInfo.permissionsCache.dirty || dsInfo.kind == DSKind::Snapshot) {
                        continue;
                    }
                    pendingPermissions.push_back(PendingPermissionEntry{
                        connInfo.connIdx,
                        poolInfo.key.poolName,
                        dsInfo.key.fullName,
                        dsInfo.permissionsCache
                    });
                }
            }
        }
        for (const PendingPermissionEntry& pendingPerm : pendingPermissions) {
            const int connIdx = pendingPerm.connIdx;
            const QString poolName = pendingPerm.poolName;
            const QString datasetName = pendingPerm.datasetName;
            DatasetPermissionsCacheEntry entry = pendingPerm.entry;
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
                subcmds << cmd;
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
                if (auto* mutableEntry = datasetPermissionsEntryMutable(connIdx, poolName, datasetName)) {
                    mutableEntry->dirty = false;
                    mirrorDatasetPermissionsEntryToModel(connIdx, poolName, datasetName);
                }
                continue;
            }

            DatasetSelectionContext ctx;
            ctx.valid = true;
            ctx.connIdx = connIdx;
            ctx.poolName = poolName;
            ctx.datasetName = datasetName;
            const QString tokenForTree = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
            if (QTreeWidgetItem* ownerNode = findDatasetItemByIdentityLocal(m_connContentTree, connIdx, poolName, datasetName)) {
                saveConnContentTreeStateFor(m_connContentTree, tokenForTree);
                Q_UNUSED(ownerNode);
            }
            const QString cmd = QStringLiteral("set -e; %1").arg(subcmds.join(QStringLiteral("; ")));
            if (!executeDatasetAction(QStringLiteral("conncontent"),
                                      QStringLiteral("Aplicar permisos"),
                                      ctx,
                                      cmd,
                                      60000,
                                      false,
                                      {},
                                      false,
                                      [this, connIdx, poolName, datasetName, refreshVisiblePermissionsNodes]() {
                                          removeDatasetPermissionsEntry(connIdx, poolName, datasetName);
                                          ensureDatasetPermissionsLoaded(connIdx, poolName, datasetName);
                                          refreshVisiblePermissionsNodes(connIdx, poolName, datasetName);
                                      })) {
                updateApplyPropsButtonState();
                return;
            }
        }

        QMap<QString, QString> renameRefreshSelectionByToken;
        QVector<PendingDatasetRenameDraft> pendingRenames;
        QVector<PendingShellActionDraft> pendingShellActions;
        for (const PendingChange& pending : std::as_const(m_pendingChangesModel)) {
            if (pending.kind == PendingChange::Kind::Rename) {
                pendingRenames.push_back(pending.renameDraft);
            } else if (pending.kind == PendingChange::Kind::ShellAction) {
                pendingShellActions.push_back(pending.shellDraft);
            }
        }
        QStringList renamePreviews;
        for (const PendingDatasetRenameDraft& draft : pendingRenames) {
            if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()
                || draft.poolName.trimmed().isEmpty()
                || draft.sourceName.trimmed().isEmpty()
                || draft.targetName.trimmed().isEmpty()) {
                continue;
            }
            const ConnectionProfile& p = m_profiles.at(draft.connIdx);
            ConnectionProfile sudoProfile = p;
            if (!ensureLocalSudoCredentials(sudoProfile)) {
                appLog(QStringLiteral("INFO"), QStringLiteral("Aplicar renombrado cancelado: faltan credenciales sudo locales"));
                updateApplyPropsButtonState();
                return;
            }
            const QString remoteCmd = withSudo(sudoProfile, pendingDatasetRenameCommand(draft));
            renamePreviews << QStringLiteral("[%1]\n%2")
                                  .arg(QStringLiteral("%1@%2:%3").arg(p.username, p.host).arg(p.port > 0 ? QString::number(p.port) : QStringLiteral("22")))
                                  .arg(buildSshPreviewCommand(p, remoteCmd));
        }
        if (!renamePreviews.isEmpty() && !confirmActionExecution(QStringLiteral("Aplicar cambios"), renamePreviews)) {
            updateApplyPropsButtonState();
            return;
        }
        if (!renamePreviews.isEmpty()) {
            setActionsLocked(true);
        }
        for (const PendingDatasetRenameDraft& draft : pendingRenames) {
            if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()
                || draft.poolName.trimmed().isEmpty()
                || draft.sourceName.trimmed().isEmpty()
                || draft.targetName.trimmed().isEmpty()) {
                continue;
            }
            QString failureDetail;
            if (!executePendingDatasetRenameDraft(draft, true, &failureDetail)) {
                setActionsLocked(false);
                updateApplyPropsButtonState();
                return;
            }
            for (int i = m_pendingChangesModel.size() - 1; i >= 0; --i) {
                const PendingChange& existing = m_pendingChangesModel.at(i);
                if (existing.kind == PendingChange::Kind::Rename
                    && existing.renameDraft.connIdx == draft.connIdx
                    && existing.renameDraft.poolName.trimmed() == draft.poolName.trimmed()
                    && existing.renameDraft.sourceName.trimmed() == draft.sourceName.trimmed()
                    && existing.renameDraft.targetName.trimmed() == draft.targetName.trimmed()) {
                    m_pendingChangesModel.removeAt(i);
                    break;
                }
            }
            invalidateDatasetCacheForPool(draft.connIdx, draft.poolName);
            invalidateDatasetPermissionsCacheForPool(draft.connIdx, draft.poolName);
            const QString token = QStringLiteral("%1::%2").arg(draft.connIdx).arg(draft.poolName.trimmed());
            renameRefreshSelectionByToken[token] = objectDatasetName(draft.targetName);
            auto remapSelection = [&draft](DatasetSelectionContext& selection) {
                const QString selectedObject =
                    selection.snapshotName.trimmed().isEmpty()
                        ? selection.datasetName.trimmed()
                        : QStringLiteral("%1@%2").arg(selection.datasetName.trimmed(), selection.snapshotName.trimmed());
                if (!selection.valid || selectedObject != draft.sourceName.trimmed()) {
                    return;
                }
                selection.poolName = draft.poolName.trimmed();
                const int at = draft.targetName.indexOf(QLatin1Char('@'));
                if (at > 0) {
                    selection.datasetName = draft.targetName.left(at).trimmed();
                    selection.snapshotName = draft.targetName.mid(at + 1).trimmed();
                } else {
                    selection.datasetName = draft.targetName.trimmed();
                    selection.snapshotName.clear();
                }
            };
            remapSelection(m_connActionOrigin);
            remapSelection(m_connActionDest);
            if (m_propsSide == QStringLiteral("conncontent")
                && m_propsToken.trimmed() == token
                && m_propsDataset.trimmed() == draft.sourceName.trimmed()) {
                m_propsDataset = draft.targetName.trimmed();
            }
        }
        for (auto it = renameRefreshSelectionByToken.cbegin(); it != renameRefreshSelectionByToken.cend(); ++it) {
            const int sep2 = it.key().indexOf(QStringLiteral("::"));
            if (sep2 > 0) {
                bool okConn = false;
                const int connIdx = it.key().left(sep2).toInt(&okConn);
                if (okConn && connIdx >= 0) {
                    connectionsToRefresh.insert(connIdx);
                }
            }
        }
        if (!renamePreviews.isEmpty()) {
            setActionsLocked(false);
        }

        for (const PendingShellActionDraft& draft : pendingShellActions) {
            if (draft.command.trimmed().isEmpty()) {
                continue;
            }
            if (!runLocalCommand(draft.displayLabel, draft.command, draft.timeoutMs, false, draft.streamProgress)) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("ZFSMgr"),
                    trk(QStringLiteral("t_pending_shell_failed_001"),
                        QStringLiteral("Error ejecutando cambio pendiente:\n%1\n\nRevisa el log para más detalles."),
                        QStringLiteral("Error executing pending change:\n%1\n\nCheck logs for more details."),
                        QStringLiteral("执行待处理更改时出错：\n%1\n\n请查看日志了解更多详情。"))
                        .arg(draft.displayLabel.trimmed().isEmpty() ? draft.command.trimmed()
                                                                     : draft.displayLabel.trimmed()));
                updateApplyPropsButtonState();
                return;
            }
            for (int i = m_pendingChangesModel.size() - 1; i >= 0; --i) {
                const PendingChange& existing = m_pendingChangesModel.at(i);
                if (existing.kind == PendingChange::Kind::ShellAction
                    && existing.shellDraft.displayLabel.trimmed() == draft.displayLabel.trimmed()
                    && existing.shellDraft.command.trimmed() == draft.command.trimmed()) {
                    m_pendingChangesModel.removeAt(i);
                    break;
                }
            }
            refreshPendingShellActionDraft(draft);
        }

        for (int connIdx : std::as_const(connectionsToRefresh)) {
            if (connIdx < 0 || connIdx >= m_profiles.size()) {
                continue;
            }
            refreshConnectionByIndex(connIdx);
        }

        if (!connectionsToRefresh.isEmpty() && m_connActionOrigin.valid) {
            reloadDatasetSide(QStringLiteral("origin"));
        }
        if (!connectionsToRefresh.isEmpty() && m_connActionDest.valid) {
            reloadDatasetSide(QStringLiteral("dest"));
        }

        bool hasPropertyDrafts = false;
        hasPropertyDrafts = !pendingConnContentPropertyDraftsFromModel().isEmpty();
        bool hasPermissionDrafts = false;
        for (auto itConn = m_connInfoById.cbegin(); itConn != m_connInfoById.cend() && !hasPermissionDrafts; ++itConn) {
            for (auto itPool = itConn->poolsByStableId.cbegin(); itPool != itConn->poolsByStableId.cend() && !hasPermissionDrafts; ++itPool) {
                for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                    if (itDs->permissionsCache.loaded && itDs->permissionsCache.dirty) {
                        hasPermissionDrafts = true;
                        break;
                    }
                }
            }
        }
        m_propsDirty = hasPropertyDrafts
                       || hasPermissionDrafts
                       || !m_pendingChangesModel.isEmpty();
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
        auto reselectDatasetInTree = [&datasetToSelect](QTreeWidget* tree) {
            if (!tree || datasetToSelect.trimmed().isEmpty()) {
                return;
            }
            std::function<QTreeWidgetItem*(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) -> QTreeWidgetItem* {
                if (!n) {
                    return nullptr;
                }
                if (n->data(0, Qt::UserRole).toString().trimmed() == datasetToSelect.trimmed()) {
                    return n;
                }
                for (int i = 0; i < n->childCount(); ++i) {
                    if (QTreeWidgetItem* f = rec(n->child(i))) {
                        return f;
                    }
                }
                return nullptr;
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                if (QTreeWidgetItem* item = rec(tree->topLevelItem(i))) {
                    tree->setCurrentItem(item);
                    break;
                }
            }
        };
        reloadDatasetSide(QStringLiteral("conncontent"));
        reselectDatasetInTree(m_connContentTree);
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
    bool localRenameDone = false;

    if (subcmds.isEmpty()) {
        if (localRenameDone) {
            m_propsDirty = false;
            if (!currentDraftKey.isEmpty()) {
                storePropertyDraftForObject(m_propsSide, currentToken, m_propsDataset, DatasetPropsDraft{});
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
            storePropertyDraftForObject(m_propsSide, currentToken, m_propsDataset, DatasetPropsDraft{});
        }
        updateApplyPropsButtonState();
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
            storePropertyDraftForObject(m_propsSide, currentToken, m_propsDataset, DatasetPropsDraft{});
        }
        if (!currentToken.isEmpty() && targetDataset != m_propsDataset) {
            storePropertyDraftForObject(m_propsSide, currentToken, targetDataset, DatasetPropsDraft{});
        }
        updateApplyPropsButtonState();
    }
}

void MainWindow::clearAllPendingChanges() {
    const QVector<PendingPropertyDraftEntry> propertyDrafts = pendingConnContentPropertyDraftsFromModel();
    for (const PendingPropertyDraftEntry& item : propertyDrafts) {
        storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
    }
    resetAllDatasetPermissionDrafts();
    for (auto itConn = m_connInfoById.begin(); itConn != m_connInfoById.end(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.begin(); itPool != itConn->poolsByStableId.end(); ++itPool) {
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                itDs->editSession.clear();
            }
        }
    }
    m_pendingChangesModel.clear();
    m_propsDirty = false;
    if (m_connContentTree) {
        auto refreshVisiblePermissionNodes = [this](QTreeWidget* tree) {
            std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* node) {
                if (!node) {
                    return;
                }
                const QString datasetName = node->data(0, Qt::UserRole).toString().trimmed();
                const QString snapshotName = node->data(1, Qt::UserRole).toString().trimmed();
                if (!datasetName.isEmpty() && snapshotName.isEmpty()) {
                    bool hasVisiblePermissionsNode = false;
                    for (int i = 0; i < node->childCount(); ++i) {
                        QTreeWidgetItem* child = node->child(i);
                        if (!child) {
                            continue;
                        }
                        const QString label = child->text(0).trimmed();
                        if (label == QStringLiteral("Permisos")
                            || label == QStringLiteral("Permissions")
                            || label == QStringLiteral("权限")) {
                            hasVisiblePermissionsNode = true;
                            break;
                        }
                    }
                    if (hasVisiblePermissionsNode) {
                        populateDatasetPermissionsNode(tree, node, false);
                    }
                }
                for (int i = 0; i < node->childCount(); ++i) {
                    rec(node->child(i));
                }
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                rec(tree->topLevelItem(i));
            }
        };
        refreshVisiblePermissionNodes(m_connContentTree);
        const DatasetSelectionContext current = currentConnContentSelection(m_connContentTree);
        if (current.valid) {
            refreshConnContentPropertiesFor(m_connContentTree);
        }
    }
    updateApplyPropsButtonState();
}

bool MainWindow::removePendingQueuedChangeLine(const QString& line) {
    PendingChange change;
    if (!findPendingChangeByDisplayLine(line, &change)) {
        return false;
    }
    if (!change.removableIndividually) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 QStringLiteral("Eliminar individual solo está disponible para cambios en cola de acciones y renombrados."));
        return false;
    }
    if (change.kind == PendingChange::Kind::ShellAction) {
        for (int i = 0; i < m_pendingChangesModel.size(); ++i) {
            const PendingChange& draft = m_pendingChangesModel.at(i);
            if (draft.kind == PendingChange::Kind::ShellAction
                && draft.shellDraft.displayLabel.trimmed() == change.shellDraft.displayLabel.trimmed()
                && draft.shellDraft.command.trimmed() == change.shellDraft.command.trimmed()) {
                m_pendingChangesModel.removeAt(i);
                updateApplyPropsButtonState();
                return true;
            }
        }
        return false;
    }
    if (change.kind == PendingChange::Kind::Rename) {
        for (int i = 0; i < m_pendingChangesModel.size(); ++i) {
            const PendingChange& draft = m_pendingChangesModel.at(i);
            if (draft.kind == PendingChange::Kind::Rename
                && draft.renameDraft.connIdx == change.renameDraft.connIdx
                && draft.renameDraft.poolName.trimmed() == change.renameDraft.poolName.trimmed()
                && draft.renameDraft.sourceName.trimmed() == change.renameDraft.sourceName.trimmed()
                && draft.renameDraft.targetName.trimmed() == change.renameDraft.targetName.trimmed()) {
                m_pendingChangesModel.removeAt(i);
                updateApplyPropsButtonState();
                return true;
            }
        }
        return false;
    }
    QMessageBox::information(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("Eliminar individual solo está disponible para cambios en cola de acciones y renombrados."));
    return false;
}

bool MainWindow::executePendingQueuedChangeLine(const QString& line) {
    PendingChange change;
    if (!findPendingChangeByDisplayLine(line, &change)) {
        return false;
    }
    if (executePendingChange(change)) {
        updateApplyPropsButtonState();
        return true;
    }
    QMessageBox::information(this,
                             QStringLiteral("ZFSMgr"),
                             QStringLiteral("La ejecución individual solo está disponible para cambios en cola de acciones y renombrados."));
    return false;
}

QVector<MainWindow::PendingChange> MainWindow::pendingChanges() const {
    QVector<PendingChange> changes;
    auto connPoolPrefix = [this](int connIdx, const QString& poolName) {
        const ConnectionProfile& p = m_profiles.at(connIdx);
        const QString connLabel = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
        return QStringLiteral("%1::%2").arg(connLabel, poolName.trimmed());
    };
    auto appendPending = [this, &changes, &connPoolPrefix](const PendingChange& base,
                                                           int connIdx,
                                                           const QString& poolName,
                                                           const QString& command,
                                                           const QString& displayText) {
        const QString trimmedCmd = command.trimmed();
        const QString trimmedDisplay = displayText.trimmed();
        if (trimmedCmd.isEmpty() || trimmedDisplay.isEmpty()) {
            return;
        }
        PendingChange change = base;
        change.connIdx = connIdx;
        change.poolName = poolName.trimmed();
        change.commandLine = QStringLiteral("%1  %2").arg(connPoolPrefix(connIdx, poolName), trimmedCmd);
        change.displayLine = QStringLiteral("%1  %2").arg(connPoolPrefix(connIdx, poolName), trimmedDisplay);
        if (change.stableId.isEmpty()) {
            change.stableId = QStringLiteral("%1|%2|%3|%4")
                                  .arg(static_cast<int>(change.kind))
                                  .arg(connIdx)
                                  .arg(poolName.trimmed(), trimmedCmd);
        }
        if (!m_pendingChangeOrderByStableId.contains(change.stableId)) {
            m_pendingChangeOrderByStableId.insert(change.stableId, m_nextPendingChangeOrder++);
        }
        changes.push_back(change);
    };
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
    auto permissionTargetText = [](const DatasetPermissionGrant& grant) {
        const QString tt = grant.targetType.trimmed().toLower();
        if (tt == QStringLiteral("user")) {
            return QStringLiteral("usuario %1").arg(grant.targetName.trimmed());
        }
        if (tt == QStringLiteral("group")) {
            return QStringLiteral("grupo %1").arg(grant.targetName.trimmed());
        }
        return QStringLiteral("everyone");
    };
    auto permissionScopeText = [](const QString& scope) {
        const QString s = scope.trimmed().toLower();
        if (s == QStringLiteral("local")) {
            return QStringLiteral("locales");
        }
        if (s == QStringLiteral("descendant")) {
            return QStringLiteral("descendientes");
        }
        if (s == QStringLiteral("local+descendant")) {
            return QStringLiteral("locales+descendientes");
        }
        return s;
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
    for (const PendingPropertyDraftEntry& item : pendingConnContentPropertyDraftsFromModel()) {
        const int connIdx = item.connIdx;
        const QString poolName = item.poolName;
        const QString objectName = item.objectName;
        const DatasetPropsDraft& draft = item.draft;
        if (connIdx < 0 || connIdx >= m_profiles.size()
            || poolName.isEmpty()
            || objectName.isEmpty()
            || objectName.contains(QLatin1Char('@'))) {
            continue;
        }
        QMap<QString, QString> originalValues;
        QMap<QString, bool> originalInherit;
        QStringList touched;
        auto appendTouched = [&touched](const QString& prop) {
            if (prop.isEmpty() || touched.contains(prop)) {
                return;
            }
            touched.push_back(prop);
        };
        const QVector<DatasetPropCacheRow> propertyRows =
            datasetPropertyRowsFromModelOrCache(connIdx, poolName, objectName);
        const bool hasLoadedCache = !propertyRows.isEmpty();
        for (const DatasetPropCacheRow& row : propertyRows) {
            originalValues[row.prop] = gsaComparableValue(row.prop, row.value);
            originalInherit[row.prop] = isDatasetPropertyCurrentlyInherited(row.source);
            appendTouched(row.prop);
        }
        if (const DSInfo* dsInfo = findDsInfo(connIdx, poolName, objectName)) {
            originalValues[QStringLiteral("mounted")] =
                dsInfo->runtime.properties.value(QStringLiteral("mounted"));
            appendTouched(QStringLiteral("mounted"));
        }
        for (auto vit = draft.valuesByProp.cbegin(); vit != draft.valuesByProp.cend(); ++vit) {
            appendTouched(vit.key());
        }
        for (auto iit = draft.inheritByProp.cbegin(); iit != draft.inheritByProp.cend(); ++iit) {
            appendTouched(iit.key());
        }

        for (const QString& prop : touched) {
            if (prop.isEmpty() || prop == QStringLiteral("dataset")
                || prop == QStringLiteral("Tamaño")
                || prop == QStringLiteral("snapshot")) {
                continue;
            }
            if (!hasLoadedCache
                && prop.compare(QStringLiteral("mounted"), Qt::CaseInsensitive) != 0
                && !prop.startsWith(QStringLiteral("org.fc16.gsa:"), Qt::CaseInsensitive)) {
                continue;
            }
            const bool originalInh = originalInherit.value(prop, false);
            const bool finalInh = draft.inheritByProp.contains(prop)
                                      ? draft.inheritByProp.value(prop)
                                      : originalInh;
            const QString originalValue = gsaComparableValue(prop, originalValues.value(prop));
            const QString finalValue = draft.valuesByProp.contains(prop)
                                           ? gsaComparableValue(prop, draft.valuesByProp.value(prop))
                                           : originalValue;

            if (prop == QStringLiteral("mounted")) {
                bool finalMounted = false;
                bool originalMounted = false;
                const bool finalKnown = mountedStateFromText(finalValue, &finalMounted);
                const bool originalKnown = mountedStateFromText(originalValue, &originalMounted);
                if (finalKnown && originalKnown && finalMounted != originalMounted) {
                    PendingChange change;
                    change.kind = PendingChange::Kind::Property;
                    change.objectName = objectName;
                    change.propertyName = QStringLiteral("Montado");
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  QStringLiteral("zfs %1 %2")
                                      .arg((isWindowsConnection(connIdx)
                                                ? (finalMounted ? QStringLiteral("mount")
                                                                : QStringLiteral("unmount"))
                                                : (finalMounted ? QStringLiteral("mount")
                                                                : QStringLiteral("umount"))),
                                           shSingleQuote(objectName)),
                                  QStringLiteral("%1 dataset %2")
                                      .arg(finalMounted ? QStringLiteral("Montar")
                                                        : QStringLiteral("Desmontar"),
                                           objectName));
                }
                continue;
            }
            if (finalInh != originalInh) {
                if (finalInh) {
                    PendingChange change;
                    change.kind = PendingChange::Kind::Property;
                    change.objectName = objectName;
                    change.propertyName = prop;
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  QStringLiteral("zfs inherit %1 %2")
                                      .arg(shSingleQuote(prop), shSingleQuote(objectName)),
                                  QStringLiteral("Heredar propiedad %1 en %2").arg(prop, objectName));
                } else {
                    PendingChange change;
                    change.kind = PendingChange::Kind::Property;
                    change.objectName = objectName;
                    change.propertyName = prop;
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  QStringLiteral("zfs set %1 %2")
                                      .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                           shSingleQuote(objectName)),
                                  QStringLiteral("Cambiar propiedad %1=%2 en %3").arg(prop, finalValue, objectName));
                }
                continue;
            }
            if (!finalInh && finalValue != originalValue) {
                PendingChange change;
                change.kind = PendingChange::Kind::Property;
                change.objectName = objectName;
                change.propertyName = prop;
                appendPending(change,
                              connIdx,
                              poolName,
                              QStringLiteral("zfs set %1 %2")
                                  .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                       shSingleQuote(objectName)),
                              QStringLiteral("Cambiar propiedad %1=%2 en %3").arg(prop, finalValue, objectName));
            }
        }
    }
    for (const PendingPermissionDraftEntry& item : dirtyDatasetPermissionsEntriesFromModel()) {
        const DatasetPermissionsCacheEntry& entry = item.entry;
        const int connIdx = item.connIdx;
        const QString poolName = item.poolName;
        const QString datasetName = item.datasetName;
        if (connIdx < 0 || connIdx >= m_profiles.size() || poolName.isEmpty() || datasetName.isEmpty()) {
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
            QStringList keys;
            auto appendKey = [&keys](const QString& key) {
                if (!key.isEmpty() && !keys.contains(key)) {
                    keys.push_back(key);
                }
            };
            for (const DatasetPermissionGrant& g : original) appendKey(grantKey(g));
            for (const DatasetPermissionGrant& g : current) appendKey(grantKey(g));
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
                    PendingChange change;
                    change.kind = PendingChange::Kind::Permission;
                    change.objectName = datasetName;
                    change.focusPermissionsNode = true;
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  QStringLiteral("zfs unallow %1%2")
                                      .arg(beforeFlags, shSingleQuote(datasetName)),
                                  QStringLiteral("Eliminar permisos %1 de %2 en %3")
                                      .arg(permissionScopeText(before.scope),
                                           permissionTargetText(before),
                                           datasetName));
                    continue;
                }
                if (!had && has) {
                    if (!afterTokens.isEmpty()) {
                        PendingChange change;
                        change.kind = PendingChange::Kind::Permission;
                        change.objectName = datasetName;
                        change.focusPermissionsNode = true;
                        appendPending(change,
                                      connIdx,
                                      poolName,
                                      QStringLiteral("zfs allow %1%2%3 %4")
                                          .arg(afterFlags,
                                               afterTokens.join(','),
                                               QString(),
                                               shSingleQuote(datasetName)),
                                      QStringLiteral("Conceder permisos %1 a %2 en %3")
                                          .arg(afterTokens.join(','),
                                               permissionTargetText(after),
                                               datasetName));
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
                    PendingChange change;
                    change.kind = PendingChange::Kind::Permission;
                    change.objectName = datasetName;
                    change.focusPermissionsNode = true;
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  cmd,
                                  QStringLiteral("Actualizar permisos de %2 en %3 a %1")
                                      .arg(afterTokens.join(','),
                                           permissionTargetText(after),
                                           datasetName));
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
            PendingChange change;
            change.kind = PendingChange::Kind::Permission;
            change.objectName = datasetName;
            change.focusPermissionsNode = true;
            appendPending(change,
                          connIdx,
                          poolName,
                          cmd,
                          QStringLiteral("Actualizar permisos de creación en %1 a %2")
                              .arg(datasetName, currentCreate.join(',')));
        }

        QMap<QString, DatasetPermissionSet> origSets;
        QMap<QString, DatasetPermissionSet> currSets;
        for (const DatasetPermissionSet& s : entry.originalPermissionSets) {
            origSets.insert(setKey(s), s);
        }
        for (const DatasetPermissionSet& s : entry.permissionSets) {
            currSets.insert(setKey(s), s);
        }
        QStringList setKeys;
        auto appendSetKey = [&setKeys](const QString& key) {
            if (!key.isEmpty() && !setKeys.contains(key)) {
                setKeys.push_back(key);
            }
        };
        for (const DatasetPermissionSet& s : entry.originalPermissionSets) appendSetKey(setKey(s));
        for (const DatasetPermissionSet& s : entry.permissionSets) appendSetKey(setKey(s));
        for (const QString& key : setKeys) {
            const bool had = origSets.contains(key);
            const bool has = currSets.contains(key);
            const DatasetPermissionSet before = origSets.value(key);
            const DatasetPermissionSet after = currSets.value(key);
            const QStringList beforeTokens = normalizeTokens(before.permissions);
            const QStringList afterTokens = normalizeTokens(after.permissions);
            if (had && !has) {
                PendingChange change;
                change.kind = PendingChange::Kind::Permission;
                change.objectName = datasetName;
                change.focusPermissionsNode = true;
                appendPending(change,
                              connIdx,
                              poolName,
                              QStringLiteral("zfs unallow -s %1 %2")
                                  .arg(before.name, shSingleQuote(datasetName)),
                              QStringLiteral("Eliminar conjunto %1 en %2").arg(before.name, datasetName));
                continue;
            }
            if (!had && has) {
                if (!afterTokens.isEmpty()) {
                    PendingChange change;
                    change.kind = PendingChange::Kind::Permission;
                    change.objectName = datasetName;
                    change.focusPermissionsNode = true;
                    appendPending(change,
                                  connIdx,
                                  poolName,
                                  QStringLiteral("zfs allow -s %1 %2 %3")
                                      .arg(after.name,
                                           afterTokens.join(','),
                                           shSingleQuote(datasetName)),
                                  QStringLiteral("Crear conjunto %1 (%2) en %3").arg(after.name, afterTokens.join(','), datasetName));
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
                PendingChange change;
                change.kind = PendingChange::Kind::Permission;
                change.objectName = datasetName;
                change.focusPermissionsNode = true;
                appendPending(change,
                              connIdx,
                              poolName,
                              cmd,
                              QStringLiteral("Actualizar conjunto %1 (%2) en %3").arg(after.name, afterTokens.join(','), datasetName));
            }
        }
    }
    for (const PendingChange& pending : m_pendingChangesModel) {
        if (pending.kind != PendingChange::Kind::Rename) {
            continue;
        }
        const PendingDatasetRenameDraft& draft = pending.renameDraft;
        if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()
            || draft.poolName.trimmed().isEmpty()
            || draft.sourceName.trimmed().isEmpty()
            || draft.targetName.trimmed().isEmpty()) {
            continue;
        }
        PendingChange change;
        change.kind = PendingChange::Kind::Rename;
        change.renameDraft = draft;
        change.objectName = draft.sourceName.trimmed();
        change.removableIndividually = true;
        change.executableIndividually = true;
        appendPending(change,
                      draft.connIdx,
                      draft.poolName,
                      pendingDatasetRenameCommand(draft),
                      QStringLiteral("Renombrar dataset %1 -> %2").arg(draft.sourceName.trimmed(), draft.targetName.trimmed()));
    }
    for (const PendingChange& pending : m_pendingChangesModel) {
        if (pending.kind != PendingChange::Kind::ShellAction) {
            continue;
        }
        const PendingShellActionDraft& draft = pending.shellDraft;
        const QString trimmed = draft.command.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QString scope = draft.scopeLabel.trimmed().isEmpty() ? QStringLiteral("local") : draft.scopeLabel.trimmed();
        PendingChange change;
        change.kind = PendingChange::Kind::ShellAction;
        change.shellDraft = draft;
        change.removableIndividually = true;
        change.executableIndividually = true;
        change.commandLine = QStringLiteral("%1  %2").arg(scope, trimmed);
        change.displayLine = QStringLiteral("%1  %2").arg(scope, draft.displayLabel.trimmed());
        change.stableId = QStringLiteral("shell|%1|%2").arg(draft.displayLabel.trimmed(), trimmed);
        if (!m_pendingChangeOrderByStableId.contains(change.stableId)) {
            m_pendingChangeOrderByStableId.insert(change.stableId, m_nextPendingChangeOrder++);
        }
        changes.push_back(change);
    }
    std::stable_sort(changes.begin(), changes.end(), [this](const PendingChange& a, const PendingChange& b) {
        const int orderA = m_pendingChangeOrderByStableId.value(a.stableId, std::numeric_limits<int>::max());
        const int orderB = m_pendingChangeOrderByStableId.value(b.stableId, std::numeric_limits<int>::max());
        return orderA < orderB;
    });
    return changes;
}

bool MainWindow::findPendingChangeByDisplayLine(const QString& line, PendingChange* out) const {
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QVector<PendingChange> changes = pendingChanges();
    for (const PendingChange& change : changes) {
        if (change.displayLine.trimmed() == trimmed) {
            if (out) {
                *out = change;
            }
            return true;
        }
    }
    return false;
}

QStringList MainWindow::pendingConnContentApplyCommands() const {
    QStringList commands;
    for (const PendingChange& change : pendingChanges()) {
        if (!change.commandLine.trimmed().isEmpty()) {
            commands.push_back(change.commandLine);
        }
    }
    return commands;
}

QStringList MainWindow::pendingConnContentApplyDisplayLines() const {
    QStringList lines;
    for (const PendingChange& change : pendingChanges()) {
        if (!change.displayLine.trimmed().isEmpty()) {
            lines.push_back(change.displayLine);
        }
    }
    return lines;
}

bool MainWindow::executePendingChange(const PendingChange& change) {
    if (!change.executableIndividually) {
        return false;
    }
    if (change.kind == PendingChange::Kind::ShellAction) {
        const PendingShellActionDraft& draft = change.shellDraft;
        if (!runLocalCommand(draft.displayLabel, draft.command, draft.timeoutMs, false, draft.streamProgress)) {
            return false;
        }
        for (int i = 0; i < m_pendingChangesModel.size(); ++i) {
            const PendingChange& existing = m_pendingChangesModel.at(i);
            if (existing.kind == PendingChange::Kind::ShellAction
                && existing.shellDraft.displayLabel.trimmed() == draft.displayLabel.trimmed()
                && existing.shellDraft.command.trimmed() == draft.command.trimmed()) {
                m_pendingChangesModel.removeAt(i);
                break;
            }
        }
        refreshPendingShellActionDraft(draft);
        return true;
    }
    if (change.kind == PendingChange::Kind::Rename) {
        const PendingDatasetRenameDraft& draft = change.renameDraft;
        if (draft.connIdx < 0 || draft.connIdx >= m_profiles.size()) {
            return false;
        }
        if (!executePendingDatasetRenameDraft(draft, true, nullptr)) {
            return false;
        }
        for (int i = 0; i < m_pendingChangesModel.size(); ++i) {
            const PendingChange& existing = m_pendingChangesModel.at(i);
            if (existing.kind == PendingChange::Kind::Rename
                && existing.renameDraft.connIdx == draft.connIdx
                && existing.renameDraft.poolName.trimmed() == draft.poolName.trimmed()
                && existing.renameDraft.sourceName.trimmed() == draft.sourceName.trimmed()
                && existing.renameDraft.targetName.trimmed() == draft.targetName.trimmed()) {
                m_pendingChangesModel.removeAt(i);
                break;
            }
        }
        invalidatePoolDatasetListingCache(draft.connIdx, draft.poolName);
        reloadConnContentPool(draft.connIdx, draft.poolName);
        reloadDatasetSide(QStringLiteral("origin"));
        reloadDatasetSide(QStringLiteral("dest"));
        return true;
    }
    return false;
}

void MainWindow::refreshPendingShellActionDraft(const PendingShellActionDraft& draft) {
    auto refreshCtx = [this](const DatasetSelectionContext& ctx,
                             bool invalidatePoolListing) {
        if (!ctx.valid || ctx.connIdx < 0 || ctx.poolName.trimmed().isEmpty()) {
            return;
        }
        if (invalidatePoolListing) {
            invalidatePoolDatasetListingCache(ctx.connIdx, ctx.poolName);
        } else if (!ctx.datasetName.trimmed().isEmpty()) {
            invalidateDatasetSubtreeCacheEntries(ctx.connIdx,
                                                ctx.poolName,
                                                ctx.datasetName,
                                                true);
        } else {
            invalidateDatasetCacheForPool(ctx.connIdx, ctx.poolName);
        }

        bool refreshed = false;
        if (m_connContentTree && m_topDetailConnIdx == ctx.connIdx) {
            reloadConnContentPool(ctx.connIdx, ctx.poolName);
            refreshed = true;
        }
        if (!refreshed) {
            refreshConnectionByIndex(ctx.connIdx);
        }
    };

    switch (draft.refreshScope) {
    case PendingShellActionDraft::RefreshScope::None:
        break;
    case PendingShellActionDraft::RefreshScope::TargetOnly:
        refreshCtx(draft.refreshTarget, true);
        break;
    case PendingShellActionDraft::RefreshScope::SourceAndTarget:
        refreshCtx(draft.refreshTarget, true);
        if (!draft.refreshSource.valid
            || draft.refreshSource.connIdx != draft.refreshTarget.connIdx
            || draft.refreshSource.poolName.trimmed() != draft.refreshTarget.poolName.trimmed()
            || draft.refreshSource.datasetName.trimmed() != draft.refreshTarget.datasetName.trimmed()) {
            refreshCtx(draft.refreshSource, false);
        }
        break;
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const QStringList pendingCommands = pendingConnContentApplyCommands();
    const QStringList pendingDisplayLines = pendingConnContentApplyDisplayLines();
    if (m_pendingChangesView) {
        const QSignalBlocker blocker(m_pendingChangesView);
        m_pendingChangesView->setPlainText(
            pendingDisplayLines.isEmpty()
                ? trk(QStringLiteral("t_apply_changes_tt_001"),
                      QStringLiteral("Sin cambios pendientes."),
                      QStringLiteral("No pending changes."),
                      QStringLiteral("没有待应用的更改。"))
                : pendingDisplayLines.join(QLatin1Char('\n')));
    }
    if (m_btnApplyConnContentProps) {
        m_btnApplyConnContentProps->setToolTip(QString());
        if (m_propsSide == QStringLiteral("conncontent")) {
            m_btnApplyConnContentProps->setEnabled(!pendingCommands.isEmpty());
            if (m_btnDiscardPendingChanges) {
                m_btnDiscardPendingChanges->setEnabled(!pendingCommands.isEmpty());
            }
            return;
        }
        if (!pendingCommands.isEmpty()) {
            m_btnApplyConnContentProps->setEnabled(true);
            if (m_btnDiscardPendingChanges) {
                m_btnDiscardPendingChanges->setEnabled(true);
            }
            return;
        }
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
    if (m_btnDiscardPendingChanges) {
        m_btnDiscardPendingChanges->setEnabled(baseEnable && m_propsSide == QStringLiteral("conncontent"));
    }
}
