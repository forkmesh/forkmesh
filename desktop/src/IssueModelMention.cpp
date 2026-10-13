#include "IssueModelMention.h"

#include <QRegularExpression>
#include <QSet>

namespace forkmesh::issuemention {
namespace {

struct AliasEntry {
    const char *alias;
    const char *model;
};

// Short names people actually type. The Claude rungs mirror claudeAutoLadder()
// in MainWindowInternal.h — the app re-resolves an alias through that ladder
// before it dispatches, so a model bump there is what wins at runtime and this
// table only has to be right for the offline/unit-test path. The rest name the
// Codex line-up, which has no ladder of its own.
const AliasEntry kAliases[] = {
    {"opus", "claude-opus-5"},     {"sonnet", "claude-sonnet-5"},
    {"haiku", "claude-haiku-4-5"}, {"fable", "claude-fable-5"},
    {"claude", "claude-sonnet-5"},
    {"codex", "gpt-5.5"},          {"gpt", "gpt-5.5"},
    {"sol", "gpt-5.6-sol"},        {"luna", "gpt-5.6-luna"},
    {"terra", "gpt-5.6-terra"},    {"spark", "gpt-5.3-codex-spark"},
};

// A mention that ends a sentence ("ask @opus.") captures the stop with it.
QString trimAlias(QString alias)
{
    while (alias.endsWith(QLatin1Char('.')) || alias.endsWith(QLatin1Char('-')))
        alias.chop(1);
    return alias;
}

// The punctuation that separates a mention from what it is asking
// ("@opus: why ...", "@opus - please ...") is not part of the request.
QString trimRequest(const QString &raw)
{
    int start = 0;
    while (start < raw.size()) {
        const QChar c = raw.at(start);
        if (!c.isSpace() && c != QLatin1Char(':') && c != QLatin1Char(',') &&
            c != QLatin1Char(';') && c != QLatin1Char('.') &&
            c != QLatin1Char('-'))
            break;
        ++start;
    }
    return raw.mid(start).trimmed();
}

// "hey, please can you ..." says nothing about whether the ask is a question or
// a job. Peeling it off is what lets "can you fix this?" read as the request
// for work it is while "how would you fix this?" stays a question.
QString stripPoliteLead(const QString &request)
{
    static const QRegularExpression lead(
        QStringLiteral("^(?:hey|hi|hello|ok|okay|so|also|and|please|pls|plz|"
                       "can\\s+you|could\\s+you|would\\s+you|will\\s+you|"
                       "do\\s+you\\s+mind|i\\s+need\\s+you\\s+to|"
                       "we\\s+need\\s+you\\s+to|let'?s|go\\s+ahead\\s+and|"
                       "if\\s+you\\s+can|when\\s+you\\s+can)\\b[\\s,:;.!-]*"),
        QRegularExpression::CaseInsensitiveOption);
    QString text = request.trimmed();
    for (int i = 0; i < 6; ++i) { // lead-ins stack: "hey, please can you ..."
        const QRegularExpressionMatch match = lead.match(text);
        if (!match.hasMatch() || match.capturedLength() == 0)
            break;
        text = text.mid(match.capturedEnd()).trimmed();
    }
    return text;
}

} // namespace

QString modelForAlias(const QString &alias)
{
    const QString name = trimAlias(alias.trimmed().toLower());
    if (name.isEmpty())
        return QString();
    for (const AliasEntry &entry : kAliases)
        if (name == QLatin1String(entry.alias))
            return QString::fromLatin1(entry.model);
    // A model id spelled out in full, so a model released after this build
    // still addresses the right thing: "@claude-fable-5", "@gpt-5.1".
    if (name.startsWith(QLatin1String("claude-")) &&
        name.size() > int(sizeof("claude-") - 1))
        return name;
    if (name.startsWith(QLatin1String("gpt-")) &&
        name.size() > int(sizeof("gpt-") - 1))
        return name;
    return QString();
}

QStringList aliases()
{
    QStringList out;
    out.reserve(int(sizeof(kAliases) / sizeof(kAliases[0])));
    for (const AliasEntry &entry : kAliases)
        out.append(QString::fromLatin1(entry.alias));
    return out;
}

QString providerForModel(const QString &model)
{
    const QString id = model.trimmed().toLower();
    if (id.startsWith(QLatin1String("claude")))
        return QStringLiteral("claude-code");
    if (id.startsWith(QLatin1String("gpt")))
        return QStringLiteral("codex");
    return QString();
}

bool requestsWork(const QString &request)
{
    const QString text = stripPoliteLead(request.simplified()).toLower();
    if (text.isEmpty())
        return false;

    // Naming the agent is unambiguous however it is phrased.
    static const QRegularExpression explicitAgent(QStringLiteral(
        "\\b(?:start|spawn|launch|kick\\s+off|fire\\s+up|run)\\s+"
        "(?:an?\\s+|the\\s+)?(?:coding\\s+|background\\s+|new\\s+)?agent\\b"));
    if (explicitAgent.match(text).hasMatch())
        return true;

    static const QRegularExpression work(QStringLiteral(
        "\\b(?:fix|implement|patch|repair|resolve|solve|refactor|rewrite|"
        "migrate|unbreak|land|ship)\\b"
        "|\\b(?:open|send|raise|submit|make)\\s+(?:an?|the)\\s+"
        "(?:pr|pull\\s+request|patch|fix|change)\\b"
        "|\\bwork\\s+on\\s+(?:this|it|the\\s+issue)\\b"
        "|\\btake\\s+(?:this|it)\\s+on\\b"
        "|\\bwrite\\s+(?:the|a)\\s+(?:code|fix|patch)\\b"
        "|\\bdo\\s+(?:it|this|that)\\b"));
    if (!work.match(text).hasMatch())
        return false;

    // "how would you fix this" asks about the work; it does not ask for it.
    static const QRegularExpression whQuestion(QStringLiteral(
        "^(?:how|what|what's|whats|why|when|where|which|who|whose|whom|"
        "thoughts|explain|describe|tell|summari[sz]e|review|compare|"
        "analy[sz]e|opinion|any\\s+idea)\\b"));
    if (whQuestion.match(text).hasMatch())
        return false;
    // An auxiliary only makes it a question when it is punctuated as one:
    // "does this need a fix?" asks, "do it" does not.
    static const QRegularExpression auxQuestion(QStringLiteral(
        "^(?:is|are|was|were|do|does|did|can|could|would|will|should|shall|"
        "may|might|have|has|had)\\b"));
    if (text.endsWith(QLatin1Char('?')) && auxQuestion.match(text).hasMatch())
        return false;
    return true;
}

QList<Mention> parse(const QString &text)
{
    QList<Mention> out;
    if (text.trimmed().isEmpty())
        return out;

    // Same shape as textMentionsNodeName's @name match, widened to the dots a
    // model id carries ("gpt-5.1") and anchored so an address ("me@opus.dev")
    // is not read as a mention.
    static const QRegularExpression mentionRe(QStringLiteral(
        "(?:^|[^A-Za-z0-9_@./-])@([A-Za-z][A-Za-z0-9.-]{0,63})"));

    struct Hit {
        qsizetype start = 0; // the '@'
        qsizetype end = 0;   // one past the alias
        QString alias;
        QString model;
    };
    QList<Hit> hits;
    auto matches = mentionRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString model = modelForAlias(match.captured(1));
        if (model.isEmpty())
            continue;
        hits.append(Hit{match.capturedStart(1) - 1, match.capturedEnd(1),
                        trimAlias(match.captured(1).toLower()), model});
    }
    if (hits.isEmpty())
        return out;

