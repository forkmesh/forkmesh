#include "BugReportsModel.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSysInfo>

namespace forkmesh::bugreports {
namespace {

QString clipped(const QString &value, int maximum)
{
    const QString trimmed = value.trimmed();
    return trimmed.size() > maximum ? trimmed.left(maximum).trimmed() : trimmed;
}

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

qint64 number(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

}

const QList<Severity> &severities()
{
    static const QList<Severity> values{
        {QStringLiteral("low"), QStringLiteral("Minor")},
        {QStringLiteral("normal"), QStringLiteral("Normal")},
        {QStringLiteral("high"), QStringLiteral("Serious")},
        {QStringLiteral("crash"), QStringLiteral("Crash")},
    };
    return values;
}

const QStringList &inboxStatuses()
{
    static const QStringList values{QStringLiteral("new"), QStringLiteral("triaged"),
                                    QStringLiteral("closed")};
    return values;
}

QString inboxStatusLabel(const QString &status)
{
    if (status == QLatin1String("new"))
        return QStringLiteral("New");
    if (status == QLatin1String("triaged"))
        return QStringLiteral("Triaged");
    if (status == QLatin1String("closed"))
        return QStringLiteral("Closed");
    return status.isEmpty() ? QStringLiteral("New") : status.left(1).toUpper() + status.mid(1);
}

QString mineStatusLabel(const BugReport &report)
{
    if (report.confirmedFixedAt > 0)
        return QStringLiteral("Fixed, confirmed by you");
    if (report.status == QLatin1String("new"))
        return QStringLiteral("Received");
    if (report.status == QLatin1String("triaged"))
        return QStringLiteral("In progress");
    return inboxStatusLabel(report.status);
}

QString statusTone(const QString &status)
{
    if (status == QLatin1String("new"))
        return QStringLiteral("accent");
    if (status == QLatin1String("triaged"))
        return QStringLiteral("warning");
    return QStringLiteral("neutral");
}

QString detailsLine(const BugReport &report)
{
    QStringList parts;
    for (const QString &part : {report.platform, report.appVersion, report.screen}) {
        if (!part.isEmpty())
            parts.append(part);
    }
    if (report.attachments > 0)
        parts.append(report.attachments == 1
                         ? QStringLiteral("1 attachment")
                         : QStringLiteral("%1 attachments").arg(report.attachments));
    return parts.join(QStringLiteral(" · "));
}

bool canConfirmFixed(const BugReport &report)
{
    return report.confirmedFixedAt <= 0;
}

BugReport parseReport(const QJsonObject &object)
{
    BugReport report;
    report.id = text(object, "id");
    report.status = text(object, "status").toLower();
    if (report.status.isEmpty())
        report.status = QStringLiteral("new");
    report.severity = text(object, "severity");
    report.source = text(object, "source");
    report.platform = text(object, "platform");
    report.appVersion = text(object, "appVersion");
    report.osVersion = text(object, "osVersion");
    report.device = text(object, "device");
    report.account = text(object, "account");
    report.contact = text(object, "contact");
    report.message = object.value(QStringLiteral("message")).toString().trimmed();
    report.screen = text(object, "screen");
    report.package = text(object, "package");
    report.createdAt = number(object, "createdAt");
    report.confirmedFixedAt = number(object, "confirmedFixedAt");
    report.attachments = object.value(QStringLiteral("attachments")).toArray().size();
    return report;
}

StatusCounts parseCounts(const QJsonObject &object)
{
    StatusCounts counts;
    counts.fresh = object.value(QStringLiteral("new")).toInt();
    counts.triaged = object.value(QStringLiteral("triaged")).toInt();
    counts.closed = object.value(QStringLiteral("closed")).toInt();
    return counts;
}

ReportList parseReportList(const QJsonObject &payload)
{
    ReportList list;
    for (const QJsonValue &value : payload.value(QStringLiteral("reports")).toArray()) {
        const BugReport report = parseReport(value.toObject());
        if (!report.id.isEmpty())
            list.reports.append(report);
    }
    list.counts = parseCounts(payload.value(QStringLiteral("counts")).toObject());
    list.more = !payload.value(QStringLiteral("nextOffset")).isNull() &&
                !payload.value(QStringLiteral("nextOffset")).isUndefined();
    return list;
}

bool messageLongEnough(const QString &message)
{
    return message.trimmed().size() >= kMinMessage;
}

QString desktopPlatform()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

QJsonObject reportPayload(const Draft &draft)
{
    QString severity = draft.severity;
    bool known = false;
    for (const Severity &option : severities())
        known = known || option.value == severity;
    if (!known)
        severity = QStringLiteral("normal");
    QJsonObject payload{
        {QStringLiteral("message"), clipped(draft.message, kMaxMessage)},
        {QStringLiteral("severity"), severity},
        {QStringLiteral("source"), QStringLiteral("desktop")},
        {QStringLiteral("platform"), desktopPlatform()},
        {QStringLiteral("screen"), QStringLiteral("Bugs")},
        {QStringLiteral("osVersion"), clipped(QSysInfo::prettyProductName(), 60)},
        {QStringLiteral("device"), clipped(QSysInfo::currentCpuArchitecture(), 120)},
    };
    const QString contact = clipped(draft.contact, kMaxContact);
    if (!contact.isEmpty())
        payload.insert(QStringLiteral("contact"), contact);
    const QString version = clipped(draft.appVersion, 60);
    if (!version.isEmpty())
        payload.insert(QStringLiteral("appVersion"), version);
    return payload;
}

QJsonObject statusPayload(const QString &reportId, const QString &status)
{
    return {{QStringLiteral("report"), reportId}, {QStringLiteral("status"), status}};
}

QJsonObject confirmFixedPayload(const QString &reportId)
{
    return {{QStringLiteral("report"), reportId}, {QStringLiteral("fixed"), true}};
}

bool signedOutError(const QString &error)
{
    const QString value = error.trimmed().toLower();
    return value == QLatin1String("sign in required") || value == QLatin1String("http 401") ||
           value.startsWith(QLatin1String("sign in to your forkmesh account"));
}

bool adminRequiredError(const QString &error)
{
    const QString value = error.trimmed().toLower();
    return value == QLatin1String("admin required") || value == QLatin1String("http 403");
}

QString sendErrorText(const QString &error)
{
    const QString value = error.trimmed().toLower();
    if (value == QLatin1String("rate limited"))
        return QStringLiteral("Too many reports from here in the last hour. Try again later.");
    if (value == QLatin1String("empty report"))
        return QStringLiteral("Write a few words about what went wrong.");
    if (signedOutError(error))
        return QStringLiteral("Sign in to your ForkMesh account to send a bug report.");
    return QStringLiteral("The report could not be sent: %1").arg(error.trimmed());
}

QString loadErrorText(const QString &error)
{
    if (signedOutError(error))
        return QStringLiteral("Sign in to your ForkMesh account to see your reports.");
    return QStringLiteral("Reports could not be loaded: %1").arg(error.trimmed());
}

}
