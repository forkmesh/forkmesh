#include "AppStoreStatusModel.h"

#include <QDate>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>

namespace forkmesh::appstorestatus {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

QString joined(const QStringList &parts)
{
    QStringList kept;
    for (const QString &part : parts) {
        if (!part.isEmpty())
            kept.append(part);
    }
    return kept.join(QStringLiteral(" · "));
}

QDateTime fromEpochNumber(double value)
{
    if (value <= 0)
        return {};
    const qint64 milliseconds = value >= 1e11 ? qint64(value) : qint64(value * 1000);
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::UTC);
}

}

Tone toneOf(const QJsonObject &status)
{
    const QString tone = text(status, "tone");
    if (tone == QLatin1String("action"))
        return Tone::Action;
    if (tone == QLatin1String("pending"))
        return Tone::Pending;
    if (tone == QLatin1String("good"))
        return Tone::Good;
    return Tone::Neutral;
}

QString toneName(Tone tone)
{
    switch (tone) {
    case Tone::Action:
        return QStringLiteral("danger");
    case Tone::Pending:
        return QStringLiteral("warning");
    case Tone::Good:
        return QStringLiteral("success");
    case Tone::Neutral:
        break;
    }
    return QStringLiteral("neutral");
}

QString humanizeState(const QString &state)
{
    QString words = state.trimmed().toLower();
    words.replace(QLatin1Char('_'), QLatin1Char(' '));
    words = words.simplified();
    if (words.isEmpty())
        return {};
    return words.left(1).toUpper() + words.mid(1);
}

QString statusLabel(const QJsonObject &status)
{
    const QString label = text(status, "label");
    if (!label.isEmpty())
        return label;
    const QString state = humanizeState(text(status, "state"));
    return state.isEmpty() ? QStringLiteral("Unknown") : state;
}

QDateTime parseTimestamp(const QJsonValue &value)
{
    if (value.isDouble())
        return fromEpochNumber(value.toDouble());
    const QString raw = value.toString().trimmed();
    if (raw.isEmpty())
        return {};
    static const QRegularExpression digits(QStringLiteral("^\\d{9,16}$"));
    if (digits.match(raw).hasMatch())
        return fromEpochNumber(raw.toDouble());
    QDateTime parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(raw, Qt::ISODate);
    return parsed;
}

QString relativeTime(const QDateTime &then, const QDateTime &now)
{
    if (!then.isValid() || !now.isValid())
        return {};
    const qint64 seconds = then.secsTo(now);
    if (seconds < 60)
        return QStringLiteral("just now");
    if (seconds < 3600) {
        const qint64 minutes = seconds / 60;
        return countText(minutes, QStringLiteral("minute"), QStringLiteral("minutes")) +
               QStringLiteral(" ago");
    }
    if (seconds < 86400) {
        const qint64 hours = seconds / 3600;
        return countText(hours, QStringLiteral("hour"), QStringLiteral("hours")) +
               QStringLiteral(" ago");
    }
    const qint64 days = then.toLocalTime().date().daysTo(now.toLocalTime().date());
    if (days <= 1)
        return QStringLiteral("yesterday");
    if (days < 30)
        return QStringLiteral("%1 days ago").arg(days);
    return QStringLiteral("on ") +
           QLocale().toString(then.toLocalTime().date(), QStringLiteral("d MMM yyyy"));
}

QString publishedText(const QJsonValue &publishedAt, const QDateTime &now)
{
    const QDateTime then = parseTimestamp(publishedAt);
    if (!then.isValid())
        return {};
    return QStringLiteral("Published ") + relativeTime(then, now);
}

QString dateText(const QJsonValue &value)
{
    const QDateTime parsed = parseTimestamp(value);
    if (!parsed.isValid())
        return {};
    const QDateTime local = parsed.toLocalTime();
    return QLocale().toString(local.date(), QStringLiteral("d MMM yyyy")) + QLatin1Char(' ') +
           QLocale().toString(local.time(), QLocale::ShortFormat);
}

QJsonObject snapshotOf(const QJsonObject &payload)
{
    const QJsonValue snapshot = payload.value(QStringLiteral("snapshot"));
    return snapshot.isObject() ? snapshot.toObject() : QJsonObject();
}

bool exportHasData(const QJsonObject &payload)
{
    const QJsonObject bundle = payload.value(QStringLiteral("export")).toObject();
    if (bundle.contains(QStringLiteral("snapshot"))) {
        const QJsonValue snapshot = bundle.value(QStringLiteral("snapshot"));
        return snapshot.isObject() && !snapshot.toObject().isEmpty();
    }
    return !bundle.isEmpty();
}

