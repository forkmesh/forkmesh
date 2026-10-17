#include "AgentNetworkGate.h"
#include "AgentReadOnlyCommand.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace forkmesh::agentnet {

namespace {

constexpr qsizetype kMaxRequestBytes = 256 * 1024;

// ---- shell command splitting ------------------------------------------------

// Splits a command line into simple commands (on ; & | newlines and the
// inside of $( ) and backticks) and each into words, honouring quotes. Rough
// by design: it only has to find the program and its arguments.
QList<QStringList> simpleCommands(const QString &command,
                                  QList<std::pair<qsizetype, qsizetype>> *spans = nullptr)
{
    QList<QStringList> commands;
    QStringList words;
    QString word;
    bool inWord = false;
    QChar quote;
    qsizetype start = -1;
    qsizetype end = -1;
    auto touch = [&](qsizetype from, qsizetype to) {
        if (start < 0)
            start = from;
        end = qMin(to, command.size());
    };
    auto endWord = [&] {
        if (inWord)
            words.append(word);
        word.clear();
        inWord = false;
    };
    auto endCommand = [&] {
        endWord();
        if (!words.isEmpty()) {
            commands.append(words);
            if (spans)
                spans->append({start, end - start});
        }
        words.clear();
        start = -1;
    };
    for (qsizetype i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (!quote.isNull()) {
            if (c == quote) {
                touch(i, i + 1);
                quote = QChar();
            } else if (c == QLatin1Char('\\') && quote == QLatin1Char('"') &&
                       i + 1 < command.size()) {
                touch(i, i + 2);
                word += command.at(++i);
            } else if (quote == QLatin1Char('"') && c == QLatin1Char('$') &&
                       i + 1 < command.size() && command.at(i + 1) == QLatin1Char('(')) {
                // A command substitution inside double quotes still runs.
                endCommand();
                ++i;
            } else {
                touch(i, i + 1);
                word += c;
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            touch(i, i + 1);
            quote = c;
            inWord = true;
        } else if (c == QLatin1Char('\\') && i + 1 < command.size()) {
            touch(i, i + 2);
            word += command.at(++i);
            inWord = true;
        } else if (c == QLatin1Char('$') && i + 1 < command.size() &&
                   command.at(i + 1) == QLatin1Char('(')) {
            endCommand();
            ++i;
        } else if (c == QLatin1Char('$') && i + 1 < command.size() &&
                   command.at(i + 1) == QLatin1Char('{')) {
            // ${NAME} is part of the word, not a { } group.
            const qsizetype close = command.indexOf(QLatin1Char('}'), i + 2);
            const qsizetype stop = close < 0 ? command.size() : close + 1;
            touch(i, stop);
            word += command.mid(i, stop - i);
            inWord = true;
            i = stop - 1;
        } else if (c == QLatin1Char(';') || c == QLatin1Char('&') ||
                   c == QLatin1Char('|') || c == QLatin1Char('\n') ||
                   c == QLatin1Char('(') || c == QLatin1Char(')') ||
                   c == QLatin1Char('`') || c == QLatin1Char('{') ||
                   c == QLatin1Char('}')) {
            endCommand();
        } else if (c.isSpace()) {
            endWord();
        } else if (c == QLatin1Char('#') && !inWord) {
            // Comment to end of line.
            while (i + 1 < command.size() && command.at(i + 1) != QLatin1Char('\n'))
                ++i;
        } else {
            touch(i, i + 1);
            word += c;
            inWord = true;
        }
    }
    endCommand();
    return commands;
}

QString baseName(const QString &program)
{
    return program.section(QLatin1Char('/'), -1);
}

// Drops leading VAR=value assignments and wrappers that run the real command.
QStringList unwrap(QStringList words)
{
    static const QSet<QString> plainWrappers{
        QStringLiteral("sudo"),    QStringLiteral("doas"),  QStringLiteral("env"),
        QStringLiteral("time"),    QStringLiteral("nice"),  QStringLiteral("nohup"),
        QStringLiteral("exec"),    QStringLiteral("command"), QStringLiteral("builtin"),
        QStringLiteral("xargs"),   QStringLiteral("stdbuf"), QStringLiteral("ionice"),
        QStringLiteral("proxychains"), QStringLiteral("torsocks")};
    static const QRegularExpression assignment(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*="));
    for (int guard = 0; guard < 16 && !words.isEmpty(); ++guard) {
        const QString first = baseName(words.first());
        if (assignment.match(words.first()).hasMatch()) {
            words.removeFirst();
        } else if (plainWrappers.contains(first)) {
            words.removeFirst();
            while (!words.isEmpty() && words.first().startsWith(QLatin1Char('-')))
                words.removeFirst();
        } else if (first == QLatin1String("timeout")) {
            words.removeFirst();
            while (!words.isEmpty() && words.first().startsWith(QLatin1Char('-')))
                words.removeFirst();
            if (!words.isEmpty())
                words.removeFirst(); // the duration
        } else {
            break;
        }
    }
    return words;
}

// The first argument that is not an option, from `from` on.
QString firstOperand(const QStringList &words, int from, int *index = nullptr)
{
    for (int i = from; i < words.size(); ++i) {
        if (words.at(i).startsWith(QLatin1Char('-')))
            continue;
        if (index)
            *index = i;
        return words.at(i);
    }
    return {};
}

QString hostOfUrl(const QString &text)
{
    const QUrl url(text.trimmed());
    const QString host = url.isValid() ? url.host().toLower() : QString();
    // An unexpanded $VAR is not a host.
    return host.contains(QLatin1Char('$')) ? QString() : host;
}

// A URL-looking word's host: scheme URLs, and scp-style user@host:path.
QString hostOfLocation(const QString &word)
{
    if (word.contains(QLatin1String("://")))
        return hostOfUrl(word);
    static const QRegularExpression scpLike(
        QStringLiteral("^(?:[^@/\\s]+@)?([A-Za-z0-9.-]+\\.[A-Za-z]{2,}|[A-Za-z0-9-]+):(?!//)"));
    const QRegularExpressionMatch match = scpLike.match(word);
    if (match.hasMatch() && !word.startsWith(QLatin1Char('/')))
        return match.captured(1).toLower();
    return {};
}

QString firstUrlHost(const QStringList &words, int from)
{
    static const QRegularExpression url(
        QStringLiteral("\\b(?:https?|ftp|wss?)://[^\\s'\"<>]+"),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = from; i < words.size(); ++i) {
        const QRegularExpressionMatch match = url.match(words.at(i));
        if (match.hasMatch())
            return hostOfUrl(match.captured(0));
    }
    return {};
}

// A bare host argument (ssh host, ping host): strips user@ and :path/:port.
QString hostWord(const QString &word)
{
    const QString location = hostOfLocation(word);
    if (!location.isEmpty())
        return location;
    QString host = word.section(QLatin1Char('@'), -1);
    host = host.section(QLatin1Char(':'), 0, 0).section(QLatin1Char('/'), 0, 0);
    if (host.contains(QLatin1Char('$')))
        return {}; // an unexpanded $VAR is not a host
    return host.toLower();
}

// ---- git remotes ------------------------------------------------------------

QString gitCommonDir(const QString &cwd)
{
    if (cwd.isEmpty())
        return {};
    QDir dir(cwd);
    for (int depth = 0; depth < 64; ++depth) {
        const QFileInfo marker(dir.filePath(QStringLiteral(".git")));
        if (marker.isDir())
            return marker.absoluteFilePath();
        if (marker.isFile()) {
            QFile file(marker.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                return {};
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (!line.startsWith(QLatin1String("gitdir:")))
                return {};
            QString gitDir = line.mid(7).trimmed();
            if (QDir::isRelativePath(gitDir))
                gitDir = dir.absoluteFilePath(gitDir);
            QFile common(QDir(gitDir).filePath(QStringLiteral("commondir")));
            if (common.open(QIODevice::ReadOnly)) {
                QString commonDir = QString::fromUtf8(common.readAll()).trimmed();
                if (QDir::isRelativePath(commonDir))
                    commonDir = QDir(gitDir).absoluteFilePath(commonDir);
                return QDir::cleanPath(commonDir);
            }
            return QDir::cleanPath(gitDir);
        }
        if (!dir.cdUp())
            break;
    }
    return {};
}

QString gitRemoteUrl(const QString &cwd, const QString &remote, bool push)
{
    const QString gitDir = gitCommonDir(cwd);
    if (gitDir.isEmpty())
        return {};
    QFile file(QDir(gitDir).filePath(QStringLiteral("config")));
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    static const QRegularExpression section(QStringLiteral("^\\s*\\[(.*)\\]\\s*$"));
    static const QRegularExpression urlLine(
        QStringLiteral("^\\s*(url|pushurl)\\s*=\\s*(.*?)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString wanted = QStringLiteral("remote \"%1\"").arg(remote);
    bool inRemote = false;
    QString fetchUrl;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        const QRegularExpressionMatch header = section.match(line);
        if (header.hasMatch()) {
            inRemote = header.captured(1).trimmed() == wanted;
            continue;
        }
        if (!inRemote)
            continue;
        const QRegularExpressionMatch url = urlLine.match(line);
        if (url.hasMatch()) {
            QString value = url.captured(2);
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) &&
                value.endsWith(QLatin1Char('"')))
                value = value.mid(1, value.size() - 2);
            const bool pushUrl =
                url.captured(1).compare(QLatin1String("pushurl"), Qt::CaseInsensitive) == 0;
            // A push goes to pushurl when there is one; everything else to url.
            if (pushUrl && push)
                return value;
            if (!pushUrl && fetchUrl.isEmpty())
                fetchUrl = value;
        }
    }
    return fetchUrl;
}

bool isLocalLocation(const QString &location)
{
    return location.startsWith(QLatin1Char('/')) || location.startsWith(QLatin1Char('.')) ||
           location.startsWith(QLatin1Char('~')) ||
           location.startsWith(QLatin1String("file:"), Qt::CaseInsensitive);
}

// Where a git remote argument (a name, URL or path) points: a host, empty
// for local, or `fallback` when it cannot be told.
ExternalCall gitRemoteCall(const QString &remoteArg, const QString &cwd,
                           const QString &summary, bool push = false)
{
    QString location = remoteArg;
    if (!location.isEmpty() && !location.contains(QLatin1String("://")) &&
        !location.contains(QLatin1Char(':')) && !isLocalLocation(location)) {
        const QString url = gitRemoteUrl(cwd, location, push);
        if (url.isEmpty()) {
            // Unknown remote name: assume it leaves the machine.
            return {true, QStringLiteral("git remote %1").arg(location), summary};
        }
        location = url;
    }
    if (location.isEmpty() || isLocalLocation(location))
        return {};
    const QString host = hostOfLocation(location);
    if (host.isEmpty())
        return {};
    if (isLocalHost(host))
        return {};
    return {true, host, summary};
}

// ---- per-program rules --------------------------------------------------------

QString shortSummary(const QStringList &words)
{
    QString text = words.join(QLatin1Char(' '));
    if (text.size() > 200)
        text = text.left(199) + QChar(0x2026);
    return text;
}

ExternalCall packageCall(const QString &registry, const QStringList &words)
{
    return {true, registry, shortSummary(words)};
}

ExternalCall scriptNetworkCall(const QString &script, const QString &cwd, const QString &summary)
{
    if (script.isEmpty() || script.contains(QLatin1Char('$')))
        return {};
    QString path = script;
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    const QFileInfo info(QDir(cwd.isEmpty() ? QDir::currentPath() : cwd).absoluteFilePath(path));
    if (!info.isFile() || info.size() > 2 * 1024 * 1024)
        return {};
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression pythonNetwork(QStringLiteral(
        "^\\s*(?:import\\s+(?:urllib\\.request|urllib3|requests|httpx|aiohttp|http\\.client|"
        "smtplib|ftplib|websockets?|paramiko)\\b|from\\s+(?:urllib\\.request|urllib3|requests|"
        "httpx|aiohttp|http\\.client|smtplib|ftplib|websockets?|paramiko)\\b|from\\s+urllib\\s+"
        "import\\s+[^\\n]*\\brequest\\b)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression nodeNetwork(QStringLiteral(
        "(?:require\\(\\s*['\"](?:node:)?(?:https?|net|tls|undici|axios|node-fetch)['\"]\\s*\\)|"
        "from\\s+['\"](?:node:)?(?:https?|net|tls|undici|axios|node-fetch)['\"]|\\bfetch\\s*\\()"));
    const bool node = info.suffix() == QLatin1String("js") || info.suffix() == QLatin1String("mjs") ||
                      info.suffix() == QLatin1String("cjs") || info.suffix() == QLatin1String("ts");
    if (!(node ? nodeNetwork : pythonNetwork).match(text).hasMatch())
        return {};
    static const QRegularExpression url(QStringLiteral("\\bhttps?://([A-Za-z0-9.-]+)"));
    bool sawLocal = false;
    auto it = url.globalMatch(text);
    while (it.hasNext()) {
        const QString host = it.next().captured(1).toLower();
        if (isLocalHost(host))
            sawLocal = true;
        else if (host.contains(QLatin1Char('.')))
            return {true, host, summary};
    }
    if (sawLocal)
        return {};
    return {true, QStringLiteral("%1 network code").arg(info.fileName()), summary};
}

ExternalCall classifyWords(const QStringList &raw, const QString &cwd)
{
    const QStringList words = unwrap(raw);
    if (words.isEmpty())
        return {};
    const QString program = baseName(words.first()).toLower();
    const QString sub = firstOperand(words, 1);
    const QString summary = shortSummary(words);

    auto withUrlHost = [&](const QString &fallback) -> ExternalCall {
        const QString host = firstUrlHost(words, 1);
        if (!host.isEmpty())
            return isLocalHost(host) ? ExternalCall{} : ExternalCall{true, host, summary};
        return {true, fallback, summary};
    };

    static const QSet<QString> urlClients{
        QStringLiteral("curl"),   QStringLiteral("wget"),  QStringLiteral("wget2"),
        QStringLiteral("http"),   QStringLiteral("https"), QStringLiteral("xh"),
        QStringLiteral("aria2c"), QStringLiteral("lynx"),  QStringLiteral("w3m"),
        QStringLiteral("websocat"), QStringLiteral("grpcurl")};
    if (urlClients.contains(program)) {
        const QString host = firstUrlHost(words, 1);
        if (!host.isEmpty())
            return isLocalHost(host) ? ExternalCall{} : ExternalCall{true, host, summary};
        // curl example.com, http :8080/path, wget host/file
        for (int i = 1; i < words.size(); ++i) {
            const QString &word = words.at(i);
            if (word.startsWith(QLatin1Char('-')))
                continue;
            if (word.startsWith(QLatin1Char(':')))
                return {}; // httpie shorthand for localhost
            const QString candidate = hostWord(word);
            if (candidate.contains(QLatin1Char('.')) || isLocalHost(candidate))
                return isLocalHost(candidate) ? ExternalCall{}
                                              : ExternalCall{true, candidate, summary};
        }
        return {true, program, summary};
    }

    static const QSet<QString> hostClients{
        QStringLiteral("ssh"),    QStringLiteral("mosh"),     QStringLiteral("telnet"),
        QStringLiteral("nc"),     QStringLiteral("ncat"),     QStringLiteral("netcat"),
        QStringLiteral("ping"),   QStringLiteral("ping6"),    QStringLiteral("dig"),
        QStringLiteral("nslookup"), QStringLiteral("host"),   QStringLiteral("whois"),
        QStringLiteral("traceroute"), QStringLiteral("ftp"),  QStringLiteral("sftp"),
        QStringLiteral("openssl")};
    if (hostClients.contains(program)) {
        if (program == QLatin1String("openssl") && sub != QLatin1String("s_client"))
            return {};
        QString target;
        for (int i = 1; i < words.size(); ++i) {
            const QString &word = words.at(i);
            if (word.startsWith(QLatin1Char('-')) || word.startsWith(QLatin1Char('@')))
                continue;
            if (program == QLatin1String("openssl") && word == QLatin1String("s_client"))
                continue;
            target = hostWord(word);
            break;
        }
        if (program == QLatin1String("openssl")) {
            const int connect = words.indexOf(QStringLiteral("-connect"));
            if (connect >= 0 && connect + 1 < words.size())
                target = hostWord(words.at(connect + 1));
        }
        if (!target.isEmpty() && isLocalHost(target))
            return {};
        return {true, target.isEmpty() ? program : target, summary};
    }

    if (program == QLatin1String("scp") || program == QLatin1String("rsync")) {
        for (int i = 1; i < words.size(); ++i) {
            const QString host = hostOfLocation(words.at(i));
            if (!host.isEmpty())
                return isLocalHost(host) ? ExternalCall{} : ExternalCall{true, host, summary};
        }
        return {};
    }

    if (program == QLatin1String("git")) {
        // Skip git's own options (-C dir, -c key=value, --git-dir=...).
        int i = 1;
        QString gitCwd = cwd;
        while (i < words.size() && words.at(i).startsWith(QLatin1Char('-'))) {
            const QString &option = words.at(i);
            if ((option == QLatin1String("-C") || option == QLatin1String("-c")) &&
                i + 1 < words.size()) {
                if (option == QLatin1String("-C"))
                    gitCwd = QDir(cwd).absoluteFilePath(words.at(i + 1));
                i += 2;
            } else {
                ++i;
            }
        }
        if (i >= words.size())
            return {};
        const QString gitSub = words.at(i);
        int operandIndex = -1;
        const QString operand = firstOperand(words, i + 1, &operandIndex);
        if (gitSub == QLatin1String("clone"))
            return gitRemoteCall(operand, gitCwd, summary);
        if (gitSub == QLatin1String("fetch") || gitSub == QLatin1String("pull") ||
            gitSub == QLatin1String("push") || gitSub == QLatin1String("ls-remote")) {
            if (words.mid(i + 1).contains(QStringLiteral("--all")))
                return {true, QStringLiteral("all git remotes"), summary};
            return gitRemoteCall(operand.isEmpty() ? QStringLiteral("origin") : operand,
                                 gitCwd, summary, gitSub == QLatin1String("push"));
        }
        if (gitSub == QLatin1String("remote") && (operand == QLatin1String("update") ||
                                                  operand == QLatin1String("show")))
            return {true, QStringLiteral("all git remotes"), summary};
        if (gitSub == QLatin1String("submodule") &&
            (operand == QLatin1String("update") || operand == QLatin1String("sync")))
            return {true, QStringLiteral("git submodule remotes"), summary};
        if (gitSub == QLatin1String("send-email"))
            return {true, QStringLiteral("email"), summary};
        return {};
    }

    static const QSet<QString> alwaysRemote{
        QStringLiteral("gh"),     QStringLiteral("glab"),    QStringLiteral("hub"),
        QStringLiteral("wrangler"), QStringLiteral("aws"),   QStringLiteral("gcloud"),
        QStringLiteral("gsutil"), QStringLiteral("az"),      QStringLiteral("doctl"),
        QStringLiteral("flyctl"), QStringLiteral("fly"),     QStringLiteral("vercel"),
        QStringLiteral("netlify"), QStringLiteral("heroku"), QStringLiteral("twine"),
        QStringLiteral("vultr-cli"), QStringLiteral("hcloud"), QStringLiteral("stripe")};
    if (alwaysRemote.contains(program)) {
        if (sub == QLatin1String("help") || sub == QLatin1String("--help") ||
            sub == QLatin1String("--version") || words.contains(QStringLiteral("--help")) ||
            words.contains(QStringLiteral("--version")))
            return {};
        static const QHash<QString, QString> hosts{
            {QStringLiteral("gh"), QStringLiteral("github.com")},
            {QStringLiteral("hub"), QStringLiteral("github.com")},
            {QStringLiteral("glab"), QStringLiteral("gitlab.com")},
            {QStringLiteral("wrangler"), QStringLiteral("api.cloudflare.com")},
            {QStringLiteral("twine"), QStringLiteral("upload.pypi.org")},
            {QStringLiteral("vercel"), QStringLiteral("vercel.com")},
            {QStringLiteral("netlify"), QStringLiteral("netlify.com")}};
        return withUrlHost(hosts.value(program, program));
    }

    // Package managers: only the subcommands that download or publish.
    static const QSet<QString> nodeManagers{QStringLiteral("npm"), QStringLiteral("pnpm"),
                                            QStringLiteral("yarn"), QStringLiteral("bun")};
    if (nodeManagers.contains(program)) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("i"),     QStringLiteral("ci"),
            QStringLiteral("add"),     QStringLiteral("update"), QStringLiteral("upgrade"),
            QStringLiteral("up"),      QStringLiteral("publish"), QStringLiteral("login"),
            QStringLiteral("view"),    QStringLiteral("info"),   QStringLiteral("outdated"),
            QStringLiteral("dlx"),     QStringLiteral("create"), QStringLiteral("audit")};
        // `yarn` alone installs.
        if (remoteSubs.contains(sub) || (program == QLatin1String("yarn") && sub.isEmpty()))
            return packageCall(QStringLiteral("registry.npmjs.org"), words);
        return {};
    }
    if (program == QLatin1String("npx") || program == QLatin1String("bunx")) {
        // Runs the local copy when installed, otherwise downloads it.
        const QString tool = firstOperand(words, 1);
        if (!tool.isEmpty() &&
            QFileInfo::exists(QDir(cwd).filePath(QStringLiteral("node_modules/.bin/") + tool)))
            return {};
        return packageCall(QStringLiteral("registry.npmjs.org"), words);
    }
    static const QSet<QString> pythonManagers{
        QStringLiteral("pip"),  QStringLiteral("pip3"),   QStringLiteral("uv"),
        QStringLiteral("pipx"), QStringLiteral("poetry"), QStringLiteral("pdm"),
        QStringLiteral("conda"), QStringLiteral("mamba")};
    if (pythonManagers.contains(program)) {
        QString action = sub;
        if ((program == QLatin1String("uv") && sub == QLatin1String("pip")) ||
            (program == QLatin1String("uv") && sub == QLatin1String("tool")))
            action = firstOperand(words, words.indexOf(sub) + 1);
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("download"), QStringLiteral("add"),
            QStringLiteral("sync"),    QStringLiteral("lock"),     QStringLiteral("publish"),
            QStringLiteral("update"),  QStringLiteral("upgrade"),  QStringLiteral("create")};
        // `pipx run` downloads the tool; `uv run` and `poetry run` run local code.
        if (remoteSubs.contains(action) ||
            (program == QLatin1String("pipx") && action == QLatin1String("run")))
            return packageCall(QStringLiteral("pypi.org"), words);
        return {};
    }
    if (program == QLatin1String("uvx"))
        return packageCall(QStringLiteral("pypi.org"), words);
    if (program.startsWith(QLatin1String("python"))) {
        const int module = words.indexOf(QStringLiteral("-m"));
        if (module >= 0 && module + 2 < words.size() &&
            (words.at(module + 1) == QLatin1String("pip") ||
             words.at(module + 1) == QLatin1String("ensurepip")) &&
            (words.at(module + 2) == QLatin1String("install") ||
             words.at(module + 2) == QLatin1String("download")))
            return packageCall(QStringLiteral("pypi.org"), words);
        if (module >= 0 && module + 1 < words.size() &&
            words.at(module + 1) == QLatin1String("http.server"))
            return {};
        const int inlineCode = words.indexOf(QStringLiteral("-c"));
        if (inlineCode >= 0 && inlineCode + 1 < words.size()) {
            static const QRegularExpression networkCode(
                QStringLiteral("\\b(urllib|requests|httpx|aiohttp|http\\.client|"
                               "socket\\.create_connection|smtplib|ftplib)\\b"));
            const QString code = words.at(inlineCode + 1);
            if (networkCode.match(code).hasMatch())
                return withUrlHost(QStringLiteral("python network code"));
            return {};
        }
        if (module >= 0)
            return {};
        return scriptNetworkCall(firstOperand(words, 1), cwd, summary);
    }
    if (program == QLatin1String("node") || program == QLatin1String("deno") ||
        program == QLatin1String("tsx")) {
        const QString script = firstOperand(words, 1);
        if (script == QLatin1String("run") && program == QLatin1String("deno"))
            return scriptNetworkCall(firstOperand(words, 2), cwd, summary);
        return scriptNetworkCall(script, cwd, summary);
    }
    if (words.first().contains(QLatin1Char('/')) &&
        (program.endsWith(QLatin1String(".py")) || program.endsWith(QLatin1String(".js")) ||
         program.endsWith(QLatin1String(".mjs"))))
        return scriptNetworkCall(words.first(), cwd, summary);
    if (program == QLatin1String("cargo")) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("fetch"),  QStringLiteral("update"),
            QStringLiteral("publish"), QStringLiteral("add"),    QStringLiteral("search"),
            QStringLiteral("login"),   QStringLiteral("yank"),   QStringLiteral("owner")};
        return remoteSubs.contains(sub) ? packageCall(QStringLiteral("crates.io"), words)
                                        : ExternalCall{};
    }
    if (program == QLatin1String("go")) {
        if (sub == QLatin1String("get") || sub == QLatin1String("install") ||
            (sub == QLatin1String("mod") && (words.contains(QStringLiteral("download")) ||
                                             words.contains(QStringLiteral("tidy")))))
            return packageCall(QStringLiteral("proxy.golang.org"), words);
        return {};
    }
    if (program == QLatin1String("gem") || program == QLatin1String("bundle") ||
        program == QLatin1String("bundler")) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("update"), QStringLiteral("push"),
            QStringLiteral("fetch"),   QStringLiteral("add"),    QStringLiteral("outdated")};
        if (remoteSubs.contains(sub) ||
            (program != QLatin1String("gem") && sub.isEmpty()))
            return packageCall(QStringLiteral("rubygems.org"), words);
        return {};
    }
    if (program == QLatin1String("composer")) {
        static const QSet<QString> remoteSubs{QStringLiteral("install"),
                                              QStringLiteral("require"),
                                              QStringLiteral("update")};
        return remoteSubs.contains(sub) ? packageCall(QStringLiteral("packagist.org"), words)
                                        : ExternalCall{};
    }
    if (program == QLatin1String("dart")) {
        const QString action = sub == QLatin1String("pub") ? firstOperand(words, 2) : sub;
        static const QSet<QString> remoteSubs{
            QStringLiteral("get"),     QStringLiteral("upgrade"), QStringLiteral("add"),
            QStringLiteral("publish"), QStringLiteral("downgrade"), QStringLiteral("outdated")};
        if ((sub == QLatin1String("pub") || sub == QLatin1String("create")) &&
            (remoteSubs.contains(action) || sub == QLatin1String("create")))
            return packageCall(QStringLiteral("pub.dev"), words);
        return {};
    }
    if (program == QLatin1String("brew")) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("update"), QStringLiteral("upgrade"),
            QStringLiteral("fetch"),   QStringLiteral("tap"),    QStringLiteral("reinstall")};
        return remoteSubs.contains(sub) ? packageCall(QStringLiteral("formulae.brew.sh"), words)
                                        : ExternalCall{};
    }
    static const QSet<QString> systemManagers{
        QStringLiteral("apt"),    QStringLiteral("apt-get"), QStringLiteral("dnf"),
        QStringLiteral("yum"),    QStringLiteral("zypper"),  QStringLiteral("apk"),
        QStringLiteral("pacman"), QStringLiteral("snap"),    QStringLiteral("flatpak")};
    if (systemManagers.contains(program)) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("install"), QStringLiteral("update"), QStringLiteral("upgrade"),
            QStringLiteral("download"), QStringLiteral("add"),   QStringLiteral("refresh"),
            QStringLiteral("full-upgrade"), QStringLiteral("dist-upgrade"),
            QStringLiteral("-S"), QStringLiteral("-Sy"), QStringLiteral("-Syu")};
        for (int i = 1; i < words.size(); ++i)
            if (remoteSubs.contains(words.at(i)))
                return packageCall(QStringLiteral("system package repositories"), words);
        return {};
    }
    if (program == QLatin1String("docker") || program == QLatin1String("podman")) {
        static const QSet<QString> remoteSubs{
            QStringLiteral("pull"), QStringLiteral("push"), QStringLiteral("login"),
            QStringLiteral("search")};
        return remoteSubs.contains(sub) ? packageCall(QStringLiteral("container registry"), words)
                                        : ExternalCall{};
    }
    return {};
}

} // namespace

