#include "ClientErrorReports.h"
#include "DesktopPingForward.h"
#include "DesktopPowerAlert.h"
#include "LocalControl.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PingBatch.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrlQuery>

namespace pingforward = forkmesh::pingforward;
namespace pingbatch = forkmesh::pingbatch;

namespace {

class PingForwardState : public QObject
{
public:
    explicit PingForwardState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshPingForwardState"));
    }

    pingforward::Gate gate;
    pingbatch::Batcher batch;
    pingforward::Ping lastHeld;
    QTimer *flushTimer = nullptr;
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
    source.runId = item.runId;
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
    if (!forkmesh::poweralert::isPowerKind(item.kind)) {
        pingforward::Screen screen;
        screen.active = isActiveWindow() && !isMinimized();
        screen.agentList = m_agentTable && m_agentTable->isVisible();
        screen.pingsPage = m_sectionStack && m_sectionStack->currentIndex() == 3;
        if (pingforward::seenOnScreen(screen, source))
            return {};
    }
    const pingforward::Ping ping = pingforward::pingFor(source);
    if (!pingforward::valid(ping))
        return {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    PingForwardState *state = pingForwardState(this);
    if (forkmesh::poweralert::isPowerKind(item.kind) || item.dismissOnly) {
        if (!state->gate.admit(ping, now) || !sendPingToPhones(ping))
            return {};
        return ping.pingId;
    }
    if (state->batch.due(now))
        flushPhonePingBatch();
    pingbatch::Item held;
    held.title = ping.title;
    held.body = ping.body;
    held.warning = ping.warning;
    held.atMs = now;
    switch (state->batch.offer(held, now)) {
    case pingbatch::Step::Show:
        if (!state->gate.admit(ping, now) || !sendPingToPhones(ping))
            return {};
        return ping.pingId;
    case pingbatch::Step::Summarize:
        state->lastHeld = ping;
        flushPhonePingBatch();
        return {};
    case pingbatch::Step::Hold:
        break;
    }
    state->lastHeld = ping;
    if (!state->flushTimer) {
        state->flushTimer = new QTimer(state);
        state->flushTimer->setSingleShot(true);
        connect(state->flushTimer, &QTimer::timeout, this,
                [this] { flushPhonePingBatch(); });
    }
    state->flushTimer->start(int(std::max<qint64>(0, state->batch.dueAtMs() - now)));
    return {};
}

void MainWindow::flushPhonePingBatch()
{
    PingForwardState *state = pingForwardState(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state->batch.held() == 0)
        return;
    if (state->flushTimer)
        state->flushTimer->stop();
    const pingbatch::Card card = state->batch.take();
    pingforward::Ping ping = state->lastHeld;
    if (card.summary) {
        ping.pingId = QStringLiteral("%1-batch").arg(now);
        ping.kind = QStringLiteral("desktop");
        ping.category.clear();
        ping.sessionId.clear();
        ping.runId.clear();
        ping.repo.clear();
        ping.warning = card.warning;
        ping.title = pingforward::fitText(card.title, pingforward::kTitleMax, true);
        ping.body = pingforward::fitText(card.body, pingforward::kBodyMax, false);
    }
    if (!pingforward::valid(ping) || !state->gate.admit(ping, now))
        return;
    sendPingToPhones(ping);
}

bool MainWindow::sendPingToPhones(const pingforward::Ping &ping)
{
    if (!m_networkAccess)
        return false;
    QNetworkRequest request;
    QByteArray payload;
    if (!preparePingForwardRequest(ping, &request, &payload))
        return false;
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
    return true;
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
