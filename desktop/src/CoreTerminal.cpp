#include "CoreTerminal.h"

#include "CoreClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace forkmesh::coreterminal {

namespace {

QPointer<CoreClient> &clientSlot()
{
    static QPointer<CoreClient> client;
    return client;
}

}

void setClient(CoreClient *client)
{
    clientSlot() = client;
    TerminalBackend::Hooks &hooks = TerminalBackend::hooks();
    if (!client) {
        hooks = {};
        return;
    }
    hooks.start = [](QObject *parent, const TerminalBackend::Start &start,
                     TerminalBackend::Callbacks callbacks) -> TerminalBackend * {
        return CoreTerminalBackend::start(parent, start.commandLine, start.cwd, start.extraEnv,
                                          start.keepHostShell, start.cols, start.rows, start.persistKey,
                                          std::move(callbacks));
    };
    hooks.resume = [](QObject *parent, const QString &persistKey, int cols, int rows,
                      TerminalBackend::Callbacks callbacks) -> TerminalBackend * {
        return CoreTerminalBackend::resume(parent, persistKey, cols, rows, std::move(callbacks));
    };
    hooks.persisted = [](const QString &persistKey) { return CoreTerminalBackend::persistedTerminal(persistKey); };
}

CoreClient *client()
{
    return clientSlot().data();
}

bool available()
{
#ifdef Q_OS_WIN
    return false;
#else
    CoreClient *core = client();
    return core && core->isConnected() && core->hasNativeRoute(QStringLiteral("core.terminal.start"));
#endif
}

}

namespace {

constexpr int kMaxInputChunk = 12 * 1024;
constexpr int kRequestMs = 30000;

QSet<QString> &liveTerminals()
{
    static QSet<QString> ids;
    return ids;
}

QString settingsKey(const QString &persistKey)
{
    return QStringLiteral("core/terminals/") + persistKey;
}

QString deviceTag()
{
    return QStringLiteral("forkmesh-desktop:%1").arg(QCoreApplication::applicationPid());
}

QString modeFor(const QString &persistKey)
{
    return persistKey.isEmpty() ? QStringLiteral("desktop") : QStringLiteral("desktop:") + persistKey;
}

bool validVariableName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern.match(name).hasMatch();
}

void closeTerminal(CoreClient *client, const QString &id)
{
    if (client && client->isConnected())
        client->request(QStringLiteral("core.terminal.close"), {{QStringLiteral("terminal"), id}});
}

}

void forkmesh::coreterminal::sweepOrphans(CoreClient *core)
{
    if (!core || !core->isConnected() || !core->hasNativeRoute(QStringLiteral("core.terminal.list")))
        return;
    QPointer<CoreClient> guard(core);
    core->request(QStringLiteral("core.terminal.list"), {},
                  [guard](bool ok, const QJsonValue &result, const QString &) {
                      if (!ok || !guard)
                          return;
                      const QJsonArray terminals =
                          result.toObject().value(QStringLiteral("terminals")).toArray();
                      for (const QJsonValue &value : terminals) {
                          const QJsonObject terminal = value.toObject();
                          const QString id = terminal.value(QStringLiteral("terminal")).toString();
                          const QString mode = terminal.value(QStringLiteral("mode")).toString();
                          const QString device = terminal.value(QStringLiteral("device")).toString();
                          if (mode == QLatin1String("desktop")) {
                              if (device != deviceTag() || !liveTerminals().contains(id))
                                  closeTerminal(guard, id);
                          } else if (mode.startsWith(QLatin1String("desktop:"))) {
                              if (CoreTerminalBackend::persistedTerminal(mode.mid(8)) != id)
                                  closeTerminal(guard, id);
                          }
                      }
                  });
}

CoreTerminalBackend::CoreTerminalBackend(QObject *parent, CoreClient *client, const QString &id,
                                         const QString &persistKey, Callbacks callbacks)
    : TerminalBackend(parent), m_client(client), m_id(id), m_persistKey(persistKey), m_callbacks(std::move(callbacks))
{
    liveTerminals().insert(m_id);
}

CoreTerminalBackend::~CoreTerminalBackend()
{
    if (!m_keep)
        close();
    liveTerminals().remove(m_id);
}

QString CoreTerminalBackend::persistedTerminal(const QString &persistKey)
{
    if (persistKey.isEmpty())
        return QString();
    return QSettings().value(settingsKey(persistKey)).toString();
}

