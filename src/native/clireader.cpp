#include "clireader.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <future>
#include <memory>

CliReader::CliReader(QObject* parent) : QObject(parent) {}

CliReader::~CliReader() {
    for (Worker& w : m_workers) {
        // The service lives in its thread: it is destroyed there, then the thread stops.
        QMetaObject::invokeMethod(w.service, [s = w.service]() { delete s; },
                                  Qt::BlockingQueuedConnection);
        w.thread->quit();
        w.thread->wait(5000);
        delete w.thread;
    }
}

void CliReader::configure(const QString& executable, const QStringList& arguments,
                          const QString& master) {
    m_executable = executable;
    m_arguments = arguments;
    m_master = master;
}

QStringList CliReader::linesForTest() const {
    QMutexLocker lock(&m_mutex);
    return m_lines;
}

CliReader::Worker& CliReader::workerFor(const QString& connection) {
    // Called with the mutex held.
    auto it = m_workers.find(connection);
    if (it != m_workers.end()) {
        return *it;
    }
    Worker w;
    w.thread = new QThread();
    w.thread->setObjectName(QStringLiteral("cli-read-%1").arg(connection));
    w.thread->start();
    w.service = new CliService();
    w.service->configure(m_executable, m_arguments, m_master);
    w.service->moveToThread(w.thread);
    return *m_workers.insert(connection, w);
}

CliService::Result CliReader::read(const QString& connection, const QString& line,
                                   int timeoutMs) {
    if (m_fake) {
        {
            QMutexLocker lock(&m_mutex);
            m_lines.push_back(line);
        }
        return m_fake(line);
    }
    CliService* service = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        m_lines.push_back(line);
        service = workerFor(connection.trimmed().toLower()).service;
    }
    auto promesa = std::make_shared<std::promise<CliService::Result>>();
    std::future<CliService::Result> futuro = promesa->get_future();
    QMetaObject::invokeMethod(service, [service, line, promesa]() {
        service->run(line, [promesa](const CliService::Result& r) { promesa->set_value(r); });
    });
    const auto plazo = std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 120000);
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        // The interface keeps painting while it waits.
        const auto hasta = std::chrono::steady_clock::now() + plazo;
        while (futuro.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (std::chrono::steady_clock::now() >= hasta) {
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            futuro.wait_for(std::chrono::milliseconds(5));
        }
    } else {
        futuro.wait_for(plazo);
    }
    if (futuro.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        CliService::Result r;
        r.rc = -1;
        r.err = QStringLiteral("timeout: %1").arg(line);
        return r;
    }
    return futuro.get();
}
