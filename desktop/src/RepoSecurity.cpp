#include "RepoSecurity.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

constexpr qsizetype kMaxScannedFileBytes = 1024 * 1024;

struct RepoFile {
    QString path;
    QByteArray content;
};

QString repoKey(const RepoSecurityInput &input)
{
    return input.owner + QStringLiteral("/") + input.name;
}

QString runGit(const QString &dir, const QStringList &args)
{
    if (dir.trimmed().isEmpty())
        return QString();
    QProcess git;
    git.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    if (!git.waitForFinished(8000) || git.exitCode() != 0)
        return QString();
    return QString::fromUtf8(git.readAllStandardOutput()).trimmed();
}

QString currentRefFor(const RepoSecurityInput &input)
{
    if (!input.ref.trimmed().isEmpty())
        return input.ref.trimmed();
    QString ref = runGit(input.localPath, {QStringLiteral("rev-parse"),
                                           QStringLiteral("--short"), QStringLiteral("HEAD")});
    if (!ref.isEmpty())
        return ref;
    ref = runGit(input.mirrorPath, {QStringLiteral("rev-parse"),
                                    QStringLiteral("--short"), QStringLiteral("HEAD")});
    return ref.isEmpty() ? QStringLiteral("HEAD") : ref;
}

QStringList trackedPathsFromLocal(const QString &localPath)
{
    const QString out = runGit(localPath, {QStringLiteral("ls-files")});
    return out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList trackedPathsFromMirror(const QString &mirrorPath)
{
    const QString out = runGit(mirrorPath, {QStringLiteral("ls-tree"),
                                            QStringLiteral("-r"),
                                            QStringLiteral("--name-only"),
                                            QStringLiteral("HEAD")});
    return out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QByteArray readLocalFile(const QString &localPath, const QString &relPath)
{
    QFile file(QDir(localPath).filePath(relPath));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > kMaxScannedFileBytes)
        return {};
    return file.readAll();
}

QByteArray runGitBatch(const QString &dir, const QStringList &args,
                       const QByteArray &input, bool *ok)
{
    QProcess git;
    git.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    git.write(input);
    git.closeWriteChannel();
    *ok = git.waitForFinished(60000) && git.exitCode() == 0;
    return *ok ? git.readAllStandardOutput() : QByteArray();
}

// HEAD:<path> for every path, read with two git processes in total (object
// sizes, then the contents of the blobs small enough to scan) instead of a
// `git show` per tracked file. A path that cannot be read as a blob, or is
// over the scan limit, maps to empty content, as a failed `git show` did.
QHash<QString, QByteArray> readMirrorFiles(const QString &mirrorPath,
                                           const QStringList &relPaths)
{
    QHash<QString, QByteArray> contents;
    if (relPaths.isEmpty())
        return contents;
    QByteArray names;
    for (const QString &relPath : relPaths)
        names += (QStringLiteral("HEAD:") + relPath).toUtf8() + '\n';
    bool ok = false;
    const QByteArray checks = runGitBatch(
        mirrorPath,
        {QStringLiteral("cat-file"),
         QStringLiteral("--batch-check=%(objectname) %(objecttype) %(objectsize)")},
        names, &ok);
    if (!ok)
        return contents;
    // One answer line per query line: the object, or "<name> missing".
    const QList<QByteArray> answers = checks.split('\n');
    QMultiHash<QByteArray, QString> pathsByOid;
    QByteArray oids;
    for (qsizetype i = 0; i < relPaths.size() && i < answers.size(); ++i) {
        const QList<QByteArray> fields = answers.at(i).split(' ');
        if (fields.size() != 3 || fields.at(1) != QByteArrayLiteral("blob"))
            continue;
        bool sizeOk = false;
        const qlonglong size = fields.at(2).toLongLong(&sizeOk);
        if (!sizeOk || size > kMaxScannedFileBytes)
            continue;
        if (!pathsByOid.contains(fields.at(0)))
            oids += fields.at(0) + '\n';
        pathsByOid.insert(fields.at(0), relPaths.at(i));
    }
    if (oids.isEmpty())
        return contents;
    const QByteArray batch = runGitBatch(
        mirrorPath, {QStringLiteral("cat-file"), QStringLiteral("--batch")}, oids, &ok);
    if (!ok)
        return contents;
    for (qsizetype pos = 0; pos < batch.size();) {
        const qsizetype nl = batch.indexOf('\n', pos);
        if (nl < 0)
            break;
        const QList<QByteArray> header = batch.mid(pos, nl - pos).split(' ');
        pos = nl + 1;
        if (header.size() != 3) // "<oid> missing" has no body
            continue;
        bool sizeOk = false;
        const qlonglong size = header.at(2).toLongLong(&sizeOk);
        if (!sizeOk || size < 0 || size > batch.size() - pos)
            break;
        const QByteArray body = batch.mid(pos, size);
        const QList<QString> paths = pathsByOid.values(header.at(0));
        for (const QString &path : paths)
            contents.insert(path, body);
        pos += size + 1; // skip body and its trailing newline
    }
    return contents;
}

bool isLikelyText(const QByteArray &content)
{
    if (content.isEmpty())
        return true;
    return !content.contains('\0');
}

QList<RepoFile> trackedTextFiles(const RepoSecurityInput &input)
{
    QList<RepoFile> files;
    const bool useLocal = !input.localPath.trimmed().isEmpty() &&
                          QDir(input.localPath).exists();
    const bool useMirror = !useLocal && !input.mirrorPath.trimmed().isEmpty() &&
                           QDir(input.mirrorPath).exists();
    const QStringList paths = useLocal ? trackedPathsFromLocal(input.localPath)
                                       : trackedPathsFromMirror(input.mirrorPath);
    const QHash<QString, QByteArray> mirrorContents =
        useMirror ? readMirrorFiles(input.mirrorPath, paths)
                  : QHash<QString, QByteArray>();
    for (const QString &path : paths) {
        QByteArray content = useLocal ? readLocalFile(input.localPath, path)
                                      : mirrorContents.value(path);
        if (content.size() > kMaxScannedFileBytes || !isLikelyText(content))
            continue;
        files.append({path, content});
    }
    return files;
}

bool pathEqualsAny(const QString &path, const QSet<QString> &needles)
{
    const QString normalized = path.trimmed().toLower();
    return needles.contains(normalized);
}

QStringList dependencyManifests(const QList<RepoFile> &files)
{
    const QSet<QString> names{
        QStringLiteral("package.json"),
        QStringLiteral("package-lock.json"),
        QStringLiteral("pnpm-lock.yaml"),
        QStringLiteral("yarn.lock"),
        QStringLiteral("requirements.txt"),
        QStringLiteral("pyproject.toml"),
        QStringLiteral("pipfile"),
        QStringLiteral("pipfile.lock"),
        QStringLiteral("pubspec.yaml"),
        QStringLiteral("pubspec.lock"),
        QStringLiteral("cargo.toml"),
        QStringLiteral("cargo.lock"),
        QStringLiteral("go.mod"),
        QStringLiteral("go.sum"),
        QStringLiteral("pom.xml"),
        QStringLiteral("build.gradle"),
        QStringLiteral("build.gradle.kts"),
        QStringLiteral("gemfile"),
        QStringLiteral("gemfile.lock"),
        QStringLiteral("composer.json"),
        QStringLiteral("composer.lock"),
        QStringLiteral("mix.exs"),
        QStringLiteral("deno.json"),
    };

    QStringList found;
    for (const RepoFile &file : files) {
        const QString base = QFileInfo(file.path).fileName().toLower();
        if (names.contains(base))
            found << file.path;
    }
    found.removeDuplicates();
    std::sort(found.begin(), found.end());
    return found;
}


struct DependencyAlert {
    QString name;
    QString version;
    QString path;
    int line = 0;
    QString reason;
};

static QPair<QList<DependencyAlert>, int> scanPackageJson(const RepoFile &file)
{
    QList<DependencyAlert> alerts;
    int totalDeps = 0;
    const QStringList lines = QString::fromUtf8(file.content).split(QLatin1Char('\n'));

    static const QRegularExpression kDepsSection(
        QStringLiteral(
            R"--("(?:dependencies|devDependencies|peerDependencies|optionalDependencies)"\s*:\s*\{)--"));
    static const QRegularExpression kEntry(
        QStringLiteral(R"--("([^"]+)"\s*:\s*"([^"]*)")--"));

    bool inDeps = false;
    int depth = 0;

    for (int i = 0; i < lines.size(); ++i) {
        const QString &ln = lines[i];
        if (!inDeps) {
            if (kDepsSection.match(ln).hasMatch()) {
                inDeps = true;
                depth = 1;
            }
            continue;
        }
        for (const QChar c : ln) {
            if (c == QLatin1Char('{'))
                ++depth;
            else if (c == QLatin1Char('}'))
                --depth;
        }
        if (depth <= 0) {
            inDeps = false;
            depth = 0;
            continue;
        }
        auto m = kEntry.match(ln);
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(1);
        const QString ver = m.captured(2).trimmed();
        if (ver.contains(QLatin1Char(':')))
            continue; // file:, git+, etc.
        ++totalDeps;
        QString reason;
        if (ver.isEmpty() || ver == QLatin1String("*") || ver == QLatin1String("x"))
            reason = QStringLiteral("unpinned (\"*\")");
        else if (ver == QLatin1String("latest"))
            reason = QStringLiteral("floating \"latest\" tag");
        else if (ver.startsWith(QLatin1String(">=")) && !ver.contains(QLatin1Char('<')))
            reason = QStringLiteral("open-ended range (no upper bound)");
        if (!reason.isEmpty())
            alerts.append({name, ver.isEmpty() ? QStringLiteral("*") : ver,
                           file.path, i + 1, reason});
    }
    return {alerts, totalDeps};
}

static QPair<QList<DependencyAlert>, int> scanRequirementsTxt(const RepoFile &file)
{
    QList<DependencyAlert> alerts;
    int totalDeps = 0;
    const QStringList lines = QString::fromUtf8(file.content).split(QLatin1Char('\n'));

    static const QRegularExpression kEntry(
        QStringLiteral(R"(^([A-Za-z0-9_\-\.]+)\s*([<>=!~\^].*)?$)"));
    static const QRegularExpression kExtras(QStringLiteral(R"(\[[^\]]*\])"));

    for (int i = 0; i < lines.size(); ++i) {
        QString ln = lines[i].trimmed();
        if (ln.isEmpty() || ln.startsWith(QLatin1Char('#')) ||
            ln.startsWith(QLatin1Char('-')))
            continue;
        const int hash = ln.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            ln = ln.left(hash).trimmed();
        const int semi = ln.indexOf(QLatin1Char(';'));
        if (semi >= 0)
            ln = ln.left(semi).trimmed();
        ln.remove(kExtras);
        auto m = kEntry.match(ln.trimmed());
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(1);
        const QString spec = m.captured(2).trimmed();
        ++totalDeps;
        QString reason;
        if (spec.isEmpty())
            reason = QStringLiteral("no version constraint");
        else if (!spec.contains(QLatin1String("==")) &&
                 (spec.startsWith(QLatin1Char('>')) ||
                  spec.contains(QLatin1String(">="))) &&
                 !spec.contains(QLatin1Char('<')))
            reason = QStringLiteral("open-ended range (no upper bound)");
        if (!reason.isEmpty())
            alerts.append({name, spec.isEmpty() ? QStringLiteral("(any)") : spec,
                           file.path, i + 1, reason});
    }
    return {alerts, totalDeps};
}

static QPair<QList<DependencyAlert>, int> scanCargoToml(const RepoFile &file)
{
    QList<DependencyAlert> alerts;
    int totalDeps = 0;
    const QStringList lines = QString::fromUtf8(file.content).split(QLatin1Char('\n'));

    static const QRegularExpression kSimple(
        QStringLiteral(R"(^\s*([A-Za-z0-9_\-]+)\s*=\s*"\*"\s*$)"));
    static const QRegularExpression kTable(
        QStringLiteral(R"(^\s*([A-Za-z0-9_\-]+)\s*=\s*\{[^}]*version\s*=\s*"\*")"));

    for (int i = 0; i < lines.size(); ++i) {
        const QString &ln = lines[i];
        auto m1 = kSimple.match(ln);
        if (m1.hasMatch()) {
            ++totalDeps;
            alerts.append({m1.captured(1), QStringLiteral("*"),
                           file.path, i + 1, QStringLiteral("unpinned (\"*\")")});
            continue;
        }
        auto m2 = kTable.match(ln);
        if (m2.hasMatch()) {
            ++totalDeps;
            alerts.append({m2.captured(1), QStringLiteral("*"),
                           file.path, i + 1, QStringLiteral("unpinned (\"*\")")});
        }
    }
    return {alerts, totalDeps};
}

static QPair<QList<DependencyAlert>, int> scanPomXml(const RepoFile &file)
{
    QList<DependencyAlert> alerts;
    int totalDeps = 0;
    const QStringList lines = QString::fromUtf8(file.content).split(QLatin1Char('\n'));

    static const QRegularExpression kVersion(
        QStringLiteral(R"(<version>(LATEST|RELEASE|[^<]*-SNAPSHOT)</version>)"),
        QRegularExpression::CaseInsensitiveOption);

    for (int i = 0; i < lines.size(); ++i) {
        auto m = kVersion.match(lines[i]);
        if (!m.hasMatch())
            continue;
        ++totalDeps;
        const QString ver = m.captured(1);
        QString reason;
        if (ver.compare(QLatin1String("LATEST"), Qt::CaseInsensitive) == 0)
            reason = QStringLiteral("floating LATEST version");
        else if (ver.compare(QLatin1String("RELEASE"), Qt::CaseInsensitive) == 0)
            reason = QStringLiteral("floating RELEASE version");
        else
            reason = QStringLiteral("mutable SNAPSHOT version");
        alerts.append({QStringLiteral("(dependency)"), ver, file.path, i + 1, reason});
    }
    return {alerts, totalDeps};
}

static QPair<QList<DependencyAlert>, int> scanGemfile(const RepoFile &file)
{
    QList<DependencyAlert> alerts;
    int totalDeps = 0;
    const QStringList lines = QString::fromUtf8(file.content).split(QLatin1Char('\n'));

    static const QRegularExpression kGem(
        QStringLiteral(R"(^\s*gem\s+['"]([^'"]+)['"]\s*(?:,\s*(.+))?$)"));

    for (int i = 0; i < lines.size(); ++i) {
        QString ln = lines[i];
        const int hash = ln.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            ln = ln.left(hash);
        auto m = kGem.match(ln.trimmed());
        if (!m.hasMatch())
            continue;
        ++totalDeps;
        const QString name = m.captured(1);
        const QString rest = m.captured(2).trimmed();
        const bool hasVersionArg =
            !rest.isEmpty() &&
            !rest.startsWith(QLatin1String("require:")) &&
            !rest.startsWith(QLatin1String("group:")) &&
            !rest.startsWith(QLatin1String("path:")) &&
            !rest.startsWith(QLatin1String("git:")) &&
            !rest.startsWith(QLatin1String("github:")) &&
            !rest.startsWith(QLatin1String("platforms:")) &&
            rest.contains(QLatin1Char('"'));
        if (!hasVersionArg)
            alerts.append({name, QStringLiteral("(any)"), file.path, i + 1,
                           QStringLiteral("no version constraint")});
    }
    return {alerts, totalDeps};
}

static QPair<QList<DependencyAlert>, QHash<QString, int>> collectDependencyAlerts(const QList<RepoFile> &files)
{
    QList<DependencyAlert> all;
    QHash<QString, int> dependencyCountByPath;
    for (const RepoFile &file : files) {
        const QString base = QFileInfo(file.path).fileName().toLower();
        QPair<QList<DependencyAlert>, int> result;
        if (base == QLatin1String("package.json"))
            result = scanPackageJson(file);
        else if (base == QLatin1String("requirements.txt"))
            result = scanRequirementsTxt(file);
        else if (base == QLatin1String("cargo.toml"))
            result = scanCargoToml(file);
        else if (base == QLatin1String("pom.xml"))
            result = scanPomXml(file);
        else if (base == QLatin1String("gemfile"))
            result = scanGemfile(file);
        else
            continue;
        all += result.first;
        if (result.second > 0)
            dependencyCountByPath[file.path] = result.second;
    }
    return {all, dependencyCountByPath};
}

QString redacted(QString match)
{
    match = match.trimmed();
    if (match.size() <= 10)
        return QStringLiteral("[redacted]");
    return match.left(4) + QStringLiteral("...") + match.right(4);
}

// Inline suppression marker: appending this token as a comment on the same
// line as an intentional/example credential (e.g. a per-provider test
// fixture) tells the scanner to skip it. This is what keeps the scanner's
// own test suite (test_crypto.cpp), which embeds realistic-looking fake
// keys to exercise each provider pattern, from flagging itself when a
// ForkMesh checkout scans its own tracked files.
const QString &secretScanIgnoreMarker()
{
    static const QString marker = QStringLiteral("forkmesh-secret-scan:ignore-line");
    return marker;
}

// A trailing backslash joins the next physical line onto this one — shell
// scripts, Makefiles and C macros all rely on it. A trailing comment cannot be
// appended to such a line: the backslash would escape the following space and
// the marker's "#" would swallow the rest of the statement. (Exactly that
// silently dropped the env prefix from www/public/install.sh's service
// bootstrap.) Suppression is therefore defined over the whole *logical* line:
// a marker on any physical line of a continuation run covers a match anywhere
// inside it, and the run's last line is where markFindingSafe writes.
bool continuesToNextLine(const QString &line)
{
    int trailingBackslashes = 0;
    for (qsizetype i = line.size() - 1;
         i >= 0 && line.at(i) == QLatin1Char('\\'); --i)
        ++trailingBackslashes;
    return trailingBackslashes % 2 == 1; // "\\" is an escaped backslash
}

// Inclusive [first, last] physical-line indices of the logical line holding idx.
QPair<int, int> logicalLineRange(const QStringList &lines, int idx)
{
    int first = idx;
    while (first > 0 && continuesToNextLine(lines.at(first - 1)))
        --first;
    int last = idx;
    while (last + 1 < lines.size() && continuesToNextLine(lines.at(last)))
        ++last;
    return {first, last};
}

bool logicalLineHasIgnoreMarker(const QStringList &lines, int idx)
{
    if (idx < 0 || idx >= lines.size())
        return false;
    const QPair<int, int> range = logicalLineRange(lines, idx);
    for (int i = range.first; i <= range.second; ++i)
        if (lines.at(i).contains(secretScanIgnoreMarker()))
            return true;
    return false;
}

// The comment tokens the ignore marker is wrapped in for a given file type.
// Returns an empty opener for formats with no comment syntax at all (JSON,
// CSV): there, annotating a line would corrupt the file rather than mark it,
// so markFindingSafe declines instead.
QPair<QString, QString> lineCommentTokens(const QString &path)
{
    static const QSet<QString> slashStyle{
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"),
        QStringLiteral("cxx"), QStringLiteral("h"), QStringLiteral("hh"),
        QStringLiteral("hpp"), QStringLiteral("js"), QStringLiteral("jsx"),
        QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("java"),
        QStringLiteral("go"), QStringLiteral("rs"), QStringLiteral("swift"),
        QStringLiteral("kt"), QStringLiteral("kts"), QStringLiteral("cs"),
        QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("scala"),
        QStringLiteral("dart"), QStringLiteral("php"), QStringLiteral("proto"),
        QStringLiteral("groovy"),
    };
    static const QSet<QString> dashStyle{
        QStringLiteral("sql"), QStringLiteral("lua"), QStringLiteral("hs"),
    };
    static const QSet<QString> markupStyle{
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("xml"),
        QStringLiteral("svg"), QStringLiteral("md"), QStringLiteral("markdown"),
        QStringLiteral("vue"), QStringLiteral("xhtml"), QStringLiteral("plist"),
    };
    static const QSet<QString> blockStyle{
        QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("less"),
    };
    static const QSet<QString> noCommentStyle{
        QStringLiteral("json"), QStringLiteral("csv"), QStringLiteral("tsv"),
        QStringLiteral("lock"),
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (slashStyle.contains(suffix))
        return {QStringLiteral("//"), QString()};
    if (dashStyle.contains(suffix))
        return {QStringLiteral("--"), QString()};
    if (markupStyle.contains(suffix))
        return {QStringLiteral("<!--"), QStringLiteral("-->")};
    if (blockStyle.contains(suffix))
        return {QStringLiteral("/*"), QStringLiteral("*/")};
    if (noCommentStyle.contains(suffix))
        return {QString(), QString()};
    return {QStringLiteral("#"), QString()};
}

struct SecretPattern {
    QString name;
    QRegularExpression re;
};

const QList<SecretPattern> &secretPatterns()
{
    static const QList<SecretPattern> kPatterns{
        {QStringLiteral("GitHub token"),
         QRegularExpression(QStringLiteral("\\bgh[pousr]_[A-Za-z0-9_]{36,}\\b"))},
        {QStringLiteral("GitHub fine-grained PAT"),
         QRegularExpression(QStringLiteral("\\bgithub_pat_[A-Za-z0-9_]{22,}\\b"))},
        {QStringLiteral("AWS access key"),
         QRegularExpression(QStringLiteral("\\bAKIA[0-9A-Z]{16,}\\b"))},
        // Temporary STS/assumed-role credentials use ASIA prefix
        {QStringLiteral("AWS temporary access key"),
         QRegularExpression(QStringLiteral("\\bASIA[0-9A-Z]{16,}\\b"))},
        {QStringLiteral("Slack token"),
         QRegularExpression(QStringLiteral("\\bxox[baprs]-[A-Za-z0-9-]{20,}\\b"))},
        {QStringLiteral("OpenAI API key"),
         QRegularExpression(QStringLiteral(
             "\\bsk-proj-[A-Za-z0-9_-]{20,}\\b|\\bsk-[A-Za-z0-9]{48,}\\b"))},
        {QStringLiteral("Anthropic API key"),
         QRegularExpression(QStringLiteral("\\bsk-ant-[A-Za-z0-9_-]{20,}\\b"))},
        {QStringLiteral("Stripe secret key"),
         QRegularExpression(QStringLiteral(
             "\\bsk_(live|test)_[A-Za-z0-9]{24,}\\b"))},
        {QStringLiteral("Stripe restricted key"),
         QRegularExpression(QStringLiteral(
             "\\brk_(live|test)_[A-Za-z0-9]{24,}\\b"))},
        {QStringLiteral("Google API key"),
         QRegularExpression(QStringLiteral("\\bAIza[A-Za-z0-9_-]{35,}\\b"))},
        {QStringLiteral("Google OAuth token"),
         QRegularExpression(QStringLiteral("\\bya29\\.[A-Za-z0-9_-]{20,}\\b"))},
        // OAuth client secrets from the Google Cloud console
        {QStringLiteral("Google OAuth client secret"),
         QRegularExpression(QStringLiteral("\\bGOCSPX-[A-Za-z0-9_-]{28,}\\b"))},
        // ---- SendGrid --------------------------------------------------------
        // Format: SG.<22-char key ID>.<43-char secret>
        {QStringLiteral("SendGrid API key"),
         QRegularExpression(QStringLiteral(
             "\\bSG\\.[A-Za-z0-9_-]{22,}\\.[A-Za-z0-9_-]{43,}\\b"))},
        {QStringLiteral("Twilio auth token"),
         QRegularExpression(QStringLiteral("\\bSK[a-f0-9]{32,}\\b"))},
        {QStringLiteral("npm access token"),
         QRegularExpression(QStringLiteral("\\bnpm_[A-Za-z0-9]{36,}\\b"))},
        {QStringLiteral("HashiCorp Vault token"),
         QRegularExpression(QStringLiteral(
             "\\bhv[sbr]\\.[A-Za-z0-9]{24,}\\b"))},
        {QStringLiteral("Cloudflare API token"),
         QRegularExpression(QStringLiteral(
             "(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)"
             "\\s*=\\s*['\"]?([A-Za-z0-9_-]{40,})['\"]?"))},
        {QStringLiteral("PEM private key"),
         QRegularExpression(QStringLiteral(
             "-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY"
             "(?:-----| BLOCK-----)"))},
        {QStringLiteral("Secret/token assignment"),
         QRegularExpression(QStringLiteral(
             "(?i)(?:password|passwd|api[_\\-]?(?:key|secret|token)|"
             "auth[_\\-]?token|secret[_\\-]?key|access[_\\-]?token|"
             "private[_\\-]?key)\\s*[:=]\\s*['\"]([^'\"\\s]{20,})['\"]"))},
    };
    return kPatterns;
}

// The generic assignment rule is the only pattern with no provider-specific
// prefix to anchor it, so it is the one that trips over test fixtures and
// documentation samples (e.g. the XKCD example password in the browser tests).
// A value that advertises itself as a placeholder is not a credential worth
// blocking a push over.
bool looksLikePlaceholderValue(const QString &value)
{
    static const QStringList kMarkers{
        QStringLiteral("correct-horse-battery-staple"),
        QStringLiteral("changeme"),
        QStringLiteral("change-me"),
        QStringLiteral("change_me"),
        QStringLiteral("placeholder"),
        QStringLiteral("example"),
        QStringLiteral("sample"),
        QStringLiteral("dummy"),
        QStringLiteral("redacted"),
        QStringLiteral("notreal"),
        QStringLiteral("not-a-real"),
        QStringLiteral("not_a_real"),
        QStringLiteral("fake"),
        QStringLiteral("your-"),
        QStringLiteral("your_"),
        QStringLiteral("test-password"),
        QStringLiteral("test_password"),
        QStringLiteral("testpassword"),
        // Template holes: "${API_TOKEN}", "{{ secret }}", "<your-token-here>".
        QStringLiteral("${"),
        QStringLiteral("{{"),
        QStringLiteral("<"),
    };
    const QString lower = value.toLower();
    for (const QString &marker : kMarkers) {
        if (lower.contains(marker))
            return true;
    }
    // A value that is nothing but an indirection holds no credential of its
    // own: `CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN"` is how a shell
    // script threads a token through the environment, not a leak. The ${VAR}
    // and {{ }} spellings are already covered above; this catches bare $VAR,
    // command substitution and Windows %VAR%.
    static const QRegularExpression kIndirection(
        QStringLiteral(R"(^(?:\$\w+|\$\{[^}]*\}|\$\([^)]*\)|%\w+%)$)"));
    if (kIndirection.match(value).hasMatch())
        return true;
    return !lower.isEmpty() &&
           std::all_of(lower.cbegin(), lower.cend(),
                       [&lower](QChar c) { return c == lower.at(0); });
}

bool isPlaceholderAssignment(const SecretPattern &pattern,
                             const QRegularExpressionMatch &match)
{
    return pattern.name == QLatin1String("Secret/token assignment") &&
           looksLikePlaceholderValue(match.captured(1));
}

// Diff health runs this scanner while rendering a branch detail.  Most added
// lines in a source diff cannot possibly satisfy any of the credential
// patterns below, so avoid invoking every regular expression for those lines.
// Each fragment is a literal required by one of secretPatterns(); this is only
// a conservative sieve, and candidates still go through the full matcher.
bool mayContainSecret(const QString &text)
{
    // The Twilio pattern is deliberately case-sensitive ("SK" followed by
    // hex), so checking its exact prefix separately avoids treating every
    // ordinary lower-case "sk" as suspicious.
    if (text.contains(QLatin1String("SK")))
        return true;

    static const QStringList kRequiredFragments{
        QStringLiteral("ghp_"), QStringLiteral("gho_"),
        QStringLiteral("ghu_"), QStringLiteral("ghs_"),
        QStringLiteral("ghr_"), QStringLiteral("github_pat_"),
        QStringLiteral("akia"), QStringLiteral("asia"),
        QStringLiteral("xoxb-"), QStringLiteral("xoxa-"),
        QStringLiteral("xoxp-"), QStringLiteral("xoxr-"),
        QStringLiteral("xoxs-"), QStringLiteral("sk-proj-"),
        QStringLiteral("sk-"), QStringLiteral("sk_live_"),
        QStringLiteral("sk_test_"), QStringLiteral("rk_live_"),
        QStringLiteral("rk_test_"), QStringLiteral("aiza"),
        QStringLiteral("ya29."), QStringLiteral("gocspx-"),
        QStringLiteral("sg."), QStringLiteral("npm_"),
        QStringLiteral("hvs."), QStringLiteral("hvb."),
        QStringLiteral("hvr."), QStringLiteral("cloudflare_api_token"),
        QStringLiteral("cf_api_token"), QStringLiteral("-----begin "),
        QStringLiteral("password"), QStringLiteral("passwd"),
        QStringLiteral("api_key"), QStringLiteral("api-key"),
        QStringLiteral("apikey"), QStringLiteral("api_secret"),
        QStringLiteral("api-secret"), QStringLiteral("apisecret"),
        QStringLiteral("api_token"), QStringLiteral("api-token"),
        QStringLiteral("apitoken"), QStringLiteral("auth_token"),
        QStringLiteral("auth-token"), QStringLiteral("authtoken"),
        QStringLiteral("secret_key"), QStringLiteral("secret-key"),
        QStringLiteral("secretkey"), QStringLiteral("access_token"),
        QStringLiteral("access-token"), QStringLiteral("accesstoken"),
        QStringLiteral("private_key"), QStringLiteral("private-key"),
        QStringLiteral("privatekey"),
    };
    const QString lower = text.toLower();
    for (const QString &fragment : kRequiredFragments) {
        if (lower.contains(fragment))
            return true;
    }
    return false;
}

// Would the scanner flag anything in this text? markFindingSafe uses it to
// refuse annotating a line that is not actually a match, so a stale or
// mis-numbered finding cannot rewrite unrelated code.
int secretMatchEndStartingBefore(const QString &text, qsizetype startLimit)
{
    int end = -1;
    for (const SecretPattern &pattern : secretPatterns()) {
        auto matches = pattern.re.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            if (match.capturedStart() >= startLimit)
                break;
            if (isPlaceholderAssignment(pattern, match))
                continue;
            end = qMax(end, int(match.capturedEnd()));
        }
    }
    return end;
}

bool matchSpanHasIgnoreMarker(const QStringList &lines, int firstIdx, int lastIdx)
{
    for (int i = firstIdx; i <= lastIdx; ++i)
        if (logicalLineHasIgnoreMarker(lines, i))
            return true;
    return false;
}

// code-health: allow(deep-nested-loop) files x fixed secretPatterns() list x regex hits, in memory with no process or file I/O; mayContainSecret skips files no pattern can match
QList<RepoSecurityFinding> secretFindings(const QList<RepoFile> &files)
{
    QList<RepoSecurityFinding> findings;
    for (const RepoFile &file : files) {
        const QString text = QString::fromUtf8(file.content);
        // The sieve holds for a whole file as it does for one line: a file
        // containing no required fragment cannot match any pattern.
        if (!mayContainSecret(text))
            continue;
        // Match offsets index this QString, so line numbers have to be counted
        // in the same units. Counting newlines in the raw UTF-8 bytes instead
        // can miss line boundaries after multi-byte characters, which is what
        // pointed "mark safe" at the wrong source line.
        const QStringList lines = text.split(QLatin1Char('\n'));
        QList<qsizetype> lineStarts;
        lineStarts.reserve(lines.size());
        qsizetype at = 0;
        for (const QString &line : lines) {
            lineStarts.append(at);
            at += line.size() + 1; // + the '\n' split consumed
        }
        const auto lineIndexForOffset = [&lineStarts](qsizetype offset) {
            const auto it = std::upper_bound(lineStarts.cbegin(),
                                             lineStarts.cend(), offset);
            return static_cast<int>(std::distance(lineStarts.cbegin(), it)) - 1;
        };
        for (const SecretPattern &pattern : secretPatterns()) {
            auto matches = pattern.re.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                const int lineIdx = lineIndexForOffset(match.capturedStart());
                const int lastIdx = lineIndexForOffset(
                    qMax(match.capturedStart(), match.capturedEnd() - 1));
                if (matchSpanHasIgnoreMarker(lines, lineIdx, lastIdx))
                    continue;
                if (isPlaceholderAssignment(pattern, match))
                    continue;
                RepoSecurityFinding finding;
                finding.id = QStringLiteral("secret:%1:%2")
                                 .arg(file.path)
                                 .arg(match.capturedStart());
                finding.category = QStringLiteral("Secret");
                finding.severity = RepoSecuritySeverity::Critical;
                finding.title = pattern.name;
                finding.detail = QStringLiteral("Possible %1 in tracked file: %2")
                                     .arg(pattern.name.toLower(),
                                          redacted(match.captured(0)));
                finding.path = file.path;
                finding.line = lineIdx + 1;
                finding.recommendedAction =
                    QStringLiteral("Rotate the credential and remove it from git.");
                findings.append(finding);
            }
        }
    }
    return findings;
}

// Scan a raw `git diff` output string for secrets introduced in added lines.
// code-health: allow(deep-nested-loop) diff files x added lines passing mayContainSecret x fixed secretPatterns() list x regex hits, in memory with no process or file I/O
QList<RepoSecurityFinding> secretFindingsFromDiff(const QString &diff)
{
    static const QRegularExpression kHunkRe(
        QStringLiteral(R"(@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@)"));

    // One entry per added/context line the diff shows, in new-revision line
    // numbers. Context lines are kept because suppression is a property of the
    // whole logical line (see continuesToNextLine): the marker sits at the end
    // of a continuation run, which may be a line the diff only carries as
    // context. Added lines are the only ones scanned, as before — a secret
    // that is merely *near* the change is not being pushed by it.
    struct ShownLine {
        QString content;
        bool added = false;
    };
    struct FileLines {
        QString path;
        QHash<int, ShownLine> byNumber;
    };
    QList<FileLines> files;

    int hunkStart = 0; // 0 while outside a hunk
    int hunkOffset = 0;
    for (const QString &line : diff.split(QLatin1Char('\n'))) {
        // `diff --git` is the only header a hunk body can never impersonate:
        // body lines always start with ' ', '+', '-' or '\'. Anchoring the
        // file boundary here means a removed "-- sql comment" (which reaches
        // us as "--- sql comment") cannot be mistaken for a `---` header.
        if (line.startsWith(QStringLiteral("diff --git "))) {
            hunkStart = 0;
            hunkOffset = 0;
            continue;
        }
        // Only a line *starting* with "@@ " is a hunk header; an added line
        // whose own content is a hunk header (a diff of a diff) arrives here
        // as "+@@ ..." and must not be mistaken for one.
        const QRegularExpressionMatch hunkMatch =
            line.startsWith(QStringLiteral("@@ ")) ? kHunkRe.match(line)
                                                   : QRegularExpressionMatch();
        if (hunkMatch.hasMatch()) {
            hunkStart = hunkMatch.captured(1).toInt();
            hunkOffset = 0;
            continue;
        }
        if (hunkStart == 0) {
            if (line.startsWith(QStringLiteral("+++ b/")))
                files.append({line.mid(6).trimmed(), {}});
            continue; // ---/+++/index/mode headers precede the first hunk
        }
        if (line.startsWith(QLatin1Char('-')) ||
            line.startsWith(QStringLiteral("\\ ")))
            continue; // removals, and "\ No newline at end of file"
        const bool added = line.startsWith(QLatin1Char('+'));
        if (!added && line.isEmpty())
            continue; // trailing element from the split, not a context line
        if (!files.isEmpty())
            files.last().byNumber.insert(hunkStart + hunkOffset,
                                         {line.mid(1), added});
        ++hunkOffset;
    }

    QList<RepoSecurityFinding> findings;
    for (const FileLines &file : files) {
        const auto lineAt = [&file](int n) -> const ShownLine * {
            const auto it = file.byNumber.constFind(n);
            return it == file.byNumber.constEnd() ? nullptr : &it.value();
        };
        const auto markerCoversLine = [&lineAt](int number) {
            int first = number;
            while (const ShownLine *prev = lineAt(first - 1)) {
                if (!continuesToNextLine(prev->content))
                    break;
                --first;
            }
            int last = number;
            while (const ShownLine *cur = lineAt(last)) {
                if (!continuesToNextLine(cur->content) || !lineAt(last + 1))
                    break;
                ++last;
            }
            for (int n = first; n <= last; ++n)
                if (const ShownLine *l = lineAt(n);
                    l && l->content.contains(secretScanIgnoreMarker()))
                    return true;
            return false;
        };

        QList<int> numbers = file.byNumber.keys();
        std::sort(numbers.begin(), numbers.end());
        for (const int lineNum : numbers) {
            const ShownLine &shown = file.byNumber.value(lineNum);
            if (!shown.added || markerCoversLine(lineNum))
                continue;
            if (!mayContainSecret(shown.content))
                continue;
            for (const SecretPattern &p : secretPatterns()) {
                auto matches = p.re.globalMatch(shown.content);
                while (matches.hasNext()) {
                    const QRegularExpressionMatch m = matches.next();
                    if (isPlaceholderAssignment(p, m))
                        continue;
                    RepoSecurityFinding finding;
                    finding.id = QStringLiteral("secret:%1:%2:%3")
                                     .arg(file.path)
                                     .arg(lineNum)
                                     .arg(m.capturedStart());
                    finding.category = QStringLiteral("Secret");
                    finding.severity = RepoSecuritySeverity::Critical;
                    finding.title = p.name;
                    finding.detail =
                        QStringLiteral("Possible %1 introduced in push: %2")
                            .arg(p.name.toLower(), redacted(m.captured(0)));
                    finding.path = file.path;
                    finding.line = lineNum;
                    finding.recommendedAction =
                        QStringLiteral("Rotate the credential and remove it from git.");
                    findings.append(finding);
                }
            }
        }
    }
    return findings;
}

bool hasSecurityPolicy(const QList<RepoFile> &files)
{
    const QSet<QString> policyPaths{
        QStringLiteral("security.md"),
        QStringLiteral(".github/security.md"),
        QStringLiteral("docs/security.md"),
    };
    for (const RepoFile &file : files)
        if (pathEqualsAny(file.path, policyPaths))
            return true;
    return false;
}

int openSecurityIssueCount(const QList<Issue> &issues)
{
    int count = 0;
    for (const Issue &issue : issues) {
        if (issue.status != QLatin1String("open"))
            continue;
        for (const QString &label : issue.labels)
            if (label.compare(QStringLiteral("security"), Qt::CaseInsensitive) == 0) {
                ++count;
                break;
            }
    }
    return count;
}

RepoSecuritySeverity qualitySeverity(const QList<ActionRun> &runs)
{
    bool hasRun = false;
    bool hasRunning = false;
    for (const ActionRun &run : runs) {
        hasRun = true;
        if (run.status == ActionStatus::Failed || run.status == ActionStatus::Rejected)
            return RepoSecuritySeverity::High;
        if (run.status == ActionStatus::Running || run.status == ActionStatus::Queued ||
            run.status == ActionStatus::AwaitingApproval)
            hasRunning = true;
    }
    if (!hasRun)
        return RepoSecuritySeverity::Warning;
    return hasRunning ? RepoSecuritySeverity::Info : RepoSecuritySeverity::Pass;
}

QString plural(int n, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? singular : plural);
}