bool isLocalHost(const QString &host)
{
    QString value = host.trimmed().toLower();
    if (value.startsWith(QLatin1Char('[')) && value.endsWith(QLatin1Char(']')))
        value = value.mid(1, value.size() - 2);
    while (value.endsWith(QLatin1Char('.')))
        value.chop(1);
    if (value == QLatin1String("localhost") || value.endsWith(QLatin1String(".localhost")) ||
        value == QLatin1String("0.0.0.0"))
        return true;
    const QHostAddress address(value);
    return !address.isNull() && address.isLoopback();
}

namespace {

// Replaces $NAME and ${NAME} with values assigned earlier in the command line.
QString expandVariables(const QString &word, const QHash<QString, QString> &variables)
{
    if (variables.isEmpty() || !word.contains(QLatin1Char('$')))
        return word;
    static const QRegularExpression reference(
        QStringLiteral("\\$(?:\\{([A-Za-z_][A-Za-z0-9_]*)\\}|([A-Za-z_][A-Za-z0-9_]*))"));
    QString out;
    qsizetype from = 0;
    auto it = reference.globalMatch(word);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString name =
            match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        out += word.mid(from, match.capturedStart() - from);
        out += variables.contains(name) ? variables.value(name) : match.captured(0);
        from = match.capturedEnd();
    }
    return out + word.mid(from);
}