CoreTerminalBackend *CoreTerminalBackend::start(QObject *parent, const QString &commandLine, const QString &cwd,
                                                const QStringList &extraEnv, bool keepHostShell, int cols,
                                                int rows, const QString &persistKey, Callbacks callbacks)
{
    if (!forkmesh::coreterminal::available())
        return nullptr;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("TERM"));
    environment.remove(QStringLiteral("COLORTERM"));
    QStringList unset;
    for (const QString &kv : extraEnv) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0) {
            environment.insert(kv.left(eq), kv.mid(eq + 1));
        } else if (eq < 0 && !kv.isEmpty()) {
            if (!validVariableName(kv))
                return nullptr;
            environment.remove(kv);
            unset << kv;
        }
    }
    QJsonObject env;
    for (const QString &key : environment.keys())
        if (!key.isEmpty() && !key.contains(QLatin1Char('=')) && key != QLatin1String("TERM") &&
            key != QLatin1String("COLORTERM"))
            env.insert(key, environment.value(key));
    QString wrapped;
    if (!unset.isEmpty())
        wrapped = QStringLiteral("unset %1; ").arg(unset.join(QLatin1Char(' ')));
    wrapped += commandLine + QStringLiteral("; ec=$?; echo; echo \"[ForkMesh: process exited ($ec)]\"");
    wrapped += keepHostShell ? QStringLiteral("; exec bash -i") : QStringLiteral("; exit $ec");

    CoreClient *core = forkmesh::coreterminal::client();
    const QString id = QUuid::createUuid().toString(QUuid::Id128).toLower();
    auto *backend = new CoreTerminalBackend(parent, core, id, persistKey, std::move(callbacks));
    backend->m_cols = cols;
    backend->m_rows = rows;
    backend->m_sentCols = cols;
    backend->m_sentRows = rows;
    backend->listen();
    QJsonObject params{{QStringLiteral("terminal"), id},
                       {QStringLiteral("program"), QStringLiteral("bash")},
                       {QStringLiteral("args"), QJsonArray{QStringLiteral("-lc"), wrapped}},
                       {QStringLiteral("env"), env},
                       {QStringLiteral("cols"), cols},
                       {QStringLiteral("rows"), rows},
                       {QStringLiteral("attach"), true},
                       {QStringLiteral("mode"), modeFor(persistKey)},
                       {QStringLiteral("device"), deviceTag()}};
    if (!cwd.isEmpty())
        params.insert(QStringLiteral("cwd"), cwd);
    QPointer<CoreTerminalBackend> guard(backend);
    QPointer<CoreClient> client(core);
    core->request(QStringLiteral("core.terminal.start"), params,
                  [guard, client, id](bool ok, const QJsonValue &, const QString &error) {
                      if (!guard || guard->m_closed) {
                          if (ok)
                              closeTerminal(client, id);
                          return;
                      }
                      if (!ok)
                          guard->fail(error.isEmpty() ? QStringLiteral("core_error") : error);
                      else
                          guard->onStarted();
                  },
                  kRequestMs);
    return backend;
}

CoreTerminalBackend *CoreTerminalBackend::resume(QObject *parent, const QString &persistKey, int cols, int rows,
                                                 Callbacks callbacks)
{
    if (!forkmesh::coreterminal::available())
        return nullptr;
    const QString id = persistedTerminal(persistKey);
    if (id.isEmpty())
        return nullptr;
    CoreClient *core = forkmesh::coreterminal::client();
    auto *backend = new CoreTerminalBackend(parent, core, id, persistKey, std::move(callbacks));
    backend->m_cols = cols;
    backend->m_rows = rows;
    backend->m_sentCols = cols;
    backend->m_sentRows = rows;
    backend->m_resuming = true;
    backend->listen();
    QPointer<CoreTerminalBackend> guard(backend);
    core->request(QStringLiteral("core.terminal.attach"),
                  {{QStringLiteral("terminal"), id}, {QStringLiteral("cols"), cols}, {QStringLiteral("rows"), rows}},
                  [guard, client = QPointer<CoreClient>(core), id](bool ok, const QJsonValue &result,
                                                                    const QString &error) {
                      if (!guard || guard->m_closed) {
                          if (ok && guard)
                              closeTerminal(client, id);
                          return;
                      }
                      if (!ok) {
                          guard->fail(error.isEmpty() ? QStringLiteral("core_error") : error);
                          return;
                      }
                      const QByteArray replay = QByteArray::fromBase64(
                          result.toObject().value(QStringLiteral("replay")).toString().toLatin1());
                      const QByteArray early = std::exchange(guard->m_early, {});
                      guard->m_resuming = false;
                      if (guard->m_callbacks.output) {
                          if (!replay.isEmpty())
                              guard->m_callbacks.output(replay);
                          if (!early.isEmpty())
                              guard->m_callbacks.output(early);
                      }
                      guard->onStarted();
                  },
                  kRequestMs);
    return backend;
}

