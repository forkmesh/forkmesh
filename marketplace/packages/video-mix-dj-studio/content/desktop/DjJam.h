#pragma once

#include "DjCore.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace forkmesh::djstudio {

struct DjMotionSample {
    double x = 0;
    double y = 0;
    bool operator==(const DjMotionSample &) const = default;
};

namespace motion {

inline constexpr double limit = 2;
inline constexpr int shared = 12;
inline constexpr double dancing = 0.3;

double clamp(double value);
QList<DjMotionSample> parse(const QJsonValue &value);

}

enum class DjPresenceState { Studio, Listening, Idle };

QString presenceStateKey(DjPresenceState state);

enum class DjPersonStatus { Invited, Away, Idle, Listening, Studio };

QString statusLabel(DjPersonStatus status);
bool isLive(DjPersonStatus status);

struct DjJamPresence {
    QString name;
    QString state = QStringLiteral("studio");
    bool playing = false;
    double energy = 0;
    QList<DjMotionSample> motion;
    qint64 ago = 0;
    bool operator==(const DjJamPresence &) const = default;
    bool dancing() const { return energy >= motion::dancing; }
    static std::optional<DjJamPresence> fromJson(const QJsonObject &object);
    static QJsonObject body(DjPresenceState state, bool playing);
};

struct DjJamReaction {
    QString id;
    QString name;
    QString kind;
    qint64 ago = 0;
    bool operator==(const DjJamReaction &) const = default;
    static std::optional<DjJamReaction> fromJson(const QJsonObject &object);
};

struct DjJamMember {
    QString name;
    QString status;
    bool operator==(const DjJamMember &) const = default;
    bool joined() const;
};

struct DjJamDeck {
    QString videoId;
    QString title;
    QString author;
    bool playing = false;
    double position = 0;
    double rate = 1;
    qint64 at = 0;
    QString by;
    bool operator==(const DjJamDeck &) const = default;
    static DjJamDeck fromJson(const QJsonObject &object);
    double expectedPosition(qint64 serverNow) const;
    QJsonObject body() const;
};

struct DjJamSummary {
    QString id;
    QString title;
    QString owner;
    QString myStatus;
    int members = 0;
    bool operator==(const DjJamSummary &) const = default;
    bool invited() const { return myStatus == QLatin1String("invited"); }
    static QList<DjJamSummary> list(const QJsonObject &object);
};

struct DjJam {
    static constexpr qint64 awayAfter = 12000;

    QString id;
    QString title;
    QString owner;
    QString me;
    QString myStatus;
    qint64 now = 0;
    QList<DjJamMember> members;
    QList<DjQueueItem> queue;
    QMap<DjSide, DjJamDeck> decks;
    QList<DjJamPresence> presence;
    QList<DjJamReaction> reactions;
    bool operator==(const DjJam &) const = default;

    std::optional<DjJamPresence> presenceOf(const QString &name) const;
    DjPersonStatus status(const DjJamMember &member) const;
    static DjPersonStatus status(const DjJamMember &member, const std::optional<DjJamPresence> &seen);
    DjJam shared() const;
    bool isOwner() const;
    bool joined() const;
    QList<DjJamMember> listeners() const;
    DjJamDeck deck(DjSide side) const;

    static std::optional<DjJam> parse(const QJsonObject &object);
    static QStringList userNames(const QJsonObject &object, const QSet<QString> &excluding = {});
    static bool needsSeek(double local, double remote, bool playing);
};

}