// Records a command made only of NAME=value words (optionally after export,
// local, readonly or declare). Returns whether the command was one.
bool recordAssignments(const QStringList &words, QHash<QString, QString> *variables)
{
    static const QRegularExpression assignment(QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)=(.*)$"),
                                               QRegularExpression::DotMatchesEverythingOption);
    static const QSet<QString> declarers{QStringLiteral("export"), QStringLiteral("local"),
                                         QStringLiteral("readonly"), QStringLiteral("declare")};
    int i = !words.isEmpty() && declarers.contains(words.first()) ? 1 : 0;
    while (i < words.size() && words.at(i).startsWith(QLatin1Char('-')))
        ++i;
    if (i >= words.size())
        return false;
    QHash<QString, QString> found;
    for (; i < words.size(); ++i) {
        const QRegularExpressionMatch match = assignment.match(words.at(i));
        if (!match.hasMatch())
            return false;
        found.insert(match.captured(1), match.captured(2));
    }
    variables->insert(found);
    return true;
}

} // namespace

bool commandOnlyReadsText(const QString &command)
{
    static const QSet<QString> readers{
        QStringLiteral("cat"),   QStringLiteral("tac"),   QStringLiteral("nl"),
        QStringLiteral("head"),  QStringLiteral("tail"),  QStringLiteral("less"),
        QStringLiteral("more"),  QStringLiteral("bat"),   QStringLiteral("batcat"),
        QStringLiteral("grep"),  QStringLiteral("egrep"), QStringLiteral("fgrep"),
        QStringLiteral("rg"),    QStringLiteral("ag"),    QStringLiteral("ack"),
        QStringLiteral("ugrep"), QStringLiteral("sed"),   QStringLiteral("awk"),
        QStringLiteral("gawk"),  QStringLiteral("cut"),   QStringLiteral("sort"),
        QStringLiteral("uniq"),  QStringLiteral("wc"),    QStringLiteral("tr"),
        QStringLiteral("ls"),    QStringLiteral("find"),  QStringLiteral("fd"),
        QStringLiteral("tree"),  QStringLiteral("diff"),  QStringLiteral("jq"),
        QStringLiteral("echo"),  QStringLiteral("printf"), QStringLiteral("cd"),
        QStringLiteral("pwd"),   QStringLiteral("true"),  QStringLiteral("column"),
        QStringLiteral("xxd"),   QStringLiteral("od"),    QStringLiteral("strings"),
        QStringLiteral("file"),  QStringLiteral("stat"),  QStringLiteral("realpath")};
    static const QSet<QString> gitReaders{
        QStringLiteral("grep"),  QStringLiteral("show"),   QStringLiteral("diff"),
        QStringLiteral("log"),   QStringLiteral("blame"),  QStringLiteral("status"),
        QStringLiteral("cat-file"), QStringLiteral("ls-files"),
        QStringLiteral("ls-tree"), QStringLiteral("rev-parse"),
        QStringLiteral("branch"), QStringLiteral("shortlog")};
    static const QRegularExpression redirection(QStringLiteral("^(?:\\d*[<>]|\\d+$)"));
    bool any = false;
    for (const QStringList &raw : simpleCommands(command)) {
        if (redirection.match(raw.first()).hasMatch())
            continue;
        const QStringList words = unwrap(raw);
        if (words.isEmpty())
            continue;
        const QString program = baseName(words.first());
        if (program == QLatin1String("git")) {
            int at = 1;
            while (at < words.size() && words.at(at).startsWith(QLatin1Char('-'))) {
                const QString &option = words.at(at);
                at += option == QLatin1String("-C") || option == QLatin1String("-c") ? 2 : 1;
            }
            if (at >= words.size() || !gitReaders.contains(words.at(at)))
                return false;
        } else if (!readers.contains(program)) {
            return false;
        }
        any = true;
    }
    return any;
}