void CoreTerminalBackend::listen()
{
    if (!m_client)
        return;
    connect(m_client, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) { onEvent(event, data); });
    connect(m_client, &CoreClient::connectedChanged, this, [this](bool connected) {
        if (connected || !m_running)
            return;
        if (m_callbacks.output)
            m_callbacks.output(QByteArrayLiteral(
                "\r\n[ForkMesh: lost the background engine; this shell is no longer shown]\r\n"));
        finish(-1, m_persistKey.isEmpty());
    });
}

void CoreTerminalBackend::onEvent(const QString &event, const QJsonObject &data)
{
    if (!m_running || data.value(QStringLiteral("terminal")).toString() != m_id)
        return;
    if (event == QLatin1String("core.terminal.output")) {
        QByteArray bytes = QByteArray::fromBase64(data.value(QStringLiteral("data")).toString().toLatin1());
        if (data.value(QStringLiteral("reset")).toBool())
            bytes.prepend("\x1b" "c");
        if (m_resuming) {
            m_early += bytes;
            return;
        }
        if (m_callbacks.output && !bytes.isEmpty())
            m_callbacks.output(bytes);
    } else if (event == QLatin1String("core.terminal.exit")) {
        finish(data.value(QStringLiteral("code")).toInt(-1));
    }
}

void CoreTerminalBackend::onStarted()
{
    if (!m_running)
        return;
    m_attached = true;
    if (!m_persistKey.isEmpty())
        QSettings().setValue(settingsKey(m_persistKey), m_id);
    flushInput();
    sendResize();
    if (m_callbacks.started)
        m_callbacks.started();
}

void CoreTerminalBackend::finish(int code, bool forgetId)
{
    if (!m_running)
        return;
    m_running = false;
    m_closed = true;
    if (forgetId)
        forget();
    if (m_callbacks.exited)
        m_callbacks.exited(code);
}

void CoreTerminalBackend::fail(const QString &error)
{
    if (!m_running)
        return;
    m_running = false;
    m_closed = true;
    forget();
    if (m_callbacks.failed)
        m_callbacks.failed(error);
}

void CoreTerminalBackend::forget()
{
    if (m_persistKey.isEmpty())
        return;
    QSettings settings;
    if (settings.value(settingsKey(m_persistKey)).toString() == m_id)
        settings.remove(settingsKey(m_persistKey));
}

void CoreTerminalBackend::write(const QByteArray &bytes)
{
    if (!m_running || bytes.isEmpty())
        return;
    m_pendingInput += bytes;
    if (m_attached)
        flushInput();
}

void CoreTerminalBackend::flushInput()
{
    if (!m_client || m_pendingInput.isEmpty())
        return;
    const QByteArray pending = std::exchange(m_pendingInput, {});
    for (qsizetype at = 0; at < pending.size(); at += kMaxInputChunk)
        m_client->notify(QStringLiteral("core.terminal.input"),
                         {{QStringLiteral("terminal"), m_id},
                          {QStringLiteral("data"), QString::fromLatin1(pending.mid(at, kMaxInputChunk).toBase64())}});
}

void CoreTerminalBackend::resize(int cols, int rows)
{
    m_cols = cols;
    m_rows = rows;
    if (!m_attached || !m_running)
        return;
    if (!m_resizeTimer) {
        m_resizeTimer = new QTimer(this);
        m_resizeTimer->setSingleShot(true);
        m_resizeTimer->setInterval(0);
        connect(m_resizeTimer, &QTimer::timeout, this, &CoreTerminalBackend::sendResize);
    }
    m_resizeTimer->start();
}

void CoreTerminalBackend::sendResize()
{
    if (!m_client || !m_attached || !m_running || m_cols <= 0 || m_rows <= 0 ||
        (m_cols == m_sentCols && m_rows == m_sentRows))
        return;
    m_sentCols = m_cols;
    m_sentRows = m_rows;
    m_client->notify(QStringLiteral("core.terminal.resize"),
                     {{QStringLiteral("terminal"), m_id}, {QStringLiteral("cols"), m_cols},
                      {QStringLiteral("rows"), m_rows}});
}

void CoreTerminalBackend::close()
{
    if (m_closed)
        return;
    m_closed = true;
    const bool wasRunning = m_running;
    m_running = false;
    forget();
    if (wasRunning && m_attached)
        closeTerminal(m_client, m_id);
}

void CoreTerminalBackend::keepRunning()
{
    m_keep = true;
}
