#include "i18nmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

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

QString I18nManager::translate(const QString& language,
                               const QString& sourceEs,
                               const QString& fallbackEn,
                               const QString& fallbackZh) {
    const QString lang = normalizeLanguage(language);
    if (!m_catalogs.contains(lang)) {
        m_catalogs.insert(lang, loadCatalog(lang));
    }
    const auto& cat = m_catalogs[lang];
    const auto it = cat.constFind(sourceEs);
    if (it != cat.cend() && !it.value().isEmpty()) {
        return it.value();
    }
    if (lang == QStringLiteral("en") && !fallbackEn.isEmpty()) {
        return fallbackEn;
    }
    if (lang == QStringLiteral("zh") && !fallbackZh.isEmpty()) {
        return fallbackZh;
    }
    return sourceEs;
}

