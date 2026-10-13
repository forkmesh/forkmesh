#include "LocalControlFleet.h"

#include <QHash>
#include <QHostAddress>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace forkmesh::localcontrol::fleet {

namespace {

constexpr int kMaxDesiredMirrors = 12;

QString lower(const QString &value)
{
    return value.trimmed().toLower();
}

bool validNodeName(const QString &name)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$"));
    return allowed.match(name).hasMatch();
}

QString fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return {};
}

}

QString familyPrefix()
{
    return QStringLiteral("fleet.");
}

QStringList mutatingActions()
{
    return {QStringLiteral("add"),        QStringLiteral("forget"),
            QStringLiteral("deployAll"),  QStringLiteral("uninstall"),
            QStringLiteral("agentClis"),  QStringLiteral("actions"),
            QStringLiteral("linkCode"),   QStringLiteral("resume"),
            QStringLiteral("fleetTarget"), QStringLiteral("reconcile"),
            QStringLiteral("mirrorPush"), QStringLiteral("deleteNode")};
}

bool isMutatingAction(const QString &action)
{
    return mutatingActions().contains(action);
}

QStringList deployModes()
{
    return {QStringLiteral("install"), QStringLiteral("reinstall"),
            QStringLiteral("source")};
}

bool validHostAddress(const QString &address)
{
    const QString text = address.trimmed();
    if (text.isEmpty() || text.size() > 253)
        return false;
    QHostAddress ip;
    if (ip.setAddress(text))
        return true;
    static const QRegularExpression dns(QStringLiteral(
        "^(?=.{1,253}$)([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)"
        "(\\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$"));
    return dns.match(text).hasMatch();
}

bool validSshUser(const QString &user)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[a-z_][a-z0-9_-]{0,31}$"));
    return allowed.match(user).hasMatch();
}

bool validLinkCode(const QString &code)
{
    static const QRegularExpression sixDigits(QStringLiteral("^[0-9]{6}$"));
    return sixDigits.match(code).hasMatch();
}

bool validRepoKey(const QString &key)
{
    static const QRegularExpression allowed(QStringLiteral(
        "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}$"));
    return allowed.match(key).hasMatch() && !key.contains(QLatin1String(".."));
}

bool protectedNode(const QString &node)
{
    const QString key = lower(node);
    return key.isEmpty() || key == QLatin1String("forkmesh") ||
           key == QLatin1String("forkmesh-mainnode");
}

QString subjectFor(const QString &action, const QJsonObject &params,
                   QString *error)
{
    const QString host = params.value(QStringLiteral("host")).toString().trimmed();
    if (!isMutatingAction(action))
        return fail(error, QStringLiteral("unsupported_action"));
    if (action == QLatin1String("add")) {
        const QString address =
            params.value(QStringLiteral("address")).toString().trimmed();
        const QString user = params.value(QStringLiteral("user")).toString().trimmed();
        if (!validNodeName(host))
            return fail(error, QStringLiteral("bad_host"));
        if (!validHostAddress(address))
            return fail(error, QStringLiteral("bad_address"));
        if (!validSshUser(user))
            return fail(error, QStringLiteral("bad_user"));
        return lower(host) + QLatin1Char('|') + address.toLower() +
               QLatin1Char('|') + user;
    }
    if (action == QLatin1String("forget") || action == QLatin1String("uninstall") ||
        action == QLatin1String("agentClis") ||
        action == QLatin1String("deleteNode")) {
        if (!validNodeName(host))
            return fail(error, QStringLiteral("bad_host"));
        if (action == QLatin1String("deleteNode") && protectedNode(host))
            return fail(error, QStringLiteral("protected_node"));
        return lower(host);
    }
    if (action == QLatin1String("actions")) {
        if (!validNodeName(host))
            return fail(error, QStringLiteral("bad_host"));
        const QJsonValue enabled = params.value(QStringLiteral("enabled"));
        if (!enabled.isBool())
            return fail(error, QStringLiteral("bad_params"));
        return lower(host) + (enabled.toBool() ? QStringLiteral("|on")
                                               : QStringLiteral("|off"));
    }
    if (action == QLatin1String("deployAll")) {
        const QString mode = params.value(QStringLiteral("mode")).toString();
        if (!deployModes().contains(mode))
            return fail(error, QStringLiteral("bad_mode"));
        return mode;
    }
    if (action == QLatin1String("linkCode")) {
        const QString code = params.value(QStringLiteral("code")).toString().trimmed();
        if (!validLinkCode(code))
            return fail(error, QStringLiteral("bad_code"));
        return code;
    }
    if (action == QLatin1String("fleetTarget")) {
        const QJsonValue enabled = params.value(QStringLiteral("enabled"));
        const int desired = params.value(QStringLiteral("desired")).toInt(-1);
        if (!enabled.isBool() || desired < 1 || desired > kMaxDesiredMirrors)
            return fail(error, QStringLiteral("bad_params"));
        return (enabled.toBool() ? QStringLiteral("on:") : QStringLiteral("off:")) +
               QString::number(desired);
    }
    if (action == QLatin1String("mirrorPush")) {
        const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
        if (!validRepoKey(repo))
            return fail(error, QStringLiteral("bad_repo"));
        return repo.toLower();
    }
    return action;
}