RepoSecuritySignal signal(QString key, QString title, RepoSecuritySeverity severity,
                          QString summary, QString detail = QString(),
                          QString actionLabel = QString(),
                          QString actionTarget = QString())
{
    RepoSecuritySignal out;
    out.key = std::move(key);
    out.title = std::move(title);
    out.severity = severity;
    out.summary = std::move(summary);
    out.detail = std::move(detail);
    out.actionLabel = std::move(actionLabel);
    out.actionTarget = std::move(actionTarget);
    return out;
}

} // namespace

RepoSecuritySnapshot RepoSecurity::scan(const RepoSecurityInput &input)
{
    RepoSecuritySnapshot snapshot;
    snapshot.repoKey = repoKey(input);
    snapshot.ref = currentRefFor(input);
    snapshot.generatedAtMs = QDateTime::currentMSecsSinceEpoch();

    const QList<RepoFile> files = trackedTextFiles(input);
    const bool policyPresent = hasSecurityPolicy(files);
    snapshot.signalList.append(
        policyPresent
            ? signal(QStringLiteral("policy"), QStringLiteral("Security policy"),
                     RepoSecuritySeverity::Pass,
                     QStringLiteral("SECURITY.md is present."),
                     QStringLiteral("ForkMesh found security reporting guidance."))
            : signal(QStringLiteral("policy"), QStringLiteral("Security policy"),
                     RepoSecuritySeverity::Warning,
                     QStringLiteral("No SECURITY.md found."),
                     QStringLiteral("Add reporting instructions before public launch."),
                     QStringLiteral("Create SECURITY.md"),
                     QStringLiteral("create:security-policy")));

    const QList<RepoSecurityFinding> secrets = secretFindings(files);
    snapshot.findings.append(secrets);
    snapshot.signalList.append(
        secrets.isEmpty()
            ? signal(QStringLiteral("secrets"), QStringLiteral("Secret scan"),
                     RepoSecuritySeverity::Pass,
                     QStringLiteral("No high-confidence secrets found."),
                     QStringLiteral("This checks tracked text files at the selected ref."))
            : signal(QStringLiteral("secrets"), QStringLiteral("Secret scan"),
                     RepoSecuritySeverity::Critical,
                     plural(secrets.size(), QStringLiteral("probable secret"),
                            QStringLiteral("probable secrets")) +
                         QStringLiteral(" found."),
                     QStringLiteral("Matched values are redacted. Rotate any exposed keys.")));

    const QStringList manifests = dependencyManifests(files);
    const auto [depAlerts, depCounts] = collectDependencyAlerts(files);
    snapshot.dependencyCounts = depCounts;

    for (const DependencyAlert &alert : depAlerts) {
        RepoSecurityFinding finding;
        finding.id =
            QStringLiteral("dep:%1:%2").arg(alert.path).arg(alert.line);
        finding.category = QStringLiteral("Dependency");
        finding.severity = RepoSecuritySeverity::Warning;
        finding.title = alert.name;
        finding.detail =
            QStringLiteral("Loose version specifier: %1 (%2)")
                .arg(alert.version, alert.reason);
        finding.path = alert.path;
        finding.line = alert.line;
        finding.recommendedAction =
            QStringLiteral("Pin to an exact version to reduce supply-chain risk.");
        snapshot.findings.append(finding);
    }

    if (manifests.isEmpty()) {
        snapshot.signalList.append(
            signal(QStringLiteral("dependencies"),
                   QStringLiteral("Dependency scan"),
                   RepoSecuritySeverity::Info,
                   QStringLiteral("No dependency manifests detected."),
                   QStringLiteral("No supported manifests found in tracked files.")));
    } else if (depAlerts.isEmpty()) {
        RepoSecuritySignal depSig =
            signal(QStringLiteral("dependencies"),
                   QStringLiteral("Dependency scan"),
                   RepoSecuritySeverity::Pass,
                   plural(manifests.size(), QStringLiteral("manifest"),
                          QStringLiteral("manifests")) +
                       QStringLiteral(" scanned - all versions pinned."),
                   QStringLiteral(
                       "No loose version specifiers found."));
        depSig.items = manifests;
        snapshot.signalList.append(depSig);
    } else {
        RepoSecuritySignal depSig =
            signal(QStringLiteral("dependencies"),
                   QStringLiteral("Dependency scan"),
                   RepoSecuritySeverity::Warning,
                   plural(depAlerts.size(), QStringLiteral("unpinned dependency"),
                          QStringLiteral("unpinned dependencies")) +
                       QStringLiteral(" detected."),
                   QStringLiteral(
                       "Loose version specifiers increase supply-chain risk. "
                       "Pin each to an exact version."));
        depSig.items = manifests;
        snapshot.signalList.append(depSig);
    }

    RepoSecuritySeverity actionsSeverity = RepoSecuritySeverity::Pass;
    QString actionsSummary;
    QString actionsDetail;
    if (!input.actionsEnabled) {
        actionsSeverity = RepoSecuritySeverity::Warning;
        actionsSummary = QStringLiteral("Actions are disabled.");
        actionsDetail = QStringLiteral("Workflows will not run on push.");
    } else if (input.workflows.isEmpty()) {
        actionsSeverity = RepoSecuritySeverity::Warning;
        actionsSummary = QStringLiteral("No .forkmesh workflows found.");
        actionsDetail = QStringLiteral("Add a workflow to make checks visible.");
    } else {
        int invalid = 0;
        for (const ActionWorkflow &workflow : input.workflows)
            if (!workflow.valid)
                ++invalid;
        if (invalid > 0) {
            actionsSeverity = RepoSecuritySeverity::High;
            actionsSummary = plural(invalid, QStringLiteral("invalid workflow"),
                                    QStringLiteral("invalid workflows"));
            actionsDetail = QStringLiteral("Fix workflow parsing errors.");
        } else {
            actionsSummary = plural(input.workflows.size(), QStringLiteral("workflow"),
                                    QStringLiteral("workflows")) +
                             QStringLiteral(" configured.");
            actionsDetail =
                QStringLiteral("Changed workflow content still uses existing approval.");
        }
    }
    snapshot.signalList.append(signal(QStringLiteral("actions"),
                                   QStringLiteral("Actions trust"),
                                   actionsSeverity, actionsSummary, actionsDetail,
                                   QStringLiteral("Open Actions"),
                                   QStringLiteral("tab:actions")));

    const int securityIssues = openSecurityIssueCount(input.issues);
    snapshot.signalList.append(
        securityIssues > 0
            ? signal(QStringLiteral("issues"), QStringLiteral("Security backlog"),
                     RepoSecuritySeverity::Warning,
                     plural(securityIssues, QStringLiteral("open security issue"),
                            QStringLiteral("open security issues")),
                     QStringLiteral("Issues tagged security should stay visible here."),
                     QStringLiteral("Open Issues"),
                     QStringLiteral("tab:issues:security"))
            : signal(QStringLiteral("issues"), QStringLiteral("Security backlog"),
                     RepoSecuritySeverity::Pass,
                     QStringLiteral("No open security-labelled issues."),
                     QStringLiteral("This checks signed issue metadata.")));

    RepoSecuritySeverity trustSeverity = RepoSecuritySeverity::Info;
    QString trustSummary;
    if (input.integrityWarning) {
        trustSeverity = RepoSecuritySeverity::Critical;
        trustSummary = QStringLiteral("Repository integrity pin needs attention.");
    } else if (input.previewOnly) {
        trustSeverity = RepoSecuritySeverity::Warning;
        trustSummary = QStringLiteral("Preview-only repository.");
    } else if (input.isPrivate) {
        trustSummary = QStringLiteral("Private repository.");
    } else if (input.publishToNetwork) {
        trustSummary = QStringLiteral("Published public repository.");
    } else {
        trustSummary = QStringLiteral("Local-only repository.");
    }
    snapshot.signalList.append(signal(QStringLiteral("trust"),
                                   QStringLiteral("ForkMesh trust"), trustSeverity,
                                   trustSummary,
                                   QStringLiteral("Derived from local repository metadata.")));

    return snapshot;
}

