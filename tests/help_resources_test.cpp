// Comprueba que TODO lo que la ayuda necesita está dentro del recurso: los temas en los
// dos idiomas, y las IMÁGENES que cada tema cita.
//
// Añadir un tema o una captura exige tocar el .qrc, y olvidarlo no rompe la compilación:
// solo hace que el menú abra «Ayuda no disponible», o que salga un hueco donde debería
// estar la captura. Eso es justo lo que no se ve revisando el código.
#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
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
    // Las imágenes se citan como `qrc:/help/img/...` y se cargan por `:/help/img/...`.
    const QRegularExpression rxImagen(QStringLiteral(R"(\]\((qrc:)?(/help/img/[^)\s]+)\))"));
    QSet<QString> imagenesVistas;
    for (const QString& l : {QStringLiteral("es"), QStringLiteral("en")}) {
        for (const QString& t : temas) {
            const QString p = QStringLiteral(":/help/%1/%2.md").arg(l, t);
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) {
                out << "FALTA " << p << " (no abre)\n";
                ++fallos;
                continue;
            }
            const QByteArray datos = f.readAll();
            if (datos.size() <= 200) {
                out << "FALTA " << p << " (" << datos.size() << " bytes)\n";
                ++fallos;
                continue;
            }
            const QString texto = QString::fromUtf8(datos);
            auto it = rxImagen.globalMatch(texto);
            while (it.hasNext()) {
                const QString ruta = QStringLiteral(":") + it.next().captured(2);
                if (imagenesVistas.contains(ruta)) {
                    continue;
                }
                imagenesVistas.insert(ruta);
                if (!QFile::exists(ruta)) {
                    out << "IMAGEN QUE NO ESTÁ EN EL RECURSO: " << ruta << " (citada en " << p
                        << ")\n";
                    ++fallos;
                }
            }
        }
    }
    if (imagenesVistas.isEmpty()) {
        out << "NINGUNA imagen citada: la ayuda perdió sus capturas\n";
        ++fallos;
    }
    out << (fallos ? QStringLiteral("FALLOS: %1\n").arg(fallos)
                   : QStringLiteral("%1 temas y %2 imágenes cargan\n")
                         .arg(temas.size() * 2)
                         .arg(imagenesVistas.size()));
    return fallos ? 1 : 0;
}