ExternalCall classifyCommand(const QString &command, const QString &cwd)
{
    ExternalCall first;
    QHash<QString, QString> variables;
    for (QStringList words : simpleCommands(command)) {
        for (QString &word : words)
            word = expandVariables(word, variables);
        if (recordAssignments(words, &variables))
            continue;
        ExternalCall call = classifyWords(words, cwd);
        if (!call.external)
            continue;
        if (!first.external)
            first = call;
    }
    if (first.external) {
        // The summary names the whole command line, shortened.
        QString line = command.simplified();
        if (line.size() > 300)
            line = line.left(299) + QChar(0x2026);
        first.summary = line;
    }
    return first;
}

CommandRisk classifyCommandRisk(const QString &command, const QString &scratch)
{
    CommandRisk risk;
    risk.command = command;
    std::pair<qsizetype, qsizetype> span{-1, 0};
    bool other = false;
    auto note = [&](QString *slot, const QString &value, bool remove = false) {
        if (slot == &risk.destructive) {
            if (remove)
                risk.removeSpans.append(span);
            else
                other = true;
        }
        if (!slot->isEmpty())
            return;
        *slot = value;
        if (slot == &risk.destructive) {
            risk.matchStart = span.first;
            risk.matchLength = span.second;
        }
    };
    auto hasShortFlag = [](const QStringList &words, int from, QChar flag) {
        for (int i = from; i < words.size(); ++i) {
            const QString &word = words.at(i);
            if (word == QLatin1String("--"))
                break;
            if (word.size() > 1 && word.startsWith(QLatin1Char('-')) &&
                !word.startsWith(QLatin1String("--")) && word.mid(1).contains(flag))
                return true;
        }
        return false;
    };
    auto onlyScratch = [](const QStringList &words, int from) {
        bool any = false;
        for (int i = from; i < words.size(); ++i) {
            const QString &word = words.at(i);
            if (word.startsWith(QLatin1Char('-')))
                continue;
            any = true;
            const QString clean = QDir::cleanPath(word);
            const bool scratch =
                (clean.startsWith(QLatin1String("/tmp/")) ||
                 clean.startsWith(QLatin1String("$TMPDIR/")) ||
                 clean.startsWith(QLatin1String("${TMPDIR}/"))) &&
                !clean.contains(QLatin1String(".."));
            if (!scratch)
                return false;
        }
        return any;
    };
    int contained = scratch.isEmpty() ? 0 : -1;
    std::pair<qsizetype, qsizetype> scratchSpan{-1, 0};
    QList<std::pair<qsizetype, qsizetype>> spans;
    const QList<QStringList> commands = simpleCommands(command, &spans);
    for (qsizetype index = 0; index < commands.size(); ++index) {
        span = spans.at(index);
        const QStringList words = unwrap(commands.at(index));
        if (words.isEmpty())
            continue;
        const QString program = baseName(words.first()).toLower();
        if (program == QLatin1String("rm") &&
            (hasShortFlag(words, 1, QLatin1Char('r')) || hasShortFlag(words, 1, QLatin1Char('R')) ||
             words.contains(QStringLiteral("--recursive"))) &&
            !onlyScratch(words, 1)) {
            if (contained < 0)
                contained = forkmesh::commandvars::deletesOnlyInside(command, scratch) ? 1 : 0;
            if (contained == 1) {
                if (scratchSpan.first < 0)
                    scratchSpan = span;
                risk.removeSpans.append(span);
                continue;
            }
            note(&risk.destructive, QStringLiteral("recursive delete"), true);
        } else if (program == QLatin1String("find") &&
                   (words.contains(QStringLiteral("-delete")) ||
                    (words.contains(QStringLiteral("-exec")) &&
                     words.contains(QStringLiteral("rm"))))) {
            note(&risk.destructive, QStringLiteral("recursive delete"));
        } else if (program == QLatin1String("shred") || program == QLatin1String("wipefs") ||
                   program.startsWith(QLatin1String("mkfs")) ||
                   (program == QLatin1String("dd") &&
                    std::any_of(words.cbegin(), words.cend(), [](const QString &word) {
                        return word.startsWith(QLatin1String("of=/dev/"));
                    }))) {
            note(&risk.destructive, QStringLiteral("disk wipe"));
        } else if (program.startsWith(QLatin1String("python"))) {
            const int inlineCode = words.indexOf(QStringLiteral("-c"));
            if (inlineCode >= 0 && inlineCode + 1 < words.size() &&
                words.at(inlineCode + 1).contains(QLatin1String("rmtree")))
                note(&risk.destructive, QStringLiteral("recursive delete"));
        } else if (program == QLatin1String("git")) {
            int i = 1;
            while (i < words.size() && words.at(i).startsWith(QLatin1Char('-')))
                i += words.at(i) == QLatin1String("-C") || words.at(i) == QLatin1String("-c") ? 2
                                                                                            : 1;
            if (i >= words.size())
                continue;
            const QString sub = words.at(i);
            const QStringList rest = words.mid(i + 1);
            const auto has = [&rest](const char *option) {
                return rest.contains(QLatin1String(option));
            };
            if (sub == QLatin1String("push")) {
                note(&risk.permission, QStringLiteral("create-branches"));
                const bool forced =
                    has("--force") || has("--mirror") || has("--delete") ||
                    hasShortFlag(rest, 0, QLatin1Char('f')) ||
                    hasShortFlag(rest, 0, QLatin1Char('d')) ||
                    std::any_of(rest.cbegin(), rest.cend(), [](const QString &word) {
                        return word.startsWith(QLatin1String("--force-with-lease")) ||
                               ((word.startsWith(QLatin1Char('+')) ||
                                 word.startsWith(QLatin1Char(':'))) &&
                                word.size() > 1);
                    });
                if (forced)
                    note(&risk.destructive, QStringLiteral("force push"));
            } else if (sub == QLatin1String("reset") && has("--hard")) {
                note(&risk.destructive, QStringLiteral("discard changes"));
            } else if (sub == QLatin1String("clean") &&
                       (hasShortFlag(rest, 0, QLatin1Char('f')) || has("--force"))) {
                note(&risk.destructive, QStringLiteral("delete untracked files"));
            } else if (sub == QLatin1String("branch")) {
                if (has("-D") || (has("--delete") && has("--force")) ||
                    (hasShortFlag(rest, 0, QLatin1Char('d')) && has("-f")))
                    note(&risk.destructive, QStringLiteral("branch delete"));
                static const QSet<QString> listing{
                    QStringLiteral("-a"), QStringLiteral("-r"), QStringLiteral("-l"),
                    QStringLiteral("-v"), QStringLiteral("-vv"), QStringLiteral("--list"),
                    QStringLiteral("--all"), QStringLiteral("--remotes"),
                    QStringLiteral("--show-current"), QStringLiteral("--contains"),
                    QStringLiteral("--merged"), QStringLiteral("--no-merged"),
                    QStringLiteral("-d"), QStringLiteral("-D"), QStringLiteral("--delete"),
                    QStringLiteral("--format"), QStringLiteral("--sort"),
                    QStringLiteral("--points-at"), QStringLiteral("-m"), QStringLiteral("-M"),
                    QStringLiteral("-u"), QStringLiteral("--set-upstream-to"),
                    QStringLiteral("--unset-upstream"), QStringLiteral("--edit-description")};
                const bool creates =
                    !firstOperand(rest, 0).isEmpty() &&
                    std::none_of(rest.cbegin(), rest.cend(), [](const QString &word) {
                        return listing.contains(word.section(QLatin1Char('='), 0, 0));
                    });
                if (creates)
                    note(&risk.permission, QStringLiteral("create-branches"));
            } else if ((sub == QLatin1String("checkout") &&
                        (has("-b") || has("-B") || has("--orphan"))) ||
                       (sub == QLatin1String("switch") &&
                        (has("-c") || has("-C") || has("--create") || has("--orphan"))) ||
                       (sub == QLatin1String("worktree") && firstOperand(rest, 0) ==
                                                                QLatin1String("add"))) {
                note(&risk.permission, QStringLiteral("create-branches"));
            } else if (sub == QLatin1String("stash") &&
                       (firstOperand(rest, 0) == QLatin1String("drop") ||
                        firstOperand(rest, 0) == QLatin1String("clear"))) {
                note(&risk.destructive, QStringLiteral("stash drop"));
            } else if (sub == QLatin1String("filter-branch") ||
                       sub == QLatin1String("filter-repo") ||
                       (sub == QLatin1String("reflog") &&
                        firstOperand(rest, 0) == QLatin1String("expire")) ||
                       (sub == QLatin1String("gc") &&
                        std::any_of(rest.cbegin(), rest.cend(), [](const QString &word) {
                            return word.startsWith(QLatin1String("--prune=now"));
                        })) ||
                       (sub == QLatin1String("update-ref") && has("-d"))) {
                note(&risk.destructive, QStringLiteral("history rewrite"));
            }
        } else if (program == QLatin1String("gh") || program == QLatin1String("glab")) {
            const QString area = firstOperand(words, 1);
            int areaIndex = words.indexOf(area);
            const QString action = firstOperand(words, areaIndex + 1);
            static const QSet<QString> issueWrites{
                QStringLiteral("create"), QStringLiteral("comment"), QStringLiteral("edit"),
                QStringLiteral("close"),  QStringLiteral("reopen"),  QStringLiteral("delete"),
                QStringLiteral("transfer"), QStringLiteral("pin"),   QStringLiteral("unpin"),
                QStringLiteral("lock"),   QStringLiteral("unlock"),  QStringLiteral("note")};
            static const QSet<QString> reviewWrites{
                QStringLiteral("review"), QStringLiteral("comment"), QStringLiteral("edit"),
                QStringLiteral("close"),  QStringLiteral("reopen"),  QStringLiteral("ready"),
                QStringLiteral("approve"), QStringLiteral("note")};
            if (area == QLatin1String("issue") && issueWrites.contains(action)) {
                note(&risk.permission, QStringLiteral("manage-issues"));
            } else if (area == QLatin1String("pr") || area == QLatin1String("mr")) {
                if (action == QLatin1String("merge"))
                    note(&risk.permission, QStringLiteral("merge"));
                else if (action == QLatin1String("create"))
                    note(&risk.permission, QStringLiteral("create-branches"));
                else if (reviewWrites.contains(action))
                    note(&risk.permission, QStringLiteral("review-prs"));
            } else if (area == QLatin1String("repo") && action == QLatin1String("delete")) {
                note(&risk.destructive, QStringLiteral("repository delete"));
            }
        }
    }
    if (risk.destructive.isEmpty() && scratchSpan.first >= 0) {
        span = scratchSpan;
        risk.destructive = QStringLiteral("scratch delete");
        risk.matchStart = span.first;
        risk.matchLength = span.second;
    }
    risk.onlyRemoves = !other && !risk.removeSpans.isEmpty();
    if (!risk.destructive.isEmpty() || !risk.permission.isEmpty()) {
        QString line = command.simplified();
        if (line.size() > 300)
            line = line.left(299) + QChar(0x2026);
        risk.summary = line;
    }
    return risk;
}

