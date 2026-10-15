#include "ClientErrorReports.h"
#include "CoreClient.h"
#include "DesktopPingForward.h"
#include "DesktopPowerAlert.h"
#include "LocalControl.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "NodeEventSocket.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>

namespace pingforward = forkmesh::pingforward;

namespace {

class PingForwardState : public QObject
{
public:
    explicit PingForwardState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshPingForwardState"));
    }

    pingforward::Gate gate;
};

PingForwardState *pingForwardState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(
        QStringLiteral("forkmeshPingForwardState"), Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<PingForwardState *>(found))
        return state;
    return new PingForwardState(window);
}

}

bool MainWindow::phoneLinkedForPings() const
{
    if (!m_localControl)
        return false;
    QList<pingforward::Phone> phones;
    for (const LocalControlServer::Device &device : m_localControl->pairedDevices())
        phones.append({device.user, device.platform, device.lastSeenMs,
                       m_localControl->isConnected(device.key)});
    return pingforward::phoneLinked(phones, accountOwner(),
                                    QDateTime::currentMSecsSinceEpoch());
}

QString MainWindow::forwardPingToPhones(const AppNotification &item)
{
    if (!m_networkAccess || !m_localControl)
        return {};
    if (!QSettings().value(pingforward::settingKey(), true).toBool())
        return {};
    if (!pingforward::forwardsKind(item.kind))
        return {};
    if (forkmesh::poweralert::isPowerKind(item.kind) &&
        !QSettings().value(forkmesh::poweralert::settingKey(), true).toBool())
        return {};
    if (forkmesh::ClientErrorReports::echoesRelayAlert(
            item.kind, item.title + QLatin1Char(' ') + item.body))
        return {};
    if (!phoneLinkedForPings())
        return {};
    pingforward::Source source;
    source.localId = item.id;
    source.timestampMs = item.timestampMs;
    source.node = machineNodeName();
    source.kind = item.kind;
    source.linkKind = item.link.kind;
    source.linkNumber = item.link.number;
    source.repo = item.repo;
    source.warning = item.warning;
    source.title = item.title;
    source.body = item.body;
    if (item.kind == forkmesh::ui::kAgentDonePingKind && item.link.kind == QLatin1String("agent") &&
        item.link.number > 0) {
        source.title = QStringLiteral("Agent #%1").arg(item.link.number);
        source.body.clear();
        if (const AgentSession *session = findAgentSession(item.link.number)) {
            source.body = session->issueTitle.trimmed();
            if (source.body.isEmpty())
                source.body = session->prompt.trimmed();
        }
    }
    const pingforward::Ping ping = pingforward::pingFor(source);
    if (!pingforward::valid(ping))
        return {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!pingForwardState(this)->gate.admit(ping, now))
        return {};
    QNetworkRequest request;
    QByteArray payload;
    if (!preparePingForwardRequest(ping, &request, &payload))
        return {};
    QNetworkReply *reply = m_networkAccess->post(request, payload);
    const QString title = ping.title.isEmpty() ? ping.body : ping.title;
    connect(reply, &QNetworkReply::finished, this, [this, reply, title] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
            return;
        logSystem(QStringLiteral("Couldn't send the ping \"%1\" to your phone (%2).")
                      .arg(title.left(80), status > 0 ? QString::number(status)
                                                       : reply->errorString()));
    });
    return ping.pingId;
}

void MainWindow::deleteForwardedPings(const QStringList &pingIds)
{
    if (!m_networkAccess || pingIds.isEmpty())
        return;
    const QString node = pingforward::nodeToken(machineNodeName());
    for (qsizetype start = 0; start < pingIds.size(); start += pingforward::kDeleteBatch) {
        const QStringList batch = pingIds.mid(start, pingforward::kDeleteBatch);
        QNetworkRequest request;
        if (!signPingForwardRequest(pingforward::deleteProof(),
                                    pingforward::deleteResource(node, batch), &request))
            return;
        QNetworkReply *reply = m_networkAccess->sendCustomRequest(
            request, QByteArrayLiteral("DELETE"),
            QJsonDocument(pingforward::deleteBody(node, batch))
                .toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
                return;
            logSystem(QStringLiteral("Couldn't delete these pings from your phone (%1).")
                          .arg(status > 0 ? QString::number(status)
                                          : reply->errorString()));
        });
    }
}

void MainWindow::announceDesktopOnline(qint64 nowMs)
{
    if (!QSettings().value(pingforward::onlineSettingKey(), true).toBool())
        return;
    if (m_nodeEventSocket && m_nodeEventSocket->coreOwned() && m_coreClient &&
        m_coreClient->isConnected() && m_coreClient->supports(pingforward::coreOnlineFeature()))
        return;
    const pingforward::Source source =
        pingforward::onlineSource(machineNodeName(), m_lastHeartbeatOkMs, nowMs);
    AppNotification item;
    item.title = source.title;
    item.body = source.body;
    item.timestampMs = source.timestampMs;
    item.kind = source.kind;
    forwardPingToPhones(item);
}

void MainWindow::wakeLocalMeshPhones(const QStringList &tokens)
{
    if (tokens.isEmpty() || !m_networkAccess)
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/push/mesh"));
    url.setQuery(QString());
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!m_profileIdentity.isValid())
        m_profileIdentity.load();
    QString user, ts, sig;
    if (signOrgTaskProof(QStringLiteral("forkmesh-mesh-wake-v1"),
                         tokens.join(QLatin1Char('\n')), &user, &ts, &sig)) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("user"), user);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    if (sessionToken.isEmpty() && !url.hasQuery())
        return;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(15000);
    if (!sessionToken.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    QJsonObject body;
    body.insert(QStringLiteral("tokens"), QJsonArray::fromStringList(tokens));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400)
            logSystem(QStringLiteral("Couldn't wake the #mesh phones (%1).").arg(status));
    });
}

bool MainWindow::preparePingForwardRequest(const pingforward::Ping &ping,
                                           QNetworkRequest *request, QByteArray *body)
{
    if (!request || !body || !pingforward::valid(ping))
        return false;
    if (!signPingForwardRequest(pingforward::proof(), pingforward::resource(ping), request))
        return false;
    *body = QJsonDocument(pingforward::body(ping)).toJson(QJsonDocument::Compact);
    return true;
}

bool MainWindow::signPingForwardRequest(const QString &proof, const QString &resource,
                                        QNetworkRequest *request)
{
    if (!request)
        return false;
    QUrl url = catalogApiUrl();
    url.setPath(pingforward::path());
    url.setQuery(QString());
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!m_profileIdentity.isValid())
        m_profileIdentity.load();
    QString user, ts, sig;
    if (signOrgTaskProof(proof, resource, &user, &ts, &sig)) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("user"), user);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    if (sessionToken.isEmpty() && !url.hasQuery())
        return false;
    *request = QNetworkRequest(url);
    request->setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json"));
    request->setTransferTimeout(15000);
    if (!sessionToken.isEmpty())
        request->setRawHeader("Authorization",
                              (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    return true;
}