QString typedConfirmation(const QString &action, const QString &subject)
{
    if (action == QLatin1String("uninstall"))
        return subject;
    if (action == QLatin1String("deleteNode"))
        return QStringLiteral("DELETE ") + subject;
    if (action == QLatin1String("deployAll") && subject == QLatin1String("reinstall"))
        return QStringLiteral("REINSTALL ALL");
    return {};
}

bool typedConfirmationMatches(const QString &typed, const QString &required)
{
    return required.isEmpty() || typed.trimmed() == required;
}

QString actionSummary(const QString &action, const QString &subject)
{
    const QString host = subject.section(QLatin1Char('|'), 0, 0);
    if (action == QLatin1String("add"))
        return QStringLiteral("Save \"%1\" (%2@%3) as a host on the desktop. It is "
                              "reached with the shared ForkMesh SSH key; if it asks "
                              "for a password, the desktop prompts for it there.")
            .arg(host, subject.section(QLatin1Char('|'), 2, 2),
                 subject.section(QLatin1Char('|'), 1, 1));
    if (action == QLatin1String("forget"))
        return QStringLiteral("Remove \"%1\" from the desktop's saved hosts. The "
                              "server itself keeps running and is not destroyed.")
            .arg(host);
    if (action == QLatin1String("deployAll")) {
        if (subject == QLatin1String("reinstall"))
            return QStringLiteral(
                "REMOVE ForkMesh and ALL of its data (identity key, mirrors, chat) "
                "from every saved host, then install it fresh. This cannot be "
                "undone.");
        if (subject == QLatin1String("source"))
            return QStringLiteral("On every saved host: pull the latest ForkMesh "
                                  "source, rebuild it and restart the node, all in "
                                  "parallel.");
        return QStringLiteral("Install the latest published ForkMesh on every "
                              "active saved host, in parallel.");
    }
    if (action == QLatin1String("uninstall"))
        return QStringLiteral("Uninstall ForkMesh from \"%1\": its service, identity "
                              "key and repositories are removed. The server keeps "
                              "running.")
            .arg(host);
    if (action == QLatin1String("agentClis"))
        return QStringLiteral("Install the Claude Code and Codex command-line tools "
                              "on \"%1\". No sign-in is copied; sign in on the host "
                              "afterwards.")
            .arg(host);
    if (action == QLatin1String("actions"))
        return subject.endsWith(QLatin1String("|on"))
                   ? QStringLiteral("Turn Actions on for \"%1\": the mirror starts "
                                    "running workflow jobs. Its variables are left "
                                    "as they are.")
                         .arg(host)
                   : QStringLiteral("Turn Actions off for \"%1\": the mirror stops "
                                    "taking workflow jobs.")
                         .arg(host);
    if (action == QLatin1String("linkCode"))
        return QStringLiteral("Link the node that shows code %1 to your account.")
            .arg(subject);
    if (action == QLatin1String("resume"))
        return QStringLiteral("Resume the paused Vultr deployment where it stopped. "
                              "No second server is created.");
    if (action == QLatin1String("fleetTarget")) {
        const bool on = subject.startsWith(QLatin1String("on:"));
        const QString count = subject.section(QLatin1Char(':'), 1, 1);
        return on ? QStringLiteral("Keep %1 healthy mirror(s) running. The desktop "
                                   "creates or destroys Vultr servers to match, "
                                   "which changes your Vultr bill.")
                        .arg(count)
                  : QStringLiteral("Stop managing the mirror fleet size. Existing "
                                   "servers are left alone.");
    }
    if (action == QLatin1String("reconcile"))
        return QStringLiteral("Check the fleet's health now and create or destroy "
                              "servers to meet the target.");
    if (action == QLatin1String("mirrorPush"))
        return QStringLiteral("Push %1 to every SSH mirror remote and managed host. "
                              "Branches are never rewound or deleted.")
            .arg(subject);
    if (action == QLatin1String("deleteNode"))
        return QStringLiteral("Permanently DELETE the node \"%1\" everywhere: its "
                              "Vultr server is destroyed, its DNS record removed, "
                              "its account, mirrors and history erased from the "
                              "relay, and the saved host forgotten. This cannot be "
                              "undone.")
            .arg(host);
    return {};
}

