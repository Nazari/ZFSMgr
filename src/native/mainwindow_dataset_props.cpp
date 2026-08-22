#include "mainwindow.h"

#include "commands/gsa.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"

#include <QApplication>
#include <QAbstractScrollArea>
#include "zfsprops.h"

#include <QComboBox>
#include <QFont>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
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
    return zfsmgr::base::zfsprops::esPropiedadDeUsuario(prop.toStdString());
}

// La familia de plataforma, el soporte por plataforma y la editabilidad viven ahora en
// `base/zfsprops`, sin Qt. Estaban DUPLICADAS letra por letra en este fichero y en el
// otro, y el servidor web necesita la misma regla para saber qué celda pinta con una caja
// de edición. Esto es solo el puente entre los QString de aquí y las cadenas de allí.
using DatasetPlatformFamily = zfsmgr::base::zfsprops::Plataforma;

DatasetPlatformFamily datasetPlatformFamilyFromStrings(const QString& osType, const QString& osLine) {
    return zfsmgr::base::zfsprops::plataformaDe(osType.toStdString(), osLine.toStdString());
}

bool isDatasetPropertySupportedOnPlatform(const QString& propName, DatasetPlatformFamily platform) {
    return zfsmgr::base::zfsprops::soportadaEn(propName.toStdString(), platform);
}