RepoSecurityManifestScan RepoSecurity::scanManifest(const RepoSecurityInput &input,
                                                    const QString &manifestPath)
{
    RepoSecurityManifestScan result;

    const bool useLocal = !input.localPath.trimmed().isEmpty() &&
                          QDir(input.localPath).exists();
    const bool useMirror = !useLocal && !input.mirrorPath.trimmed().isEmpty() &&
                           QDir(input.mirrorPath).exists();

    QByteArray content = useLocal ? readLocalFile(input.localPath, manifestPath)
                                  : useMirror ? readMirrorFiles(input.mirrorPath, {manifestPath})
                                                    .value(manifestPath)
                                              : QByteArray();
    if (content.isEmpty() || !isLikelyText(content))
        return result;

    RepoFile file{manifestPath, content};
    const QString base = QFileInfo(file.path).fileName().toLower();
    QPair<QList<DependencyAlert>, int> scanResult;

    if (base == QLatin1String("package.json"))
        scanResult = scanPackageJson(file);
    else if (base == QLatin1String("requirements.txt"))
        scanResult = scanRequirementsTxt(file);
    else if (base == QLatin1String("cargo.toml"))
        scanResult = scanCargoToml(file);
    else if (base == QLatin1String("pom.xml"))
        scanResult = scanPomXml(file);
    else if (base == QLatin1String("gemfile"))
        scanResult = scanGemfile(file);
    else
        return result;

    result.dependencyCount = scanResult.second;
    for (const DependencyAlert &alert : scanResult.first) {
        RepoSecurityFinding finding;
        finding.id =
            QStringLiteral("dep:%1:%2").arg(alert.path).arg(alert.line);
        finding.category = QStringLiteral("Dependency");
        finding.severity = RepoSecuritySeverity::Warning;
        finding.title = alert.name;
        finding.detail =
            QStringLiteral("Loose version specifier: %1 (%2)")
                .arg(alert.version, alert.reason);
        finding.path = alert.path;
        finding.line = alert.line;
        finding.recommendedAction =
            QStringLiteral("Pin to an exact version to reduce supply-chain risk.");
        result.findings.append(finding);
    }

    return result;
}

