#include "PeopleModel.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace forkmesh::people {
namespace {

constexpr qint64 kHour = 60LL * 60 * 1000;
constexpr qint64 kNewWindowMs = 14LL * 24 * 60 * 60 * 1000;
constexpr int kBlazingScore = 70;

int recency(const QString &bucket)
{
    static const QHash<QString, int> points{
        {QStringLiteral("hour"), 48}, {QStringLiteral("5h"), 46}, {QStringLiteral("24h"), 42},
        {QStringLiteral("3d"), 36},   {QStringLiteral("5d"), 32}, {QStringLiteral("10d"), 25},
        {QStringLiteral("30d"), 18},  {QStringLiteral("60d"), 11}, {QStringLiteral("120d"), 5},
    };
    return points.value(bucket, 0);
}

int tier(qint64 value, std::initializer_list<std::pair<qint64, int>> tiers)
{
    for (const auto &[minimum, points] : tiers) {
        if (value >= minimum)
            return points;
    }
    return 0;
}

qint64 count(const QJsonValue &value)
{
    return std::max<qint64>(0, value.toVariant().toLongLong());
}

bool before(const Ranked &left, const Ranked &right)
{
    if (left.score != right.score)
        return left.score > right.score;
    if (left.member.totalActiveMs != right.member.totalActiveMs)
        return left.member.totalActiveMs > right.member.totalActiveMs;
    return left.member.id() < right.member.id();
}

} // namespace

QList<Ring> allRings()
{
    return {Ring::Blazing, Ring::Glowing, Ring::Kindling, Ring::Warm, Ring::Embers};
}

int ringOrder(Ring ring)
{
    return static_cast<int>(ring);
}

QString ringId(Ring ring)
{
    switch (ring) {
    case Ring::Blazing: return QStringLiteral("blazing");
    case Ring::Glowing: return QStringLiteral("glowing");
    case Ring::Kindling: return QStringLiteral("kindling");
    case Ring::Warm: return QStringLiteral("warm");
    case Ring::Embers: return QStringLiteral("embers");
    }
    return QString();
}

QString ringTitle(Ring ring)
{
    switch (ring) {
    case Ring::Blazing: return QStringLiteral("Blazing");
    case Ring::Glowing: return QStringLiteral("Glowing");
    case Ring::Kindling: return QStringLiteral("Kindling");
    case Ring::Warm: return QStringLiteral("Warm");
    case Ring::Embers: return QStringLiteral("Embers");
    }
    return QString();
}

QString ringBlurb(Ring ring)
{
    switch (ring) {
    case Ring::Blazing: return QStringLiteral("Active today and deep in the work");
    case Ring::Glowing: return QStringLiteral("Active in the last five days");
    case Ring::Kindling: return QStringLiteral("Joined in the last two weeks");
    case Ring::Warm: return QStringLiteral("Active this month, or has contributed");
    case Ring::Embers: return QStringLiteral("Quiet for a while");
    }
    return QString();
}

QColor ringColor(Ring ring)
{
    switch (ring) {
    case Ring::Blazing: return QColor(0xff, 0x6b, 0x2b);
    case Ring::Glowing: return QColor(0xff, 0xa4, 0x3a);
    case Ring::Kindling: return QColor(0xf4, 0xcf, 0x6b);
    case Ring::Warm: return QColor(0x5f, 0xc2, 0x8e);
    case Ring::Embers: return QColor(0x7e, 0xa6, 0xc4);
    }
    return QColor();
}

QString avatarBase64(const QString &dataUrl)
{
    if (!dataUrl.startsWith(QLatin1String("data:image/")))
        return QString();
    const qsizetype comma = dataUrl.indexOf(QLatin1Char(','));
    if (comma < 0 || !dataUrl.left(comma).endsWith(QLatin1String(";base64")))
        return QString();
    return dataUrl.mid(comma + 1);
}

std::optional<Member> parseMember(const QJsonObject &object)
{
    const QString name = object.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty() || object.value(QStringLiteral("kind")).toString(QStringLiteral("user")) != QLatin1String("user"))
        return std::nullopt;
    const QJsonObject status = object.value(QStringLiteral("userStatus")).toObject();
    Member member;
    member.name = name;
    member.avatar = avatarBase64(object.value(QStringLiteral("avatarThumb")).toString());
    member.createdAt = count(object.value(QStringLiteral("createdAt")));
    member.activityBucket = object.value(QStringLiteral("activityBucket")).toString();
    member.totalActiveMs = count(object.value(QStringLiteral("totalActiveMs")));
    member.totalSteps = count(object.value(QStringLiteral("totalSteps")));
    member.officeVisits = int(count(object.value(QStringLiteral("officeVisits"))));
    for (const char *key : {"issues", "pulls", "commits", "discussions"})
        member.contributions += int(count(object.value(QLatin1String(key))));
    member.nodeCount = int(object.value(QStringLiteral("nodes")).toArray().size());
    member.emailVerified = object.value(QStringLiteral("emailVerified")).toBool();
    member.hasPayoutAddress = !object.value(QStringLiteral("solana")).toString().trimmed().isEmpty();
    member.statusText = status.value(QStringLiteral("text")).toString().left(80);
    member.statusEmoji = status.value(QStringLiteral("emoji")).toString().left(8);
    return member;
}