CommandRisk classifyCommandRisk(const QString &command,
                                const forkmesh::commandvars::Environment &environment)
{
    const QString scratch = forkmesh::commandvars::trustedScratch(environment);
    CommandRisk risk = classifyCommandRisk(command, scratch);
    risk.scratch = scratch;
    if (risk.onlyRemoves)
        risk.removedPaths =
            forkmesh::commandvars::removedPaths(command, environment, risk.removeSpans);
    return risk;
}

ExternalCall classifyToolCall(const QString &toolName, const QJsonObject &input,
                              const QString &cwd)
{
    if (toolName == QLatin1String("Bash"))
        return classifyCommand(input.value(QStringLiteral("command")).toString(), cwd);
    if (toolName == QLatin1String("WebFetch")) {
        const QString url = input.value(QStringLiteral("url")).toString().trimmed();
        const QString host = hostOfUrl(url);
        if (!host.isEmpty() && isLocalHost(host))
            return {};
        return {true, host.isEmpty() ? QStringLiteral("web") : host,
                QStringLiteral("Fetch %1").arg(url.left(300))};
    }
    if (toolName == QLatin1String("WebSearch")) {
        return {true, QStringLiteral("web-search"),
                QStringLiteral("Search the web for \xE2\x80\x9C%1\xE2\x80\x9D")
                    .arg(input.value(QStringLiteral("query")).toString().left(200))};
    }
    return {};
}

