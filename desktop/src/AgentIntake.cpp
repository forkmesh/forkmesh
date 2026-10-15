#include "AgentIntake.h"
#include "LocalControlHandoff.h"
#include "ReleaseDraft.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace forkmesh::agentintake {

namespace {

bool fail(QString *error, const QString &code)
{
    if (error)
        *error = code;
    return false;
}

QString stringField(const QJsonObject &params, const QString &key, bool *wrongType)
{
    const QJsonValue value = params.value(key);
    if (value.isUndefined() || value.isNull())
        return QString();
    if (!value.isString()) {
        *wrongType = true;
        return QString();
    }
    return value.toString();
}

}

bool repositorySlugValid(const QString &slug, QString *owner, QString *name)
{
    static const QRegularExpression part(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    const QStringList parts = slug.trimmed().split(QLatin1Char('/'));
    if (parts.size() != 2)
        return false;
    for (const QString &p : parts)
        if (!part.match(p).hasMatch() || p.contains(QLatin1String("..")))
            return false;
    if (owner)
        *owner = parts.at(0);
    if (name)
        *name = parts.at(1);
    return true;
}

bool modelIdValid(const QString &model)
{
    static const QRegularExpression shape(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/@\\[\\]-]{0,99}$"));
    return model.size() <= kMaxModelChars && shape.match(model).hasMatch();
}

bool providerIdValid(const QString &provider)
{
    static const QRegularExpression shape(QStringLiteral("^[a-z0-9][a-z0-9-]{0,39}$"));
    return provider.size() <= kMaxProviderChars && shape.match(provider).hasMatch();
}

bool worktreePathValid(const QString &path)
{
    if (path.isEmpty() || path.size() > kMaxPathChars || path.contains(QChar(0)))
        return false;
    if (!QDir::isAbsolutePath(path))
        return false;
    return QDir::cleanPath(path) == path;
}

bool taskIdValid(const QString &taskId)
{
    static const QRegularExpression shape(QStringLiteral("^[a-f0-9]{32}$"));
    return shape.match(taskId).hasMatch();
}

bool parseCreateRequest(const QJsonObject &params, CreateRequest *out,
                        QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("bad_request"));
    bool wrongType = false;
    const QString repository =
        stringField(params, QStringLiteral("repository"), &wrongType);
    const QString prompt =
        stringField(params, QStringLiteral("prompt"), &wrongType).trimmed();
    const QString title =
        stringField(params, QStringLiteral("title"), &wrongType).simplified();
    const QString branch =
        stringField(params, QStringLiteral("branch"), &wrongType).trimmed();
    const QString worktree =
        stringField(params, QStringLiteral("worktree"), &wrongType).trimmed();
    const QString provider =
        stringField(params, QStringLiteral("provider"), &wrongType).trimmed();
    const QString model =
        stringField(params, QStringLiteral("model"), &wrongType).trimmed();
    const QString task =
        stringField(params, QStringLiteral("task"), &wrongType).trimmed().toLower();
    if (wrongType)
        return fail(error, QStringLiteral("bad_request"));

    CreateRequest request;
    if (!repositorySlugValid(repository, &request.owner, &request.name))
        return fail(error, QStringLiteral("bad_repository"));
    if (prompt.isEmpty() || prompt.size() > kMaxPromptChars)
        return fail(error, QStringLiteral("bad_task"));
    if (title.size() > kMaxTitleChars)
        return fail(error, QStringLiteral("bad_title"));
    if (!branch.isEmpty() &&
        !forkmesh::localcontrol::handoffRefNameValid(branch))
        return fail(error, QStringLiteral("bad_branch"));
    if (!worktree.isEmpty() && !worktreePathValid(worktree))
        return fail(error, QStringLiteral("bad_worktree"));
    if (!provider.isEmpty() && !providerIdValid(provider))
        return fail(error, QStringLiteral("bad_provider"));
    if (!model.isEmpty() && !modelIdValid(model))
        return fail(error, QStringLiteral("bad_model"));
    if (!task.isEmpty() && !taskIdValid(task))
        return fail(error, QStringLiteral("bad_task_id"));

    const QJsonValue handoff = params.value(QStringLiteral("handoff"));
    if (!handoff.isUndefined() && !handoff.isNull() && !handoff.isBool())
        return fail(error, QStringLiteral("bad_request"));
    const QJsonValue paused = params.value(QStringLiteral("paused"));
    if (!paused.isUndefined() && !paused.isNull() && !paused.isBool())
        return fail(error, QStringLiteral("bad_request"));
    const QJsonValue priority = params.value(QStringLiteral("priority"));
    if (!priority.isUndefined() && !priority.isNull()) {
        if (!priority.isDouble())
            return fail(error, QStringLiteral("bad_priority"));
        const double value = priority.toDouble();
        if (value != double(qint64(value)) || value < 1 || value > 99)
            return fail(error, QStringLiteral("bad_priority"));
        request.hasPriority = true;
        request.priority = int(value);
    }

    request.prompt = prompt;
    request.title = title;
    request.branch = branch;
    request.worktree = worktree;
    request.provider = provider;
    request.model = model;
    request.task = task;
    request.handoff = handoff.toBool(false);
    request.paused = paused.toBool(false);
    *out = request;
    return true;
}

