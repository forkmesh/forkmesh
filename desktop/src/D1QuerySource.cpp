#include "D1QuerySource.h"

#include <QChar>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QRegularExpression>

#include <algorithm>

namespace forkmesh {
namespace {

// A Worker module is a few hundred kilobytes; anything larger is generated or
// vendored and is not where a query is written.
constexpr qint64 kMaxSourceFileBytes = 4 * 1024 * 1024;
// Below this many shared characters two statements only agree on "select
// something from somewhere", which names no file.
constexpr int kMinMatchPrefix = 24;
constexpr QChar kEllipsis = QChar(0x2026);

bool startsStatement(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral("\\A\\s*(select|insert|update|delete|replace|with|"
                       "create|alter|drop|pragma|vacuum)\\s"),
        QRegularExpression::CaseInsensitiveOption);
    return text.size() >= 12 && re.match(text).hasMatch();
}

// A docstring can open with "Insert one normalized report…" and read as a
// statement; real SQL also carries a clause keyword.
bool looksLikeStatement(const QString &normalized)
{
    static const QRegularExpression re(
        QStringLiteral("(\\bfrom\\b|\\binto\\b|\\bset\\b|\\bvalues\\b|"
                       "\\btable\\b|\\bwhere\\b|\\bindex\\b|\\bpragma\\b)"));
    return re.match(normalized).hasMatch();
}

struct Literal {
    bool found = false;
    int prefixStart = -1; // the f/r/b prefix letters, or the opening quote
    int end = -1;         // just past the closing quote run
    QString text;
    QString terminator;
    bool closed = false;
};

bool isLiteralPrefixLetter(QChar ch)
{
    static const QString letters = QStringLiteral("fFrRbBuU");
    return letters.contains(ch);
}

// The next Python string literal at or after `from`, with its contents
// unescaped enough for SQL comparison. A `#` outside a literal ends the line.
Literal nextLiteral(const QString &line, int from)
{
    Literal lit;
    for (int i = qMax(0, from); i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('#'))
            return lit;
        if (ch != QLatin1Char('"') && ch != QLatin1Char('\''))
            continue;
        int prefix = i;
        while (prefix > 0 && isLiteralPrefixLetter(line.at(prefix - 1)))
            --prefix;
        // A quote glued to an identifier is not an opening quote.
        if (prefix > 0) {
            const QChar before = line.at(prefix - 1);
            if (before.isLetterOrNumber() || before == QLatin1Char('_')) {
                if (prefix != i)
                    continue;
            }
        }
        const QString triple = QString(3, ch);
        const bool isTriple = line.mid(i, 3) == triple;
        lit.terminator = isTriple ? triple : QString(ch);
        lit.found = true;
        lit.prefixStart = prefix;
        int at = i + lit.terminator.size();
        QString text;
        while (at < line.size()) {
            if (line.at(at) == QLatin1Char('\\') && at + 1 < line.size()) {
                const QChar escaped = line.at(at + 1);
                if (escaped == QLatin1Char('n') ||
                    escaped == QLatin1Char('t') ||
                    escaped == QLatin1Char('r'))
                    text += QLatin1Char(' ');
                else
                    text += escaped;
                at += 2;
                continue;
            }
            if (line.mid(at, lit.terminator.size()) == lit.terminator) {
                lit.closed = true;
                at += lit.terminator.size();
                break;
            }
            text += line.at(at);
            ++at;
        }
        lit.text = text;
        lit.end = at;
        return lit;
    }
    return lit;
}

// What follows a literal on its line, with trailing comment and whitespace
// removed. Empty means the statement may continue on the next line through
// Python's implicit concatenation.
QString remainderAfter(const QString &line, int end)
{
    if (end < 0 || end >= line.size())
        return QString();
    QString rest = line.mid(end);
    const int hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        rest = rest.left(hash);
    return rest.trimmed();
}

} // namespace

QString normalizeD1Query(const QString &sql)
{
    QString text = sql;
    text.replace(kEllipsis, QLatin1Char(' '));
    static const QRegularExpression hexRe(
        QStringLiteral("\\b[xX]'(?:''|[^'])*'"));
    text.replace(hexRe, QStringLiteral("?"));
    static const QRegularExpression stringRe(
        QStringLiteral("'(?:''|[^'])*'"));
    text.replace(stringRe, QStringLiteral("?"));
    static const QRegularExpression blockCommentRe(
        QStringLiteral("/\\*.*?\\*/"),
        QRegularExpression::DotMatchesEverythingOption);
    text.replace(blockCommentRe, QStringLiteral(" "));
    static const QRegularExpression lineCommentRe(
        QStringLiteral("--[^\\r\\n]*"));
    text.replace(lineCommentRe, QStringLiteral(" "));
    static const QRegularExpression numberRe(
        QStringLiteral("\\b\\d+(?:\\.\\d+)?\\b"));
    text.replace(numberRe, QStringLiteral("?"));
    return text.simplified().toLower();
}

