#include "CoreNodeLogs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace forkmesh {
namespace corelogs {

namespace {

constexpr int kMaximumMessageChars = 2000;
constexpr int kLegacySshPort = 22;

struct Palette {
    QString text;
    QString muted;
    QString rule;
    QString headerBg;
    QString track;
    QString ok;
    QString warn;
    QString danger;
    QString link;
};

Palette palette(bool dark)
{
    Palette p;
    p.text = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    p.muted = dark ? QStringLiteral("#8b949e") : QStringLiteral("#57606a");
    p.rule = dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de");
    p.headerBg = dark ? QStringLiteral("#161b22") : QStringLiteral("#f6f8fa");
    p.track = dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2");
    p.ok = dark ? QStringLiteral("#3fb950") : QStringLiteral("#1a7f37");
    p.warn = dark ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
    p.danger = dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
    p.link = dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
    return p;
}

QString sectionTitle(const QString &title, const QString &subtitle,
                     const Palette &p)
{
    QString html = QStringLiteral(
                       "<p style='margin-top:14px;margin-bottom:4px;'>"
                       "<b style='color:%1;'>%2</b>")
                       .arg(p.text, title.toHtmlEscaped());
    if (!subtitle.isEmpty())
        html += QString::fromUtf8(
                    " <span style='color:%1;font-size:11px;'>\xC2\xB7 %2</span>")
                    .arg(p.muted, subtitle.toHtmlEscaped());
    return html + QStringLiteral("</p>");
}

QString notice(const QString &text, const Palette &p,
               const QString &colour = QString())
{
    return QStringLiteral("<p style='color:%1;font-size:11px;'>%2</p>")
        .arg(colour.isEmpty() ? p.muted : colour, text.toHtmlEscaped());
}

QString headerRow(const QStringList &columns, const Palette &p)
{
    QString html = QStringLiteral("<tr>");
    for (int index = 0; index < columns.size(); ++index)
        html += QStringLiteral(
                    "<td bgcolor='%1' style='padding:4px 6px;color:%2;"
                    "font-size:10px;'%3><b>%4</b></td>")
                    .arg(p.headerBg, p.muted,
                         index == 0 ? QString()
                                    : QStringLiteral(" align='right'"),
                         columns.at(index).toHtmlEscaped());
    return html + QStringLiteral("</tr>");
}

QString cell(const QString &value, const Palette &p, bool first = false,
             const QString &colour = QString())
{
    return QStringLiteral(
               "<td style='padding:3px 6px;border-top:1px solid %1;color:%2;"
               "font-size:11px;'%3>%4</td>")
        .arg(p.rule, colour.isEmpty() ? p.text : colour,
             first ? QString() : QStringLiteral(" align='right'"), value);
}

QString countCell(qint64 value, const Palette &p,
                  const QString &nonZeroColour = QString())
{
    if (value <= 0)
        return cell(QString::fromUtf8("\xE2\x80\x94"), p, false, p.muted);
    return cell(QString::number(value), p, false, nonZeroColour);
}

QString ago(qint64 thenMs, qint64 nowMs)
{
    if (thenMs <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    const qint64 seconds = qMax<qint64>(0, (nowMs - thenMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s ago").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m ago").arg(seconds / 60);
    return QStringLiteral("%1h ago").arg(seconds / 3600);
}

QString nodeColour(int index)
{
    static const char *const kColours[] = {"#58a6ff", "#f778ba", "#3fb950",
                                           "#d29922", "#a371f7", "#39c5cf"};
    return QString::fromLatin1(kColours[index % int(std::size(kColours))]);
}

QString truncated(const QString &text)
{
    if (text.size() <= kMaximumMessageChars)
        return text;
    return text.left(kMaximumMessageChars - 3) + QStringLiteral("...");
}

} // namespace

namespace {

QString projectsRoot()
{
    const QString configured =
        qEnvironmentVariable("FORKMESH_PROJECTS_DIR").trimmed();
    if (!configured.isEmpty())
        return QDir::cleanPath(configured);
    return QDir::homePath() + QStringLiteral("/projects");
}

QString envFileValue(const QString &path, const QString &name)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QList<QByteArray> lines = file.read(64 * 1024).split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.startsWith("export "))
            line = line.mid(7).trimmed();
        const int equals = line.indexOf('=');
        if (line.isEmpty() || line.startsWith('#') || equals <= 0)
            continue;
        if (QString::fromUtf8(line.left(equals).trimmed()) != name)
            continue;
        QByteArray value = line.mid(equals + 1).trimmed();
        if (value.size() >= 2 && value.front() == value.back() &&
            (value.front() == '"' || value.front() == '\''))
            value = value.mid(1, value.size() - 2);
        return QString::fromUtf8(value);
    }
    return QString();
}

bool validAddress(const QString &ip)
{
    static const QRegularExpression address(
        QStringLiteral("^[0-9A-Fa-f:.]{2,45}$"));
    return address.match(ip).hasMatch() && ip != QLatin1String("0.0.0.0");
}

bool nodeFromLabel(const QString &label, CoreNode *node, int *order)
{
    static const QRegularExpression pattern(
        QStringLiteral("^fmnext-(core|auto|run|burst)-([0-9]{1,4})$"));
    const QRegularExpressionMatch match = pattern.match(label);
    if (!match.hasMatch())
        return false;
    const QString kind = match.captured(1);
    const int number = match.captured(2).toInt();
    int rank = 0;
    QString prefix = QStringLiteral("n");
    QString role = QStringLiteral("core");
    if (kind == QLatin1String("auto")) {
        rank = 1;
        prefix = QStringLiteral("a");
    } else if (kind == QLatin1String("run")) {
        rank = 2;
        prefix = QStringLiteral("r");
        role = QStringLiteral("runner");
    } else if (kind == QLatin1String("burst")) {
        rank = 3;
        prefix = QStringLiteral("b");
        role = QStringLiteral("runner");
    }
    node->name = prefix + QString::number(number);
    node->role = role;
    node->label = label;
    *order = rank * 100000 + number;
    return true;
}

QList<CoreNode> sortedNodes(QList<QPair<int, CoreNode>> found)
{
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QList<CoreNode> nodes;
    for (const auto &entry : std::as_const(found))
        nodes.append(entry.second);
    return nodes;
}

QString shortUnit(const QString &unit)
{
    static const QRegularExpression session(
        QStringLiteral("^session-[0-9]+\\.scope$"));
    static const QRegularExpression job(
        QStringLiteral("^fmnext-job-.*\\.scope$"));
    static const QRegularExpression run(QStringLiteral("^run-.*\\.scope$"));
    if (session.match(unit).hasMatch())
        return QStringLiteral("session");
    if (job.match(unit).hasMatch())
        return QStringLiteral("job");
    if (run.match(unit).hasMatch())
        return QStringLiteral("run");
    if (unit == QLatin1String("init.scope"))
        return QStringLiteral("systemd");
    QString label = unit;
    if (label.endsWith(QLatin1String(".service")))
        label.chop(8);
    return label;
}

QString compactJsonLine(const QJsonObject &object)
{
    QStringList parts;
    const QString logger = object.value(QStringLiteral("logger")).toString();
    const QString msg = object.value(QStringLiteral("msg")).toString();
    if (!logger.isEmpty())
        parts.append(logger + QLatin1Char(':'));
    if (!msg.isEmpty())
        parts.append(msg);
    for (const char *key : {"error", "err", "stacktrace", "url", "host",
                            "upstream", "server_name", "identifiers"}) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            continue;
        const QString text =
            value.isString()
                ? value.toString()
                : QString::fromUtf8(
                      QJsonDocument(value.isArray()
                                        ? QJsonDocument(value.toArray())
                                        : QJsonDocument(value.toObject()))
                          .toJson(QJsonDocument::Compact));
        if (!text.isEmpty())
            parts.append(QStringLiteral("%1=%2").arg(QLatin1String(key), text));
    }
    return parts.join(QLatin1Char(' '));
}

QString jsonFields(const QJsonObject &object, const QStringList &skip,
                   const QString &summary = QString());

QString jsonValueText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == std::floor(number) && std::abs(number) < 1e15)
            return QString::number(qint64(number));
        return QString::number(number);
    }
    if (value.isArray()) {
        QStringList items;
        for (const QJsonValue &item : value.toArray()) {
            const QString text = jsonValueText(item);
            if (!text.isEmpty())
                items.append(text);
        }
        return items.join(QStringLiteral(", "));
    }
    if (value.isObject()) {
        const QString fields = jsonFields(value.toObject(), {});
        return fields.isEmpty() ? QString()
                                : QLatin1Char('(') + fields + QLatin1Char(')');
    }
    return QString();
}

bool mentionedIn(const QString &summary, const QString &text)
{
    if (summary.isEmpty() || text.isEmpty())
        return false;
    const QRegularExpression word(
        QStringLiteral("(^|[^A-Za-z0-9_])%1($|[^A-Za-z0-9_])")
            .arg(QRegularExpression::escape(text)));
    return word.match(summary).hasMatch();
}

QString jsonFields(const QJsonObject &object, const QStringList &skip,
                   const QString &summary)
{
    QStringList parts;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (skip.contains(it.key()))
            continue;
        const QJsonValue value = it.value();
        const QString text = jsonValueText(value);
        if (text.isEmpty())
            continue;
        if (!value.isArray() && !value.isObject() && mentionedIn(summary, text))
            continue;
        parts.append(it.key() + QLatin1Char('=') + text);
    }
    return parts.join(QLatin1Char(' '));
}

QString byteSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f',
                                        1);
}

QString requestRecordLine(const QJsonObject &object)
{
    const QString operation = object.value(QStringLiteral("operation")).toString();
    const QJsonValue status = object.value(QStringLiteral("status"));
    if (operation.isEmpty() || !status.isDouble())
        return {};
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    QString text = operation + QLatin1Char(' ') + QString::number(status.toInt());
    const QJsonValue duration = object.value(QStringLiteral("durationMs"));
    if (duration.isDouble())
        text += dot + QString::number(qint64(duration.toDouble())) +
                QStringLiteral(" ms");
    const QJsonValue bytes = object.value(QStringLiteral("bytes"));
    if (bytes.isDouble())
        text += dot + byteSize(qint64(bytes.toDouble()));
    const QString request = object.value(QStringLiteral("requestId")).toString();
    if (!request.isEmpty())
        text += dot + QStringLiteral("request ") + request;
    const QString error = object.value(QStringLiteral("error")).toString();
    if (!error.isEmpty())
        text += dot + QStringLiteral("error=") + error;
    return text;
}