bool releaseTagValid(const QString &tag)
{
    return forkmesh::releasedraft::tagNameValid(tag);
}

bool parseReleaseRequest(const QString &method, const QJsonObject &params,
                         ReleaseRequest *out, QString *error)
{
    if (!out)
        return fail(error, QStringLiteral("bad_request"));
    if (method != ReleaseListMethod && method != ReleaseCreateMethod &&
        method != ReleasePublishMethod)
        return fail(error, QStringLiteral("unknown_method"));
    bool wrongType = false;
    const QString repository =
        stringField(params, QStringLiteral("repository"), &wrongType).trimmed();
    const QString tag = stringField(params, QStringLiteral("tag"), &wrongType).trimmed();
    const QString target =
        stringField(params, QStringLiteral("target"), &wrongType).trimmed();
    const QString title =
        stringField(params, QStringLiteral("title"), &wrongType).simplified();
    const QString notes = stringField(params, QStringLiteral("notes"), &wrongType).trimmed();
    if (wrongType)
        return fail(error, QStringLiteral("bad_request"));

    ReleaseRequest request;
    request.method = method;
    if (!repositorySlugValid(repository, &request.owner, &request.name))
        return fail(error, QStringLiteral("bad_repository"));
    if (!tag.isEmpty() && !releaseTagValid(tag))
        return fail(error, QStringLiteral("bad_tag"));
    if (method == ReleasePublishMethod && tag.isEmpty())
        return fail(error, QStringLiteral("bad_tag"));
    if (!target.isEmpty() && !forkmesh::localcontrol::handoffRefNameValid(target))
        return fail(error, QStringLiteral("bad_target"));
    if (title.size() > kMaxReleaseTitleChars)
        return fail(error, QStringLiteral("bad_title"));
    if (notes.size() > kMaxReleaseNotesChars)
        return fail(error, QStringLiteral("bad_notes"));

    const QJsonValue rollout = params.value(QStringLiteral("rollout"));
    if (!rollout.isUndefined() && !rollout.isNull()) {
        if (!rollout.isBool())
            return fail(error, QStringLiteral("bad_request"));
        request.hasRollout = true;
        request.rollout = rollout.toBool();
    }
    const QJsonValue count = params.value(QStringLiteral("count"));
    if (!count.isUndefined() && !count.isNull()) {
        if (!count.isDouble())
            return fail(error, QStringLiteral("bad_count"));
        const double value = count.toDouble();
        if (value != double(qint64(value)) || value < 1 || value > kMaxReleaseListCount)
            return fail(error, QStringLiteral("bad_count"));
        request.count = int(value);
    }

    request.tag = tag;
    request.target = target;
    request.title = title;
    request.notes = notes;
    *out = request;
    return true;
}

QString mintToken(int sessionId)
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / 4);
    return QStringLiteral("%1.%2").arg(sessionId).arg(QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
}

int tokenSessionId(const QString &token)
{
    const int dot = token.indexOf(QLatin1Char('.'));
    if (dot <= 0 || dot > 10 || token.size() > 128)
        return -1;
    bool ok = false;
    const int id = token.left(dot).toInt(&ok);
    return ok && id > 0 ? id : -1;
}

