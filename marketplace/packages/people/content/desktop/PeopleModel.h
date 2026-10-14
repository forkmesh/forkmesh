#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>

#include <optional>

namespace forkmesh::people {

enum class Ring { Blazing, Glowing, Kindling, Warm, Embers };

constexpr int kRingCount = 5;

QList<Ring> allRings();
QString ringId(Ring ring);
QString ringTitle(Ring ring);
QString ringBlurb(Ring ring);
QColor ringColor(Ring ring);
int ringOrder(Ring ring);

struct Member {
    QString name;
    QString avatar;
    qint64 createdAt = 0;
    QString activityBucket;
    qint64 totalActiveMs = 0;
    qint64 totalSteps = 0;
    int officeVisits = 0;
    int contributions = 0;
    int nodeCount = 0;
    bool emailVerified = false;
    bool hasPayoutAddress = false;
    QString statusText;
    QString statusEmoji;

    QString id() const { return name.toLower(); }
};

struct Ranked {
    Member member;
    Ring ring = Ring::Embers;
    int score = 0;
};

struct Section {
    Ring ring = Ring::Embers;
    QList<Ranked> people;
};

struct Seat {
    Ring ring = Ring::Embers;
    QPointF center;
    std::optional<Ranked> person;
    int overflow = 0;
};

struct Profile {
    QString name;
    bool exists = true;
    bool isPrivate = false;
    QString avatar;
    QString bio;
    QString location;
    qint64 createdAt = 0;
    int followers = 0;
    int following = 0;
    bool isFollowing = false;
    QString statusText;
    QString statusEmoji;
};

QString avatarBase64(const QString &dataUrl);
std::optional<Member> parseMember(const QJsonObject &object);
QList<Member> parseDirectory(const QJsonObject &payload);
int score(const Member &member);
Ring ring(const Member &member, qint64 now);
QString lastActive(const QString &bucket);
QList<Section> sections(const QList<Member> &members, qint64 now);
QList<Section> filtered(const QList<Section> &all, const QString &query, std::optional<Ring> focus);
Profile parseProfile(const QJsonObject &object, const QString &requested);
bool validName(const QString &name);

double radius(int ring, double size);
double labelGap(int ring, double size, double seat, double label);
int capacity(int ring, double size, double seat, double label);
QList<Seat> seats(const QList<Section> &all, double size, double seat, double label);

QString directoryPath();
QString profilePath(const QString &name, const QString &viewer);
QString followPath(const QString &name);
QString followError(const QJsonObject &payload, const QString &error, bool follow);

} // namespace forkmesh::people
