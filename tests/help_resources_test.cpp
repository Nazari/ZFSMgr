// Comprueba que TODOS los temas de ayuda están dentro del recurso y no vacíos, en los
// tres idiomas. Es la comprobación que faltaba: añadir un tema exige tocar el .qrc, y
// olvidarlo no rompe la compilación — solo hace que el menú abra «Ayuda no disponible».
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList temas = {
        "accion_clonar", "accion_enviar", "accion_desde_dir", "accion_desglosar",
        "accion_diff", "accion_ensamblar", "accion_hacia_dir", "accion_nivelar",
        "accion_sincronizar", "atajos_estados", "conexiones_windows",
        "configuracion_archivos", "linea_de_ordenes", "logs_aplicacion", "manual_rapido",
        "menus_contextuales", "propiedades_inline_columnas"};
    QTextStream out(stdout);
    int fallos = 0;
    for (const QString& l : {QStringLiteral("es"), QStringLiteral("en"), QStringLiteral("zh")}) {
        for (const QString& t : temas) {
            const QString p = QStringLiteral(":/help/%1/%2.md").arg(l, t);
            QFile f(p);
            const bool ok = f.open(QIODevice::ReadOnly) && f.size() > 200;
            if (!ok) { out << "FALTA " << p << " (" << f.size() << " bytes)\n"; ++fallos; }
        }
    }
    out << (fallos ? QStringLiteral("FALLOS: %1\n").arg(fallos) : QStringLiteral("los 51 temas cargan\n"));
    return fallos ? 1 : 0;
}
