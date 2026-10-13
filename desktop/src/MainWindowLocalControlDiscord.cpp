// The "discord." local control family (WS13): the desktop's Discord bridge.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.

#include "DiscordClient.h"
#include "LocalControl.h"
#include "LocalControlDiscord.h"
#include "LocalControlHosts.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

namespace discordctl = forkmesh::localcontrol::discord;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

class DiscordControlState : public QObject
{
public:
    explicit DiscordControlState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshDiscordControlState"));
        bell.setSingleShot(true);
        bell.setInterval(int(discordctl::kBellDebounceMs));
    }

    hostctl::ConfirmationStore confirmations;
    QPointer<DiscordClient> watched;
    QTimer bell;
    QList<QMetaObject::Connection> links;
};

DiscordControlState *discordControlState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(
        QStringLiteral("forkmeshDiscordControlState"), Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<DiscordControlState *>(found))
        return state;
    return new DiscordControlState(window);
}

QString discordConfirmationError(hostctl::ConfirmationStore::Result result)
{
    switch (result) {
    case hostctl::ConfirmationStore::Result::Ok:
        return {};
    case hostctl::ConfirmationStore::Result::Expired:
        return QStringLiteral("confirmation_expired");
    case hostctl::ConfirmationStore::Result::Mismatch:
        return QStringLiteral("confirmation_mismatch");
    case hostctl::ConfirmationStore::Result::Missing:
        break;
    }
    return QStringLiteral("confirmation_required");
}

} // namespace

void MainWindow::handleLocalControlDiscordRequest(int connectionId, int requestId,
                                                  const QString &method,
                                                  const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    if (device.key.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("not_owner"));
        return;
    }
    DiscordControlState *state = discordControlState(this);
    DiscordClient *client = m_discordPackageActive ? m_discordClient : nullptr;

    if (client && state->watched != client) {
        for (const QMetaObject::Connection &link : std::as_const(state->links))
            QObject::disconnect(link);
        state->links.clear();
        state->watched = client;
        QTimer *bell = &state->bell;
        state->links.append(connect(client, &DiscordClient::messageArrived, state,
                                    [bell](const ChatMessage &) {
                                        if (!bell->isActive())
                                            bell->start();
                                    }));
        state->links.append(connect(client, &DiscordClient::conversationsChanged, state,
                                    [bell](const QStringList &) {
                                        if (!bell->isActive())
                                            bell->start();
                                    }));
        QPointer<LocalControlServer> guard(server);
        state->links.append(connect(&state->bell, &QTimer::timeout, state, [guard] {
            if (guard)
                guard->broadcast(QStringLiteral("discord.changed"));
        }));
    }

    if (method == QLatin1String("discord.status")) {
        server->respond(connectionId, requestId,
                        {{QStringLiteral("available"), client != nullptr},
                         {QStringLiteral("configured"), client && client->isConfigured()},
                         {QStringLiteral("connected"), client && client->isConnected()},
                         {QStringLiteral("bot"), client ? client->botName().left(discordctl::kMaxNameChars)
                                                        : QString()}});
        return;
    }
    if (!client) {
        server->respondError(connectionId, requestId, QStringLiteral("discord_unavailable"));
        return;
    }

    if (method == QLatin1String("discord.list")) {
        QJsonArray rows;
        for (const QString &conversation : client->conversations()) {
            if (rows.size() >= discordctl::kMaxChannels)
                break;
            discordctl::ChannelFacts facts;
            facts.id = forkmesh::discord::channelIdFromConversation(conversation);
            if (!discordctl::validChannelId(facts.id))
                continue;
            facts.display = client->displayName(conversation);
            facts.group = client->groupName(conversation);
            facts.shortName = client->shortName(conversation);
            facts.topic = client->topic(conversation);
            facts.guild = client->isGuildConversation(conversation);
            facts.direct = !facts.guild;
            facts.canSend = client->canSend(conversation);
            rows.append(discordctl::channelRow(facts));
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("channels"), rows},
                         {QStringLiteral("connected"), client->isConnected()}});
        return;
    }

    if (method != QLatin1String("discord.messages") && method != QLatin1String("discord.prepare") &&
        method != QLatin1String("discord.send")) {
        server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
        return;
    }
    const QString channelId = params.value(QStringLiteral("channel")).toString();
    const QString conversation = discordctl::conversationForChannelId(channelId);
    if (conversation.isEmpty() || !client->conversations().contains(conversation)) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_channel"));
        return;
    }

    if (method == QLatin1String("discord.messages")) {
        client->openConversation(conversation);
        const int limit = discordctl::boundedLimit(params.value(QStringLiteral("limit")).toInt());
        const QList<ChatMessage> history = m_history.value(conversation);
        QJsonArray rows;
        for (int i = qMax(0, int(history.size()) - limit); i < history.size(); ++i) {
            const ChatMessage &message = history.at(i);
            if (message.deleted)
                continue;
            discordctl::MessageFacts facts;
            facts.id = message.id;
            facts.senderName = message.senderName;
            facts.text = message.text;
            facts.timestampMs = message.timestampMs;
            facts.self = message.self;
            facts.threadRootId = message.threadRootId;
            facts.fileName = message.fileName;
            facts.fileMime = message.fileMime;
            facts.edited = message.edited;
            facts.reactions = m_reactions.value(message.id);
            rows.append(discordctl::messageRow(facts));
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("channel"), channelId},
                         {QStringLiteral("display"),
                          client->displayName(conversation).left(discordctl::kMaxNameChars)},
                         {QStringLiteral("canSend"), client->canSend(conversation)},
                         {QStringLiteral("messages"), rows}});
        return;
    }

    const QString text = discordctl::normalizedSendText(params.value(QStringLiteral("text")).toString());
    const QString replyTo = params.value(QStringLiteral("replyTo")).toString();
    if (method == QLatin1String("discord.prepare") || method == QLatin1String("discord.send")) {
        if (text.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_text"));
            return;
        }
        if (!discordctl::validReplyId(replyTo)) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_reply"));
            return;
        }
        if (!client->canSend(conversation)) {
            server->respondError(connectionId, requestId, QStringLiteral("not_writable"));
            return;
        }
    }
    const QString target = discordctl::sendTarget(channelId, text, replyTo);

    if (method == QLatin1String("discord.prepare")) {
        const QString token = state->confirmations.issue(
            device.key, QStringLiteral("send"), target, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("summary"),
                          discordctl::sendSummary(client->displayName(conversation), text,
                                                  !replyTo.isEmpty())},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("discord.send")) {
        const hostctl::ConfirmationStore::Result verdict = state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, QStringLiteral("send"),
            target, QDateTime::currentMSecsSinceEpoch());
        if (verdict != hostctl::ConfirmationStore::Result::Ok) {
            logSystem(QStringLiteral("Refused a Discord send to %1 from %2: %3.")
                          .arg(client->displayName(conversation),
                               localControlDeviceLabel(connectionId),
                               discordConfirmationError(verdict)));
            server->respondError(connectionId, requestId, discordConfirmationError(verdict));
            return;
        }
        const bool sent = replyTo.isEmpty() ? client->sendMessage(conversation, text)
                                            : client->sendReply(conversation, replyTo, text);
        if (!sent) {
            server->respondError(connectionId, requestId, QStringLiteral("send_failed"));
            return;
        }
        logSystem(QStringLiteral("Sent a Discord message to %1 from %2.")
                      .arg(client->displayName(conversation),
                           localControlDeviceLabel(connectionId)));
        server->respond(connectionId, requestId, {{QStringLiteral("ok"), true}});
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}