bool isDatasetPropertyEditable(const QString& propName,
                               const QString& datasetType,
                               const QString& source,
                               const QString& readonly,
                               DatasetPlatformFamily platform) {
    return zfsmgr::base::zfsprops::editableEnLinea(propName.toStdString(),
                                                   datasetType.toStdString(),
                                                   source.toStdString(),
                                                   readonly.toStdString(), platform);
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

// El motivo TIPADO que devuelve la capa base, redactado como lo hacía esta función.
//
// Los textos y sus claves son los mismos de antes a propósito: lo que se ha movido son las
// reglas, no los mensajes, y cambiarlos de paso habría mezclado dos cosas en un cambio.
QString MainWindow::gsaMensajeDeMotivo(const zfsmgr::base::gsa::Motivo& m,
                                       const QString& dataset) const {
    using F = zfsmgr::base::gsa::Fallo;
    switch (m.fallo) {
        case F::RetencionNoEntera: {
            struct { const char* prop; const char* clave; const char* es; const char* en; const char* zh; } kCuales[] = {
                {"org.fc16.gsa:horario", "t_gsa_invalid_hourly_001",
                 "La retención horaria de %1 no es válida. Debe ser un entero mayor o igual que 0.",
                 "The hourly retention for %1 is invalid. It must be an integer greater than or equal to 0.",
                 "%1 的每小时保留值无效。它必须是大于或等于 0 的整数。"},
                {"org.fc16.gsa:diario", "t_gsa_invalid_daily_001",
                 "La retención diaria de %1 no es válida. Debe ser un entero mayor o igual que 0.",
                 "The daily retention for %1 is invalid. It must be an integer greater than or equal to 0.",
                 "%1 的每日保留值无效。它必须是大于或等于 0 的整数。"},
                {"org.fc16.gsa:semanal", "t_gsa_invalid_weekly_001",
                 "La retención semanal de %1 no es válida. Debe ser un entero mayor o igual que 0.",
                 "The weekly retention for %1 is invalid. It must be an integer greater than or equal to 0.",
                 "%1 的每周保留值无效。它必须是大于或等于 0 的整数。"},
                {"org.fc16.gsa:mensual", "t_gsa_invalid_monthly_001",
                 "La retención mensual de %1 no es válida. Debe ser un entero mayor o igual que 0.",
                 "The monthly retention for %1 is invalid. It must be an integer greater than or equal to 0.",
                 "%1 的每月保留值无效。它必须是大于或等于 0 的整数。"},
                {"org.fc16.gsa:anual", "t_gsa_invalid_yearly_001",
                 "La retención anual de %1 no es válida. Debe ser un entero mayor o igual que 0.",
                 "The yearly retention for %1 is invalid. It must be an integer greater than or equal to 0.",
                 "%1 的每年保留值无效。它必须是大于或等于 0 的整数。"},
            };
            for (const auto& c : kCuales) {
                if (m.detalle == c.prop) {
                    return trk(QString::fromLatin1(c.clave), QString::fromUtf8(c.es),
                               QString::fromUtf8(c.en), QString::fromUtf8(c.zh)).arg(dataset);
                }
            }
            return QString::fromStdString(zfsmgr::base::gsa::etiquetaDe(m.fallo));
        }
        case F::ActivadaSinRetencion:
            return trk(QStringLiteral("t_gsa_requires_retention_001"),
                       QStringLiteral("La programación GSA de %1 está activada pero no tiene ninguna retención mayor que 0."),
                       QStringLiteral("GSA scheduling for %1 is enabled but it does not have any retention greater than 0."),
                       QStringLiteral("%1 的 GSA 计划已启用，但没有任何大于 0 的保留值。")).arg(dataset);
        case F::NivelarSinDestino:
            return trk(QStringLiteral("t_gsa_level_dest_required_001"),
                       QStringLiteral("La programación GSA de %1 tiene Nivelar=on pero no tiene Destino."),
                       QStringLiteral("GSA scheduling for %1 has Level=on but no Destination."),
                       QStringLiteral("%1 的 GSA 计划启用了层级同步，但未指定目标。")).arg(dataset);
        case F::DestinoMalFormado:
            return trk(QStringLiteral("t_gsa_dest_format_001"),
                       QStringLiteral("El destino GSA de %1 debe tener formato Con::Pool/Dataset."),
                       QStringLiteral("The GSA destination for %1 must use the Con::Pool/Dataset format."),
                       QStringLiteral("%1 的 GSA 目标必须使用 Con::Pool/Dataset 格式。")).arg(dataset);
        case F::DestinoSinConexion:
            return trk(QStringLiteral("t_gsa_dest_conn_missing_001"),
                       QStringLiteral("El destino GSA de %1 referencia una conexión inexistente: %2."),
                       QStringLiteral("The GSA destination for %1 references a missing connection: %2."),
                       QStringLiteral("%1 的 GSA 目标引用了不存在的连接：%2。"))
                .arg(dataset, QString::fromStdString(m.detalle));
        case F::ChocaConRecursiva:
            return trk(QStringLiteral("t_gsa_recursive_child_conflict_001"),
                       QStringLiteral("No se puede programar %1 porque %2 ya tiene una programación GSA recursiva."),
                       QStringLiteral("%1 cannot be scheduled because %2 already has a recursive GSA schedule."),
                       QStringLiteral("无法为 %1 设置计划，因为 %2 已经有递归 GSA 计划。"))
                .arg(dataset, QString::fromStdString(m.detalle));
        case F::Ninguno:
            break;
    }
    return QString::fromStdString(zfsmgr::base::gsa::etiquetaDe(m.fallo));
}

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
    for (auto itConn = m_conns.connInfoById.cbegin(); itConn != m_conns.connInfoById.cend(); ++itConn) {
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

                const QString token = QStringLiteral("%1::%2").arg(connToken(connIdx), poolName);
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
                if (!state.hasExplicitConfig) {
                    continue;
                }

                // Las REGLAS están en la capa base (`src/base/gsa.h`), no aquí. Vivían en
                // esta función y en ningún sitio más, así que el intérprete no podía
                // programar sin reescribirlas — y serían dos copias que se separan. Aquí
                // queda lo que sí es de la interfaz: reunir las propiedades y redactar el
                // motivo en los tres idiomas.
                std::map<std::string, std::string> propsStd;
                for (auto pit = propValues.cbegin(); pit != propValues.cend(); ++pit) {
                    propsStd[pit.key().toStdString()] = pit.value().toStdString();
                }
                zfsmgr::base::gsa::Programacion prog;
                zfsmgr::base::gsa::Motivo motivo;
                const auto conexionExiste = [this](const std::string& nombre) {
                    return connectionIndexByNameOrId(QString::fromStdString(nombre)) >= 0;
                };
                if (!zfsmgr::base::gsa::desdePropiedades(propsStd, prog, motivo)
                    || !zfsmgr::base::gsa::valida(datasetName.toStdString(), prog, conexionExiste,
                                                  motivo)) {
                    return fail(gsaMensajeDeMotivo(motivo, datasetName));
                }
                state.enabled = prog.activado;
                state.recursive = prog.recursivo;
                state.level = prog.nivelar;
                state.destination = QString::fromStdString(prog.destino);
                state.hourly = prog.horario;
                state.daily = prog.diario;
                state.weekly = prog.semanal;
                state.monthly = prog.mensual;
                state.yearly = prog.anual;

                statesByKey.insert(QStringLiteral("%1::%2::%3").arg(connToken(connIdx)).arg(poolName, datasetName), state);
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

    // El choque entre recursivas, agrupando por máquina y pool, que es donde tiene
    // sentido: dos pools distintos no se solapan aunque los datasets se llamen igual.
    QMap<QString, QVector<int>> porPool;
    for (int i = 0; i < enabledStates.size(); ++i) {
        const GsaState& s = enabledStates.at(i);
        porPool[QStringLiteral("%1|%2").arg(s.connIdx).arg(s.poolName.toLower())].push_back(i);
    }
    for (auto it = porPool.cbegin(); it != porPool.cend(); ++it) {
        std::vector<zfsmgr::base::gsa::Entrada> juego;
        for (int idx : it.value()) {
            const GsaState& s = enabledStates.at(idx);
            zfsmgr::base::gsa::Programacion prog;
            prog.activado = s.enabled;
            prog.recursivo = s.recursive;
            juego.push_back({s.datasetName.toStdString(), prog});
        }
        zfsmgr::base::gsa::Motivo motivo;
        if (!zfsmgr::base::gsa::validaConjunto(juego, motivo)) {
            return fail(gsaMensajeDeMotivo(motivo, QString::fromStdString(motivo.dataset)));
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

bool MainWindow::runDatasetRenameNow(const PendingDatasetRenameDraft& draft, QString* errorOut) {
    // Renombrar deja de encolarse y se hace al momento, como el resto de las acciones.
    //
    // Con ello desaparecen dos reglas que solo tenían sentido en una cola: rechazar un
    // segundo renombrado hacia el mismo nombre —dos entradas que chocarían al aplicarse— y
    // sustituir el renombrado anterior del mismo dataset. Sin cola no hay nada que chocar:
    // lo que decide si el nombre está libre es ZFS, en el momento, y su respuesta es la
    // verdad en vez de una predicción nuestra.
    auto fail = [this, errorOut, &draft](const QString& text) {
        if (errorOut) {
            *errorOut = text;
        }
        appLog(QStringLiteral("WARN"),
               QStringLiteral("[accion] no se renombró «%1»: %2").arg(draft.sourceName.trimmed(), text));
        return false;
    };
    if (draft.connIdx < 0 || draft.connIdx >= m_conns.profiles.size()) {
        return fail(QStringLiteral("Conexión inválida para el renombrado."));
    }
    const QString poolName = draft.poolName.trimmed();
    const QString sourceName = draft.sourceName.trimmed();
    const QString targetName = draft.targetName.trimmed();
    if (poolName.isEmpty() || sourceName.isEmpty() || targetName.isEmpty()) {
        return fail(QStringLiteral("Faltan datos para el renombrado."));
    }
    if (sourceName == targetName) {
        return fail(QStringLiteral("El origen y el destino del renombrado son iguales."));
    }
    const PendingDatasetRenameDraft limpio{draft.connIdx, poolName, sourceName, targetName};
    if (!executePendingDatasetRenameDraft(limpio, true, nullptr)) {
        if (m_lastActionWasCancelled) {
            m_lastActionWasCancelled = false;
            if (errorOut) {
                errorOut->clear();
            }
            return false;
        }
        return fail(QStringLiteral("El renombrado falló; revise el registro."));
    }
    invalidatePoolDatasetListingCache(limpio.connIdx, limpio.poolName);
    reloadConnContentPool(limpio.connIdx, limpio.poolName);
    reloadDatasetSide(QStringLiteral("origin"));
    reloadDatasetSide(QStringLiteral("dest"));
    return true;
}

void MainWindow::refreshDatasetProperties(const QString& side) {
    refreshDatasetProperties(side, m_connContentTree);
}

void MainWindow::refreshDatasetProperties(const QString& side, QTreeWidget* connContentTree) {
    static thread_local bool s_refreshDatasetPropertiesInProgress = false;
    if (s_refreshDatasetPropertiesInProgress) {
        return;
    }
    QScopedValueRollback<bool> refreshGuard(s_refreshDatasetPropertiesInProgress, true);
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
    int connIdx = -1;
    QString poolName;
    if (!splitConnToken(token, connIdx, poolName)) {
        m_propsToken.clear();
        endTransientUiBusy();
        return;
    }
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
    const ConnectionProfile p = m_conns.profiles[connIdx];
    const DatasetPlatformFamily platform =
        datasetPlatformFamilyFromStrings(p.osType, (connIdx >= 0 && connIdx < m_conns.states.size()) ? m_conns.states[connIdx].osLine : QString());

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
        ;
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
    // La tabla vive en la capa base: la comparten la interfaz y el intérprete, que la usa
    // para completar con el tabulador. Tenerla aquí dentro impedía que el CLI la ofreciera,
    // y copiarla habría sido garantizar que las dos se separen.
    QMap<QString, QStringList> enumValues;
    for (const auto& kv : zfsmgr::base::zfsprops::propiedadesConValores()) {
        QStringList vals;
        for (const std::string& v : kv.second) {
            vals << QString::fromStdString(v);
        }
        enumValues.insert(QString::fromStdString(kv.first), vals);
    }
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
        ) {
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
        startPendingApplyAnimation();
        struct PendingApplySuppressionGuard {
            MainWindow* self{nullptr};
            explicit PendingApplySuppressionGuard(MainWindow* w) : self(w) {
                if (self) {
                    self->m_pendingApplyFinishSuppressed = true;
                }
            }
            ~PendingApplySuppressionGuard() {
                if (self) {
                    self->m_pendingApplyFinishSuppressed = false;
                }
            }
        };
        PendingApplySuppressionGuard pendingApplyGuard(this);
        auto pendingLinePrefix = [this](int connIdx, const QString& poolName) {
            if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
                return QStringLiteral("%1::%2").arg(connToken(connIdx), poolName.trimmed());
            }
            const ConnectionProfile p = m_conns.profiles.at(connIdx);
            const QString connLabel = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
            return QStringLiteral("%1::%2").arg(connLabel, poolName.trimmed());
        };
        // El tick de estado por fila (pendiente / en curso / hecha / fallida) se pintaba
        // sobre la lista, y esa lista ya no muestra cambios sino trabajos. Lo que sí hay que
        // conservar es el respiro al bucle de eventos entre pasos largos, o la ventana se
        // queda congelada mientras se aplica una tanda de propiedades.
        auto markPendingRunning = [](const QString&) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        };
        auto markPendingDone = [](const QString&, bool) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        };
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

            struct PropertyOp {
                QString command;
                QString displayLine;
            };
            QVector<PropertyOp> ops;
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
                        ops.push_back(PropertyOp{
                            QStringLiteral("zfs %1 %2")
                                .arg((isWindowsConnection(item.ctx.connIdx)
                                          ? (finalMounted ? QStringLiteral("mount")
                                                          : QStringLiteral("unmount"))
                                          : (finalMounted ? QStringLiteral("mount")
                                                          : QStringLiteral("umount"))),
                                     shSingleQuote(item.ctx.datasetName)),
                            QStringLiteral("%1 dataset %2")
                                .arg(finalMounted ? QStringLiteral("Montar")
                                                  : QStringLiteral("Desmontar"),
                                     item.objectName)
                        });
                    }
                    continue;
                }
                if (finalInh != originalInh) {
                    if (finalInh) {
                        ops.push_back(PropertyOp{
                            QStringLiteral("zfs inherit %1 %2")
                                .arg(shSingleQuote(prop), shSingleQuote(item.ctx.datasetName)),
                            QStringLiteral("Heredar propiedad %1 en %2").arg(prop, item.objectName)
                        });
                    } else {
                        ops.push_back(PropertyOp{
                            QStringLiteral("zfs set %1 %2")
                                .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                     shSingleQuote(item.ctx.datasetName)),
                            QStringLiteral("Cambiar propiedad %1=%2 en %3").arg(prop, finalValue, item.objectName)
                        });
                    }
                    continue;
                }
                if (!finalInh && finalValue != originalValue) {
                    ops.push_back(PropertyOp{
                        QStringLiteral("zfs set %1 %2")
                            .arg(shSingleQuote(prop + QStringLiteral("=") + finalValue),
                                 shSingleQuote(item.ctx.datasetName)),
                        QStringLiteral("Cambiar propiedad %1=%2 en %3").arg(prop, finalValue, item.objectName)
                    });
                }
            }

            if (ops.isEmpty()) {
                storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
                continue;
            }

            const bool isWin = isWindowsConnection(item.ctx.connIdx);
            const bool useGranularDatasetRefresh = touchedAnyProperty;
            const bool useGranularGsaRefresh = useGranularDatasetRefresh && touchedOnlyGsaProperties;
            for (const PropertyOp& op : std::as_const(ops)) {
                const QString displayLine =
                    QStringLiteral("%1  %2").arg(pendingLinePrefix(item.ctx.connIdx, item.ctx.poolName),
                                                 op.displayLine.trimmed());
                markPendingRunning(displayLine);
                const QString cmd = op.command;
                // Registrar ANTES de ejecutar, siempre.
                //
                // Un cambio de propiedad aplicado en Windows no dejó NI UNA línea en el
                // registro: se cambió `driveletter`, el pool se suspendió a continuación
                // y no había forma de saber qué orden se había enviado ni si había
                // llegado a enviarse. Toda la depuración de esta jornada se ha apoyado en
                // el registro; una acción que cambia estado sin dejar rastro no puede
                // existir.
                appLog(QStringLiteral("NORMAL"),
                       QStringLiteral("Aplicar propiedades en %1::%2 -> %3")
                           .arg(m_conns.profiles.value(item.ctx.connIdx).name,
                                item.objectName,
                                mwhelpers::oneLine(mwhelpers::maskCommandSecrets(cmd))));
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
                                                        restoreConnContentTreeStateFor(tree, token);
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
                    markPendingDone(displayLine, false);
                    updateApplyPropsButtonState();
                    return;
                }
                markPendingDone(displayLine, true);
            }
            if (!useGranularDatasetRefresh) {
                connectionsToRefresh.insert(item.ctx.connIdx);
            }
            storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
        }

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
        for (auto itConn = m_conns.connInfoById.cbegin(); itConn != m_conns.connInfoById.cend(); ++itConn) {
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
            const DatasetPermissionsCacheEntry entry = pendingPerm.entry;

            // El diff —qué se retira y qué se concede para que lo delegado coincida con lo
            // que muestra la ficha— vivía aquí, en una lambda dentro de esta función de 400
            // líneas. Eso lo hacía imposible de probar: tiene CUATRO estados por entrada
            // —aparece, desaparece, cambia, sigue igual— por tres alcances, más «al crear» y
            // los conjuntos con nombre. Doce combinaciones que nunca se comprobaron.
            //
            // Ahora es una función pura en `uilogic`, con un caso de prueba por combinación,
            // y compone el argv con `commands::zfsallow` en vez de pegar cadenas de shell.
            const QList<QStringList> ordenes =
                zfsmgr::uilogic::permissionChangeCommands(entry, datasetName);
            QStringList subcmds;
            for (const QStringList& argv : ordenes) {
                subcmds << mwhelpers::cadenaDeArgv(QStringLiteral("zfs"), argv);
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
            const QString permissionDisplayLine =
                QStringLiteral("%1  %2")
                    .arg(pendingLinePrefix(connIdx, poolName),
                         QStringLiteral("Actualizar permisos en %1").arg(datasetName));
            markPendingRunning(permissionDisplayLine);
            // La cadena se sigue componiendo, pero solo para el respaldo por SSH y para la
            // vista previa de la confirmación. Lo que viaja al daemon es el LOTE de argv:
            // antes esta cadena se volvía a trocear por «; » al otro lado, con un corte que
            // no respeta comillas.
            const QString cmd = subcmds.join(QStringLiteral("; "));
            const QStringList loteArgv = daemonizeZfsAllowBatchArgs(connIdx, ordenes);
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
                                      },
                                      loteArgv)) {
                markPendingDone(permissionDisplayLine, false);
                updateApplyPropsButtonState();
                return;
            }
            markPendingDone(permissionDisplayLine, true);
        }

        // Aquí se aplicaban los RENOMBRADOS y las ACCIONES que esperaban en la lista.
        //
        // Ya no espera ninguna: se ejecutan al pulsarlas. Este bloque recorría un modelo
        // que nadie llena, así que no hacía nada —pero seguía pidiendo credenciales de sudo
        // y montando vistas previas para una lista vacía—. Lo que queda de esta función es
        // lo suyo: aplicar los borradores de propiedades y de permisos, que sí se editan en
        // lote y sí tienen un botón que los aplica.

        for (int connIdx : std::as_const(connectionsToRefresh)) {
            if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
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
        for (auto itConn = m_conns.connInfoById.cbegin(); itConn != m_conns.connInfoById.cend() && !hasPermissionDrafts; ++itConn) {
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
                       ;
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
        {
            int connIdx = -1;
            QString poolName;
            if (splitConnToken(tokenCtx, connIdx, poolName)) {
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
        int connIdx = -1;
        QString poolName;
        if (!splitConnToken(token, connIdx, poolName)) {
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

QStringList MainWindow::pendingConnContentApplyCommands() const {
    QStringList commands;
    return commands;
}

QStringList MainWindow::pendingConnContentApplyDisplayLines() const {
    QStringList lines;
    return lines;
}

int MainWindow::pendingShellSingleConnectionIdx(const PendingShellActionDraft& draft) const {
    QSet<int> connIdxs;
    if (draft.refreshSource.valid && draft.refreshSource.connIdx >= 0) {
        connIdxs.insert(draft.refreshSource.connIdx);
    }
    if (draft.refreshTarget.valid && draft.refreshTarget.connIdx >= 0) {
        connIdxs.insert(draft.refreshTarget.connIdx);
    }
    if (connIdxs.size() != 1) {
        return -1;
    }
    const int connIdx = *connIdxs.cbegin();
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        return -1;
    }
    return connIdx;
}

bool MainWindow::tryExecutePendingShellActionRemotely(const PendingShellActionDraft& draft,
                                                       bool* handledOut,
                                                       QString* failureDetailOut) {
    if (handledOut) {
        *handledOut = false;
    }
    const int connIdx = pendingShellSingleConnectionIdx(draft);
    if (connIdx < 0 || connIdx >= m_conns.profiles.size()) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc skip: conexión no resolvible (single-conn) para \"%1\"")
                   .arg(draft.displayLabel.trimmed()));
        return true;
    }
    const ConnectionProfile p = m_conns.profiles.at(connIdx);
    if (isWindowsConnection(p)) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc skip: Windows no soportado para \"%1\" en %2")
                   .arg(draft.displayLabel.trimmed(), p.name));
        return true;
    }

    QString rawCmd = draft.command.trimmed();
    if (rawCmd.isEmpty()) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc skip: comando vacío para \"%1\" en %2")
                   .arg(draft.displayLabel.trimmed(), p.name));
        return true;
    }
    // Si el pendiente viene envuelto con sshExecFromLocal(conn, remoteCmd),
    // desempaquetar remoteCmd para intentar daemon-rpc directamente.
    if (!isLocalConnection(p)) {
        const QStringList outerParts = QProcess::splitCommand(rawCmd);
        if (outerParts.size() >= 3 && outerParts.first().trimmed() == QStringLiteral("ssh")) {
            const QString expectedTarget = mwhelpers::sshUserHost(p).trimmed();
            int targetIdx = -1;
            auto sshOptionNeedsValue = [](const QString& tok) {
                static const QSet<QString> opts = {
                    QStringLiteral("-B"), QStringLiteral("-b"), QStringLiteral("-c"),
                    QStringLiteral("-D"), QStringLiteral("-E"), QStringLiteral("-e"),
                    QStringLiteral("-F"), QStringLiteral("-I"), QStringLiteral("-i"),
                    QStringLiteral("-J"), QStringLiteral("-L"), QStringLiteral("-l"),
                    QStringLiteral("-m"), QStringLiteral("-O"), QStringLiteral("-o"),
                    QStringLiteral("-p"), QStringLiteral("-Q"), QStringLiteral("-R"),
                    QStringLiteral("-S"), QStringLiteral("-W"), QStringLiteral("-w")
                };
                if (opts.contains(tok)) {
                    return true;
                }
                // Opciones compactas tipo -p22, -oStrictHostKeyChecking=no, -i/path
                const QString t = tok.trimmed();
                if (t.startsWith(QStringLiteral("-p")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-o")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-i")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-F")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-J")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-S")) && t.size() > 2) return false;
                if (t.startsWith(QStringLiteral("-W")) && t.size() > 2) return false;
                return false;
            };
            for (int i = 1; i < outerParts.size(); ++i) {
                const QString tok = outerParts.at(i).trimmed();
                if (tok.startsWith(QLatin1Char('-'))) {
                    if (sshOptionNeedsValue(tok) && i + 1 < outerParts.size()) {
                        ++i;
                    }
                    continue;
                }
                if (tok == expectedTarget) {
                    targetIdx = i;
                    break;
                }
            }
            if (targetIdx >= 1 && targetIdx + 1 == outerParts.size() - 1) {
                const QString unwrapped = outerParts.last().trimmed();
                if (!unwrapped.isEmpty()) {
                    rawCmd = unwrapped;
                }
            }
        }
    }

    const QStringList parts = QProcess::splitCommand(rawCmd);
    if (parts.size() < 2) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc skip: comando no parseable para \"%1\" en %2")
                   .arg(draft.displayLabel.trimmed(), p.name));
        return true;
    }
    const QString tool = parts.at(0).trimmed().toLower();
    QStringList execArgv;
    QString remoteActionLabel =
        draft.displayLabel.trimmed().isEmpty()
            ? QStringLiteral("Pending shell")
            : draft.displayLabel.trimmed();
    if (tool == QStringLiteral("zfs")) {
        execArgv = daemonizeZfsMutationArgs(connIdx, rawCmd);
    } else if (tool == QStringLiteral("zpool")) {
        execArgv = daemonizeZpoolMutationArgs(connIdx, rawCmd);
    } else {
        // Evita comandos que dependen de orquestación local (ssh/powershell/pscp/scp).
        const QString lc = rawCmd.toLower();
        if (lc.contains(QStringLiteral("ssh "))
            || lc.contains(QStringLiteral("\tssh "))
            || lc.contains(QStringLiteral(" powershell "))
            || lc.contains(QStringLiteral(" pwsh "))
            || lc.contains(QStringLiteral(" scp "))
            || lc.contains(QStringLiteral(" pscp "))) {
            appLog(QStringLiteral("DEBUG"),
                   QStringLiteral("Pending shell daemon-rpc skip: comando de orquestación local para \"%1\" en %2")
                       .arg(draft.displayLabel.trimmed(), p.name));
            return true;
        }
        execArgv = daemonizeShellMutationArgs(connIdx, rawCmd);
        remoteActionLabel =
            draft.displayLabel.trimmed().isEmpty()
                ? QStringLiteral("Pending shell generic")
                : draft.displayLabel.trimmed();
    }
    if (execArgv.isEmpty()) {
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc skip: comando no permitido/no compatible para \"%1\" en %2")
                   .arg(draft.displayLabel.trimmed(), p.name));
        return true;
    }
    if (handledOut) {
        *handledOut = true;
    }
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("Pending shell daemon-rpc: ejecutando \"%1\" en %2")
               .arg(remoteActionLabel, p.name));

    ConnectionProfile sudoProfile = p;
    if (!ensureLocalSudoCredentials(sudoProfile)) {
        if (failureDetailOut) {
            *failureDetailOut = QStringLiteral("faltan credenciales sudo locales");
        }
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc fallo: sudo local no disponible para \"%1\" en %2")
                   .arg(remoteActionLabel, p.name));
        return false;
    }
    // agentShellCommand aplica sudo y PATH; no debe envolverse otra vez.
    const QString remoteCmd = mwhelpers::agentShellCommand(sudoProfile, execArgv);
    QString detail;
    const bool ok = executeConnectionCommand(connIdx,
                                             remoteActionLabel,
                                             remoteCmd,
                                             draft.timeoutMs > 0 ? draft.timeoutMs : 60000,
                                             &detail);
    if (!ok) {
        if (failureDetailOut) {
            *failureDetailOut = detail;
        }
        appLog(QStringLiteral("DEBUG"),
               QStringLiteral("Pending shell daemon-rpc fallo en \"%1\" (%2): %3")
                   .arg(remoteActionLabel, p.name, mwhelpers::oneLine(detail)));
        return false;
    }
    appLog(QStringLiteral("DEBUG"),
           QStringLiteral("Pending shell daemon-rpc OK: \"%1\" en %2")
               .arg(remoteActionLabel, p.name));
    return true;
}