QString RepoSecurity::severityText(RepoSecuritySeverity severity)
{
    switch (severity) {
    case RepoSecuritySeverity::Pass:
        return QStringLiteral("Pass");
    case RepoSecuritySeverity::Info:
        return QStringLiteral("Info");
    case RepoSecuritySeverity::Warning:
        return QStringLiteral("Warning");
    case RepoSecuritySeverity::High:
        return QStringLiteral("High");
    case RepoSecuritySeverity::Critical:
        return QStringLiteral("Critical");
    }
    return QStringLiteral("Info");
}

// code-health: allow(blocking-wait) only caller is the runOffThread worker in MainWindowChat pushCurrentRepoUpstream
QList<RepoSecurityFinding> RepoSecurity::findSecretsInPush(
    const QString &localPath, const QString &upstreamRef)
{
    if (localPath.trimmed().isEmpty())
        return {};

    if (!upstreamRef.trimmed().isEmpty()) {
        QProcess git;
        git.start(QStringLiteral("git"),
                  {QStringLiteral("-C"), localPath, QStringLiteral("diff"),
                   upstreamRef + QStringLiteral("..HEAD")});
        if (git.waitForFinished(15000) && git.exitCode() == 0) {
            const QString diff = QString::fromUtf8(git.readAllStandardOutput());
            if (!diff.trimmed().isEmpty())
                return secretFindingsFromDiff(diff);
            return {};
        }
    }

    RepoSecurityInput input;
    input.localPath = localPath;
    return secretFindings(trackedTextFiles(input));
}

