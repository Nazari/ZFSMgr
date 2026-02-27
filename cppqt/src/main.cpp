#include "masterpassworddialog.h"
#include "mainwindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ZFSMgr"));
    QApplication::setApplicationName(QStringLiteral("ZFSMgr"));

    MasterPasswordDialog dlg;
    if (dlg.exec() != QDialog::Accepted) {
        return 0;
    }

    MainWindow w(dlg.password());
    w.show();
    return app.exec();
}