QString hookScript()
{
    return QStringLiteral(R"PY(import json, os, re, socket, sys

# Words that may mean the tool could not reach something. The desktop decides;
# this only spares it the outputs that plainly do not.
HINTS = ("resolve", "name resolution", "not known", "enotfound", "eai_again",
         "lookup", "servname", "not found", "permission denied", "eacces",
         "unreachable", "no route to host", "timed out", "timeout", "etimedout")

def output_text(event):
    parts = []
    if event.get("error"):
        parts.append(str(event.get("error")))
    response = event.get("tool_response")
    if isinstance(response, dict):
        for key in ("stdout", "stderr", "error", "content", "result"):
            value = response.get(key)
            if isinstance(value, str):
                parts.append(value)
    elif isinstance(response, str):
        parts.append(response)
    text = "\n".join(parts)
    if len(text) > 96000:
        text = text[:48000] + "\n" + text[-48000:]
    return text

VARIABLE = re.compile(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)")
SECRET = re.compile(r"TOKEN|SECRET|PASSW|KEY|AUTH|CREDENTIAL|COOKIE|SESSION|PRIVATE", re.I)

def environment(command):
    names = set(VARIABLE.findall(command)) | {"FORKMESH_AGENT_SCRATCH", "HOME"}
    values, hidden = {}, []
    for name in sorted(names)[:64]:
        if name not in os.environ:
            continue
        if SECRET.search(name):
            hidden.append(name)
        else:
            values[name] = os.environ[name][:4096]
    return {"values": values, "hidden": hidden}

def ask(path, request):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(880)
        conn.connect(path)
        conn.sendall(json.dumps(request).encode("utf-8") + b"\n")
        data = b""
        while not data.endswith(b"\n") and len(data) < 65536:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("utf-8") or "{}")

def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0
    path = os.environ.get("FORKMESH_AGENT_GATE", "")
    if not path:
        return 0
    hook = event.get("hook_event_name") or "PreToolUse"
    request = {
        "v": 1,
        "session": os.environ.get("FORKMESH_AGENT_GATE_SESSION", ""),
        "label": os.environ.get("FORKMESH_AGENT_GATE_LABEL", ""),
        "tool": event.get("tool_name", ""),
        "input": event.get("tool_input", {}),
        "cwd": event.get("cwd", ""),
    }
    if hook == "PreToolUse" and isinstance(request["input"], dict):
        request["env"] = environment(str(request["input"].get("command", "")))
    if hook != "PreToolUse":
        text = output_text(event)
        lowered = text.lower()
        if not any(hint in lowered for hint in HINTS):
            return 0
        request["event"] = "failure"
        request["output"] = text
        try:
            reply = ask(path, request)
        except Exception:
            return 0
        if reply.get("context"):
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": hook,
                "additionalContext": reply.get("context")}}))
        return 0
    try:
        reply = ask(path, request)
    except Exception as exc:
        reply = {"decision": "deny",
                 "reason": "ForkMesh could not ask the user about this call (%s). "
                           "External calls need the project's permission." % exc}
    if reply.get("decision") != "allow":
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reply.get("reason")
                or "The project did not allow this external call."}}))
    return 0

