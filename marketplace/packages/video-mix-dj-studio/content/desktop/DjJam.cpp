#include "DjJam.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

QString text(const QJsonObject &object, const QString &key, const QString &fallback = QString())
{
    return object.value(key).toString(fallback);
}

qint64 whole(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? qint64(value.toDouble()) : 0;
}

QList<QJsonObject> rows(const QJsonValue &value)
{
    QList<QJsonObject> out;
    for (const QJsonValue &row : value.toArray()) {
        if (row.isObject())
            out.append(row.toObject());
    }
    return out;
}

}

namespace motion {

double clamp(double value)
{
    if (!std::isfinite(value))
        return 0;
    return std::min(limit, std::max(-limit, value));
}

QList<DjMotionSample> parse(const QJsonValue &value)
{
    QList<DjMotionSample> out;
    const QJsonArray items = value.toArray();
    for (qsizetype i = 0; i < std::min<qsizetype>(32, items.size()); ++i) {
        const QJsonValue item = items.at(i);
        if (!item.isArray())
            continue;
        const QJsonArray pair = item.toArray();
        if (pair.size() != 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble())
            continue;
        out.append(DjMotionSample{clamp(pair.at(0).toDouble()), clamp(pair.at(1).toDouble())});
    }
    return out;
}

}

QString presenceStateKey(DjPresenceState state)
{
    switch (state) {
    case DjPresenceState::Listening:
        return QStringLiteral("listening");
    case DjPresenceState::Idle:
        return QStringLiteral("idle");
    default:
        return QStringLiteral("studio");
    }
}

QString statusLabel(DjPersonStatus status)
{
    switch (status) {
    case DjPersonStatus::Invited:
        return QStringLiteral("Invited");
    case DjPersonStatus::Away:
        return QStringLiteral("Away");
    case DjPersonStatus::Idle:
        return QStringLiteral("Not listening");
    case DjPersonStatus::Listening:
        return QStringLiteral("Listening in the background");
    case DjPersonStatus::Studio:
        return QStringLiteral("In the studio");
    }
    return QString();
}

bool isLive(DjPersonStatus status) { return status == DjPersonStatus::Listening || status == DjPersonStatus::Studio; }

std::optional<DjJamPresence> DjJamPresence::fromJson(const QJsonObject &object)
{
    const QString name = text(object, QStringLiteral("name"));
    if (name.isEmpty())
        return std::nullopt;
    DjJamPresence presence;
    presence.name = name.left(64);
    presence.state = text(object, QStringLiteral("state"), QStringLiteral("studio"));
    presence.playing = object.value(QStringLiteral("playing")).toBool(false);
    presence.energy = std::min(1.0, std::max(0.0, object.value(QStringLiteral("energy")).toDouble(0)));
    presence.motion = motion::parse(object.value(QStringLiteral("motion")));
    presence.ago = std::max<qint64>(0, whole(object, QStringLiteral("ago")));
    return presence;
}

QJsonObject DjJamPresence::body(DjPresenceState state, bool playing)
{
    return {{QStringLiteral("state"), presenceStateKey(state)},
            {QStringLiteral("playing"), playing},
            {QStringLiteral("energy"), 0.0},
            {QStringLiteral("motion"), QJsonArray()}};
}

std::optional<DjJamReaction> DjJamReaction::fromJson(const QJsonObject &object)
{
    const QString id = text(object, QStringLiteral("id"));
    const QString kind = text(object, QStringLiteral("kind"));
    if (id.isEmpty() || !object.value(QStringLiteral("kind")).isString() || !DjReaction::find(kind))
        return std::nullopt;
    return DjJamReaction{id, text(object, QStringLiteral("name")), kind,
                         std::max<qint64>(0, whole(object, QStringLiteral("ago")))};
}

bool DjJamMember::joined() const
{
    return status == QLatin1String("owner") || status == QLatin1String("joined");
}

DjJamDeck DjJamDeck::fromJson(const QJsonObject &object)
{
    const QString id = text(object, QStringLiteral("videoId"));
    DjJamDeck deck;
    deck.videoId = studio::isVideoId(id) ? id : QString();
    deck.title = text(object, QStringLiteral("title")).left(300);
    deck.author = text(object, QStringLiteral("author")).left(200);
    deck.playing = object.value(QStringLiteral("playing")).toBool(false);
    deck.position = std::max(0.0, object.value(QStringLiteral("position")).toDouble(0));
    deck.rate = studio::clampRate(object.value(QStringLiteral("rate")).toDouble(1));
    deck.at = whole(object, QStringLiteral("at"));
    deck.by = text(object, QStringLiteral("by"));
    return deck;
}

double DjJamDeck::expectedPosition(qint64 serverNow) const
{
    if (!playing || at <= 0 || serverNow <= at)
        return position;
    return position + double(serverNow - at) / 1000 * rate;
}

