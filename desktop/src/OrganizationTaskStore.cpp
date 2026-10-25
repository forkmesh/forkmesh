#include "OrganizationTaskStore.h"

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "BackoffNetworkAccessManager.h"
#include "NetworkReplyError.h"
#include "SettingsWrite.h"
#include "TaskWorkItems.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>

using namespace forkmesh::ui;

namespace forkmesh::tasks {

namespace {

OrganizationTaskStore *g_current = nullptr;

QString taskErrorText(const QJsonObject &payload, const QString &fallback)
{
    QString error = payload.value(QStringLiteral("error")).toString().trimmed();
    if (error.isEmpty())
        return fallback;
    error.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString blocked = taskBlockedText(payload);
    if (!blocked.isEmpty())
        error += QStringLiteral(" (%1)").arg(blocked);
    return error;
}

QString taskRequestContextText(const QJsonObject &payload)
{
    const QJsonObject context =
        payload.value(QStringLiteral("request")).toObject();
    static const QList<QPair<QString, QString>> fields = {
        {QStringLiteral("user"), QStringLiteral("user")},
        {QStringLiteral("via"), QStringLiteral("via")},
        {QStringLiteral("node"), QStringLiteral("node")},
        {QStringLiteral("bot"), QStringLiteral("bot")},
        {QStringLiteral("agent"), QStringLiteral("agent")},
        {QStringLiteral("ip"), QStringLiteral("ip")},
        {QStringLiteral("userAgent"), QStringLiteral("user agent")},
    };
    QStringList shown;
    for (const auto &[key, label] : fields) {
        const QString value = context.value(key).toString().trimmed();
        if (!value.isEmpty())
            shown.append(label + QLatin1Char(' ') + value);
    }
    return shown.join(QStringLiteral(", "));
}

QString organizationTaskProof(const QByteArray &method, const QString &path,
                              const QJsonObject &body, QString *resource)
{
    static const QRegularExpression completeRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/complete/?$"));
    static const QRegularExpression undoRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/undo/?$"));
    static const QRegularExpression attachmentRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/attachments/"
                       "([a-f0-9]{32})/?$"));
    static const QRegularExpression itemRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/?$"));
    static const QRegularExpression agentStatusRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/agent-status/?$"));
    static const QRegularExpression pingRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/(remind|checkin)/?$"));
    static const QRegularExpression replyRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/replies/?$"));
    static const QRegularExpression historyRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/history/?$"));
    const bool collection = path == QLatin1String("/api/tasks") ||
                            path == QLatin1String("/api/tasks/");
    if (method == QByteArrayLiteral("GET"))
    {
        if (collection)
            return kOrgTaskListProof;
        const QRegularExpressionMatch attachment = attachmentRe.match(path);
        if (attachment.hasMatch()) {
            *resource = attachment.captured(1) + QLatin1Char('\n') +
                        attachment.captured(2);
            return kOrgTaskAttachmentProof;
        }
        const QRegularExpressionMatch history = historyRe.match(path);
        if (history.hasMatch()) {
            *resource = history.captured(1);
            return kOrgTaskHistoryProof;
        }
        return QString();
    }
    if (method == QByteArrayLiteral("DELETE")) {
        const QRegularExpressionMatch item = itemRe.match(path);
        if (!item.hasMatch())
            return QString();
        const QString digest = body.isEmpty()
                                   ? QString()
                                   : forkmesh::taskwork::updateDigest(body);
        if (!body.isEmpty() && digest.isEmpty())
            return QString();
        *resource = digest.isEmpty()
                        ? item.captured(1)
                        : item.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskDeleteProof;
    }
    if (method == QByteArrayLiteral("PATCH")) {
        const QRegularExpressionMatch item = itemRe.match(path);
        QStringList fields = body.keys();
        fields.removeAll(QStringLiteral("sessionToken"));
        const QString digest = forkmesh::taskwork::updateDigest(body);
        if (!item.hasMatch() || digest.isEmpty() ||
            fields == QStringList{QStringLiteral("priority")} ||
            fields == QStringList{QStringLiteral("progress")})
            return QString();
        *resource = item.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskUpdateProof;
    }
    if (method != QByteArrayLiteral("POST"))
        return QString();
    if (collection)
        return kOrgTaskOpenProof;
    const QRegularExpressionMatch complete = completeRe.match(path);
    if (complete.hasMatch()) {
        *resource = complete.captured(1);
        return kOrgTaskCompleteProof;
    }
    const QRegularExpressionMatch agentStatus = agentStatusRe.match(path);
    if (agentStatus.hasMatch()) {
        *resource = agentStatus.captured(1);
        return kOrgTaskAgentStatusProof;
    }
    const QRegularExpressionMatch reply = replyRe.match(path);
    if (reply.hasMatch()) {
        const QString digest = forkmesh::taskwork::updateDigest(body);
        if (digest.isEmpty())
            return QString();
        *resource = reply.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskReplyProof;
    }
    const QRegularExpressionMatch ping = pingRe.match(path);
    if (ping.hasMatch()) {
        const QString digest = body.isEmpty()
                                   ? QString()
                                   : forkmesh::taskwork::updateDigest(body);
        if (!body.isEmpty() && digest.isEmpty())
            return QString();
        *resource = digest.isEmpty()
                        ? ping.captured(1)
                        : ping.captured(1) + QLatin1Char('\n') + digest;
        return ping.captured(2) == QLatin1String("remind")
                   ? kOrgTaskRemindProof
                   : kOrgTaskCheckinProof;
    }
    const QRegularExpressionMatch undo = undoRe.match(path);
    if (!undo.hasMatch())
        return QString();
    *resource = undo.captured(1);
    return kOrgTaskUndoProof;
}