bool MainWindow::executeShellActionDraft(const PendingShellActionDraft& draft) {
    // Recibe el BORRADOR, no una entrada de la lista.
    //
    // Antes tomaba un `PendingChange` porque venía de una cola, y traía consigo una rama
    // para el renombrado y una comprobación de «¿es ejecutable por separado?» que solo tenía
    // sentido con varias entradas en pantalla. Sin cola, quien llama tiene el borrador y lo
    // ejecuta: no hay nada que seleccionar ni que marcar.
    {
        // Aquí había un aviso de «esta orden la construyó una versión anterior del
        // programa». Existía porque la lista de pendientes guardaba la orden YA CONSTRUIDA y
        // podía esperar días en disco: si entretanto se corregía cómo se construye, la de la
        // lista seguía siendo la vieja. Pasó de verdad —un «Montar» encolado antes de que
        // Montar aprendiera a cargar la clave de un dataset cifrado falló después con
        // «encryption key not loaded», sin que nada dijera que la orden era anterior—.
        //
        // Al ejecutar al instante, quien construye y quien ejecuta son el mismo binario
        // siempre, así que el aviso no podía dispararse. Se retira con la lista.
        // Al fallar hay que refrescar igualmente. Una orden puede fallar DESPUÉS de haber
        // cambiado cosas —el caso real: Desde Dir creaba el dataset y luego se rechazaba
        // por no tener punto de montaje—, y sin refrescar el árbol se queda mintiendo: lo
        // creado no aparece, el usuario lo vuelve a intentar y choca con que ya existe.
        auto failAndRefresh = [this, &draft]() {
            refreshPendingShellActionDraft(draft);
            return false;
        };
        // Paso previo tipado, si lo hay. Va ANTES que la orden de shell y, si falla, no
        // se ejecuta nada más: en Desde Dir este paso crea el dataset destino, y sin él
        // la tubería de tar escribiría en un sitio que no existe.
        appLog(QStringLiteral("INFO"),
               QStringLiteral("[pendiente] %1: paso previo RPC conn=%2 argv=%3")
                   .arg(draft.displayLabel.trimmed())
                   .arg(draft.rpcConnIdx)
                   .arg(draft.rpcArgv.size()));
        if (!applyFromDirCreateStep(draft.rpcConnIdx, draft.rpcArgv, draft.rpcSecret,
                                    draft.displayLabel.trimmed())) {
            return failAndRefresh();
        }
        // Acción de dataset diferida (Desglosar, Ensamblar): se ejecuta por su camino
        // de siempre, con su confirmación, su envío como trabajo y su progreso. Lo
        // único que cambia respecto a antes es CUÁNDO ocurre.
        if (!draft.datasetActionName.isEmpty()) {
            const bool okAction = executeDatasetAction(draft.datasetActionSide,
                                                       draft.datasetActionName,
                                                       draft.datasetActionCtx,
                                                       draft.command,
                                                       0,
                                                       draft.datasetActionAllowWindowsScript,
                                                       draft.datasetActionStdin,
                                                       true,
                                                       {},
                                                       draft.datasetActionArgv);
            if (!okAction) {
                return failAndRefresh();
            }
            refreshPendingShellActionDraft(draft);
            return true;
        }
        bool handledRemotely = false;
        QString remoteFailure;
        if (!tryExecutePendingShellActionRemotely(draft, &handledRemotely, &remoteFailure)) {
            return failAndRefresh();
        }
        if (!handledRemotely
            && !runLocalCommand(draft.displayLabel, draft.command, draft.timeoutMs, false, draft.streamProgress)) {
            return failAndRefresh();
        }
        // Ya no hay lista de la que desactivar la entrada ni a la que devolverle el estado:
        // solo hace falta refrescar lo que la acción haya tocado.
        refreshPendingShellActionDraft(draft);
        return true;
    }
}

