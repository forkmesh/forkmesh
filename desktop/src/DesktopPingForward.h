#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::pingforward {

inline constexpr qint64 kPhoneRecentMs = 30LL * 24 * 60 * 60 * 1000;
inline constexpr qint64 kDuplicateWindowMs = 15 * 1000;
inline constexpr qint64 kBurstWindowMs = 10 * 60 * 1000;
inline constexpr int kBurstLimit = 30;
inline constexpr int kTitleMax = 160;
inline constexpr int kBodyMax = 500;
inline constexpr qint64 kOnlineGapMs = 10 * 60 * 1000;
inline constexpr int kDeleteBatch = 50;
inline constexpr qint64 kCelebrationFreshMs = 5 * 60 * 1000;

struct Ping {
    QString pingId;
    QString node;
    QString kind;
    QString category;
    QString sessionId;
    QString runId;
    QString repo;
    bool warning = false;
    QString title;
    QString body;
};

struct Phone {
    QString user;
    QString platform;
    qint64 lastSeenMs = 0;
    bool connected = false;
};

struct Source {
    qint64 localId = 0;
    qint64 timestampMs = 0;
    QString node;
    QString kind;
    QString linkKind;
    int linkNumber = -1;
    int runId = 0;
    QString repo;
    bool warning = false;
    QString title;
    QString body;
};

struct Screen {
    bool active = false;
    bool agentList = false;
    bool pingsPage = false;
};

QString proof();
QString deleteProof();
QString path();
QString kind();
QString settingKey();
QString onlineKind();
QString onlineSettingKey();

bool phoneLinked(const QList<Phone> &phones, const QString &account,
                 qint64 nowMs);
bool forwardsKind(const QString &kind);
QString categoryFor(const QString &kind, const QString &linkKind, bool warning);
QString fitText(const QString &text, int max, bool singleLine);
QString nodeToken(const QString &node);
Ping pingFor(const Source &source);
bool valid(const Ping &ping);
bool seenOnScreen(const Screen &screen, const Source &source);
QString resource(const Ping &ping);
QString canonical(const QString &user, const Ping &ping, const QString &ts);
QJsonObject body(const Ping &ping);
QString deleteResource(const QString &node, const QStringList &pingIds);
QString deleteCanonical(const QString &user, const QString &node,
                        const QStringList &pingIds, const QString &ts);
QJsonObject deleteBody(const QString &node, const QStringList &pingIds);
QString duplicateKey(const Ping &ping);
bool isOwnEcho(const QJsonObject &alert, const QString &node);
bool isFreshCelebration(const QJsonObject &alert, qint64 nowMs);
bool cameOnline(qint64 lastOnlineMs, qint64 nowMs);
QString offlineSpan(qint64 ms);
Source onlineSource(const QString &node, qint64 lastOnlineMs, qint64 nowMs);

class Gate
{
public:
    bool admit(const Ping &ping, qint64 nowMs);

private:
    struct Sent {
        qint64 atMs = 0;
        QString key;
    };
    QList<Sent> m_sent;
};

} // namespace forkmesh::pingforward