QStringList lowerNames(const QJsonArray &values)
{
    QStringList names;
    for (const QJsonValue &value : values) {
        const QString name = value.toString().trimmed().toLower();
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names;
}

}

QString taskText(const QJsonObject &task, const QString &field)
{
    return task.value(field).toString().trimmed();
}

bool taskIsClosed(const QJsonObject &task)
{
    return task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
           taskText(task, QStringLiteral("status")) == QLatin1String("done");
}

int openTaskCount(const QJsonArray &tasks)
{
    int open = 0;
    for (const QJsonValue &value : tasks) {
        if (!taskIsClosed(value.toObject()))
            ++open;
    }
    return open;
}

QString taskBlockedText(const QJsonObject &payload)
{
    QStringList reasons;
    for (const QJsonValue &value :
         payload.value(QStringLiteral("blockedBy")).toArray()) {
        const QString code = value.toString();
        if (code == QLatin1String("agent_running"))
            reasons.append(QStringLiteral("an agent job is running on it"));
        else if (code == QLatin1String("qa_in_progress"))
            reasons.append(QStringLiteral("it is waiting for QA review"));
        else if (!code.isEmpty())
            reasons.append(code);
    }
    return reasons.join(QStringLiteral(" and "));
}

QString taskImageMimeForPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".png")))
        return QStringLiteral("image/png");
    if (lower.endsWith(QStringLiteral(".jpg")) ||
        lower.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lower.endsWith(QStringLiteral(".gif")))
        return QStringLiteral("image/gif");
    if (lower.endsWith(QStringLiteral(".webp")))
        return QStringLiteral("image/webp");
    return {};
}

bool taskImageMimeAllowed(const QString &mime)
{
    return mime == QLatin1String("image/png") ||
           mime == QLatin1String("image/jpeg") ||
           mime == QLatin1String("image/gif") ||
           mime == QLatin1String("image/webp");
}

QByteArray taskImagePreviewBytes(const QString &path, QString *mime)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        return {};
    QByteArray encoded;
    for (const int side : {1200, 960, 720, 540}) {
        QImage shown = image;
        if (shown.width() > side || shown.height() > side)
            shown = shown.scaled(side, side, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        for (const int quality : {82, 72, 62, 52}) {
            encoded.clear();
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly))
                return {};
            QImageWriter writer(&buffer, QByteArrayLiteral("jpg"));
            writer.setQuality(quality);
            if (!writer.write(shown.convertToFormat(QImage::Format_RGB32)))
                return {};
            if (encoded.size() <= 256 * 1024)
                break;
        }
        if (encoded.size() <= 256 * 1024)
            break;
    }
    if (encoded.size() > 256 * 1024)
        return {};
    if (mime)
        *mime = QStringLiteral("image/jpeg");
    return encoded;
}