void MainWindow::refreshPendingShellActionDraft(const PendingShellActionDraft& draft) {
    auto refreshCtx = [this](const DatasetSelectionContext& ctx,
                             bool invalidatePoolListing,
                             const PendingShellActionDraft& shellDraft) {
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

        if (shouldRefreshSizePropsForCommand(shellDraft.displayLabel, shellDraft.command)
            && !ctx.datasetName.trimmed().isEmpty()) {
            refreshDatasetAndPoolSizeProperties(ctx.connIdx, ctx.poolName, ctx.datasetName);
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
        refreshCtx(draft.refreshTarget, true, draft);
        break;
    case PendingShellActionDraft::RefreshScope::SourceAndTarget:
        refreshCtx(draft.refreshTarget, true, draft);
        if (!draft.refreshSource.valid
            || draft.refreshSource.connIdx != draft.refreshTarget.connIdx
            || draft.refreshSource.poolName.trimmed() != draft.refreshTarget.poolName.trimmed()
            || draft.refreshSource.datasetName.trimmed() != draft.refreshTarget.datasetName.trimmed()) {
            refreshCtx(draft.refreshSource, false, draft);
        }
        break;
    }
}

void MainWindow::updateApplyPropsButtonState() {
    const QStringList pendingCommands = pendingConnContentApplyCommands();
    if (m_pendingApplyInProgress && !m_pendingApplyFinishSuppressed) {
        finishPendingApplyAnimation();
    }
    updatePendingChangesList();
    if (m_btnApplyConnContentProps) {
        m_btnApplyConnContentProps->setToolTip(QString());
        // Descartar mira la lista ENTERA, no solo lo activo: si todo está desmarcado
        // —el estado normal tras aplicar— el usuario tiene que poder vaciarla igual, sin
        // ir borrando una a una.
        const bool hasAnyRow = !pendingConnContentApplyDisplayLines().isEmpty();
        if (m_propsSide == QStringLiteral("conncontent")) {
            m_btnApplyConnContentProps->setEnabled(!pendingCommands.isEmpty());
            if (m_btnDiscardPendingChanges) {
                m_btnDiscardPendingChanges->setEnabled(hasAnyRow);
            }
            return;
        }
        if (!pendingCommands.isEmpty() || hasAnyRow) {
            m_btnApplyConnContentProps->setEnabled(!pendingCommands.isEmpty());
            if (m_btnDiscardPendingChanges) {
                m_btnDiscardPendingChanges->setEnabled(hasAnyRow);
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

// Descartar los borradores de edición sin aplicar: propiedades y permisos.
//
// Se llamaba `discardAllDraftEdits` y vaciaba además la cola de acciones y su copia en
// disco. Sin cola, lo único que queda por descartar son las ediciones a medias del árbol,
// que es lo que el botón «Deshacer» ha significado siempre para quien lo usa.
void MainWindow::discardAllDraftEdits() {
    const QVector<PendingPropertyDraftEntry> propertyDrafts = pendingConnContentPropertyDraftsFromModel();
    for (const PendingPropertyDraftEntry& item : propertyDrafts) {
        storePropertyDraftForObject(QStringLiteral("conncontent"), item.token, item.objectName, DatasetPropsDraft{});
    }
    resetAllDatasetPermissionDrafts();
    for (auto itConn = m_conns.connInfoById.begin(); itConn != m_conns.connInfoById.end(); ++itConn) {
        for (auto itPool = itConn->poolsByStableId.begin(); itPool != itConn->poolsByStableId.end(); ++itPool) {
            for (auto itDs = itPool->objectsByFullName.begin(); itDs != itPool->objectsByFullName.end(); ++itDs) {
                itDs->editSession.clear();
            }
        }
    }
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
