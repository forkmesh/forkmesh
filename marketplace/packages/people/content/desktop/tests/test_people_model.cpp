#include "PeopleModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

using namespace forkmesh::people;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

QJsonObject fixture()
{
    const QString relative = QStringLiteral("marketplace/packages/people/tests/fixtures/rings.json");
    QStringList starts{QDir::currentPath(), QCoreApplication::applicationDirPath(),
                       QFileInfo(QStringLiteral(__FILE__)).absolutePath()};
    for (const QString &start : starts) {
        QDir dir(start);
        do {
            QFile file(dir.filePath(relative));
            if (file.open(QIODevice::ReadOnly))
                return QJsonDocument::fromJson(file.readAll()).object();
        } while (dir.cdUp());
    }
    return {};
}

void fixtureCases(const QJsonObject &data)
{
    const qint64 now = data.value(QStringLiteral("now")).toVariant().toLongLong();
    QStringList ids;
    for (Ring each : allRings())
        ids << ringId(each);
    check(data.value(QStringLiteral("rings")).toVariant().toStringList() == ids, QStringLiteral("ring ids match the fixture"));
    for (const QJsonValue &value : data.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonObject user = item.value(QStringLiteral("user")).toObject();
        const std::optional<Member> member = parseMember(user);
        if (item.value(QStringLiteral("member")).isNull()) {
            check(!member, QStringLiteral("skips '%1'").arg(user.value(QStringLiteral("name")).toString()));
            continue;
        }
        const QJsonObject want = item.value(QStringLiteral("member")).toObject();
        if (!member) {
            check(false, QStringLiteral("parses %1").arg(want.value(QStringLiteral("name")).toString()));
            continue;
        }
        const QString name = member->name;
        check(member->contributions == want.value(QStringLiteral("contributions")).toInt(), name + QStringLiteral(" contributions"));
        check(member->nodeCount == want.value(QStringLiteral("nodeCount")).toInt(), name + QStringLiteral(" nodes"));
        check(member->hasPayoutAddress == want.value(QStringLiteral("hasPayoutAddress")).toBool(), name + QStringLiteral(" payout"));
        check(member->avatar == want.value(QStringLiteral("avatar")).toString(), name + QStringLiteral(" avatar"));
        check(score(*member) == item.value(QStringLiteral("score")).toInt(), name + QStringLiteral(" score"));
        check(ringId(ring(*member, now)) == item.value(QStringLiteral("ring")).toString(), name + QStringLiteral(" ring"));
    }
    QList<Member> members;
    for (const QJsonValue &value : data.value(QStringLiteral("cases")).toArray()) {
        if (const std::optional<Member> member = parseMember(value.toObject().value(QStringLiteral("user")).toObject()))
            members.append(*member);
    }
    const QList<Section> all = sections(members, now);
    const QJsonArray order = data.value(QStringLiteral("order")).toArray();
    check(all.size() == order.size(), QStringLiteral("one section per ring"));
    for (int i = 0; i < all.size() && i < order.size(); ++i) {
        QStringList names;
        for (const Ranked &entry : all.at(i).people)
            names << entry.member.name;
        check(names == order.at(i).toObject().value(QStringLiteral("names")).toVariant().toStringList(),
              QStringLiteral("order in %1").arg(ringId(all.at(i).ring)));
    }
    for (const QJsonValue &value : data.value(QStringLiteral("layout")).toArray()) {
        const QJsonObject layout = value.toObject();
        const double size = layout.value(QStringLiteral("size")).toDouble();
        const double seat = layout.value(QStringLiteral("seat")).toDouble();
        const double label = layout.value(QStringLiteral("label")).toDouble();
        for (int ringIndex = 0; ringIndex < kRingCount; ++ringIndex) {
            check(std::abs(radius(ringIndex, size) - layout.value(QStringLiteral("radii")).toArray().at(ringIndex).toDouble()) < 1e-4,
                  QStringLiteral("radius %1 at %2").arg(ringIndex).arg(size));
            check(capacity(ringIndex, size, seat, label) == layout.value(QStringLiteral("capacities")).toArray().at(ringIndex).toInt(),
                  QStringLiteral("capacity %1 at %2").arg(ringIndex).arg(size));
        }
    }
}

void overflowSeat()
{
    QList<Member> people;
    for (int i = 0; i < 40; ++i) {
        Member member;
        member.name = QStringLiteral("p%1").arg(i);
        member.activityBucket = QStringLiteral("120d");
        people.append(member);
    }
    const QList<Seat> placed = seats(sections(people, 0), 360, 30, 72);
    const int cap = capacity(4, 360, 30, 72);
    check(placed.size() == cap, QStringLiteral("a full ring fills its capacity"));
    check(!placed.last().person && placed.last().overflow == 40 - (cap - 1), QStringLiteral("the last seat counts the rest"));
}

void paths()
{
    check(profilePath(QStringLiteral("Bob"), QStringLiteral("Ada")) == QStringLiteral("/api/accounts/bob?viewer=ada"), QStringLiteral("profile asks as the viewer"));
    check(profilePath(QStringLiteral("bob"), QString()) == QStringLiteral("/api/accounts/bob"), QStringLiteral("signed out asks anonymously"));
    check(followPath(QStringLiteral("Bob")) == QStringLiteral("/api/accounts/bob/follow"), QStringLiteral("follow path"));
    check(!validName(QStringLiteral("../x")) && validName(QStringLiteral("ada-1")), QStringLiteral("names are validated"));
    check(followError({{QStringLiteral("error"), QStringLiteral("email_not_verified")}}, QString(), true) == QStringLiteral("Verify your email to follow people."),
          QStringLiteral("unverified email is explained"));
    const QList<Section> found = filtered(sections({Member{QStringLiteral("Ada")}, Member{QStringLiteral("bob")}}, 0), QStringLiteral(" @ad "), std::nullopt);
    int matches = 0;
    for (const Section &section : found)
        matches += int(section.people.size());
    check(matches == 1, QStringLiteral("filter ignores the at sign"));
    const Profile profile = parseProfile({{QStringLiteral("name"), QStringLiteral("bob")},
                                          {QStringLiteral("social"), QJsonObject{{QStringLiteral("followers"), 3}, {QStringLiteral("isFollowing"), true}}}},
                                         QStringLiteral("bob"));
    check(profile.followers == 3 && profile.isFollowing, QStringLiteral("profile reads the social block"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QJsonObject data = fixture();
    check(!data.isEmpty(), QStringLiteral("fixture found"));
    if (!data.isEmpty())
        fixtureCases(data);
    overflowSeat();
    paths();
    if (failures)
        qCritical("%d failure(s)", failures);
    return failures ? 1 : 0;
}