QJsonArray taskImageAttachments(const QStringList &paths, QString *error)
{
    constexpr qsizetype maxEach = 256 * 1024;
    constexpr qsizetype maxTotal = 1024 * 1024;
    if (paths.size() > 4) {
        if (error)
            *error = QStringLiteral(
                "A task can include up to four images.");
        return {};
    }
    QJsonArray attachments;
    qsizetype total = 0;
    for (const QString &path : paths) {
        if (forkmesh::ui::isVideoAttachmentPath(path)) {
            if (error)
                *error = QStringLiteral(
                    "Organization tasks accept image attachments only.");
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Could not read task image %1.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        QByteArray bytes = file.readAll();
        QString mime = taskImageMimeForPath(path);
        if (bytes.isEmpty() || QImage::fromData(bytes).isNull()) {
            if (error)
                *error = QStringLiteral("Could not decode task image %1.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        if (bytes.size() > maxEach || !taskImageMimeAllowed(mime)) {
            QString previewMime;
            bytes = taskImagePreviewBytes(path, &previewMime);
            mime = previewMime;
        }
        if (bytes.isEmpty() || bytes.size() > maxEach) {
            if (error)
                *error = QStringLiteral(
                    "Task image %1 is larger than 256 KiB after resizing.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        total += bytes.size();
        if (total > maxTotal) {
            if (error)
                *error = QStringLiteral(
                    "Task images must total 1 MiB or less.");
            return {};
        }
        QImage thumbnailImage = QImage::fromData(bytes);
        thumbnailImage = thumbnailImage.scaled(48, 48, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
        QByteArray thumbnailBytes;
        QBuffer thumbnailBuffer(&thumbnailBytes);
        thumbnailBuffer.open(QIODevice::WriteOnly);
        QImageWriter thumbnailWriter(&thumbnailBuffer, QByteArrayLiteral("jpg"));
        thumbnailWriter.setQuality(72);
        thumbnailWriter.write(thumbnailImage.convertToFormat(QImage::Format_RGB32));
        attachments.append(QJsonObject{
            {QStringLiteral("name"), QFileInfo(path).fileName()},
            {QStringLiteral("mime"), mime},
            {QStringLiteral("size"), bytes.size()},
            {QStringLiteral("file"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("thumbnail"),
             QStringLiteral("data:image/jpeg;base64,") +
                 QString::fromLatin1(thumbnailBytes.toBase64())},
        });
    }
    return attachments;
}

OrganizationTaskStore::OrganizationTaskStore(QObject *parent)
    : QObject(parent)
{
    m_badgeCount =
        QSettings().value(kOrganizationTaskOpenCountSetting, 0).toInt();
    g_current = this;
}

OrganizationTaskStore::~OrganizationTaskStore()
{
    if (g_current == this)
        g_current = nullptr;
}

OrganizationTaskStore *OrganizationTaskStore::current()
{
    return g_current;
}

void OrganizationTaskStore::setTransport(const TaskTransport &transport)
{
    m_transport = transport;
}

void OrganizationTaskStore::setCatalogHook(
    std::function<void(const QJsonArray &, bool)> hook)
{
    m_catalogHook = std::move(hook);
}

void OrganizationTaskStore::request(const QByteArray &method,
                                    const QString &path,
                                    const QJsonObject &body,
                                    TaskReplyHandler handler,
                                    const QList<int> &expectedHttpStatuses)
{
    QNetworkAccessManager *network = m_transport.network;
    if (!network || !m_transport.apiUrl) {
        handler(false, {}, QStringLiteral(
            "Sign in to an organization account to use private tasks."));
        return;
    }
    QUrl url = m_transport.apiUrl();
    const QUrl relative(path);
    url.setPath(relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    for (const int expected : expectedHttpStatuses)
        forkmesh::markExpectedHttpStatus(request, expected);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (const auto *backoff = qobject_cast<BackoffNetworkAccessManager *>(network))
        request.setHeader(
            QNetworkRequest::UserAgentHeader,
            QStringLiteral("ForkMeshDesktop/%1")
                .arg(backoff->clientVersion().isEmpty()
                         ? QStringLiteral("0")
                         : backoff->clientVersion()));
    bool keySigned = false;
    const QString sessionToken =
        m_transport.sessionToken ? m_transport.sessionToken(url) : QString();
    if (!sessionToken.isEmpty()) {
        request.setRawHeader(
            QByteArrayLiteral("Authorization"),
            QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    } else {
        QString resource;
        const QString proof = organizationTaskProof(
            method, relative.path(), body, &resource);
        if (proof.isEmpty() || !m_transport.authenticate ||
            !m_transport.authenticate(url, request, proof, resource)) {
            const bool authenticated = m_transport.accountAuthenticated &&
                                       m_transport.accountAuthenticated();
            handler(false, {},
                    authenticated
                        ? QString::fromUtf8(
                              "This change needs a password sign-in on this "
                              "device, not just its saved account key \xE2"
                              "\x80\x94 use Settings \xE2\x86\x92 \"Log in to "
                              "a user account\", then try again.")
                        : QStringLiteral(
                              "Sign in to an organization account to use "
                              "private tasks."));
            return;
        }
        keySigned = true;
    }
    const QByteArray payload =
        body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(
                                             QJsonDocument::Compact);
    if (!payload.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));

    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("GET"))
        reply = network->get(request);
    else if (method == QByteArrayLiteral("POST"))
        reply = network->post(request, payload);
    else
        reply = network->sendCustomRequest(request, method, payload);
    const QPointer<QNetworkAccessManager> guarded(network);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, guarded, method, path, keySigned, expectedHttpStatuses,
             handler = std::move(handler)]() mutable {
                const QByteArray raw = reply->readAll();
                const QJsonDocument document = QJsonDocument::fromJson(raw);
                QJsonObject data = document.object();
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                status >= 200 && status < 300 &&
                                data.value(QStringLiteral("ok")).toBool(true);
                if (!ok && (BackoffNetworkAccessManager::isBackoffSuppressed(reply)
                            || status == 429)) {
                    const auto *backoff =
                        qobject_cast<BackoffNetworkAccessManager *>(guarded.data());
                    data.insert(kTaskBackoffKey,
                                double(backoff ? backoff->hostCooldownRemainingMs(
                                                     reply->url().host())
                                               : 0));
                }
                QString error = ok
                    ? QString()
                    : taskErrorText(
                          data,
                          status > 0
                              ? QStringLiteral("HTTP %1").arg(status)
                              : reply->errorString());
                if (keySigned && status == 401)
                    error += QString::fromUtf8(
                        " \xE2\x80\x94 this machine's key is not authorized "
                        "for the account. Use Settings \xE2\x86\x92 \"Log in "
                        "to a user account\" to register it.");
                if (!ok && !expectedHttpStatuses.contains(status) &&
                    m_transport.log) {
                    const QString context = taskRequestContextText(data);
                    m_transport.log(
                        QStringLiteral("Organization tasks: %1 %2 failed: %3%4.")
                            .arg(QString::fromLatin1(method), path, error,
                                 context.isEmpty()
                                     ? QString()
                                     : QStringLiteral(" [%1]").arg(context)));
                }
                reply->deleteLater();
                handler(ok, data, error);
            });
}

void OrganizationTaskStore::requestAttachment(const QString &taskId,
                                              const QString &attachmentId,
                                              TaskAttachmentHandler handler)
{
    QNetworkAccessManager *network = m_transport.network;
    if (!network || !m_transport.apiUrl || taskId.isEmpty() ||
        attachmentId.isEmpty()) {
        handler(false, {}, {}, QStringLiteral("Task image unavailable"));
        return;
    }
    const QString path = QStringLiteral("/api/tasks/%1/attachments/%2")
                             .arg(taskId, attachmentId);
    QUrl url = m_transport.apiUrl();
    const QUrl relative(path);
    url.setPath(relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request;
    if (!m_transport.authenticate ||
        !m_transport.authenticate(url, request, kOrgTaskAttachmentProof,
                                  taskId + QLatin1Char('\n') + attachmentId)) {
        handler(false, {}, {}, QStringLiteral("Task image authentication failed"));
        return;
    }
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("image/*"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = network->get(request);
    if (!reply) {
        handler(false, {}, {}, QStringLiteral("Task image request failed"));
        return;
    }
    connect(reply, &QNetworkReply::finished, this,
            [reply, handler = std::move(handler)]() mutable {
                const QByteArray bytes = reply->readAll();
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                status >= 200 && status < 300 &&
                                !bytes.isEmpty();
                const QString mime = QString::fromLatin1(
                    reply->header(QNetworkRequest::ContentTypeHeader)
                        .toByteArray())
                    .section(QLatin1Char(';'), 0, 0)
                    .trimmed()
                    .toLower();
                const QString error = ok
                    ? QString()
                    : QStringLiteral("Task image request failed (%1)")
                          .arg(status > 0 ? QString::number(status)
                                          : reply->errorString());
                reply->deleteLater();
                handler(ok, bytes, mime, error);
            });
}

void OrganizationTaskStore::requestCatalog(TaskReplyHandler handler)
{
    requestCatalogPage(0, QJsonArray(), QJsonObject(), std::move(handler));
}

void OrganizationTaskStore::requestCatalogPage(int offset, QJsonArray catalog,
                                               QJsonObject firstPage,
                                               TaskReplyHandler handler)
{
    constexpr int kTaskCatalogLimit = 2000;
    const QString path = offset > 0
        ? QStringLiteral("/api/tasks?offset=%1").arg(offset)
        : QStringLiteral("/api/tasks");
    request(
        QByteArrayLiteral("GET"), path, {},
        [this, offset, catalog = std::move(catalog),
         firstPage = std::move(firstPage),
         handler = std::move(handler)](
            bool ok, const QJsonObject &payload,
            const QString &error) mutable {
            if (!ok) {
                handler(false, {}, error);
                return;
            }
            if (offset == 0)
                firstPage = payload;
            const QJsonArray page =
                payload.value(QStringLiteral("tasks")).toArray();
            for (const QJsonValue &task : page) {
                if (catalog.size() >= kTaskCatalogLimit)
                    break;
                catalog.append(task);
            }
            const int next =
                payload.value(QStringLiteral("nextOffset")).toInt(-1);
            if (payload.value(QStringLiteral("hasMore")).toBool() &&
                next > offset && catalog.size() < kTaskCatalogLimit) {
                requestCatalogPage(next, std::move(catalog),
                                   std::move(firstPage), std::move(handler));
                return;
            }
            const bool complete =
                !payload.value(QStringLiteral("hasMore")).toBool();
            firstPage.insert(QStringLiteral("tasks"), catalog);
            if (m_catalogHook)
                m_catalogHook(catalog, complete);
            handler(true, firstPage, QString());
        });
}

void OrganizationTaskStore::refresh()
{
    if (m_loading)
        return;
    setLoading(true);
    requestCatalog(
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            setLoading(false);
            const auto pushed = qScopeGuard([this] { drainPendingEvent(); });
            if (!ok) {
                emit catalogFailed(error);
                return;
            }
            applyPayload(payload);
        });
}

void OrganizationTaskStore::applyPayload(const QJsonObject &payload)
{
    m_payload = payload;
    m_loaded = true;
    m_loadedAt = QDateTime::currentMSecsSinceEpoch();
    setBadgeCount(openTaskCount(tasks()));
    emit catalogLoaded(payload);
}

void OrganizationTaskStore::refreshOnce()
{
    if (m_loading || m_loaded)
        return;
    refresh();
}

void OrganizationTaskStore::handleEvent()
{
    if (m_loading) {
        m_eventPending = true;
        return;
    }
    m_eventPending = false;
    refresh();
}

void OrganizationTaskStore::drainPendingEvent()
{
    if (!m_eventPending || m_loading)
        return;
    handleEvent();
}

void OrganizationTaskStore::reset()
{
    m_loaded = false;
    m_loadedAt = 0;
    m_payload = QJsonObject();
}

void OrganizationTaskStore::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

QJsonArray OrganizationTaskStore::tasks() const
{
    return m_payload.value(QStringLiteral("tasks")).toArray();
}

QString OrganizationTaskStore::actor() const
{
    return m_payload.value(QStringLiteral("actor")).toString().trimmed().toLower();
}

QStringList OrganizationTaskStore::members() const
{
    return lowerNames(m_payload.value(QStringLiteral("members")).toArray());
}

bool OrganizationTaskStore::canManage() const
{
    return m_payload.value(QStringLiteral("canManage")).toBool();
}

void OrganizationTaskStore::replaceTask(const QJsonObject &task)
{
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    QJsonArray list = tasks();
    for (int index = 0; index < list.size(); ++index) {
        if (taskText(list.at(index).toObject(), QStringLiteral("id")) != id)
            continue;
        list.replace(index, task);
        m_payload.insert(QStringLiteral("tasks"), list);
        emit taskChanged(task);
        return;
    }
}

void OrganizationTaskStore::updateTaskField(const QString &taskId,
                                            const QString &field,
                                            const QJsonValue &value)
{
    QJsonArray list = tasks();
    for (int index = 0; index < list.size(); ++index) {
        QJsonObject task = list.at(index).toObject();
        if (taskText(task, QStringLiteral("id")) != taskId)
            continue;
        task.insert(field, value);
        list.replace(index, task);
        m_payload.insert(QStringLiteral("tasks"), list);
        emit taskChanged(task);
        return;
    }
}

void OrganizationTaskStore::removeTask(const QString &taskId)
{
    QJsonArray list = tasks();
    for (int index = 0; index < list.size(); ++index) {
        if (taskText(list.at(index).toObject(), QStringLiteral("id")) != taskId)
            continue;
        list.removeAt(index);
        m_payload.insert(QStringLiteral("tasks"), list);
        return;
    }
}

void OrganizationTaskStore::setBadgeCount(int openCount)
{
    if (m_badgeCount == openCount)
        return;
    forkmesh::settings::put(QSettings(), kOrganizationTaskOpenCountSetting,
                            openCount);
    m_badgeCount = openCount;
}

}
