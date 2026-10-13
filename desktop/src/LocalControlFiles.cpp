#include "LocalControlFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace forkmesh::localcontrol::filesbrowse {

namespace {

constexpr int kMaxPathChars = 1024;
constexpr int kMaxComponentChars = 255;

bool insideRoot(const QString &rootCanonical, const QString &candidate)
{
    if (rootCanonical.isEmpty() || candidate.isEmpty())
        return false;
    if (candidate == rootCanonical)
        return true;
    const QString prefix = rootCanonical.endsWith(QLatin1Char('/'))
                               ? rootCanonical
                               : rootCanonical + QLatin1Char('/');
    return candidate.startsWith(prefix);
}

bool hasGitComponent(const QString &relative)
{
    for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (part.compare(QLatin1String(".git"), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString readSmallFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(4096)).trimmed();
}

}

QString familyPrefix()
{
    return QStringLiteral("files.browse.");
}

QString normalizeRelativePath(const QString &path, bool *ok)
{
    if (ok)
        *ok = false;
    if (path.size() > kMaxPathChars)
        return {};
    if (path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')))
        return {};
    QStringList parts;
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..") || part.size() > kMaxComponentChars)
            return {};
        for (const QChar ch : part)
            if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
                return {};
        parts.append(part);
    }
    if (ok)
        *ok = true;
    return parts.join(QLatin1Char('/'));
}

bool isSensitiveName(const QString &name)
{
    const QString lower = name.toLower();
    if (lower == QLatin1String(".env") || lower.startsWith(QLatin1String(".env.")) ||
        lower == QLatin1String(".netrc") || lower == QLatin1String(".npmrc") ||
        lower == QLatin1String(".pypirc") || lower == QLatin1String(".git-credentials") ||
        lower == QLatin1String("credentials.json") ||
        lower.startsWith(QLatin1String("id_rsa")) ||
        lower.startsWith(QLatin1String("id_ed25519")) ||
        lower.startsWith(QLatin1String("id_ecdsa")))
        return true;
    static const QStringList suffixes = {QStringLiteral(".pem"), QStringLiteral(".key"),
                                         QStringLiteral(".p12"), QStringLiteral(".pfx"),
                                         QStringLiteral(".keystore"),
                                         QStringLiteral(".jks")};
    for (const QString &suffix : suffixes)
        if (lower.endsWith(suffix))
            return true;
    return false;
}

QString resolveInside(const QString &rootPath, const QString &relative,
                      QString *error)
{
    const auto failWith = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    const QString rootCanonical = QFileInfo(rootPath).canonicalFilePath();
    if (rootCanonical.isEmpty())
        return failWith("root_unavailable");
    bool ok = false;
    const QString clean = normalizeRelativePath(relative, &ok);
    if (!ok)
        return failWith("bad_path");
    if (hasGitComponent(clean))
        return failWith("not_allowed");
    const QString target =
        clean.isEmpty() ? rootCanonical : QDir(rootCanonical).filePath(clean);
    const QString canonical = QFileInfo(target).canonicalFilePath();
    if (canonical.isEmpty())
        return failWith("not_found");
    if (!insideRoot(rootCanonical, canonical))
        return failWith("outside_root");
    if (hasGitComponent(canonical.mid(rootCanonical.size())))
        return failWith("not_allowed");
    return canonical;
}

Listing listDirectory(const QString &rootPath, const QString &dirPath,
                      int maxEntries)
{
    Listing listing;
    const QString rootCanonical = QFileInfo(rootPath).canonicalFilePath();
    QDir dir(dirPath);
    const QFileInfoList infos = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::NoSort);
    QList<QJsonObject> rows;
    for (const QFileInfo &info : infos) {
        const QString name = info.fileName();
        if (name.compare(QLatin1String(".git"), Qt::CaseInsensitive) == 0)
            continue;
        ++listing.total;
        QString kind = info.isDir() ? QStringLiteral("dir") : QStringLiteral("file");
        bool escapes = false;
        if (info.isSymLink()) {
            kind = QStringLiteral("symlink");
            const QString target = info.canonicalFilePath();
            escapes = target.isEmpty() || !insideRoot(rootCanonical, target);
            if (!escapes && QFileInfo(target).isDir())
                kind = QStringLiteral("dirlink");
        }
        QJsonObject row{{QStringLiteral("name"), name},
                        {QStringLiteral("kind"), kind},
                        {QStringLiteral("size"), info.isDir() ? 0.0 : double(info.size())},
                        {QStringLiteral("modifiedMs"),
                         double(info.lastModified().toMSecsSinceEpoch())}};
        if (escapes)
            row.insert(QStringLiteral("escapes"), true);
        if (!info.isDir() && isSensitiveName(name))
            row.insert(QStringLiteral("sensitive"), true);
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const auto rank = [](const QJsonObject &row) {
            const QString kind = row.value(QStringLiteral("kind")).toString();
            return kind == QLatin1String("dir") || kind == QLatin1String("dirlink") ? 0 : 1;
        };
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return a.value(QStringLiteral("name")).toString().compare(
                   b.value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) < 0;
    });
    listing.truncated = rows.size() > maxEntries;
    for (const QJsonObject &row : rows.mid(0, maxEntries))
        listing.entries.append(row);
    return listing;
}