bool tokenMatches(const QString &expected, const QString &presented)
{
    const QByteArray a = expected.toUtf8();
    const QByteArray b = presented.toUtf8();
    if (a.isEmpty() || a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

int authorizedSession(const QHash<int, QString> &tokens, const QString &presented)
{
    const int session = tokenSessionId(presented);
    if (session <= 0)
        return -1;
    return tokenMatches(tokens.value(session), presented) ? session : -1;
}

int RateLimiter::used(qint64 nowMs)
{
    while (!m_times.isEmpty() && nowMs - m_times.first() >= m_windowMs)
        m_times.removeFirst();
    return int(m_times.size());
}

bool RateLimiter::allow(qint64 nowMs)
{
    if (used(nowMs) >= m_count)
        return false;
    m_times.append(nowMs);
    return true;
}

QJsonObject errorReply(const QString &code, const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), code},
                       {QStringLiteral("message"), message}};
}

QJsonObject unauthorizedReply(const QString &method)
{
    QString action = QStringLiteral("create agents");
    if (method.startsWith(ReleaseMethodPrefix))
        action = QStringLiteral("manage its releases");
    else if (method.startsWith(WindowMethodPrefix))
        action = QStringLiteral("open or read its windows");
    return errorReply(QStringLiteral("unauthorized"),
                      QStringLiteral("Only agents this ForkMesh desktop launched "
                                     "can %1.")
                          .arg(action));
}

QString socketDirectory()
{
    const QString runtime =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty() && QFileInfo(runtime).isDir())
        return runtime;
    return QDir::tempPath();
}

Server::Server()
    : m_server(new QLocalServer)
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(m_server, &QLocalServer::newConnection, m_server,
                     [this] { accept(); });
}

Server::~Server()
{
    delete m_server;
}

bool Server::listen(QString *error)
{
    if (m_server->isListening())
        return true;
    const QString name = QDir(socketDirectory())
                             .filePath(QStringLiteral("forkmesh-agent-intake-%1-%2")
                                           .arg(QCoreApplication::applicationPid())
                                           .arg(QRandomGenerator::system()->generate64(),
                                                16, 16, QLatin1Char('0')));
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

QString Server::socketPath() const
{
    return m_server->fullServerName();
}

void Server::accept()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        socket->setParent(m_server);
        QPointer<QLocalSocket> guard(socket);
        QObject::connect(socket, &QLocalSocket::readyRead, m_server,
                         [this, socket] { readRequest(socket); });
        QObject::connect(socket, &QLocalSocket::disconnected, socket,
                         &QObject::deleteLater);
        QTimer::singleShot(kRequestTimeoutMs, m_server, [guard] {
            if (guard && !guard->property("agentIntakeHandled").toBool())
                guard->abort();
        });
    }
}

void Server::readRequest(QLocalSocket *socket)
{
    if (socket->property("agentIntakeHandled").toBool())
        return;
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > kMaxRequestBytes)
            socket->abort();
        return;
    }
    socket->setProperty("agentIntakeHandled", true);
    QPointer<QLocalSocket> guard(socket);
    const QByteArray line = socket->readLine(kMaxRequestBytes + 2);
    QJsonObject reply;
    if (line.size() > kMaxRequestBytes) {
        reply = errorReply(QStringLiteral("too_large"),
                           QStringLiteral("The request is larger than 64 KiB."));
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            reply = errorReply(QStringLiteral("bad_request"),
                               QStringLiteral("The request is not a JSON object."));
        } else {
            const QJsonObject object = doc.object();
            Request request;
            request.token = object.value(QStringLiteral("token")).toString().left(128);
            request.method = object.value(QStringLiteral("method")).toString().left(64);
            request.params = object.value(QStringLiteral("params")).toObject();
            if (m_asyncHandler) {
                auto replied = std::make_shared<bool>(false);
                m_asyncHandler(request, [guard, replied](const QJsonObject &answer) {
                    if (*replied)
                        return;
                    *replied = true;
                    writeReply(guard, answer);
                });
                return;
            }
            reply = m_handler ? m_handler(request)
                              : errorReply(QStringLiteral("unavailable"),
                                           QStringLiteral("The desktop is not taking "
                                                          "agent requests."));
        }
    }
    writeReply(guard, reply);
}

void Server::writeReply(QLocalSocket *socket, const QJsonObject &reply)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
    socket->disconnectFromServer();
}

}