QJsonObject costSummary(const QJsonArray &hosts)
{
    int vultr = 0;
    double monthly = 0;
    QMap<QString, QPair<int, double>> byRegion;
    QMap<QString, QPair<int, double>> byPlan;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("provider")).toString().compare(
                QLatin1String("Vultr"), Qt::CaseInsensitive) != 0)
            continue;
        ++vultr;
        const double cost = qMax(0.0, host.value(QStringLiteral("monthlyCost")).toDouble());
        monthly += cost;
        QString region = host.value(QStringLiteral("region")).toString().trimmed();
        QString plan = host.value(QStringLiteral("planType")).toString().trimmed();
        if (region.isEmpty())
            region = QStringLiteral("unknown");
        if (plan.isEmpty())
            plan = QStringLiteral("unknown");
        byRegion[region].first += 1;
        byRegion[region].second += cost;
        byPlan[plan].first += 1;
        byPlan[plan].second += cost;
    }
    const auto rows = [](const QMap<QString, QPair<int, double>> &map,
                         const QString &key) {
        QList<QJsonObject> list;
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            list.append(QJsonObject{{key, it.key()},
                                    {QStringLiteral("count"), it.value().first},
                                    {QStringLiteral("monthlyCost"), it.value().second}});
        std::stable_sort(list.begin(), list.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
                             return a.value(QStringLiteral("monthlyCost")).toDouble() >
                                    b.value(QStringLiteral("monthlyCost")).toDouble();
                         });
        QJsonArray out;
        for (const QJsonObject &row : list.mid(0, 20))
            out.append(row);
        return out;
    };
    return QJsonObject{{QStringLiteral("hosts"), hosts.size()},
                       {QStringLiteral("vultr"), vultr},
                       {QStringLiteral("monthlyCost"), monthly},
                       {QStringLiteral("byRegion"), rows(byRegion, QStringLiteral("region"))},
                       {QStringLiteral("byPlan"), rows(byPlan, QStringLiteral("plan"))}};
}

QStringList provisionStageNames()
{
    return {QStringLiteral("Credentials"),   QStringLiteral("Plan + image"),
            QStringLiteral("Create server"), QStringLiteral("Boot + connect"),
            QStringLiteral("Install"),       QStringLiteral("Live traffic")};
}

QJsonObject provisionStatus(bool active, const QString &node, int stage,
                            const QString &state, const QString &detail,
                            const QString &message, const QString &gap,
                            const QStringList &stageDetails, bool resumable)
{
    const QStringList names = provisionStageNames();
    const int current = qBound(0, stage, int(names.size()));
    const bool failed = state == QLatin1String("failed");
    QJsonArray stages;
    for (int i = 0; i < names.size(); ++i) {
        const int number = i + 1;
        QString status = QStringLiteral("pending");
        if (current > number || (current == number && state == QLatin1String("done")))
            status = QStringLiteral("done");
        else if (current == number)
            status = failed ? QStringLiteral("failed") : QStringLiteral("active");
        stages.append(QJsonObject{
            {QStringLiteral("number"), number},
            {QStringLiteral("name"), names.at(i)},
            {QStringLiteral("status"), status},
            {QStringLiteral("detail"), plainText(stageDetails.value(i), 300)}});
    }
    return QJsonObject{{QStringLiteral("active"), active},
                       {QStringLiteral("node"), node},
                       {QStringLiteral("stage"), current},
                       {QStringLiteral("stageCount"), int(names.size())},
                       {QStringLiteral("state"), state},
                       {QStringLiteral("detail"), plainText(detail, 600)},
                       {QStringLiteral("message"), plainText(message, 600)},
                       {QStringLiteral("gap"), plainText(gap, 300)},
                       {QStringLiteral("resumable"), resumable},
                       {QStringLiteral("stages"), stages}};
}

