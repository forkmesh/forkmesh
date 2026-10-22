#pragma once

#include <QJsonObject>
#include <QString>

namespace forkmesh::agenthub {

inline const QString SuggestMethod = QStringLiteral("sessions.suggest");
inline const QString ListMethod = QStringLiteral("agents.list");
inline const QString MessageMethod = QStringLiteral("agents.message");
inline const QString RunnersMethod = QStringLiteral("agents.runners");
inline const QString NotifyMethod = QStringLiteral("desktop.notify");

constexpr int kMaxReasonChars = 300;
constexpr int kMaxMessageChars = 8000;
constexpr int kMessageLimit = 30;
constexpr qint64 kMessageWindowMs = 10 * 60 * 1000;
constexpr int kDefaultListed = 30;
constexpr int kMaxListed = 100;
constexpr int kMaxNotifyTitleChars = 120;
constexpr int kMaxNotifyBodyChars = 1000;
constexpr int kNotifyLimit = 10;
constexpr qint64 kNotifyWindowMs = 10 * 60 * 1000;
constexpr int kSuggestionPreviewChars = 1200;

inline const QString StartButton = QStringLiteral("Start session");
inline const QString DismissButton = QStringLiteral("Not now");

struct SuggestRequest {
    QString owner;
    QString name;
    QString prompt;
    QString title;
    QString provider;
    QString model;
    QString reason;
};

struct ListRequest {
    bool includeFinished = false;
    int limit = kDefaultListed;
};

struct MessageRequest {
    int target = 0;
    QString text;
};

struct NotifyRequest {
    QString title;
    QString body;
};

bool parseSuggestRequest(const QJsonObject &params, SuggestRequest *out, QString *error);
bool parseListRequest(const QJsonObject &params, ListRequest *out, QString *error);
bool parseMessageRequest(const QJsonObject &params, MessageRequest *out, QString *error);
bool parseNotifyRequest(const QJsonObject &params, NotifyRequest *out, QString *error);

QString suggestionTitle(const SuggestRequest &request);
QString suggestionBody(const SuggestRequest &request, const QString &slug,
                       const QString &fromLabel);

int permissionRank(const QString &modeLabel, int unknownRank);
bool messageAllowed(const QString &senderMode, const QString &targetMode);
QString messageEnvelope(int senderId, const QString &senderTitle, const QString &text);
QString messageLead(int senderId, const QString &senderTitle);

}
