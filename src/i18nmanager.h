#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

class I18nManager final {
public:
    static I18nManager& instance();

    QString translate(const QString& language,
                      const QString& sourceEs,
                      const QString& fallbackEn = QString(),
                      const QString& fallbackZh = QString());
    QString translateKey(const QString& language,
                         const QString& key,
                         const QString& fallbackEs = QString(),
                         const QString& fallbackEn = QString(),
                         const QString& fallbackZh = QString());
    bool areJsonCatalogsAvailable(QStringList* missingLanguages = nullptr) const;

private:
    I18nManager() = default;
    QHash<QString, QString> loadCatalog(const QString& language);
    QHash<QString, QString> loadLegacyAliases();
    static QString normalizeLanguage(const QString& language);

    QHash<QString, QHash<QString, QString>> m_catalogs;
    QHash<QString, QString> m_legacyAliases;
    bool m_legacyLoaded{false};
};