QList<Member> parseDirectory(const QJsonObject &payload)
{
    QList<Member> members;
    QSet<QString> seen;
    for (const QJsonValue &value : payload.value(QStringLiteral("users")).toArray()) {
        const std::optional<Member> member = parseMember(value.toObject());
        if (member && !seen.contains(member->id())) {
            seen.insert(member->id());
            members.append(*member);
        }
    }
    return members;
}

int score(const Member &member)
{
    const int depth =
        tier(member.totalActiveMs, {{40 * kHour, 22}, {12 * kHour, 17}, {4 * kHour, 12}, {kHour, 7}, {10LL * 60 * 1000, 3}}) +
        tier(member.totalSteps, {{100000, 13}, {25000, 10}, {5000, 7}, {500, 4}, {1, 1}}) +
        tier(member.officeVisits, {{20, 6}, {5, 4}, {1, 2}}) +
        std::min(14, int(std::floor(std::log2(double(member.contributions + 1)) * 3.5)));
    const int readiness = (member.emailVerified ? 4 : 0) + (member.hasPayoutAddress ? 4 : 0) +
                          std::min(7, member.nodeCount * 3);
    return std::clamp(recency(member.activityBucket) + depth + readiness, 0, 100);
}

Ring ring(const Member &member, qint64 now)
{
    const QString &bucket = member.activityBucket;
    const bool today = bucket == QLatin1String("hour") || bucket == QLatin1String("5h") || bucket == QLatin1String("24h");
    if (today && score(member) >= kBlazingScore)
        return Ring::Blazing;
    if (today || bucket == QLatin1String("3d") || bucket == QLatin1String("5d"))
        return Ring::Glowing;
    if (member.createdAt > 0 && now - member.createdAt < kNewWindowMs)
        return Ring::Kindling;
    if (bucket == QLatin1String("10d") || bucket == QLatin1String("30d") || member.contributions > 0 ||
        member.totalSteps > 0 || member.officeVisits > 0)
        return Ring::Warm;
    return Ring::Embers;
}

QString lastActive(const QString &bucket)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("hour"), QStringLiteral("Active in the last hour")},
        {QStringLiteral("5h"), QStringLiteral("Active in the last few hours")},
        {QStringLiteral("24h"), QStringLiteral("Active today")},
        {QStringLiteral("3d"), QStringLiteral("Active in the last 3 days")},
        {QStringLiteral("5d"), QStringLiteral("Active in the last 5 days")},
        {QStringLiteral("10d"), QStringLiteral("Active in the last 10 days")},
        {QStringLiteral("30d"), QStringLiteral("Active this month")},
        {QStringLiteral("60d"), QStringLiteral("Active in the last 2 months")},
        {QStringLiteral("120d"), QStringLiteral("Active in the last 4 months")},
    };
    return labels.value(bucket, QStringLiteral("Not seen recently"));
}

QList<Section> sections(const QList<Member> &members, qint64 now)
{
    QList<Section> out;
    for (Ring each : allRings())
        out.append({each, {}});
    for (const Member &member : members) {
        const Ring placed = ring(member, now);
        out[ringOrder(placed)].people.append({member, placed, score(member)});
    }
    for (Section &section : out)
        std::stable_sort(section.people.begin(), section.people.end(), before);
    return out;
}

QList<Section> filtered(const QList<Section> &all, const QString &query, std::optional<Ring> focus)
{
    QString needle = query.trimmed().toLower();
    while (needle.startsWith(QLatin1Char('@')))
        needle.remove(0, 1);
    QList<Section> out;
    for (const Section &section : all) {
        if (focus && section.ring != *focus)
            continue;
        Section copy{section.ring, {}};
        for (const Ranked &entry : section.people) {
            if (needle.isEmpty() || entry.member.id().contains(needle))
                copy.people.append(entry);
        }
        out.append(copy);
    }
    return out;
}

