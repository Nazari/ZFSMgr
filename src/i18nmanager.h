#pragma once

#include <QHash>
#include <QString>

class I18nManager final {
public:
    static I18nManager& instance();

    QString translate(const QString& language,
                      const QString& sourceEs,
                      const QString& fallbackEn = QString(),
                      const QString& fallbackZh = QString());

private:
    I18nManager() = default;
    QHash<QString, QString> loadCatalog(const QString& language);
    static QString normalizeLanguage(const QString& language);

    QHash<QString, QHash<QString, QString>> m_catalogs;
};