bool looksBinary(const QByteArray &bytes)
{
    if (bytes.contains('\0'))
        return true;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    if (!decoder.hasError())
        return false;
    int invalid = 0;
    for (const QChar ch : text)
        if (ch == QChar::ReplacementCharacter)
            ++invalid;
    return invalid > 4;
}

QJsonObject readFileSlice(const QString &path, qint64 offset, qint64 maxBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{QStringLiteral("error"), QStringLiteral("unreadable")}};
    const qint64 size = file.size();
    const qint64 start = qBound<qint64>(0, offset, size);
    const qint64 length = qBound<qint64>(0, maxBytes, kMaxReadBytes);
    if (!file.seek(start))
        return {{QStringLiteral("error"), QStringLiteral("unreadable")}};
    QByteArray bytes = file.read(length);
    const bool eof = start + bytes.size() >= size;
    QJsonObject result{{QStringLiteral("size"), double(size)},
                       {QStringLiteral("offset"), double(start)},
                       {QStringLiteral("length"), double(bytes.size())},
                       {QStringLiteral("eof"), eof}};
    if (looksBinary(bytes)) {
        result.insert(QStringLiteral("binary"), true);
        result.insert(QStringLiteral("base64"), QString::fromLatin1(bytes.toBase64()));
        return result;
    }
    if (!eof) {
        int cut = bytes.size();
        while (cut > 0 && cut > bytes.size() - 4 &&
               (static_cast<unsigned char>(bytes.at(cut - 1)) & 0xC0) == 0x80)
            --cut;
        if (cut > 0 && (static_cast<unsigned char>(bytes.at(cut - 1)) & 0xC0) == 0xC0)
            --cut;
        if (cut > 0 && cut < bytes.size()) {
            bytes.truncate(cut);
            result.insert(QStringLiteral("length"), double(bytes.size()));
        }
    }
    result.insert(QStringLiteral("binary"), false);
    result.insert(QStringLiteral("text"), QString::fromUtf8(bytes));
    return result;
}

bool validRootId(const QString &id)
{
    static const QRegularExpression allowed(QStringLiteral(
        "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}"
        "(#[A-Za-z0-9._][A-Za-z0-9._-]{0,127})?$"));
    return id.size() <= 300 && allowed.match(id).hasMatch() &&
           !id.contains(QLatin1String(".."));
}

QString worktreeRootId(const QString &repoKey, const QString &folder)
{
    return repoKey + QLatin1Char('#') + folder;
}

QList<Root> discoverWorktrees(const QString &repoKey, const QString &repoPath,
                              int maxWorktrees)
{
    QList<Root> roots;
    const QString gitDir = QDir(repoPath).filePath(QStringLiteral(".git"));
    if (!QFileInfo(gitDir).isDir())
        return roots;
    QDir worktrees(QDir(gitDir).filePath(QStringLiteral("worktrees")));
    const QStringList folders =
        worktrees.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    static const QRegularExpression folderName(
        QStringLiteral("^[A-Za-z0-9._][A-Za-z0-9._-]{0,127}$"));
    for (const QString &folder : folders) {
        if (roots.size() >= maxWorktrees)
            break;
        if (!folderName.match(folder).hasMatch())
            continue;
        const QString meta = worktrees.filePath(folder);
        const QString pointer = readSmallFile(QDir(meta).filePath(QStringLiteral("gitdir")));
        if (pointer.isEmpty())
            continue;
        const QString checkout = QFileInfo(pointer).absolutePath();
        if (!QFileInfo(checkout).isDir())
            continue;
        QString branch = readSmallFile(QDir(meta).filePath(QStringLiteral("HEAD")));
        if (branch.startsWith(QLatin1String("ref: refs/heads/")))
            branch = branch.mid(16);
        else if (branch.size() >= 7)
            branch = branch.left(7);
        Root root;
        root.id = worktreeRootId(repoKey, folder);
        root.label = branch.isEmpty() ? folder : branch;
        root.kind = QStringLiteral("worktree");
        root.path = checkout;
        root.branch = branch;
        root.repo = repoKey;
        roots.append(root);
    }
    return roots;
}

QJsonObject rootRow(const Root &root)
{
    return {{QStringLiteral("id"), root.id},
            {QStringLiteral("label"), root.label},
            {QStringLiteral("kind"), root.kind},
            {QStringLiteral("branch"), root.branch},
            {QStringLiteral("repo"), root.repo}};
}