QJsonObject DjJamDeck::body() const
{
    return {{QStringLiteral("videoId"), videoId},
            {QStringLiteral("title"), title},
            {QStringLiteral("author"), author},
            {QStringLiteral("playing"), playing},
            {QStringLiteral("position"), std::round(position * 1000) / 1000},
            {QStringLiteral("rate"), rate}};
}

QList<DjJamSummary> DjJamSummary::list(const QJsonObject &object)
{
    QList<DjJamSummary> out;
    for (const QJsonObject &row : rows(object.value(QStringLiteral("jams")))) {
        const QString id = text(row, QStringLiteral("id"));
        if (id.isEmpty())
            continue;
        out.append(DjJamSummary{id, text(row, QStringLiteral("title")), text(row, QStringLiteral("owner")),
                                text(row, QStringLiteral("myStatus")), int(whole(row, QStringLiteral("members")))});
    }
    return out;
}

std::optional<DjJamPresence> DjJam::presenceOf(const QString &name) const
{
    for (const DjJamPresence &seen : presence) {
        if (seen.name == name)
            return seen;
    }
    return std::nullopt;
}

DjPersonStatus DjJam::status(const DjJamMember &member) const { return status(member, presenceOf(member.name)); }

DjPersonStatus DjJam::status(const DjJamMember &member, const std::optional<DjJamPresence> &seen)
{
    if (!member.joined())
        return DjPersonStatus::Invited;
    if (!seen || seen->ago > awayAfter)
        return DjPersonStatus::Away;
    if (seen->state == presenceStateKey(DjPresenceState::Studio))
        return DjPersonStatus::Studio;
    return seen->playing ? DjPersonStatus::Listening : DjPersonStatus::Idle;
}

DjJam DjJam::shared() const
{
    DjJam copy = *this;
    copy.now = 0;
    copy.presence.clear();
    copy.reactions.clear();
    return copy;
}

bool DjJam::isOwner() const { return myStatus == QLatin1String("owner"); }

bool DjJam::joined() const
{
    return myStatus == QLatin1String("owner") || myStatus == QLatin1String("joined");
}

QList<DjJamMember> DjJam::listeners() const
{
    QList<DjJamMember> out;
    for (const DjJamMember &member : members) {
        if (member.joined())
            out.append(member);
    }
    return out;
}

DjJamDeck DjJam::deck(DjSide side) const { return decks.value(side, DjJamDeck()); }

std::optional<DjJam> DjJam::parse(const QJsonObject &object)
{
    const QJsonValue value = object.value(QStringLiteral("jam"));
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject jam = value.toObject();
    const QString id = text(jam, QStringLiteral("id"));
    if (id.isEmpty())
        return std::nullopt;
    DjJam out;
    out.id = id;
    out.title = text(jam, QStringLiteral("title"));
    out.owner = text(jam, QStringLiteral("owner"));
    out.me = text(jam, QStringLiteral("me"));
    out.myStatus = text(jam, QStringLiteral("myStatus"));
    out.now = whole(jam, QStringLiteral("now"));
    const QJsonValue decks = jam.value(QStringLiteral("decks"));
    if (decks.isObject()) {
        const QJsonObject sides = decks.toObject();
        for (const DjSide side : {DjSide::A, DjSide::B}) {
            const QJsonValue row = sides.value(sideKey(side));
            if (row.isObject())
                out.decks.insert(side, DjJamDeck::fromJson(row.toObject()));
        }
    }
    for (const QJsonObject &row : rows(jam.value(QStringLiteral("members")))) {
        const QString name = text(row, QStringLiteral("name"));
        if (!name.isEmpty())
            out.members.append(DjJamMember{name, text(row, QStringLiteral("status"))});
    }
    for (const QJsonObject &row : rows(jam.value(QStringLiteral("queue")))) {
        if (text(row, QStringLiteral("id")).isEmpty())
            continue;
        if (const auto item = DjQueueItem::fromJson(row))
            out.queue.append(*item);
    }
    for (const QJsonObject &row : rows(jam.value(QStringLiteral("presence")))) {
        if (const auto seen = DjJamPresence::fromJson(row))
            out.presence.append(*seen);
    }
    for (const QJsonObject &row : rows(jam.value(QStringLiteral("reactions")))) {
        if (const auto reaction = DjJamReaction::fromJson(row))
            out.reactions.append(*reaction);
    }
    return out;
}

QStringList DjJam::userNames(const QJsonObject &object, const QSet<QString> &excluding)
{
    QStringList names;
    for (const QJsonObject &row : rows(object.value(QStringLiteral("users")))) {
        const QString name = text(row, QStringLiteral("name")).trimmed().toLower();
        if (!name.isEmpty() && !excluding.contains(name) && !names.contains(name))
            names.append(name);
    }
    return names;
}

bool DjJam::needsSeek(double local, double remote, bool playing)
{
    if (!std::isfinite(local) || !std::isfinite(remote))
        return false;
    return std::abs(local - remote) > (playing ? 1.5 : 0.5);
}

}
