#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

// The "discord." local control family (WS13; docs/native/DESKTOP_PARITY.md
// section 19): the desktop's Discord bridge. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlDiscord.cpp. Gate 2 creates it empty; WS13 owns it.
namespace forkmesh::localcontrol::discord {

// The method prefix this family answers.
QString familyPrefix();

constexpr int kMaxChannels = 300;
constexpr int kMaxMessages = 100;
constexpr int kDefaultMessages = 50;
constexpr int kMaxRowTextChars = 4000;
constexpr int kMaxSendChars = 2000;
constexpr int kMaxNameChars = 120;
constexpr int kMaxTopicChars = 240;
constexpr int kMaxReactions = 24;
constexpr qint64 kBellDebounceMs = 1500;

bool validChannelId(const QString &id);
bool validReplyId(const QString &id);
QString conversationForChannelId(const QString &id);
int boundedLimit(int requested);
QString normalizedSendText(const QString &text);
QString sendTarget(const QString &channelId, const QString &text,
                   const QString &replyTo);
QString sendSummary(const QString &displayName, const QString &text,
                    bool reply);

struct ChannelFacts {
    QString id;
    QString display;
    QString group;
    QString shortName;
    QString topic;
    bool direct = false;
    bool guild = false;
    bool canSend = false;
};
QJsonObject channelRow(const ChannelFacts &facts);

struct MessageFacts {
    QString id;
    QString senderName;
    QString text;
    qint64 timestampMs = 0;
    bool self = false;
    QString threadRootId;
    QString fileName;
    QString fileMime;
    bool edited = false;
    QMap<QString, QStringList> reactions;
};
QJsonObject messageRow(const MessageFacts &facts);

} // namespace forkmesh::localcontrol::discord
