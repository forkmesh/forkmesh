#include "AgentAccessFailure.h"
#include "AgentReadOnlyCommand.h"

#include "AgentNetworkGate.h"

#include <QList>
#include <QRegularExpression>
#include <QStringList>

namespace forkmesh::agentnet {

namespace {

using Kind = AccessFailure::Kind;

constexpr qsizetype kMaxEvidence = 300;

struct Pattern {
    Kind kind;
    QRegularExpression re; // capture 1 is the target when the text names it
};

QRegularExpression rx(const QString &pattern)
{
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
}

const QList<Pattern> &patterns()
{
    static const QList<Pattern> list{
        // Resolvers: curl, git, ssh, wget, node, go, python (urllib3/requests), Qt.
        {Kind::Resolve, rx(QStringLiteral(R"(could not resolve host(?:name)?:?\s+([^\s;,:'"]+))"))},
        {Kind::Resolve, rx(QStringLiteral(R"(unable to resolve host address\s+[‘'"]?([^’'"\s]+))"))},
        {Kind::Resolve, rx(QStringLiteral(R"(getaddrinfo\s+(?:ENOTFOUND|EAI_AGAIN)\s+([^\s,'"]+))"))},
        {Kind::Resolve, rx(QStringLiteral(R"(dial tcp: lookup ([^\s:]+))"))},
        {Kind::Resolve, rx(QStringLiteral(R"(failed to resolve '([^']+)')"))},
        {Kind::Resolve, rx(QStringLiteral(R"(host='([^']+)'.*(?:name resolution|not known|nodename nor servname))"))},
        {Kind::Resolve, rx(QStringLiteral(R"(\bhost ([a-z0-9.-]+\.[a-z]{2,}) not found)"))},
        {Kind::Resolve, rx(QStringLiteral(R"((?:temporary failure in name resolution|name or service not known|no address associated with hostname|nodename nor servname provided|failed to lookup address information|\bEAI_AGAIN\b|\bENOTFOUND\b))"))},
        // Hosts that did not answer. A refused connection is a server that is
        // down, not access being withheld, so it does not count.
        {Kind::Connect, rx(QStringLiteral(R"(failed to connect to ([^\s]+) port \d+.*(?:timed out|timeout|no route to host|network is unreachable))"))},
        {Kind::Connect, rx(QStringLiteral(R"(connect (?:ETIMEDOUT|EHOSTUNREACH|ENETUNREACH) ([0-9a-f.:\[\]]+?)(?::\d+)?(?:\s|$))"))},
        {Kind::Connect, rx(QStringLiteral(R"((?:network is unreachable|no route to host))"))},
        // Permission denied on a path: node / Claude Code tools, python,
        // coreutils and shells.
        {Kind::Folder, rx(QStringLiteral(R"(EACCES: permission denied, \w+ '(/[^']+)')"))},
        {Kind::Folder, rx(QStringLiteral(R"(\[Errno 13\] Permission denied: '(/[^']+)')"))},
        {Kind::Folder, rx(QStringLiteral(R"([‘'"](/[^‘’'"]+)[’'"][^\n]{0,40}: Permission denied)"))},
        {Kind::Folder, rx(QStringLiteral(R"((?:^|[\s:])(/[^\s:‘’'"]+): Permission denied)"))},
    };
    return list;
}

// The host a command or line points at: a URL's host, git's user@host:path,
// or ssh's user@host.
QString hostIn(const QString &text)
{
    static const QRegularExpression url(
        QStringLiteral(R"([a-z][a-z0-9+.-]*://(?:[^@/\s'"]*@)?(\[[0-9a-f:]+\]|[a-z0-9.-]+))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression scp(
        QStringLiteral(R"((?:^|\s)[a-z0-9._-]+@([a-z0-9.-]+\.[a-z]{2,})(?::|\s|$))"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = url.match(text);
    if (match.hasMatch())
        return match.captured(1);
    match = scp.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString cleanHost(QString host)
{
    while (!host.isEmpty() && QStringLiteral(".,;:)]'\"").contains(host.back()))
        host.chop(1);
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    return host.toLower();
}

bool validHost(const QString &host)
{
    static const QRegularExpression ipv4(QStringLiteral(
        R"(^(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)$)"));
    static const QRegularExpression ipv6(QStringLiteral(R"(^[0-9a-f]*(?::[0-9a-f.]*){2,}$)"));
    static const QRegularExpression label(
        QStringLiteral(R"(^[a-z0-9_](?:[a-z0-9_-]{0,61}[a-z0-9])?$)"));
    if (host.isEmpty() || host.size() > 253)
        return false;
    if (ipv4.match(host).hasMatch() || ipv6.match(host).hasMatch())
        return true;
    const QStringList labels = host.split(QLatin1Char('.'));
    for (const QString &part : labels) {
        if (!label.match(part).hasMatch())
            return false;
    }
    return true;
}

bool quotedTarget(const QString &line, const QRegularExpressionMatch &match)
{
    if (match.lastCapturedIndex() < 1 || match.capturedEnd(1) != match.capturedEnd(0))
        return false;
    const qsizetype end = match.capturedEnd(1);
    if (end >= line.size())
        return false;
    static const QString quoting = QStringLiteral("\"\\&<`");
    return quoting.contains(line.at(end));
}

bool readsFileContent(const QString &toolName)
{
    static const QStringList tools{
        QStringLiteral("Read"),      QStringLiteral("Grep"),
        QStringLiteral("Glob"),      QStringLiteral("LS"),
        QStringLiteral("Edit"),      QStringLiteral("MultiEdit"),
        QStringLiteral("Write"),     QStringLiteral("NotebookEdit"),
        QStringLiteral("NotebookRead"), QStringLiteral("TodoWrite"),
    };
    return tools.contains(toolName);
}

bool writeVerb(const QString &line, const QString &toolName)
{
    static const QRegularExpression verbs(
        QStringLiteral(R"(\b(?:cannot create|cannot remove|cannot touch|cannot move|)"
                       R"(for writing|mkdir|rmdir|unlink|rename|copyfile|symlink|)"
                       R"(write|writing|touch)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return toolName == QLatin1String("Write") || toolName == QLatin1String("Edit") ||
           toolName == QLatin1String("MultiEdit") ||
           toolName == QLatin1String("NotebookEdit") || verbs.match(line).hasMatch();
}

} // namespace

AccessFailure detectAccessFailure(const QString &toolName, const QJsonObject &input,
                                  const QString &output)
{
    AccessFailure none;
    if (output.isEmpty())
        return none;
    QString called = input.value(QStringLiteral("command")).toString();
    if (called.isEmpty())
        called = input.value(QStringLiteral("url")).toString();
    const QString filePath = input.value(QStringLiteral("file_path")).toString();
    const bool bash = toolName == QLatin1String("Bash");
    if (bash && commandOnlyReadsText(called))
        return none;
    const bool reaches = bash || toolName == QLatin1String("WebFetch");
    static const QRegularExpression quotedFromFile(
        QStringLiteral(R"(^(?:[^\s:]*[./][^\s:]*[:-]\d+[:-]|\d+[\t\x{2192}]))"));

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.size() > 4000)
            continue;
        if (quotedFromFile.match(line).hasMatch())
            continue;
        for (const Pattern &pattern : patterns()) {
            if (pattern.kind != Kind::Folder && !reaches)
                continue;
            const QRegularExpressionMatch match = pattern.re.match(line);
            if (!match.hasMatch())
                continue;
            AccessFailure failure;
            failure.kind = pattern.kind;
            failure.evidence = line.left(kMaxEvidence);
            QString target = match.lastCapturedIndex() >= 1 ? match.captured(1) : QString();
            if (pattern.kind == Kind::Folder) {
                if (target.isEmpty())
                    target = filePath;
                if (!target.startsWith(QLatin1Char('/')))
                    continue;
                failure.target = target;
                failure.write = writeVerb(line, toolName);
                return failure;
            }
            if (readsFileContent(toolName) || quotedTarget(line, match))
                continue;
            if (target.isEmpty())
                target = hostIn(line);
            if (target.isEmpty())
                target = hostIn(called);
            target = cleanHost(target);
            if (!validHost(target) || isLocalHost(target))
                continue;
            failure.target = target;
            return failure;
        }
    }
    return none;
}

QString accessFailureKindKey(AccessFailure::Kind kind)
{
    switch (kind) {
    case Kind::Folder:
        return QStringLiteral("folder");
    case Kind::Resolve:
        return QStringLiteral("resolve");
    case Kind::Connect:
        return QStringLiteral("connect");
    case Kind::None:
        break;
    }
    return QString();
}

} // namespace forkmesh::agentnet