QString versionTitle(const QString &version, const QString &build)
{
    const QString name = version.trimmed().isEmpty() ? QStringLiteral("Unnamed version")
                                                     : version.trimmed();
    if (build.trimmed().isEmpty())
        return name;
    return QStringLiteral("%1 (%2)").arg(name, build.trimmed());
}

QString releaseTypeLabel(const QString &releaseType)
{
    const QString type = releaseType.trimmed();
    if (type == QLatin1String("AFTER_APPROVAL"))
        return QStringLiteral("Releases after approval");
    if (type == QLatin1String("MANUAL"))
        return QStringLiteral("Manual release");
    if (type == QLatin1String("SCHEDULED"))
        return QStringLiteral("Scheduled release");
    return humanizeState(type);
}

QString versionDetail(const QJsonObject &version)
{
    const QString created = dateText(version.value(QStringLiteral("createdAt")));
    return joined({releaseTypeLabel(text(version, "releaseType")),
                   created.isEmpty() ? QString() : QStringLiteral("Created ") + created});
}

QString buildDetail(const QJsonObject &build)
{
    const QString uploaded = dateText(build.value(QStringLiteral("uploadedAt")));
    QStringList groups;
    for (const QJsonValue &group : build.value(QStringLiteral("groups")).toArray()) {
        const QString name = group.toString().trimmed();
        if (!name.isEmpty())
            groups.append(name);
    }
    return joined({uploaded.isEmpty() ? QString() : QStringLiteral("Uploaded ") + uploaded,
                   groups.isEmpty() ? QString()
                                    : QStringLiteral("Groups: ") + groups.join(QStringLiteral(", "))});
}

QString submissionTitle(const QJsonObject &submission)
{
    const QString submitted = dateText(submission.value(QStringLiteral("submittedAt")));
    return submitted.isEmpty() ? QStringLiteral("Not submitted yet")
                               : QStringLiteral("Submitted ") + submitted;
}

QString countText(qint64 count, const QString &singular, const QString &plural)
{
    return QLocale().toString(count) + QLatin1Char(' ') + (count == 1 ? singular : plural);
}

QString spanLabel(int days)
{
    if (days <= 1)
        return QStringLiteral("Last day");
    return QStringLiteral("Last %1 days").arg(days);
}

QList<Figures> downloadFigures(const QJsonObject &downloads)
{
    QList<Figures> rows;
    if (!downloads.value(QStringLiteral("available")).toBool())
        return rows;
    const QJsonObject total = downloads.value(QStringLiteral("total")).toObject();
    const QString since = text(downloads, "since");
    const QDate month = QDate::fromString(since + QStringLiteral("-01"), Qt::ISODate);
    Figures all;
    all.label = month.isValid() ? QStringLiteral("Since ") +
                                      QLocale().toString(month, QStringLiteral("MMM yyyy"))
                : since.isEmpty() ? QStringLiteral("All time")
                                  : QStringLiteral("Since ") + since;
    all.downloads = integer(total, "downloads");
    all.redownloads = integer(total, "redownloads");
    all.updates = integer(total, "updates");
    rows.append(all);
    for (const QJsonValue &value : downloads.value(QStringLiteral("recent")).toArray()) {
        const QJsonObject span = value.toObject();
        Figures row;
        row.label = spanLabel(int(integer(span, "days")));
        row.downloads = integer(span, "downloads");
        row.redownloads = integer(span, "redownloads");
        row.updates = integer(span, "updates");
        rows.append(row);
    }
    return rows;
}

QJsonArray sortedGroups(const QJsonArray &groups)
{
    QList<QJsonObject> ordered;
    for (const QJsonValue &value : groups)
        ordered.append(value.toObject());
    std::stable_partition(ordered.begin(), ordered.end(), [](const QJsonObject &group) {
        return group.value(QStringLiteral("internal")).toBool();
    });
    QJsonArray result;
    for (const QJsonObject &group : ordered)
        result.append(group);
    return result;
}