QString idsJsonLine(const QJsonObject &object)
{
    const QString kind = object.value(QStringLiteral("ids")).toString();
    if (kind != QLatin1String("incident")) {
        const QString fields = jsonFields(object, {QStringLiteral("ids")});
        return fields.isEmpty() ? kind : kind + QLatin1Char(' ') + fields;
    }
    const QString severity = object.value(QStringLiteral("severity")).toString();
    const QString detector = object.value(QStringLiteral("detector")).toString();
    const QString summary = object.value(QStringLiteral("summary")).toString();
    QStringList head;
    if (!severity.isEmpty())
        head.append(severity);
    if (!detector.isEmpty())
        head.append(detector + QLatin1Char(':'));
    if (!summary.isEmpty())
        head.append(summary);
    QStringList parts{head.join(QLatin1Char(' '))};
    const QString detail = jsonFields(
        object.value(QStringLiteral("detail")).toObject(), {}, summary);
    if (!detail.isEmpty())
        parts.append(detail);
    const QString rest = jsonFields(
        object, {QStringLiteral("ids"), QStringLiteral("severity"),
                 QStringLiteral("detector"), QStringLiteral("summary"),
                 QStringLiteral("detail"), QStringLiteral("node"),
                 QStringLiteral("id")});
    if (!rest.isEmpty())
        parts.append(rest);
    const QString id = object.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        parts.append(QStringLiteral("incident ") + id);
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

QString genericJsonLine(const QJsonObject &object)
{
    static const QStringList kLead = {
        QStringLiteral("level"), QStringLiteral("logger"),
        QStringLiteral("message"), QStringLiteral("event")};
    static const QStringList kSkip = {
        QStringLiteral("level"), QStringLiteral("logger"),
        QStringLiteral("message"), QStringLiteral("event"),
        QStringLiteral("ts"), QStringLiteral("time"),
        QStringLiteral("timestamp")};
    QStringList parts;
    for (const QString &key : kLead) {
        const QString text = jsonValueText(object.value(key));
        if (text.isEmpty())
            continue;
        parts.append(key == QLatin1String("logger") ? text + QLatin1Char(':')
                                                    : text);
    }
    const QString fields = jsonFields(object, kSkip);
    if (!fields.isEmpty())
        parts.append(fields);
    return parts.join(QLatin1Char(' '));
}

bool pythonExceptionLine(const QString &message)
{
    static const QRegularExpression exception(QStringLiteral(
        "^[A-Za-z_][A-Za-z0-9_.]*(Error|Exception|Interrupt|Exit|Warning)"
        "(: .*)?$"));
    return exception.match(message).hasMatch();
}

bool tracebackContinuation(const QString &message)
{
    return message.startsWith(QLatin1Char(' ')) ||
           message.startsWith(QLatin1Char('\t')) || message.isEmpty() ||
           message.startsWith(QLatin1String("Traceback (most recent call")) ||
           message.startsWith(QLatin1String("During handling of the above")) ||
           message.startsWith(QLatin1String("The above exception was")) ||
           message == QLatin1String("^^^^^^") ||
           message.startsWith(QLatin1String("  +"));
}

} // namespace

QString nextHomeDirectory()
{
    const QString configured =
        qEnvironmentVariable("FORKMESH_NEXT_HOME").trimmed();
    if (!configured.isEmpty())
        return QDir::cleanPath(configured);
    return projectsRoot() + QStringLiteral("/forkmesh-next");
}

QString fleetConfigDirectory()
{
    const QString configured =
        qEnvironmentVariable("FMNEXT_CONFIG_DIR").trimmed();
    if (!configured.isEmpty())
        return QDir::cleanPath(configured);
    const QString modern = projectsRoot() + QStringLiteral("/forkmesh/config/fleet");
    const QString legacy = nextHomeDirectory() + QStringLiteral("/config");
    if (QFileInfo::exists(modern) || !QFileInfo::exists(legacy))
        return modern;
    return legacy;
}

QString fleetSshDirectory()
{
    const QString modern = projectsRoot() + QStringLiteral("/forkmesh/ssh/fleet");
    if (QFileInfo(modern).isDir())
        return modern;
    const QString legacy = nextHomeDirectory() + QStringLiteral("/ssh");
    if (QFileInfo::exists(modern) || !QFileInfo::exists(legacy))
        return modern;
    return legacy;
}

QString fleetLogsDirectory()
{
    const QString configured = qEnvironmentVariable("FMNEXT_LOGS_DIR").trimmed();
    if (!configured.isEmpty())
        return QDir::cleanPath(configured);
    const QString modern = projectsRoot() + QStringLiteral("/forkmesh/logs/fleet");
    const QString legacy = nextHomeDirectory() + QStringLiteral("/logs");
    if (QFileInfo::exists(modern) || !QFileInfo::exists(legacy))
        return modern;
    return legacy;
}

QString readerKeyPath()
{
    const QString configured = qEnvironmentVariable("FMNEXT_LOGREAD_KEY").trimmed();
    if (!configured.isEmpty())
        return configured;
    return fleetSshDirectory() + QStringLiteral("/next_logread_ed25519");
}

QString fleetKnownHostsPath()
{
    return fleetConfigDirectory() + QStringLiteral("/known_hosts");
}

QString fleetTomlPath()
{
    const QString configured = qEnvironmentVariable("FMNEXT_FLEET_TOML").trimmed();
    if (!configured.isEmpty())
        return QDir::cleanPath(configured);
    return projectsRoot() + QStringLiteral("/forkmesh/src/deploy/fleet.toml");
}