sys.exit(main())
)PY");
}

QString claudeHookSettings(bool gateCalls)
{
    const QString encoded = QString::fromLatin1(hookScript().toUtf8().toBase64());
    const QJsonObject hook{
        {QStringLiteral("type"), QStringLiteral("command")},
        {QStringLiteral("command"),
         QStringLiteral("python3 -c \"import base64,sys;exec(base64.b64decode(sys.argv[1]))\" %1")
             .arg(encoded)},
        // Long enough for the user to notice and answer; the hook itself gives
        // up a little earlier and denies.
        {QStringLiteral("timeout"), 900}};
    // Tools whose failure can mean something was out of reach; Bash also
    // reports on success, since a pipeline can hide a failed fetch.
    const QString failureTools =
        QStringLiteral("Bash|WebFetch|Read|Write|Edit|MultiEdit|NotebookEdit|Glob|Grep");
    QJsonObject events{
        {QStringLiteral("PostToolUse"),
         QJsonArray{QJsonObject{{QStringLiteral("matcher"), QStringLiteral("Bash")},
                                {QStringLiteral("hooks"), QJsonArray{hook}}}}},
        {QStringLiteral("PostToolUseFailure"),
         QJsonArray{QJsonObject{{QStringLiteral("matcher"), failureTools},
                                {QStringLiteral("hooks"), QJsonArray{hook}}}}}};
    if (gateCalls)
        events.insert(QStringLiteral("PreToolUse"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("matcher"), QStringLiteral("Bash|WebFetch|WebSearch")},
                          {QStringLiteral("hooks"), QJsonArray{hook}}}});
    const QJsonObject settings{{QStringLiteral("hooks"), events}};
    return QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact));
}

