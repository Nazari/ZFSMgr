#include "i18nmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
QStringList catalogCandidates(const QString& fileName) {
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << QDir(appDir).filePath(QStringLiteral("i18n/%1").arg(fileName));
#ifdef Q_OS_MAC
    {
        QDir d(appDir);
        if (d.cdUp() && d.cdUp()) {
            candidates << d.filePath(QStringLiteral("Resources/i18n/%1").arg(fileName));
        }
    }
#endif
    candidates << QStringLiteral(":/i18n/%1").arg(fileName);
    return candidates;
}
}

I18nManager& I18nManager::instance() {
    static I18nManager mgr;
    return mgr;
}

QString I18nManager::normalizeLanguage(const QString& language) {
    const QString l = language.trimmed().toLower();
    if (l == QStringLiteral("en")) return QStringLiteral("en");
    if (l == QStringLiteral("zh")) return QStringLiteral("zh");
    return QStringLiteral("es");
}

QHash<QString, QString> I18nManager::loadCatalog(const QString& language) {
    const QString lang = normalizeLanguage(language);
    const QString fileName = QStringLiteral("%1.json").arg(lang);
    const QStringList candidates = catalogCandidates(fileName);

    for (const QString& path : candidates) {
        QFile f(path);
        if (!f.exists()) {
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        QJsonObject table;
        if (obj.contains(QStringLiteral("translations")) && obj.value(QStringLiteral("translations")).isObject()) {
            table = obj.value(QStringLiteral("translations")).toObject();
        } else {
            table = obj;
        }
        QHash<QString, QString> map;
        for (auto it = table.begin(); it != table.end(); ++it) {
            map.insert(it.key(), it.value().toString());
        }
        return map;
    }
    return {};
}

QHash<QString, QString> I18nManager::loadLegacyAliases() {
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << QDir(appDir).filePath(QStringLiteral("i18n/legacy_keys.json"));
#ifdef Q_OS_MAC
    {
        QDir d(appDir);
        if (d.cdUp() && d.cdUp()) {
            candidates << d.filePath(QStringLiteral("Resources/i18n/legacy_keys.json"));
        }
    }
#endif
    candidates << QStringLiteral(":/i18n/legacy_keys.json");

    for (const QString& path : candidates) {
        QFile f(path);
        if (!f.exists()) {
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject root = doc.object();
        if (!root.contains(QStringLiteral("legacy_keys")) || !root.value(QStringLiteral("legacy_keys")).isObject()) {
            continue;
        }
        const QJsonObject table = root.value(QStringLiteral("legacy_keys")).toObject();
        QHash<QString, QString> map;
        for (auto it = table.begin(); it != table.end(); ++it) {
            const QString legacy = it.key();
            const QString id = it.value().toString().trimmed();
            if (!legacy.isEmpty() && !id.isEmpty()) {
                map.insert(legacy, id);
            }
        }
        return map;
    }
    return {};
}

QString I18nManager::translate(const QString& language,
                               const QString& sourceEs,
                               const QString& fallbackEn,
                               const QString& fallbackZh) {
    Q_UNUSED(fallbackEn);
    Q_UNUSED(fallbackZh);
    const QString lang = normalizeLanguage(language);
    if (!m_catalogs.contains(lang)) {
        m_catalogs.insert(lang, loadCatalog(lang));
    }
    if (!m_legacyLoaded) {
        m_legacyAliases = loadLegacyAliases();
        m_legacyLoaded = true;
    }
    const auto& cat = m_catalogs[lang];
    QString lookupKey = sourceEs;
    if (!lookupKey.isEmpty() && !cat.contains(lookupKey)) {
        const auto legacyIt = m_legacyAliases.constFind(lookupKey);
        if (legacyIt != m_legacyAliases.cend()) {
            lookupKey = legacyIt.value();
        }
    }
    const auto it = cat.constFind(lookupKey);
    if (it != cat.cend() && !it.value().isEmpty()) {
        return it.value();
    }
    return sourceEs;
}

QString I18nManager::translateKey(const QString& language,
                                  const QString& key,
                                  const QString& fallbackEs,
                                  const QString& fallbackEn,
                                  const QString& fallbackZh) {
    const QString lang = normalizeLanguage(language);
    if (!m_catalogs.contains(lang)) {
        m_catalogs.insert(lang, loadCatalog(lang));
    }
    const auto tryFromLang = [&](const QString& l, const QString& k) -> QString {
        if (!m_catalogs.contains(l)) {
            m_catalogs.insert(l, loadCatalog(l));
        }
        const auto& cat = m_catalogs[l];
        const auto it = cat.constFind(k);
        if (it != cat.cend() && !it.value().isEmpty()) {
            return it.value();
        }
        return QString();
    };

    QString out = tryFromLang(lang, key);
    if (!out.isEmpty()) {
        return out;
    }
    if (lang == QStringLiteral("en") && !fallbackEn.isEmpty()) {
        return fallbackEn;
    }
    if (lang == QStringLiteral("zh") && !fallbackZh.isEmpty()) {
        return fallbackZh;
    }
    if (lang == QStringLiteral("es") && !fallbackEs.isEmpty()) {
        return fallbackEs;
    }
    out = tryFromLang(QStringLiteral("es"), key);
    if (!out.isEmpty()) {
        return out;
    }
    if (!fallbackEs.isEmpty()) {
        return fallbackEs;
    }
    return key;
}

bool I18nManager::areJsonCatalogsAvailable(QStringList* missingLanguages) const {
    QStringList missing;
    const QStringList langs = {QStringLiteral("es"), QStringLiteral("en"), QStringLiteral("zh")};
    for (const QString& lang : langs) {
        const QString fileName = QStringLiteral("%1.json").arg(lang);
        bool found = false;
        for (const QString& path : catalogCandidates(fileName)) {
            if (QFile::exists(path)) {
                found = true;
                break;
            }
        }
        if (!found) {
            missing << lang;
        }
    }
    if (missingLanguages) {
        *missingLanguages = missing;
    }
    return missing.isEmpty();
}