QList<D1QueryStatement> scanD1QueryStatements(const QString &relativePath,
                                              const QString &source)
{
    QList<D1QueryStatement> statements;
    const QStringList lines = source.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        Literal lit;
        for (int at = 0; at < line.size();) {
            const Literal candidate = nextLiteral(line, at);
            if (!candidate.found)
                break;
            if (startsStatement(candidate.text)) {
                lit = candidate;
                break;
            }
            if (!candidate.closed)
                break;
            at = qMax(candidate.end, at + 1);
        }
        if (!lit.found)
            continue;

        QString text = lit.text;
        int last = i;
        if (!lit.closed) {
            // A triple-quoted block: take following lines up to its close.
            for (int j = i + 1; j < lines.size(); ++j) {
                last = j;
                const int close = lines.at(j).indexOf(lit.terminator);
                if (close < 0) {
                    text += QLatin1Char(' ') + lines.at(j);
                    continue;
                }
                text += QLatin1Char(' ') + lines.at(j).left(close);
                break;
            }
        } else {
            QString rest = remainderAfter(line, lit.end);
            for (int j = i + 1; rest.isEmpty() && j < lines.size(); ++j) {
                const QString &next = lines.at(j);
                const Literal cont = nextLiteral(next, 0);
                if (!cont.found || !cont.closed)
                    break;
                // Only a line that is nothing but a string literal continues
                // the statement; anything else is the next argument.
                if (!next.left(cont.prefixStart).trimmed().isEmpty())
                    break;
                text += QLatin1Char(' ') + cont.text;
                last = j;
                rest = remainderAfter(next, cont.end);
            }
        }

        const QString normalized = normalizeD1Query(text);
        if (looksLikeStatement(normalized))
            statements.append(D1QueryStatement{relativePath, i + 1,
                                               normalized});
        i = last;
    }
    return statements;
}

QList<D1QueryStatement> buildD1QuerySourceIndex(const QStringList &roots)
{
    for (const QString &root : roots) {
        if (root.isEmpty())
            continue;
        const QString base = QDir(root).filePath(QStringLiteral("app/src"));
        if (!QFileInfo(base).isDir())
            continue;
        QList<D1QueryStatement> statements;
        QDirIterator it(base, {QStringLiteral("*.py")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            QFile file(absolute);
            if (file.size() > kMaxSourceFileBytes ||
                !file.open(QIODevice::ReadOnly))
                continue;
            const QString relative =
                QStringLiteral("app/src/") +
                QDir(base).relativeFilePath(absolute);
            statements += scanD1QueryStatements(
                relative, QString::fromUtf8(file.readAll()));
        }
        if (statements.isEmpty())
            continue;
        std::sort(statements.begin(), statements.end(),
                  [](const D1QueryStatement &a, const D1QueryStatement &b) {
                      if (a.path != b.path)
                          return a.path < b.path;
                      return a.line < b.line;
                  });
        return statements;
    }
    return {};
}

D1QuerySite matchD1QuerySite(const QString &providerQuery,
                             const QList<D1QueryStatement> &index)
{
    const bool truncated = providerQuery.trimmed().endsWith(kEllipsis);
    QString wanted = normalizeD1Query(providerQuery);
    if (truncated) {
        // The relay cut the text mid-token; that partial word matches nothing.
        const int space = wanted.lastIndexOf(QLatin1Char(' '));
        wanted = space > 0 ? wanted.left(space) : QString();
    }
    if (wanted.size() < kMinMatchPrefix)
        return {};

    int bestShared = 0;
    int bestDistance = 0;
    bool bestExact = false;
    bool ambiguous = false;
    int exactSites = 0;
    D1QuerySite best;
    for (const D1QueryStatement &statement : index) {
        const QString &candidate = statement.normalized;
        int shared = 0;
        const int limit = qMin(wanted.size(), candidate.size());
        while (shared < limit && wanted.at(shared) == candidate.at(shared))
            ++shared;
        if (shared < kMinMatchPrefix)
            continue;
        const bool exact =
            shared == wanted.size() && candidate.size() == wanted.size();
        if (exact)
            ++exactSites;
        const int distance = qAbs(candidate.size() - wanted.size());
        int better = 1;
        if (best.isValid()) {
            if (exact != bestExact)
                better = exact ? 1 : -1;
            else if (shared != bestShared)
                better = shared > bestShared ? 1 : -1;
            else if (distance != bestDistance)
                better = distance < bestDistance ? 1 : -1;
            else
                better = 0;
        }
        if (better < 0)
            continue;
        if (better == 0) {
            // Two call sites fit equally well. One is the statement written
            // out twice, which still answers "where does this run"; a partial
            // agreement is a guess, so name neither place.
            if (!exact)
                ambiguous = true;
            continue;
        }
        bestShared = shared;
        bestDistance = distance;
        bestExact = exact;
        ambiguous = false;
        best = D1QuerySite{statement.path, statement.line};
    }
    if (!best.isValid() || ambiguous)
        return {};
    if (bestExact)
        best.otherSites = qMax(0, exactSites - 1);
    if (bestShared == wanted.size())
        return best;
    // A statement assembled at run time (an IN list, a generated column set)
    // only agrees with the source up to the generated part; require most of
    // the provider's text before naming a file.
    return bestShared * 4 >= wanted.size() * 3 ? best : D1QuerySite{};
}

} // namespace forkmesh