QString withClaudeHookSettings(const QString &command)
{
    return withClaudeArguments(command,
                               QStringLiteral("--settings '%1'").arg(claudeHookSettings()));
}

QString withClaudeArguments(const QString &command, const QString &arguments)
{
    static const QRegularExpression claudeWord(
        QStringLiteral("(^|[\\s;&|(]|exec\\s)(claude)(?=\\s|$)"));
    const QRegularExpressionMatch match = claudeWord.match(command);
    if (!match.hasMatch() || arguments.trimmed().isEmpty())
        return command;
    QString result = command;
    result.insert(match.capturedEnd(2), QLatin1Char(' ') + arguments.trimmed());
    return result;
}

// ---- server ---------------------------------------------------------------------

Server::Server()
    : m_server(new QLocalServer)
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(m_server, &QLocalServer::newConnection, m_server, [this] { accept(); });
}

Server::~Server()
{
    for (QLocalSocket *socket : std::as_const(m_pending))
        if (socket)
            socket->disconnect(m_server);
    m_pending.clear();
    delete m_server;
}

bool Server::listen(QString *error)
{
    if (m_server->isListening())
        return true;
    const QString name = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("forkmesh-agent-gate-%1-%2")
                                           .arg(QCoreApplication::applicationPid())
                                           .arg(QRandomGenerator::system()->generate(), 8,
                                                16, QLatin1Char('0')));
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

QString Server::socketPath() const
{
    return m_server->fullServerName();
}

void Server::accept()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        socket->setParent(m_server);
        QObject::connect(socket, &QLocalSocket::readyRead, m_server,
                         [this, socket] { readRequest(socket); });
        QObject::connect(socket, &QLocalSocket::disconnected, m_server, [this, socket] {
            const QString id = socket->property("gateRequestId").toString();
            if (!id.isEmpty() && m_pending.value(id) == socket) {
                m_pending.remove(id);
                if (m_onCancel)
                    m_onCancel(id);
            }
            socket->deleteLater();
        });
    }
}

void Server::readRequest(QLocalSocket *socket)
{
    if (!socket->property("gateRequestId").toString().isEmpty())
        return; // one request per connection
    if (socket->bytesAvailable() > kMaxRequestBytes) {
        socket->abort();
        return;
    }
    if (!socket->canReadLine())
        return;
    const QByteArray line = socket->readLine(kMaxRequestBytes);
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    Request request;
    request.session = object.value(QStringLiteral("session")).toString().left(128);
    request.label = object.value(QStringLiteral("label")).toString().left(200);
    request.tool = object.value(QStringLiteral("tool")).toString().left(64);
    if (object.value(QStringLiteral("event")).toString() == QLatin1String("failure")) {
        request.failure = detectAccessFailure(request.tool,
                                              object.value(QStringLiteral("input")).toObject(),
                                              object.value(QStringLiteral("output")).toString());
        if (!request.failure.found() || !m_onFailure) {
            socket->write(QByteArrayLiteral("{}\n"));
            socket->flush();
            socket->disconnectFromServer();
            return;
        }
        request.id = QString::number(m_nextId++);
        socket->setProperty("gateRequestId", request.id);
        m_pending.insert(request.id, socket);
        m_onFailure(request);
        return;
    }
    request.input = object.value(QStringLiteral("input")).toObject();
    request.cwd = object.value(QStringLiteral("cwd")).toString();
    request.call = classifyToolCall(request.tool, request.input, request.cwd);
    const QJsonValue environment = object.value(QStringLiteral("env"));
    if (environment.isObject()) {
        request.environment.sent = true;
        const QJsonObject values = environment.toObject().value(QStringLiteral("values")).toObject();
        for (auto it = values.constBegin();
             it != values.constEnd() && request.environment.values.size() < 64; ++it)
            if (it.value().isString())
                request.environment.values.insert(it.key().left(128),
                                                  it.value().toString().left(4096));
        for (const QJsonValue &name :
             environment.toObject().value(QStringLiteral("hidden")).toArray())
            if (request.environment.hidden.size() < 64 && name.isString())
                request.environment.hidden.append(name.toString().left(128));
    }
    if (request.tool == QLatin1String("Bash"))
        request.risk = classifyCommandRisk(
            request.input.value(QStringLiteral("command")).toString(), request.environment);
    if (!m_onRequest && request.risk.any()) {
        socket->write(QByteArrayLiteral("{\"decision\":\"deny\",\"reason\":"
                                        "\"No one can approve this command right now.\"}\n"));
        socket->flush();
        socket->disconnectFromServer();
        return;
    }
    if ((!request.call.external && !request.risk.any()) || !m_onRequest) {
        const QByteArray reply = request.call.external
                                     ? QByteArrayLiteral("{\"decision\":\"deny\",\"reason\":"
                                                         "\"No one can approve external "
                                                         "calls right now.\"}\n")
                                     : QByteArrayLiteral("{\"decision\":\"allow\"}\n");
        socket->write(reply);
        socket->flush();
        socket->disconnectFromServer();
        return;
    }
    request.id = QString::number(m_nextId++);
    socket->setProperty("gateRequestId", request.id);
    m_pending.insert(request.id, socket);
    m_onRequest(request);
}

void Server::answer(const QString &id, Decision decision, const QString &reason)
{
    QJsonObject object{{QStringLiteral("decision"), decision == Decision::Allow
                                                         ? QStringLiteral("allow")
                                                         : QStringLiteral("deny")}};
    if (!reason.isEmpty())
        object.insert(QStringLiteral("reason"), reason);
    reply(id, object);
}

void Server::answerFailure(const QString &id, const QString &context)
{
    QJsonObject object;
    if (!context.isEmpty())
        object.insert(QStringLiteral("context"), context);
    reply(id, object);
}

void Server::reply(const QString &id, const QJsonObject &object)
{
    QLocalSocket *socket = m_pending.take(id);
    if (!socket)
        return;
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
    socket->disconnectFromServer();
}

} // namespace forkmesh::agentnet