QList<RepoSecurityFinding> RepoSecurity::findSecretsInDiff(const QString &diff)
{
    return secretFindingsFromDiff(diff);
}

bool RepoSecurity::markFindingSafe(const QString &localPath,
                                   const RepoSecurityFinding &finding)
{
    if (localPath.trimmed().isEmpty() || finding.path.trimmed().isEmpty() ||
        finding.line <= 0)
        return false;

    QFile file(QDir(localPath).filePath(finding.path));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray raw = file.readAll();
    file.close();

    const bool hadTrailingNewline = raw.endsWith('\n');
    QStringList lines = QString::fromUtf8(raw).split(QLatin1Char('\n'));
    if (hadTrailingNewline && !lines.isEmpty())
        lines.removeLast();

    const int idx = finding.line - 1;
    if (idx < 0 || idx >= lines.size())
        return false;
    if (logicalLineHasIgnoreMarker(lines, idx))
        return true;

    const QPair<int, int> start = logicalLineRange(lines, idx);
    constexpr int kMaxSpanLines = 8;
    const int windowLast =
        qMin(int(lines.size()) - 1, start.second + kMaxSpanLines);
    QStringList window;
    qsizetype startLimit = 0;
    for (int i = start.first; i <= windowLast; ++i) {
        window.append(lines.at(i));
        if (i <= start.second)
            startLimit += lines.at(i).size() + 1;
    }
    const QString windowText = window.join(QLatin1Char('\n'));
    const int matchEnd = secretMatchEndStartingBefore(windowText, startLimit);
    if (matchEnd < 0)
        return false;
    const int spanLast =
        start.first
        + int(windowText.left(qMax(0, matchEnd - 1)).count(QLatin1Char('\n')));
    if (matchSpanHasIgnoreMarker(lines, start.first, spanLast))
        return true;
    const QPair<int, int> range = logicalLineRange(lines, spanLast);

    const QPair<QString, QString> comment = lineCommentTokens(finding.path);
    if (comment.first.isEmpty())
        return false; // no comment syntax: annotating would corrupt the file
    // Append at the end of the logical line — the one spot a comment cannot eat
    // a "\" continuation. An unterminated run at end of file has no such spot.
    if (continuesToNextLine(lines.at(range.second)))
        return false;
    lines[range.second] +=
        comment.second.isEmpty()
            ? QStringLiteral("  %1 %2").arg(comment.first, secretScanIgnoreMarker())
            : QStringLiteral("  %1 %2 %3")
                  .arg(comment.first, secretScanIgnoreMarker(), comment.second);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QString content = lines.join(QLatin1Char('\n'));
    if (hadTrailingNewline)
        content += QLatin1Char('\n');
    file.write(content.toUtf8());
    file.close();
    return true;
}