QString groupDetail(const QJsonObject &group)
{
    const bool internal = group.value(QStringLiteral("internal")).toBool();
    QString link;
    if (group.value(QStringLiteral("publicLinkEnabled")).toBool()) {
        const QString url = text(group, "publicLink");
        link = url.isEmpty() ? QStringLiteral("public link on") : QStringLiteral("public link ") + url;
        const qint64 limit = integer(group, "publicLinkLimit");
        if (limit > 0)
            link += QStringLiteral(" (limit %1)").arg(QLocale().toString(limit));
    } else if (!internal) {
        link = QStringLiteral("no public link");
    }
    return joined({countText(integer(group, "testers"), QStringLiteral("tester"),
                             QStringLiteral("testers")),
                   group.value(QStringLiteral("allBuilds")).toBool() ? QStringLiteral("every build")
                                                                     : QStringLiteral("chosen builds"),
                   link,
                   group.value(QStringLiteral("feedbackEnabled")).toBool()
                       ? QStringLiteral("feedback on")
                       : QStringLiteral("feedback off")});
}

QString feedbackTitle(const QJsonObject &feedback)
{
    const QString kind = text(feedback, "kind");
    const QString label = kind == QLatin1String("screenshot") ? QStringLiteral("Screenshot feedback")
                          : kind == QLatin1String("crash")    ? QStringLiteral("Crash report")
                                                              : QStringLiteral("Feedback");
    const QString from = text(feedback, "from");
    return from.isEmpty() ? label : QStringLiteral("%1 from %2").arg(label, from);
}

QString feedbackDetail(const QJsonObject &feedback)
{
    return joined({text(feedback, "device"), dateText(feedback.value(QStringLiteral("createdAt")))});
}

QList<Screenshot> screenshotsOf(const QJsonObject &feedback)
{
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{24}$"));
    QList<Screenshot> found;
    for (const QJsonValue &value : feedback.value(QStringLiteral("screenshots")).toArray()) {
        const QJsonObject shot = value.toObject();
        const QString id = text(shot, "id");
        if (!idPattern.match(id).hasMatch())
            continue;
        found.append({id, int(std::clamp<qint64>(integer(shot, "width"), 0, 20000)),
                      int(std::clamp<qint64>(integer(shot, "height"), 0, 20000))});
        if (found.size() >= 4)
            break;
    }
    return found;
}

QString screenshotPath(const QString &id)
{
    return QStringLiteral("/api/app-store-status/screenshots/") + id;
}

QByteArray screenshotBytes(const QJsonObject &payload)
{
    const QString mime = text(payload, "mime");
    const QByteArray raw = QByteArray::fromBase64(payload.value(QStringLiteral("data")).toString().toLatin1(),
                                                  QByteArray::AbortOnBase64DecodingErrors);
    if (mime == QLatin1String("image/jpeg") && raw.startsWith("\xff\xd8\xff"))
        return raw;
    if (mime == QLatin1String("image/png") && raw.startsWith("\x89PNG\r\n\x1a\n"))
        return raw;
    return {};
}

QString stars(int rating)
{
    const int filled = std::clamp(rating, 0, 5);
    return QString(filled, QChar(0x2605)) + QString(5 - filled, QChar(0x2606));
}

QString reviewTitle(const QJsonObject &review)
{
    const QString title = text(review, "title");
    return title.isEmpty() ? QStringLiteral("Untitled review") : title;
}

QString reviewDetail(const QJsonObject &review)
{
    return joined({text(review, "territory"), dateText(review.value(QStringLiteral("createdAt")))});
}

QString sectionTitle(const QString &key)
{
    if (key == QLatin1String("versions"))
        return QStringLiteral("App Store versions");
    if (key == QLatin1String("submissions"))
        return QStringLiteral("App Review submissions");
    if (key == QLatin1String("downloads"))
        return QStringLiteral("Downloads");
    if (key == QLatin1String("builds"))
        return QStringLiteral("TestFlight builds");
    if (key == QLatin1String("groups"))
        return QStringLiteral("TestFlight groups");
    if (key == QLatin1String("feedback"))
        return QStringLiteral("Tester feedback");
    if (key == QLatin1String("reviews"))
        return QStringLiteral("Customer reviews");
    return humanizeState(key);
}

QString unavailableText(const QJsonObject &snapshot, const QString &key)
{
    return snapshot.value(QStringLiteral("unavailable")).toObject().value(key).toString().trimmed();
}

QStringList attentionLines(const QJsonObject &snapshot)
{
    QStringList lines;
    for (const QJsonValue &value : snapshot.value(QStringLiteral("attention")).toArray()) {
        const QString line = value.toString().trimmed();
        if (!line.isEmpty())
            lines.append(line);
    }
    return lines;
}

}