QString redactSecrets(const QString &line)
{
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral(
            "(?i)((?:api[_-]?key|token|secret|password|passwd|authorization|bearer)"
            "[\"']?\\s*[:=]?\\s*[\"']?)([^\\s\"',;]{6,})")),
        QRegularExpression(QStringLiteral("-----BEGIN [A-Z ]*PRIVATE KEY-----.*")),
        QRegularExpression(QStringLiteral("\\b(sk-[A-Za-z0-9_-]{12,}|gh[pousr]_[A-Za-z0-9]{20,})\\b")),
    };
    QString out = line;
    out.replace(patterns[0], QStringLiteral("\\1[redacted]"));
    out.replace(patterns[1], QStringLiteral("[redacted private key]"));
    out.replace(patterns[2], QStringLiteral("[redacted]"));
    return out;
}

QStringList tailLines(const QByteArray &tail, int maxLines, int maxLineChars)
{
    QStringList lines =
        QString::fromUtf8(tail).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.size() > maxLines)
        lines = lines.mid(lines.size() - maxLines);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        line = redactSecrets(line.left(maxLineChars));
    }
    return lines;
}

QJsonArray gatewayRows(const QStringList &records, qint64 nowMs,
                       qint64 retiredAfterMs, int maxRows)
{
    struct Row {
        QString host;
        qint64 sinceMs = 0;
    };
    QList<Row> rows;
    for (const QString &record : records) {
        const QStringList fields = record.split(QLatin1Char('\t'));
        if (fields.size() < 2)
            continue;
        Row row;
        row.host = fields.at(0).trimmed();
        row.sinceMs = fields.at(1).toLongLong();
        if (row.host.isEmpty() || row.sinceMs <= 0)
            continue;
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row &a, const Row &b) { return a.sinceMs > b.sinceMs; });
    QJsonArray out;
    for (const Row &row : rows.mid(0, maxRows)) {
        const qint64 downFor = qMax<qint64>(0, nowMs - row.sinceMs);
        out.append(QJsonObject{{QStringLiteral("host"), row.host.left(200)},
                               {QStringLiteral("downSinceMs"), double(row.sinceMs)},
                               {QStringLiteral("downForMs"), double(downFor)},
                               {QStringLiteral("retired"), downFor >= retiredAfterMs}});
    }
    return out;
}

QString plainText(const QString &html, int maxChars)
{
    QString text = html;
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    text.replace(tags, QStringLiteral(" "));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text = text.simplified();
    if (text.size() > maxChars)
        text = text.left(maxChars - 1) + QChar(0x2026);
    return text;
}

bool validRemotePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 512 || !path.startsWith(QLatin1Char('/')))
        return false;
    static const QRegularExpression allowed(QStringLiteral("^/[A-Za-z0-9._/@+-]*$"));
    if (!allowed.match(path).hasMatch())
        return false;
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (part == QLatin1String("..") || part == QLatin1String("."))
            return false;
    return true;
}

QString diskUsageCommand(const QString &path)
{
    if (!validRemotePath(path))
        return {};
    return QStringLiteral("sh -c 'du -x -k --max-depth=1 -- \"%1\" 2>/dev/null | "
                          "sort -rn | head -n 80'")
        .arg(path);
}

QJsonArray parseDiskUsage(const QString &output, const QString &path, int maxRows)
{
    QString base = path;
    while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
        base.chop(1);
    struct Row {
        QString name;
        QString fullPath;
        qint64 kb = 0;
        bool self = false;
    };
    QList<Row> rows;
    for (const QString &raw : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        const int tab = line.indexOf(QRegularExpression(QStringLiteral("\\s")));
        if (tab <= 0)
            continue;
        bool ok = false;
        const qint64 kb = line.left(tab).toLongLong(&ok);
        if (!ok || kb < 0)
            continue;
        QString full = line.mid(tab).trimmed();
        while (full.size() > 1 && full.endsWith(QLatin1Char('/')))
            full.chop(1);
        Row row;
        row.kb = kb;
        row.fullPath = full;
        row.self = full == base;
        if (!row.self) {
            const QString prefix = base == QLatin1String("/") ? base : base + QLatin1Char('/');
            if (!full.startsWith(prefix))
                continue;
            row.name = full.mid(prefix.size());
            if (row.name.isEmpty() || row.name.contains(QLatin1Char('/')))
                continue;
        }
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.self != b.self)
            return a.self;
        return a.kb > b.kb;
    });
    QJsonArray out;
    for (const Row &row : rows.mid(0, maxRows + 1))
        out.append(QJsonObject{{QStringLiteral("name"), row.self ? QStringLiteral(".") : row.name},
                               {QStringLiteral("path"), row.fullPath},
                               {QStringLiteral("bytes"), double(row.kb) * 1024.0},
                               {QStringLiteral("total"), row.self}});
    return out;
}

} // namespace forkmesh::localcontrol::fleet