RepoSecuritySeverity RepoSecurity::highestSeverity(
    const RepoSecuritySnapshot &snapshot)
{
    RepoSecuritySeverity highest = RepoSecuritySeverity::Pass;
    auto rank = [](RepoSecuritySeverity severity) {
        switch (severity) {
        case RepoSecuritySeverity::Pass:
            return 0;
        case RepoSecuritySeverity::Info:
            return 1;
        case RepoSecuritySeverity::Warning:
            return 2;
        case RepoSecuritySeverity::High:
            return 3;
        case RepoSecuritySeverity::Critical:
            return 4;
        }
        return 1;
    };
    for (const RepoSecuritySignal &signal : snapshot.signalList)
        if (rank(signal.severity) > rank(highest))
            highest = signal.severity;
    for (const RepoSecurityFinding &finding : snapshot.findings)
        if (rank(finding.severity) > rank(highest))
            highest = finding.severity;
    return highest;
}


namespace {

bool looksLikeTestPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.startsWith(QLatin1String("test/")) ||
        lower.startsWith(QLatin1String("tests/")) ||
        lower.contains(QLatin1String("/test/")) ||
        lower.contains(QLatin1String("/tests/")) ||
        lower.contains(QLatin1String("/__tests__/")) ||
        lower.contains(QLatin1String("/spec/")))
        return true;
    const QString base = QFileInfo(lower).fileName();
    return base.startsWith(QLatin1String("test_")) ||
           base.contains(QLatin1String("_test.")) ||
           base.contains(QLatin1String(".test.")) ||
           base.contains(QLatin1String(".spec.")) ||
           base.contains(QLatin1String("_spec."));
}