QJsonObject parsePorcelainStatus(const QByteArray &output, int maxFiles)
{
    const QList<QByteArray> fields = output.split('\0');
    QString branch;
    QString upstream;
    int ahead = 0;
    int behind = 0;
    QJsonArray files;
    int total = 0;
    int staged = 0;
    int unstaged = 0;
    int untracked = 0;
    int conflicted = 0;
    static const QRegularExpression aheadRe(QStringLiteral("ahead (\\d+)"));
    static const QRegularExpression behindRe(QStringLiteral("behind (\\d+)"));
    for (int i = 0; i < fields.size(); ++i) {
        const QString entry = QString::fromUtf8(fields.at(i));
        if (entry.isEmpty())
            continue;
        if (entry.startsWith(QLatin1String("## "))) {
            QString head = entry.mid(3);
            const int bracket = head.indexOf(QLatin1String(" ["));
            QString counts;
            if (bracket >= 0) {
                counts = head.mid(bracket);
                head = head.left(bracket);
            }
            const int dots = head.indexOf(QLatin1String("..."));
            branch = dots >= 0 ? head.left(dots) : head;
            upstream = dots >= 0 ? head.mid(dots + 3) : QString();
            if (branch.startsWith(QLatin1String("No commits yet on ")))
                branch = branch.mid(18);
            const auto a = aheadRe.match(counts);
            const auto b = behindRe.match(counts);
            ahead = a.hasMatch() ? a.captured(1).toInt() : 0;
            behind = b.hasMatch() ? b.captured(1).toInt() : 0;
            continue;
        }
        if (entry.size() < 4)
            continue;
        const QChar x = entry.at(0);
        const QChar y = entry.at(1);
        QString path = entry.mid(3);
        QString from;
        if (x == QLatin1Char('R') || x == QLatin1Char('C')) {
            if (i + 1 < fields.size())
                from = QString::fromUtf8(fields.at(++i));
        }
        ++total;
        QString kind;
        const QString xy = QString(x) + y;
        if (xy == QLatin1String("??")) {
            kind = QStringLiteral("untracked");
            ++untracked;
        } else if (xy.contains(QLatin1Char('U')) || xy == QLatin1String("AA") ||
                   xy == QLatin1String("DD")) {
            kind = QStringLiteral("conflicted");
            ++conflicted;
        } else {
            if (x != QLatin1Char(' ') && x != QLatin1Char('?'))
                ++staged;
            if (y != QLatin1Char(' '))
                ++unstaged;
            const QChar code = x != QLatin1Char(' ') ? x : y;
            switch (code.toLatin1()) {
            case 'A': kind = QStringLiteral("added"); break;
            case 'D': kind = QStringLiteral("deleted"); break;
            case 'R': kind = QStringLiteral("renamed"); break;
            case 'C': kind = QStringLiteral("copied"); break;
            default: kind = QStringLiteral("modified"); break;
            }
        }
        if (files.size() >= maxFiles)
            continue;
        QJsonObject row{{QStringLiteral("path"), path.left(1024)},
                        {QStringLiteral("index"), QString(x)},
                        {QStringLiteral("worktree"), QString(y)},
                        {QStringLiteral("kind"), kind}};
        if (!from.isEmpty())
            row.insert(QStringLiteral("from"), from.left(1024));
        files.append(row);
    }
    return {{QStringLiteral("branch"), branch},
            {QStringLiteral("upstream"), upstream},
            {QStringLiteral("ahead"), ahead},
            {QStringLiteral("behind"), behind},
            {QStringLiteral("total"), total},
            {QStringLiteral("staged"), staged},
            {QStringLiteral("unstaged"), unstaged},
            {QStringLiteral("untracked"), untracked},
            {QStringLiteral("conflicted"), conflicted},
            {QStringLiteral("truncated"), total > files.size()},
            {QStringLiteral("files"), files}};
}

QStringList gitActions()
{
    return {QStringLiteral("fetch"), QStringLiteral("pull")};
}

QStringList gitArguments(const QString &action)
{
    if (action == QLatin1String("fetch"))
        return {QStringLiteral("fetch"), QStringLiteral("--quiet")};
    if (action == QLatin1String("pull"))
        return {QStringLiteral("pull"), QStringLiteral("--ff-only"),
                QStringLiteral("--quiet")};
    return {};
}

QString gitActionSummary(const QString &action, const QString &label)
{
    if (action == QLatin1String("fetch"))
        return QStringLiteral("Fetch \"%1\" from its remote. Nothing in the working "
                              "tree changes.")
            .arg(label);
    if (action == QLatin1String("pull"))
        return QStringLiteral("Pull \"%1\" from its upstream, fast-forward only. "
                              "Nothing is merged or rewound; a diverged branch is "
                              "left as it is.")
            .arg(label);
    return {};
}

} // namespace forkmesh::localcontrol::filesbrowse
