#include "connectioncapabilities.h"

#include <QtTest/QtTest>

using namespace zfsmgr::caps;

namespace {
Platform unixReady() {
    Platform p;
    p.isWindows = false;
    p.daemonActive = true;
    p.daemonApiOk = true;
    return p;
}
Platform windowsReady() {
    Platform p = unixReady();
    p.isWindows = true;
    return p;
}
} // namespace

class CapabilitiesTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Lo que el agente de Windows NO sirve. Comprobado por RPC contra un Windows 11
    // real: responde "unknown command". Si alguien porta uno de estos verbos y olvida
    // actualizar la tabla, este test lo caza.
    void windowsReportsPendingFeaturesAsUnavailable() {
        const Feature pending[] = {
            Feature::BackgroundJobs,
            Feature::RepairAltMountpoints,
            Feature::DirToDir,
            Feature::ToolAvailability,
            Feature::DirBreakdown,
            Feature::DirAssemble,
            Feature::RsyncSync,
            Feature::SendRecvStreaming,
        };
        for (const Feature f : pending) {
            const Availability a = featureAvailability(f, windowsReady());
            QVERIFY(!a.available);
            QCOMPARE(a.reason, Reason::WindowsAgentPending);
        }
    }

    // Distinguir "aún no implementado" de "no aplica" no es cosmético: al usuario le
    // dice si esperar una versión futura o no esperarla.
    void windowsSeparatesNotApplicableFromPending() {
        QCOMPARE(featureAvailability(Feature::HelperCommandInstall, windowsReady()).reason,
                 Reason::WindowsNotApplicable);
        QCOMPARE(featureAvailability(Feature::AlternateMount, windowsReady()).reason,
                 Reason::WindowsNotApplicable);
        QCOMPARE(featureAvailability(Feature::AutoSnapshotsGsa, windowsReady()).reason,
                 Reason::WindowsNotApplicable);
        QCOMPARE(featureAvailability(Feature::ShellActions, windowsReady()).reason,
                 Reason::WindowsNeedsUnixShell);
    }

    // El daemon de Windows SÍ sirve --dump-zfs-allow: los permisos estaban
    // deshabilitados por plataforma, no por falta de soporte.
    void windowsSupportsDatasetPermissions() {
        const Availability a = featureAvailability(Feature::DatasetPermissions, windowsReady());
        QVERIFY(a.available);
        QCOMPARE(a.reason, Reason::Available);
    }

    void unixKeepsEverythingAvailableWhenDaemonIsReady() {
        const Feature all[] = {
            Feature::DatasetPermissions, Feature::AutoSnapshotsGsa, Feature::BackgroundJobs,
            Feature::AlternateMount,     Feature::RepairAltMountpoints, Feature::DirBreakdown,
            Feature::DirAssemble,        Feature::DirToDir,          Feature::SendRecvStreaming,
            Feature::RsyncSync,          Feature::ShellActions,      Feature::HelperCommandInstall,
            Feature::ToolAvailability,
        };
        for (const Feature f : all) {
            QVERIFY2(featureAvailability(f, unixReady()).available, "Unix no debe perder nada");
        }
    }

    // Sin daemon, lo que depende de él no está: y el motivo debe decir eso, no
    // confundirse con una limitación de plataforma.
    void featuresNeedingDaemonReportWhyWhenItIsDown() {
        Platform p = unixReady();
        p.daemonActive = false;
        const Availability a = featureAvailability(Feature::DatasetPermissions, p);
        QVERIFY(!a.available);
        QCOMPARE(a.reason, Reason::DaemonNotReady);

        Platform mismatch = unixReady();
        mismatch.daemonApiOk = false;
        QCOMPARE(featureAvailability(Feature::DatasetPermissions, mismatch).reason,
                 Reason::DaemonApiMismatch);

        // Lo que no depende del daemon no debe caerse con él.
        QVERIFY(featureAvailability(Feature::ShellActions, p).available);
    }

    // Cuando el agente publique sus capacidades, su respuesta manda sobre la tabla:
    // la tabla es una suposición y se desincroniza en cuanto se porte un verbo.
    void declaredAgentCapabilitiesOverrideTheStaticTable() {
        Platform p = windowsReady();
        p.daemonCaps.insert(featureAgentVerb(Feature::DirToDir));
        const Availability a = featureAvailability(Feature::DirToDir, p);
        QVERIFY2(a.available, "si el agente lo declara, está disponible aunque la tabla diga que no");

        // Y al revés: un verbo ausente de lo declarado no está, aunque la tabla calle.
        Platform q = windowsReady();
        q.daemonCaps.insert(QStringLiteral("--algo-distinto"));
        QVERIFY(!featureAvailability(Feature::DatasetPermissions, q).available);
    }
};

QTEST_MAIN(CapabilitiesTest)
#include "capabilities_test.moc"