bool isSourceCodePath(const QString &path)
{
    static const QSet<QString> kCodeExtensions{
        QStringLiteral("c"),   QStringLiteral("cc"),    QStringLiteral("cpp"),
        QStringLiteral("cxx"), QStringLiteral("h"),     QStringLiteral("hh"),
        QStringLiteral("hpp"), QStringLiteral("hxx"),   QStringLiteral("m"),
        QStringLiteral("mm"),  QStringLiteral("cs"),    QStringLiteral("java"),
        QStringLiteral("kt"),  QStringLiteral("go"),    QStringLiteral("rs"),
        QStringLiteral("py"),  QStringLiteral("rb"),    QStringLiteral("js"),
        QStringLiteral("jsx"), QStringLiteral("ts"),    QStringLiteral("tsx"),
        QStringLiteral("mjs"), QStringLiteral("cjs"),   QStringLiteral("php"),
        QStringLiteral("swift"), QStringLiteral("scala"), QStringLiteral("sh"),
        QStringLiteral("bash"), QStringLiteral("pl"),   QStringLiteral("lua"),
        QStringLiteral("sql"), QStringLiteral("dart"),  QStringLiteral("ex"),
        QStringLiteral("exs"), QStringLiteral("hs"),    QStringLiteral("vue"),
        QStringLiteral("svelte"),
    };
    return kCodeExtensions.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

RepoSecuritySnapshot RepoQuality::scan(const RepoSecurityInput &input)
{
    RepoSecuritySnapshot snapshot;
    snapshot.repoKey = repoKey(input);
    snapshot.ref = currentRefFor(input);
    snapshot.generatedAtMs = QDateTime::currentMSecsSinceEpoch();

    int passedRuns = 0;
    int failedRuns = 0;
    int activeRuns = 0;
    for (const ActionRun &run : input.actionRuns) {
        if (run.status == ActionStatus::Success)
            ++passedRuns;
        else if (run.status == ActionStatus::Failed ||
                 run.status == ActionStatus::Rejected)
            ++failedRuns;
        else if (run.status == ActionStatus::Running ||
                 run.status == ActionStatus::Queued ||
                 run.status == ActionStatus::AwaitingApproval)
            ++activeRuns;
    }
    const RepoSecuritySeverity checksSeverity = qualitySeverity(input.actionRuns);
    const QString checksSummary =
        input.actionRuns.isEmpty()
            ? QStringLiteral("No quality check run recorded.")
            : QStringLiteral("%1 recorded: %2 passed, %3 failed, %4 active.")
                  .arg(plural(input.actionRuns.size(), QStringLiteral("run"),
                              QStringLiteral("runs")))
                  .arg(passedRuns)
                  .arg(failedRuns)
                  .arg(activeRuns);
    snapshot.signalList.append(
        signal(QStringLiteral("checks"), QStringLiteral("Quality checks"),
               checksSeverity, checksSummary,
               QStringLiteral("Derived from local ForkMesh action runs."),
               QStringLiteral("Open Actions"), QStringLiteral("tab:actions")));
    for (const ActionRun &run : input.actionRuns) {
        if (run.status != ActionStatus::Failed &&
            run.status != ActionStatus::Rejected)
            continue;
        RepoSecurityFinding finding;
        finding.id = QStringLiteral("check:%1").arg(run.id);
        finding.category = QStringLiteral("Checks");
        finding.severity = RepoSecuritySeverity::High;
        finding.title = run.workflowName.isEmpty() ? run.workflowPath
                                                   : run.workflowName;
        finding.detail = run.status == ActionStatus::Rejected
                             ? QStringLiteral("Run #%1 was rejected").arg(run.id)
                             : QStringLiteral("Run #%1 failed").arg(run.id);
        finding.path = run.workflowPath;
        finding.recommendedAction =
            QStringLiteral("Fix the failing workflow (see the Actions tab).");
        snapshot.findings.append(finding);
    }

    const QList<RepoFile> files = trackedTextFiles(input);
    static const QRegularExpression kTodoMarker(
        QStringLiteral("\\b(TODO|FIXME|HACK|XXX)\\b"));
    constexpr int kMaxTodoFindings = 120;
    constexpr int kLongFileLines = 1200;
    constexpr int kVeryLongFileLines = 3000;

    qint64 totalLines = 0;
    qint64 blankLines = 0;
    int sourceFiles = 0;
    int testFiles = 0;
    int todoCount = 0;
    int longFiles = 0;
    QString largestPath;
    int largestLines = 0;
    bool hasReadme = false;
    bool hasLicense = false;
    bool hasContributing = false;
    bool hasChangelog = false;
    bool hasDocsDir = false;
    QList<RepoSecurityFinding> todoFindings;
    QList<RepoSecurityFinding> longFileFindings;

    for (const RepoFile &file : files) {
        const QString lowerPath = file.path.toLower();
        const QString base = QFileInfo(lowerPath).fileName();
        if (!file.path.contains(QLatin1Char('/'))) {
            if (base.startsWith(QLatin1String("readme")))
                hasReadme = true;
            if (base.startsWith(QLatin1String("license")) ||
                base.startsWith(QLatin1String("copying")))
                hasLicense = true;
            if (base.startsWith(QLatin1String("contributing")))
                hasContributing = true;
            if (base.startsWith(QLatin1String("changelog")) ||
                base == QLatin1String("news") ||
                base.startsWith(QLatin1String("news.")))
                hasChangelog = true;
        }
        if (lowerPath.startsWith(QLatin1String("docs/")) ||
            lowerPath.startsWith(QLatin1String("doc/")))
            hasDocsDir = true;

        const bool isSource = isSourceCodePath(file.path);
        if (isSource) {
            ++sourceFiles;
            if (looksLikeTestPath(file.path))
                ++testFiles;
        }

        const QStringList lines =
            QString::fromUtf8(file.content).split(QLatin1Char('\n'));
        int fileLines = lines.size();
        if (!lines.isEmpty() && lines.last().isEmpty())
            --fileLines;
        totalLines += fileLines;
        for (int i = 0; i < fileLines; ++i) {
            const QString &line = lines.at(i);
            if (line.trimmed().isEmpty()) {
                ++blankLines;
                continue;
            }
            const QRegularExpressionMatch match = kTodoMarker.match(line);
            if (match.hasMatch()) {
                ++todoCount;
                if (todoFindings.size() < kMaxTodoFindings) {
                    RepoSecurityFinding finding;
                    finding.id = QStringLiteral("todo:%1:%2")
                                     .arg(file.path)
                                     .arg(i + 1);
                    finding.category = QStringLiteral("Maintenance");
                    finding.severity = RepoSecuritySeverity::Info;
                    finding.title = match.captured(1);
                    QString text = line.trimmed();
                    if (text.size() > 140)
                        text = text.left(139) + QStringLiteral("…");
                    finding.detail = text;
                    finding.path = file.path;
                    finding.line = i + 1;
                    finding.recommendedAction =
                        QStringLiteral("Resolve the marker or track it as an issue.");
                    todoFindings.append(finding);
                }
            }
        }

        if (fileLines > largestLines) {
            largestLines = fileLines;
            largestPath = file.path;
        }
        if (isSource && fileLines > kLongFileLines) {
            ++longFiles;
            RepoSecurityFinding finding;
            finding.id = QStringLiteral("long:%1").arg(file.path);
            finding.category = QStringLiteral("Large file");
            finding.severity = fileLines > kVeryLongFileLines
                                   ? RepoSecuritySeverity::Warning
                                   : RepoSecuritySeverity::Info;
            finding.title = QFileInfo(file.path).fileName();
            finding.detail = QStringLiteral("%1 lines").arg(fileLines);
            finding.path = file.path;
            finding.recommendedAction =
                QStringLiteral("Consider splitting into smaller modules.");
            longFileFindings.append(finding);
        }
    }

    const int fileCount = files.size();
    const int avgLines = fileCount > 0 ? int(totalLines / fileCount) : 0;
    const int blankPct =
        totalLines > 0 ? int(blankLines * 100 / totalLines) : 0;
    QString volumeDetail;
    if (fileCount > 0) {
        volumeDetail = QStringLiteral("Average %1 lines per file; %2% blank.")
                           .arg(avgLines)
                           .arg(blankPct);
        if (!largestPath.isEmpty())
            volumeDetail += QStringLiteral(" Largest: %1 (%2 lines).")
                                .arg(largestPath)
                                .arg(largestLines);
    }
    snapshot.signalList.append(
        signal(QStringLiteral("volume"), QStringLiteral("Code volume"),
               RepoSecuritySeverity::Info,
               fileCount > 0
                   ? QStringLiteral("%1, %2 lines.")
                         .arg(plural(fileCount, QStringLiteral("tracked text file"),
                                     QStringLiteral("tracked text files")))
                         .arg(totalLines)
                   : QStringLiteral("No tracked text files found."),
               volumeDetail));

    const int coreDocs = int(hasReadme) + int(hasLicense) + int(hasContributing) +
                         int(hasChangelog);
    RepoSecuritySeverity docsSeverity = RepoSecuritySeverity::Pass;
    if (!hasReadme)
        docsSeverity = RepoSecuritySeverity::Warning;
    else if (coreDocs < 4)
        docsSeverity = RepoSecuritySeverity::Info;
    QStringList missingDocs;
    if (!hasReadme)
        missingDocs << QStringLiteral("README");
    if (!hasLicense)
        missingDocs << QStringLiteral("LICENSE");
    if (!hasContributing)
        missingDocs << QStringLiteral("CONTRIBUTING");
    if (!hasChangelog)
        missingDocs << QStringLiteral("CHANGELOG");
    QString docsDetail = missingDocs.isEmpty()
                             ? QStringLiteral("README, LICENSE, CONTRIBUTING and "
                                              "CHANGELOG are all present.")
                             : QStringLiteral("Missing: %1.")
                                   .arg(missingDocs.join(QStringLiteral(", ")));
    if (hasDocsDir)
        docsDetail += QStringLiteral(" A docs/ directory is present.");
    snapshot.signalList.append(
        signal(QStringLiteral("docs"), QStringLiteral("Documentation"),
               docsSeverity,
               QStringLiteral("%1 of 4 core docs present.").arg(coreDocs),
               docsDetail));
    for (const QString &doc : missingDocs) {
        RepoSecurityFinding finding;
        finding.id = QStringLiteral("doc:%1").arg(doc.toLower());
        finding.category = QStringLiteral("Documentation");
        finding.severity = doc == QLatin1String("README")
                               ? RepoSecuritySeverity::Warning
                               : RepoSecuritySeverity::Info;
        finding.title = QStringLiteral("Missing %1").arg(doc);
        finding.detail =
            QStringLiteral("No root-level %1 file was found.").arg(doc);
        finding.recommendedAction =
            QStringLiteral("Add a %1 file at the repository root.").arg(doc);
        snapshot.findings.append(finding);
    }

    RepoSecuritySeverity testsSeverity = RepoSecuritySeverity::Pass;
    QString testsSummary;
    QString testsDetail;
    if (testFiles > 0) {
        testsSummary = QStringLiteral("%1 across %2 source files.")
                           .arg(plural(testFiles, QStringLiteral("test file"),
                                       QStringLiteral("test files")))
                           .arg(sourceFiles);
        if (testFiles < sourceFiles)
            testsDetail = QStringLiteral("About 1 test file per %1 source files.")
                              .arg(qMax(1, sourceFiles / testFiles));
    } else if (sourceFiles > 0) {
        testsSeverity = RepoSecuritySeverity::Warning;
        testsSummary = QStringLiteral("No test files detected.");
        testsDetail = QStringLiteral(
            "Add automated tests (tests/ directory or test_* files) to track "
            "regressions.");
    } else {
        testsSeverity = RepoSecuritySeverity::Info;
        testsSummary = QStringLiteral("No source files detected.");
    }
    snapshot.signalList.append(signal(QStringLiteral("tests"),
                                   QStringLiteral("Tests"), testsSeverity,
                                   testsSummary, testsDetail));

    RepoSecuritySeverity todoSeverity = RepoSecuritySeverity::Pass;
    if (todoCount > 50)
        todoSeverity = RepoSecuritySeverity::Warning;
    else if (todoCount > 0)
        todoSeverity = RepoSecuritySeverity::Info;
    snapshot.signalList.append(
        signal(QStringLiteral("todos"), QStringLiteral("Maintenance markers"),
               todoSeverity,
               todoCount > 0
                   ? plural(todoCount, QStringLiteral("TODO/FIXME/HACK marker"),
                            QStringLiteral("TODO/FIXME/HACK markers")) +
                         QStringLiteral(" in tracked files.")
                   : QStringLiteral("No TODO/FIXME markers found."),
               todoCount > 0
                   ? QStringLiteral("Each marker is listed under findings with "
                                    "its file and line.")
                   : QString()));
    snapshot.findings.append(todoFindings);

    RepoSecuritySeverity sizeSeverity = RepoSecuritySeverity::Pass;
    for (const RepoSecurityFinding &finding : longFileFindings)
        if (finding.severity == RepoSecuritySeverity::Warning)
            sizeSeverity = RepoSecuritySeverity::Warning;
    if (sizeSeverity == RepoSecuritySeverity::Pass && longFiles > 0)
        sizeSeverity = RepoSecuritySeverity::Info;
    snapshot.signalList.append(
        signal(QStringLiteral("filesize"), QStringLiteral("File size health"),
               sizeSeverity,
               longFiles > 0
                   ? plural(longFiles, QStringLiteral("source file"),
                            QStringLiteral("source files")) +
                         QStringLiteral(" over %1 lines.").arg(kLongFileLines)
                   : QStringLiteral("No source files over %1 lines.")
                         .arg(kLongFileLines),
               QStringLiteral("Long files are harder to review and merge.")));
    snapshot.findings.append(longFileFindings);

    const QString gitDir = (!input.localPath.trimmed().isEmpty() &&
                            QDir(input.localPath).exists())
                               ? input.localPath
                               : input.mirrorPath;
    const int commits30 =
        runGit(gitDir, {QStringLiteral("rev-list"), QStringLiteral("--count"),
                        QStringLiteral("--since=30 days ago"),
                        QStringLiteral("HEAD")})
            .toInt();
    const QStringList authorLines =
        runGit(gitDir, {QStringLiteral("shortlog"), QStringLiteral("-sn"),
                        QStringLiteral("HEAD")})
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const qint64 lastCommitSecs =
        runGit(gitDir, {QStringLiteral("log"), QStringLiteral("-1"),
                        QStringLiteral("--format=%ct"), QStringLiteral("HEAD")})
            .toLongLong();
    QString activityDetail;
    if (!authorLines.isEmpty())
        activityDetail = plural(authorLines.size(), QStringLiteral("contributor"),
                                QStringLiteral("contributors")) +
                         QStringLiteral(" over the repository history.");
    if (lastCommitSecs > 0)
        activityDetail +=
            QStringLiteral(" Last commit %1.")
                .arg(QDateTime::fromSecsSinceEpoch(lastCommitSecs)
                         .toString(QStringLiteral("yyyy-MM-dd")));
    snapshot.signalList.append(
        signal(QStringLiteral("activity"), QStringLiteral("Commit activity"),
               commits30 > 0 ? RepoSecuritySeverity::Pass
                             : RepoSecuritySeverity::Info,
               commits30 > 0
                   ? plural(commits30, QStringLiteral("commit"),
                            QStringLiteral("commits")) +
                         QStringLiteral(" in the last 30 days.")
                   : QStringLiteral("No commits in the last 30 days."),
               activityDetail.trimmed()));

    int openIssues = 0;
    int closedIssues = 0;
    for (const Issue &issue : input.issues) {
        if (issue.status == QLatin1String("open"))
            ++openIssues;
        else
            ++closedIssues;
    }
    QString issuesSummary;
    QString issuesDetail;
    if (openIssues + closedIssues == 0) {
        issuesSummary = QStringLiteral("No issues recorded.");
    } else {
        issuesSummary = QStringLiteral("%1 open, %2 closed.")
                            .arg(openIssues)
                            .arg(closedIssues);
        issuesDetail =
            QStringLiteral("%1% of recorded issues are closed.")
                .arg(closedIssues * 100 / (openIssues + closedIssues));
    }
    snapshot.signalList.append(
        signal(QStringLiteral("issues"), QStringLiteral("Issue hygiene"),
               openIssues == 0 ? RepoSecuritySeverity::Pass
                               : RepoSecuritySeverity::Info,
               issuesSummary, issuesDetail, QStringLiteral("Open Issues"),
               QStringLiteral("tab:issues")));

    return snapshot;
}