Profile parseProfile(const QJsonObject &object, const QString &requested)
{
    const QJsonObject social = object.value(QStringLiteral("social")).toObject();
    const QJsonObject status = object.value(QStringLiteral("userStatus")).toObject();
    const auto pick = [&](const QString &key) {
        return int(count(social.contains(key) ? social.value(key) : object.value(key)));
    };
    Profile profile;
    profile.name = object.value(QStringLiteral("name")).toString(requested);
    if (profile.name.isEmpty())
        profile.name = requested;
    profile.exists = object.value(QStringLiteral("exists")).toBool(true);
    profile.isPrivate = object.value(QStringLiteral("profilePrivate")).toBool();
    profile.avatar = object.value(QStringLiteral("avatarPng")).toString();
    profile.bio = object.value(QStringLiteral("profileBio")).toString().left(500);
    profile.location = object.value(QStringLiteral("profileLocation")).toString().left(120);
    profile.createdAt = count(object.value(QStringLiteral("createdAt")));
    profile.followers = pick(QStringLiteral("followers"));
    profile.following = pick(QStringLiteral("following"));
    profile.isFollowing = (social.contains(QStringLiteral("isFollowing")) ? social : object)
                              .value(QStringLiteral("isFollowing")).toBool();
    profile.statusText = status.value(QStringLiteral("text")).toString().left(80);
    profile.statusEmoji = status.value(QStringLiteral("emoji")).toString().left(8);
    return profile;
}

bool validName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,62}$"));
    return pattern.match(name).hasMatch();
}

double radius(int ring, double size)
{
    const double half = size / 2;
    const double inner = half * 0.26;
    return inner + (half * 0.9 - inner) / double(kRingCount - 1) * ring;
}

double labelGap(int ring, double size, double seat, double label)
{
    return std::asin(std::min(1.0, (label / 2 + seat / 2 + 2) / radius(ring, size)));
}

int capacity(int ring, double size, double seat, double label)
{
    const double arc = 2 * M_PI - 2 * labelGap(ring, size, seat, label);
    return std::max(2, int(std::floor(arc * radius(ring, size) / (seat * 1.15))));
}

QList<Seat> seats(const QList<Section> &all, double size, double seat, double label)
{
    QList<Seat> out;
    for (const Section &section : all) {
        if (section.people.isEmpty())
            continue;
        const int index = ringOrder(section.ring);
        const int cap = capacity(index, size, seat, label);
        const int overflow = section.people.size() > cap ? int(section.people.size()) - (cap - 1) : 0;
        const int shown = overflow > 0 ? cap - 1 : int(section.people.size());
        const int total = shown + (overflow > 0 ? 1 : 0);
        const double r = radius(index, size);
        const double arc = 2 * M_PI - 2 * labelGap(index, size, seat, label);
        const double spread = std::min(arc, total * seat * 1.15 / r);
        const double start = M_PI / 2 - spread / 2;
        for (int slot = 0; slot < total; ++slot) {
            const double angle = start + spread * (slot + 0.5) / total;
            Seat placed;
            placed.ring = section.ring;
            placed.center = QPointF(size / 2 + r * std::cos(angle), size / 2 + r * std::sin(angle));
            if (slot < shown)
                placed.person = section.people.at(slot);
            else
                placed.overflow = overflow;
            out.append(placed);
        }
    }
    return out;
}

QString directoryPath()
{
    return QStringLiteral("/api/accounts/users?view=world");
}

QString profilePath(const QString &name, const QString &viewer)
{
    const QString base = QStringLiteral("/api/accounts/") + name.toLower();
    const QString who = viewer.trimmed().toLower();
    if (!validName(who))
        return base;
    return base + QStringLiteral("?viewer=") + QString::fromLatin1(QUrl::toPercentEncoding(who));
}

QString followPath(const QString &name)
{
    return QStringLiteral("/api/accounts/") + name.toLower() + QStringLiteral("/follow");
}

QString followError(const QJsonObject &payload, const QString &error, bool follow)
{
    const QString code = payload.value(QStringLiteral("error")).toString();
    if (code == QLatin1String("cannot_follow_self"))
        return QStringLiteral("You cannot follow yourself.");
    if (code == QLatin1String("email_not_verified") || code == QLatin1String("account_pending_verification") ||
        error.contains(QLatin1String("403")))
        return QStringLiteral("Verify your email to follow people.");
    if (code == QLatin1String("invalid_session") || error.contains(QLatin1String("401")))
        return QStringLiteral("Sign in to your ForkMesh account to follow people.");
    return follow ? QStringLiteral("Could not follow.") : QStringLiteral("Could not unfollow.");
}

} // namespace forkmesh::people