int fleetSshPort()
{
    bool ok = false;
    const int configured = qEnvironmentVariable("FMNEXT_SSH_PORT").trimmed().toInt(&ok);
    if (ok && configured > 0 && configured < 65536)
        return configured;
    QFile file(fleetTomlPath());
    if (!file.open(QIODevice::ReadOnly))
        return kLegacySshPort;
    static const QRegularExpression line(
        QStringLiteral("^ssh_port\\s*=\\s*([0-9]{1,5})\\s*(?:#.*)?$"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match =
        line.match(QString::fromUtf8(file.read(256 * 1024)));
    const int port = match.hasMatch() ? match.captured(1).toInt() : 0;
    return port > 0 && port < 65536 ? port : kLegacySshPort;
}

QString fleetInstancesPath()
{
    return fleetConfigDirectory() + QStringLiteral("/fleet-instances.json");
}

QString autoscaledInstancesPath()
{
    return fleetConfigDirectory() + QStringLiteral("/autoscaled-instances.json");
}

bool writeAutoscaledInstances(const QJsonObject &instances)
{
    const QString path = autoscaledInstancesPath();
    const QByteArray body = QJsonDocument(instances).toJson(QJsonDocument::Indented);
    QFile current(path);
    if (current.open(QIODevice::ReadOnly) && current.read(1024 * 1024) == body)
        return true;
    current.close();
    if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(body) != body.size())
        return false;
    if (!file.commit())
        return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString vultrApiKey()
{
    return envFileValue(fleetConfigDirectory() + QStringLiteral("/vultr.env"),
                        QStringLiteral("VULTR_API_KEY"));
}

QList<CoreNode> parseFleetNodes(const QByteArray &fleetInstancesJson)
{
    const QJsonObject instances =
        QJsonDocument::fromJson(fleetInstancesJson).object();
    QList<QPair<int, CoreNode>> found;
    for (auto it = instances.constBegin(); it != instances.constEnd(); ++it) {
        CoreNode node;
        int order = 0;
        if (!nodeFromLabel(it.key(), &node, &order))
            continue;
        node.address =
            it.value().toObject().value(QStringLiteral("ip")).toString().trimmed();
        if (!validAddress(node.address))
            continue;
        found.append({order, node});
    }
    return sortedNodes(found);
}

QList<CoreNode> loadFleetNodes()
{
    QList<CoreNode> listed;
    QFile file(fleetInstancesPath());
    if (file.open(QIODevice::ReadOnly))
        listed = parseFleetNodes(file.read(1024 * 1024));
    QFile scaled(autoscaledInstancesPath());
    if (!scaled.open(QIODevice::ReadOnly))
        return listed;
    QList<CoreNode> autoscaled;
    for (const CoreNode &node : parseFleetNodes(scaled.read(1024 * 1024))) {
        if (node.label.startsWith(QLatin1String("fmnext-auto-")) ||
            node.label.startsWith(QLatin1String("fmnext-burst-")))
            autoscaled.append(node);
    }
    return mergeFleetNodes(listed, autoscaled);
}

QList<CoreNode> parseVultrInstances(const QByteArray &instancesJson)
{
    const QJsonArray instances = QJsonDocument::fromJson(instancesJson)
                                     .object()
                                     .value(QStringLiteral("instances"))
                                     .toArray();
    QList<QPair<int, CoreNode>> found;
    for (const QJsonValue &value : instances) {
        const QJsonObject instance = value.toObject();
        bool tagged = false;
        for (const QJsonValue &tag :
             instance.value(QStringLiteral("tags")).toArray())
            tagged |= tag.toString() == QLatin1String("forkmesh-next");
        if (!tagged)
            continue;
        CoreNode node;
        int order = 0;
        if (!nodeFromLabel(instance.value(QStringLiteral("label")).toString(),
                           &node, &order))
            continue;
        node.address = instance.value(QStringLiteral("main_ip")).toString();
        if (!validAddress(node.address))
            continue;
        found.append({order, node});
    }
    return sortedNodes(found);
}

QList<CoreNode> mergeFleetNodes(const QList<CoreNode> &primary,
                                const QList<CoreNode> &extra)
{
    QList<QPair<int, CoreNode>> found;
    QSet<QString> seen;
    for (const QList<CoreNode> *list : {&primary, &extra}) {
        for (const CoreNode &node : *list) {
            if (seen.contains(node.name))
                continue;
            seen.insert(node.name);
            CoreNode copy = node;
            int order = 0;
            if (!nodeFromLabel(node.label, &copy, &order))
                order = (node.role == QLatin1String("core") ? 0 : 200000) +
                        node.name.mid(1).toInt();
            copy.address = node.address;
            found.append({order, copy});
        }
    }
    return sortedNodes(found);
}

bool validJournalCursor(const QString &cursor)
{
    static const QRegularExpression shape(
        QStringLiteral("^[A-Za-z0-9=;_-]{1,256}$"));
    return shape.match(cursor).hasMatch();
}

QString readerRequest(const QString &afterCursor)
{
    if (validJournalCursor(afterCursor))
        return QStringLiteral("follow after ") + afterCursor;
    return QStringLiteral("follow lines %1").arg(kFirstConnectReplayLines);
}

FeedCommand buildFeedCommand(const CoreNode &node, const QString &afterCursor,
                             bool legacyPort)
{
    FeedCommand command;
    const QString key = readerKeyPath();
    if (!QFileInfo(key).isFile()) {
        command.error =
            QStringLiteral("the log reader key is missing (%1; tools/"
                           "next_fleet.py logread-key)")
                .arg(key);
        return command;
    }
    if (!validAddress(node.address)) {
        command.error = QStringLiteral("no address for %1").arg(node.name);
        return command;
    }
    const QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (ssh.isEmpty()) {
        command.error = QStringLiteral("no ssh client on PATH");
        return command;
    }
    command.program = ssh;
    command.arguments = {
        QStringLiteral("-i"), key,
        QStringLiteral("-o"), QStringLiteral("IdentitiesOnly=yes"),
        QStringLiteral("-o"), QStringLiteral("IdentityAgent=none"),
        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-o"),
        QStringLiteral("UserKnownHostsFile=") + fleetKnownHostsPath(),
        QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=4"),
        QStringLiteral("-o"), QStringLiteral("ClearAllForwardings=yes"),
        QStringLiteral("-o"), QStringLiteral("LogLevel=ERROR"),
        QStringLiteral("-T"),
    };
    const int port = legacyPort ? kLegacySshPort : fleetSshPort();
    if (port != kLegacySshPort)
        command.arguments << QStringLiteral("-o")
                          << QStringLiteral("HostKeyAlias=") + node.address
                          << QStringLiteral("-o")
                          << QStringLiteral("Port=") + QString::number(port);
    command.arguments << QStringLiteral("root@") + node.address
                      << readerRequest(afterCursor);
    return command;
}

ReaderControl parseReaderControl(const QByteArray &line)
{
    ReaderControl control;
    if (!line.startsWith("{\"") || line.contains("\"__CURSOR\""))
        return control;
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    if (object.contains(QStringLiteral("logread_vitals"))) {
        control.kind = ReaderControl::Vitals;
    } else if (object.contains(QStringLiteral("logread_idle"))) {
        control.kind = ReaderControl::Idle;
    } else if (object.contains(QStringLiteral("logread_error"))) {
        control.kind = ReaderControl::Failure;
        control.error = object.value(QStringLiteral("logread_error")).toString();
    } else if (object.contains(QStringLiteral("logread"))) {
        control.kind = object.contains(QStringLiteral("error"))
                           ? ReaderControl::Failure
                           : ReaderControl::Header;
        control.error = object.value(QStringLiteral("error")).toString();
        control.node = object.value(QStringLiteral("node")).toString();
        control.role = object.value(QStringLiteral("role")).toString();
        control.resumed = object.value(QStringLiteral("resumed")).toBool();
    }
    return control;
}

NodeVitals parseNodeVitals(const QByteArray &line)
{
    NodeVitals vitals;
    if (!line.startsWith("{\""))
        return vitals;
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    const QJsonValue stamp = object.value(QStringLiteral("logread_vitals"));
    if (!stamp.isDouble())
        return vitals;
    const auto whole = [&object](const char *key) -> qint64 {
        const QJsonValue value = object.value(QLatin1String(key));
        return value.isDouble() && value.toDouble() >= 0
                   ? qint64(value.toDouble())
                   : -1;
    };
    vitals.valid = true;
    vitals.atMs = qint64(stamp.toDouble());
    vitals.cpus = int(qMax<qint64>(0, whole("cpus")));
    const QJsonValue busy = object.value(QStringLiteral("cpu_busy"));
    if (busy.isDouble())
        vitals.cpuBusy = qBound(0.0, busy.toDouble(), 100.0);
    const QJsonArray load = object.value(QStringLiteral("load")).toArray();
    if (load.size() >= 3) {
        vitals.load1 = load.at(0).toDouble(-1.0);
        vitals.load5 = load.at(1).toDouble(-1.0);
        vitals.load15 = load.at(2).toDouble(-1.0);
    }
    vitals.procsRunning = int(whole("procs_running"));
    vitals.procsTotal = int(whole("procs_total"));
    vitals.memTotal = whole("mem_total");
    vitals.memAvailable = whole("mem_available");
    vitals.swapTotal = whole("swap_total");
    vitals.swapFree = whole("swap_free");
    vitals.diskTotal = whole("disk_total");
    vitals.diskFree = whole("disk_free");
    vitals.inodesTotal = whole("inodes_total");
    vitals.inodesFree = whole("inodes_free");
    vitals.filesOpen = whole("files_open");
    vitals.filesMax = whole("files_max");
    vitals.uptimeSeconds = whole("uptime");
    return vitals;
}

StatementShape statementShape(const QString &sql)
{
    StatementShape shape;
    static const QRegularExpression verb(
        QStringLiteral("^\\s*(WITH|SELECT|INSERT|REPLACE|UPDATE|DELETE|CREATE|"
                       "DROP|ALTER|PRAGMA|BEGIN|COMMIT|ROLLBACK)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch verbMatch = verb.match(sql);
    shape.verb = verbMatch.hasMatch() ? verbMatch.captured(1).toUpper()
                                      : QStringLiteral("OTHER");
    if (shape.verb == QLatin1String("REPLACE"))
        shape.verb = QStringLiteral("INSERT");
    if (shape.verb == QLatin1String("WITH")) {
        static const QRegularExpression body(
            QStringLiteral("\\)\\s*(SELECT|INSERT|UPDATE|DELETE)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch bodyMatch = body.match(sql);
        shape.verb = bodyMatch.hasMatch() ? bodyMatch.captured(1).toUpper()
                                          : QStringLiteral("SELECT");
    }
    static const QRegularExpression table(
        QStringLiteral("\\b(?:FROM|INTO|UPDATE(?:\\s+OR\\s+[A-Za-z]+)?|JOIN|"
                       "TABLE(?:\\s+IF\\s+(?:NOT\\s+)?EXISTS)?)\\s+"
                       "[\"`\\[]?([A-Za-z_][A-Za-z0-9_]*)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = table.globalMatch(sql);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        const QString lower = name.toLower();
        if (lower == QLatin1String("select") ||
            lower == QLatin1String("json_each") ||
            lower == QLatin1String("pragma_table_info"))
            continue;
        shape.table = lower;
        break;
    }
    return shape;
}

QString readerFailureState(const QString &sshStderrLine)
{
    const QString lower = sshStderrLine.toLower();
    if (lower.contains(QLatin1String("permission denied")))
        return QStringLiteral("reader key not accepted (node not rolled with "
                              "next_fleet.py install --steps logread?)");
    if (lower.contains(QLatin1String("host key verification failed")) ||
        lower.contains(QLatin1String("remote host identification has changed")))
        return QStringLiteral("host key changed; check the node before "
                              "trusting it");
    if (lower.contains(QLatin1String("timed out")) ||
        lower.contains(QLatin1String("no route")) ||
        lower.contains(QLatin1String("refused")))
        return QStringLiteral("unreachable");
    return QString();
}

QHash<QString, QString> loadCursors(const QString &path)
{
    QHash<QString, QString> cursors;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return cursors;
    const QJsonObject object =
        QJsonDocument::fromJson(file.read(256 * 1024)).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString cursor = it.value().toString();
        if (validJournalCursor(cursor))
            cursors.insert(it.key(), cursor);
    }
    return cursors;
}

bool saveCursors(const QString &path, const QHash<QString, QString> &cursors)
{
    QJsonObject object;
    for (auto it = cursors.constBegin(); it != cursors.constEnd(); ++it)
        if (validJournalCursor(it.value()))
            object.insert(it.key(), it.value());
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

VultrInventory loadVultrInventory(const QString &path)
{
    VultrInventory inventory;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return inventory;
    const QByteArray body = file.read(1024 * 1024);
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    inventory.fetchedMs =
        qBound<qint64>(0, object.value(QStringLiteral("fetchedMs")).toInteger(), now);
    inventory.retryAtMs = qBound<qint64>(
        0, object.value(QStringLiteral("retryAtMs")).toInteger(),
        now + kVultrInventoryRetryAfterMaxMs);
    inventory.failures =
        qBound(0, object.value(QStringLiteral("failures")).toInt(), 64);
    inventory.nodes = parseVultrInstances(body);
    return inventory;
}

bool saveVultrInventory(const QString &path, const VultrInventory &inventory)
{
    QJsonArray instances;
    for (const CoreNode &node : inventory.nodes)
        instances.append(QJsonObject{
            {QStringLiteral("label"), node.label},
            {QStringLiteral("main_ip"), node.address},
            {QStringLiteral("tags"), QJsonArray{QStringLiteral("forkmesh-next")}}});
    const QJsonObject object{
        {QStringLiteral("fetchedMs"), inventory.fetchedMs},
        {QStringLiteral("retryAtMs"), inventory.retryAtMs},
        {QStringLiteral("failures"), inventory.failures},
        {QStringLiteral("instances"), instances}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

qint64 vultrInventoryRetryDelayMs(int failures, qint64 retryAfterMs)
{
    qint64 delay = kVultrInventoryRetryBaseMs;
    for (int i = 1; i < failures && delay < kVultrInventoryRetryMaxMs; ++i)
        delay *= 2;
    delay = qMin(delay, kVultrInventoryRetryMaxMs);
    return qMax(delay, qMin(retryAfterMs, kVultrInventoryRetryAfterMaxMs));
}

qint64 vultrInventoryDueMs(const VultrInventory &inventory)
{
    if (inventory.failures > 0)
        return inventory.retryAtMs;
    return inventory.fetchedMs + kVultrInventoryRefreshMs;
}

JournalEntry parseJournalLine(const QByteArray &line)
{
    JournalEntry entry;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return entry;
    const QJsonObject object = document.object();
    if (!object.contains(QStringLiteral("__CURSOR")) &&
        !object.contains(QStringLiteral("MESSAGE")))
        return entry;
    const QJsonValue message = object.value(QStringLiteral("MESSAGE"));
    if (message.isString()) {
        entry.message = message.toString();
    } else if (message.isArray()) {
        QByteArray bytes;
        for (const QJsonValue &value : message.toArray())
            bytes.append(char(value.toInt() & 0xff));
        entry.message = QString::fromUtf8(bytes);
    }
    while (!entry.message.isEmpty() && entry.message.back().isSpace())
        entry.message.chop(1);
    entry.cursor = object.value(QStringLiteral("__CURSOR")).toString();
    entry.realtimeMs =
        object.value(QStringLiteral("__REALTIME_TIMESTAMP")).toString().toLongLong() /
        1000;
    bool numeric = false;
    const int priority =
        object.value(QStringLiteral("PRIORITY")).toString().toInt(&numeric);
    entry.priority = numeric ? qBound(0, priority, 7) : 6;
    entry.unit = object.value(QStringLiteral("_SYSTEMD_UNIT")).toString();
    entry.userUnit =
        object.value(QStringLiteral("_SYSTEMD_USER_UNIT")).toString();
    entry.identifier =
        object.value(QStringLiteral("SYSLOG_IDENTIFIER")).toString();
    entry.transport = object.value(QStringLiteral("_TRANSPORT")).toString();
    entry.valid = true;
    return entry;
}

QString journalSource(const JournalEntry &entry)
{
    const QString unit = entry.unit.toLower();
    const QString ident = entry.identifier.toLower();
    if (ident.startsWith(QLatin1String("postfix")) ||
        ident == QLatin1String("opendkim") ||
        ident == QLatin1String("fmnext-mail-events") ||
        unit.startsWith(QLatin1String("postfix")) ||
        unit.startsWith(QLatin1String("opendkim")) ||
        unit.startsWith(QLatin1String("fmnext-mail-events")))
        return QStringLiteral("mail");
    if (unit == QLatin1String("forkmesh-app.service"))
        return QStringLiteral("app");
    if (unit.startsWith(QLatin1String("rqlite")))
        return QStringLiteral("rqlite");
    if (unit.startsWith(QLatin1String("caddy")))
        return QStringLiteral("web");
    if (unit.startsWith(QLatin1String("fmnext-job-")) ||
        (unit.startsWith(QLatin1String("run-")) &&
         unit.endsWith(QLatin1String(".scope"))))
        return QStringLiteral("job");
    if (unit.startsWith(QLatin1String("forkmesh")))
        return QStringLiteral("mirror");
    if (unit.startsWith(QLatin1String("fmnext-ids")))
        return QStringLiteral("ids");
    if (unit.startsWith(QLatin1String("fmnext-")) ||
        unit.startsWith(QLatin1String("earlyoom")) ||
        unit.startsWith(QLatin1String("fail2ban")))
        return QStringLiteral("ops");
    if (ident.startsWith(QLatin1String("forkmesh")) ||
        ident.startsWith(QLatin1String("fmnext")))
        return QStringLiteral("ops");
    if (ident.startsWith(QLatin1String("ssh")) ||
        unit == QLatin1String("ssh.service"))
        return QStringLiteral("ssh");
    if (ident == QLatin1String("kernel") ||
        entry.transport == QLatin1String("kernel"))
        return QStringLiteral("kernel");
    return QStringLiteral("system");
}

QString unitLabel(const JournalEntry &entry)
{
    const QString ident = entry.identifier.trimmed();
    if (!entry.unit.isEmpty()) {
        const QString unit = shortUnit(entry.unit);
        if ((unit == QLatin1String("session") || unit == QLatin1String("run")) &&
            !ident.isEmpty())
            return ident;
        return unit;
    }
    if (!ident.isEmpty())
        return ident;
    return entry.transport == QLatin1String("kernel") ? QStringLiteral("kernel")
                                                      : QStringLiteral("system");
}

bool isErrorEntry(const JournalEntry &entry, const QString &source)
{
    return classifyEntry(entry, source).severity == Severity::Error;
}

bool isKnownBootNoise(const JournalEntry &entry)
{
    const QString &message = entry.message;
    return message.contains(
               QLatin1String("Failed to query local AF_VSOCK CID")) ||
           (message.contains(QLatin1String("systemd-ssh-generator")) &&
            message.contains(QLatin1String("failed with exit status")));
}

bool isCaddyReloadNoise(const JournalEntry &entry)
{
    return entry.message.contains(
               QLatin1String("setting HTTP/3 Alt-Svc header")) &&
           entry.message.contains(QLatin1String("no port can be announced"));
}

bool isBacklogEntry(const JournalEntry &entry, qint64 nowMs)
{
    return entry.realtimeMs > 0 && nowMs - entry.realtimeMs > kBacklogAlertMs;
}

QString backlogNote(const JournalEntry &entry)
{
    if (entry.realtimeMs <= 0)
        return {};
    return QStringLiteral(" (logged %1, replayed)")
        .arg(QDateTime::fromMSecsSinceEpoch(entry.realtimeMs)
                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

Classified classifyEntry(const JournalEntry &entry, const QString &source)
{
    Classified result;
    result.unit = unitLabel(entry);
    const QString &message = entry.message;
    if (entry.priority <= 3)
        result.severity = Severity::Error;
    else if (entry.priority == 4)
        result.severity = Severity::Warning;
    if (message.startsWith(QLatin1Char('{'))) {
        const QJsonObject object =
            QJsonDocument::fromJson(message.toUtf8()).object();
        const QString level = object.value(QStringLiteral("level")).toString();
        if (level == QLatin1String("error") || level == QLatin1String("panic") ||
            level == QLatin1String("fatal") || level == QLatin1String("dpanic"))
            result.severity = Severity::Error;
        else if (level == QLatin1String("warn") &&
                 result.severity == Severity::Info)
            result.severity = Severity::Warning;
        if (source == QLatin1String("ids")) {
            const QString severity =
                object.value(QStringLiteral("severity")).toString();
            if (severity == QLatin1String("alert") ||
                severity == QLatin1String("lockdown"))
                result.severity = Severity::Error;
        }
    }
    if (message.startsWith(QLatin1String("panic: ")) ||
        message.startsWith(QLatin1String("fatal error: ")) ||
        message.startsWith(QLatin1String("Traceback (most recent call last)")) ||
        message.startsWith(QLatin1String("CRITICAL")) ||
        message.startsWith(QLatin1String("ERROR:")) ||
        message.startsWith(QLatin1String("ERROR ")))
        result.severity = Severity::Error;
    if (source == QLatin1String("ssh") || isKnownBootNoise(entry))
        result.severity = Severity::Info;
    if (source == QLatin1String("web") && isCaddyReloadNoise(entry) &&
        result.severity == Severity::Error)
        result.severity = Severity::Warning;
    result.forward = entry.valid && !message.trimmed().isEmpty();
    return result;
}

bool isPoolChange(const JournalEntry &entry)
{
    static const QRegularExpression pool(QStringLiteral(
        "(runner pool|autoscaler): (creating|destroying) fmnext-(run|burst|auto)-[0-9]+"));
    return pool.match(entry.message).hasMatch();
}

AccessRecord parseAccessRecord(const JournalEntry &entry, const QString &source)
{
    AccessRecord record;
    if (source != QLatin1String("web") ||
        !entry.message.startsWith(QLatin1Char('{')) ||
        !entry.message.contains(QLatin1String("http.log.access")))
        return record;
    const QJsonObject object =
        QJsonDocument::fromJson(entry.message.toUtf8()).object();
    if (!object.value(QStringLiteral("logger"))
             .toString()
             .startsWith(QLatin1String("http.log.access")))
        return record;
    const QJsonObject request = object.value(QStringLiteral("request")).toObject();
    record.method = request.value(QStringLiteral("method")).toString();
    record.host = request.value(QStringLiteral("host")).toString().toLower();
    const int colon = record.host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !record.host.contains(QLatin1Char(']')))
        record.host.truncate(colon);
    QString uri = request.value(QStringLiteral("uri")).toString();
    const int query = uri.indexOf(QLatin1Char('?'));
    if (query >= 0)
        uri.truncate(query);
    if (uri.startsWith(QLatin1String("/_app/")))
        uri = uri.mid(5);
    record.path = uri.isEmpty() ? QStringLiteral("/") : uri;
    record.status = object.value(QStringLiteral("status")).toInt();
    record.durationMs =
        qRound64(object.value(QStringLiteral("duration")).toDouble() * 1000.0);
    record.bytes = qint64(object.value(QStringLiteral("size")).toDouble());
    const double ts = object.value(QStringLiteral("ts")).toDouble();
    record.atMs = ts > 0 ? qint64(ts * 1000.0) : entry.realtimeMs;
    record.clientIp = request.value(QStringLiteral("client_ip")).toString();
    if (record.clientIp.isEmpty())
        record.clientIp = request.value(QStringLiteral("remote_ip")).toString();
    record.clientIp = record.clientIp.trimmed().left(64);
    const auto text = [&object](const char *key) {
        return object.value(QLatin1String(key)).toString().trimmed();
    };
    record.expectedDegraded = text("expected_degraded") == QLatin1String("1");
    const bool proxied = !QHostAddress(text("cf_ip").left(64)).isNull();
    const bool relayed = !proxied && !QHostAddress(text("edge_ip").left(64)).isNull();
    if (proxied || relayed) {
        record.edgeIp = record.clientIp;
        record.clientIp = text(proxied ? "cf_ip" : "edge_ip").left(64);
        record.relay = proxied ? QStringLiteral("Cloudflare")
                               : QStringLiteral("forkmesh.com");
    }
    const QString country = text(relayed ? "edge_country" : "cf_country").toUpper();
    static const QRegularExpression countryCode(QStringLiteral("^[A-Z]{2}$"));
    if (countryCode.match(country).hasMatch() && country != QLatin1String("XX"))
        record.country = country;
    if (relayed) {
        const QUrlQuery where(
            text("edge_where").replace(QLatin1Char('+'), QStringLiteral("%20")));
        const auto part = [&where](const char *key) {
            return accessPlaceName(where.queryItemValue(QLatin1String(key),
                                                        QUrl::FullyDecoded));
        };
        record.region = part("region");
        record.city = part("city");
        const QString org = part("org");
        const QString asn = part("asn");
        static const QRegularExpression digits(QStringLiteral("^[0-9]{1,10}$"));
        const QString as = digits.match(asn).hasMatch() ? QStringLiteral("AS") + asn
                                                        : QString();
        record.network = org.isEmpty() || as.isEmpty()
                             ? org + as
                             : org + QStringLiteral(", ") + as;
    } else {
        record.region = accessPlaceName(text("cf_region"));
        record.city = accessPlaceName(text("cf_city"));
    }
    QString agent = object.value(QStringLiteral("user_agent")).toString();
    if (agent.isEmpty())
        agent = request.value(QStringLiteral("headers"))
                    .toObject()
                    .value(QStringLiteral("User-Agent"))
                    .toArray()
                    .at(0)
                    .toString();
    static const QRegularExpression unprintable(
        QStringLiteral("[\\p{Cc}\\p{Cf}\\s]+"));
    agent = agent.replace(unprintable, QStringLiteral(" ")).trimmed();
    if (agent.size() > 160)
        agent = agent.left(159) + QChar(0x2026);
    record.userAgent = agent;
    record.valid = !record.method.isEmpty() && record.status > 0;
    return record;
}

QString accessLogMessage(const AccessRecord &record)
{
    QString line = QStringLiteral("%1 %2%3 -> %4")
                       .arg(record.method, record.host, record.path)
                       .arg(record.status);
    if (record.expectedDegraded)
        line += QStringLiteral(" degraded");
    if (record.status < 500) {
        line += QStringLiteral(" %1ms").arg(record.durationMs);
        if (record.bytes > 0)
            line += QLatin1Char(' ') + compactByteSize(record.bytes);
    }
    if (!record.clientIp.isEmpty()) {
        const bool ours = !record.clientLabel.isEmpty() &&
                          record.clientLabel != QLatin1String("Cloudflare");
        const QString flag = ours ? QString() : countryFlag(record.country);
        line += QStringLiteral(" \u00b7 ") +
                (flag.isEmpty() ? QString() : flag + QLatin1Char(' ')) +
                record.clientIp;
        QStringList details;
        if (!record.clientLabel.isEmpty())
            details << record.clientLabel;
        QStringList place;
        for (const QString &part : {record.city, record.region})
            if (!part.isEmpty() && !place.contains(part))
                place << part;
        if (!place.isEmpty() && !ours) {
            if (!record.country.isEmpty())
                place << record.country;
            details << place.join(QStringLiteral(", "));
        }
        if (!record.network.isEmpty() && !ours)
            details << record.network;
        if (!record.relay.isEmpty())
            details << QStringLiteral("via ") + record.relay;
        if (!details.isEmpty())
            line += QStringLiteral(" (%1)").arg(details.join(QStringLiteral("; ")));
    }
    if (!record.userAgent.isEmpty())
        line += QStringLiteral(" \u00b7 ") + record.userAgent;
    return line;
}

QString accessPlaceName(const QString &value)
{
    static const QRegularExpression unprintable(
        QStringLiteral("[\\p{Cc}\\p{Cf}\\s]+"));
    static const QRegularExpression separators(QStringLiteral("[;()\\x{00B7}]"));
    QString name = value;
    name.replace(unprintable, QStringLiteral(" "));
    name.remove(separators);
    return name.trimmed().left(60);
}

QString accessClientLabel(const QString &address, const QList<CoreNode> &fleet)
{
    const QHostAddress host(address);
    if (host.isNull())
        return {};
    for (const CoreNode &node : fleet) {
        if (!node.address.isEmpty() && QHostAddress(node.address).isEqual(host))
            return node.name;
    }
    if (host.isLoopback())
        return QStringLiteral("local");
    if (host.isInSubnet(QHostAddress(QStringLiteral("10.77.0.0")), 24))
        return QStringLiteral("fleet");
    static const QList<QPair<QHostAddress, int>> cloudflare = [] {
        QList<QPair<QHostAddress, int>> ranges;
        for (const char *cidr :
             {"173.245.48.0/20", "103.21.244.0/22", "103.22.200.0/22",
              "103.31.4.0/22", "141.101.64.0/18", "108.162.192.0/18",
              "190.93.240.0/20", "188.114.96.0/20", "197.234.240.0/22",
              "198.41.128.0/17", "162.158.0.0/15", "104.16.0.0/13",
              "104.24.0.0/14", "172.64.0.0/13", "131.0.72.0/22",
              "2400:cb00::/32", "2606:4700::/32", "2803:f800::/32",
              "2405:b500::/32", "2405:8100::/32", "2a06:98c0::/29",
              "2c0f:f248::/32"})
            ranges.append(QHostAddress::parseSubnet(QString::fromLatin1(cidr)));
        return ranges;
    }();
    for (const auto &range : cloudflare) {
        if (host.isInSubnet(range))
            return QStringLiteral("Cloudflare");
    }
    return {};
}

namespace {

const QRegularExpression &lineAddressPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        "(?<![A-Za-z0-9_])IP ([0-9A-Fa-f][0-9A-Fa-f.:]{0,43}[0-9A-Fa-f:])(?![0-9A-Za-z_:]|\\.[0-9A-Za-z])"));
    return pattern;
}

bool lineAddressValid(const QString &address)
{
    return (address.contains(QLatin1Char(':')) ||
            address.count(QLatin1Char('.')) == 3) &&
           !QHostAddress(address).isNull();
}

}

QStringList lineClientAddresses(const QString &text)
{
    QStringList addresses;
    if (!text.contains(QLatin1String("IP ")))
        return addresses;
    QRegularExpressionMatchIterator it = lineAddressPattern().globalMatch(text);
    while (it.hasNext()) {
        const QString address = it.next().captured(1);
        if (lineAddressValid(address))
            addresses << address;
    }
    return addresses;
}

QString withClientDetails(
    const QString &text,
    const std::function<ClientDetails(const QString &)> &describe)
{
    if (!text.contains(QLatin1String("IP ")))
        return text;
    QString out;
    qsizetype done = 0;
    QRegularExpressionMatchIterator it = lineAddressPattern().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString address = match.captured(1);
        if (!lineAddressValid(address))
            continue;
        const ClientDetails details = describe(address);
        const bool ours = !details.label.isEmpty() &&
                          details.label != QLatin1String("Cloudflare");
        const QString flag = ours ? QString() : countryFlag(details.country);
        QStringList notes;
        if (!details.label.isEmpty())
            notes << details.label;
        if (!details.network.isEmpty() && !ours)
            notes << details.network;
        out += text.mid(done, match.capturedStart(1) - done);
        if (!flag.isEmpty())
            out += flag + QLatin1Char(' ');
        out += address;
        if (!notes.isEmpty())
            out += QStringLiteral(" (%1)").arg(notes.join(QStringLiteral("; ")));
        done = match.capturedEnd(1);
    }
    if (done == 0)
        return text;
    return out + text.mid(done);
}

QString compactByteSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(qMax<qint64>(0, bytes));
    static const char *const units[] = {"KB", "MB", "GB"};
    double value = double(bytes);
    int unit = -1;
    while (value >= 1024.0 && unit < 2) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', value < 10.0 ? 1 : 0)
        .arg(QLatin1String(units[unit]));
}

QString withEmojiFont(const QString &html)
{
    static const QRegularExpression flags(
        QStringLiteral("((?:[\\x{1F1E6}-\\x{1F1FF}]{2})+)"));
    QString out = html;
    out.replace(flags,
                QStringLiteral("<span style=\"font-family:'Noto Color Emoji',"
                               "'Apple Color Emoji','Segoe UI Emoji'\">\\1</span>"));
    return out;
}

QString countryFlag(const QString &code)
{
    if (code.size() != 2)
        return QString();
    QString flag;
    for (const QChar c : code.toUpper()) {
        if (c < QLatin1Char('A') || c > QLatin1Char('Z'))
            return QString();
        const char32_t point = 0x1F1E6 + (c.unicode() - 'A');
        flag += QString::fromUcs4(&point, 1);
    }
    return flag;
}

QString accessLogCategory(const AccessRecord &record, bool backlog)
{
    return record.status >= 500 && !backlog && !record.expectedDegraded
               ? QStringLiteral("ERROR")
               : QStringLiteral("CLOUD");
}

QString accessSiteGroup(const AccessRecord &record)
{
    if (record.path.startsWith(QLatin1String("/api/")))
        return QString();
    if (record.host.startsWith(QLatin1String("world.")))
        return QStringLiteral("world");
    static const QRegularExpression nodeHost(
        QStringLiteral("^n[0-9]+\\.next\\.forkmesh\\.com$"));
    if (nodeHost.match(record.host).hasMatch())
        return QStringLiteral("git");
    if (record.path.startsWith(QLatin1String("/git/")))
        return QStringLiteral("git");
    return QStringLiteral("pages");
}

QString statusClassOf(int status)
{
    if (status >= 500)
        return QStringLiteral("5xx");
    if (status >= 400)
        return QStringLiteral("4xx");
    if (status >= 300)
        return QStringLiteral("3xx");
    return QStringLiteral("2xx");
}

QString severityWord(Severity severity)
{
    switch (severity) {
    case Severity::Error:
        return QStringLiteral("err");
    case Severity::Warning:
        return QStringLiteral("warn");
    case Severity::Info:
        break;
    }
    return QStringLiteral("info");
}

QString nodeLogLine(const CoreNode &node, const QString &unit,
                    const QString &word, const QString &message,
                    qint64 foldedRepeats)
{
    QString safeUnit = unit;
    safeUnit.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9@._:-]")),
                     QStringLiteral("_"));
    if (safeUnit.isEmpty())
        safeUnit = QStringLiteral("system");
    QString text = QStringLiteral("Node %1 %2 %3 %4: %5")
                       .arg(node.name, node.role, safeUnit, word,
                            truncated(message));
    if (foldedRepeats > 0)
        text += QStringLiteral(" (+%1 identical line%2 folded)")
                    .arg(foldedRepeats)
                    .arg(foldedRepeats == 1 ? QString() : QStringLiteral("s"));
    return text;
}

NodeLinePrefix parseNodeLinePrefix(const QString &message)
{
    NodeLinePrefix prefix;
    if (!message.startsWith(QLatin1String("Node ")))
        return prefix;
    static const QRegularExpression pattern(QStringLiteral(
        "^Node ([nrab][0-9]{1,4}) (core|runner) ([A-Za-z0-9@._:-]+) "
        "([a-z]+): "));
    const QRegularExpressionMatch match = pattern.match(message);
    if (!match.hasMatch())
        return prefix;
    prefix.valid = true;
    prefix.node = match.captured(1);
    prefix.role = match.captured(2);
    prefix.unit = match.captured(3);
    prefix.severity = match.captured(4);
    return prefix;
}

QString nodeLineBadge(const QString &message, bool errorsWin)
{
    const NodeLinePrefix node = parseNodeLinePrefix(message);
    if (!node.valid)
        return {};
    static const QRegularExpression replayed(QStringLiteral(
        " \\(logged [0-9-]+ [0-9:]+, replayed\\)$"));
    if (errorsWin && node.severity == QLatin1String("err") &&
        !replayed.match(message).hasMatch())
        return QStringLiteral("ERROR");
    const QString unit = node.unit.toLower();
    if (unit.startsWith(QLatin1String("rqlite")))
        return QStringLiteral("DB");
    if (unit == QLatin1String("feed"))
        return QStringLiteral("CLOUD");
    if (unit == QLatin1String("forkmesh-app")) {
        const int body = message.indexOf(QLatin1String(": "));
        return message.mid(body + 2).startsWith(QLatin1String("cron"))
                   ? QStringLiteral("CRON")
                   : QStringLiteral("APP");
    }
    if (unit.startsWith(QLatin1String("caddy")))
        return QStringLiteral("WEB");
    if (unit.startsWith(QLatin1String("forkmesh")))
        return QStringLiteral("DAEMON");
    if (unit.startsWith(QLatin1String("fmnext-ids")))
        return QStringLiteral("IDS");
    if (unit.startsWith(QLatin1String("fmnext-cert")))
        return QStringLiteral("CERT");
    if (unit.startsWith(QLatin1String("postfix")) ||
        unit.startsWith(QLatin1String("opendkim")) ||
        unit.startsWith(QLatin1String("fmnext-mail-events")))
        return QStringLiteral("MAIL");
    if (unit == QLatin1String("job") || unit == QLatin1String("run") ||
        unit.startsWith(QLatin1String("fmnext-job")))
        return QStringLiteral("JOB");
    if (unit.startsWith(QLatin1String("fmnext-")) ||
        unit.startsWith(QLatin1String("earlyoom")) ||
        unit.startsWith(QLatin1String("fail2ban")))
        return QStringLiteral("OPS");
    return QStringLiteral("SYSTEM");
}

QStringList nodeLineBadges()
{
    return {QStringLiteral("APP"),    QStringLiteral("CRON"),
            QStringLiteral("WEB"),    QStringLiteral("DAEMON"),
            QStringLiteral("IDS"),    QStringLiteral("CERT"),
            QStringLiteral("OPS"),    QStringLiteral("JOB"),
            QStringLiteral("MAIL"),   QStringLiteral("SYSTEM")};
}

bool isFleetBadge(const QString &badge)
{
    return badge == QLatin1String("CLOUD") || badge == QLatin1String("DB") ||
           badge == QLatin1String("DEPLOY") ||
           nodeLineBadges().contains(badge);
}

namespace {

struct Mark {
    qsizetype start = 0;
    qsizetype end = 0;
    QString ink;
    bool bold = false;
};

struct Rule {
    const QRegularExpression *pattern;
    QString (*ink)(const QRegularExpressionMatch &, const Palette &, bool);
    bool bold;
};

QString inkFor(const QString &ink, const QString &text, bool bold)
{
    return QStringLiteral("<span style='color:%1%2'>%3</span>")
        .arg(ink, bold ? QStringLiteral("; font-weight:600") : QString(),
             text.toHtmlEscaped());
}

QString padHtml(int count)
{
    return QStringLiteral("&nbsp;").repeated(qMax(0, count));
}

QString paddedPlain(const QString &raw, qsizetype from, qsizetype to,
                    const QMap<qsizetype, int> &pads)
{
    QString html;
    for (auto it = pads.lowerBound(from); it != pads.cend() && it.key() < to; ++it) {
        html += raw.mid(from, it.key() - from).toHtmlEscaped() + padHtml(it.value());
        from = it.key();
    }
    return html + raw.mid(from, to - from).toHtmlEscaped();
}

QString markedHtml(const QString &raw, const QList<Rule> &rules,
                   const Palette &p, bool dark,
                   const QMap<qsizetype, int> &pads = {})
{
    QList<Mark> marks;
    for (int order = 0; order < rules.size(); ++order) {
        const Rule &rule = rules.at(order);
        auto it = rule.pattern->globalMatch(raw);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const int group = match.lastCapturedIndex() >= 1 ? 1 : 0;
            if (match.capturedLength(group) <= 0)
                continue;
            const QString ink = rule.ink(match, p, dark);
            if (ink.isEmpty())
                continue;
            marks.append({match.capturedStart(group), match.capturedEnd(group), ink,
                          rule.bold});
        }
    }
    std::stable_sort(marks.begin(), marks.end(),
                     [](const Mark &left, const Mark &right) {
                         return left.start < right.start;
                     });
    QString html;
    qsizetype at = 0;
    for (const Mark &mark : std::as_const(marks)) {
        if (mark.start < at)
            continue;
        html += paddedPlain(raw, at, mark.start, pads);
        html += inkFor(mark.ink, raw.mid(mark.start, mark.end - mark.start),
                       mark.bold);
        at = mark.end;
    }
    html += paddedPlain(raw, at, raw.size(), pads);
    return html;
}

QMap<qsizetype, int> webColumnPads(const QString &body)
{
    static const QRegularExpression access(QStringLiteral(
        "^([A-Z]+) (\\S+) -> [1-5][0-9]{2}(?: degraded)?( [0-9]+ms)?"
        "( [0-9]+(?:\\.[0-9])? (?:B|KB|MB|GB))?(?= \\x{00B7} | \\(|$)"));
    const QRegularExpressionMatch match = access.match(body);
    QMap<qsizetype, int> pads;
    if (!match.hasMatch())
        return pads;
    constexpr int methodWidth = 7;
    constexpr int urlWidth = 64;
    constexpr int durationWidth = 6;
    constexpr int sizeWidth = 7;
    constexpr int statsWidth = 2 + durationWidth + sizeWidth;
    pads[match.capturedEnd(1)] += methodWidth - match.capturedLength(1);
    pads[match.capturedEnd(2)] += urlWidth - match.capturedLength(2);
    const qsizetype statusEnd = match.capturedEnd(2) + 7;
    int used = int(match.capturedEnd(0) - statusEnd);
    if (match.capturedLength(3) > 0) {
        const int pad = durationWidth + 1 - match.capturedLength(3);
        pads[match.capturedStart(3)] += pad;
        used += qMax(0, pad);
    }
    if (match.capturedLength(4) > 0) {
        const int pad = sizeWidth + 1 - match.capturedLength(4);
        pads[match.capturedStart(4)] += pad;
        used += qMax(0, pad);
    }
    if (match.capturedEnd(0) < body.size())
        pads[match.capturedEnd(0)] += statsWidth - used;
    return pads;
}

QString mutedInk(const QRegularExpressionMatch &, const Palette &p, bool)
{
    return p.muted;
}

QString dangerInk(const QRegularExpressionMatch &, const Palette &p, bool)
{
    return p.danger;
}

QString warnInk(const QRegularExpressionMatch &, const Palette &p, bool)
{
    return p.warn;
}

QString linkInk(const QRegularExpressionMatch &, const Palette &p, bool)
{
    return p.link;
}

QString textInk(const QRegularExpressionMatch &, const Palette &p, bool)
{
    return p.text;
}

QString statusInk(const QRegularExpressionMatch &match, const Palette &p, bool)
{
    const int status = match.captured(1).toInt();
    if (status >= 500)
        return p.danger;
    if (status >= 400)
        return p.warn;
    if (status >= 300)
        return p.link;
    return p.ok;
}

QString durationInk(const QRegularExpressionMatch &match, const Palette &p, bool)
{
    const double ms = match.captured(2).toDouble();
    if (ms >= 1000)
        return p.danger;
    if (ms >= 250)
        return p.warn;
    return p.ok;
}

QString percentInk(const QRegularExpressionMatch &match, const Palette &p, bool)
{
    const double percent = match.captured(2).toDouble();
    if (percent < 10)
        return p.danger;
    if (percent < 25)
        return p.warn;
    return p.ok;
}

QString levelInk(const QRegularExpressionMatch &match, const Palette &p, bool)
{
    const QString level = match.captured(1);
    if (level == QLatin1String("warn"))
        return p.warn;
    if (level == QLatin1String("info") || level == QLatin1String("debug"))
        return p.muted;
    return p.danger;
}

QString sqlKeywordInk(const QRegularExpressionMatch &, const Palette &, bool dark)
{
    return dark ? QStringLiteral("#f778ba") : QStringLiteral("#bf3989");
}

QString sqlStringInk(const QRegularExpressionMatch &, const Palette &, bool dark)
{
    return dark ? QStringLiteral("#a5d6ff") : QStringLiteral("#0a3069");
}

QString sqlPlaceholderInk(const QRegularExpressionMatch &, const Palette &,
                          bool dark)
{
    return dark ? QStringLiteral("#ffa657") : QStringLiteral("#953800");
}

QList<Rule> commonRules()
{
    static const QRegularExpression folded(
        QStringLiteral("\\(\\+[0-9]+ identical lines? folded\\)"));
    static const QRegularExpression replayed(
        QStringLiteral("\\(logged [^)]*, replayed\\)"));
    static const QRegularExpression arrowStatus(
        QStringLiteral("-> ([1-5][0-9]{2})\\b"));
    static const QRegularExpression duration(
        QStringLiteral("\\b(([0-9]+(?:\\.[0-9]+)?) ?ms)\\b"));
    static const QRegularExpression method(QStringLiteral(
        "\\b(GET|POST|PUT|PATCH|DELETE|HEAD|OPTIONS)(?= /| [a-z0-9.-]+\\.[a-z]{2,})"));
    static const QRegularExpression failure(QStringLiteral(
        "\\b(Traceback|ERROR|CRITICAL|FATAL|panic|fatal error)\\b"));
    static const QRegularExpression slow(QStringLiteral("^slow request\\b"));
    static const QRegularExpression level(
        QStringLiteral("^(debug|info|warn|error|panic|fatal)\\b"));
    static const QRegularExpression jsonKey(
        QStringLiteral("\"([A-Za-z_][A-Za-z0-9_]*)\"(?=\\s*:)"));
    static const QRegularExpression field(
        QStringLiteral("(?<![A-Za-z0-9_])([a-z_][a-z0-9_]*)=(?=\\S)"));
    static const QRegularExpression separator(QStringLiteral("\\x{00B7}"));
    return {
        {&folded, mutedInk, false},     {&replayed, mutedInk, false},
        {&arrowStatus, statusInk, true}, {&duration, durationInk, false},
        {&method, linkInk, true},       {&failure, dangerInk, true},
        {&slow, warnInk, true},         {&level, levelInk, true},
        {&jsonKey, mutedInk, false},    {&field, mutedInk, false},
        {&separator, mutedInk, false},
    };
}

QList<Rule> bodyRules(const QString &badge)
{
    QList<Rule> rules;
    if (badge == QLatin1String("DB")) {
        static const QRegularExpression keyword(QStringLiteral(
            "\\b(SELECT|INSERT|INTO|UPDATE|DELETE|FROM|WHERE|AND|OR|NOT|NULL|"
            "IN|IS|JOIN|LEFT|INNER|OUTER|CROSS|ON|GROUP|ORDER|BY|LIMIT|OFFSET|"
            "VALUES|SET|AS|DISTINCT|EXISTS|REPLACE|CONFLICT|DO|NOTHING|"
            "RETURNING|WITH|CASE|WHEN|THEN|ELSE|END|BEGIN|COMMIT|ROLLBACK|"
            "CREATE|TABLE|INDEX|DROP|ALTER|PRAGMA|LIKE|BETWEEN|DESC|ASC|"
            "HAVING|UNION|ALL|IGNORE|COUNT|SUM|MIN|MAX|COALESCE|IFNULL)\\b"));
        static const QRegularExpression literal(QStringLiteral("'[^']*'"));
        static const QRegularExpression placeholder(
            QStringLiteral("(?<![A-Za-z0-9_])(\\?[0-9]*)"));
        static const QRegularExpression args(
            QStringLiteral("\\[[0-9]+ args?\\]"));
        static const QRegularExpression origin(QStringLiteral(
            "\\x{00B7} from [A-Za-z0-9_.-]+(?: in [A-Za-z0-9_<>.]+)?$"));
        static const QRegularExpression client(QStringLiteral(
            "(?<![A-Za-z0-9_])IP (?:\\S+ )?[0-9A-Fa-f.:]+ (\\([^()]*\\))"));
        rules = {{&client, mutedInk, false},
                 {&origin, mutedInk, false},      {&args, mutedInk, false},
                 {&literal, sqlStringInk, false}, {&placeholder, sqlPlaceholderInk, true},
                 {&keyword, sqlKeywordInk, true}};
        return rules;
    }
    if (badge == QLatin1String("DAEMON")) {
        static const QRegularExpression operation(
            QStringLiteral("^([a-z][a-z-]*) (?=[1-5][0-9]{2}\\b)"));
        static const QRegularExpression status(
            QStringLiteral("^[a-z][a-z-]* ([1-5][0-9]{2})\\b"));
        rules = {{&operation, textInk, true}, {&status, statusInk, true}};
    } else if (badge == QLatin1String("IDS")) {
        static const QRegularExpression alarm(
            QStringLiteral("^(alert|lockdown)\\b"));
        static const QRegularExpression detector(QStringLiteral(
            "^(?:[a-z]+ )?([a-z][a-z0-9_]*)(?=: )"));
        rules = {{&alarm, dangerInk, true}, {&detector, textInk, true}};
    } else if (badge == QLatin1String("OPS")) {
        static const QRegularExpression percent(
            QStringLiteral("\\b(([0-9]+(?:\\.[0-9]+)?)%)"));
        rules = {{&percent, percentInk, true}};
    } else if (badge == QLatin1String("WEB")) {
        static const QRegularExpression method(
            QStringLiteral("^([A-Z]+)(?= \\S+ -> [1-5][0-9]{2}\\b)"));
        static const QRegularExpression host(QStringLiteral(
            "^[A-Z]+ ([^\\s/]+)(?=/\\S* -> [1-5][0-9]{2}\\b)"));
        static const QRegularExpression size(
            QStringLiteral("(?<=ms )([0-9]+(?:\\.[0-9])? (?:B|KB|MB|GB))\\b"));
        static const QRegularExpression client(QStringLiteral(
            " \\((n[0-9]+|r[0-9]+|a[0-9]+|b[0-9]+|fleet|local|Cloudflare)[;)]"));
        static const QRegularExpression via(QStringLiteral(
            "(?:\\(|; )(via (?:Cloudflare|forkmesh\\.com))\\)"));
        static const QRegularExpression agent(QStringLiteral(
            "-> [^\\x{00B7}]*\\x{00B7} [^\\x{00B7}]* \\x{00B7} ([^\\x{00B7}]+?)"
            "(?= \\(logged [^)]*, replayed\\)$|$)"));
        rules = {{&method, linkInk, true},
                 {&host, mutedInk, false},
                 {&size, mutedInk, false},
                 {&client, textInk, true},
                 {&via, mutedInk, false},
                 {&agent, mutedInk, false}};
    } else if (badge == QLatin1String("CRON")) {
        static const QRegularExpression owner(
            QStringLiteral("\\b(?:owned by|ran on) ([nab][0-9]{1,4})\\b"));
        rules = {{&owner, textInk, true}};
    }
    rules += commonRules();
    return rules;
}

QString nodeInk(const QString &name)
{
    const int number = qMax(1, name.mid(1).toInt());
    const QChar kind = name.isEmpty() ? QLatin1Char('n') : name.at(0);
    const int offset = kind == QLatin1Char('a')   ? 3
                       : kind == QLatin1Char('r') ? 4
                       : kind == QLatin1Char('b') ? 5
                                                  : 0;
    return nodeColour(number - 1 + offset);
}

} // namespace

QString nodeInkColour(const QString &node)
{
    return nodeInk(node);
}

QString nodeLineHtml(const QString &message, bool dark, const QString &unitAccent)
{
    if (!message.startsWith(QLatin1String("Node ")))
        return {};
    static const QRegularExpression pattern(QStringLiteral(
        "^Node ([nrab][0-9]{1,4}) (core|runner) ([A-Za-z0-9@._:-]+) "
        "([a-z]+): "));
    const QRegularExpressionMatch match = pattern.match(message);
    if (!match.hasMatch())
        return {};
    const Palette p = palette(dark);
    const QString severity = match.captured(4);
    const QString severityInk =
        severity == QLatin1String("err")    ? p.danger
        : severity == QLatin1String("warn") ? p.warn
        : severity == QLatin1String("info") ? p.muted
                                            : sqlKeywordInk({}, p, dark);
    const QString body = message.mid(match.capturedEnd());
    const QString badge = nodeLineBadge(message, false);
    const auto column = [](const QString &text, int width) {
        return padHtml(width - int(text.size())) + QLatin1Char(' ');
    };
    QString html = inkFor(p.muted, QStringLiteral("Node"), false);
    html += QLatin1Char(' ') + inkFor(nodeInk(match.captured(1)), match.captured(1), true);
    html += column(match.captured(1), 3) + inkFor(p.muted, match.captured(2), false);
    html += column(match.captured(2), 6) +
            inkFor(unitAccent.isEmpty() ? p.text : unitAccent, match.captured(3), true);
    html += column(match.captured(3), 12) +
            inkFor(severityInk, severity + QLatin1Char(':'), true);
    html += column(severity, 5) +
            markedHtml(body, bodyRules(badge), p, dark,
                       badge == QLatin1String("WEB") ? webColumnPads(body)
                                                     : QMap<qsizetype, int>());
    return withEmojiFont(html);
}

bool NodeLogFilter::accepts(const QString &message) const
{
    if (!active())
        return true;
    const NodeLinePrefix prefix = parseNodeLinePrefix(message);
    if (!prefix.valid)
        return false;
    if (!nodes.isEmpty() && !nodes.contains(prefix.node))
        return false;
    if (!units.isEmpty() && !units.contains(prefix.unit))
        return false;
    return !errorsOnly || prefix.severity == QLatin1String("err");
}

StatementLine parseStatementLine(const JournalEntry &entry,
                                 const QString &source)
{
    StatementLine statement;
    if (source != QLatin1String("app"))
        return statement;
    static const QRegularExpression head(
        QStringLiteral("^D1 ([a-z0-9 ]+): "));
    const QRegularExpressionMatch headMatch = head.match(entry.message);
    if (!headMatch.hasMatch())
        return statement;
    statement.kind = headMatch.captured(1);
    statement.sql = entry.message.mid(headMatch.capturedEnd()).trimmed();
    static const QRegularExpression origin(QStringLiteral(
        " \\(from ([A-Za-z0-9_.-]+\\.py)(?: in ([A-Za-z0-9_<>.]+))?\\)$"));
    const QRegularExpressionMatch originMatch = origin.match(statement.sql);
    if (originMatch.hasMatch()) {
        statement.originFile = originMatch.captured(1);
        statement.originFunction = originMatch.captured(2);
        statement.sql.truncate(originMatch.capturedStart());
    }
    static const QRegularExpression caller(
        QStringLiteral(" \\(for ([^()]+)\\)$"));
    const QRegularExpressionMatch callerMatch = caller.match(statement.sql);
    if (callerMatch.hasMatch()) {
        statement.caller = callerMatch.captured(1).trimmed();
        statement.sql.truncate(callerMatch.capturedStart());
    }
    statement.valid = !statement.sql.isEmpty();
    return statement;
}

WebRequestBatch parseWebRequestBatch(const JournalEntry &entry,
                                     const QString &source)
{
    WebRequestBatch batch;
    static const QLatin1String prefix("web-requests ");
    if (source != QLatin1String("app") || !entry.message.startsWith(prefix))
        return batch;
    const QJsonObject object =
        QJsonDocument::fromJson(entry.message.mid(prefix.size()).toUtf8())
            .object();
    batch.windowStartMs =
        qint64(object.value(QStringLiteral("t")).toDouble());
    batch.windowSeconds = object.value(QStringLiteral("s")).toInt();
    if (batch.windowStartMs <= 0)
        return batch;
    for (const QJsonValue &value :
         object.value(QStringLiteral("r")).toArray()) {
        const QJsonArray cells = value.toArray();
        if (cells.size() < 5 || !cells.at(0).isString() ||
            !cells.at(1).isString())
            continue;
        WebRequestRow row;
        row.group = cells.at(0).toString();
        row.statusClass = cells.at(1).toString();
        row.count = qint64(cells.at(2).toDouble());
        row.durationSumMs = qint64(cells.at(3).toDouble());
        row.durationMaxMs = qint64(cells.at(4).toDouble());
        if (row.group.isEmpty() || row.count <= 0)
            continue;
        batch.rows.append(row);
    }
    batch.valid = true;
    return batch;
}

StatementDone parseStatementDone(const JournalEntry &entry,
                                 const QString &source)
{
    StatementDone done;
    if (source != QLatin1String("app"))
        return done;
    static const QRegularExpression pattern(QStringLiteral(
        "^D1-done ([0-9]+(?:\\.[0-9]+)?) ms, ([0-9]+) rows, ([0-9]+) "
        "changed((?:, [a-z]+)*) #([0-9a-f]{6,40})$"));
    const QRegularExpressionMatch match = pattern.match(entry.message.trimmed());
    if (!match.hasMatch())
        return done;
    done.ms = match.captured(1).toDouble();
    done.rows = match.captured(2).toLongLong();
    done.changes = match.captured(3).toLongLong();
    for (const QString &flag :
         match.captured(4).split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        if (flag == QLatin1String("failed"))
            done.failed = true;
        else
            done.served = flag;
    }
    done.key = match.captured(5);
    done.valid = true;
    return done;
}

QString statementText(const QString &sql)
{
    static const QRegularExpression args(QStringLiteral(" \\[[0-9]+ args?\\]$"));
    QString text = sql;
    const QRegularExpressionMatch match = args.match(text);
    if (match.hasMatch())
        text.truncate(match.capturedStart());
    return text;
}

int statementArgCount(const QString &sql)
{
    static const QRegularExpression args(QStringLiteral(" \\[([0-9]+) args?\\]$"));
    const QRegularExpressionMatch match = args.match(sql);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString statementKey(const QString &sql)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(statementText(sql).toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(10));
}

QString callerRoute(const QString &path)
{
    static const QRegularExpression hexId(QStringLiteral("^[0-9a-fA-F]{12,}$"));
    static const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
    QStringList parts = path.split(QLatin1Char('/'));
    for (QString &part : parts) {
        if (numeric.match(part).hasMatch() || hexId.match(part).hasMatch() ||
            part.size() > 40)
            part = QStringLiteral(":id");
    }
    return parts.join(QLatin1Char('/'));
}

CallerShape callerShape(const QString &caller)
{
    CallerShape shape;
    const QStringList fields =
        caller.trimmed().split(QStringLiteral(", "), Qt::SkipEmptyParts);
    if (fields.isEmpty()) {
        shape.kind = QStringLiteral("none");
        shape.label = QStringLiteral("(no caller)");
        return shape;
    }
    QString head;
    for (const QString &field : fields) {
        if (field.startsWith(QLatin1String("IP ")))
            shape.ip = field.mid(3);
        else if (field.startsWith(QLatin1String("user ")))
            shape.user = field.mid(5);
        else if (field.startsWith(QLatin1String("session ")))
            shape.session = field.mid(8);
        else if (head.isEmpty())
            head = field;
    }
    static const QRegularExpression request(
        QStringLiteral("^([A-Z]+) (/\\S*)$"));
    const QRegularExpressionMatch match = request.match(head);
    if (match.hasMatch()) {
        shape.kind = QStringLiteral("request");
        shape.label = match.captured(1) + QLatin1Char(' ') +
                      callerRoute(match.captured(2));
    } else if (head == QLatin1String("cron") ||
               head.startsWith(QLatin1String("cron "))) {
        shape.kind = QStringLiteral("cron");
        shape.label = QStringLiteral("cron");
    } else if (head.startsWith(QLatin1String("queue"))) {
        shape.kind = QStringLiteral("queue");
        shape.label = head;
    } else if (head.startsWith(QLatin1String("alarm"))) {
        shape.kind = QStringLiteral("alarm");
        shape.label = head;
    } else if (head.isEmpty()) {
        shape.kind = QStringLiteral("request");
        shape.label = QStringLiteral("(unnamed request)");
    } else {
        shape.kind = QStringLiteral("other");
        shape.label = head;
    }
    return shape;
}

QString mainLogStatementLine(const CoreNode &node,
                             const StatementLine &statement)
{
    QString text = nodeLogLine(node, QStringLiteral("rqlite"), statement.kind.section(QLatin1Char(' '), 0, 0),
                               statement.sql);
    if (!statement.originFile.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 from ") + statement.originFile +
                (statement.originFunction.isEmpty()
                     ? QString()
                     : QStringLiteral(" in ") + statement.originFunction);
    if (!statement.caller.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 for ") + statement.caller;
    return text;
}

QString feedNoteLine(const CoreNode &node, const QString &note)
{
    return nodeLogLine(node, QStringLiteral("feed"), QStringLiteral("info"), note);
}

QString displayMessage(const JournalEntry &entry)
{
    if (!entry.message.startsWith(QLatin1Char('{')))
        return entry.message;
    const QJsonObject object =
        QJsonDocument::fromJson(entry.message.toUtf8()).object();
    if (object.isEmpty())
        return entry.message;
    if (object.value(QStringLiteral("ids")).isString())
        return idsJsonLine(object);
    if (!object.contains(QStringLiteral("msg"))) {
        const QString request = requestRecordLine(object);
        if (!request.isEmpty())
            return request;
        const QString generic = genericJsonLine(object);
        return generic.isEmpty() ? entry.message : generic;
    }
    const QString level = object.value(QStringLiteral("level")).toString();
    const QString compact = compactJsonLine(object);
    return level.isEmpty() ? compact : level + QLatin1Char(' ') + compact;
}

QList<JournalEntry> TracebackAssembler::feed(const QString &node,
                                             const JournalEntry &entry,
                                             qint64 nowMs)
{
    QList<JournalEntry> out;
    const QString key = node + QLatin1Char('\n') + entry.unit +
                        QLatin1Char('\n') + entry.identifier;
    auto open = m_open.find(key);
    const bool starts = entry.message.startsWith(
        QLatin1String("Traceback (most recent call last)"));
    if (open != m_open.end()) {
        if (tracebackContinuation(entry.message) &&
            open->lines.size() < 400) {
            open->lines.append(entry.message);
            open->lastMs = nowMs;
            open->first.cursor = entry.cursor;
            return out;
        }
        Open done = *open;
        m_open.erase(open);
        if (pythonExceptionLine(entry.message)) {
            done.lines.append(entry.message);
            done.first.cursor = entry.cursor;
            out.append(finish(done));
            return out;
        }
        out.append(finish(done));
    }
    if (starts) {
        Open fresh;
        fresh.first = entry;
        fresh.lines.append(entry.message);
        fresh.lastMs = nowMs;
        m_open.insert(key, fresh);
        return out;
    }
    out.append(entry);
    return out;
}

QList<QPair<QString, JournalEntry>> TracebackAssembler::takeStale(qint64 nowMs,
                                                                  qint64 quietMs)
{
    QList<QPair<QString, JournalEntry>> out;
    for (auto it = m_open.begin(); it != m_open.end();) {
        if (nowMs - it->lastMs < quietMs) {
            ++it;
            continue;
        }
        out.append({it.key().section(QLatin1Char('\n'), 0, 0), finish(*it)});
        it = m_open.erase(it);
    }
    return out;
}

JournalEntry TracebackAssembler::finish(const Open &open)
{
    JournalEntry entry = open.first;
    entry.priority = qMin(entry.priority, 3);
    QString exception;
    QString location;
    static const QRegularExpression frame(QStringLiteral(
        "^\\s*File \"([^\"]+)\", line ([0-9]+), in (.+)$"));
    for (const QString &line : open.lines) {
        const QRegularExpressionMatch match = frame.match(line);
        if (match.hasMatch())
            location = QStringLiteral("%1:%2 in %3")
                           .arg(QFileInfo(match.captured(1)).fileName(),
                                match.captured(2), match.captured(3));
        else if (pythonExceptionLine(line))
            exception = line;
    }
    if (exception.isEmpty())
        exception = QStringLiteral("Traceback (incomplete)");
    entry.message = QStringLiteral("Traceback: %1").arg(exception);
    if (!location.isEmpty())
        entry.message += QString::fromUtf8(" \xC2\xB7 at ") + location;
    entry.detail = open.lines.join(QLatin1Char('\n'));
    return entry;
}

QString normalizedRepeatKey(const QString &message)
{
    static const QRegularExpression stamps(QStringLiteral(
        "\\d{4}[-/]\\d{2}[-/]\\d{2}[ T]\\d{2}:\\d{2}:\\d{2}(?:\\.\\d+)?Z?"));
    QString key = message;
    key.remove(stamps);
    return key.simplified();
}

QString statementLinkKey(const QString &originFile, const QString &sql)
{
    return originFile + QLatin1Char('\n') + sql;
}

RepeatVerdict RepeatFilter::admit(const QString &node, const QString &source,
                                  const QString &message, qint64 nowMs)
{
    const QString key = node + QLatin1Char('\n') + source + QLatin1Char('\n') +
                        normalizedRepeatKey(message);
    auto found = m_seen.find(key);
    if (found != m_seen.end() && nowMs - found->admittedMs < kRepeatWindowMs) {
        ++found->folded;
        return {false, 0};
    }
    RepeatVerdict verdict;
    if (found != m_seen.end()) {
        verdict.foldedRepeats = found->folded;
        found->admittedMs = nowMs;
        found->folded = 0;
        return verdict;
    }
    if (m_seen.size() >= m_capacity) {
        for (auto it = m_seen.begin(); it != m_seen.end();) {
            if (nowMs - it->admittedMs >= kRepeatWindowMs)
                it = m_seen.erase(it);
            else
                ++it;
        }
        if (m_seen.size() >= m_capacity)
            m_seen.clear();
    }
    m_seen.insert(key, Seen{nowMs, 0});
    return verdict;
}

NodeRow &ActivityTally::nodeRow(const QString &node, qint64 nowMs)
{
    if (m_sinceMs <= 0)
        m_sinceMs = nowMs;
    NodeRow &row = m_nodes[node];
    row.node = node;
    return row;
}

void ActivityTally::noteFeedState(const QString &node, bool connected,
                                  const QString &state, qint64 nowMs)
{
    NodeRow &row = nodeRow(node, nowMs);
    row.connected = connected;
    row.state = state;
    row.stateMs = nowMs;
}

void ActivityTally::noteLine(const QString &node, const QString &source,
                             bool forwarded, bool folded, bool error,
                             qint64 nowMs)
{
    NodeRow &row = nodeRow(node, nowMs);
    ++row.lines;
    ++row.bySource[source];
    if (forwarded)
        ++row.forwarded;
    if (folded)
        ++row.folded;
    if (error)
        ++row.errors;
    row.lastLineMs = nowMs;
}

void ActivityTally::noteStatement(const QString &node,
                                  const StatementLine &statement, qint64 nowMs)
{
    NodeRow &row = nodeRow(node, nowMs);
    ++row.statements;
    ++m_totalStatements;
    ++m_minutes[nowMs / 60000][node];
    const qint64 oldest = nowMs / 60000 - kTimelineMinutes;
    while (!m_minutes.isEmpty() && m_minutes.firstKey() <= oldest)
        m_minutes.erase(m_minutes.begin());
    const QString key = statement.kind + QLatin1Char('\n') + statement.sql +
                        QLatin1Char('\n') + statement.originFile;
    auto found = m_statements.find(key);
    if (found == m_statements.end()) {
        if (m_statements.size() >= m_statementCapacity) {
            ++m_overflowStatements;
            return;
        }
        StatementRow fresh;
        fresh.kind = statement.kind;
        fresh.sql = statement.sql;
        fresh.originFile = statement.originFile;
        fresh.originFunction = statement.originFunction;
        found = m_statements.insert(key, fresh);
    }
    ++found->count;
    ++found->perNode[node];
    found->lastMs = nowMs;
}

QList<StatementRow> ActivityTally::topStatements(int limit) const
{
    QList<StatementRow> rows = m_statements.values();
    std::sort(rows.begin(), rows.end(),
              [](const StatementRow &a, const StatementRow &b) {
                  if (a.count != b.count)
                      return a.count > b.count;
                  return a.lastMs > b.lastMs;
              });
    if (limit >= 0 && rows.size() > limit)
        rows = rows.mid(0, limit);
    return rows;
}

QList<NodeRow> ActivityTally::nodes() const
{
    return m_nodes.values();
}

QList<QPair<qint64, QMap<QString, qint64>>>
ActivityTally::timeline(qint64 nowMs) const
{
    QList<QPair<qint64, QMap<QString, qint64>>> minutes;
    const qint64 current = nowMs / 60000;
    for (qint64 minute = current - kTimelineMinutes + 1; minute <= current;
         ++minute)
        minutes.append({minute * 60000, m_minutes.value(minute)});
    return minutes;
}

QString renderActivityHtml(const ActivityTally &tally, qint64 nowMs, bool dark,
                           const QString &idleReason,
                           const QHash<QString, StatementLink> &links)
{
    const Palette p = palette(dark);
    QString html = QStringLiteral("<div style='color:%1;'>").arg(p.text);
    const QList<NodeRow> nodes = tally.nodes();
    html += sectionTitle(
        QStringLiteral("Core nodes: live rqlite statements"),
        tally.sinceMs() > 0
            ? QStringLiteral("%1 statement%2 from the cores' journals since %3")
                  .arg(tally.totalStatements())
                  .arg(tally.totalStatements() == 1 ? QString()
                                                    : QStringLiteral("s"),
                       QDateTime::fromMSecsSinceEpoch(tally.sinceMs())
                           .toString(QStringLiteral("HH:mm")))
            : QStringLiteral("read live from each core's journal over SSH"),
        p);
    if (!idleReason.isEmpty())
        html += notice(idleReason, p, p.warn);

    QStringList nodeNames;
    for (const NodeRow &row : nodes)
        nodeNames.append(row.node);

    if (!nodes.isEmpty()) {
        html += QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0'>");
        html += headerRow({QStringLiteral("Node"), QStringLiteral("Feed"),
                           QStringLiteral("Journal lines"),
                           QStringLiteral("In the log"),
                           QStringLiteral("Folded"),
                           QStringLiteral("Statements"),
                           QStringLiteral("Errors"),
                           QStringLiteral("Last line")},
                          p);
        for (int index = 0; index < nodes.size(); ++index) {
            const NodeRow &row = nodes.at(index);
            QStringList sources;
            for (auto it = row.bySource.constBegin();
                 it != row.bySource.constEnd(); ++it)
                sources.append(QStringLiteral("%1 %2").arg(it.key()).arg(it.value()));
            QString nodeCell =
                QStringLiteral("<b style='color:%1;'>%2</b>")
                    .arg(nodeColour(index), row.node.toHtmlEscaped());
            if (!sources.isEmpty())
                nodeCell += QStringLiteral(
                                "<div style='color:%1;font-size:10px;'>%2</div>")
                                .arg(p.muted,
                                     sources.join(QString::fromUtf8(" \xC2\xB7 "))
                                         .toHtmlEscaped());
            html += QStringLiteral("<tr>");
            html += cell(nodeCell, p, true);
            html += cell((row.state.isEmpty() ? QStringLiteral("starting")
                                              : row.state)
                             .toHtmlEscaped(),
                         p, false, row.connected ? p.ok : p.warn);
            html += countCell(row.lines, p);
            html += countCell(row.forwarded, p);
            html += countCell(row.folded, p, p.muted);
            html += countCell(row.statements, p);
            html += countCell(row.errors, p, p.danger);
            html += cell(ago(row.lastLineMs, nowMs), p, false, p.muted);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    const auto minutes = tally.timeline(nowMs);
    qint64 peak = 0;
    for (const auto &minute : minutes) {
        qint64 total = 0;
        for (const qint64 value : minute.second)
            total += value;
        peak = qMax(peak, total);
    }
    html += sectionTitle(
        QStringLiteral("Statements per minute"),
        QStringLiteral("last %1 minutes, newest first, by node")
            .arg(kTimelineMinutes),
        p);
    if (peak <= 0) {
        html += notice(QStringLiteral("No statements in the last %1 minutes.")
                           .arg(kTimelineMinutes),
                       p);
    } else {
        html += QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0'>");
        for (int i = minutes.size() - 1; i >= 0; --i) {
            const auto &minute = minutes.at(i);
            qint64 total = 0;
            for (const qint64 value : minute.second)
                total += value;
            if (total <= 0)
                continue;
            QString bar = QStringLiteral(
                "<table width='100%' cellspacing='0' cellpadding='0'><tr>");
            int used = 0;
            for (int n = 0; n < nodeNames.size(); ++n) {
                const qint64 value = minute.second.value(nodeNames.at(n));
                if (value <= 0)
                    continue;
                const int width = qMax(1, int(value * 100 / peak));
                used += width;
                bar += QStringLiteral(
                           "<td width='%1%' bgcolor='%2' style='font-size:6px;'>"
                           "&nbsp;</td>")
                           .arg(width)
                           .arg(nodeColour(n));
            }
            if (used < 100)
                bar += QStringLiteral(
                           "<td width='%1%' bgcolor='%2' style='font-size:6px;'>"
                           "&nbsp;</td>")
                           .arg(100 - used)
                           .arg(p.track);
            bar += QStringLiteral("</tr></table>");
            QStringList perNode;
            for (auto it = minute.second.constBegin();
                 it != minute.second.constEnd(); ++it)
                perNode.append(QStringLiteral("%1 %2").arg(it.key()).arg(it.value()));
            html += QStringLiteral("<tr>");
            html += QStringLiteral(
                        "<td width='60' style='padding:2px 6px;color:%1;"
                        "font-size:10px;'>%2</td>")
                        .arg(p.muted,
                             QDateTime::fromMSecsSinceEpoch(minute.first)
                                 .toString(QStringLiteral("HH:mm")));
            html += QStringLiteral("<td style='padding:2px 6px;'>%1</td>")
                        .arg(bar);
            html += QStringLiteral(
                        "<td width='160' align='right' style='padding:2px 6px;"
                        "color:%1;font-size:10px;'>%2</td>")
                        .arg(p.muted,
                             QStringLiteral("%1 (%2)")
                                 .arg(total)
                                 .arg(perNode.join(QStringLiteral(", ")))
                                 .toHtmlEscaped());
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    const QList<StatementRow> rows = tally.topStatements(40);
    html += sectionTitle(
        QStringLiteral("Most frequent rqlite statements"),
        tally.overflowStatements() > 0
            ? QStringLiteral("%1 run%2 of statements past the first %3 are "
                             "counted only in the totals")
                  .arg(tally.overflowStatements())
                  .arg(tally.overflowStatements() == 1 ? QString()
                                                       : QStringLiteral("s"))
                  .arg(tally.statementCapacity())
            : QStringLiteral("bound values are never logged"),
        p);
    if (rows.isEmpty()) {
        html += notice(
            QStringLiteral(
                "No statements seen yet. A core writes one journal line per "
                "statement only while FORKMESH_HOST_D1_LOG=1 is set in its "
                "app.env; its other logs still arrive under Cloud."),
            p);
    } else {
        QStringList columns{QStringLiteral("Statement"), QStringLiteral("Runs")};
        for (const QString &name : std::as_const(nodeNames))
            columns.append(name);
        columns.append(QStringLiteral("Last"));
        html += QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0'>");
        html += headerRow(columns, p);
        for (const StatementRow &row : rows) {
            QString statementCell =
                QStringLiteral("<code>%1</code>").arg(row.sql.toHtmlEscaped());
            QStringList origin;
            if (!row.kind.isEmpty() && row.kind != QLatin1String("query"))
                origin.append(row.kind);
            if (!row.originFile.isEmpty())
                origin.append(row.originFunction.isEmpty()
                                  ? row.originFile
                                  : row.originFile + QStringLiteral(" in ") +
                                        row.originFunction);
            if (!origin.isEmpty())
                statementCell +=
                    QStringLiteral("<div style='color:%1;font-size:10px;'>%2</div>")
                        .arg(p.muted,
                             origin.join(QString::fromUtf8(" \xC2\xB7 "))
                                 .toHtmlEscaped());
            const StatementLink link =
                links.value(statementLinkKey(row.originFile, row.sql));
            if (!link.href.isEmpty())
                statementCell +=
                    QStringLiteral("<div style='padding-top:2px;'><a href='%1' "
                                   "style='color:%2;font-size:10px;"
                                   "text-decoration:none;'>%3</a></div>")
                        .arg(link.href.toHtmlEscaped(), p.link,
                             link.label.toHtmlEscaped());
            html += QStringLiteral("<tr>");
            html += cell(statementCell, p, true);
            html += countCell(row.count, p);
            for (const QString &name : std::as_const(nodeNames))
                html += countCell(row.perNode.value(name), p);
            html += cell(ago(row.lastMs, nowMs), p, false, p.muted);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    return html + QStringLiteral("</div>");
}

} // namespace corelogs
} // namespace forkmesh
