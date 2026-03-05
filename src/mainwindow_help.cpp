#include "mainwindow.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
QString normalizeLang(const QString& lang) {
    const QString l = lang.trimmed().toLower();
    if (l.startsWith(QStringLiteral("en"))) {
        return QStringLiteral("en");
    }
    if (l.startsWith(QStringLiteral("zh"))) {
        return QStringLiteral("zh");
    }
    return QStringLiteral("es");
}

QString readUtf8File(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}
} // namespace

QString MainWindow::loadHelpTopicMarkdown(const QString& topicId) const {
    const QString lang = normalizeLang(m_language);
    QStringList langCandidates;
    langCandidates << lang;
    if (lang != QStringLiteral("en")) {
        langCandidates << QStringLiteral("en");
    }
    if (lang != QStringLiteral("es")) {
        langCandidates << QStringLiteral("es");
    }

    QStringList paths;
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString& l : langCandidates) {
        paths << QDir(appDir).filePath(QStringLiteral("help/%1/%2.md").arg(l, topicId));
#ifdef Q_OS_MAC
        QDir d(appDir);
        if (d.dirName() == QStringLiteral("MacOS")) {
            d.cdUp();
            d.cdUp();
            d.cd(QStringLiteral("Resources"));
            paths << d.filePath(QStringLiteral("help/%1/%2.md").arg(l, topicId));
        }
#endif
        paths << QStringLiteral(":/help/%1/%2.md").arg(l, topicId);
    }

    for (const QString& p : paths) {
        if (p.startsWith(QStringLiteral(":/"))) {
            const QString md = readUtf8File(p);
            if (!md.trimmed().isEmpty()) {
                return md;
            }
            continue;
        }
        if (QFile::exists(p)) {
            const QString md = readUtf8File(p);
            if (!md.trimmed().isEmpty()) {
                return md;
            }
        }
    }
    return QString();
}

void MainWindow::openHelpTopic(const QString& topicId, const QString& titleOverride) {
    const QString title = titleOverride.trimmed().isEmpty()
                              ? trk(QStringLiteral("t_help_title_001"),
                                    QStringLiteral("Ayuda"),
                                    QStringLiteral("Help"),
                                    QStringLiteral("帮助"))
                              : titleOverride;
    QString md = loadHelpTopicMarkdown(topicId);
    if (md.trimmed().isEmpty()) {
        md = trk(QStringLiteral("t_help_missing_001"),
                 QStringLiteral("# Ayuda no disponible\n\nNo se encontró contenido para este tema."),
                 QStringLiteral("# Help not available\n\nNo content was found for this topic."),
                 QStringLiteral("# 帮助不可用\n\n未找到该主题内容。"));
    }

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(title);
    dlg.resize(900, 620);

    auto* root = new QVBoxLayout(&dlg);
    auto* browser = new QTextBrowser(&dlg);
    browser->setOpenExternalLinks(true);
    browser->setReadOnly(true);
    browser->setMarkdown(md);
    root->addWidget(browser, 1);

    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* closeBtn = buttons->addButton(
        trk(QStringLiteral("t_close_btn_001"), QStringLiteral("Cerrar"), QStringLiteral("Close"), QStringLiteral("关闭")),
        QDialogButtonBox::AcceptRole);
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    root->addWidget(buttons);

    dlg.exec();
}