    // What a trailing mention ("...this makes no sense @opus") is asking about
    // is the message itself, so keep a copy with the mentions taken out.
    QString stripped;
    int cursor = 0;
    for (const Hit &hit : std::as_const(hits)) {
        stripped += text.mid(cursor, hit.start - cursor);
        cursor = hit.end;
    }
    stripped = (stripped + text.mid(cursor)).simplified();

    for (int i = 0; i < hits.size(); ++i) {
        const Hit &hit = hits.at(i);
        const qsizetype stop =
            i + 1 < hits.size() ? hits.at(i + 1).start : text.size();
        QString request = trimRequest(text.mid(hit.end, stop - hit.end));
        if (request.size() < 2)
            request = stripped;
        Mention mention;
        mention.alias = hit.alias;
        mention.model = hit.model;
        mention.provider = providerForModel(hit.model);
        mention.request = request;
        mention.intent = requestsWork(request) ? Intent::Work : Intent::Answer;
        out.append(mention);
    }
    return out;
}

QList<FileCitation> citedFilePaths(const QString &text)
{
    // A path-shaped token with an extension, optionally followed by ":line".
    // Deliberately loose -- the repository, not this regex, is what decides
    // whether a token names a file (trackedPathFor).
    static const QRegularExpression cite(QStringLiteral(
        "([A-Za-z0-9_][A-Za-z0-9_./+-]*\\.[A-Za-z0-9]{1,8})(?::(\\d+))?"));
    QList<FileCitation> out;
    QSet<QString> seen;
    auto matches = cite.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString path = match.captured(1);
        if (path.size() < 4 || seen.contains(path))
            continue;
        // "Qt 6.4.2" and "v1.2.3" are path-shaped and are not files. No real
        // extension is all digits, so that is the cheap way to say so -- and
        // it matters, because version numbers would otherwise crowd the real
        // citations out of the bounded list below.
        const QString extension = path.mid(path.lastIndexOf(QLatin1Char('.')) + 1);
        bool allDigits = !extension.isEmpty();
        for (const QChar c : extension)
            if (!c.isDigit())
                allDigits = false;
        if (allDigits)
            continue;
        seen.insert(path);
        out.append(FileCitation{path, match.captured(2).toInt()});
        if (out.size() >= 12)
            break;
    }
    return out;
}

QString trackedPathFor(const QStringList &trackedPaths, const QString &cited)
{
    if (trackedPaths.contains(cited))
        return cited;
    const QString suffix = QLatin1Char('/') + cited;
    QString match;
    for (const QString &path : trackedPaths) {
        if (!path.endsWith(suffix))
            continue;
        if (!match.isEmpty())
            return QString(); // ambiguous: two directories share the name
        match = path;
    }
    return match;
}

QString numberedFileWindow(const QString &body, int line, int radius,
                           int maxChars)
{
    const QStringList lines = body.split(QLatin1Char('\n'));
    const int from = line > 0 ? qMax(1, line - radius) : 1;
    const int to = line > 0 ? qMin(int(lines.size()), line + radius)
                            : qMin(int(lines.size()), radius * 2);
    QString out;
    for (int i = from; i <= to; ++i) {
        out += QStringLiteral("%1| %2\n").arg(i, 5).arg(lines.at(i - 1));
        if (out.size() > maxChars) {
            out += QStringLiteral("[file window truncated]\n");
            break;
        }
    }
    return out;
}

QString botAuthorName(const QString &modelLabel)
{
    const QString label = modelLabel.trimmed();
    return (label.isEmpty() ? QStringLiteral("model") : label) +
           QStringLiteral(" [bot]");
}

bool isBotAuthorName(const QString &authorName)
{
    return authorName.trimmed().endsWith(QLatin1String("[bot]"),
                                         Qt::CaseInsensitive);
}

} // namespace forkmesh::issuemention
